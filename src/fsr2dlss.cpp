// DLSS in place of Dead Island 2's FSR 2 (0.1.0).
//
// The game runs AMD's FSR 2.1 through its UE plugin with FFX's native DX12 backend: six compute
// passes recorded on the game's command list (docs/FINDINGS.md). Each gets cbFSR2 as 37 root
// constants at root parameter 2, its UAVs in the descriptor table at parameter 0 and its SRVs in
// the table at parameter 1. FFX binds the tables before the pipeline and the constants after it.
// In order:
//   0 compute luminance pyramid  (also 6 SPD root constants at parameter 3): t0 = input colour
//   1 prepare input colour
//   2 reconstruct + dilate       t0 = motion vectors (render size rg16f, converted by the plugin
//                                from UE's velocity), t1 = UE's scene depth (full-size texture,
//                                the view at its top-left), t2 = reactive mask, t3 = transparency
//                                and composition mask
//   3 depth clip
//   4 lock
//   5 accumulate                 u2 = the upscaled output (display size rgba16f), which the
//                                post-processing reads. Dispatched over the display size / 8.
// The passes are told apart by those constants and their order in the command list, not by
// shader ids (kept in docs/FINDINGS.md and logged), so a game patch that rebuilds the shaders
// doesn't break it.
//
// At the accumulate dispatch DLSS takes the inputs gathered from passes 0 and 2, writes the
// output, and the game's accumulate is skipped. Once DLSS has run, passes 0-4 are skipped too
// (their results only feed FSR 2 itself). cbFSR2 gives the sizes (bytes 0 and 8), the frame index
// (byte 28: a drop means FSR 2 was reset) and the jitter in render pixels (byte 40).
//
// The render resolution is the game's: its FSR 2 quality setting.
//
// NGX records its own compute state and descriptor heaps on the game's command list, so every
// command list's bindings are tracked and put back afterwards (from CallistoDLSS).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>

#include "common.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../third_party/reshade/utils/descriptor_tracking.hpp"
#include "dlss.hpp"
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

constexpr int kToggleHotkey = VK_F9;

// FSR 2's root parameters (FFX native DX12 backend).
constexpr uint32_t kUavTable = 0, kSrvTable = 1, kCbParam = 2, kSpdParam = 3;
constexpr uint32_t kCbDwords = 37, kSpdDwords = 6;
// Registers, by pass (FSR 2.1's binding order, confirmed by the 0.0.1 capture).
constexpr uint32_t kPyramidColor = 0;                                       // t
constexpr uint32_t kReconMotion = 0, kReconDepth = 1, kReconReactive = 2;  // t
constexpr uint32_t kAccumulateOutput = 2;                                   // u
constexpr int kReconstructIndex = 2;

// Ids from the 0.0.1 capture (game build 25629748), for the log only.
const uint32_t kKnownIds[6] = {0x07e74148, 0x78b5ccfe, 0x240d257d, 0x089f5b7b, 0x3a8b05d2, 0xfbd2dd5b};

// ---------------------------------------------------------------- settings

enum MotionUnits { kMotionUv, kMotionPixels, kMotionUnitsCount };
const char* const kMotionUnitsNames[kMotionUnitsCount] = {"UV (scaled by render size)", "Render pixels"};

struct Settings {
    bool enabled = true;
    int preset = dlss::kPresetAuto;  // DlssModel
    int motionUnits = kMotionUv;     // MotionUnits (test)
    bool flipJitter = false;         // FlipJitter (test)
    bool flipMotionX = false, flipMotionY = false;  // FlipMotionX/Y (test)
    bool skipPasses = true;          // SkipFsr2Passes: skip FSR 2's first five passes while DLSS runs
    bool reactiveBias = false;       // ReactiveBias: the game's reactive mask as DLSS's bias mask
} g_set;

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    reshade::get_config_value(nullptr, RD_PRODUCT, "MotionUnits", g_set.motionUnits);
    if (g_set.motionUnits < 0 || g_set.motionUnits >= kMotionUnitsCount) g_set.motionUnits = kMotionUv;
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitter", g_set.flipJitter);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipMotionX", g_set.flipMotionX);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipMotionY", g_set.flipMotionY);
    reshade::get_config_value(nullptr, RD_PRODUCT, "SkipFsr2Passes", g_set.skipPasses);
    reshade::get_config_value(nullptr, RD_PRODUCT, "ReactiveBias", g_set.reactiveBias);
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "MotionUnits", g_set.motionUnits);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitter", g_set.flipJitter);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipMotionX", g_set.flipMotionX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipMotionY", g_set.flipMotionY);
    reshade::set_config_value(nullptr, RD_PRODUCT, "SkipFsr2Passes", g_set.skipPasses);
    reshade::set_config_value(nullptr, RD_PRODUCT, "ReactiveBias", g_set.reactiveBias);
}

