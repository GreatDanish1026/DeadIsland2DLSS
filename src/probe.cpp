// Probe (0.0.x, built with RD_PROBE): watches Dead Island 2 without changing anything, and writes
// what's needed for replacing its FSR 2 with DLSS to DeadIsland2DLSS.log:
//
// - The game's FSR 2 passes. The game ships AMD's FSR 2 UE plugin (r.FidelityFX.FSR2.*, ffxFsr2*
//   in the exe) with a native DX12 path (r.FidelityFX.FSR2.UseNativeDX12), which records FSR 2's
//   own compute passes on the game's command list. Every FSR 2 pass gets the cbFSR2 constants,
//   which start with the render size followed (at byte 8, 16 or 24 depending on the FSR 2
//   version) by the display size. Root constants are checked on every compute pass; constant
//   buffers once every ~2 seconds (a sampled frame). Each pass found is logged with its shader id,
//   constants, dispatch size and the resources in its descriptor tables (read guarded: FSR 2's
//   shaders are DXIL, so their declarations aren't known).
// - The View uniform buffer: where ViewSizeAndInvSize-like entries and ClipToPrevClip sit, and
//   whether UE 4.25's offsets (CallistoDLSS, StrayDLSS) hold.
// - Console variables (read only): FSR 2, anti-aliasing, screen percentage, ray tracing, HDR.
//
// Nothing is set, skipped or recorded on the game's command lists.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "common.hpp"

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../third_party/reshade/utils/descriptor_tracking.hpp"
#include "cvars.hpp"
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// 32-bit values as floats where they look like one, else hex (as the frame capture writes them).
std::string Values(const uint32_t* v, size_t n) {
    std::string s;
    for (size_t k = 0; k < n; ++k) {
        float f;
        memcpy(&f, &v[k], 4);
        const uint32_t e = (v[k] >> 23) & 0xff;
        if (v[k] == 0) s += "          0";
        else if (v[k] < 65536) s += Fmt(" %10u", v[k]);  // small integers (sizes, frame index)
        else if (e > 90 && e < 165) s += Fmt(" %10.6g", f);
        else s += Fmt("  0x%08x", v[k]);
    }
    return s;
}

void LogValues(const char* prefix, const uint32_t* v, size_t n) {
    for (size_t i = 0; i < n; i += 4) Log("%s %4zu:%s", prefix, i * 4, Values(v + i, n - i < 4 ? n - i : 4).c_str());
}

// ---------------------------------------------------------------- state

std::mutex g_mutex;
std::atomic<uint32_t> g_bbW{0}, g_bbH{0};
std::atomic<uint64_t> g_presents{0};
std::atomic<bool> g_sample{false};  // this frame's constant buffers are read
std::atomic<int> g_sampleMaps{0};   // constant buffers read in this sampled frame (capped)
std::atomic<int> g_mapFailures{0};

struct Tables {
    pipeline_layout layout = {0};
    uint32_t first = 0, count = 0;
    descriptor_table t[8] = {};
};

struct ListState {
    uint32_t id = 0;     // compute shader id bound now
    bool fsr2 = false;   // FSR 2 constants seen since that pipeline was bound
    std::string how;     // where they came from
    std::map<uint32_t, Tables> tables;  // compute descriptor tables bound, by first parameter
};
std::unordered_map<uint64_t, ListState> g_lists;

struct Fsr2Pass {
    uint64_t seen = 0, dispatches = 0;
    int logged = 0, dispatchLogged = 0;
    uint32_t renderW = 0, renderH = 0, dispW = 0, dispH = 0;
    int dispOffset = 0;
    std::string how;
    uint32_t x = 0, y = 0, z = 0;  // last dispatch size
    uint64_t lastPresent = 0;
};
std::map<uint32_t, Fsr2Pass> g_fsr2;  // by compute shader id
std::vector<uint32_t> g_fsr2Order;    // in the order first seen
uint64_t g_fsr2LastPresent = 0;
uint32_t g_renderW = 0, g_renderH = 0, g_dispW = 0, g_dispH = 0;

std::string g_viewStatus = "View uniform buffer: not analysed yet";
int g_viewLogs = 0;
bool g_viewDoneThisSample = false;

// ---------------------------------------------------------------- FSR 2 constants

