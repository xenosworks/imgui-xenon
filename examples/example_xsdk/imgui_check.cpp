// dear imgui: checks of imgui_impl_xsdk on the Xbox 360 (docs/README-xsdk.md, xsdk:docs/scope/57-text-and-ui.md)
// Each check builds its frames from fixed inputs, draws them through the backend, resolves them into a front buffer and
// reads it back; it logs "PASS: <check>" or "FAIL: <check>", then "tests: <passed> passed, <failed> failed".
// breaks/ breaks each one on purpose. The backend is built with IMGUI_IMPL_XSDK_CHECK, which stands in for a person at
// XAM's keyboard.

#include "imgui.h"
#include "imgui_impl_xsdk.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"

static const uint32_t   SPLASH_MS = 2000;
static const uint32_t   BUFFERS = 2;
static const uint32_t   SWAP_INTERVAL = 1;
// The Guide's first opening held a frame up 2.7 s (xsdk:samples/triangle/main.c).
static const uint32_t   BEGIN_TIMEOUT_MS = 5000;
static const uint32_t   FENCE_TIMEOUT_MS = 1000;
static const float      FRAME_SECONDS = 1.0f / 60.0f;
static const uint32_t   MAX_WIDTH = 1280;
static const uint32_t   CLEAR_ARGB = 0xFF000000;
static const int        MAX_LOGGED = 4;
// Frames for a window to settle, and for events to trickle through ImGui's queue.
static const int        SETTLE_FRAMES = 3;
static const int        TRICKLE_FRAMES = 20;

// Each channel 0 or 255, so exact however the GPU rounds, and red apart from blue.
static const ImU32      WINDOW_COLOR = IM_COL32(255, 255, 0, 255);
static const uint32_t   WINDOW_ARGB = 0xFFFFFF00;
static const ImU32      CLIP_PARENT_COLOR = IM_COL32(0, 0, 255, 255);
static const uint32_t   CLIP_PARENT_ARGB = 0xFF0000FF;
static const ImU32      CLIP_RECT_COLOR = IM_COL32(255, 0, 0, 255);
static const uint32_t   CLIP_RECT_ARGB = 0xFFFF0000;
static const float      CLIP_SCROLL = 50.0f;
static const uint32_t   IMAGE_TEXELS[16] =
{
    0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF, 0xFF808080,
    0xFF800000, 0xFF008000, 0xFF000080, 0xFF808000, 0xFF800080, 0xFF008080, 0xFF404040, 0xFFC0C0C0,
};
static const int        IMAGE_TEXEL_PIXELS = 16;
// Glyphs a space apart, so no two boxes overlap.
static const char       FONT_TEXT[] = "W # M @ g";
static const float      FONT_NEW_SIZE = 29.0f;
static const int        MESH_RECTS = 17000;
static const int        MESH_COLUMNS = 160;
static const float      MESH_PITCH = 4.0f;
static const float      MESH_SIZE = 3.0f;
static const float      MESH_TOP = 40.0f;
static const ImU32      MESH_FIRST_COLOR = IM_COL32(255, 0, 0, 255);
static const uint32_t   MESH_FIRST_ARGB = 0xFFFF0000;
static const ImU32      MESH_LAST_COLOR = IM_COL32(0, 255, 0, 255);
static const uint32_t   MESH_LAST_ARGB = 0xFF00FF00;
static const ImU32      MESH_OTHER_COLOR = IM_COL32(128, 128, 128, 255);
static const char       ENTRY_BEFORE[] = "before";
static const uint16_t*  ENTRY_ACCEPTED = (const uint16_t*)u"accepted \u00C5\u00DF";
static const char       ENTRY_ACCEPTED_UTF8[] = "accepted \xC3\x85\xC3\x9F";
static const uint16_t*  ENTRY_CANCELLED = (const uint16_t*)u"cancelled";
// Xenia's cancelled keyboard: result 0, extended error ERROR_CANCELLED (xenia:src/xenia/kernel/xam/xam_ui.cc:438-441).
static const uint32_t   ERROR_CANCELLED = 0x4C7;

