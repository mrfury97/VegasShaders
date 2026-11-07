# Builds the RT test host (tools/nvr-rt-host/README.md): NvrRtHelper.exe (x64, MSBuild) and NvrRtHost.exe (x86, cl), and puts the
# NVR DXVK fork's d3d9.dll next to the host. Output: tools\nvr-rt-host\bin\ (git-ignored).
#   -Fork <d3d9.dll>    the fork to test with (default: the bench copy fork-r13, or $env:NVR_RT_HOST_FORK)
#   -Compile            also compile the trace shaders the scenes use from src\hlsl\NewVegas\Compute\Rt\*.comp into bin\spv
#                       (default: the scenes run the committed .spv files, which is what the game ships)
param([string]$Fork = "", [switch]$Compile, [switch]$SkipHelper)
$ErrorActionPreference = "Continue"
$here = $PSScriptRoot
$repo = (Resolve-Path "$here\..\..").Path
$bin = Join-Path $here "bin"
New-Item -ItemType Directory -Force $bin | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw "no Visual Studio with the C++ tools (vswhere)" }

# 1. the helper (x64), the project's own command
if (-not $SkipHelper) {
	$msbuild = Join-Path $vs "MSBuild\Current\Bin\MSBuild.exe"
	& $msbuild "$repo\tools\nvr-rt-helper\NvrRtHelper.vcxproj" /p:Configuration=Release /p:Platform=x64 /v:m /nologo
	if ($LASTEXITCODE -ne 0) { throw "NvrRtHelper build failed" }
	Copy-Item "$repo\tools\nvr-rt-helper\bin\NvrRtHelper.exe" $bin -Force
}

# 2. the host (x86: the game's side is 32-bit, as the fork's d3d9.dll)
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvarsall.bat"
$src = Join-Path $here "rt_host.cpp"
$cmd = "set `"PATH=%PATH%;${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer`" && `"$vcvars`" x86 >nul && cl /nologo /O2 /EHsc /std:c++17 /MT /W3 /DUNICODE_OFF /I`"$repo\src\core`" /I`"$repo\tools\nvr-rt-helper`" `"$src`" /Fo`"$bin\\`" /Fe`"$bin\NvrRtHost.exe`" /link d3d9.lib user32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "NvrRtHost build failed" }

# 3. the fork's d3d9.dll next to the host
if (-not $Fork) { $Fork = $env:NVR_RT_HOST_FORK }
# (2026-11-06) or tools\nvr-rt-host\fork-path.txt (one line, not in git): this machine's fork d3d9.dll
if (-not $Fork) { $f = Join-Path $PSScriptRoot "fork-path.txt"; if (Test-Path $f) { $Fork = (Get-Content $f -TotalCount 1).Trim() } }
if (-not (Test-Path $Fork)) { throw "no fork d3d9.dll at $Fork (pass -Fork)" }
Copy-Item $Fork (Join-Path $bin "d3d9.dll") -Force

# 4. optional: the scenes' shaders from source
if ($Compile) {
	$glslang = Get-ChildItem "C:\VulkanSDK\*\Bin\glslangValidator.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
	if (-not $glslang) { throw "no glslangValidator (Vulkan SDK)" }
	$rt = Join-Path $repo "src\hlsl\NewVegas\Compute\Rt"
	$spv = Join-Path $bin "spv\Rt"
	New-Item -ItemType Directory -Force $spv | Out-Null
	# trace shaders (+ the -D variants the scenes use, as tools/nvr-dxvk/compile-compute.sh builds them)
	$jobs = @(@("RtPathTrace", "RtPathTrace", ""), @("RtPathTraceGrid", "RtPathTrace", "-DPT_LIGHT_GRID"), @("RtPathTraceGI", "RtPathTrace", "-DPT_RESTIR_GI"), @("RtPathTraceCache", "RtPathTraceCache", ""),
		@("RtRestirGI", "RtRestirGI", ""), @("RtLampTrace", "RtLampTrace", ""), @("RtLampTraceMany", "RtLampTraceMany", ""), @("RtLampTraceRestir", "RtLampTraceRestir", ""),
		@("ReflectionsRT", "ReflectionsRT", ""), @("RtSunShadow", "RtSunShadow", ""), @("RtAOTrace", "RtAOTrace", ""),
		# (2026-11-07) the RT shadow denoiser's scenes (sigmalamps, sigmasun): the trace variant, the old filter, the module's stages
		@("RtLampTraceSigma", "RtLampTrace", "-DSIGMA"), @("RtLampFilter", "RtLampFilter", ""), @("RtShadowDenoise", "RtShadowDenoise", ""),
		@("RtShadowDenoiseTemporalSun", "RtShadowDenoiseTemporal", "-DSUN"), @("RtShadowDenoiseTemporalLamps", "RtShadowDenoiseTemporal", "-DLAMPS"),
		@("RtShadowDenoiseBlur", "RtShadowDenoiseBlur", ""), @("RtShadowDenoisePostBlur", "RtShadowDenoiseBlur", "-DPOST"),
		@("RtShadowDenoiseStabilize", "RtShadowDenoiseStabilize", ""), @("RtShadowDenoisePackSun", "RtShadowDenoisePack", "-DSUN"),
		@("RtShadowDenoisePackLamps", "RtShadowDenoisePack", "-DLAMPS"), @("RtLampPrimary", "RtLampPrimary", ""))
	foreach ($j in $jobs) {
		$a = @("-V", "--target-env", "vulkan1.2", "-I$rt", "-o", "$spv\$($j[0]).spv")
		if ($j[2]) { $a += $j[2] }
		$a += "$rt\$($j[1]).comp"
		& $glslang.FullName @a | Out-Null
		if ($LASTEXITCODE -ne 0) { & $glslang.FullName @a; throw "compile $($j[0]) failed" }
	}
	# the denoiser's fork passes (D3D9 side)
	$cs = Join-Path $repo "src\hlsl\NewVegas\Compute"
	# (2026-11-07) + the RT reflection denoiser's passes (refldenoise: core/RtReflDenoise.h)
	foreach ($name in "RtDenoiseTemporal", "RtDenoiseAtrous", "RtReflDenoiseReproject", "RtReflDenoiseAverage", "RtReflDenoisePrefilter", "RtReflDenoiseTemporal",
		"RtReflDenoiseOursReproject", "RtReflDenoiseOursBlur", "RtReflDenoiseOursTemporal") {
		& $glslang.FullName -V --target-env vulkan1.1 "-I$cs" -o "$bin\spv\$name.spv" "$cs\$name.frag" | Out-Null
		if ($LASTEXITCODE -ne 0) { throw "compile $name failed" }
	}
}
Write-Host "built: $bin\NvrRtHost.exe, NvrRtHelper.exe, d3d9.dll (fork: $Fork)"