bool Plausible(uint32_t w, uint32_t h) { return w >= 64 && h >= 64 && w <= 16384 && h <= 16384; }

// cbFSR2 starts with iRenderSize; iDisplaySize follows at byte 8 (FSR 2.1) or 16 (2.2, after
// iMaxRenderSize). Display = the back buffer, or the render size scaled evenly in x and y.
bool Fsr2Constants(const uint32_t* c, size_t n, uint32_t bw, uint32_t bh, int* dispOffset) {
    if (n < 12 || !Plausible(c[0], c[1])) return false;
    for (int o : {2, 4, 6}) {
        const uint32_t dw = c[o], dh = c[o + 1];
        if (!Plausible(dw, dh) || dw < c[0] || dh < c[1]) continue;
        const bool screen = dw == bw && dh == bh;
        const float rx = float(c[0]) / dw, ry = float(c[1]) / dh;
        if (screen || (std::fabs(rx - ry) < 0.02f && dw >= 640)) {
            *dispOffset = o * 4;
            return true;
        }
    }
    return false;
}

void OnFsr2Constants(command_list* cl, const uint32_t* c, size_t n, int dispOffset, const std::string& how) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ListState& l = g_lists[cl->get_native()];
    l.fsr2 = true;
    l.how = how;
    if (!l.id) return;
    const bool first = g_fsr2.find(l.id) == g_fsr2.end();
    Fsr2Pass& p = g_fsr2[l.id];
    if (first) g_fsr2Order.push_back(l.id);
    ++p.seen;
    const uint32_t dw = c[dispOffset / 4], dh = c[dispOffset / 4 + 1];
    const bool changed = p.renderW != c[0] || p.renderH != c[1] || p.dispW != dw || p.dispH != dh;
    p.renderW = c[0], p.renderH = c[1], p.dispW = dw, p.dispH = dh, p.dispOffset = dispOffset, p.how = how;
    p.lastPresent = g_presents.load();
    g_fsr2LastPresent = p.lastPresent;
    g_renderW = c[0], g_renderH = c[1], g_dispW = dw, g_dispH = dh;
    if (p.logged < 2 || (changed && p.logged < 12)) {
        ++p.logged;
        Log("FSR 2 constants in cs%08x (%s): render %ux%u, display %ux%u at byte %d%s", l.id, how.c_str(), c[0], c[1], dw, dh,
            dispOffset, changed && p.seen > 1 ? " (size changed)" : "");
        LogValues("  cbFSR2", c, n > 64 ? 64 : n);
    }
}

// ---------------------------------------------------------------- View uniform buffer

bool LooksLikeClipToPrevClip(const float* m) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i])) return false;
    return std::fabs(m[0] - 1) < 0.2f && std::fabs(m[5] - 1) < 0.2f && std::fabs(m[15] - 1) < 0.2f;
}

bool SmallJitterRow(const float* r) {
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(r[i]) || std::fabs(r[i]) >= 0.01f) return false;
    return r[0] != 0 && r[1] != 0;
}

bool NearRcp(float v, float of) { return of > 0 && std::fabs(v * of - 1) < 1e-3f; }

