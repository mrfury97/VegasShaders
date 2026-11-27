#pragma once
using RenderShadowMapFn = void(__thiscall*)(void*);
extern RenderShadowMapFn RenderShadowMap;
void __fastcall RenderShadowMapHook(void* apThis, UInt32);
void LeavesNodeNameHook();