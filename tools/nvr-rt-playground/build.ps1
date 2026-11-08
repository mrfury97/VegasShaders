# Builds the RT playground (tools/nvr-rt-playground/README.md) into tools\nvr-rt-playground\bin\ (git-ignored), a folder that runs on
# its own: NvrRtPlayground.exe (x86), NvrRtHelper.exe (x64, the project's MSBuild), the NVR DXVK fork's d3d9.dll, the shaders it runs
# (shaders\Rt: the trace passes; shaders\Compute: the fork's denoiser + show passes) and run-playground.cmd.
#   -Fork <d3d9.dll>   the fork (default: $env:NVR_RT_HOST_FORK, then tools\nvr-rt-host\fork-path.txt)
#   -Compile           compile those shaders from src\hlsl\NewVegas\Compute now (glslangValidator) instead of copying the committed .spv
#   -SkipHelper        do not rebuild NvrRtHelper.exe (copy tools\nvr-rt-helper\bin\NvrRtHelper.exe as it is)
#   -Scratch <folder>  the compilers' intermediates + TEMP (default: $env:NVR_BUILD_SCRATCH\playground when set, else obj\ here)
param([string]$Fork = "", [switch]$Compile, [switch]$SkipHelper, [string]$Scratch = "")
$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
$repo = (Resolve-Path "$here\..\..").Path
$bin = Join-Path $here "bin"
New-Item -ItemType Directory -Force $bin, "$bin\shaders\Rt", "$bin\shaders\Compute" | Out-Null
if (-not $Scratch) { $Scratch = if ($env:NVR_BUILD_SCRATCH) { Join-Path $env:NVR_BUILD_SCRATCH "playground" } else { Join-Path $here "obj" } }
New-Item -ItemType Directory -Force "$Scratch\helper\bin", "$Scratch\helper\obj", "$Scratch\playground", "$Scratch\tmp" | Out-Null
$env:TEMP = "$Scratch\tmp"; $env:TMP = "$Scratch\tmp"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw "no Visual Studio with the C++ tools (vswhere)" }

# 1. the helper (x64), the project's own command
if (-not $SkipHelper) {
	$msbuild = Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe"
	& $msbuild "$repo\tools\nvr-rt-helper\NvrRtHelper.vcxproj" /p:Configuration=Release /p:Platform=x64 /v:m /nologo "/p:OutDir=$Scratch\helper\bin\" "/p:IntDir=$Scratch\helper\obj\"
	if ($LASTEXITCODE -ne 0) { throw "NvrRtHelper build failed" }
	Copy-Item "$Scratch\helper\bin\NvrRtHelper.exe" $bin -Force
} elseif (-not (Test-Path "$bin\NvrRtHelper.exe")) {
	Copy-Item "$repo\tools\nvr-rt-helper\bin\NvrRtHelper.exe" $bin -Force
}

# 2. the playground (x86: the game's side is 32-bit, as the fork's d3d9.dll)
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvarsall.bat"
$src = Join-Path $here "playground.cpp"
$cmd = "set `"PATH=%PATH%;${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer`" && `"$vcvars`" x86 >nul && cl /nologo /O2 /EHsc /std:c++17 /MT /W3 /I`"$repo\src\core`" /I`"$repo\tools\nvr-rt-helper`" /I`"$here`" `"$src`" /Fo`"$Scratch\playground\\`" /Fe`"$bin\NvrRtPlayground.exe`" /link /SUBSYSTEM:CONSOLE d3d9.lib d3dcompiler.lib user32.lib gdi32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "NvrRtPlayground build failed" }

# 3. the fork's d3d9.dll next to it
if (-not $Fork) { $Fork = $env:NVR_RT_HOST_FORK }
if (-not $Fork) { $f = Join-Path $repo "tools\nvr-rt-host\fork-path.txt"; if (Test-Path $f) { $Fork = (Get-Content $f -TotalCount 1).Trim() } }
if (-not (Test-Path $Fork)) { throw "no fork d3d9.dll at $Fork (pass -Fork)" }
Copy-Item $Fork (Join-Path $bin "d3d9.dll") -Force

# 4. the shaders: the trace passes (helper) + the fork's passes (D3D9 side), copied or compiled
$rt = Join-Path $repo "src\hlsl\NewVegas\Compute\Rt"
$cs = Join-Path $repo "src\hlsl\NewVegas\Compute"
$traceShaders = @("RtPathTrace", "RtPathTraceCache", "RtSunShadow", "RtEmissiveBuild")
$forkShaders = @("RtDenoiseTemporalRgb", "RtDenoiseTemporalSpec", "RtDenoiseFirefly", "RtDenoiseAtrousRgb", "RtPathTraceShow")
if ($Compile) {
	$glslang = Get-ChildItem "C:\VulkanSDK\*\Bin\glslangValidator.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
	if (-not $glslang) { throw "no glslangValidator (Vulkan SDK)" }
	foreach ($n in $traceShaders) {
		& $glslang.FullName -V --target-env vulkan1.2 "-I$rt" -o "$bin\shaders\Rt\$n.spv" "$rt\$n.comp" | Out-Null
		if ($LASTEXITCODE -ne 0) { & $glslang.FullName -V --target-env vulkan1.2 "-I$rt" -o "$bin\shaders\Rt\$n.spv" "$rt\$n.comp"; throw "compile $n failed" }
	}
	foreach ($n in $forkShaders) {
		& $glslang.FullName -V --target-env vulkan1.1 "-I$cs" -o "$bin\shaders\Compute\$n.spv" "$cs\$n.frag" | Out-Null
		if ($LASTEXITCODE -ne 0) { throw "compile $n failed" }
	}
} else {
	foreach ($n in $traceShaders) { Copy-Item "$rt\$n.spv" "$bin\shaders\Rt\" -Force }
	foreach ($n in $forkShaders) { Copy-Item "$cs\$n.spv" "$bin\shaders\Compute\" -Force }
}

# 5. a double-click launcher for the folder (the device filter: this PC's Radeon iGPU runs ray queries; a tester's RTX: remove it)
$cmdFile = @"
@echo off
rem The RT playground: double-click, or run from a console to see its log. Options: --size 640x360 (internal) --window 1280x720
rem --device <part of the GPU's name> (DXVK_FILTER_DEVICE_NAME; empty = DXVK's choice) --demo 60 (a scripted minute + screenshots).
cd /d "%~dp0"
set DXVK_LOG_LEVEL=warn
set DXVK_LOG_PATH=%~dp0out
if not exist out mkdir out
NvrRtPlayground.exe --helper NvrRtHelper.exe --spv shaders\Rt --compute shaders\Compute --out out %*
"@
Set-Content -Path (Join-Path $bin "run-playground.cmd") -Value $cmdFile -Encoding ascii
Write-Host "built: $bin\NvrRtPlayground.exe, NvrRtHelper.exe, d3d9.dll (fork: $Fork), shaders\, run-playground.cmd"
