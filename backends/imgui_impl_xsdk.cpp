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

// CHANGELOG
//  2026-10-21: Initial version, for xsdk's milestone 57 (xsdk:docs/scope/57-text-and-ui.md).

#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_xsdk.h"
#include "imgui_internal.h"     // ImGui::GetInputTextState(), ImTextCharFromUtf8()
#include <stddef.h>             // offsetof()
#include <string.h>
#include <time.h>

// xsdk
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"
#include "xsdk/xam.h"
#include "imgui_impl_xsdk_shaders.h" // From xsdk/imgui_impl_xsdk.vert and .frag, see docs/README-xsdk.md.

// Frames whose vertices and indices may be in flight at once, a slot each.
static const int        IMGUI_IMPL_XSDK_SLOTS = 3;
// A draw takes up to 65535 indices (xsdk/gpu.h); this many keeps each piece whole triangles starting on 4 bytes.
static const unsigned   IMGUI_IMPL_XSDK_DRAW_INDICES = 65532;
// xsdk_gpu_alloc() gives whole 64 KiB pages (xsdk:docs/GUIDE.md, "Memory the GPU reads").
static const uint32_t   IMGUI_IMPL_XSDK_PAGE_BYTES = 64 * 1024;
// The Guide's first opening held a frame up 2.7 s (xsdk:samples/triangle/main.c, BEGIN_TIMEOUT_MS).
static const uint32_t   IMGUI_IMPL_XSDK_TIMEOUT_MS = 5000;
static const int        IMGUI_IMPL_XSDK_KEYBOARD_CHARS = 256;
// As imgui_impl_sdl3.cpp takes a stick's dead zone.
static const int        IMGUI_IMPL_XSDK_THUMB_DEAD_ZONE = 8000;

// Vertices then indices of one frame, written again once the GPU is past Fence.
struct ImGui_ImplXsdk_Slot
{
    volatile uint32_t*  Memory;
    uint32_t            SizeBytes;
    uint32_t            Fence;
};

struct ImGui_ImplXsdk_Data
{
    XsdkShader              VertexShader;
    XsdkShader              PixelShader;
    XsdkSamplerState        SamplerLinear;
    XsdkSamplerState        SamplerNearest;
    const XsdkSamplerState* Sampler;
    const XsdkTexture*      BoundTexture;
    XsdkBlendState          Blend;
    XsdkDepthStencilState   DepthStencil;
    XsdkRasterState         Raster;
    ImGui_ImplXsdk_Slot     Slots[IMGUI_IMPL_XSDK_SLOTS];
    int                     SlotIndex;
    uint32_t                LastFence;      // The last frame's, which a texture waits for before it changes.
    double                  Time;
    bool                    GamepadGiven;

    ImGui_ImplXsdk_Data()   { memset((void*)this, 0, sizeof(*this)); }
};

// XAM's keyboard is the console's one, and XAM writes into the overlapped and the text whenever it finishes, so they
// outlive any context (xsdk:docs/GUIDE.md, "Saves, the user and XAM's dialogs").
struct ImGui_ImplXsdk_Keyboard
{
    XAM_OVERLAPPED      Overlapped;
    uint16_t            Default[IMGUI_IMPL_XSDK_KEYBOARD_CHARS];
    uint16_t            Text[IMGUI_IMPL_XSDK_KEYBOARD_CHARS];
    bool                Open;
    bool                Held;           // The controller reads as released until the button that closed it is let go.
    ImGuiID             Field;          // The text field it was opened for, until that field is left.
};
static ImGui_ImplXsdk_Keyboard g_Keyboard;

// Backend data stored in io.BackendRendererUserData to allow support for multiple Dear ImGui contexts
// It is STRONGLY preferred that you use docking branch with multi-viewports (== single Dear ImGui context + multiple windows) instead of multiple Dear ImGui contexts.
static ImGui_ImplXsdk_Data* ImGui_ImplXsdk_GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplXsdk_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

static bool ImGui_ImplXsdk_Wait(uint32_t fence)
{
    if (fence == 0)
        return true;
    const NTSTATUS status = xsdk_gpu_wait(fence, IMGUI_IMPL_XSDK_TIMEOUT_MS);
    if (!NT_SUCCESS(status))
        xsdk_log_write("imgui_impl_xsdk: xsdk_gpu_wait(%u) failed: 0x%08X", (unsigned)fence, (unsigned)status);
    return NT_SUCCESS(status);
}

