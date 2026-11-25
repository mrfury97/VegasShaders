#pragma once

// 0xB89D80
VirtFuncDetour kSkyShaderConstantsDetour;
void __fastcall SkyShader__UpdateConstants(SkyShader* apThis, void*, const NiPropertyState* apProperties) {
    const SkyShaderProperty* pShaderProp = apProperties->GetShadeProperty<SkyShaderProperty const>();
    uint32_t eSkyObjectType = pShaderProp->eSkyObjectType;
    TheShaderManager->ShaderConst.skyObjectID = eSkyObjectType;

    ThisCall(kSkyShaderConstantsDetour.GetOverwrittenAddr(), apThis, apProperties);

    // UNOFFICIAL 2026-10-29: the id alone (SKYTEX.pso reads c20.x). This uploaded 16 bytes from &skyObjectID, i.e. the id and the sun
    // disc colour after it, into c20, where SKY.pso kept its horizon tint until it moved to c32 (red horizon band, tester + user).
    const float objectID[4] = { TheShaderManager->ShaderConst.skyObjectID, 0.0f, 0.0f, 0.0f };
    TheRenderManager->device->SetPixelShaderConstantF(20, objectID, 1);
}

// UNOFFICIAL: the skin shader's hooks (effects/Skin.h, SkinTemplate.hlsl; 2026-10-25). After the hooks in Conor's (mrfury97) NVR
// testbed (github.com/mrfury97/Conor-sClaudeHell-NVR-SAL-Fork, branch testbed, src/NewVegas/Hooks/Shaders.cpp), which bound his
// screen-space blur's targets this way; this build's lamp shadows and HudDrops are set per bind in SetShadersHook (Render.cpp).
// Every hook here does nothing while Vanilla Plus Skin's shaders are in use (SkinShaders::Active).
//
// SkinShader's per-geometry virtuals (vtable 0x10BB980), bracketing every skin draw on both of
// the batch renderer's paths (BSBatchRenderer::RenderPassImmediately_Standard and _Skinned):
// slot 27 PrepareGeometryForRendering (NiD3DShader, 0xE812F0) runs just before the draw, slot 35
// PostGeometry (ShadowLightShader, 0xB7C320) just after. The skin scattering effect binds its
// second render target between the two and nowhere else (src/effects/SkinScattering.h: for NVR's
// skin pixel shaders, and for the game's highlight passes on skin while the blur runs). The
// batch-level SetupPassShaders hook (SetShadersHook) runs once per batch, not per draw.
VirtFuncDetour kSkinPrepareGeometryDetour;
VirtFuncDetour kSkinPostGeometryDetour;

// The engine also draws its own highlight passes on skin geometry (traced on FaceGenFace:
// BSSM_2x_SPECULARDIR_S / _SPECULARPT_S with the object shaders), BSSM_SPECULARDIR (0x154) to
// BSSM_2x_SPECULARPT3_Sb (0x177). NVR's skin shader draws physically based highlights itself, so
// while it is on these are muted (colour writes off for the draw): with them, faces had two sets
// of highlights, and the screen-space scattering blurred the second as if it were diffuse light.
static bool  sSkinHighlightMuted = false;
static DWORD sSkinSavedWriteMask = 0xF;

// FaceGen faces in the light-only passes (AD2/AD3: SKIN2004-SKIN2007). The engine multiplies the
// frame by the face's colour afterwards in BSSM_TEXTURE_SFg (SLS1005: the base texture with both
// FaceGen maps, 2((2(fg0 - 0.5) + base) 2 fg1), the same blend as the full pass), but binds only
// the base texture to these passes (SkinShader::SetupGeometryTextures, 0xBD02E0), so the skin
// shader divided its highlights, and wrote the scattering effect's albedo and brightness, with
// the bare base texture: off by the FaceGen tint, which showed as tinted lips, brows and eyelids
// and threw the scattering's skin test. The FaceGen maps go to stages 2 and 3, as the full pass
// has them (texture 1 of the diffuse and normal sets); stage 3 is the glow map here, which the
// skin shader does not read and the engine sets again for every draw. Stage 2 is otherwise empty
// in these passes and is emptied again after the draw (ApplyTextureStages binds only stages with
// a texture), so nothing later binds a stale texture through it.
static void* sFaceGenStage2 = nullptr;   // the stage whose texture PostGeometry clears

