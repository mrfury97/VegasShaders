Vegas Shaders RT test rooms (tester kit, 2026-11-06, RT room added 2026-11-07)

Two plugins, each one cell. Install: copy the Data folder's contents into the game's Data folder (or install the folder as a mod in
MO2) and enable the plugin(s). Needs: the Vegas Shaders DXVK build with the RT helper on an RT-capable GPU (RTX, RX 6000+, Arc,
Radeon 680M-780M). F switches to third person (to see yourself in the mirrors). The test objects have no collision (you walk through
the spheres, the panels and the glass).

1) VS-RTRoom.esp: the RT playground room, in game (cell VSRTRoom)
- An office room (8 x 6 tiles): the west part roofed, the east part open to the sky. The cell is flagged "behave like exterior", so
  the game gives it the weather's sky and sun (clear-weather climate).
- South wall: three window openings. Glass pane in the west one, the chain-link fence inside the middle one, a fern by the east one.
  A big warm "sun lamp" stands outside the windows: Vegas Shaders treats "behave like exterior" cells as interiors (no sun shadows,
  no RT sun there), so this lamp throws the window, fence and fern shadows into the room (interior lamp shadows / RT lamp shadows).
- West wall: a mirror wall (five vs_mirror01 panels) with chrome, gold and red spheres in front of it and a small warm lamp.
- North wall: a glowing panel (glow map: PTGlowLights / glow textures). A pillar, a warm ceiling lamp.
- Open part: a plant in the sun and a BLUE lamp.
- Camera marks: player.moveto VSRTRoomCam01 (the whole room), Cam02 (the window wall), Cam03 (the mirror and spheres), Cam04 (the
  glowing panel), Cam05 (the open part and the sky). coc VSRTRoom works too.
- VegasShaders\Presets\VSRTRoom.ini: the room's settings (RT lamp shadows, RT reflections, Mirror 1 on the mirror panels only
  (MirrorSmoothOnly), roughness overrides for the mirror, spheres and glass; the path tracer is left as you have it: turn PTRealtime on in the
  Ray tracing page to compare). With the Preset Manager on it applies by itself in the cell; otherwise apply it from the preset
  browser, then restart the game once (the RT switches need a restart).
- Our own files for it (made by tools\testkit\build_rtroom.py, no vanilla file copied):
  Meshes\VegasShaders\Test\vs_sphere_chrome01 / gold01 / red01.nif (smooth spheres, radius 64), vs_glass01.nif (a two-sided pane,
  alpha blended: the material rules' "glass"), vs_glowpanel01.nif (240 x 120, glow map in slot 2), vs_windowwall01.nif (an office
  wall with an opening, the vanilla office wall texture by path);
  Textures\VegasShaders\Test\vs_sphere_*_d.dds + _rmaos.dds (chrome: roughness 0.05 metal 1; gold: 0.2 metal 1; red: 0.5 metal 0),
  vs_glass01_d.dds (pale green, 22 % opaque) + _rmaos.dds, vs_glowpanel01_d.dds + _g.dds.

2) VS-AlphaLampTest.esp: RT lamp shadows through cut-outs (cell VSAlphaLampTest)
- A chain-link fence with a steady lamp behind it, an opaque wooden fence beside it, a polished marble disc on the floor, and a
  mirror wall (five flat mirror panels) by the south wall with its own lamp (RT reflections of you, third and first person).
- coc VSAlphaLampTest, then player.moveto VSAlphaLampCam01 (the fence's shadow on the floor), VSAlphaLampCam02 (the room and the
  marble), VSAlphaLampCam03 (in front of the mirror).
- VegasShaders\Presets\VSAlphaLampTest.ini: its settings (as above, for this cell).

The mirror (both rooms)
- Meshes\VegasShaders\Test\vs_mirror01.nif: the vanilla Ultra-Luxe wall screen (nv_ul_wallscreen01.nif) with its screen's alpha
  test switched off and its texture swapped for the plain one below (one flat quad in the gold frame).
- Textures\VegasShaders\Test\vs_mirror01_d.dds (4 x 4 near-black) + vs_mirror01_rmaos.dds (4 x 4: roughness 0.02, metal 1): only
  the reflection shows; RoughnessOverrides "vs_mirror01:0.02" + Mirror 1 with MirrorSmoothOnly make it a mirror and nothing else.

Sources: tools\testkit (build_rtroom.py, build_alphalamp.py, their shared writers, verify_kit.py; README.md there says how to
rebuild). The mirror model is patched from the vanilla mesh by a small script (alpha flags 0x12EC -> 0x10EC on block 16, block 15's
first texture path swapped, its size fixed); that script is not kept.
Packaging: the test release copies resource\testkit\Data as "Optional - RT test room".
