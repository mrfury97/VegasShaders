# Vegas Shaders architecture

> Technical reference. Moved from the `vs-architecture` skill on 2026-11-25 (content unchanged apart from links).

## The plugin
- Vegas Shaders (formerly New Vegas Reloaded) is a 32-bit xNVSE plugin for Fallout: New Vegas: `VegasShaders.dll`, built from
  `NewVegasReloaded\NewVegasReloaded.vcxproj` (Release, Win32, MSVC toolset v142). Build: the `vs-build-deploy` skill.
- Shaders under `src\hlsl\NewVegas\` are compiled at runtime, not built into the DLL: a shader change ships as files under
  `<game>\Data\Shaders\VegasShaders\`. The shader cache reuses an entry only when the whole preprocessed text matches, so editing
  a shared include recompiles every shader that uses it (minutes on a slow PC).
- Ray-traced effects run in a separate 64-bit helper through the project's DXVK fork, because 32-bit Vulkan has no ray tracing.

## Key files
- `src/core/ShaderManager.cpp`: main render loop; calls `ImGuiManager::NewFrame()` and `Render()`.
- `src/core/EffectChain.cpp`: frame order of the effects (see the `vs-effects-modular` skill).
- `src/core/ImGuiManager.cpp`: ImGui settings overlay (DX9 + Win32 backend); input under DXVK: [dxvk-input.md](dxvk-input.md).
- `src/core/SettingManager.cpp`: TOML-backed settings; the ImGui menu is auto-populated from it.
- `src/core/Paths.h/.cpp`: every runtime file and folder name (see [runtime-paths.md](runtime-paths.md)).
- `src/core/SkinBones.h`: every reader of skin `BoneObjects` goes through it (bones matched by pointer; NV-SMP-Physics safe).
- `lib/imgui`: git submodule, docking branch (`git submodule update --init --recursive` after a fresh clone).
- xNVSE headers: `src/NewVegas/nvse/`.

## Docs
- Docs map: the repo `CLAUDE.md`. Current design docs live in `docs\`, technical reference in `docseference\`, history in
  `docsrchive\` (see its `MANIFEST.md`). When a doc goes stale, rewrite it or move it to the archive; do not leave it in place.
- Deferred vs forward background: [../deferred-vs-forward.md](../deferred-vs-forward.md).
