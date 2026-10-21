# Dear ImGui on the Xbox 360 through xsdk (docs/README-xsdk.md): example_xsdk, ImGui's demo window on controller 1, and
# imgui_check, imgui_impl_xsdk's checks. Build xsdk first (make in its checkout), then:
#   cmake -S examples/example_xsdk -B build-xsdk -G Ninja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake
#   cmake --build build-xsdk
cmake_minimum_required(VERSION 3.25)
project(imgui_xsdk CXX)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# ImGui's five sources as upstream ships them. Its default shell function calls fork() and execvp(), which the console
# hasn't got.
add_library(imgui STATIC "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_demo.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

# Each title builds the backend itself: imgui_check's with IMGUI_IMPL_XSDK_CHECK, which stands in for a person at XAM's
# keyboard.
add_executable(example_xsdk main.cpp "${IMGUI_DIR}/backends/imgui_impl_xsdk.cpp")
add_executable(imgui_check imgui_check.cpp "${IMGUI_DIR}/backends/imgui_impl_xsdk.cpp")
target_compile_definitions(imgui_check PRIVATE IMGUI_IMPL_XSDK_CHECK)
foreach(title example_xsdk imgui_check)
    target_link_libraries(${title} PRIVATE imgui)
    target_compile_options(${title} PRIVATE -Wall -Wextra -Werror)
endforeach()
xsdk_add_xex(example_xsdk TITLE_ID 0x58534944 NAME "Dear ImGui demo")
xsdk_add_xex(imgui_check TITLE_ID 0x58534943 NAME "imgui_check")
