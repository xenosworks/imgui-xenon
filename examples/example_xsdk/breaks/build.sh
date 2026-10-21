#!/bin/sh
# imgui_check built once for each patch here, broken on purpose, as <out>/imgui_check_<break>.xex; each patch is taken
# back out after its build, even a failed one. Run in WSL from anywhere, after xsdk's make:
#   sh examples/example_xsdk/breaks/build.sh <xsdk checkout> [<out>, build-xsdk-breaks in this repository]
# Each fails the check it names and no other:
# - half_pixel: the projection without the half pixel; fails imgui_window, and may fail imgui_font too, as it moves
#   every pixel's sample onto a texel's corner, where the texel taken can go either way.
# - clip_ignored: clip rectangles ignored, the scissor over the display; fails imgui_clip.
# - font_bound: the command's texture ignored, the font atlas bound; fails imgui_image.
# - pixels_as_bytes: ImGui's pixels read as the bytes R, G, B then A; fails imgui_font, its glyphs solid boxes.
# - updates_ignored: ImGui's texture updates ignored; fails imgui_font, the new size drawing nothing.
# - vtx_offset_ignored: VtxOffset ignored; fails imgui_large_mesh, the last rectangle not drawn.
# - dpad_swapped: the D-pad's up and down swapped; fails imgui_nav.
# - accepted_dropped: an accepted keyboard's text dropped; fails imgui_text_entry.
# - cancel_accepted: a cancelled keyboard taken as accepted; fails imgui_text_entry.
set -eu
xsdk=$(readlink -f "$1")
cd "$(dirname "$0")/../../.."
out=$(readlink -f "${2:-build-xsdk-breaks}")
for name in half_pixel clip_ignored font_bound pixels_as_bytes updates_ignored vtx_offset_ignored dpad_swapped \
    accepted_dropped cancel_accepted; do
    patch -p1 --forward --quiet < "examples/example_xsdk/breaks/$name.patch"
    trap 'patch -p1 -R --quiet < "examples/example_xsdk/breaks/$name.patch"' EXIT INT TERM
    cmake -S examples/example_xsdk -B "$out/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$xsdk/cmake/xsdk.cmake" \
        -DCMAKE_BUILD_TYPE=Release > /dev/null
    cmake --build "$out/build" --target imgui_check
    patch -p1 -R --quiet < "examples/example_xsdk/breaks/$name.patch"
    trap - EXIT INT TERM
    cp "$out/build/imgui_check.xex" "$out/imgui_check_$name.xex"
done
