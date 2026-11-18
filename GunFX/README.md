# GunFX assets

GunFX used to be a plugin of its own (`GunFX.dll`); since 2026-11-16 it is built into Vegas Shaders: the source is
in [`src/gunfx/`](../src/gunfx/) and its settings are in Vegas Shaders' own settings (`[Main.GunFX.*]`). The player
guide is [`docs/GUNFX.md`](../docs/GUNFX.md); the move is described in
[`docs/next/GUNFX-BUILTIN.md`](../docs/next/GUNFX-BUILTIN.md).

What is left here:

- `assets/Meshes/GunFX/`: the baked smoke effects, shipped as `Data\Meshes\GunFX\`. They are vanilla
  `effects\fxlightsmoke.nif` rebuilt with different numbers. GunFX only sets their emitter rate at run time
  (the puff and port bursts start when spawned and stop after `BurstSeconds`); it does not rewrite particle modifiers.

The old `GunFX.vcxproj` and `config/GunFX.ini.defaults` were removed (git history has them; `src/gunfx/GunFXKeys.h`
keeps every key with its shipped default).