// Functions
static void ImGui_ImplXsdk_SetupRenderState(ImDrawData* draw_data)
{
    ImGui_ImplXsdk_Data* bd = ImGui_ImplXsdk_GetBackendData();
    const float width = draw_data->DisplaySize.x;
    const float height = draw_data->DisplaySize.y;

    // Setup viewport, shaders, alpha-blending, no face culling, no depth testing, bilinear sampling.
    xsdk_gpu_set_viewport(0.0f, 0.0f, width, height, 0.0f, 1.0f);
    xsdk_gpu_set_shaders(&bd->VertexShader, &bd->PixelShader);
    xsdk_built_shader_set_literals(&imgui_impl_xsdk_vert);
    xsdk_built_shader_set_literals(&imgui_impl_xsdk_frag);
    xsdk_gpu_set_blend(&bd->Blend);
    xsdk_gpu_set_depth_stencil(&bd->DepthStencil);
    xsdk_gpu_set_raster(&bd->Raster);
    bd->Sampler = &bd->SamplerLinear;
    bd->BoundTexture = nullptr;

    // imgui_impl_dx9.cpp's orthographic projection, its half pixel landing ImGui's pixels on D3D9's, whose pixel
    // centres xsdk takes (xsdk:docs/scope/57-text-and-ui.md): clip = (pos - (L, T)) * (2 / w, -2 / h) + (-1, 1).
    const float L = draw_data->DisplayPos.x + 0.5f;
    const float T = draw_data->DisplayPos.y + 0.5f;
    const float scale_offset[4] = { 2.0f / width, -2.0f / height, -1.0f - 2.0f * L / width, 1.0f + 2.0f * T / height };
    xsdk_gpu_set_vertex_constants(IMGUI_IMPL_XSDK_VERT_PUSH, scale_offset, IMGUI_IMPL_XSDK_VERT_PUSH_REGISTERS);
}

// Draw callbacks
static void ImGui_ImplXsdk_DrawCallback_ResetRenderState(const ImDrawList*, const ImDrawCmd*)   {} // Intentionally empty. Used as an identifier for rendering loop to call its code. Simpler to implement this way.
static void ImGui_ImplXsdk_DrawCallback_SetSamplerLinear(const ImDrawList*, const ImDrawCmd*)   { ImGui_ImplXsdk_Data* bd = ImGui_ImplXsdk_GetBackendData(); bd->Sampler = &bd->SamplerLinear; bd->BoundTexture = nullptr; }
static void ImGui_ImplXsdk_DrawCallback_SetSamplerNearest(const ImDrawList*, const ImDrawCmd*)  { ImGui_ImplXsdk_Data* bd = ImGui_ImplXsdk_GetBackendData(); bd->Sampler = &bd->SamplerNearest; bd->BoundTexture = nullptr; }

// The next slot, holding at least size_bytes, once the GPU is done with it.
static ImGui_ImplXsdk_Slot* ImGui_ImplXsdk_TakeSlot(ImGui_ImplXsdk_Data* bd, uint32_t size_bytes)
{
    ImGui_ImplXsdk_Slot* slot = &bd->Slots[bd->SlotIndex];
    bd->SlotIndex = (bd->SlotIndex + 1) % IMGUI_IMPL_XSDK_SLOTS;
    if (!ImGui_ImplXsdk_Wait(slot->Fence))
        return nullptr;
    if (slot->SizeBytes >= size_bytes)
        return slot;
    xsdk_gpu_free(slot->Memory);
    slot->SizeBytes = (size_bytes + IMGUI_IMPL_XSDK_PAGE_BYTES - 1) & ~(IMGUI_IMPL_XSDK_PAGE_BYTES - 1);
    slot->Memory = xsdk_gpu_alloc(slot->SizeBytes);
    if (slot->Memory != nullptr)
        return slot;
    xsdk_log_write("imgui_impl_xsdk: xsdk_gpu_alloc(%u) failed", (unsigned)slot->SizeBytes);
    slot->SizeBytes = 0;
    return nullptr;
}

// A command's indices start on 4 bytes, as the console has drawn lists only from there (xsdk:docs/GUIDE.md, "Memory
// the GPU reads").
static uint32_t ImGui_ImplXsdk_IndexBytes(const ImDrawCmd* pcmd)
{
    return (pcmd->ElemCount * (uint32_t)sizeof(ImDrawIdx) + 3) & ~3u;
}

