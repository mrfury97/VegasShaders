---
name: vs-architecture
description: Map of the Vegas Shaders code base - what the plugin is, the key source files (ShaderManager render loop, ImGuiManager overlay, SettingManager, xNVSE headers, imgui submodule), runtime-compiled shaders and their cache, the 64-bit RT helper, and where design docs live. Use when starting work in an unfamiliar part of the repo or deciding where a change belongs.
---

# Vegas Shaders architecture

## The plugin
- Vegas Shaders (formerly New Vegas Reloaded) is a 32-bit xNVSE plugin for Fallout: New Vegas: `VegasShaders.dll`, built from
  `NewVegasReloaded\NewVegasReloaded.vcxproj` (Release, Win32, MSVC toolset v142). Build: the vs-build-deploy skill.
- Shaders under `src\hlsl\NewVegas\` are compiled at runtime, not built into the DLL: a shader change ships as files under
  `<game>\Data\Shaders\VegasShaders\`. The shader cache reuses an entry only when the whole preprocessed text matches, so editing
  a shared include recompiles every shader that uses it (minutes on a slow PC).
- Ray-traced effects run in a separate 64-bit helper through the project's DXVK fork, because 32-bit Vulkan has no ray tracing.

## Key files
- `src/core/ShaderManager.cpp`: main render loop; calls `ImGuiManager::NewFrame()` and `Render()`.
- `src/core/EffectChain.cpp`: frame order of the effects (see vs-effects-modular).
- `src/core/ImGuiManager.cpp`: ImGui settings overlay (DX9 + Win32 backend); input under DXVK: vs-dxvk-input.
- `src/core/SettingManager.cpp`: TOML-backed settings; the ImGui menu is auto-populated from it.
- `src/core/Paths.h/.cpp`: every runtime file and folder name (see vs-runtime-paths).
- `src/core/SkinBones.h`: every reader of skin `BoneObjects` goes through it (bones matched by pointer; NV-SMP-Physics safe).
- `lib/imgui`: git submodule, docking branch (`git submodule update --init --recursive` after a fresh clone).
- xNVSE headers: `src/NewVegas/nvse/`.

## Docs
- Design docs live in `docs\` (no index yet). When a doc goes stale, rewrite or delete it rather than leaving it.
- Deferred vs forward background: `docs\deferred-vs-forward.md`.
