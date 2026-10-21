# Xbox 360 (xsdk)

Dear ImGui for the Xbox 360 through [xsdk](https://github.com/xenosworks/xsdk), a homebrew SDK that builds titles for
the console's retail kernel. This is Dear ImGui 1.93.0 with upstream's files unchanged; the `xenon` branch adds a
backend, `backends/imgui_impl_xsdk.cpp`, that draws through xsdk's GPU layer and reads controller 1, its shaders, a
demo and a check title in `examples/example_xsdk`, and this file.

## Adding ImGui to a title

Build xsdk first (`make` in its checkout builds `libxsdk.a`). A title adds ImGui's five sources, `imgui.cpp`,
`imgui_demo.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp` and `imgui_widgets.cpp`, and `backends/imgui_impl_xsdk.cpp`
to its build, with this repository and its `backends` folder on the include path, and defines
`IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`: ImGui's default shell function calls `fork()` and `execvp()`, which the
console hasn't got. `examples/example_xsdk/CMakeLists.txt` does that through xsdk's toolchain file:

```bash
cmake -S examples/example_xsdk -B build-xsdk -GNinja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake
cmake --build build-xsdk
```

It builds `example_xsdk.xex`, ImGui's demo window on controller 1, which runs until Back or Xbox Home, and
`imgui_check.xex` (below). The backend needs no glslang: its shaders are built into
`backends/imgui_impl_xsdk_shaders.h`, which is committed.

A frame, after `xsdk_gpu_open()`, `xsdk_present_open()`, `ImGui::CreateContext()` and `ImGui_ImplXsdk_Init()`:

```cpp
ImGui_ImplXsdk_NewFrame(width, height);     // the size xsdk_present_open() gave
ImGui::NewFrame();
// ... the title's windows ...
ImGui::Render();
xsdk_present_begin(&address, timeout_ms);
xsdk_gpu_set_target(width, height);
// ... the title's own drawing ...
ImGui_ImplXsdk_RenderDrawData(ImGui::GetDrawData());
xsdk_gpu_resolve(address, width, height, clear_argb);
xsdk_present_end();
```

`ImGui_ImplXsdk_Shutdown()` goes before `xsdk_gpu_close()`. ImGui is MIT licensed: a title that ships it carries
ImGui's notice, `LICENSE.txt`.

## Notes

-   **Drawing:** `ImGui_ImplXsdk_RenderDrawData()` draws into the target set now, and leaves set its shaders, vertex
    stream 0, texture slot 0, vertex constant 0, ImGui's blending, depth, stencil and culling off, and the viewport
    and scissor over the display; a title sets what it needs again after it. Not inside an xsdk recording or tiled
    pass, which stops at an assert.
-   **Buffers:** each frame's vertices and indices go into the next of three slots of GPU memory, each reused once the
    GPU is past its frame and grown when short. Every command's indices start on 4 bytes, as the console has drawn
    index lists only from there, and a command of more than 65,532 indices goes in pieces.
-   **Textures:** an `ImTextureID` is an `XsdkTexture*`, so `ImGui::Image((ImTextureID)(intptr_t)&texture, size)`
    draws any xsdk texture. ImGui's own textures are tiled `XSDK_FORMAT_A8R8G8B8`, red and blue swapped by
    `xsdk_texture_swizzle()`, as ImGui writes its RGBA32 pixels as `ImU32` words, 0xAABBGGRR on the console. Their
    changed rows are uploaded, and a texture freed, only once the GPU is past the last frame, so a frame that first
    draws new glyphs may wait for the one before. The standard draw callbacks, nearest and linear sampling and
    resetting the render state, are there.
-   **Controller:** controller 1 through `XamInputGetState()`, mapped as `imgui_impl_win32.cpp` maps XInput's, with
    `ImGuiBackendFlags_HasGamepad`, so a title turns on `ImGuiConfigFlags_NavEnableGamepad`. A title that reads its
    controllers itself passes a state to `ImGui_ImplXsdk_ProcessGamepad()` before `ImGui_ImplXsdk_NewFrame()`, which
    then doesn't read controller 1, as `examples/example_xsdk/main.cpp` does to end on Back.
-   **Text fields:** when a writable text field becomes active, the backend opens XAM's on-screen keyboard with the
    field's text through xsdk's `xsdk_xam_keyboard()` and looks at it once a frame. Accepted, it replaces the field's
    text through ImGui's own events, select all, delete, the characters, then Ctrl+Enter, which leaves the field; any
    other ending leaves the field as it was. The controller reads as released to ImGui while the keyboard is open, and
    until the button that closed it is let go.
-   No mouse, and ImGui draws no cursor. No chatpads or USB keyboards.
-   Xenia: xsdk's `tools/xenia_scan.py` lists instructions Xenia skips in ImGui's font atlas functions
    (`ImFontAtlasTextureBlockQueueUpload` and others), so text checks in Xenia are unreliable.

## The shaders

`backends/xsdk/imgui_impl_xsdk.vert` and `.frag`, GLSL, built by xsdk's `tools/build_shaders.py` through xost. After
changing them, in WSL, with xsdk built:

```bash
python3 <xsdk>/tools/build_shaders.py --glslang ~/xsdk-gcc/bin/glslang --output backends/imgui_impl_xsdk_shaders.h \
    backends/xsdk/imgui_impl_xsdk.vert backends/xsdk/imgui_impl_xsdk.frag
```

## The check title

`imgui_check.xex` builds each frame from fixed inputs, draws it through the backend, resolves it into a front buffer and
reads it back, and logs `PASS` or `FAIL` for each check, then `tests: <passed> passed, <failed> failed`:

-   `imgui_window`: a square, unbordered window at (100, 80), 300x200, its edge pixels in, the ones beside them out,
    and a rectangle in it a quarter pixel off whole ones on the pixels whose centres it holds;
-   `imgui_clip`: a rectangle past a scrolled child's clip, drawn inside the clip alone;
-   `imgui_image`: `ImGui::Image` of a 4x4 xsdk texture at 64x64, each block's centre its texel;
-   `imgui_font`: text at ImGui's default size, then at a size not drawn before, which comes as an update of the atlas,
    each glyph's box its texels' alpha, white where solid;
-   `imgui_large_mesh`: one list of 17,000 rectangles, 68,000 vertices, the first and last where placed;
-   `imgui_nav`: controller states passed in, the D-pad down twice and A press the third of three buttons;
-   `imgui_text_entry`: a field given an accepted keyboard's text holds it, and a cancelled one leaves it as it was.

The backend in it is built with `IMGUI_IMPL_XSDK_CHECK`, which opens no keyboard and gives the check
`ImGui_ImplXsdk_CompleteKeyboard()` to end it as XAM would, as the keyboard needs a person.
`examples/example_xsdk/breaks/build.sh <xsdk>` builds it once for each patch there, broken on purpose, as
`imgui_check_<break>.xex`; the script says which check each fails.
