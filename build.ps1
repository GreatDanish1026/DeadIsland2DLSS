# Builds bin\DeadIsland2DLSS.addon64 with MSVC (Visual Studio 2022+ with C++ tools).
# MSVC is required: ReShade is built with it, and MinGW returns API structs differently.
# Usage: .\build.ps1 [-Install] [-Package] [-GameBin <folder of DeadIsland-Win64-Shipping.exe>]
#   -Install  copies the add-on, nvngx_dlss.dll, AMD's FSR runtime and the Streamline runtime (in a
#             streamline folder) next to the game's exe (game closed).
#   -Package  makes bin\release\DeadIsland2DLSS-<version>.zip: the add-on and the runtimes laid out as
#             in the game folder, README, LICENSE, THIRD_PARTY_NOTICES and the licences.
#
# Sources built: main.cpp (overlay, frame capture), fsr2dlss.cpp (DLSS / FSR in place of the game's
# FSR 2), dlss.cpp, fsr.cpp, framegen.cpp (DLSS-G), fsrfg.cpp (FSR FG), hdrswap.cpp. upscaler.cpp
# (CallistoDLSS's TAA replacement) isn't built. $Probe = $true builds the 0.0.x probe instead.
#
# The NVIDIA runtime comes from third_party\streamline\bin\x64: bin\x64 of NVIDIA's Streamline
# SDK release 2.14.16 (github.com/NVIDIA-RTX/Streamline/releases), which also carries
# nvngx_dlss.dll 310.9.16. AMD's FSR runtime
# (amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll,
# amd_fidelityfx_framegeneration_dx12.dll) comes from Kits\FidelityFX\signedbin of AMD's FSR SDK
# 2.4.0, copied to third_party\fidelityfx\bin. Those DLLs aren't in this repository.
param(
    [switch]$Install,
    [switch]$Package,
    [string]$GameBin = "E:\SteamLibrary\steamapps\common\Dead Island 2\DeadIsland\Binaries\Win64"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = "DeadIsland-Win64-Shipping.exe"
$Probe = $false  # $true builds the 0.0.x probe (main.cpp + probe.cpp, read only)

# The user's PATH has a Steam DSX entry with "(x86)" that breaks vcvars64.bat under cmd /c.
$env:PATH = (($env:PATH -split ';') | Where-Object { $_ -and $_ -notmatch 'DSX' }) -join ';'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars64.bat runs vswhere by name
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$obj = "$root\bin\obj"
New-Item -ItemType Directory -Force $obj | Out-Null
$addon = "$root\bin\DeadIsland2DLSS.addon64"

# ReShade add-on headers in third_party\reshade match ReShade 6.8.0.
$include = "/I `"$obj`" /I `"$root\third_party\imgui`" /I`"$root\third_party\reshade\include`" "
if ($Probe) {
    $sources = "`"$root\src\main.cpp`" `"$root\third_party\reshade\utils\descriptor_tracking.cpp`" `"$root\src\probe.cpp`""
    $cmd = @(
        "call `"$vcvars`" >nul",
        ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DRD_PROBE " + $include +
         "/Fo`"$obj\\`" $sources user32.lib advapi32.lib " +
         "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\DeadIsland2DLSS.pdb`" /DEBUG")
    ) -join " && "
} else {
    # DLSS or FSR 3.1/4 in place of the game's FSR 2 (fsr2dlss.cpp + CallistoDLSS's dlss.cpp and fsr.cpp),
    # and frame generation (CallistoDLSS's framegen.cpp: DLSS-G through Streamline, fsrfg.cpp: FSR's;
    # hdrswap.cpp only acts on an scRGB swapchain, Dead Island 2's is HDR10).
    $sources = "`"$root\src\main.cpp`" `"$root\third_party\reshade\utils\descriptor_tracking.cpp`" `"$root\src\fsr2dlss.cpp`" `"$root\src\dlss.cpp`" `"$root\src\fsr.cpp`" " +
               "`"$root\src\framegen.cpp`" `"$root\src\fsrfg.cpp`" `"$root\src\hdrswap.cpp`""
    $cmd = @(
        "call `"$vcvars`" >nul",
        "fxc /nologo /T cs_5_1 /E main /O3 /Vn kUiAlphaCs /Fh `"$obj\ui_alpha_cs.h`" `"$root\src\ui_alpha.hlsl`" >nul",
        "fxc /nologo /T cs_5_1 /E main /O3 /Vn kHudComposeCs /Fh `"$obj\hud_compose_cs.h`" `"$root\src\hud_compose.hlsl`" >nul",
        "fxc /nologo /T vs_5_1 /E vs /O3 /Vn kHdrConvertVs /Fh `"$obj\hdr_convert_vs.h`" `"$root\src\hdr_convert.hlsl`" >nul",
        "fxc /nologo /T ps_5_1 /E ps /O3 /Vn kHdrConvertPs /Fh `"$obj\hdr_convert_ps.h`" `"$root\src\hdr_convert.hlsl`" >nul",
        ("cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " + $include +
         "/I `"$root\third_party\dlss\include`" /I `"$root\third_party\streamline\include`" " +
         "/I `"$root\third_party\fidelityfx\api\include`" /I `"$root\third_party\fidelityfx\upscalers\include`" " +
         "/I `"$root\third_party\fidelityfx\framegeneration\include`" " +
         "/Fo`"$obj\\`" $sources user32.lib advapi32.lib ole32.lib d3d12.lib dxgi.lib `"$root\third_party\dlss\lib\nvsdk_ngx_s.lib`" " +
         "/link /DLL /OUT:`"$addon`" /PDB:`"$obj\DeadIsland2DLSS.pdb`" /DEBUG")
    ) -join " && "
}
cmd /c $cmd
if ($LASTEXITCODE) { throw "build failed" }
Write-Host "Built $addon"

$sl = "$root\third_party\streamline\bin\x64"
$ffx = "$root\third_party\fidelityfx\bin"
$slFiles = "sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll", "nvngx_dlss.dll"
$ffxFiles = "amd_fidelityfx_loader_dx12.dll", "amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll"

# Lays out the add-on and the NVIDIA and AMD runtimes as they go in the game's Win64 folder:
# nvngx_dlss.dll and AMD's signed FSR runtime (FSR SDK 2.4.0 Kits\FidelityFX\signedbin) next to the
# exe, NVIDIA's Streamline (DLSS Frame Generation) in Win64\streamline. Streamline starts NGX with
# that folder as its feature path, so DLSS goes there too (CallistoDLSS 2026-11-12).
function Copy-Runtime([string]$dest) {
    Copy-Item $addon $dest -Force
    if ($Probe) { return }
    foreach ($f in $slFiles) { if (-not (Test-Path "$sl\$f")) { throw "$f not found in $sl (Streamline 2.14.16 release)" } }
    foreach ($f in $ffxFiles) { if (-not (Test-Path "$ffx\$f")) { throw "$f not found in $ffx (FSR SDK 2.4.0 signedbin)" } }
    Copy-Item "$sl\nvngx_dlss.dll" $dest -Force
    foreach ($f in $ffxFiles) { Copy-Item "$ffx\$f" $dest -Force }
    New-Item -ItemType Directory -Force "$dest\streamline" | Out-Null
    foreach ($f in $slFiles) { Copy-Item "$sl\$f" "$dest\streamline" -Force }
}

if ($Install) {
    if (-not (Test-Path "$GameBin\$exe")) { throw "$exe not found in $GameBin (pass -GameBin)" }
    if (Get-Process ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue) { throw "Close the game first." }
    Copy-Runtime $GameBin
    if (-not $Probe) {
        Copy-Item "$root\third_party\streamline\license.txt" "$GameBin\streamline\streamline.license.txt" -Force
        Copy-Item "$root\third_party\streamline\licenses\reflex.license.txt" "$GameBin\streamline" -Force
    }
    Write-Host "Installed $addon to $GameBin"
}

if ($Package) {
    if ($Probe) { throw "No package for the probe." }
    $version = [regex]::Match((Get-Content "$root\src\version.h" -Raw), 'RD_VERSION "([^"]+)"').Groups[1].Value
    $name = "DeadIsland2DLSS-$version"
    $stage = "$root\bin\release\$name"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force "$stage\licenses" | Out-Null
    # Everything at the top level (and the streamline folder) goes next to the game's shipping exe.
    Copy-Runtime $stage
    Copy-Item "$root\README.md", "$root\LICENSE", "$root\THIRD_PARTY_NOTICES.txt" $stage
    Copy-Item "$root\third_party\streamline\license.txt" "$stage\licenses\Streamline-LICENSE.txt"
    Copy-Item "$root\third_party\streamline\licenses\nvngx_dlss.license.txt" "$stage\licenses\NVIDIA-DLSS-LICENSE.txt"
    Copy-Item "$root\third_party\streamline\licenses\reflex.license.txt" "$stage\licenses\NVIDIA-Reflex-LICENSE.txt"
    Copy-Item "$root\third_party\fidelityfx\license.md" "$stage\licenses\AMD-FSR-LICENSE.txt"
    $zip = "$root\bin\release\$name.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    # Forward-slash entry names (Compress-Archive in PowerShell 5.1 writes backslashes, which Linux
    # unzip tools extract as file names: streamline\x.dll; CallistoDLSS 1.0.0-1.0.4).
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        Get-ChildItem $stage -Recurse -File | ForEach-Object {
            $entry = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally { $archive.Dispose() }
    Write-Host "Packaged $zip ($((Get-Item $zip).Length) bytes)"
    Get-ChildItem $stage -Recurse -File | ForEach-Object {
        "{0,-45} {1,-16} {2,12:N0}" -f $_.FullName.Substring($stage.Length + 1), $_.VersionInfo.FileVersion, $_.Length
    }
}
