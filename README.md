# DeadIsland2DLSS

A ReShade add-on that is to add NVIDIA DLSS (and later FSR 3.1/4 and frame generation) to
Dead Island 2 in place of the game's own FSR 2.

**Status: 0.0.1 probe.** It only watches the game and writes a log and frame captures; it changes
nothing. Findings are in [docs/FINDINGS.md](docs/FINDINGS.md).

Forked from CallistoDLSS. By GreatDanish. MIT licence (see LICENSE); third-party notices in
THIRD_PARTY_NOTICES.txt.

## Build

`.\build.ps1` (Visual Studio 2022+ with the C++ x64 tools), `.\build.ps1 -Install` to copy the
add-on next to `DeadIsland-Win64-Shipping.exe` (game closed). Needs ReShade 6.8 with add-on support.
