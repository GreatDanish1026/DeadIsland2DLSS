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

## Probe 0.0.1 (built and installed 2026-11-20, not run yet)

Built from main.cpp (F10 capture, kTsrMapped=false: every compute pass written in full, root
constants now logged too, non-DXBC tables read guarded) and probe.cpp (read only). Nothing is set
or skipped. Writes DeadIsland2DLSS.log and DeadIsland2DLSS-frame-N.txt next to the exe.
