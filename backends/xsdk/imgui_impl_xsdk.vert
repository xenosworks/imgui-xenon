#version 450
// ImGui's vertex: its position in pixels into clip space, its colour and its texture coordinate as they are.
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
// xy scales a position, zw moves it, set each frame by imgui_impl_xsdk.cpp.
layout(push_constant) uniform Push { vec4 scale_offset; } pc;

void main()
{
    gl_Position = vec4(a_position * pc.scale_offset.xy + pc.scale_offset.zw, 0.0, 1.0);
    v_color = a_color;
    v_uv = a_uv;
}
