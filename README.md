# Vegas Shaders

**A graphics overhaul for Fallout: New Vegas:** lighting, shadows, materials, sky, weather and post-processing, drawn by
an xNVSE plugin into the game's own renderer. Every effect has its own switch and sliders in an in-game menu.

> [!NOTE]
> **Early days.** Vegas Shaders 1.0.0 is the first release under this name. It grew out of an unofficial optimized fork
> of New Vegas Reloaded and has since been rewritten as its own project (see **Credits** below). It has been tested
> on a small number of PCs; reports of what works and what breaks are welcome in this repository's issues.

## What it does

- **Lighting:** physically based lighting on objects, terrain and skin; ambient light and sky light that follow the
  scene; bounce light; lamp-by-lamp shadows indoors; contact-hardening sun shadows; cloud and mountain shadows.
- **Materials:** metal, wetness and optional per-texture material maps (`_rmaos`, the Community Shaders True PBR layout,
  including the specular level).
- **Atmosphere:** volumetric fog and clouds, god rays, volumetric light, a physical sky, rain and snow on surfaces.
- **Post-processing:** tonemapping, bloom, depth of field, colour grading with LUTs (including `.cube` files), lens
  effects, TAA, SMAA, and FSR 3.1 or DLSS upscaling (the upscalers need the DXVK build below).
- **Tools:** a performance meter (F10), a per-effect log, and a settings menu with presets, skins and search.

## Requirements

- Fallout: New Vegas (1.4.0.525) with [xNVSE](https://github.com/xNVSE/NVSE).
- Recommended: [JIP LN NVSE](https://github.com/jazzisparis/JIP-LN-NVSE).
- Optional: the matching DXVK build (`d3d9.dll`, from the Releases page) for the Vulkan versions of the effects and for
  FSR / DLSS upscaling. Without it, every effect runs on the game's own Direct3D 9.

## Install

1. With the game closed, download the latest zip from **Releases** and extract it into your `Data` folder, or install
   it with your mod manager.
2. Start the game and press <kbd>O</kbd> to open the menu.

| What | Where (in `Data`) |
|---|---|
| plugin | `NVSE\Plugins\VegasShaders.dll` |
| your settings | `NVSE\Plugins\VegasShaders.toml` (created on the first Save) |
| defaults, Max Quality, material table | `NVSE\Plugins\VegasShaders.defaults.toml`, `.maxquality.toml`, `.materials.txt` |
| shaders | `Shaders\VegasShaders\` |
| LUTs | `Textures\VegasShaders\LUTs\` |
| presets, variants, keywords, menu skins | `VegasShaders\` |
| logs (in the game folder) | `VegasShaders-Logs\VegasShaders_<date>.log` |

**Coming from New Vegas Reloaded or the unofficial optimized build:** remove `NVSE\Plugins\NewVegasReloaded.dll`
(only one of the two plugins may load). Your old settings, presets, LUTs and shader folder are read as they are while
the new-named ones are missing: the first Save writes `VegasShaders.toml` and the old files are never changed. Mods
that look for the plugin still find it (its xNVSE name stays `NewVegasReloaded`, and the console commands keep the
`NVR` prefix), and old setting names and presets keep working.

If an effect looks wrong after an update, delete the `Cache` folders inside `Data\Shaders\VegasShaders\` and check
the newest log for lines containing `error X`.

## Using it

- <kbd>O</kbd> opens the menu: File (Save, Revert), View (text size, skin), Tools (presets, lighting, dev tools),
  Help (About and credits). Changes apply at once; **Save** keeps them.
- **Menu skins:** View > Skin. Built in: Default, Pip-Boy, Vault-Tec, Amber Terminal, NCR, Legion, Brotherhood,
  Strip Neon, Midnight, High Contrast and Light. View > Skin > Own colours changes any single part (background, panel,
  field, accent, text, heading, border, slider, selection, rounding, opacity, background picture) on top of whichever skin
  is picked; the same `Custom...` keys are on the Main.Menu.Style page. Export writes the current look to
  `Data\VegasShaders\Skins\` as a small file you can edit (colours, rounding, an optional background picture).
- **F10** starts and stops the performance meter; the cost of each effect goes to the log.
- **Bug reports:** attach the log of the session (`VegasShaders-Logs` in the game folder).

## Building from source

Requirements: Visual Studio 2022 (or Build Tools) with the v143 C++ toolset for x86. The D3DX package is restored by
NuGet (`Microsoft.DXSDK.D3DX`). After cloning, fetch the submodules:

```shell
git submodule update --init --recursive
```

Then build `NewVegasReloaded/NewVegasReloaded.vcxproj` (Release, Win32), or run `build.bat "NewVegasReloaded"`. The
plugin comes out as `VegasShaders.dll`. Shaders are compiled by the game at runtime from `src/hlsl/NewVegas`, which
maps to `Data\Shaders\VegasShaders\`. Each effect's code, settings defaults and notes are in `src/effects/<Name>/`
(index: `src/effects/INDEX.md`). The generated textures can be rebuilt with `tools/gen_textures.py`.

## Credits

Vegas Shaders is based on **New Vegas Reloaded / TESReloaded** by Alessio Tamburini (Alenet), maintained by Llde,
with the New Vegas Reloaded team and contributors (Timeslip, scanti, ShadeMe, Ethatron, GBR, mcfurston,
noonemusteverknow, Mathieu U and others, see [License.md](License.md)). It is an independent project: it is not made,
endorsed or supported by the New Vegas Reloaded authors, so please don't ask for help with it in their channels.

With work from:
- [Conor (mrfury97)](https://github.com/mrfury97): terrain shadow sweep, terrain variation, many lamps, skin work, Pip-Boy
  fit, the game MSAA block.
- [PixelsSquared](https://github.com/PixelsSquared): the Perceptual tone mapper, eye adaptation, cloud moonlight and sky light.
- macintroll: snow accumulation and the snow look, captured normals, lens and underwater shaders.
- Balebandro (Physically-Based-Reloaded): PBR weather scales, the grass size fade.
- VHeyes (mirjomi): fixes including SMAA and the material pass
  ([docs/upstream-cherry-picks.md](docs/upstream-cherry-picks.md)).
- The [Community Shaders](https://github.com/community-shaders/skyrim-community-shaders) team (GPL-3.0): terrain and
  cloud shadows, grass collision, skylighting, LOD blending, terrain variation and the True PBR layout
  ([docs/COMMUNITY-SHADERS-PORTS.md](docs/COMMUNITY-SHADERS-PORTS.md)).
- [WallSoGB](https://github.com/WallSoGB)'s Fallout: New Vegas database, behind the engine fixes; the NVSE / xNVSE
  projects and the community's reverse engineering of the game.
- SMAA by Jorge Jimenez et al. (MIT).
- Polyphony Digital: the Perceptual tone mapper (their sample code, MIT) and the eye adaptation design, from "Driving Toward
  Reality" (SIGGRAPH 2025, Gran Turismo 7).

## License

GPL-3.0 with the additional terms in [License.md](License.md) (see also [COPYING.txt](COPYING.txt)). Binaries may not
be sold. Provided as is, without warranty.
