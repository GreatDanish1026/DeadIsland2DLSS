# Dead Island 2: findings

DeadIsland2DLSS adds DLSS (and later FSR 3.1/4 and frame generation) to Dead Island 2 in place of
the game's own FSR 2. Forked 2026-11-20 from CallistoDLSS 1.0.5 (The Callisto Protocol, UE 4.27.2,
which ships the same AMD FSR 2 UE plugin).

## Install (static, 2026-11-20)

- Steam, app 934700, build 25629748 (updated 2026-11-20 11:27), `E:\SteamLibrary\steamapps\common\Dead Island 2`.
- Exe: `DeadIsland\Binaries\Win64\DeadIsland-Win64-Shipping.exe`, 119,669,760 bytes, ProductVersion
  1.8.0.0 (PLAION). Launcher stub `DeadIsland.exe` in the game root.
- Not protected: normal sections (.text .rdata .data .pdata .msvcjmc _RDATA .rsrc .reloc), no
  Denuvo/SteamStub sections, .text entropy 6.86. No anti-cheat files (EOS SDK only for online).
- DirectX 12 (D3D12 RHI strings, ray tracing code compiled in but the game has no RT options).
  Only PCD3D_SM5 shader format strings: the engine's own shaders are DXBC (SM 5.x), as in Callisto.
- Settings: `%LOCALAPPDATA%\DeadIsland\Saved\Config\WindowsNoEditor` (GameUserSettings.ini has
  `[/Script/DeadIsland.DIGameUserSettings]`, sg.AntiAliasingQuality=3, 4K, HDR on, 1000 nits).
  The upscaler choice isn't in GameUserSettings.ini (probably in the save/profile); the probe
  reads the cvars instead.
- Shipping log is almost empty (memory stats only): no engine version in it.

## Engine

- Unreal Engine 4, Dambuster's own branch: build string `++depot+dev+unreal+UE_MS`, no
  `++UE4+Release-4.xx` string. No r.Lumen/r.Nanite/r.TSR (not UE 5). IoStore paks (`global.utoc`,
  `.ucas`): 4.25 or later. Has r.RayTracing.Reflections.ExperimentalDeferred (4.26+),
  D3D12.AFRSyncTemporalResources (gone in 5.0), no r.TemporalAA.Algorithm, no
  r.PathTracing.Denoiser (4.27). Best guess: a 4.26-based branch. The cvar layout check and the
  View uniform buffer check in the probe settle what matters (vtable offsets, View offsets).

## The game's FSR 2

- AMD's FSR 2 UE plugin (`Engine/Plugins/Runtime/AMD/.../FSR2TemporalUpscaling`, modules
  FSR2TemporalUpscaling + FSR2TemporalUpscalingD3D12), statically linked FFX FSR 2 API
  (`ffxFsr2ContextCreate/Dispatch/GenerateReactiveMask`, `ffxFsr2GetInterfaceDX`).
- cvars: r.FidelityFX.FSR2.Enabled, QualityMode, Sharpness, AutoExposure, CreateReactiveMask,
  HistoryFormat, DeDither, AdjustMipBias, EnableWithIdealSettings, UseSSRExperimentalDenoiser,
  ForceVertexDeformationOutputsVelocity, ReactiveMask*, **UseNativeDX12**, UseNativeVulkan.