// ---------------------------------------------------------------- status

std::recursive_mutex g_mutex;  // recursive: NGX calls can re-enter ReShade's events on this thread
std::atomic<uint64_t> g_presents{0};
std::atomic<uint64_t> g_lastDlssPresent{0}, g_lastFsr2Present{0};
std::atomic<bool> g_lastOk{false};  // DLSS ran at the last accumulate
bool g_resetNext = true;
uint32_t g_lastFrameIndex = 0;
unsigned g_render[2] = {}, g_display[2] = {};
std::string g_error;  // the last failure, for the overlay
std::map<std::string, uint32_t> g_why;  // reasons the game's FSR 2 was kept, this log window
uint32_t g_evaluated = 0, g_kept = 0, g_skippedPasses = 0;  // this log window
int g_statLines = 0;
std::set<uint32_t> g_idsLogged;

void Why(const std::string& why) {
    ++g_why[why];
    ++g_kept;
}

// ---------------------------------------------------------------- per command list bindings

struct StageState {
    pipeline_layout layout = {0};
    std::vector<descriptor_table> tables;
    struct Root {
        shader_stage stages;
        descriptor_type type;
        buffer_range range;
    };
    std::map<uint32_t, Root> roots;
    struct Constants {
        shader_stage stages;
        std::vector<uint32_t> values;
    };
    std::map<uint32_t, Constants> constants;

    void Layout(pipeline_layout l) {
        if (l == layout) return;
        layout = l;  // a new root signature resets every binding
        tables.clear();
        roots.clear();
        constants.clear();
    }
};

// One FSR 2 run's inputs, gathered in the command list as its passes are recorded.
struct Chain {
    int next = -1;  // index of the next FSR 2 pass (-1: none seen since the last accumulate)
    resource color = {0}, depth = {0}, motion = {0}, reactive = {0};
};

struct __declspec(uuid("b6a2c5e4-1f3d-4e8a-9c71-2d5e8f0a3b47")) ListState {
    pipeline pipe = {0};
    pipeline_stage pipeStages = pipeline_stage::all;
    uint32_t computeId = 0;
    bool cbFresh = false, spdFresh = false;  // FSR 2 / SPD constants pushed since the pipeline bind
    uint32_t cb[kCbDwords] = {};
    StageState compute, graphics;
    Chain chain;
};

ListState* State(command_list* cl) { return cl->get_private_data<ListState>(); }

StageState* Stage(ListState* s, shader_stage stages) {
    if ((stages & shader_stage::all_compute) != 0) return &s->compute;
    if ((stages & shader_stage::all_graphics) != 0) return &s->graphics;
    return nullptr;
}

void OnInitList(command_list* cl) { cl->create_private_data<ListState>(); }
void OnDestroyList(command_list* cl) { cl->destroy_private_data<ListState>(); }

void OnResetList(command_list* cl) {
    if (ListState* s = State(cl)) *s = ListState();
}

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    ListState* s = State(cl);
    if (!s) return;
    s->pipe = pipe;
    s->pipeStages = stages;
    s->computeId = PipeId(pipe);
    s->cbFresh = s->spdFresh = false;
}

void OnBindTables(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                  const descriptor_table* tables, uint32_t, const uint32_t*) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    if (st->tables.size() < first + count) st->tables.resize(first + count, descriptor_table{0});
    for (uint32_t i = 0; i < count; ++i) st->tables[first + i] = tables[i];
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& u) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    if (u.count != 1 || u.table.handle) return;
    if (u.type == descriptor_type::constant_buffer || u.type == descriptor_type::buffer_shader_resource_view ||
        u.type == descriptor_type::buffer_unordered_access_view)
        st->roots[param] = {stages, u.type, *static_cast<const buffer_range*>(u.descriptors)};
}