static uint32_t         g_Width;
static uint32_t         g_Height;
static int              g_Passed;
static int              g_Failed;
static uint32_t         g_Address;      // The front buffer the last frame went into.
static XsdkTexture      g_Frame;        // That buffer as a texture, to read rows of.
static uint32_t         g_Row[MAX_WIDTH];
static uint32_t         g_RowY = UINT32_MAX;
static XsdkTexture      g_Image;
static ImTextureStatus  g_AtlasStatus;  // The font atlas's, as the last frame's draw found it.

static void Check(const char* name, bool passed)
{
    xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
    if (passed)
        g_Passed++;
    else
        g_Failed++;
}

// Builds a frame from fixed inputs, the controller released unless gamepad says, draws it into the next front buffer
// and waits for the GPU; shows it unless keep, which leaves it to read until EndFrame().
static bool Frame(void (*ui)(), const INPUT_GAMEPAD* gamepad, bool keep)
{
    static const INPUT_GAMEPAD released = {};
    ImGui_ImplXsdk_ProcessGamepad(gamepad != nullptr ? gamepad : &released);
    ImGui_ImplXsdk_NewFrame(g_Width, g_Height);
    ImGui::GetIO().DeltaTime = FRAME_SECONDS;
    ImGui::NewFrame();
    ui();
    ImGui::Render();
    g_AtlasStatus = ImGui::GetIO().Fonts->TexData->Status;

    NTSTATUS status = xsdk_present_begin(&g_Address, BEGIN_TIMEOUT_MS);
    if (!NT_SUCCESS(status))
    {
        xsdk_log_write("imgui_check: xsdk_present_begin failed: 0x%08X", (unsigned)status);
        return false;
    }
    xsdk_gpu_set_target(g_Width, g_Height);
    ImGui_ImplXsdk_RenderDrawData(ImGui::GetDrawData());
    xsdk_gpu_resolve(g_Address, g_Width, g_Height, CLEAR_ARGB);
    status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
    if (!NT_SUCCESS(status))
        xsdk_log_write("imgui_check: xsdk_gpu_wait failed: 0x%08X", (unsigned)status);
    g_RowY = UINT32_MAX;
    if (!keep)
        xsdk_present_end();
    return NT_SUCCESS(status);
}

static void EndFrame()
{
    xsdk_present_end();
}

// Pixel (x, y) of the kept frame as 0xAARRGGBB: the front buffer is a tiled A8R8G8B8 texture whose words hold B, G, R
// then A (xsdk:docs/GUIDE.md, "Textures, render targets and resolves"). A row at a time, as reading it is slow.
static uint32_t Pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= (int)g_Width || y >= (int)g_Height)
        return 0;
    if ((uint32_t)y != g_RowY)
    {
        xsdk_texture_attach(&g_Frame, xsdk_present_pixels(g_Address));
        xsdk_texture_download_rows(&g_Frame, 0, (uint32_t)y, 1, g_Row, sizeof(g_Row));
        g_RowY = (uint32_t)y;
    }
    return __builtin_bswap32(g_Row[x]);
}

// Whether pixel (x, y) holds want's red, green and blue, each within 1, as a colour converts once on the way to the 8-bit
// target (xsdk:tests/cmake/cmake_check.cpp, TOLERANCE); logs the first few that don't.
static bool PixelIs(const char* check, int x, int y, uint32_t want, int* wrong)
{
    const uint32_t held = Pixel(x, y);
    bool near = true;
    for (int shift = 0; shift < 24; shift += 8)
    {
        const int a = (int)(held >> shift & 0xFF), b = (int)(want >> shift & 0xFF);
        near = near && a - b <= 1 && b - a <= 1;
    }
    if (near)
        return true;
    if ((*wrong)++ < MAX_LOGGED)
        xsdk_log_write("%s: pixel (%d, %d) held %08X, not %08X", check, x, y, (unsigned)held, (unsigned)want);
    return false;
}

// A fixed window of no border or rounding, in colour.
static void BeginFixedWindow(const char* name, ImVec2 pos, ImVec2 size, ImU32 color, ImGuiWindowFlags flags)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, color);
    ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

//-----------------------------------------------------------------------------
// imgui_window: a window's edges land on whole pixels, through the projection's half pixel.
//-----------------------------------------------------------------------------

