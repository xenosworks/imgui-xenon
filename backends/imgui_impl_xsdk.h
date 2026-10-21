// dear imgui: Platform + Renderer Backend for the Xbox 360 through xsdk (https://github.com/xenosworks/xsdk)
// This draws through xsdk's GPU layer and reads controller 1, so it needs no other backend.

// Implemented features:
//  [X] Renderer: User texture binding. Use 'XsdkTexture*' as texture identifier: (ImTextureID)(intptr_t)&texture.
//  [X] Renderer: Large meshes support (64k+ vertices) even with 16-bit indices (ImGuiBackendFlags_RendererHasVtxOffset).
//  [X] Renderer: Texture updates support for dynamic font atlas (ImGuiBackendFlags_RendererHasTextures).
//  [X] Platform: Gamepad support: controller 1, or states passed to ImGui_ImplXsdk_ProcessGamepad().
//  [X] Platform: Text fields typed into through XAM's on-screen keyboard.
// Missing features or Issues:
//  [ ] Mouse, mouse cursors, chatpads and USB keyboards.
//  [ ] Drawing inside an xsdk recording or tiled pass, which stops at an assert.

// You can use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp
// How a title adds it: docs/README-xsdk.md.

#pragma once
#include "imgui.h"      // IMGUI_IMPL_API
#ifndef IMGUI_DISABLE
#include <stdint.h>
#include "xsdk/kernel.h" // INPUT_GAMEPAD

// Follow "Getting Started" link and check examples/ folder to learn about using backends!
// Init after xsdk_gpu_open(); Shutdown before xsdk_gpu_close().
IMGUI_IMPL_API bool     ImGui_ImplXsdk_Init();
IMGUI_IMPL_API void     ImGui_ImplXsdk_Shutdown();
// Before ImGui::NewFrame(), with the size xsdk_present_open() gave.
IMGUI_IMPL_API void     ImGui_ImplXsdk_NewFrame(uint32_t width, uint32_t height);
// Draws into the target set now. Leaves set its shaders, vertex stream 0, texture slot 0, vertex constant 0, ImGui's
// blending, depth, stencil and culling off, and the viewport and scissor over the display.
IMGUI_IMPL_API void     ImGui_ImplXsdk_RenderDrawData(ImDrawData* draw_data);

// For a title that reads its controllers itself: a state passed before ImGui_ImplXsdk_NewFrame() stands in for
// controller 1's that frame.
IMGUI_IMPL_API void     ImGui_ImplXsdk_ProcessGamepad(const INPUT_GAMEPAD* gamepad);

// (Advanced) Use e.g. if you need to precisely control the timing of texture updates (e.g. for staged rendering), by setting ImDrawData::Textures = nullptr to handle this manually.
IMGUI_IMPL_API void     ImGui_ImplXsdk_UpdateTexture(ImTextureData* tex);

#ifdef IMGUI_IMPL_XSDK_CHECK
// The check title's way in, as XAM's keyboard needs a person: completes the open keyboard as XAM would, with text in
// its buffer, and the overlapped's result and extended error. Returns false if no keyboard was open.
IMGUI_IMPL_API bool     ImGui_ImplXsdk_CompleteKeyboard(uint32_t result, uint32_t extended_error, const uint16_t* text);
#endif

#endif // #ifndef IMGUI_DISABLE
