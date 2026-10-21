#version 450
// The vertex colour times the texture, as ImGui's other renderers draw.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main()
{
    o_color = v_color * texture(u_texture, v_uv);
}