// Render function.
void ImGui_ImplXsdk_RenderDrawData(ImDrawData* draw_data)
{
    // Avoid rendering when minimized
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
        return;

    ImGui_ImplXsdk_Data* bd = ImGui_ImplXsdk_GetBackendData();

    // Catch up with texture updates. Most of the times, the list will have 1 element with an OK status, aka nothing to do.
    // (This almost always points to ImGui::GetPlatformIO().Textures[] but is part of ImDrawData to allow overriding or disabling texture updates).
    if (draw_data->Textures != nullptr)
        for (ImTextureData* tex : *draw_data->Textures)
            if (tex->Status != ImTextureStatus_OK)
                ImGui_ImplXsdk_UpdateTexture(tex);
    if (draw_data->TotalVtxCount == 0)
        return;

    // Every list's vertices as ImGui wrote them, then every command's indices.
    const uint32_t vertex_bytes = (uint32_t)draw_data->TotalVtxCount * (uint32_t)sizeof(ImDrawVert);
    uint32_t index_bytes = 0;
    for (const ImDrawList* draw_list : draw_data->CmdLists)
        for (const ImDrawCmd& cmd : draw_list->CmdBuffer)
            index_bytes += ImGui_ImplXsdk_IndexBytes(&cmd);
    ImGui_ImplXsdk_Slot* slot = ImGui_ImplXsdk_TakeSlot(bd, vertex_bytes + index_bytes);
    if (slot == nullptr)
        return;

    // xsdk's memcpy() writes GPU memory in order, never as the 64-bit stores that fault there (xsdk:docs/GUIDE.md,
    // "Memory the GPU reads").
    unsigned char* memory = (unsigned char*)(uintptr_t)slot->Memory;
    const uint32_t base = xsdk_gpu_address(slot->Memory);
    uint32_t vtx_offset = 0;
    uint32_t idx_offset = vertex_bytes;
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        memcpy(memory + vtx_offset, draw_list->VtxBuffer.Data, (size_t)draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
        vtx_offset += (uint32_t)draw_list->VtxBuffer.Size * (uint32_t)sizeof(ImDrawVert);
        for (const ImDrawCmd& cmd : draw_list->CmdBuffer)
        {
            memcpy(memory + idx_offset, draw_list->IdxBuffer.Data + cmd.IdxOffset, (size_t)cmd.ElemCount * sizeof(ImDrawIdx));
            idx_offset += ImGui_ImplXsdk_IndexBytes(&cmd);
        }
    }
    xsdk_gpu_invalidate(base, vertex_bytes + index_bytes);

    // Setup desired state
    ImGui_ImplXsdk_SetupRenderState(draw_data);

    // Render command lists
    // (Because we merged all buffers into a single one, we maintain our own offset into them)
    const ImVec2 clip_off = draw_data->DisplayPos;
    const float width = draw_data->DisplaySize.x;
    const float height = draw_data->DisplaySize.y;
    uint32_t global_vtx_offset = 0;
    idx_offset = vertex_bytes;
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        unsigned int stream_vtx_offset = UINT32_MAX;
        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
            const uint32_t indices = base + idx_offset;
            idx_offset += ImGui_ImplXsdk_IndexBytes(pcmd);
            if (pcmd->UserCallback != nullptr)
            {
                // User callback, registered via ImDrawList::AddCallback(); it may set the stream and texture slot too.
                if (pcmd->UserCallback == ImGui_ImplXsdk_DrawCallback_ResetRenderState)
                    ImGui_ImplXsdk_SetupRenderState(draw_data);
                else
                    pcmd->UserCallback(draw_list, pcmd);
                stream_vtx_offset = UINT32_MAX;
                bd->BoundTexture = nullptr;
                continue;
            }

            // Project scissor/clipping rectangles into framebuffer space, inside it, as xsdk's scissor takes no less than 0.
            const ImVec2 clip_min(ImMax(pcmd->ClipRect.x - clip_off.x, 0.0f), ImMax(pcmd->ClipRect.y - clip_off.y, 0.0f));
            const ImVec2 clip_max(ImMin(pcmd->ClipRect.z - clip_off.x, width), ImMin(pcmd->ClipRect.w - clip_off.y, height));
            const uint32_t x0 = (uint32_t)clip_min.x, y0 = (uint32_t)clip_min.y;
            const uint32_t x1 = (uint32_t)ImMax(clip_max.x, 0.0f), y1 = (uint32_t)ImMax(clip_max.y, 0.0f);
            if (x1 <= x0 || y1 <= y0 || pcmd->ElemCount == 0)
                continue;
            xsdk_gpu_set_scissor(x0, y0, x1 - x0, y1 - y0);

            // Bind texture, Draw; none where its creation failed.
            const XsdkTexture* texture = (const XsdkTexture*)(intptr_t)pcmd->GetTexID();
            if (texture == nullptr)
                continue;
            if (texture != bd->BoundTexture)
            {
                xsdk_gpu_set_texture(IMGUI_IMPL_XSDK_FRAG_SAMPLER_0_0, texture, bd->Sampler);
                bd->BoundTexture = texture;
            }
            if (pcmd->VtxOffset != stream_vtx_offset)
            {
                // A vertex is 20 bytes, so the stream stays on 4 (imgui_impl_dx9.cpp's VtxOffset).
                stream_vtx_offset = pcmd->VtxOffset;
                const uint32_t first_bytes = (global_vtx_offset + pcmd->VtxOffset) * (uint32_t)sizeof(ImDrawVert);
                xsdk_gpu_set_vertices(0, base + first_bytes, (uint32_t)(draw_list->VtxBuffer.Size - (int)pcmd->VtxOffset) * (uint32_t)sizeof(ImDrawVert));
            }
            for (unsigned int done = 0; done < pcmd->ElemCount; done += IMGUI_IMPL_XSDK_DRAW_INDICES)
                xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, indices + done * (uint32_t)sizeof(ImDrawIdx),
                    ImMin(pcmd->ElemCount - done, IMGUI_IMPL_XSDK_DRAW_INDICES), sizeof(ImDrawIdx) == 4);
        }
        global_vtx_offset += (uint32_t)draw_list->VtxBuffer.Size;
    }
    xsdk_gpu_set_scissor(0, 0, (uint32_t)width, (uint32_t)height);
    slot->Fence = bd->LastFence = xsdk_gpu_fence();
}