bool Plausible(uint32_t w, uint32_t h) { return w >= 64 && h >= 64 && w <= 16384 && h <= 16384; }

void OnPushConstants(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first,
                     uint32_t count, const void* values) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    StageState::Constants& c = st->constants[param];
    c.stages = stages;
    if (c.values.size() < first + count) c.values.resize(first + count);
    memcpy(c.values.data() + first, values, count * 4);
    if ((stages & shader_stage::compute) == 0 || first != 0) return;
    const uint32_t* v = static_cast<const uint32_t*>(values);
    if (param == kCbParam && count == kCbDwords && Plausible(v[0], v[1]) && Plausible(v[2], v[3]) && v[2] >= v[0] &&
        v[3] >= v[1]) {
        memcpy(s->cb, v, sizeof(s->cb));
        s->cbFresh = true;
    } else if (param == kSpdParam && count == kSpdDwords) {
        s->spdFresh = true;
    }
}

// Puts the game's bindings back after DLSS. A bind with no tables makes ReShade set the game's
// descriptor heaps and root signature again (NGX changed both behind its back).
void RestoreStage(command_list* cl, const StageState& st, shader_stage stages) {
    if (!st.layout.handle) return;
    cl->bind_descriptor_tables(stages, st.layout, 0, 0, nullptr);
    for (uint32_t i = 0; i < st.tables.size(); ++i)
        if (st.tables[i].handle) cl->bind_descriptor_tables(stages, st.layout, i, 1, &st.tables[i]);
    for (const auto& [param, r] : st.roots) {
        descriptor_table_update u = {};
        u.count = 1;
        u.type = r.type;
        u.descriptors = &r.range;
        cl->push_descriptors(r.stages, st.layout, param, u);
    }
    for (const auto& [param, c] : st.constants)
        cl->push_constants(c.stages, st.layout, param, 0, static_cast<uint32_t>(c.values.size()), c.values.data());
}

void Restore(command_list* cl, const ListState& s) {
    RestoreStage(cl, s.graphics, shader_stage::all_graphics);
    RestoreStage(cl, s.compute, shader_stage::all_compute);
    if (s.pipe.handle) cl->bind_pipeline(s.pipeStages, s.pipe);
}

// ---------------------------------------------------------------- FSR 2's bindings

// A view's resource and its description, guarded: FSR 2's tables are read without the shaders'
// declarations (DXIL), and a stale descriptor's resource may be gone.
bool TryResource(device* dev, resource_view v, resource* r, resource_desc* d) {
    __try {
        *r = dev->get_resource_from_view(v);
        if (!r->handle) return false;
        *d = dev->get_resource_desc(*r);
        return d->type == resource_type::texture_2d;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The texture at register <reg> of the table bound at compute parameter <param>, or {0}.
resource Bound(command_list* cl, const StageState& st, uint32_t param, uint32_t reg, resource_desc* desc) {
    if (param >= st.tables.size() || !st.tables[param].handle) return {0};
    device* dev = cl->get_device();
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return {0};
    const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, param);
    if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) return {0};
    for (uint32_t r = 0; r < p.descriptor_table.count; ++r) {
        const descriptor_range& range = p.descriptor_table.ranges[r];
        if (range.dx_register_space != 0 || reg < range.dx_register_index || reg - range.dx_register_index >= range.count)
            continue;
        descriptor_heap heap = {0};
        uint32_t offset = 0;
        dev->get_descriptor_heap_offset(st.tables[param], range.binding + (reg - range.dx_register_index), 0, &heap, &offset);
        const resource_view v = dt->get_resource_view(heap, offset);
        resource res = {0};
        resource_desc d = {};
        if (!v.handle || !TryResource(dev, v, &res, &d)) return {0};
        if (desc) *desc = d;
        return res;
    }
    return {0};
}

bool IsHdrColor(format f) { return f == format::r16g16b16a16_float || f == format::r11g11b10_float; }

void LogId(int index, uint32_t id) {
    if (!g_idsLogged.insert(id).second || g_idsLogged.size() > 32) return;
    const bool known = index >= 0 && index < 6 && kKnownIds[index] == id;
    Log("FSR 2 pass %d is cs%08x%s", index, id, known ? "" : " (not the 0.0.1 capture's id)");
}