// UE 4.25 offsets (CallistoDLSS, StrayDLSS, TrueColorsDLSS): ViewToClip 448 (its jitter row 480),
// ClipToPrevClip 1952, TemporalAAJitter 2016, ViewRectMin 2064, ViewSizeAndInvSize 2080.
void AnalyseView(const float* v, size_t n, uint32_t param, bool compute) {
    std::string sizes;
    int sizeCount = 0;
    for (size_t i = 0; i + 4 <= n && sizeCount < 8; i += 4) {
        const float w = v[i], h = v[i + 1];
        if (!(w >= 320 && h >= 200 && w <= 16384 && h <= 16384) || w != std::floor(w) || h != std::floor(h)) continue;
        if (!NearRcp(v[i + 2], w) || !NearRcp(v[i + 3], h)) continue;
        sizes += Fmt(" %zu:%gx%g", i * 4, w, h);
        ++sizeCount;
    }
    if (!sizeCount) return;  // not the View buffer
    std::string clip;
    int clipCount = 0;
    for (size_t i = 0; i + 20 <= n && clipCount < 6; i += 4)
        if (LooksLikeClipToPrevClip(v + i) && SmallJitterRow(v + i + 16)) {
            clip += Fmt(" %zu (row after: %g %g %g %g)", i * 4, v[i + 16], v[i + 17], v[i + 18], v[i + 19]);
            ++clipCount;
        }
    bool ue425 = false;
    if (n >= 2096 / 4) {
        const float* j = v + 2016 / 4;
        ue425 = LooksLikeClipToPrevClip(v + 1952 / 4) && j[0] == v[480 / 4] && j[1] == v[480 / 4 + 1] && j[0] != 0 &&
                v[2080 / 4] >= 320 && NearRcp(v[2080 / 4 + 2], v[2080 / 4]);
    }
    const std::string status =
        Fmt("View (%s root param %u): size entries at%s; ClipToPrevClip candidates at%s; UE 4.25 layout %s",
            compute ? "compute" : "graphics", param, sizes.c_str(), clipCount ? clip.c_str() : " none",
            ue425 ? "HOLDS" : "does NOT hold");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_viewDoneThisSample = true;
    // Compare without the jitter values (they change every frame).
    const std::string key = Fmt("%u%d%s%d%d", param, compute, sizes.c_str(), clipCount, ue425);
    static std::string lastKey;
    g_viewStatus = Fmt("View: UE 4.25 layout %s; render size %gx%g (byte 2080)", ue425 ? "holds" : "does NOT hold",
                       n > 2084 / 4 ? v[2080 / 4] : 0.0f, n > 2084 / 4 ? v[2084 / 4] : 0.0f);
    if (key == lastKey || g_viewLogs >= 20) return;
    lastKey = key;
    ++g_viewLogs;
    Log("%s", status.c_str());
    if (n >= 2096 / 4) {
        Log("  ViewToClip jitter row (480): %g %g; ClipToPrevClip (1952) diag %g %g; TemporalAAJitter (2016): %g %g %g %g; "
            "ViewRectMin (2064): %g %g; ViewSizeAndInvSize (2080): %g %g %g %g",
            v[120], v[121], v[488], v[493], v[504], v[505], v[506], v[507], v[516], v[517], v[520], v[521], v[522], v[523]);
    }
}

// ---------------------------------------------------------------- events

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    if ((stages & pipeline_stage::compute_shader) == 0) return;
    const uint32_t id = PipeId(pipe);
    std::lock_guard<std::mutex> lock(g_mutex);
    ListState& l = g_lists[cl->get_native()];
    l.id = id;
    l.fsr2 = false;
}

void OnReset(command_list* cl) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lists.erase(cl->get_native());
}

void OnPushConstants(command_list* cl, shader_stage stages, pipeline_layout, uint32_t param, uint32_t first, uint32_t count,
                     const void* values) {
    if ((stages & shader_stage::compute) == 0 || first != 0 || count < 12) return;
    int off = 0;
    const uint32_t* c = static_cast<const uint32_t*>(values);
    if (Fsr2Constants(c, count, g_bbW.load(), g_bbH.load(), &off))
        OnFsr2Constants(cl, c, count, off, Fmt("root constants, param %u", param));
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout, uint32_t param, const descriptor_table_update& u) {
    if (u.type != descriptor_type::constant_buffer || !g_sample.load(std::memory_order_relaxed)) return;
    device* dev = cl->get_device();
    const bool compute = (stages & shader_stage::compute) != 0;
    for (uint32_t i = 0; i < u.count && i < 4; ++i) {
        if (g_sampleMaps.fetch_add(1) >= 600) return;
        const buffer_range& b = static_cast<const buffer_range*>(u.descriptors)[i];
        if (!b.buffer.handle) continue;
        const resource_desc d = dev->get_resource_desc(b.buffer);
        if (d.type != resource_type::buffer || b.offset >= d.buffer.size) continue;
        uint64_t size = d.buffer.size - b.offset;
        if (b.size != UINT64_MAX && b.size < size) size = b.size;
        bool wantView;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            wantView = !g_viewDoneThisSample;
        }
        const uint64_t want = wantView ? 16384 : 256;
        if (size > want) size = want;
        if (size < 48) continue;
        void* data = nullptr;
        if (!dev->map_buffer_region(b.buffer, b.offset, size, map_access::read_only, &data) || !data) {
            if (g_mapFailures.fetch_add(1) < 5) Log("constant buffer can't be read (heap %u, param %u)", static_cast<uint32_t>(d.heap), param);
            continue;
        }
        std::vector<uint32_t> v(static_cast<size_t>(size / 4));
        memcpy(v.data(), data, v.size() * 4);
        dev->unmap_buffer_region(b.buffer);
        int off = 0;
        if (compute && Fsr2Constants(v.data(), v.size() < 64 ? v.size() : 64, g_bbW.load(), g_bbH.load(), &off))
            OnFsr2Constants(cl, v.data(), v.size() < 64 ? v.size() : 64, off, Fmt("root CBV, param %u", param));
        else if (wantView && v.size() >= 1024)
            AnalyseView(reinterpret_cast<const float*>(v.data()), v.size(), param, compute);
    }
}