void ImGui_ImplXsdk_UpdateTexture(ImTextureData* tex)
{
    ImGui_ImplXsdk_Data* bd = ImGui_ImplXsdk_GetBackendData();

    if (tex->Status == ImTextureStatus_WantCreate)
    {
        // Create and upload new texture to graphics system
        IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
        XsdkTexture* backend_tex = IM_NEW(XsdkTexture)();
        const NTSTATUS status = xsdk_texture_create(backend_tex, XSDK_FORMAT_A8R8G8B8, (uint32_t)tex->Width, (uint32_t)tex->Height, 1, true);
        if (!NT_SUCCESS(status))
        {
            xsdk_log_write("imgui_impl_xsdk: xsdk_texture_create(%dx%d) failed: 0x%08X", tex->Width, tex->Height, (unsigned)status);
            IM_DELETE(backend_tex);
            return;
        }
        // ImGui's RGBA32 pixels are ImU32 words, 0xAABBGGRR on the console (imgui.h, IM_COL32), so A8R8G8B8 reads
        // red and blue swapped.
        static const uint32_t channels[4] = { XSDK_CHANNEL_BLUE, XSDK_CHANNEL_GREEN, XSDK_CHANNEL_RED, XSDK_CHANNEL_ALPHA };
        xsdk_texture_swizzle(backend_tex, channels);
        xsdk_texture_upload(backend_tex, 0, tex->GetPixels(), (uint32_t)tex->GetPitch());
        xsdk_gpu_invalidate(backend_tex->address, backend_tex->size_bytes);

        // Store identifiers
        tex->SetTexID((ImTextureID)(intptr_t)backend_tex);
        tex->BackendUserData = backend_tex;
        tex->SetStatus(ImTextureStatus_OK);
    }
    else if (tex->Status == ImTextureStatus_WantUpdates)
    {
        // The changed rows whole, once the GPU is past the last frame, as uploads may not run while it reads (xsdk/texture.h).
        if (!ImGui_ImplXsdk_Wait(bd->LastFence))
            return;
        XsdkTexture* backend_tex = (XsdkTexture*)tex->BackendUserData;
        const ImTextureRect& r = tex->UpdateRect;
        xsdk_texture_upload_rows(backend_tex, 0, r.y, r.h, tex->GetPixelsAt(0, r.y), (uint32_t)tex->GetPitch());
        xsdk_gpu_invalidate(backend_tex->address, backend_tex->size_bytes);
        tex->SetStatus(ImTextureStatus_OK);
    }
    else if (tex->Status == ImTextureStatus_WantDestroy)
    {
        if (!ImGui_ImplXsdk_Wait(bd->LastFence))
            return;
        if (XsdkTexture* backend_tex = (XsdkTexture*)tex->BackendUserData)
        {
            xsdk_texture_free(backend_tex);
            IM_DELETE(backend_tex);
        }

        // Clear identifiers and mark as destroyed (in order to allow e.g. calling InvalidateDeviceObjects while running)
        tex->SetTexID(ImTextureID_Invalid);
        tex->BackendUserData = nullptr;
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

static bool ImGui_ImplXsdk_CreateDeviceObjects(ImGui_ImplXsdk_Data* bd)
{
    // ImDrawVert by location: 0 a position, 1 a colour and 2 a texture coordinate (xsdk:docs/GUIDE.md, "Shaders and
    // their constants"). An ImU32 colour's low byte is red (imgui.h, IM_COL32_R_SHIFT), which UBYTE4N reads as x.
    static const XsdkVertexElement layout[] =
    {
        { XSDK_USAGE_POSITION, 0, XSDK_VERTEX_FLOAT2,  0, (uint32_t)offsetof(ImDrawVert, pos), {}, false },
        { XSDK_USAGE_COLOR,    0, XSDK_VERTEX_UBYTE4N, 0, (uint32_t)offsetof(ImDrawVert, col), {}, false },
        { XSDK_USAGE_TEXCOORD, 0, XSDK_VERTEX_FLOAT2,  0, (uint32_t)offsetof(ImDrawVert, uv),  {}, false },
    };
    const uint32_t stride = (uint32_t)sizeof(ImDrawVert);
    NTSTATUS status = xsdk_shader_load(&bd->VertexShader, imgui_impl_xsdk_vert.container, imgui_impl_xsdk_vert.container_bytes);
    if (NT_SUCCESS(status))
        status = xsdk_shader_load(&bd->PixelShader, imgui_impl_xsdk_frag.container, imgui_impl_xsdk_frag.container_bytes);
    if (NT_SUCCESS(status))
        status = xsdk_shader_set_vertex_layout(&bd->VertexShader, layout, IM_ARRAYSIZE(layout), &stride);
    if (!NT_SUCCESS(status))
    {
        xsdk_log_write("imgui_impl_xsdk: loading the shaders failed: 0x%08X", (unsigned)status);
        if (bd->PixelShader.microcode != nullptr)
            xsdk_shader_free(&bd->PixelShader);
        if (bd->VertexShader.microcode != nullptr)
            xsdk_shader_free(&bd->VertexShader);
        return false;
    }

    XsdkSampler sampler = {};
    sampler.mag_filter = sampler.min_filter = XSDK_FILTER_LINEAR;
    sampler.address_u = sampler.address_v = sampler.address_w = XSDK_ADDRESS_CLAMP;
    xsdk_texture_sampler_state(&bd->SamplerLinear, &sampler);
    sampler.mag_filter = sampler.min_filter = XSDK_FILTER_POINT;
    xsdk_texture_sampler_state(&bd->SamplerNearest, &sampler);

    // imgui_impl_dx9.cpp's blending: colour by source alpha, alpha kept for the output.
    XsdkBlend blend = {};
    blend.color_source = XSDK_BLEND_SRC_ALPHA;
    blend.color_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA;
    blend.color_op = XSDK_BLEND_OP_ADD;
    blend.alpha_source = XSDK_BLEND_ONE;
    blend.alpha_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA;
    blend.alpha_op = XSDK_BLEND_OP_ADD;
    blend.write_mask = XSDK_WRITE_ALL;
    xsdk_gpu_blend_state(&bd->Blend, &blend);
    const XsdkDepthStencil depth_stencil = {};
    xsdk_gpu_depth_stencil_state(&bd->DepthStencil, &depth_stencil);
    XsdkRaster raster = {};
    raster.cull = XSDK_CULL_NONE;
    raster.fill = XSDK_FILL_SOLID;
    xsdk_gpu_raster_state(&bd->Raster, &raster);
    return true;
}

static void ImGui_ImplXsdk_InvalidateDeviceObjects(ImGui_ImplXsdk_Data* bd)
{
    // A GPU that never got past the last frame may still read what's below, so it's kept.
    if (!ImGui_ImplXsdk_Wait(bd->LastFence))
        return;

    // Destroy all textures
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1)
        {
            tex->SetStatus(ImTextureStatus_WantDestroy);
            ImGui_ImplXsdk_UpdateTexture(tex);
        }
    for (ImGui_ImplXsdk_Slot& slot : bd->Slots)
        xsdk_gpu_free(slot.Memory);
    xsdk_shader_free(&bd->PixelShader);
    xsdk_shader_free(&bd->VertexShader);
}

// UTF-8 to UTF-16, past U+FFFF as surrogate pairs, ended by a 0 within out_chars.
static void ImGui_ImplXsdk_Utf8ToUtf16(uint16_t* out, int out_chars, const char* in, const char* in_end)
{
    int n = 0;
    while (in < in_end && *in != 0)
    {
        unsigned int c;
        in += ImTextCharFromUtf8(&c, in, in_end);
        if (c < 0x10000 && n + 1 < out_chars)
            out[n++] = (uint16_t)c;
        else if (c >= 0x10000 && n + 2 < out_chars)
        {
            out[n++] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
        }
        else
            break;
    }
    out[n] = 0;
}

// An accepted keyboard completes with 0 and extended error 0 (xsdk:docs/HARDWARE.md, "XAM's dialogs"); Xenia's
// cancel sets extended error ERROR_CANCELLED (xenia:src/xenia/kernel/xam/xam_ui.cc:438-441), and what the console
// gives is open (xsdk:docs/PARITY.md), so anything else leaves the field as it was.
static void ImGui_ImplXsdk_EndKeyboard(uint32_t result, uint32_t extended_error)
{
    ImGui_ImplXsdk_Keyboard& kb = g_Keyboard;
    kb.Open = false;
    if (ImGui::GetActiveID() != kb.Field)
        return;
    if (result != ERROR_SUCCESS || extended_error != ERROR_SUCCESS)
    {
        ImGui::ClearActiveID();
        return;
    }

    // ImGui's own events: select all, delete, the text, then Ctrl+Enter, which ends single and multi-line fields alike.
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddKeyEvent(ImGuiKey_Delete, true);
    io.AddKeyEvent(ImGuiKey_Delete, false);
    for (int i = 0; i < IMGUI_IMPL_XSDK_KEYBOARD_CHARS && kb.Text[i] != 0; i++)
        io.AddInputCharacterUTF16(kb.Text[i]);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
}

static void ImGui_ImplXsdk_OpenKeyboard(ImGuiInputTextState* state)
{
    ImGui_ImplXsdk_Keyboard& kb = g_Keyboard;
    ImGui_ImplXsdk_Utf8ToUtf16(kb.Default, IMGUI_IMPL_XSDK_KEYBOARD_CHARS, state->TextA.Data, state->TextA.Data + state->TextLen);
    memset(kb.Text, 0, sizeof(kb.Text));
    kb.Held = true;
#ifdef IMGUI_IMPL_XSDK_CHECK
    // As XAM would, without a person to type: ImGui_ImplXsdk_CompleteKeyboard() ends it.
    const uint32_t result = ERROR_IO_PENDING;
#else
    // Controller 1's user, the one this backend reads.
    const uint32_t chars = (uint32_t)ImMin(state->BufCapacity, IMGUI_IMPL_XSDK_KEYBOARD_CHARS);
    const uint32_t result = xsdk_xam_keyboard(0, (const uint16_t*)u"Dear ImGui", (const uint16_t*)u"Enter text",
        kb.Default, kb.Text, chars, &kb.Overlapped);
#endif
    if (result == ERROR_IO_PENDING)
        kb.Open = true;
    else
        ImGui_ImplXsdk_EndKeyboard(result, kb.Overlapped.extended_error); // XAM refuses one while its UI is up (xsdk:docs/HARDWARE.md, "XAM's dialogs").
}

// The keyboard opens as a writable text field becomes active, once for each time it does.
static void ImGui_ImplXsdk_UpdateKeyboard()
{
    ImGui_ImplXsdk_Keyboard& kb = g_Keyboard;
    if (kb.Open)
    {
#ifndef IMGUI_IMPL_XSDK_CHECK
        // Looked at once a frame, as XAM's dialogs show only while the title presents (xsdk:docs/GUIDE.md, "Saves, the
        // user and XAM's dialogs").
        const uint32_t result = xsdk_xam_wait(&kb.Overlapped, 0, nullptr);
        if (result != ERROR_IO_INCOMPLETE)
            ImGui_ImplXsdk_EndKeyboard(result, kb.Overlapped.extended_error);
#endif
        return;
    }
    const ImGuiID id = ImGui::GetActiveID();
    ImGuiInputTextState* state = ImGui::GetInputTextState(id);
    if (state == nullptr || (state->Flags & ImGuiInputTextFlags_ReadOnly) != 0)
        kb.Field = 0;
    else if (id != kb.Field)
    {
        kb.Field = id;
        ImGui_ImplXsdk_OpenKeyboard(state);
    }
}

#ifdef IMGUI_IMPL_XSDK_CHECK
bool ImGui_ImplXsdk_CompleteKeyboard(uint32_t result, uint32_t extended_error, const uint16_t* text)
{
    ImGui_ImplXsdk_Keyboard& kb = g_Keyboard;
    if (!kb.Open)
        return false;
    int n = 0;
    for (; n < IMGUI_IMPL_XSDK_KEYBOARD_CHARS - 1 && text[n] != 0; n++)
        kb.Text[n] = text[n];
    kb.Text[n] = 0;
    ImGui_ImplXsdk_EndKeyboard(result, extended_error);
    return true;
}
#endif

// Mapped as imgui_impl_win32.cpp maps XInput's, whose layout INPUT_GAMEPAD has (xenia:src/xenia/hid/input.h:57-72).
static void ImGui_ImplXsdk_UpdateGamepad(const INPUT_GAMEPAD* gamepad)
{
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    static const INPUT_GAMEPAD released = {};
    if (g_Keyboard.Held && !g_Keyboard.Open && gamepad->buttons == 0)
        g_Keyboard.Held = false;
    if (g_Keyboard.Held)
        gamepad = &released;

    #define IM_SATURATE(V)                      (V < 0.0f ? 0.0f : V > 1.0f ? 1.0f : V)
    #define MAP_BUTTON(KEY_NO, BUTTON_ENUM)     { io.AddKeyEvent(KEY_NO, (gamepad->buttons & BUTTON_ENUM) != 0); }
    #define MAP_ANALOG(KEY_NO, VALUE, V0, V1)   { float vn = (float)(VALUE - V0) / (float)(V1 - V0); io.AddKeyAnalogEvent(KEY_NO, vn > 0.10f, IM_SATURATE(vn)); }
    MAP_BUTTON(ImGuiKey_GamepadStart,           INPUT_GAMEPAD_START);
    MAP_BUTTON(ImGuiKey_GamepadBack,            INPUT_GAMEPAD_BACK);
    MAP_BUTTON(ImGuiKey_GamepadFaceLeft,        INPUT_GAMEPAD_X);
    MAP_BUTTON(ImGuiKey_GamepadFaceRight,       INPUT_GAMEPAD_B);
    MAP_BUTTON(ImGuiKey_GamepadFaceUp,          INPUT_GAMEPAD_Y);
    MAP_BUTTON(ImGuiKey_GamepadFaceDown,        INPUT_GAMEPAD_A);
    MAP_BUTTON(ImGuiKey_GamepadDpadLeft,        INPUT_GAMEPAD_DPAD_LEFT);
    MAP_BUTTON(ImGuiKey_GamepadDpadRight,       INPUT_GAMEPAD_DPAD_RIGHT);
    MAP_BUTTON(ImGuiKey_GamepadDpadUp,          INPUT_GAMEPAD_DPAD_UP);
    MAP_BUTTON(ImGuiKey_GamepadDpadDown,        INPUT_GAMEPAD_DPAD_DOWN);
    MAP_BUTTON(ImGuiKey_GamepadL1,              INPUT_GAMEPAD_LEFT_SHOULDER);
    MAP_BUTTON(ImGuiKey_GamepadR1,              INPUT_GAMEPAD_RIGHT_SHOULDER);
    MAP_ANALOG(ImGuiKey_GamepadL2,              gamepad->left_trigger, 0, 255);
    MAP_ANALOG(ImGuiKey_GamepadR2,              gamepad->right_trigger, 0, 255);
    MAP_BUTTON(ImGuiKey_GamepadL3,              INPUT_GAMEPAD_LEFT_THUMB);
    MAP_BUTTON(ImGuiKey_GamepadR3,              INPUT_GAMEPAD_RIGHT_THUMB);
    MAP_ANALOG(ImGuiKey_GamepadLStickLeft,      gamepad->thumb_lx, -IMGUI_IMPL_XSDK_THUMB_DEAD_ZONE, -32768);
    MAP_ANALOG(ImGuiKey_GamepadLStickRight,     gamepad->thumb_lx, +IMGUI_IMPL_XSDK_THUMB_DEAD_ZONE, +32767);
    MAP_ANALOG(ImGuiKey_GamepadLStickUp,        gamepad->thumb_ly, +IMGUI_IMPL_XSDK_THUMB_DEAD_ZONE, +32767);
    MAP_ANALOG(ImGuiKey_GamepadLStickDown,      gamepad->thumb_ly, -IMGUI_IMPL_XSDK_THUMB_DEAD_ZONE, -32768);
    MAP_ANALOG(ImGuiKey_GamepadRStickLeft,      gamepad->thumb_rx, -IMGUI_IMPL_XSDK_THUMB_DEAD_ZONE, -32768);
    MAP_ANALOG(ImGuiKey_GamepadRStickRight,     gamepad->thumb_rx, +IMGUI_IMPL_XSDK_THUMB_DEAD_ZONE, +32767);
    MAP_ANALOG(ImGuiKey_GamepadRStickUp,        gamepad->thumb_ry, +IMGUI_IMPL_XSDK_THUMB_DEAD_ZONE, +32767);
    MAP_ANALOG(ImGuiKey_GamepadRStickDown,      gamepad->thumb_ry, -IMGUI_IMPL_XSDK_THUMB_DEAD_ZONE, -32768);
    #undef MAP_BUTTON
    #undef MAP_ANALOG
    #undef IM_SATURATE
}

void ImGui_ImplXsdk_ProcessGamepad(const INPUT_GAMEPAD* gamepad)
{
    ImGui_ImplXsdk_Data* bd = ImGui_ImplXsdk_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplXsdk_Init()?");
    bd->GamepadGiven = true;
    ImGui_ImplXsdk_UpdateGamepad(gamepad);
}

void ImGui_ImplXsdk_NewFrame(uint32_t width, uint32_t height)
{
    ImGui_ImplXsdk_Data* bd = ImGui_ImplXsdk_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplXsdk_Init()?");
    ImGuiIO& io = ImGui::GetIO();

    // Setup display size (every frame to accommodate for window resizing)
    io.DisplaySize = ImVec2((float)width, (float)height);

    // Setup time step
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const double current_time = (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
    io.DeltaTime = bd->Time > 0.0 ? (float)ImMax(current_time - bd->Time, 0.000001) : 1.0f / 60.0f;
    bd->Time = current_time;

    // Keyboard first, as it holds the controller while it's open.
    ImGui_ImplXsdk_UpdateKeyboard();
    if (!bd->GamepadGiven)
    {
        INPUT_STATE state = {};
        if (XamInputGetState(0, 0, &state) == ERROR_SUCCESS)
            ImGui_ImplXsdk_UpdateGamepad(&state.gamepad);
        else
            io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    }
    bd->GamepadGiven = false;
}

bool ImGui_ImplXsdk_Init()
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    IM_ASSERT(io.BackendPlatformUserData == nullptr && "Already initialized a platform backend!");

    ImGui_ImplXsdk_Data* bd = IM_NEW(ImGui_ImplXsdk_Data)();
    if (!ImGui_ImplXsdk_CreateDeviceObjects(bd))
    {
        IM_DELETE(bd);
        return false;
    }

    // Setup backend capabilities flags
    io.BackendRendererUserData = io.BackendPlatformUserData = (void*)bd;
    io.BackendRendererName = io.BackendPlatformName = "imgui_impl_xsdk";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;   // We can honor ImGuiPlatformIO::Textures[] requests during render.

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Renderer_TextureMaxWidth = platform_io.Renderer_TextureMaxHeight = XSDK_TEXTURE_MAX_SIZE;
    platform_io.DrawCallback_ResetRenderState = ImGui_ImplXsdk_DrawCallback_ResetRenderState;
    platform_io.DrawCallback_SetSamplerLinear = ImGui_ImplXsdk_DrawCallback_SetSamplerLinear;
    platform_io.DrawCallback_SetSamplerNearest = ImGui_ImplXsdk_DrawCallback_SetSamplerNearest;
    return true;
}

void ImGui_ImplXsdk_Shutdown()
{
    ImGui_ImplXsdk_Data* bd = ImGui_ImplXsdk_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    ImGui_ImplXsdk_InvalidateDeviceObjects(bd);
    io.BackendRendererName = io.BackendPlatformName = nullptr;
    io.BackendRendererUserData = io.BackendPlatformUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_HasGamepad);
    platform_io.ClearRendererHandlers();
    IM_DELETE(bd);
}

//-----------------------------------------------------------------------------

#endif // #ifndef IMGUI_DISABLE
