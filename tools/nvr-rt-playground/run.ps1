# The RT playground's one command (tools/nvr-rt-playground/README.md): build (helper + playground + fork + shaders into bin\), then
# open the window. No game launch.
#   powershell -ExecutionPolicy Bypass -File tools\nvr-rt-playground\run.ps1 [-Size 640x360] [-Window 1280x720] [-Device Radeon]
#                                                                           [-Demo 60] [-Fork <d3d9.dll>] [-Compile] [-NoBuild] [-SkipHelper]
#   -Size     the internal render size (the trace, the G-buffer, the denoiser); the picture is scaled up to -Window
#   -Device   part of the GPU's name for DXVK (DXVK_FILTER_DEVICE_NAME): default empty = the first GPU with ray queries (2026-11-07);
#             (a PC with a GTX 10xx + an iGPU: the iGPU; a tester's RTX card: the RTX)
#   -Demo     the scripted minute (walks, toggles, saves labelled screenshots + per-segment fps into out\) run in that many seconds
#   -FixedStep  the demo's script seconds a frame (e.g. 0.2): every run sees the same frames (A/B comparisons); 0 = real time
#   -Set      toggles held through the demo, by their key: -Set R=0,B=1 (PTRestirDI off, 1 bounce); -Tag: a suffix for its screenshots
param([string]$Size = "640x360", [string]$Window = "1280x720", [string]$Device = "", [double]$Demo = 0, [string]$Fork = "",
	[switch]$Compile, [switch]$NoBuild, [switch]$SkipHelper, [string[]]$Set = @(), [string]$Tag = "", [double]$FixedStep = 0, [string]$Frames = "", [string]$FramesRange = "")
$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
$bin = Join-Path $here "bin"
$out = Join-Path $here "out"
if (-not $NoBuild) {
	$bargs = @{ Fork = $Fork }
	if ($Compile) { $bargs.Compile = $true }
	if ($SkipHelper) { $bargs.SkipHelper = $true }
	& "$here\build.ps1" @bargs
	if (-not $?) { Write-Host "RESULT: FAIL (build)"; exit 1 }
}
New-Item -ItemType Directory -Force $out | Out-Null
$env:DXVK_LOG_LEVEL = "warn"
$env:DXVK_LOG_PATH = $out
$a = @("--helper", "$bin\NvrRtHelper.exe", "--spv", "$bin\shaders\Rt", "--compute", "$bin\shaders\Compute", "--out", $out, "--size", $Size, "--window", $Window)
if ($Device) { $a += @("--device", $Device) }
if ($Demo -gt 0) { $a += @("--demo", "$Demo") }
foreach ($s in $Set) { foreach ($one in $s.Split(",")) { if ($one) { $a += @("--set", $one.Trim()) } } }
if ($Tag) { $a += @("--tag", $Tag) }
if ($FixedStep -gt 0) { $a += @("--fixed-step", "$FixedStep") }
if ($FramesRange) { $a += @("--frames-range", $FramesRange) }   # (2026-11-09) only the demo's script seconds a,b (e.g. 30,40: segment 05)
if ($Frames) { $a += @("--frames", $Frames) }   # (2026-11-09) every frame's picture, raw RGB at the window size, for a film (see the README)
& "$bin\NvrRtPlayground.exe" @a
exit $LASTEXITCODE