static bool BindFaceGenMaps(void* apThis, NiGeometry* Geometry, NiD3DPass* Pass) {
    if (!Geometry || !Pass || Pass->StageCount < 4) return false;
    BSShaderPPLightingProperty* Property = static_cast<BSShaderPPLightingProperty*>(Geometry->GetProperty(NiProperty::PropertyType::kType_Shade));
    if (!Property || !(Property->ulFlags[0] & BSShaderProperty::FaceGen)) return false;
    if (!Property->ppTextures[0] || !Property->ppTextures[0][1] || !Property->ppTextures[1] || !Property->ppTextures[1][1]) return false;

    void** Stages = *(void***)((UInt8*)Pass + 0x24);   // NiD3DPass::m_kStages.m_pBase
    if (!Stages || !Stages[2] || !Stages[3]) return false;

    ThisCall(0xB7C0A0, apThis, Property, 2, 1);   // ShadowLightShader::SetDiffuseMap: FaceGenMap0
    ThisCall(0xB7C0E0, apThis, Property, 3, 1);   // ShadowLightShader::SetNormalMap: FaceGenMap1
    ThisCall(0xBE2170, apThis);                   // BSShader::ApplyTextureStages
    sFaceGenStage2 = Stages[2];
    return true;
}

void* __fastcall SkinShader__PrepareGeometryForRendering(void* apThis, void*, void* apGeometry, void* apPartition, void* apRendererData, void* apState) {
    void* Result = (void*)ThisCall(kSkinPrepareGeometryDetour.GetOverwrittenAddr(), apThis, apGeometry, apPartition, apRendererData, apState);
    if (!TheShaderManager || !TheRenderManager || !TheSettingManager) return Result;

    NiD3DPass* Pass = *(NiD3DPass**)0x0126F74C;   // NiD3DShader::m_pCurrentPass
    NiD3DPixelShaderEx* PixelShader = Pass ? (NiD3DPixelShaderEx*)Pass->PixelShader : nullptr;
    const UInt16 PassType = *(UInt16*)0x011F91E4;  // BSShaderManager::eCurrentPass

    const bool NVRSkin = TheShaderManager->Shaders.Skin->Active() && TheShaderManager->Shaders.Skin->Enabled && TheSettingManager->SettingsMain.Main.RenderEffects;
    const bool SkinPixelShader = PixelShader && PixelShader->Name && !memcmp(PixelShader->Name, "SKIN", 4);
    // The pass type alone is not trusted to be current: a skin pixel shader is never muted.
    // Muted with [Shaders.Skin.Main] MuteGameHighlights (the default: the skin shader draws its own highlights). Kept otherwise:
    // while the screen-space blur runs they are then drawn into the blur's skin target as well as the scene
    // (BindForGameHighlights), so the blur leaves them sharp instead of blurring them as light; muted where that copy cannot
    // be made (with MSAA off, faces came out 22% darker with the blur than without it while they were muted: evidence\skin-resync-LOG.md run B2).
    SkinScatteringEffect* Blur = TheShaderManager->Effects.SkinScattering;
    const bool BlurRuns = Blur && Blur->Enabled && Blur->Supported && Blur->ScreenSpace;
    const bool GameHighlightPass = NVRSkin && !SkinPixelShader && PassType >= 0x154 && PassType <= 0x177;
    if (GameHighlightPass && !sSkinHighlightMuted) {
        const bool Keep = !TheShaderManager->Shaders.Skin->MuteGameHighlights && (!BlurRuns || Blur->BindForGameHighlights(PixelShader));
        if (!Keep) {
            TheRenderManager->device->GetRenderState(D3DRS_COLORWRITEENABLE, &sSkinSavedWriteMask);
            TheRenderManager->device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
            sSkinHighlightMuted = true;
        }
    }

    // The scattering targets only for NVR's own skin pixel shaders, not the highlight or fog
    // passes the engine also draws with this shader.
    SkinScatteringEffect* Scattering = TheShaderManager->Effects.SkinScattering;
    if (Scattering && SkinPixelShader)
        Scattering->BindForSkinDraw(PixelShader);

    // The light-only passes: SKIN2004-SKIN2007.
    bool FaceGenBound = false;
    if (NVRSkin && TheShaderManager->Shaders.Skin->BindFaceGenMaps && SkinPixelShader && !memcmp(PixelShader->Name, "SKIN200", 7) && PixelShader->Name[7] >= '4' && PixelShader->Name[7] <= '7')
        FaceGenBound = BindFaceGenMaps(apThis, (NiGeometry*)apGeometry, Pass);

    // Develop.DebugMode + the TraceShaders key: what this hook did for every skin draw that frame.
    if (TheSettingManager->SettingsMain.Develop.DebugMode && Global->OnKeyDown(TheSettingManager->SettingsMain.Develop.TraceShaders)) {
        Logger::Log("SkinHook: pass %s (0x%X), pixel shader %s: highlights muted %d, copied %d, scattering bound %d",
            Pointers::Functions::GetPassDescription(PassType), (UInt32)PassType,
            (PixelShader && PixelShader->Name) ? PixelShader->Name : "(none)",
            sSkinHighlightMuted ? 1 : 0, (Scattering && Scattering->HighlightsBound) ? 1 : 0, (Scattering && Scattering->Bound) ? 1 : 0);
    }
    // Port, diagnostic ([Shaders.Skin.Debug] LogDraws, read once): the first LogDraws world-scene draws through SkinShader, in
    // the log (pass, both pass enums, pixel shader, blend, what this hook did).
    static int sLogLeft = -1;
    if (sLogLeft < 0) sLogLeft = (std::max)(0, TheSettingManager->GetSettingI("Shaders.Skin.Debug", "LogDraws"));
    if (sLogLeft > 0 && Scattering && Scattering->InWorldScene) {
        sLogLeft--;
        DWORD Blend = 0, Src = 0, Dst = 0, Write = 0;
        TheRenderManager->device->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
        TheRenderManager->device->GetRenderState(D3DRS_SRCBLEND, &Src);
        TheRenderManager->device->GetRenderState(D3DRS_DESTBLEND, &Dst);
        TheRenderManager->device->GetRenderState(D3DRS_COLORWRITEENABLE, &Write);
        const void* RenderPass = *(void**)0x011F91E0;	// BSShaderManager::pCurrentRenderPass (+0x04 its pass enum)
        const UInt16 PassEnum = RenderPass ? *(const UInt16*)((const UInt8*)RenderPass + 0x04) : 0xFFFF;
        const bool Nvr = PixelShader && PixelShader->ShaderHandleBackup && PixelShader->ShaderHandle != PixelShader->ShaderHandleBackup;
        Logger::Log("SKIN DRAW %s: eCurrentPass 0x%X (%s), render pass 0x%X (%s), PS %s (%s), blend %d %d->%d, write 0x%X: muted %d, highlights copied %d, scatter bound %d, facegen %d",
            ((NiGeometry*)apGeometry)->m_pcName ? ((NiGeometry*)apGeometry)->m_pcName : "?", (UInt32)PassType, Pointers::Functions::GetPassDescription(PassType),
            (UInt32)PassEnum, Pointers::Functions::GetPassDescription(PassEnum), (PixelShader && PixelShader->Name) ? PixelShader->Name : "(none)",
            Nvr ? "NVR" : "game", (int)Blend, (int)Src, (int)Dst, (UInt32)Write, sSkinHighlightMuted ? 1 : 0, Scattering->HighlightsBound ? 1 : 0, Scattering->Bound ? 1 : 0, FaceGenBound ? 1 : 0);
    }

    // NVR_SkinDraw (Includes/Skin.hlsl, c194), every skin draw: x 1 while the blur's targets are bound, so the shader leaves
    // the diffusion to the screen-space blur; y 1 while the FaceGen maps are bound to a light-only pass.
    const float ScreenSpace[4] = { (Scattering && Scattering->Bound) ? 1.0f : 0.0f, FaceGenBound ? 1.0f : 0.0f, 0.0f, 0.0f };
    TheRenderManager->device->SetPixelShaderConstantF(194, ScreenSpace, 1);
    return Result;
}

