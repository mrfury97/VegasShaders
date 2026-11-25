# Vegas Shaders with NV-SMP-Physics (2026-11-18)

NV-SMP-Physics (github.com/Balebandro/NV-SMP-Physics, commit fd9ff44, read as a reference only at `F:\Claude-refs\refs\NV-SMP-Physics`)
simulates SMP cloth, hair and physics bones in New Vegas. A tester crashed with it (CrashLogger 2026-11-18-09-20-54). This is what was
found and changed on branch `pipboyfit-crash`.

## The crash
- `PipboyFit::ForGeometry` (`src/core/PipboyFit.h:122` before the fix) ran `_stricmp` on the name of every bone a skinned mesh pointed at.
  On a mod's first-person shape "VirtualLegs" (`BSDismemberSkinInstance`) one bone slot pointed at float data: the name pointer was
  0xC1880000 (-17.0f) and the read faulted (main thread; `RenderPassSkinnedHook`, `Render.cpp:4408`).
- Why a bone slot can point at garbage (believed for this crash, documented by NV-SMP-Physics `src/nv/ActorManager.cpp:33-48`): when the
  game attaches an item it drops the item's own bone nodes from the scene, but the skin keeps pointing at them until the game binds the skin
  to the skeleton, seconds later, after the Pip-Boy closes. Those nodes may be freed or their memory reused. The crash stack held the Pip-Boy
  manager and its screen, which fits. This is the game's own behaviour; NV-SMP-Physics works around it for its items (reads slots under SEH,
  puts its own never-freed nodes in stale slots: `ActorManager.cpp:49-135`), and shapes it does not handle keep the stale slots.

## What NV-SMP-Physics does (proven from its source)
- Hooks: `TESMain::DrawWorld_WaitForParallelUpdate` 0x00872AD0 (it runs the physics after it, on the main thread, before the draw) and
  `Animation::FindSkinnedNode` at the call 0x004AF0FC (SMP items are attached as rigid add-ons, keeping their own bone nodes)
  (`src/nv/main.cpp:309-335,554-555`, `src/nv/Game.h:18-28`). No render hooks: nothing overlaps `RenderPassSkinnedHook`.
- Writes into skins: stale slots of SMP items swapped for its own nodes (`ActorManager.cpp:133`); hair and race-menu heads re-bound with
  bone arrays it allocates with the game's allocator (`ActorManager.cpp:1000-1030,1110-1130`, freed with the skin by the game); the hair's
  head slot moved to the skeleton's head (`ActorManager.cpp:820-831`). The bones it simulates get their world transforms written before
  the draw, so everything that reads bones at draw time sees the simulated pose.

## What Vegas Shaders does now
- `src/core/SkinBones.h`: `Live(bone)` (the slot's object readable under SEH and its vtable in the game's .rdata, where every game vtable
  is) and `Name(bone)` (the name copied under SEH). Every reader of `NiSkinInstance::BoneObjects` goes through them.
- PipboyFit finds the forearm bones by name in the player's own live skeletons once a frame and compares slots by pointer only.
- ClothWind `SkeletonOf` and CharacterObjectReach `Classify` (bone names, any actor): names through `Name`; a non-live slot makes the
  result "unsure", not cached (ClothWind retries at most once a frame; its cloth auto-detect stores no verdict then).
- Transform readers (ClothWind `BoneXf`, PipboyFit `ForPartition`, GearWeather, DeferredFirstPerson, ShadowManager's skinned bounds and
  caster boxes, RtCore's RT skinning palette) skip a non-live slot.

## Still wrong with SMP (not crashes)
- (Fixed 2026-11-18) ClothWind's procedural wind moved garments SMP already simulates a second time. Now a skinned shape carrying SMP's
  "SMP Bones" string extra data (the plugin's own marker, one per skinned shape: its README, ActorManager.cpp:110) gets no cloth wind:
  `NiObjectNET::ExtraDataNamed` (GameNi.h; the extra-data list at +0x10, its count at +0x14, each entry's name at +0x08, as JIP LN's
  headers have it; LayoutCheck.h asserts the offsets). SMP blows them in the weather's wind itself.
- `Live` shows a slot looks like a node, not that it is still in the scene: a dropped node not yet freed passes, and for one frame its
  transform can be garbage in RT skinning, shadow bounds or cloth points during the bind wait.

## Not run
Nothing here was run in game. The VirtualLegs shape's own layout was not seen.
