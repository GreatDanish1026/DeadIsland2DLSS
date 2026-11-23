DeadIsland2DLSS 1.0.0: NVIDIA DLSS and AMD FSR 3.1/4 in place of Dead Island 2's FSR 2, with DLAA and
DLSS or FSR Frame Generation. A ReShade add-on.

- **Upscaler** (F9): Auto (DLSS on NVIDIA RTX, FSR elsewhere), DLSS 310.9.16 or FSR 3.1 / 4 (AMD FSR
  SDK 2.4.0), taking over the game's FSR 2 with FSR 2's own inputs and masks.
- **Mode**: DLAA / native AA, Quality, Balanced, Performance, Ultra Performance, or the game's FSR 2
  setting. Switches live.
- **DLSS model** (Automatic, K, J, L, M) and **FSR sharpness**.
- **Frame generation**: DLSS 2x / 3x / 4x / Dynamic (Streamline 2.14.16, Reflex) or FSR 2x, with HUD
  protection that keeps the compass, objective text and subtitles sharp and smooth (HDR).

**Install**: ReShade 6.8+ with add-on support as **`dxgi.dll`** (the game doesn't load
`dinput8.dll`), remove OptiScaler, copy the zip's contents into
`Dead Island 2\DeadIsland\Binaries\Win64`, choose **FSR 2** in the game's graphics settings, then set
things up in ReShade's Add-ons tab. Details in the README.

Tested with the Steam version at 4K on an RTX 5080 (HDR via RenoDX): DLSS in every mode, FSR 3.1,
DLSS Frame Generation 2x/3x/4x/Dynamic, FSR Frame Generation 2x. FSR 4 (Radeon) is untested.
