---
name: vs-runtime-paths
description: "Vegas Shaders runtime file and folder names after the 2026-11-02 rename (VegasShaders.* vs old NewVegasReloaded.*), which old names are still read, which names were kept on purpose, and the Paths:: rule. Use before touching any code that reads or writes a runtime file or folder (DLL, settings toml, shader/texture/preset folders, logs), or when a tester's install has old names."
---

# Runtime names (2026-11-02 rename)

`src/core/Paths.h/.cpp` owns every runtime name.

| New | Old (still read when the new one is missing) |
|---|---|
| `Data\NVSE\Plugins\VegasShaders.dll` (+ `.pdb`) | `NewVegasReloaded.dll` (an old copy left beside the new one is logged as an error: two plugins) |
| `VegasShaders.toml` (user settings; always written under this name) | `NewVegasReloaded.dll.toml` (read only; never changed or deleted) |
| `VegasShaders.defaults.toml`, `.maxquality.toml`, `.maxquality.user.toml`, `.materials.txt`, `.imgui.ini` | `NewVegasReloaded.dll.<same>` / `NewVegasReloaded.imgui.ini` |
| `Data\Shaders\VegasShaders\` (Shaders, Effects, Compute, caches) | `Data\Shaders\NewVegasReloaded\` |
| `Data\Textures\VegasShaders\` (LUTs) | `Data\Textures\NewVegasReloaded\` |
| `Data\VegasShaders\` (Presets, Variants, Keywords, Skins, EnabledVariants.ini) | `Data\NVR\` |
| `VegasShaders-Logs\VegasShaders_<date>.log` (game folder) | none: logs are only written under the new names |

- Folders are chosen once at startup (new if it exists, else old if it exists, else new) and logged in one `PATHS folders:` line.
- Kept unchanged on purpose: the xNVSE plugin name `NewVegasReloaded` (Main.cpp; mods check it), the console prefix `NVR`, every
  settings section/key, `TESR_*` names, preset formats, log line tags, and the legacy `NewVegasReloaded.dll.config` (only read).
- New code that touches a runtime file or folder goes through `Paths::` (or `VS_LOGS_FOLDER` for the logs), never a literal path.