static const ImVec2 WINDOW_POS(100.0f, 80.0f);
static const ImVec2 WINDOW_SIZE(300.0f, 200.0f);
// Whole-pixel edges sit on pixel centres without the half pixel, which a top-left fill rule fills alike, so a rectangle
// a quarter pixel off them shows the half pixel whatever the rule: ImGui's (120.25, 100.25) to (160.25, 140.25) covers
// pixels 120 to 159 across and 100 to 139 down with it, 121 to 160 and 101 to 140 without.
static const ImVec2 QUARTER_MIN(120.25f, 100.25f);
static const ImVec2 QUARTER_MAX(160.25f, 140.25f);
static const ImU32 QUARTER_COLOR = IM_COL32(0, 255, 255, 255);
static const uint32_t QUARTER_ARGB = 0xFF00FFFF;

static void WindowUi()
{
    BeginFixedWindow("imgui_window", WINDOW_POS, WINDOW_SIZE, WINDOW_COLOR, ImGuiWindowFlags_NoInputs);
    // A bare quad: AddRectFilled() gives off-pixel rectangles an anti-aliased edge.
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->PrimReserve(6, 4);
    draw_list->PrimRect(QUARTER_MIN, QUARTER_MAX, QUARTER_COLOR);
    ImGui::End();
}

static bool Inside(int x, int y, int x0, int y0, int x1, int y1)
{
    return x >= x0 && x < x1 && y >= y0 && y < y1;
}

static void CheckWindow()
{
    const char* name = "imgui_window: a square, unbordered window at (100, 80), 300x200: its edge pixels and inside "
        "its colour, the pixels beside it the clear colour, and a rectangle in it a quarter pixel off whole ones "
        "on the pixels whose centres it holds";
    bool drawn = true;
    for (int i = 0; i < SETTLE_FRAMES; i++)
        drawn = Frame(WindowUi, nullptr, i == SETTLE_FRAMES - 1) && drawn;
    int wrong = 0;
    const int x0 = (int)WINDOW_POS.x, y0 = (int)WINDOW_POS.y;
    const int x1 = x0 + (int)WINDOW_SIZE.x, y1 = y0 + (int)WINDOW_SIZE.y;
    const int qx0 = (int)QUARTER_MIN.x, qy0 = (int)QUARTER_MIN.y, qx1 = (int)QUARTER_MAX.x, qy1 = (int)QUARTER_MAX.y;
    for (int y = y0 - 1; y <= y1; y++)
        for (int x = x0 - 1; x <= x1; x++)
        {
            const uint32_t want = Inside(x, y, qx0, qy0, qx1, qy1) ? QUARTER_ARGB : Inside(x, y, x0, y0, x1, y1) ? WINDOW_ARGB : CLEAR_ARGB;
            PixelIs("imgui_window", x, y, want, &wrong);
        }
    EndFrame();
    Check(name, drawn && wrong == 0);
}

//-----------------------------------------------------------------------------
// imgui_clip: a scrolled child's clip rectangle reaches the scissor.
//-----------------------------------------------------------------------------

static ImVec2 g_ClipMin, g_ClipMax;     // The child's clip rectangle, as its draw list had it.
static ImVec2 g_RectMin, g_RectMax;     // The rectangle drawn past it.
static float g_ClipScrollY;

static void ClipUi()
{
    BeginFixedWindow("imgui_clip", WINDOW_POS, WINDOW_SIZE, CLIP_PARENT_COLOR, ImGuiWindowFlags_NoInputs);
    ImGui::SetCursorPos(ImVec2(50.0f, 50.0f));
    ImGui::BeginChild("clip", ImVec2(100.0f, 100.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs);
    ImGui::SetScrollY(CLIP_SCROLL);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(100.0f, 300.0f));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    g_ClipMin = draw_list->GetClipRectMin();
    g_ClipMax = draw_list->GetClipRectMax();
    g_RectMin = ImVec2(origin.x - 30.0f, origin.y + 40.0f);
    g_RectMax = ImVec2(origin.x + 130.0f, origin.y + 190.0f);
    draw_list->AddRectFilled(g_RectMin, g_RectMax, CLIP_RECT_COLOR);
    g_ClipScrollY = ImGui::GetScrollY();
    ImGui::EndChild();
    ImGui::End();
}