- With UseNativeDX12 (the plugin's default), FFX's own DX12 backend records FSR 2's passes on the
  game's command list: its own root signature, descriptor heap, and likely root constants for
  cbFSR2; its shaders are likely DXIL (no DXBC declarations). The probe looks for both root
  constants and root CBVs starting with render size + display size.
- Also the FSR 1 plugin (r.FidelityFX.FSR.RCAS.*).
- No DLSS, XeSS or Streamline in the game itself.

## Things that were in the game folder

- OptiScaler 0.9.4 (installed 2026-09-18 as dxgi.dll by an installer, with fakenvapi,
  dlssg_to_fsr3, XeSS, FFX DLLs, NVIDIA DLLs, OptiPatcher.asi, D3D12_Optiscaler). **Removed
  2026-11-20 on the user's request**: every file it listed in its rhi_install.txt, plus its
  `.original` markers (all 0 bytes: those files didn't exist before) and plugins/OptiPatcher.asi,
  moved to `E:\DeadIsland2DLSS\_optiscaler-backup-0.9.4` (gitignored). To restore, move them back.
- Kept: ReShade 6.8.0.2152 as dinput8.dll (add-on build), RenoDX `renodx-deadisland2.addon64`,
  `renodx-dlssfix.addon64` (2026-11-05; purpose unknown, maybe for OptiScaler's DLSS output:
  rename to .off if it gets in the way), reshade-shaders.

## ReShade loading (2026-11-20, first probe run)

The first run wrote nothing (no ReShade.log, no DeadIsland2DLSS.log): the exe doesn't import
dinput8.dll (imports include dxgi.dll, d3d11.dll, d3d9.dll; d3d12.dll delay-loaded), so ReShade
as dinput8.dll was never loaded. With OptiScaler it had been loaded through OptiScaler's dxgi.dll.
ReShade is now dxgi.dll (renamed, same file, 6.8.0.2152). renodx-dlssfix.addon64 was gone at the
second run (removed by the user).

## Plan

1. 0.0.1 probe (this): find FSR 2's passes (shader ids, cbFSR2 layout, dispatch sizes, inputs:
   colour, depth, motion vectors, exposure, reactive mask, output), the View uniform buffer
   offsets, and the cvar layout. Captures with F10.
2. Pick the approach:
   - A (CallistoDLSS): hold r.FidelityFX.FSR2.Enabled=0 so UE's TAA(U) runs, replace the TAA with
     DLSS/FSR. Proven code (Callisto, Stray), needs UE's TAA ids for this branch (capture with
     FSR 2 off) and the View offsets.
   - B (RE4DLSS): DLSS takes FSR 2's accumulate pass, inputs from FSR 2's own bindings. Needs
     FSR 2's descriptor tables readable (DXIL, own heap).
