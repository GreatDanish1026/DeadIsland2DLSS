# DeadIsland2DLSS 1.0.0

ReShade add-on for **Dead Island 2** (Unreal Engine 4, DirectX 12) that replaces the game's own
**FSR 2** with **NVIDIA DLSS** or **AMD FSR 3.1 / 4**, adds **DLAA** and the full range of render
resolutions, and adds **DLSS Frame Generation** or **FSR Frame Generation**, in SDR and in HDR. The
game itself only offers FSR 2.1.

Needs ReShade 6.8 or later with add-on support, and **FSR 2 chosen in the game's graphics settings**
(the add-on takes over the game's FSR 2).

| | Graphics card |
|---|---|
| DLSS upscaling and DLAA | NVIDIA RTX |
| FSR upscaling | any DirectX 12 card: FSR 4 on Radeon cards that support it, FSR 3.1 elsewhere |
| DLSS Frame Generation | NVIDIA RTX 40 or 50 (3x, 4x and Dynamic: RTX 50), hardware-accelerated GPU scheduling |
| FSR Frame Generation | any DirectX 12 card: FSR 4 on Radeon RX 9000 under Windows 11, FSR 3.1 elsewhere |

Upscaler and frame generation can be mixed (for example DLSS upscaling with FSR Frame Generation on
an RTX 30 card).

## What it does

- **Upscaler** (F9 to switch on and off): Auto (DLSS on NVIDIA RTX, FSR elsewhere), DLSS or FSR in
  place of the game's FSR 2. It takes FSR 2's inputs (colour, depth, motion vectors and the game's
  reactive and transparency masks) and writes FSR 2's output; FSR 2's own passes are skipped while
  it runs. DLSS Super Resolution 310.9.16; FSR through AMD's FSR SDK 2.4.0.
- **Mode** (the render resolution): the game's FSR 2 setting, or DLAA / native AA, Quality (67%),
  Balanced (58%), Performance (50%) or Ultra Performance (33%), whatever FSR 2 quality the game is
  set to. Switches right away. DLAA renders one pixel short of the screen (3839x2159 at 4K), since
  the game turns its upscaler off at exactly 100%; you won't see the difference.
- **DLSS model**: Automatic (default: K, M at Performance, L at Ultra Performance), or K, J, L or M
  at every resolution, or NVIDIA's default. **FSR sharpness**: 0-1 (default 0, as the game's FSR 2).
- **Frame generation**: Off, DLSS 2x / 3x / 4x / Dynamic (NVIDIA Streamline 2.14.16, with Reflex),
  or FSR 2x. Runs in game, not in menus or loading screens. Switching it on, or between DLSS and
  FSR, takes a game restart; DLSS's multipliers switch right away.
- **HUD protection** for frame generation (HDR): the game's picture from before the HUD (and, for
  DLSS, the HUD's opacity) is given to frame generation, so the compass, objective text and
  subtitles stay sharp and move smoothly in generated frames. On by default; can be changed live.

HDR tone mapping isn't part of this add-on; the RenoDX Dead Island 2 add-on works alongside it.

The overlay (ReShade's Add-ons tab) has **Settings**, **Status** and **Advanced** (frame capture)
tabs. Settings are saved in `ReShade.ini` under `[DeadIsland2DLSS]`.

## Install

1. Install ReShade 6.8 or later with full add-on support for `DeadIsland-Win64-Shipping.exe`, in
   `Dead Island 2\DeadIsland\Binaries\Win64`, as **DirectX 12**, so that ReShade's file is
   **`dxgi.dll`**. (The game doesn't load `dinput8.dll`, so ReShade installed under that name never
   starts.)
2. Remove OptiScaler if it's installed there (its `dxgi.dll`, `OptiScaler.ini`, its `nvngx_*.dll`
   files and the rest): it replaces the same FSR 2 this add-on replaces.
3. Copy everything from the release zip into that folder: `DeadIsland2DLSS.addon64`,
   `nvngx_dlss.dll`, the three `amd_fidelityfx_*_dx12.dll` files and the `streamline` folder (which
   also carries a copy of `nvngx_dlss.dll`: with frame generation on, NVIDIA's runtime looks for it
   there).
4. In the game's graphics settings choose **FSR 2** (any quality). Open ReShade (Home key), Add-ons
   tab, DeadIsland2DLSS: pick the upscaler, a mode and, if you like, frame generation (restart after
   picking frame generation).
5. A DLSS model override for the game in the NVIDIA App takes precedence over DLSS model.

Frame counters that hook the game's own presents (RTSS, for one) may not count generated frames;
the Steam or NVIDIA overlay does.

The zip includes NVIDIA's DLSS 310.9.16 and Streamline 2.14.16 runtime files, signed by NVIDIA (the
add-on checks Streamline's signature before loading it), and AMD's FSR SDK 2.4.0 runtime files,
signed by AMD. Their licences are in `licenses`, and attribution is in `THIRD_PARTY_NOTICES.txt`.
To uninstall, delete those files. Logs: `DeadIsland2DLSS.log` next to the game's exe, Streamline's
in `streamline\sl.log`.

Tested with the Steam version (build 25629748) at 3840x2160 on an NVIDIA RTX 5080, HDR via RenoDX:
DLSS in every mode including DLAA, FSR 3.1 upscaling, DLSS Frame Generation 2x, 3x, 4x and Dynamic,
FSR Frame Generation 2x. FSR 4 (Radeon) isn't tested.

## Build

`.\build.ps1` (Visual Studio 2022 C++ tools, Windows SDK for `fxc`). The runtimes aren't in the
repository: unzip `bin\x64` of NVIDIA's Streamline 2.14.16 release into
`third_party\streamline\bin\x64`, and copy `amd_fidelityfx_loader_dx12.dll`,
`amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` from
`Kits\FidelityFX\signedbin` of AMD's FSR SDK 2.4.0 into `third_party\fidelityfx\bin`.
`.\build.ps1 -Install` copies the add-on and the runtimes into the game folder (game closed);
`.\build.ps1 -Package` makes the release zip in `bin\release`.

## How it works

Forked from CallistoDLSS (The Callisto Protocol, which ships the same AMD FSR 2 Unreal plugin).
The game's FSR 2.1 runs through AMD's native DirectX 12 backend: six compute passes recorded on the
game's command list, with their constants as root constants. `src/fsr2dlss.cpp` recognises them by
those constants and their order, gathers FSR 2's inputs from its own descriptor tables, and runs
DLSS or FSR 3.1/4 at FSR 2's last pass. The render resolution comes from overriding the ratio FSR
2.1's `ffxFsr2GetUpscaleRatioFromQualityMode` returns (found in the exe by its code). Frame
generation (CallistoDLSS's `src/framegen.cpp`, `src/fsrfg.cpp`) gets the camera from Unreal's View
uniform buffer (UE 4.25 layout). `docs/FINDINGS.md` has the details and the test results. The frame
capture (Advanced tab; F10 with `CaptureHotkey=1` in `ReShade.ini`) writes two frames of GPU
commands to `DeadIsland2DLSS-frame-N.txt`, for finding FSR 2's passes again after a game update.

By GreatDanish. MIT licence (`LICENSE`).