static void CheckClip()
{
    bool drawn = true;
    for (int i = 0; i < SETTLE_FRAMES; i++)
        drawn = Frame(ClipUi, nullptr, i == SETTLE_FRAMES - 1) && drawn;
    // The rectangle runs past the clip on every side, all of it over the parent.
    const bool past = g_RectMin.x < g_ClipMin.x && g_RectMin.y < g_ClipMin.y && g_RectMax.x > g_ClipMax.x && g_RectMax.y > g_ClipMax.y;
    int wrong = 0;
    for (int y = (int)g_RectMin.y - 1; y <= (int)g_RectMax.y; y++)
        for (int x = (int)g_RectMin.x - 1; x <= (int)g_RectMax.x; x++)
        {
            const bool clipped_in = x >= (int)g_ClipMin.x && x < (int)g_ClipMax.x && y >= (int)g_ClipMin.y && y < (int)g_ClipMax.y;
            PixelIs("imgui_clip", x, y, clipped_in ? CLIP_RECT_ARGB : CLIP_PARENT_ARGB, &wrong);
        }
    EndFrame();
    char name[200];
    snprintf(name, sizeof(name), "imgui_clip: a rectangle past a child scrolled %.0f pixels, clipped to (%.0f, %.0f)-(%.0f, "
        "%.0f): drawn inside the clip alone", g_ClipScrollY, g_ClipMin.x, g_ClipMin.y, g_ClipMax.x, g_ClipMax.y);
    Check(name, drawn && past && g_ClipScrollY == CLIP_SCROLL && wrong == 0);
}

//-----------------------------------------------------------------------------
// imgui_image: a command's own texture is bound.
//-----------------------------------------------------------------------------

static ImVec2 g_ImageMin;

static void ImageUi()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    BeginFixedWindow("imgui_image", WINDOW_POS, ImVec2(64.0f, 64.0f), CLIP_PARENT_COLOR, ImGuiWindowFlags_NoInputs);
    ImGui::PopStyleVar();
    // Point sampled, so each pixel is one texel.
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
    ImGui::Image((ImTextureID)(intptr_t)&g_Image, ImVec2(4.0f * IMAGE_TEXEL_PIXELS, 4.0f * IMAGE_TEXEL_PIXELS));
    g_ImageMin = ImGui::GetItemRectMin();
    draw_list->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
    ImGui::End();
}

static void CheckImage()
{
    const char* name = "imgui_image: ImGui::Image of a 4x4 xsdk texture at 64x64: each block's centre its texel";
    bool made = NT_SUCCESS(xsdk_texture_create(&g_Image, XSDK_FORMAT_A8R8G8B8, 4, 4, 1, false));
    if (made)
    {
        xsdk_texture_upload(&g_Image, 0, IMAGE_TEXELS, 4 * sizeof(uint32_t));
        xsdk_gpu_invalidate(g_Image.address, g_Image.size_bytes);
    }
    bool drawn = made;
    for (int i = 0; made && i < SETTLE_FRAMES; i++)
        drawn = Frame(ImageUi, nullptr, i == SETTLE_FRAMES - 1) && drawn;
    int wrong = 0;
    for (int i = 0; made && i < 16; i++)
    {
        const int x = (int)g_ImageMin.x + (i % 4) * IMAGE_TEXEL_PIXELS + IMAGE_TEXEL_PIXELS / 2;
        const int y = (int)g_ImageMin.y + (i / 4) * IMAGE_TEXEL_PIXELS + IMAGE_TEXEL_PIXELS / 2;
        PixelIs("imgui_image", x, y, IMAGE_TEXELS[i], &wrong);
    }
    if (made)
        EndFrame();
    Check(name, drawn && wrong == 0);
}

//-----------------------------------------------------------------------------
// imgui_font: ImGui's RGBA32 atlas reads right, and its updates arrive.
//-----------------------------------------------------------------------------

