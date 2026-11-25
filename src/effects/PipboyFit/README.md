# PipboyFit

Code: `src/core/PipboyFit.h` (no C++ file in this folder), shader `src/hlsl/NewVegas/Shaders/Includes/PipboyFit.hlsl`, defaults
`resource/defaults/PipboyFit.toml` (`[_Shaders.PipboyFit.Main]`). Written 2026-11-25 from the room-02 memory note
`pipboy-sleeve-clipping-research` (2026-11-03 to 2026-11-05 work), which is archived in `F:\Claude Brain\archive\2026-11-25\vs\memory\`.
Where this file and the code disagree, the code wins: the note's values were checked against `resource/defaults/PipboyFit.toml` on 2026-11-25.

## 1. Purpose
After GPU skinning, pulls the left-forearm vertices of armor sleeves in under the Pip-Boy casing, using a soft clamp inside an ellipse
in the frame of the "Bip01 L ForeTwist" bone, so sleeves no longer poke through the casing.

- Who gets it: first person, plus the player only in third person (`ThirdPerson`), never NPCs. Bare skin is never fitted
  (`SkinTemplate` has no fit code; owner's decision 2026-11-03: a bare arm, Improved Vanilla Male Body, got a ~0.9 unit dent at x 3-4).
- Upstream has had the fit since 1.0.2; only the `PowerArmor` toggle is ours (fork branch `pipboy-fit`: d7af59b fit, bc75ba2 PowerArmor).

## 2. Inputs, registers, passes
- Compiled into ObjectTemplate and EnvTemplate under `#ifdef SKIN` only; compile define `PIPBOY_FIT_ON` (restart to change).
- Vertex-shader registers c182-c187, written per skinned partition draw from `ClothWind::DrawPartition` (the partition hook is installed
  when Cloth or PipboyFit is on). c182: bone slots, w = wrist scale; c183-c185: shader space to capsule frame; c186: on, strength, knee,
  falloff; c187: start, end, radius y, radius z. Conflicts checked in 2026-11: PS c182 (PointShadowForward) is a separate register file,
  and GDECALS VS c180-c187 is not skinned.
- Finding its draws: `ForGeometry` from `RenderPassSkinnedHook` (skin has "Bip01 L ForeTwist", name not containing "pipboy"); in third
  person only for the player (`OnPlayer` walks m_parent to Player->GetNode()). Only vertices weighted to L ForeTwist / Forearm / Hand move.
- Bone matching goes through `src/core/SkinBones.h` (by pointer; an earlier `_stricmp` through a bad bone slot crashed with a VirtualLegs
  mesh from PlayerPhysics / NV-SMP-Physics: see `docs/smp-compatibility.md`). Tester workaround for old builds: `Exclude += virtuallegs`.

## 3. Settings (`[_Shaders.PipboyFit.Main]`, all live except Enabled; defaults as in the repo)
Shape: Strength 1, Knee 0.7, Falloff 2, Start 4.5, End 17, CenterY 0.1, CenterZ 0.4, RadiusY 2.6, RadiusZ 2.6, WristScale 0.6, Axis 0.
Who: ThirdPerson true, PowerArmor true (false skips the fit while the player wears a power-armor torso), Exclude "pipboy".
The owner tunes the values live in the menu; old saved values can hide changed defaults (RadiusY/Z 3.0 stayed after the default changed).

## 4. Measurements and engine facts
**Proven by measurement (2026-11-03, a Sonnet agent):**
- ForeTwist +X runs along the forearm; the wrist is at x 18.1.
- The casing is `meshes\pipboy3000\pipboyarm.nif`: shape at x 4.4-17.1, inner radius about 3.3, centre y 0.1 z 0.4; its inner narrows to
  about 1.6-2.6 over the last 4 units at the wrist end (why WristScale tapers the radius over End-4..End).
- The glove1st NIFs hold only the hand. Vanilla sleeves poke out to about 1.0-1.7.
- The third-person skeleton + `pipboyarmnpc.nif` measure the same, so one set of values serves both views.
**Engine facts (FNV 1.4.0.525):** Pip-Boy = biped slot 6; the pipboyon / pipboyoff child-name switch is in BipedAnim::AttachSkinnedObject
(0x4AD4A0); `foo1st.nif` is used automatically in first person (0x4AF950); power-armor torso test =
MiddleHighProcess::IsWearingPowerArmorTorso (0x8D8290) on Player->process.

## 5. What correct / wrong looks like, debugging
- Correct: sleeves end on the casing's inside, no cloth visible outside the Pip-Boy, no step at the elbow or wrist end.
- If it misbehaves, suspect the shader-space derivation (palette c44 against the computed bone) or the Axis. The 2026-11-03 log build
  printed "PipboyFit: space check" (want about 0; it read 0.0001-0.0007 in first and third person, scale 1.000). One 1800-frame window
  had 288k first-person fits (~160 a frame, likely many lamp passes or the Pip-Boy menu), otherwise about 2 a frame.

## 6. Open
- **Open:** an in-game visual check of the tighter values and of power armor (not done as far as the note says).
- **Open:** cloth wind on SMP-simulated garments needs an extra-data reader for "SMP Bones" (docs/smp-compatibility.md).
- Balebandro's fork fixed a crash in `PipboyFit::ForPartition` after a load; ours is the by-pointer bone match above.
