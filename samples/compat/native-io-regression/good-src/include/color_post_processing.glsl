///////////////////////////////////////////////////////////
// VERTEX SHADER
///////////////////////////////////////////////////////////
#if SHADER_STAGE__VERTEX
in vec3 a_position;
in vec2 a_texcoord0;

out vec2 v_texcoord0;

void main() {
    v_texcoord0 = a_texcoord0;
    gl_Position = vec4(a_position.xy * 2.0 - 1.0, 0.0, 1.0);
}
#endif

///////////////////////////////////////////////////////////
// FRAGMENT SHADER
///////////////////////////////////////////////////////////
#if SHADER_STAGE__FRAGMENT
uniform vec4 ExposureCompensation;
uniform vec4 LuminanceMinMaxAndWhitePointAndMinWhitePoint;

SAMPLER2D(s_ColorTexture);
SAMPLER2D(s_PreExposureLuminance);
SAMPLER2D(s_AverageLuminance);
SAMPLER2D(s_CustomExposureCompensation);
SAMPLER2D(s_RasterizedColor);

#include "lib/common.glsl"

// AgX tonemapping
// https://github.com/google/filament/blob/main/filament/src/ToneMapper.cpp
// https://github.com/bWFuanVzYWth/AgX

const mat3 REC709_TO_REC2020 = mat3(
    0.6274040, 0.0690970, 0.0163916,
    0.3292820, 0.9195400, 0.0880132,
    0.0433136, 0.0113612, 0.8955950
);

const mat3 REC2020_TO_REC709 = mat3(
     1.6604910, -0.1245505, -0.0181508,
    -0.5876411, 1.1328999, -0.1005789,
    -0.0728499, -0.0083494, 1.1187297
);

const mat3 AGX_INSET = mat3(
    0.856627153315983, 0.137318972929847, 0.11189821299995,
    0.0951212405381588, 0.761241990602591, 0.0767994186031903,
    0.0482516061458583, 0.101439036467562, 0.811302368396859
);

const mat3 AGX_OUTSET = mat3(
    1.1271005818144368, -0.1413297634984383, -0.14132976349843826,
    -0.11060664309660323, 1.157823702216272, -0.11060664309660294,
    -0.016493938717834573, -0.016493938717834257, 1.2519364065950405
);

const float AGX_MIN_EV = -12.473931188332413;
const float AGX_MAX_EV = 4.026068811667588;
const float AGX_DYNAMIC_RANGE = AGX_MAX_EV - AGX_MIN_EV;

vec3 agxCurve3(vec3 v) {
    const float threshold = 0.6060606060606061;
    const float a_up = 69.86278913545539;
    const float a_down = 59.507875;
    const float b_up = 13.0 / 4.0;
    const float b_down = 3.0 / 1.0;
    const float c_up = -4.0 / 13.0;
    const float c_down = -1.0 / 3.0;

    vec3 mask = step(v, vec3(threshold));
    vec3 a = a_up + (a_down - a_up) * mask;
    vec3 b = b_up + (b_down - b_up) * mask;
    vec3 c = c_up + (c_down - c_up) * mask;
    return 0.5 + (((-2.0 * threshold)) + 2.0 * v) * pow(1.0 + a * pow(abs(v - threshold), b), c);
}

vec3 agx(vec3 val) {
    val = REC709_TO_REC2020 * val;
    val = AGX_INSET * val;

    val = max(val, vec3(1e-6));
    val = clamp(log2(val) * (1.0 / AGX_DYNAMIC_RANGE) - (AGX_MIN_EV / AGX_DYNAMIC_RANGE), 0.0, 1.0);
    val = agxCurve3(val);

    val = AGX_OUTSET * val;

    val = max(val, vec3(0.0));
    val = pow(val, vec3(2.2));

    val = REC2020_TO_REC709 * val;
    return val;
}

in vec2 v_texcoord0;
out vec4 fragColor;

void main() {
    vec3 inputColor = texture(s_ColorTexture, v_texcoord0).rgb;
    inputColor = unExposeLighting(inputColor, texture(s_PreExposureLuminance, vec2(0.5)).r);

    float refLuminance = clamp(texture(s_AverageLuminance, vec2(0.5)).r,
        LuminanceMinMaxAndWhitePointAndMinWhitePoint.r,
        LuminanceMinMaxAndWhitePointAndMinWhitePoint.g);
    float exposure = (RDL_MiddleGray.r / refLuminance) * ExposureCompensation.g
        * RDL_OutputExposure.r;
    inputColor *= exposure;

    vec4 overlayCol = texture(s_RasterizedColor, v_texcoord0);
    inputColor = mix(inputColor, overlayCol.rgb, overlayCol.a);

    vec3 outColor = agx(inputColor);
    outColor = fromLinear(outColor);
    fragColor = vec4(outColor, 1.0);
}
#endif //SHADER_STAGE__FRAGMENT