3. Then FSR 3.1/4, DLSS frame generation and FSR FG (CallistoDLSS's framegen.cpp, fsrfg.cpp,
   hdrswap.cpp for the game's HDR swapchain).

## Probe 0.0.1

Built from main.cpp (F10 capture, kTsrMapped=false: every compute pass written in full, root
constants now logged too, non-DXBC tables read guarded) and probe.cpp (read only). Nothing is set
or skipped. Writes DeadIsland2DLSS.log and DeadIsland2DLSS-frame-N.txt next to the exe.

### Run 2026-11-20 (4K HDR, FSR 2 Quality, then TAA), no crash, clean unload after 15,292 presents

cvars (layout check passed: UE 4.25-4.27 vtable layout, IsVariableBool slot 0x30, console manager
found 5/5): the game's menu sets r.FidelityFX.FSR2.Enabled 0/1 and QualityMode at SetByCode
(0x08) and EnableWithIdealSettings 1 with FSR 2; UseNativeDX12 1, Sharpness 0 (no RCAS pass),
CreateReactiveMask 1, AutoExposure 0, DeDither 2, AdjustMipBias 1; r.ScreenPercentage 100 (FSR 2
sets its own render size), r.DefaultFeature.AntiAliasing 2 (TAA), r.TemporalAA.Upscaler 1,
r.HDR.EnableHDROutput 1, OutputDevice 3, ColorGamut 2, back buffer 3840x2160 rgb10a2 (HDR10).
r.RayTracing 0.

FSR 2 = **FSR 2.1, FFX native DX12 backend**, 6 compute passes (DXIL), cbFSR2 as 37 **root
constants at root param 2** (148 bytes), plus 6 SPD constants at param 3 for the pyramid. FFX binds
each pass's descriptor tables (param 0 = UAV table, param 1 = SRV table) **before** its pipeline,
so in the capture the tables listed after a dispatch belong to the next pass. Order and slots
(capture 1, Quality 2560x1440 -> 3840x2160); slot orders match FSR 2.1's shader bindings:

| pass | id | dispatch | SRVs | UAVs |
|---|---|---|---|---|
| compute luminance pyramid | cs07e74148 | 40x23 | t0 input colour #87fce 2560x1440 rgba16f | u0 SPD counter 1x1 r32uint, mips r16f 1280x720 mips11, auto exposure 1x1 rg32f |
| prepare input colour | cs78b5ccfe | 320x180 | t0 input colour, t1 exposure (UE eye adaptation 4x1 rgba32f #63830) | u0 reconstructed prev depth r32uint, u1 prepared colour rgba16f, u2 luma history rgba8 |
| reconstruct + dilate | cs240d257d | 320x180 | **t0 motion #87cd1 2560x1440 rg16f**, **t1 depth #6217a 3840x2160 r32g8x24** (UE scene depth, view at top-left), **t2 reactive r8unorm #87c38**, **t3 transparency/composition r8unorm #87f35**, t4 prepared colour | u0 prev depth, u1 dilated motion rg16f, u2 dilated depth r32f, u3 dilated reactive rg8 |
| depth clip | cs089f5b7b | 320x180 | t0 prev depth, t1 dilated motion, t2 dilated depth, ... | u0 depth clip r8 |
| lock | cs3a8b05d2 | 320x180 | ... | u0 lock status 3840x2160 r11g11b10f |
| accumulate | csfbd2dd5b | 480x270 | t0 exposure, t1 auto exposure, t2 dilated motion, t3 history rgba16f, t4 lock, t5 depth clip, t6 prepared colour, t7 luma history, ... | u0 internal history 3840x2160 rgba16f, u1 lock, **u2 output #733ce 3840x2160 rgba16f** (srv -> uav before the lock pass, uav -> srv after; post-processing reads it) |

Before FSR 2, the plugin's own passes (DXBC, UE style: pipeline, then tables, then View at root
CBV param 3): csc7e3dbe7 converts UE velocity (t1 #628a6 3840x2160 rg16unorm) + depth (t0) into
FSR's motion #87cd1 (u0, render size rg16f; 480x270 dispatch); another pass writes the render-size
input colour #87fce (uav -> srv before the pyramid).

cbFSR2 (FSR 2.1 layout; values at Quality): 0 iRenderSize 2560 1440; 8 iDisplaySize 3840 2160;
16 uLumaMipDimensions 80 45; 24 uLumaMipLevelToUse 4; 28 **uFrameIndex** (0, 1, ...); 32
fDisplaySizeRcp; 40 **fJitter in render pixels** (0.25 -0.389, then -0.375 -0.056); 48
fDeviceToViewDepth (1.19e-7, -1, 0.1, 1.19e-7: reverse-Z, infinite far, near 0.1 cm-units?); 64..
(1 1 1 1); 80 1/render size; 96 fDownscaleFactor 0.667 0.667; 104 0.1209 (pre-exposure, likely),
108 1; 120 fJitterSequenceLength 18; 128 1/19; 140 1/1024 (luma mip rcp); 144 (0, then 0.1209:
previous pre-exposure?).

UE's own TAA (capture 2, FSR 2 off, 100%): **cs3e816dfc**, DXBC, UE 4.26 shape: t0 eye adaptation
4x1 rgba32f, t1 scene colour 3840x2160 rgba16f, t2 depth r32g8x24, t3 velocity rg16unorm, t4
stencil (fmt65), t5 depth again, t6 history rgba16f; u0 output rgba16f; root CBV param 3 its own
parameters (16 KB buffer with a ~1.8 KB repeating struct, not the View). The View uniform buffer
wasn't identified: the probe's View heuristic matched other buffers (its "UE 4.25 layout does NOT
hold" lines are not meaningful). Needed only for frame generation / Ray Reconstruction later.

### Decision: approach B (RE4DLSS style)

DLSS (and FSR 3.1/4) takes the accumulate dispatch csfbd2dd5b: colour #87fce (pyramid t0), depth
(reconstruct t1, 3840x2160 texture, render-size subrect), motion (reconstruct t0, UV units:
DLSS scale = render size, sign to confirm), jitter cbFSR2 byte 40, reset when uFrameIndex drops,
exposure = pre-exposure (byte 104) or UE's eye adaptation texture, reactive masks for FSR 3.1/
DLSS bias; output into the accumulate's u2. The five FSR 2 passes before it are skipped. Render
size = the game's FSR 2 quality (2.1 has Quality/Balanced/Performance/Ultra Performance; DLAA would
need a cvar or a forced screen percentage). FSR 2's tables are read guarded at each FSR 2 dispatch
(tables bound before the pipeline: track them per command list, don't clear on pipeline bind).
Fallback: approach A (hold FSR2.Enabled=0, replace UE's TAA cs3e816dfc).
- Run 1 log and captures kept in _captures/run1 (gitignored).

## 0.1.0 (built and installed 2026-11-20, untested)

src/fsr2dlss.cpp: DLSS at FSR 2's accumulate. FSR 2 passes found by their cbFSR2 root constants
(param 2, 37 values, sizes plausible) and order (pyramid = the pass with 6 SPD constants at param
3; accumulate = the dispatch covering the display size in 8x8 tiles; index 2 = reconstruct);
shader ids only logged. Inputs: colour = pyramid t1-table t0; motion, depth, reactive =
reconstruct t0/t1/t2; output = accumulate u2. Jitter cbFSR2 byte 40 as is, motion scaled by
render size (FSR 2 UV units, assumed from the UE plugin), reset when uFrameIndex drops or sizes
change. Passes 0-4 skipped while DLSS ran at the last accumulate. Test switches in the overlay's
Advanced tab / ReShade.ini [DeadIsland2DLSS]: MotionUnits (0 UV, 1 pixels), FlipMotionX/Y,
FlipJitter, SkipFsr2Passes, ReactiveBias. F9 toggles. Installs nvngx_dlss.dll 310.9.16 next to the
exe. Own NGX project id. Not built: CallistoDLSS's upscaler.cpp, framegen.cpp, fsr*.cpp,
hdrswap.cpp (reference for later steps); probe.cpp (build.ps1 $Probe = $true).

Also in the game folder (not ours): PSOBoost.addon64 (2026-11-20 15:01, the PSOBoost project).

## 0.1.0 test (2026-11-20): WORKS

User: "everything looked good". Log: FSR 2 pass ids as in the capture; DLSS ran in Quality
2560x1440, Balanced 2259x1271 (colour texture 2260x1272), Performance 1920x1080 -> 3840x2160,
presets K/K/M, no "FSR 2 kept" diag lines. MotionUnits UV (x render size) and jitter as is are
right. The user's NVIDIA app DLSS override is on (NGX loads 310.9.16 from ProgramData).

## 0.2.0: DLSS modes incl. DLAA (2026-11-20, installed, untested)

FSR 2.1's ffxFsr2GetUpscaleRatioFromQualityMode is statically linked at exe RVA 0xd438c0 (build
25629748): `sub ecx,1/je` x3, `cmp ecx,1/je`, `xorps xmm0,xmm0/ret`, then `movss xmm0,[rip+x]/ret`
loading 3.0, 2.0, 1.7, 1.5. Called from 0xd43856 (GetRenderResolutionFromQualityMode) and the
plugin at 0xd492ea, 0xd520a9, 0xd520d9, 0xd53caa; no other code uses its 1.7 constant. The add-on
finds it at run time by that code + the four constants and, for a DLSS mode, overwrites its start
with `mov eax,<ratio>; movd xmm0,eax; ret` (DLAA 1.0, Quality 1.5, Balanced 1/0.58, Performance 2,
Ultra Performance 3); "Game's FSR 2 setting" (DlssMode=0, default) restores the original bytes,
also on unload. FSR 2's accumulate is now picked by order (sixth pass after the SPD pyramid) and
display-sized dispatch, since at native resolution passes 1-4 are display-sized too.
Unknown until tested: whether the plugin accepts ratio 1.0, and whether it notices a live change
(it may only recreate its FSR 2 context when its quality mode changes).

## 0.2.0 test (2026-11-20)

Quality/Balanced/Performance/Ultra Performance work and switch live (the plugin re-reads the ratio
every frame: new render size within ~20 ms): 2560x1440, 2228x1253, 1920x1080, 1280x720 -> 4K,
presets K/K/M/L. UE rounds the render size UP (ceil(3840/1.7241) = 2228, game's 1.7 gave 2259).
**DLAA at ratio exactly 1.0: the game stops running FSR 2 altogether** (no FSR 2 passes; overlay
"waiting for FSR 2"; changing the in-game quality didn't help). 0.2.1 (installed, untested): DLAA
uses ratio 1.0001 (ceil gives 3840x2160 still, but it counts as upscaling).

## 0.2.1 test and 0.2.2 (2026-11-20)

0.2.1 (ratio 1.0001): DLAA still no FSR 2. Disassembly (dumpbin /disasm on the exe): the plugin's
view extension at exe 0xd53bd0 takes the game's quality cvar clamped to 1..4, calls the ratio
function (0xd53caa) and gives UE a screen percentage driver with fraction 1/ratio (0xd53cdf);
0xd520a0 / 0xd520d0 are the upscaler's max (mode 1) and min (mode 4) fractions; 0xd492d0 the
fraction for a quality mode. With 1/1.0001 UE's ceil still gives 3840x2160 = the output, and UE
then takes its plain path without the temporal upscaler. 0.2.2 (installed, untested): DLAA ratio =
H / (H - 1.05) from the back buffer height (re-applied at present when it changes; 1.0005 before
the first present), i.e. 3839x2159 at 4K: one pixel short, so UE upscales and FSR 2 (DLSS) runs.

## 0.2.2 test: DLAA WORKS (2026-11-20)

User: "everything looked good". DLAA ratio 1.00049 (from the 2160 back buffer) -> FSR 2 render
3839x2159 -> 3840x2160, DLSS took it (colour/motion textures 3840x2160), switches live to Quality
and back. NGX had created it as "Quality mode" (dlss.cpp picked DLAA only at render >= output);
0.2.3 (installed, not retested, only the NGX quality value changes): within 2 px counts as DLAA.

## 0.3.0: FSR 3.1/4 option (2026-11-20, installed, untested)

Upscaler setting (ini Upscaler: 0 Auto = DLSS if NGX starts else FSR, 1 DLSS, 2 FSR) with
CallistoDLSS's fsr.cpp (FSR SDK 2.4.0 FidelityFX API; AMD's runtime picks FSR 4 on supported
Radeons, else FSR 3.1). Same inputs as DLSS plus the game's FSR 2 reactive mask (reconstruct t2)
and transparency/composition mask (t3, new fsr::Inputs::transparency). Jitter and motion scale as
for DLSS (as the game gave FSR 2). Camera: near 10 cm (UE default; not read from the game), FOV =
2*atan(cbFSR2 byte 108) when it reads like tan(FOV/2) (it read 1.0 in the 0.0.1 capture -> 90
degrees; unverified), else 1.05 rad. FsrSharpness 0..1 (default 0, like the game's FSR 2). The
Mode dropdown (render resolution) applies to both. Installs amd_fidelityfx_loader_dx12.dll and
amd_fidelityfx_upscaler_dx12.dll next to the exe.

## 0.3.0 test: FSR WORKS (2026-11-20)

User: "it looked great". Log: Auto picked DLSS; DLAA now created as NGX DLAA mode (0.2.3 fix
confirmed); switched to FSR: runtime offered FSR 3.1.6 and 2.3.4 on the RTX 5080, FSR 3.1.6 ran
at native AA (3839x2159), Quality, Balanced, Performance, switching live; back to DLSS cleanly.
FSR 4 (Radeon) untested.

## View uniform buffer and 0.4.0 frame generation (2026-11-20, installed, untested)

The View buffer is the root CBV at compute parameter 3 of UE's passes (the FSR 2 plugin's velocity
conversion csc7e3dbe7, the pass before FSR 2's pyramid; capture 1 +3055360) and it has **UE 4.25's
layout**: TranslatedWorldToView 192, ViewToTranslatedWorld 256, ViewToClip 448 (jitter row 480 =
TemporalAAJitter 2016), ViewToClipNoAA 512 (0.839 / 1.4917: horizontal FOV 100, vertical 67.7
degrees; near plane [14] = 10), ClipToPrevClip 1952, ViewRectMin 2064 = 0, ViewSizeAndInvSize 2080
= render size. (The probe's View heuristic had looked at the wrong buffers.) cbFSR2 byte 108 (=1)
is NOT tan(FOV/2): 0.3.0's FSR got 90 degrees; 0.4.0 takes near/FOV from the View.
UE's depth at FSR 2 time is in state 0xe0 (depth read + both shader-resource states), as
framegen.cpp's kDepthReadState expects.

0.4.0: CallistoDLSS's framegen.cpp (DLSS-G 2x/3x/4x/Dynamic through Streamline 2.14.16, own NGX
project id), fsrfg.cpp (FSR FG 2x), hdrswap.cpp (passes HDR10 through; only converts scRGB) built
in. fsr2dlss.cpp keeps the last compute root CBV at parameter 3 per command list, takes it at FSR
2's pyramid, reads 2112 bytes at the accumulate and checks it (ViewSize = render size, jitter rows
equal, projection sane); on success with frame generation active and the output = back buffer
size: copies FSR 2's motion vectors into our own texture (the plugin's may be reused before the
Present) and calls FrameGenSetCamera(view, 1952) + FrameGenTag(depth, our motion, render size,
jitter, reset, motion scale = render size: FSR 2's UV units). FrameGenTag/fsrfg got a motion scale
(Callisto's motion was in pixels). Installs streamline\ (sl.*, nvngx_dlssg, nvngx_dlss copy) and
amd_fidelityfx_framegeneration_dx12.dll. Frame generation choice needs a game restart (Streamline
or FSR's swapchain is set up at launch). Pre-HUD picture (hudless) off, as in Callisto.

## 0.4.0 test: DLSS FRAME GENERATION WORKS (2026-11-20)

User: "looks good". DLSS + DLSS-G 2x (FrameGenMode 1, tech DLSS): Streamline 2.14 started for the
game's swapchain (created through Streamline, HDR10 rgb10a2, colour space 3 after the HDR switch;
hdrswap passes it through), View check passed (projection 1.0 / 1.7778 this time = 90 degrees
horizontal, 58.7 vertical: the FOV differs from capture 1, read live), first frame tagged at
submission, 2 frames presented per game frame. Untagged presents only twice (one frame at
15:47:21, four in a row at 15:47:54: menu/loading without FSR 2) -> paused, resumed 1 s later.
Clean Streamline shutdown at exit. Not yet tried: 3x/4x/Dynamic, FSR FG 2x, FSR upscaling + FG.

## FSR FG test (2026-11-20) and 0.4.1

FSR FG 2x ran, but the HUD and subtitles jittered/smeared: no pre-HUD picture was ever found (no
"pre-HUD picture found" in the log), so FSR interpolated the finished frame. Capture 1's end of
frame (UE 4 HDR UI composite, r.HDR.UI.CompositeMode 1): tonemapper -> #ab3ee (rgb10a2 HDR10
scene); a draw writes it to the back buffer (= the scene without HUD); the HUD/subtitles are drawn
into the UI layer #c083f (3840x2160 bgra8 typeless, with depth); the game copies the back buffer
to #c0a0a with **CopyTextureRegion**; a final draw composites #c0a0a + UI layer into the back
buffer. framegen.cpp only watched copy_resource (Callisto's game used CopyResource). 0.4.1
(installed, untested): whole-texture copy_texture_region of the back buffer is handled like
copy_resource, so FSR FG gets the pre-HUD picture and the UI layer's opacity as its mask
(FsrHudMode composite). DLSS-G still gets no pre-HUD picture (FgHudless off, as in Callisto).

## 0.4.1 test and 0.4.2 (2026-11-23)

User: HUD/subtitle problems with both DLSS-G and FSR FG; ReShade screenshot in the exe folder. The
screenshot can't show generated frames (ReShade captures the game's own frames, before frame
generation); the doubled outlines in it (objective icons) are in rendered frames, likely the HUD's
own style. Log: pre-HUD picture found (CopyTextureRegion hook works), HUD layer alpha found
inverted (UE HDR UI layer: alpha = 1 - coverage) -> mask flipped; FSR FG used "composite (HUD
mask)" all along, "picture before the HUD" only while paused in menus. DLSS-G never got a pre-HUD
picture (FgHudless off by default, Callisto's choice). 0.4.2 (installed, untested): DLSS-G HUD
protection on by default (ini FgHudless, checkbox in the overlay): kBufferTypeHUDLessColor + UI
alpha mask. FSR FG's HUD issue still unexplained: next step is the user's description / comparing
FSR's HUD protection modes.

## 0.4.2 test and 0.4.3 (2026-11-23)

User: DLSS-G HUD "looks great" with HUD protection (pre-HUD picture + UI mask). FSR FG: the game
smooth, the HUD not: with "Composite" the HUD comes from the rendered frames only, so its moving
parts (compass) step at the base frame rate. "Picture before the HUD" made the compass, text and
subtitles smooth in motion (the game's pre-HUD copy is clean here, unlike Reunion's). 0.4.3: FSR
FG HUD protection default "Picture before the HUD" (FsrHudMode 1), help text updated. Frame
generation now confirmed for DLSS-G 2x and FSR FG 2x; 3x/4x/Dynamic untried.

## DLSS-G 3x / 4x / Dynamic test (2026-11-23): WORK

Log: 2x -> 3x ("3 frames presented per game frame") -> 4x (4) -> Dynamic (Streamline: "Dynamic MFG
is supported", interpolation enabled in eDynamic mode; our state counter read 1 right after the
switch, as in Callisto), all switched live. Pre-HUD picture + UI mask given to DLSS-G. Only two
4-frame gaps without inputs (menu/loading), paused and resumed about 1 s later. Frame generation is
now confirmed in every mode except FSR 4 / FSR FG 4 (Radeon only).

## 1.0.0 packaged (2026-11-23, not released)

Testing switches out of the overlay (ini keys still read: MotionUnits, FlipMotionX/Y, FlipJitter,
SkipFsr2Passes, ReactiveBias); F10 capture only with ini CaptureHotkey=1 (button always). README,
THIRD_PARTY_NOTICES (no Ray Reconstruction DLL shipped: the game has no ray tracing), release notes
docs/release-notes-1.0.0.md. build.ps1 -Package: bin/release/DeadIsland2DLSS-1.0.0.zip, 122,441,760
bytes, 19 entries, forward-slash names checked raw (add-on, nvngx_dlss 310.9.16, AMD loader 2.3.0 /
upscaler 4.1.2 / framegeneration 4.0.2, streamline/ with sl.* 2.14.16 + nvngx_dlssg + nvngx_dlss
copy, README, LICENSE (GreatDanish), THIRD_PARTY_NOTICES, licenses/). Installed in the game too.

## 1.0.0 RELEASED (2026-11-23)

User checked the 1.0.0 overlay ("looks good") and approved publishing. Private repo
https://github.com/GreatDanish1026/DeadIsland2DLSS (master), tag v1.0.0 at a2e9fb8, release
https://github.com/GreatDanish1026/DeadIsland2DLSS/releases/tag/v1.0.0 with DeadIsland2DLSS-1.0.0.zip
(122,441,760 bytes, upload size verified), notes docs/release-notes-1.0.0.md.
