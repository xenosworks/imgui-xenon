// Dear ImGui: standalone example application for the Xbox 360 through xsdk (docs/README-xsdk.md)
// ImGui's demo window on controller 1: the D-pad or left stick moves, A activates, B goes back, X opens a window's
// menu and Y holds to move and resize windows. Text fields open XAM's keyboard. Back or Xbox Home ends it.

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

#include "imgui.h"
#include "imgui_impl_xsdk.h"

#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"

static const uint32_t SPLASH_MS = 2000;
// The Guide's first opening held a frame up 2.7 s (xsdk:samples/triangle/main.c).
static const uint32_t BEGIN_TIMEOUT_MS = 5000;
static const uint32_t CLEAR_ARGB = 0xFF334D73;

// Main code
int main()
{
    const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
    if (!NT_SUCCESS(splash))
        xsdk_log_write("example_xsdk: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
    uint32_t width = 0, height = 0;
    NTSTATUS status = xsdk_gpu_open();
    if (NT_SUCCESS(status))
        status = xsdk_present_open(XSDK_DISPLAY_720P, 2, 1, &width, &height);
    if (!NT_SUCCESS(status))
    {
        xsdk_log_write("example_xsdk: opening the GPU and display failed: 0x%08X", (unsigned)status);
        return 1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;     // Enable Gamepad Controls
    io.IniFilename = nullptr;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 20.0f;     // Larger than a PC's, for a television across the room.
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;

    // Setup Platform/Renderer backends
    if (!ImGui_ImplXsdk_Init())
    {
        xsdk_log_write("example_xsdk: ImGui_ImplXsdk_Init failed");
        return 1;
    }

    // Main loop
    for (;;)
    {
        // The title reads controller 1 itself, for Back, and passes it on.
        INPUT_STATE input = {};
        if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS)
        {
            if (input.gamepad.buttons & INPUT_GAMEPAD_BACK)
                break;
            ImGui_ImplXsdk_ProcessGamepad(&input.gamepad);
        }

        // Start the Dear ImGui frame
        ImGui_ImplXsdk_NewFrame(width, height);
        ImGui::NewFrame();
        ImGui::ShowDemoWindow();
        ImGui::Render();

        // Draw, then resolve into the next front buffer, which clears eDRAM for the frame after.
        uint32_t address = 0;
        status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
        if (!NT_SUCCESS(status))
        {
            xsdk_log_write("example_xsdk: xsdk_present_begin failed: 0x%08X", (unsigned)status);
            break;
        }
        xsdk_gpu_set_target(width, height);
        ImGui_ImplXsdk_RenderDrawData(ImGui::GetDrawData());
        xsdk_gpu_resolve(address, width, height, CLEAR_ARGB);
        xsdk_present_end();
    }

    // Cleanup
    ImGui_ImplXsdk_Shutdown();
    ImGui::DestroyContext();
    xsdk_present_close();
    status = xsdk_gpu_close();
    if (!NT_SUCCESS(status))
        xsdk_log_write("example_xsdk: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
    return 0;
}
