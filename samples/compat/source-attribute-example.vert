#version 450

// Identifiers used only in comments must remain unchanged.
in vec3 vaPosition;
in vec2 vaUV0;
in vec4 vaColor;

out vec2 v_uv;
out vec4 v_color;

void main() {
    v_uv = vaUV0;
    v_color = vaColor;
    gl_Position = vec4(vaPosition, 1.0);
}