void OnBindTables(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                  const descriptor_table* tables, uint32_t, const uint32_t*) {
    if ((stages & shader_stage::compute) == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    Tables& t = g_lists[cl->get_native()].tables[first];
    t.layout = layout, t.first = first, t.count = count < 8 ? count : 8;
    for (uint32_t i = 0; i < t.count; ++i) t.t[i] = tables[i];
}

char Letter(descriptor_type t) {
    switch (t) {
        case descriptor_type::constant_buffer: return 'b';
        case descriptor_type::buffer_unordered_access_view:
        case descriptor_type::texture_unordered_access_view: return 'u';
        default: return 't';
    }
}

// The resources in the compute tables bound now (g_mutex not held: describing can be slow).
void LogTables(device* dev, const std::map<uint32_t, Tables>& all) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return;
    for (const auto& [first, t] : all)
        for (uint32_t i = 0; i < t.count; ++i) {
            const pipeline_layout_param p = dt->get_pipeline_layout_param(t.layout, t.first + i);
            if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) continue;
            for (uint32_t r = 0; r < p.descriptor_table.count; ++r) {
                const descriptor_range& range = p.descriptor_table.ranges[r];
                if (range.type == descriptor_type::sampler) continue;
                const uint32_t n = range.count > 24 ? 24 : range.count;
                for (uint32_t k = 0; k < n; ++k) {
                    descriptor_heap heap = {0};
                    uint32_t offset = 0;
                    dev->get_descriptor_heap_offset(t.t[i], range.binding + k, 0, &heap, &offset);
                    std::string what;
                    if (range.type == descriptor_type::constant_buffer) {
                        const buffer_range b = dt->get_buffer_range(heap, offset);
                        if (!b.buffer.handle) continue;
                        what = Fmt("cbv %llx +%llu", b.buffer.handle, b.offset);
                    } else {
                        const resource_view v = dt->get_resource_view(heap, offset);
                        if (!v.handle) continue;
                        what = DescribeViewSafe(dev, v);
                    }
                    Log("  table param %u: %c%u space%u = %s", t.first + i, Letter(range.type), range.dx_register_index + k,
                        range.dx_register_space, what.c_str());
                }
            }
        }
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t z) {
    uint32_t id = 0;
    std::string how;
    std::map<uint32_t, Tables> tables;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_lists.find(cl->get_native());
        if (it == g_lists.end() || !it->second.fsr2 || !it->second.id) return false;
        Fsr2Pass& p = g_fsr2[it->second.id];
        ++p.dispatches;
        p.x = x, p.y = y, p.z = z;
        if (p.dispatchLogged >= 2) return false;
        ++p.dispatchLogged;
        id = it->second.id;
        how = it->second.how;
        tables = it->second.tables;
    }
    // Find the pipeline handle for its declarations: not kept here; the id is enough to match the
    // frame capture, which has them.
    Log("FSR 2 pass cs%08x DISPATCH %u x %u x %u (constants from %s); bound compute tables:", id, x, y, z, how.c_str());
    LogTables(cl->get_device(), tables);
    return false;
}

// ---------------------------------------------------------------- console variables (read only)

