Vegas Shaders RT test room (tester kit, 2026-11-06)

What it is
- VS-AlphaLampTest.esp: one small interior, cell VSAlphaLampTest (vanilla FalloutNV.esm assets + the mirror below). A chain-link fence
  with a steady lamp behind it (RT lamp shadows through cut-outs), an opaque wooden fence beside it, a polished marble disc on the
  floor, and a mirror wall (five flat mirror panels) by the south wall with its own lamp (RT reflections of you, third and first person).
- Meshes\VegasShaders\Test\vs_mirror01.nif: the mirror: the vanilla Ultra-Luxe wall screen (nv_ul_wallscreen01.nif) with its screen's
  alpha test switched off and its texture swapped for the plain one below (one flat quad in the gold frame).
- Textures\VegasShaders\Test\vs_mirror01_d.dds (4 x 4 near-black) + vs_mirror01_rmaos.dds (4 x 4: roughness 0.02, metal 1): only
  the reflection shows; RoughnessOverrides "vs_mirror01:0.02" + Mirror 1 with MirrorSmoothOnly make it a mirror and nothing else.
- VegasShaders\Presets\VSAlphaLampTest.ini: the test's settings (RT lamp shadows, RT reflections, Mirror 1 on the mirror panels only (MirrorSmoothOnly), the overrides). Only those
  keys change. Apply it from the preset browser, then restart the game once (the RT switches need a restart).

Install: copy the Data folder's contents into the game's Data folder (or install the folder as a mod in MO2) and enable
VS-AlphaLampTest.esp. In game: coc VSAlphaLampTest, then player.moveto VSAlphaLampCam01 (the fence's shadow on the floor),
VSAlphaLampCam02 (the room and the marble), VSAlphaLampCam03 (in front of the mirror). F switches to third person.
Needs: the Vegas Shaders DXVK build with the RT helper on an RT-capable GPU (RTX, RX 6000+, Arc, Radeon 680M-780M).

Sources (not in this repo): the ESP is built by work\tools\nvr-testcell\build_alphalamp.py; the mirror model is patched from the
vanilla mesh by a small script (alpha flags 0x12EC -> 0x10EC on block 16, block 15's first texture path swapped, its size fixed).
Packaging: the test release copies resource\testkit\Data as "Optional - RT test room".