static float g_FontSize;                // 0 draws at ImGui's default size.
static int g_FontFirstVtx, g_FontEndVtx;

static void FontUi()
{
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    g_FontFirstVtx = draw_list->VtxBuffer.Size;
    draw_list->AddText(ImGui::GetFont(), g_FontSize > 0.0f ? g_FontSize : ImGui::GetFontSize(), WINDOW_POS, IM_COL32_WHITE, FONT_TEXT);
    g_FontEndVtx = draw_list->VtxBuffer.Size;
}

// Every pixel of each glyph's box is its texel's alpha as white over black; returns the wrong ones, and counts the solid
// and empty texels, so a box all blank or all solid can't pass.
static int CheckGlyphs(const char* check, int* solid, int* empty)
{
    const ImTextureData* tex = ImGui::GetIO().Fonts->TexData;
    const ImDrawVert* vtx = ImGui::GetForegroundDrawList()->VtxBuffer.Data;
    int wrong = 0;
    for (int i = g_FontFirstVtx; i + 3 < g_FontEndVtx; i += 4)
    {
        // A glyph's quad: vertex 0 its top-left corner, vertex 2 its bottom-right (imgui_draw.cpp, PrimRectUV).
        const ImDrawVert& a = vtx[i];
        const ImDrawVert& c = vtx[i + 2];
        const int x0 = (int)a.pos.x, y0 = (int)a.pos.y, x1 = (int)c.pos.x, y1 = (int)c.pos.y;
        const int tx0 = (int)(a.uv.x * tex->Width + 0.5f), ty0 = (int)(a.uv.y * tex->Height + 0.5f);
        const int tx1 = (int)(c.uv.x * tex->Width + 0.5f), ty1 = (int)(c.uv.y * tex->Height + 0.5f);
        if ((float)x0 != a.pos.x || (float)y0 != a.pos.y || x1 - x0 != tx1 - tx0 || y1 - y0 != ty1 - ty0)
        {
            xsdk_log_write("%s: a glyph at (%.2f, %.2f) isn't a texel a pixel", check, a.pos.x, a.pos.y);
            wrong++;
            continue;
        }
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
            {
                const uint32_t alpha = ((const ImU32*)tex->Pixels)[(ty0 + y - y0) * tex->Width + tx0 + x - x0] >> IM_COL32_A_SHIFT;
                *solid += alpha == 255;
                *empty += alpha == 0;
                PixelIs(check, x, y, alpha * 0x010101, &wrong);
            }
    }
    return wrong;
}

static void CheckFont()
{
    // The default size, whose glyphs the atlas took as it was made (FirstUi), then a size not drawn before, whose glyphs
    // come as an update.
    g_FontSize = 0.0f;
    bool drawn = Frame(FontUi, nullptr, true);
    int solid = 0, empty = 0;
    int wrong = CheckGlyphs("imgui_font", &solid, &empty);
    const bool default_seen = solid > 0 && empty > 0;
    EndFrame();

    g_FontSize = FONT_NEW_SIZE;
    drawn = Frame(FontUi, nullptr, true) && drawn;
    const bool updated = g_AtlasStatus == ImTextureStatus_WantUpdates;
    if (!updated)
        xsdk_log_write("imgui_font: the new size's glyphs left the atlas in status %d, not an update", (int)g_AtlasStatus);
    solid = empty = 0;
    wrong += CheckGlyphs("imgui_font", &solid, &empty);
    const bool new_seen = solid > 0 && empty > 0;
    EndFrame();
    char name[300];
    snprintf(name, sizeof(name), "imgui_font: text at ImGui's default size, then at %.0f pixels, which came as an update of "
        "the atlas: every pixel of each glyph's box its texel, white where solid", FONT_NEW_SIZE);
    Check(name, drawn && default_seen && new_seen && updated && wrong == 0);
}

//-----------------------------------------------------------------------------
// imgui_large_mesh: one list past 65536 vertices, drawn through VtxOffset.
//-----------------------------------------------------------------------------

static ImVec2 MeshAt(int i)
{
    return ImVec2((float)(i % MESH_COLUMNS) * MESH_PITCH, MESH_TOP + (float)(i / MESH_COLUMNS) * MESH_PITCH);
}