const wchar_t* const kLogged[] = {
    L"r.FidelityFX.FSR2.Enabled", L"r.FidelityFX.FSR2.QualityMode", L"r.FidelityFX.FSR2.UseNativeDX12",
    L"r.FidelityFX.FSR2.Sharpness", L"r.FidelityFX.FSR2.AutoExposure", L"r.FidelityFX.FSR2.CreateReactiveMask",
    L"r.FidelityFX.FSR2.HistoryFormat", L"r.FidelityFX.FSR2.DeDither", L"r.FidelityFX.FSR2.AdjustMipBias",
    L"r.FidelityFX.FSR2.EnableWithIdealSettings", L"r.FidelityFX.FSR2.UseSSRExperimentalDenoiser",
    L"r.FidelityFX.FSR2.ForceVertexDeformationOutputsVelocity", L"r.FidelityFX.FSR.Enabled",
    L"r.FidelityFX.FSR.RCAS.Enabled", L"r.ScreenPercentage", L"r.SecondaryScreenPercentage.GameViewport",
    L"r.TemporalAA.Upsampling", L"r.TemporalAA.Upscaler", L"r.TemporalAASamples", L"r.TemporalAACurrentFrameWeight",
    L"r.DefaultFeature.AntiAliasing", L"r.PostProcessAAQuality", L"r.BasePassOutputsVelocity",
    L"r.VertexDeformationOutputsVelocity", L"r.SSR.ExperimentalDenoiser", L"r.SSR.Quality", L"r.MipMapLODBias",
    L"r.DynamicRes.OperationMode", L"r.RayTracing", L"r.RayTracing.EnableInGame", L"r.RayTracing.Shadows",
    L"r.RayTracing.Reflections", L"r.RayTracing.AmbientOcclusion", L"r.RayTracing.GlobalIllumination",
    L"r.HDR.EnableHDROutput", L"r.HDR.Display.OutputDevice", L"r.HDR.Display.ColorGamut", L"r.HDR.UI.CompositeMode",
    L"r.DefaultBackBufferPixelFormat", L"r.D3D12.UseAllowTearing", L"r.RHICmdBypass", L"r.RHIThread.Enable"};

void* g_manager = nullptr;
bool g_cvarFailed = false;
std::string g_cvarStatus = "console variables: not read yet";

void ConnectCvars() {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    int votes = 0, matches = 0;
    void** singleton = cvars::FindConsoleManagerSingleton(base, &votes, &matches);
    void* manager = nullptr;
    if (singleton) cvars::SafeCopy(&manager, singleton, 8);
    Log("console manager: singleton %p (%d of %d matches), manager %p", singleton, votes, matches, manager);
    if (!manager || !cvars::IsConsoleManager(base, manager)) {
        g_cvarFailed = true;
        g_cvarStatus = "console variables: console manager not found";
        Log("%s", g_cvarStatus.c_str());
        return;
    }
    void* aniso = cvars::FindVariable(manager, L"r.MaxAnisotropy");
    void* screen = cvars::FindVariable(manager, L"r.ScreenPercentage");
    float value = -1;
    if (!aniso || !screen || !cvars::HasModuleVtable(base, screen) || !cvars::FindTypeSlots(aniso, screen) ||
        !cvars::GetFloat(screen, value) || !(value >= 10 && value <= 400)) {
        g_cvarFailed = true;
        g_cvarStatus = "console variables: layout check failed (see the log)";
        Log("cvar layout check failed: r.MaxAnisotropy %p, r.ScreenPercentage %p reads %g, IsVariableBool slot 0x%x", aniso,
            screen, value, cvars::g_isVariableBool);
        return;
    }
    Log("cvar layout check passed (UE 4.25-4.27 layout): r.ScreenPercentage %g, IsVariableBool slot 0x%x", value,
        cvars::g_isVariableBool);
    g_manager = manager;
}