void __fastcall SkinShader__PostGeometry(void* apThis, void*, void* apProperties) {
    ThisCall(kSkinPostGeometryDetour.GetOverwrittenAddr(), apThis, apProperties);
    if (TheShaderManager && TheShaderManager->Effects.SkinScattering) TheShaderManager->Effects.SkinScattering->Unbind();
    if (sFaceGenStage2) {
        *(void**)((UInt8*)sFaceGenStage2 + 0x08) = nullptr;   // NiD3DTextureStage::m_pkTexture
        sFaceGenStage2 = nullptr;
    }
    if (sSkinHighlightMuted) {
        if (TheRenderManager && TheRenderManager->device)
            TheRenderManager->device->SetRenderState(D3DRS_COLORWRITEENABLE, sSkinSavedWriteMask);
        sSkinHighlightMuted = false;
    }
}

// UNOFFICIAL 2026-11-04 (from Physically-Based-Reloaded) the Pip-Boy's screen: once the game has rendered the menu into the screen's
// texture (its scanlines and blur included), the raindrops are drawn into it (HudRaindropsEffect::DrawOnScreen), so they sit on the
// screen itself. FOPipboyManager's vtable 0x10780AC, slot 20.
VirtFuncDetour kPipboyPostProcessDetour;

void __fastcall FOPipboyManager__RenderPostProcess(void* apThis, void*, void* apSource, BSRenderedTexture* apTarget, UInt32 aeClearFlags) {
    ThisCall(kPipboyPostProcessDetour.GetOverwrittenAddr(), apThis, apSource, apTarget, aeClearFlags);
    if (TheShaderManager->Effects.HudRaindrops) TheShaderManager->Effects.HudRaindrops->DrawOnScreen(apTarget);
}