static void MeshUi()
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < MESH_RECTS; i++)
    {
        const ImVec2 p = MeshAt(i);
        const ImU32 color = i == 0 ? MESH_FIRST_COLOR : i == MESH_RECTS - 1 ? MESH_LAST_COLOR : MESH_OTHER_COLOR;
        draw_list->AddRectFilled(p, ImVec2(p.x + MESH_SIZE, p.y + MESH_SIZE), color);
    }
}

static void CheckMesh()
{
    const bool drawn = Frame(MeshUi, nullptr, true);
    const ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    bool offset = false;
    for (const ImDrawCmd& cmd : draw_list->CmdBuffer)
        offset = offset || cmd.VtxOffset > 0;
    int wrong = 0;
    const int ends[2] = { 0, MESH_RECTS - 1 };
    for (int end : ends)
    {
        const ImVec2 p = MeshAt(end);
        for (int y = (int)p.y; y < (int)(p.y + MESH_SIZE); y++)
            for (int x = (int)p.x; x < (int)(p.x + MESH_SIZE); x++)
                PixelIs("imgui_large_mesh", x, y, end == 0 ? MESH_FIRST_ARGB : MESH_LAST_ARGB, &wrong);
    }
    EndFrame();
    char name[200];
    snprintf(name, sizeof(name), "imgui_large_mesh: one list of %d rectangles, %d vertices, past a VtxOffset: the first and "
        "last where placed", MESH_RECTS, draw_list->VtxBuffer.Size);
    Check(name, drawn && offset && draw_list->VtxBuffer.Size == 4 * MESH_RECTS && wrong == 0);
}

//-----------------------------------------------------------------------------
// imgui_nav: controller states passed in move ImGui's focus and press a button.
//-----------------------------------------------------------------------------

static const char* NAV_BUTTONS[3] = { "one", "two", "three" };
static bool g_NavFocus;
static bool g_NavPressed[3];

static void NavUi()
{
    BeginFixedWindow("imgui_nav", WINDOW_POS, WINDOW_SIZE, CLIP_PARENT_COLOR, 0);
    for (int i = 0; i < 3; i++)
    {
        if (g_NavFocus && i == 0)
            ImGui::SetKeyboardFocusHere();
        if (ImGui::Button(NAV_BUTTONS[i]))
            g_NavPressed[i] = true;
    }
    g_NavFocus = false;
    ImGui::End();
}

static void CheckNav()
{
    // From the first button: D-pad down, down, then A, each held a frame and let go for two.
    static const uint16_t presses[] = { INPUT_GAMEPAD_DPAD_DOWN, INPUT_GAMEPAD_DPAD_DOWN, INPUT_GAMEPAD_A };
    ImGui::SetWindowFocus(nullptr);
    g_NavFocus = true;
    bool drawn = true;
    for (int i = 0; i < SETTLE_FRAMES; i++)
        drawn = Frame(NavUi, nullptr, false) && drawn;
    for (uint16_t buttons : presses)
    {
        INPUT_GAMEPAD pressed = {};
        pressed.buttons = buttons;
        drawn = Frame(NavUi, &pressed, false) && drawn;
        for (int i = 0; i < 2; i++)
            drawn = Frame(NavUi, nullptr, false) && drawn;
    }
    Check("imgui_nav: from the first of three buttons, the D-pad down twice and A press the third alone",
        drawn && !g_NavPressed[0] && !g_NavPressed[1] && g_NavPressed[2]);
}

//-----------------------------------------------------------------------------
// imgui_text_entry: XAM's keyboard fills a field, or leaves it be when cancelled.
//-----------------------------------------------------------------------------

static char g_Entry[64];
static bool g_EntryFocus;
static bool g_EntryActive;

static void EntryUi()
{
    BeginFixedWindow("imgui_text_entry", WINDOW_POS, WINDOW_SIZE, CLIP_PARENT_COLOR, 0);
    if (g_EntryFocus)
        ImGui::SetKeyboardFocusHere();
    g_EntryFocus = false;
    ImGui::InputText("##entry", g_Entry, sizeof(g_Entry));
    g_EntryActive = ImGui::IsItemActive();
    ImGui::End();
}

