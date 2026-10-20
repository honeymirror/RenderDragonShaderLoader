#version 450

in vec3 vaPosition;
in vec2 vaUV0;
in vec4 vaColor;

out vec2 texcoord;
out vec4 tint;

void main() {
    texcoord = vaUV0;
    tint = vaColor;
    gl_Position = vec4(vaPosition, 1.0);
}