// Interior faces. BSShaderPPLightingProperty::GetRenderPasses_2x (0xBDF790) builds every lit
// mesh's pass list, and for FaceGen geometry in interiors it skips the passes SkinShader draws
// itself (ADT/ADT2 and AD2/AD3: the two "!bIsFaceGen || !BSShaderManager::bInterior" tests). An
// interior face got the generic ones instead, traced in game:
//   BSSM_AMBIENT_S (SLS1000) + BSSM_DIFFUSEDIR_S (SLS1002): ambient and the main light, vanilla
//   BSSM_DIFFUSEPT2/3_SFg (SKIN2008/9): additive point-light passes, the only NVR skin
//   BSSM_TEXTURE_SFg (SLS1005): the FaceGen texture multiplied in
// So indoors the skin shader lit only the lamps, the highlights of the main light were gone (its
// specular pass is muted while NVR skin is on) and the screen-space scattering never ran: only
// an opaque skin pass writes the depth and albedo it needs to find skin.
//
// Both tests read bInterior from one load, "mov al, [BSShaderManager::bInterior]" at 0xBE018B
// (A0 27 94 1F 01; the third way into the second test, from 0xBE01B4, reuses al too). The patch
// points that load at FaceGenInteriorFlag: 0 while NVR's skin shader is on, so interior faces
// take the exterior route (one ADT pass, or AD + point-light + texture passes, all SkinShader's
// own); the real bInterior when it is off, which keeps vanilla's behaviour. Nothing else reads
// it: bInterior's other test in that function (no projected actor shadows indoors) is a separate
// load and untouched.
static UInt8 FaceGenInteriorFlag = 0;

void InstallFaceGenInteriorPatch() {
    static const UInt8 Expected[5] = { 0xA0, 0x27, 0x94, 0x1F, 0x01 };
    if (memcmp((const void*)0xBE018B, Expected, sizeof(Expected)) != 0) {
        Logger::Log("[WARNING] Skin: unexpected code at 0xBE018B; interior faces keep vanilla's passes");
        return;
    }
    SafeWrite32(0xBE018C, (UInt32)&FaceGenInteriorFlag);
}

// Every frame, before the scene's pass lists are built (RenderHook).
void UpdateFaceGenInteriorFlag() {
    const UInt8 Interior = *(UInt8*)0x011F9427;   // BSShaderManager::bInterior
    const bool NVRSkin = TheShaderManager->Shaders.Skin && TheShaderManager->Shaders.Skin->Active() && TheShaderManager->Shaders.Skin->Enabled && TheSettingManager->SettingsMain.Main.RenderEffects;
    FaceGenInteriorFlag = NVRSkin ? 0 : Interior;
}