// ---------------------------------------------------------------- DLSS at the accumulate

// True if the game's accumulate is to be skipped (DLSS wrote its output).
bool OnAccumulate(command_list* cl, ListState& s) {
    Chain& c = s.chain;
    const uint32_t* cb = s.cb;
    const unsigned rw = cb[0], rh = cb[1], dw = cb[2], dh = cb[3];
    float jitter[2];
    memcpy(jitter, cb + 10, 8);
    const uint32_t frameIndex = cb[7];
    if (c.next != 5) {
        Why(c.next < 0 ? "FSR 2's earlier passes weren't in this command list" : "FSR 2 ran an unexpected number of passes");
        return false;
    }
    resource_desc od = {}, cd = {}, dd = {}, md = {};
    const resource output = Bound(cl, s.compute, kUavTable, kAccumulateOutput, &od);
    if (!output.handle || !c.color.handle || !c.depth.handle || !c.motion.handle) {
        Why("an input is missing");
        return false;
    }
    device* dev = cl->get_device();
    cd = dev->get_resource_desc(c.color);
    dd = dev->get_resource_desc(c.depth);
    md = dev->get_resource_desc(c.motion);
    if (od.texture.width != dw || od.texture.height != dh || !IsHdrColor(od.texture.format)) {
        Why("the output isn't the display-size HDR texture");
        return false;
    }
    if (!IsHdrColor(cd.texture.format) || cd.texture.width < rw || cd.texture.height < rh || dd.texture.width < rw ||
        dd.texture.height < rh || md.texture.width < rw || md.texture.height < rh) {
        Why("colour, depth or motion isn't as expected");
        return false;
    }
    if (!(std::fabs(jitter[0]) <= 0.5f && std::fabs(jitter[1]) <= 0.5f)) {
        Why("jitter out of range");
        return false;
    }

    ID3D12Device* native = reinterpret_cast<ID3D12Device*>(dev->get_native());
    std::string err;
    if (!dlss::Init(native, err)) {
        g_error = err;
        Why("DLSS isn't available");
        return false;
    }
    const bool reset = g_resetNext || frameIndex < g_lastFrameIndex || frameIndex == 0 || rw != g_render[0] ||
                       rh != g_render[1] || dw != g_display[0] || dh != g_display[1];
    g_resetNext = false;
    g_lastFrameIndex = frameIndex;
    if (rw != g_render[0] || rh != g_render[1] || dw != g_display[0] || dh != g_display[1])
        Log("FSR 2 render %ux%u -> %ux%u: DLSS takes it (colour %ux%u fmt %u, depth %ux%u fmt %u, motion %ux%u fmt %u, output fmt %u)",
            rw, rh, dw, dh, cd.texture.width, cd.texture.height, static_cast<uint32_t>(cd.texture.format), dd.texture.width,
            dd.texture.height, static_cast<uint32_t>(dd.texture.format), md.texture.width, md.texture.height,
            static_cast<uint32_t>(md.texture.format), static_cast<uint32_t>(od.texture.format));
    g_render[0] = rw, g_render[1] = rh, g_display[0] = dw, g_display[1] = dh;

    dlss::Inputs in;
    in.color = reinterpret_cast<ID3D12Resource*>(c.color.handle);
    in.depth = reinterpret_cast<ID3D12Resource*>(c.depth.handle);
    in.motion = reinterpret_cast<ID3D12Resource*>(c.motion.handle);
    in.output = reinterpret_cast<ID3D12Resource*>(output.handle);
    if (g_set.reactiveBias && c.reactive.handle) in.biasMask = reinterpret_cast<ID3D12Resource*>(c.reactive.handle);
    in.renderWidth = rw, in.renderHeight = rh;
    in.outputWidth = dw, in.outputHeight = dh;
    const float js = g_set.flipJitter ? -1.0f : 1.0f;
    in.jitterX = js * jitter[0], in.jitterY = js * jitter[1];
    const float ms = g_set.motionUnits == kMotionUv ? 1.0f : 0.0f;
    in.motionScaleX = (g_set.flipMotionX ? -1.0f : 1.0f) * (ms ? float(rw) : 1.0f);
    in.motionScaleY = (g_set.flipMotionY ? -1.0f : 1.0f) * (ms ? float(rh) : 1.0f);
    in.reset = reset;
    in.preset = g_set.preset;
    const bool ok = dlss::Evaluate(reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()), in, err);
    cl->barrier(output, resource_usage::unordered_access, resource_usage::unordered_access);
    Restore(cl, s);
    if (!ok) {
        if (g_error != err) Log("DLSS: %s", err.c_str());
        g_error = err;
        g_resetNext = true;
        Why("DLSS failed");
        return false;
    }
    if (g_evaluated == 0 && g_lastDlssPresent.load() == 0)
        Log("DLSS: first frame %ux%u -> %ux%u, jitter %.3f %.3f, frame index %u", rw, rh, dw, dh, in.jitterX, in.jitterY,
            frameIndex);
    g_error.clear();
    ++g_evaluated;
    g_lastDlssPresent = g_presents.load();
    return true;
}

