$input a_position
$input a_texcoord0

$output v_texcoord0

#include "bgfx_shader.sh"

void main() {
    v_texcoord0 = a_texcoord0;
    gl_Position = vec4(a_position.xy * 2.0 - 1.0, 0.0, 1.0);
}
