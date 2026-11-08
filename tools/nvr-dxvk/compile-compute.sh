#!/usr/bin/env bash
# compile-compute.sh [frag name ...]  (UNOFFICIAL 2026-10-26): the Vulkan pass shaders in src/hlsl/NewVegas/Compute as SPIR-V.
# Each <name>.frag/.comp -> <name>.spv (glslangValidator, Vulkan 1.1, as before), and each line of Compute/variants.txt ->
# <frag>-<suffix>.spv with its -D defines, run through spirv-opt -O (dead paths of the fixed switches removed; exact: no
# floating-point reassociation). Every result is checked with spirv-val. Names given: only those (and their variants).
# Needs glslangValidator, spirv-opt and spirv-val on PATH (Linux/WSL: apt install glslang-tools spirv-tools).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../src/hlsl/NewVegas/Compute"
want() { [ $# -eq 0 ] && return 0; local n; for n in "${NAMES[@]}"; do [ "$n" = "$1" ] && return 0; done; return 1; }
NAMES=("$@")
n=0
for f in *.frag *.comp; do
  [ -e "$f" ] || continue
  b="${f%.*}"
  [ ${#NAMES[@]} -eq 0 ] || want "$b" || continue
  # (glslang prints its errors on stdout: shown, and the run stops, when a shader does not compile)
  # UNOFFICIAL 2026-10-28 (fork RT): ray-query shaders need SPIR-V 1.5 (Vulkan 1.2); the others stay Vulkan 1.1 (same bytes as before).
  env=vulkan1.1
  grep -q "GL_EXT_ray_query" "$f" && env=vulkan1.2
  if ! log=$(glslangValidator -V --target-env $env -o "$b.spv" "$f" 2>&1); then echo "$log" >&2; echo "FAILED: $f" >&2; exit 1; fi
  spirv-val --target-env $env "$b.spv"
  n=$((n+1))
done
# UNOFFICIAL 2026-10-28 (RT): Rt/*.comp are the RT helper's passes (NvrRtHelper.exe, 64-bit; GL_EXT_ray_query needs SPIR-V 1.4+): Vulkan 1.2.
for f in Rt/*.comp; do
  [ -e "$f" ] || continue
  b="${f%.*}"
  [ ${#NAMES[@]} -eq 0 ] || want "$(basename "$b")" || continue
  if ! log=$(glslangValidator -V --target-env vulkan1.2 -o "$b.spv" "$f" 2>&1); then echo "$log" >&2; echo "FAILED: $f" >&2; exit 1; fi
  spirv-val --target-env vulkan1.2 "$b.spv"
  n=$((n+1))
done
# UNOFFICIAL 2026-11-02: the RT helper's passes built from another pass's source with a define (no .comp of their own; each source's
# header names its variants; 2026-11-04 RtPathTraceGrid: PTLightGrid). Checked 2026-11-02: all four rebuild byte for byte with the Vulkan SDK 1.3.296.0 glslangValidator.
while read -r out src defs; do
  [ ${#NAMES[@]} -eq 0 ] || want "$out" || want "$src" || continue
  # shellcheck disable=SC2086
  if ! log=$(glslangValidator -V --target-env vulkan1.2 $defs -o "Rt/$out.spv" "Rt/$src.comp" 2>&1); then echo "$log" >&2; echo "FAILED: Rt/$out" >&2; exit 1; fi
  spirv-val --target-env vulkan1.2 "Rt/$out.spv"
  n=$((n+1))
done <<'RT_VARIANTS'
RtGITrace RtAOTrace -DRT_GI_PASS
RtRadianceCache RtAOTrace -DRT_CACHE_PASS
RtLampTraceSplit RtLampTrace -DSPLIT
RtLampTraceExterior RtLampTrace -DEXTERIOR
RtLampTraceSigma RtLampTrace -DSIGMA
RtPathTraceGrid RtPathTrace -DPT_LIGHT_GRID
RtPathTraceGI RtPathTrace -DPT_RESTIR_GI
RT_VARIANTS
v=0
while read -r frag suffix defs; do
  case "$frag" in ""|\#*) continue ;; esac
  [ ${#NAMES[@]} -eq 0 ] || want "$frag" || continue
  out="$frag-$suffix.spv"
  # shellcheck disable=SC2086
  if ! log=$(glslangValidator -V --target-env vulkan1.1 $defs -o "$out.tmp" "$frag.frag" 2>&1); then echo "$log" >&2; echo "FAILED: $frag $suffix" >&2; exit 1; fi
  spirv-opt -O --target-env=vulkan1.1 "$out.tmp" -o "$out"
  rm -f "$out.tmp"
  spirv-val --target-env vulkan1.1 "$out"
  v=$((v+1))
done < <(tr -d '\r' < variants.txt)
echo "compiled $n shaders, $v variants"
