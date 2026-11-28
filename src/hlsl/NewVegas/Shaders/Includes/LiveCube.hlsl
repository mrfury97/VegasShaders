// UNOFFICIAL: the live surroundings cube for the object shaders -- metal (Object.hlsl getMetalAmbient, ObjectTemplate.hlsl) and
// the env-map pass (EnvTemplate.hlsl). The cube is built by the Reflections effect (src/effects/Reflections.cpp, CubeMap):
// a small cube round the camera, from what was on screen in each direction (Effects/Reflections.fx.hlsl Capture/Filter),
// blurred into mips by roughness. It is one frame old when the objects read it. Premultiplied: rgb = linear colour x
// coverage, a = coverage (0 where nothing was seen yet).
// TESR_EnvCube (PBRShaders::UpdateConstants): x metal's share ([Shaders.PBR.Metal] CubeReflections), y the env-map pass's
// ([Shaders.PBR.EnvMaps] CubeReflections), both 0 while the cube is not kept up (Reflections off, CubeMap off), both times
// min([Shaders.Reflections.Main] CubeStrength, 1); z the last mip level; w (2026-11-27 cube-visible) 1 / max(CubeStrength, 1).
// The sampler is never read while its share is 0, so what else may sit on s10 then does not matter.
// s10: free in the object shaders (game samplers s0-s7, the sun atlas s9, the interior shadow cubes s8 and s11-s15); taken
// by the sun cross-fade's old atlas when SUN_CROSSFADE is on, so the includer leaves this out then.

float4 TESR_EnvCube : register(c190);
// One line (ShaderTextureValue::GetSamplerStateString reads the states only to the end of the register's line).
samplerCUBE TESR_ReflectionsEnv : register(s10) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

// The cube along a world direction, blurred for the roughness: rgb in the object shaders' encoding (the square root of
// linear light, as SkyAmbient's sky), a how much of it was ever seen. A coarser level fills the gaps of the sharper one.
float4 LiveCube(float3 worldDir, float roughness) {
    // (2026-11-27 cube-visible) CubeStrength k above 1: smooth metal (roughness up to 0.2) reads k times sharper, back to the plain lod
    // in a straight ramp by 0.5 (Compute/Reflections.glsl CubeLod, the same rule), so rough surfaces keep their blur.
    float lod = roughness * TESR_EnvCube.z * lerp(TESR_EnvCube.w, 1.0f, saturate(roughness * 3.3333333f - 0.6666667f));
    float4 sharp = texCUBElod(TESR_ReflectionsEnv, float4(worldDir, lod));
    float4 coarse = texCUBElod(TESR_ReflectionsEnv, float4(worldDir, min(lod + 2.0f, TESR_EnvCube.z)));
    float4 m = sharp + coarse * (1.0f - saturate(sharp.a));
    return float4(sqrt(max(m.rgb / max(m.a, 1.0e-4f), 0.0f)), saturate(m.a));
}
