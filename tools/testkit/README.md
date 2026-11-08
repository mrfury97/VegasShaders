# Test kit builders

Python 3 scripts (no packages) that write the tester kit in `resource/testkit/Data`: the test plugins and the few models and textures of
our own that they use. They read the game's `FalloutNV.esm` (set `FNV_ESM` to its full path; read only) to find vanilla base forms by
editor ID; no vanilla record is overridden and no vanilla file is copied.

| script | writes |
|---|---|
| `build_rtroom.py --out <dir>` | `<dir>\Data\VS-RTRoom.esp` (cell VSRTRoom) + `Meshes\VegasShaders\Test\vs_sphere_*01.nif, vs_glass01.nif, vs_glowpanel01.nif, vs_windowwall01.nif` + their textures under `Textures\VegasShaders\Test\`, and `<dir>\rtroom-views.json` (the camera marks) |
| `build_alphalamp.py --out <dir>` | `VS-AlphaLampTest.esp` (cell VSAlphaLampTest; uses the kit's `vs_mirror01.nif`, which is a patched vanilla model and is not made here) |
| `build_testcell.py --out <dir>` | `NVR-TestCell.esp` (the bench's test cells; the others reuse its writer) |
| `verify_kit.py <Data dir>` | checks a kit: each `.esp` with `verify_esp.py`, each `.nif` read back block by block (sizes, strings, geometry, triangle facing), each `.dds` size |

`vs_nif.py` writes the models (FNV static `.nif` 20.2.0.7 / user 11 / Bethesda 34: BSFadeNode, NiTriShape, BSShaderPPLightingProperty,
BSShaderTextureSet, NiMaterialProperty, NiAlphaProperty; no collision) and uncompressed DDS textures. `esm.py` reads the master,
`tes4.py` writes records.

To rebuild the RT room into the kit:

```
set FNV_ESM=<game>\Data\FalloutNV.esm
py -3 -I tools\testkit\build_rtroom.py --out %TEMP%\rtroom
py -3 -I tools\testkit\verify_kit.py %TEMP%\rtroom\Data
```

then copy `%TEMP%\rtroom\Data\*` over `resource\testkit\Data`. The output is the same bytes on every run.