bool OnDispatch(command_list* cl, uint32_t x, uint32_t y, uint32_t) {
    ListState* s = State(cl);
    if (!s || !s->cbFresh) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_lastFsr2Present = g_presents.load();
    Chain& c = s->chain;
    const uint32_t* cb = s->cb;
    // The accumulate covers the display size in 8x8 tiles; the other passes the render size.
    const bool accumulate = x * 8 >= cb[2] && y * 8 >= cb[3] && x * 8 < cb[2] + 8 && y * 8 < cb[3] + 8 && !s->spdFresh;
    if (s->spdFresh) {
        c = Chain();
        c.next = 0;
        c.color = Bound(cl, s->compute, kSrvTable, kPyramidColor, nullptr);
    }
    const int index = accumulate ? 5 : c.next;
    if (index >= 0) LogId(index, s->computeId);
    if (index == kReconstructIndex) {
        c.motion = Bound(cl, s->compute, kSrvTable, kReconMotion, nullptr);
        c.depth = Bound(cl, s->compute, kSrvTable, kReconDepth, nullptr);
        c.reactive = Bound(cl, s->compute, kSrvTable, kReconReactive, nullptr);
    }
    if (!g_set.enabled) {
        c.next = accumulate ? -1 : (c.next >= 0 ? c.next + 1 : -1);
        g_lastOk = false;
        return false;
    }
    if (accumulate) {
        const bool ok = OnAccumulate(cl, *s);
        g_lastOk = ok;
        c = Chain();
        return ok;
    }
    if (c.next >= 0) ++c.next;
    // FSR 2's own passes: only FSR 2 reads their results. Skipped once DLSS runs.
    if (g_set.skipPasses && g_lastOk.load() && index >= 0 && index < 5) {
        ++g_skippedPasses;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- present

bool g_keyWasDown = false;

void LogStats() {
    if (g_presents.load() % 240 != 0) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const bool problem = g_kept > 0;
    if (problem && g_statLines < 40) {
        std::string why;
        for (const auto& [w, n] : g_why) why += " {" + w + ": " + std::to_string(n) + "}";
        Log("diag: DLSS %u, FSR 2 kept %u, passes skipped %u, render %ux%u ->%ux%u:%s%s", g_evaluated, g_kept,
            g_skippedPasses, g_render[0], g_render[1], g_display[0], g_display[1], why.c_str(),
            ++g_statLines == 40 ? " (last diag line)" : "");
    }
    g_why.clear();
    g_evaluated = g_kept = g_skippedPasses = 0;
}

void OnPresent(command_queue*, swapchain*, const rect*, const rect*, uint32_t, const rect*) {
    ++g_presents;
    LogStats();
    dlss::EndFrame();
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_set.enabled = !g_set.enabled;
        g_resetNext = true;
        SaveSettings();
        Log("DLSS %s (F9)", g_set.enabled ? "on" : "off");
    }
    g_keyWasDown = down;
}

void OnDestroyDevice(device*) { dlss::Shutdown(); }

bool Running() { return g_presents.load() - g_lastDlssPresent.load() < 30 && g_lastDlssPresent.load() > 0; }
bool Fsr2Seen() { return g_presents.load() - g_lastFsr2Present.load() < 30 && g_lastFsr2Present.load() > 0; }

}  // namespace

void RestoreListState(command_list* cl) {
    if (ListState* s = State(cl)) Restore(cl, *s);
}