void LogCvars() {
    if (!g_manager) return;
    static std::wstring last;
    std::wstring all;
    for (const wchar_t* name : kLogged) {
        void* var = cvars::FindVariable(g_manager, name);
        wchar_t buf[64] = L"-";
        if (var) cvars::GetText(var, cvars::GetType(var), buf, 64);
        all += buf, all += L";";
    }
    if (all == last) return;
    Log(last.empty() ? "cvars:" : "cvars changed:");
    last = all;
    std::string status;
    for (const wchar_t* name : kLogged) {
        void* var = cvars::FindVariable(g_manager, name);
        if (!var) {
            Log("  %ls not found", name);
            continue;
        }
        wchar_t buf[64] = L"?";
        cvars::GetText(var, cvars::GetType(var), buf, 64);
        Log("  %ls = %ls (%s, set by 0x%02x)", name, buf, cvars::TypeName(cvars::GetType(var)), cvars::Priority(var));
        if (!wcscmp(name, L"r.FidelityFX.FSR2.Enabled")) status += Fmt("FSR2.Enabled %ls", buf);
        if (!wcscmp(name, L"r.FidelityFX.FSR2.QualityMode")) status += Fmt(", QualityMode %ls", buf);
        if (!wcscmp(name, L"r.FidelityFX.FSR2.UseNativeDX12")) status += Fmt(", UseNativeDX12 %ls", buf);
        if (!wcscmp(name, L"r.ScreenPercentage")) status += Fmt(", ScreenPercentage %ls", buf);
        if (!wcscmp(name, L"r.DefaultFeature.AntiAliasing")) status += Fmt(", AA %ls", buf);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cvarStatus = status;
}

// ---------------------------------------------------------------- present

void LogSummary() {
    std::string s;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t id : g_fsr2Order) {
            const Fsr2Pass& p = g_fsr2[id];
            s += Fmt(" cs%08x(%llu, %ux%ux%u)", id, p.dispatches, p.x, p.y, p.z);
        }
        if (s.empty()) s = " none";
        s = Fmt("FSR 2 passes so far (id(dispatches, last size)):%s; render %ux%u -> display %ux%u; back buffer %ux%u", s.c_str(),
                g_renderW, g_renderH, g_dispW, g_dispH, g_bbW.load(), g_bbH.load());
    }
    static std::string last;
    // Without the counts, so it's only logged when the set of passes or the sizes change.
    std::string key;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t id : g_fsr2Order) key += Fmt("%08x;", id);
        key += Fmt("%u %u %u %u %u %u", g_renderW, g_renderH, g_dispW, g_dispH, g_bbW.load(), g_bbH.load());
    }
    if (key == last) return;
    last = key;
    Log("%s", s.c_str());
}

void OnPresent(command_queue*, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    const uint64_t n = ++g_presents;
    if (n == 1 || n % 300 == 0) {
        const resource bb = sc->get_current_back_buffer();
        const resource_desc d = sc->get_device()->get_resource_desc(bb);
        if (g_bbW.load() != d.texture.width || g_bbH.load() != d.texture.height)
            Log("back buffer %s", DescribeResource(sc->get_device(), bb).c_str());
        g_bbW = d.texture.width, g_bbH = d.texture.height;
    }
    // One frame in 120 has its constant buffers read.
    const bool sample = n % 120 == 0;
    if (sample) {
        g_sampleMaps = 0;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_viewDoneThisSample = false;
    }
    g_sample = sample;
    if (n == 300 && !g_manager && !g_cvarFailed) ConnectCvars();
    if (n >= 300 && n % 600 == 0) {
        LogCvars();
        LogSummary();
    }
}

}  // namespace

void RegisterProbe() {
    Log("probe: watches the game's FSR 2 passes, View uniform buffer and console variables; changes nothing");
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::reset_command_list>(&OnReset);
    reshade::register_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
}

void UnregisterProbe() {
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(&OnReset);
    reshade::unregister_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::unregister_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
    LogSummary();
    Log("probe: unloaded after %llu presents", g_presents.load());
}

void DrawProbeStatus() {
    std::string fsr, view, cv;
    uint64_t presents = g_presents.load(), last;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        last = g_fsr2LastPresent;
        if (g_fsr2Order.empty()) fsr = "Game's FSR 2: not seen yet";
        else
            fsr = Fmt("Game's FSR 2: %s, %zu passes, render %ux%u -> %ux%u", presents - last < 240 ? "running" : "not running now",
                      g_fsr2Order.size(), g_renderW, g_renderH, g_dispW, g_dispH);
        view = g_viewStatus;
        cv = g_cvarStatus;
    }
    ImGui::TextUnformatted(fsr.c_str());
    ImGui::TextUnformatted(view.c_str());
    ImGui::TextWrapped("%s", cv.c_str());
    ImGui::TextDisabled("Back buffer %ux%u", g_bbW.load(), g_bbH.load());
}

}  // namespace rd
