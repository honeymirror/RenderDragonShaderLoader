#version 450

uniform sampler2D gtexture;

in vec2 texcoord;
in vec4 tint;
out vec4 fragColor;

void main() {
    fragColor = texture(gtexture, texcoord) * tint;
}

/* DRAWBUFFERS:01 */