void RegisterUpscaler() {
    LoadSettings();
    Log("DLSS in place of the game's FSR 2: %s (F9), model %d, motion %s%s%s, jitter%s flipped, FSR 2 passes %s",
        g_set.enabled ? "on" : "off", g_set.preset, kMotionUnitsNames[g_set.motionUnits], g_set.flipMotionX ? ", x flipped" : "",
        g_set.flipMotionY ? ", y flipped" : "", g_set.flipJitter ? "" : " not", g_set.skipPasses ? "skipped" : "kept");
    reshade::register_event<reshade::addon_event::init_command_list>(&OnInitList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyList);
    reshade::register_event<reshade::addon_event::reset_command_list>(&OnResetList);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::register_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::register_event<reshade::addon_event::present>(&OnPresent);
}

void UnregisterUpscaler() {
    reshade::unregister_event<reshade::addon_event::init_command_list>(&OnInitList);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(&OnDestroyList);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(&OnResetList);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::unregister_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::unregister_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::unregister_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::unregister_event<reshade::addon_event::present>(&OnPresent);
}

std::string UpscalerHeadline(bool* warn) {
    *warn = false;
    if (!g_set.enabled) return "DLSS off (F9): the game's FSR 2 runs";
    if (Running()) {
        char buf[160];
        snprintf(buf, sizeof(buf), "DLSS running: %ux%u -> %ux%u (render size = the game's FSR 2 quality)", g_render[0],
                 g_render[1], g_display[0], g_display[1]);
        return buf;
    }
    *warn = true;
    if (!g_error.empty()) return "DLSS not running: " + g_error;
    if (!Fsr2Seen()) return "Waiting for the game's FSR 2: choose FSR 2 in the game's graphics settings";
    return "DLSS not running: the game's FSR 2 is kept (see the log)";
}

void DrawUpscalerSettings() {
    bool changed = false;
    if (ImGui::Checkbox("DLSS in place of FSR 2 (F9)", &g_set.enabled)) changed = true, g_resetNext = true;
    Help("Choose FSR 2 in the game's graphics settings; its quality mode sets the render resolution, and DLSS "
         "upscales instead of FSR 2.");
    static const char* const kModels[] = {"Automatic", "NVIDIA default", "J", "K", "L", "M"};
    static const int kModelValues[] = {dlss::kPresetAuto, 0, 10, 11, 12, 13};
    int sel = 0;
    for (int i = 0; i < 6; ++i)
        if (kModelValues[i] == g_set.preset) sel = i;
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::Combo("DLSS model", &sel, kModels, 6)) g_set.preset = kModelValues[sel], changed = true;
    Help("Automatic: K for Quality and Balanced, M for Performance, L for Ultra Performance. The NVIDIA app's "
         "override, if set, wins.");
    if (changed) SaveSettings();
}

void DrawUpscalerStatus() {
    ImGui::TextUnformatted(dlss::Status().c_str());
    ImGui::Text("Game's FSR 2: %s", Fsr2Seen() ? "running" : "not seen in the last frames");
    if (!g_error.empty()) ImGui::TextColored(kWarn, "Last error: %s", g_error.c_str());
}

void DrawUpscalerAdvanced() {
    bool changed = false;
    ImGui::SeparatorText("Testing");
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::Combo("Motion vector units", &g_set.motionUnits, kMotionUnitsNames, kMotionUnitsCount)) changed = true;
    Help("How the game's FSR 2 motion vectors are scaled for DLSS. Wrong units smear moving things badly.");
    if (ImGui::Checkbox("Flip motion x", &g_set.flipMotionX)) changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Flip motion y", &g_set.flipMotionY)) changed = true;
    if (ImGui::Checkbox("Flip jitter", &g_set.flipJitter)) changed = true, g_resetNext = true;
    Help("With the wrong jitter sign the picture shimmers and looks soft when standing still.");
    if (ImGui::Checkbox("Skip FSR 2's own passes while DLSS runs", &g_set.skipPasses)) changed = true;
    if (ImGui::Checkbox("Reactive mask as DLSS bias mask", &g_set.reactiveBias)) changed = true, g_resetNext = true;
    Help("Gives DLSS the game's FSR 2 reactive mask (particles, transparency). Off by default.");
    if (changed) SaveSettings();
}

}  // namespace rd
