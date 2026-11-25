# Upstream PRs cherry-picked into this branch

This branch pulls in fixes from a handful of PRs opened against `master` by
`mirjomi` (VHeyes), independently of the feature work done directly on this
branch. Recorded here so the provenance survives a squash/rebase and so
whoever merges this branch to `master` knows to close the originals as
superseded rather than merge them again — they'd otherwise land a second
time under different commit hashes.

All are based on the same merge-base as this branch, `747976001523f1ef484cfaea0909dd4a38d67e9c`
("Merge pull request #45 from macrimmon12-tech/revert-point-light-attenuation-fix").

## PR #46 — Fix the MaterialPass depth pull, which shipped ten times too small

https://github.com/macrimmon12-tech/TESReloaded10/pull/46

Cherry-picked whole (1 commit, no changes needed — applied cleanly):

| Upstream commit | This branch |
|---|---|
| `c91a51aef68989daed0c14b174ed3538f1f71d4d` — "Fix the MaterialPass depth pull, which shipped ten times too small" | `5a86ff21abc1cf3cc67ff1155160cee2afed976d` |

Retunes the flashlight relighting pass's vertex depth-pull constant
(`0.99999` → `0.9997`) after measuring on a runtime ladder that the flicker
it exists to prevent was still present at the previous value.

## PR #48 — Fix two SMAA bugs that made it under-blend in every edge mode

https://github.com/macrimmon12-tech/TESReloaded10/pull/48

Cherry-picked whole (2 commits, no changes needed — applied cleanly):

| Upstream commit | This branch |
|---|---|
| `f62470d9ff7af057aca7cd6f232369aa87aade08` — "Fix two SMAA bugs that both read as the effect being too weak" | `f32b6c9f1eb64315b0f0c77655768af7a20f5294` |
| `8370291b79efa69de53f5f67da3596b3df35a7d6` — "Give SMAA its tuning controls, a combined edge mode, and honest defaults" | `9998d7efc54a41875eff5461cd842c62cb22e17c` |

Fixes an `SMAA_RT_METRICS` z/w (width/height) swap that silently weakened
blending in every edge mode, and a depth-edge threshold that was being
compared against the far plane instead of relative depth. Adds a combined
luma+depth edge mode (now the default) and exposes several previously
hardcoded `SMAA_PRESET_ULTRA` values as tunable settings.

## PR #49 — Fix DitherBuster: it could not detect dither, and its edge pass could not reach a neighbouring pixel

https://github.com/macrimmon12-tech/TESReloaded10/pull/49

Cherry-picked whole (2 commits, no changes needed — applied cleanly):

| Upstream commit | This branch |
|---|---|
| `108d5b777fc960b5381ce0e06d441466339c2991` — "Let DitherBuster see the dither it is named for" | `e93c5bd2e45bd9fe5a99bbc28affb4d18d8d1a4b` |
| `5d7bab8610a6b915a9c82ea96f7115d3fa6e78b2` — "Make DitherBuster's directional pass actually reach a neighbouring pixel" | `33d0217a67400d6121404de799443c94a480eb26` |

Fixes the edge detector being mathematically blind to the exact one-pixel
checkerboard pattern DXVK's alpha-test dither produces (central differences
cancel on it), a units bug that left the directional smoothing pass barely
reaching any neighboring pixel, and a `float4`→`half` truncation that made
"luma" read the red channel alone. Effect ships disabled by default, same
as upstream.

## PR #47 — Stabilise exterior sun shadows so the sun can move continuously

https://github.com/macrimmon12-tech/TESReloaded10/pull/47

**Evaluated, not cherry-picked.** Deep-dived twice (see session history) —
unlike the three above, this one does not apply cleanly: this branch's own
forward-shadows work (`2081e7f` "Fix forward sun shadows leaking into
interiors", and the terrain-skylighting/bullet-hole commits before it)
diverged from master's shadow implementation, producing real conflicts in
`ShadowsExterior.cpp/.h`, `SunShadows.fx.hlsl` (4 regions), and
`ShadowMap.pso.hlsl` that need hand-resolution, not a straight merge — plus
a texel-scaled bias fix that upstream only applied to the deferred shadow
path, which would need porting into the forward path (`Shadow.hlsl`)
ourselves to avoid the two paths disagreeing.

The PR also grew mid-review to add a second, largely independent feature
(tracking up to 32 moving actors to stop their shadows trailing under the
temporal reprojection filter), which adds its own conflict surface and a
new file (`ShadowManager.h`). Parked as a deliberate decision, not an
oversight — worth revisiting as its own pass rather than folded into this
branch's PR.
