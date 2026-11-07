# The RT test host's one command (tools/nvr-rt-host/README.md): build the helper + host, check the constant-block layouts, run every
# scene; PASS / FAIL per scene. Exit code 0 only when everything passed. No game launch.
#   powershell -ExecutionPolicy Bypass -File tools\nvr-rt-host\run.ps1 [-Compile] [-Spv <folder>] [-Scene furnace,mirror] [-Fork <d3d9.dll>] [-NoBuild]
#   -Compile   run the scenes on .spv compiled now from the .comp sources (bin\spv) instead of the committed .spv
#   -Spv       run them on the .spv files of this folder (a changed shader under test; missing names fall back to nothing: give all four)
param([switch]$Compile, [string]$Spv = "", [string[]]$Scene = @(), [string]$Fork = "", [switch]$NoBuild, [string]$Device = "Radeon")
$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
$repo = (Resolve-Path "$here\..\..").Path
$bin = Join-Path $here "bin"
$out = Join-Path $here "out"
$start = Get-Date

if (-not $NoBuild) {
	$bargs = @{ Fork = $Fork }
	if ($Compile) { $bargs.Compile = $true }
	& "$here\build.ps1" @bargs
	if (-not $?) { Write-Host "RESULT: FAIL (build)"; exit 1 }
}

Write-Host "`n== constant-block layouts (game C++ structs vs shader blocks)"
$py = Get-Command py -ErrorAction SilentlyContinue
if ($py) { & py -3 "$here\rt_layout_check.py" $repo } else { & python "$here\rt_layout_check.py" $repo }
$layoutOk = $LASTEXITCODE -eq 0

if (-not $Spv) { $Spv = if ($Compile) { Join-Path $bin "spv\Rt" } else { Join-Path $repo "src\hlsl\NewVegas\Compute\Rt" } }
$computeDir = if ($Compile) { Join-Path $bin "spv" } else { Join-Path $repo "src\hlsl\NewVegas\Compute" }
$includesDir = Join-Path $repo "src\hlsl\NewVegas\Shaders\Includes"
Write-Host "`n== scenes (shaders: $Spv)"
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -Filter *.pfm -ErrorAction SilentlyContinue | Remove-Item -Force
$env:DXVK_LOG_LEVEL = "warn"
$env:DXVK_LOG_PATH = $out
$hostArgs = @("--helper", "$bin\NvrRtHelper.exe", "--spv", $Spv, "--compute", $computeDir, "--includes", $includesDir, "--out", $out, "--device", $Device, "--keep-going")
foreach ($s in $Scene) { foreach ($one in $s.Split(",")) { if ($one) { $hostArgs += @("--scene", $one.Trim()) } } }
& "$bin\NvrRtHost.exe" @hostArgs
$scenesOk = $LASTEXITCODE -eq 0

$secs = [math]::Round(((Get-Date) - $start).TotalSeconds, 1)
Write-Host ("`nlayout {0}, scenes {1}; {2} s in all; report + failed images in {3}" -f ($(if ($layoutOk) { "PASS" } else { "FAIL" })), ($(if ($scenesOk) { "PASS" } else { "FAIL" })), $secs, $out)
if ($layoutOk -and $scenesOk) { Write-Host "RT HOST: PASS"; exit 0 } else { Write-Host "RT HOST: FAIL"; exit 1 }