// Focuses the field, completes the keyboard its focus opens as XAM would, and lets ImGui take the events; false if no
// keyboard opened.
static bool EnterText(uint32_t result, uint32_t extended_error, const uint16_t* text, bool* drawn)
{
    g_EntryFocus = true;
    bool opened = false;
    for (int i = 0; i < SETTLE_FRAMES + 2 && !opened; i++)
    {
        *drawn = Frame(EntryUi, nullptr, false) && *drawn;
        opened = ImGui_ImplXsdk_CompleteKeyboard(result, extended_error, text);
    }
    for (int i = 0; i < TRICKLE_FRAMES; i++)
        *drawn = Frame(EntryUi, nullptr, false) && *drawn;
    return opened;
}

static void CheckEntry()
{
    strcpy(g_Entry, ENTRY_BEFORE);
    bool drawn = true;
    const bool accepted = EnterText(ERROR_SUCCESS, ERROR_SUCCESS, ENTRY_ACCEPTED, &drawn);
    const bool holds = strcmp(g_Entry, ENTRY_ACCEPTED_UTF8) == 0 && !g_EntryActive;
    if (!holds)
        xsdk_log_write("imgui_text_entry: accepted, the field held \"%s\", %s", g_Entry, g_EntryActive ? "active" : "left");
    const bool cancelled = EnterText(ERROR_SUCCESS, ERROR_CANCELLED, ENTRY_CANCELLED, &drawn);
    const bool kept = strcmp(g_Entry, ENTRY_ACCEPTED_UTF8) == 0 && !g_EntryActive;
    if (!kept)
        xsdk_log_write("imgui_text_entry: cancelled, the field held \"%s\", %s", g_Entry, g_EntryActive ? "active" : "left");
    Check("imgui_text_entry: a focused field opens the keyboard; accepted, its text replaces the field's; cancelled, the "
        "field stays as it was", drawn && accepted && holds && cancelled && kept);
}

//-----------------------------------------------------------------------------

static void FirstUi()
{
    // The default size's glyphs, so the atlas takes them as it's made.
    ImGui::GetForegroundDrawList()->AddText(WINDOW_POS, IM_COL32_WHITE, FONT_TEXT);
}

int main()
{
    xsdk_log_write("imgui_check: Dear ImGui %s through imgui_impl_xsdk", IMGUI_VERSION);
    const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
    if (!NT_SUCCESS(splash))
        xsdk_log_write("imgui_check: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
    NTSTATUS status = xsdk_gpu_open();
    if (NT_SUCCESS(status))
        status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &g_Width, &g_Height);
    if (NT_SUCCESS(status) && g_Width > MAX_WIDTH)
        status = STATUS_NOT_IMPLEMENTED;
    if (NT_SUCCESS(status))
        status = xsdk_texture_init(&g_Frame, XSDK_FORMAT_A8R8G8B8, g_Width, g_Height, 1, true);
    if (!NT_SUCCESS(status))
    {
        xsdk_log_write("imgui_check: opening the GPU and display at %ux%u failed: 0x%08X", (unsigned)g_Width, (unsigned)g_Height, (unsigned)status);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplXsdk_Init())
    {
        xsdk_log_write("imgui_check: ImGui_ImplXsdk_Init failed");
        return 1;
    }

    // The first frame lands on whatever eDRAM held (xsdk:docs/GUIDE.md), so no check reads it.
    bool drawn = Frame(FirstUi, nullptr, false);
    CheckWindow();
    CheckClip();
    CheckImage();
    CheckFont();
    CheckMesh();
    CheckNav();
    CheckEntry();

    ImGui_ImplXsdk_Shutdown();
    ImGui::DestroyContext();
    xsdk_present_close();
    xsdk_texture_free(&g_Image);
    status = xsdk_gpu_close();
    if (!drawn || !NT_SUCCESS(status))
        xsdk_log_write("imgui_check: the first frame or xsdk_gpu_close failed: 0x%08X", (unsigned)status);
    xsdk_log_write("tests: %d passed, %d failed", g_Passed, g_Failed);
    return g_Failed != 0;
}
