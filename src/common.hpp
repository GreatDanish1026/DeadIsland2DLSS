// Shared between the capture tool (main.cpp), the TSR hook (upscaler.cpp) and the NGX wrapper
// (dlss.cpp).
#pragma once

#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>

#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace rd {

// DeadIsland2DLSS.log next to the game exe.
void Log(const char* fmt, ...);

// The folder of the game exe, with a trailing backslash.
const std::wstring& ExeDir();

// FNV-1a of a compute pipeline's shader bytecode (0 if unknown): stable across runs.
uint32_t PipeId(reshade::api::pipeline pipe);
// True if the compute shader declares exactly t0-t5 and u0 (DXBC), the TAA's registers: only then
// are its bindings read to check whether it's a TAA permutation (undeclared slots can hold
// descriptors of freed resources).
bool PipeHasTaaRegisters(reshade::api::pipeline pipe);
// True if the compute shader (DXBC) declares exactly the SRVs t0..t63 in <srvMask> and the UAVs in
// <uavMask>: only then may those bindings be read (see above).
bool PipeDeclaresExactly(reshade::api::pipeline pipe, uint64_t srvMask, uint64_t uavMask);

// The upscaler's compute shaders. True Colors (UE 4.25) upscales with TAA/TAAU, not TSR, so
// these are placeholders until a frame capture maps the passes (docs/FINDINGS.md); while
// kTsrMapped is false, DLSS and frame generation stay off and F10 captures every compute pass
// in full. The names are kept from the TSR add-ons for now. Placeholders are never 0 (unknown
// pipelines hash to 0).
constexpr bool kTsrMapped = false;  // Dead Island 2: not mapped yet (0.0.x probe)
// Frame generation's inputs go to their frame when the game submits them (false; UE 4.25 submits
// and presents on its RHI thread) or when they're recorded (true, ReunionDLSS: UE 5.6 submits on a
// thread of its own). framegen.cpp is shared with ReunionDLSS and DoubleExposureDLSS.
constexpr bool kTagAtRecord = false;
// UE 4.25's main TAA (TemporalAA.usf compute, the full-screen permutation at 100% screen
// percentage; frame capture 2026-10-27, docs/FINDINGS.md): t0 exposure (1x1), t1 scene colour
// (rgba16f, translucency already composited), t2 depth, t3 velocity (rg16unorm), t4 stencil,
// t5 history; u0 output (= next frame's history); root CBV 3 its parameters, 4 the View buffer.
// The Callisto Protocol (UE 4.27.2, Striking Distance's fork) has a TAA of its own in seven compute
// passes (capture 2026-11-12, docs/FINDINGS.md). kTaaMain is its last, the resolve: t0 scene colour
// (r11g11b10f), t1 depth, u3 the output post-processing reads, u0 next frame's history (ping-pong);
// root CBV 4 the View buffer. UE's velocity (rg16unorm) is only bound earlier, at t1 of the
// velocity dilation pass kTaaVelocityPass, in the same command list.
// Dead Island 2: placeholders until a capture maps its passes (CallistoDLSS had 0x1bf8ea52 / 0x1f086929).
constexpr uint32_t kTaaMain = 0xdead000a;
constexpr uint32_t kTaaVelocityPass = 0xdead000b;
constexpr uint32_t kTaaColorReg = 0, kTaaDepthReg = 1, kTaaOutputReg = 3, kTaaHistoryReg = 0;
// The TAA's passes between the velocity pass and the resolve, whose results only the TAA itself
// reads (capture 2026-11-12): skipped, with the velocity pass, while the upscaler takes the resolve.
constexpr uint32_t kTaaSkippable[] = {0xdead000c};
constexpr uint32_t kTsrClearPrevTextures = 0xdead0001;
constexpr uint32_t kTsrForwardScatterDepth = 0xdead0002;
constexpr uint32_t kTsrDilateVelocity = 0xdead0003;
constexpr uint32_t kTsrDecimateHistory = 0xdead0004;
constexpr uint32_t kTsrRejectShading = 0xdead0005;
constexpr uint32_t kTsrSpatialAntiAliasing = 0xdead0006;
constexpr uint32_t kTsrFilterAntiAliasing = 0xdead0007;
constexpr uint32_t kTsrUpdateHistory = 0xdead0008;
constexpr uint32_t kTsrResolveHistory = 0xdead0009;

// upscaler.cpp
void RegisterUpscaler();
void UnregisterUpscaler();
// Overlay: the Settings tab and the Status tab.
void DrawUpscalerSettings();
void DrawUpscalerStatus();
void DrawUpscalerAdvanced();
// One line for the top of the overlay; *warn when it's a problem.
std::string UpscalerHeadline(bool* warn);

// Overlay helpers (main.cpp), in the Tweaks add-ons' style.
float ControlWidth();
void Help(const char* text);  // a "(?)" after the last item, with <text> as its tooltip
extern const ImVec4 kWarn;
// Puts the game's pipeline, root signatures, descriptor heaps and bindings back on <cl> after
// the add-on recorded work of its own on it.
void RestoreListState(reshade::api::command_list* cl);

// framegen.cpp: DLSS Frame Generation through Streamline.
void RegisterFrameGen();
void UnregisterFrameGen();
void DrawFrameGenSettings();
void DrawFrameGenStatus();
std::string FrameGenHeadline(bool* warn);
// The View uniform buffer (first 16 KB) of this frame's main view, with View.ClipToPrevClip's offset.
void FrameGenSetCamera(const float* view, int clipToPrevClipOffset);
// True if a TSR output of this size is the picture on screen (the back buffer's size).
bool FrameGenIsScreen(unsigned width, unsigned height);
std::string FrameGenDiag();  // diagnostics for the upscaler's diag line
// This frame's depth (UE's, state depth read + shader resource) and our motion vectors (render
// pixels, previous minus current; state non-pixel shader resource), tagged on <list>.
void FrameGenTag(ID3D12GraphicsCommandList* list, ID3D12Resource* depth, ID3D12Resource* motion, unsigned renderWidth,
                 unsigned renderHeight, float jitterX, float jitterY, bool reset);

// probe.cpp (0.0.x, RD_PROBE): read-only logging of the game's FSR 2 passes, View uniform buffer
// and console variables.
// main.cpp helpers for it: the registers a compute shader declares ("not DXBC" for DXIL), a view's
// resource described (guarded: stale descriptors fault), a resource described.
std::string PipeDeclText(reshade::api::pipeline pipe);
std::string DescribeViewSafe(reshade::api::device* dev, reshade::api::resource_view v);
std::string DescribeResource(reshade::api::device* dev, reshade::api::resource r);
void RegisterProbe();
void UnregisterProbe();
void DrawProbeStatus();

}  // namespace rd
