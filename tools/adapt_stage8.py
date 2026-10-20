"""Build RenderDragonShaderLoader metadata overlays for Mirai and newb-x.

Usage:
  python tools/adapt_stage8.py --mirai <Mirai.zip> --newb <newb-x.zip> --out <dir>

The resulting packs keep the original payload, add rdloader.pack.json,
options.json, variants.json and lang files next to the project root, and for
newb-x also flatten the lazurite material project into a loader-compatible
root.
"""

from __future__ import annotations

import argparse
import io
import json
import re
import shutil
import subprocess
import tomllib
import zipfile
from pathlib import Path


def json_bytes(value) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def human_name(macro: str) -> str:
    return macro.replace("NL_", "").replace("_", " ").title()


# newb-x config.h 的分组标题 -> 中文
NEWB_GROUP_ZH = {
    "Color correction": "色彩校正",
    "Lighting": "光照",
    "Fog": "雾",
    "Sky": "天空",
    "Rainbow": "彩虹",
    "Ore glow intensity": "矿石发光强度",
    "Waving": "摆动",
    "Water": "水面",
    "Underwater": "水下",
    "Vanilla cloud settings - make sure to remove clouds.png when using this": "原版云设置（使用前记得移除 clouds.png）",
    "Soft cloud settings": "柔和云设置",
    "Rounded cloud settings": "圆角云设置",
    "Realistic cloud settings": "写实云设置",
    "Aurora settings": "极光设置",
    "Shooting star": "流星",
    "Galaxy": "银河",
    "Sun/Moon": "日月",
    "Fake godrays during sunrise/sunset": "日出/日落时的伪上帝光",
    "Sky reflection": "天空反射",
    "Entity (Actor, ItemInHand)": "实体（生物、手持物）",
    "Weather particles": "天气粒子",
    "Lava effects": "岩浆效果",
}

# newb-x config.h 的宏名 -> 中文标签
NEWB_OPTION_ZH = {
    "NL_TONEMAP_TYPE": "色调映射类型",
    "NL_GAMMA": "伽马",
    "NL_EXPOSURE": "曝光",
    "NL_SATURATION": "饱和度",
    "NL_TINT": "色调",
    "NL_SUNLIGHT_INTENSITY": "日光强度",
    "NL_TORCHLIGHT_INTENSITY": "火把光强度",
    "NL_SHADOW_INTENSITY": "阴影强度",
    "NL_MIN_LIGHTING_BOOST": "暗部最低亮度提升",
    "NL_BLINKING_TORCH": "火把闪烁",
    "NL_CLOUD_SHADOW": "云影",
    "NL_MIST_DENSITY": "薄雾浓度",
    "NL_FOG_STRENGTH": "远景雾强度",
    "NL_SKY_VOID_FACTOR": "虚空天空混合",
    "NL_SKY_VOID_DARKNESS": "虚空天空暗度",
    "NL_SKY_RAIN_MIX_FACTOR": "雨天天空混合",
    "NL_RAINBOW": "彩虹",
    "NL_RAINBOW_CLEAR": "晴天彩虹亮度",
    "NL_RAINBOW_RAIN": "雨天彩虹亮度",
    "NL_GLOW_TEX": "发光贴图强度",
    "NL_GLOW_SHIMMER_SPEED": "闪烁速度",
    "NL_GLOW_LEAK": "发光溢色",
    "NL_PLANTS_WAVE": "植物摆动",
    "NL_LANTERN_WAVE": "灯笼摆动",
    "NL_WAVE_SPEED": "摆动速度",
    "NL_EXTRA_PLANTS_WAVE": "额外植物摆动",
    "NL_WAVE_RANGE": "摆动范围",
    "NL_WATER_TRANSPARENCY": "水面透明度",
    "NL_WATER_BUMP": "水面凹凸",
    "NL_WATER_WAVE_SPEED": "水波速度",
    "NL_WATER_TEX_OPACITY": "水面贴图不透明度",
    "NL_WATER_WAVE": "水波",
    "NL_WATER_REFL_MASK": "水面反射遮罩",
    "NL_UNDERWATER_BRIGHTNESS": "水下亮度",
    "NL_CAUSTIC_INTENSITY": "焦散强度",
    "NL_UNDERWATER_WAVE": "水下波纹",
    "NL_CLOUD0_THICKNESS": "原版云厚度",
    "NL_CLOUD0_RAIN_THICKNESS": "原版云雨天厚度",
    "NL_CLOUD0_OPACITY": "原版云不透明度",
    "NL_CLOUD0_MULTILAYER": "原版云多层",
    "NL_CLOUD1_DEPTH": "柔和云厚度",
    "NL_CLOUD1_SPEED": "柔和云速度",
    "NL_CLOUD1_DENSITY": "柔和云密度",
    "NL_CLOUD1_OPACITY": "柔和云不透明度",
    "NL_CLOUD_HEIGHT": "云层高度偏移",
    "NL_CLOUD2_THICKNESS": "圆角云厚度",
    "NL_CLOUD2_RAIN_THICKNESS": "圆角云雨天厚度",
    "NL_CLOUD2_STEPS": "圆角云步进",
    "NL_CLOUD2_DENSITY": "圆角云锐度",
    "NL_CLOUD2_VELOCITY": "圆角云速度",
    "NL_CLOUD2_LAYER2": "圆角云第二层",
    "NL_CLOUD2_LAYER2_OFFSET": "圆角云二层高度",
    "NL_CLOUD2_LAYER2_THICKNESS": "圆角云二层厚度",
    "NL_CLOUD2_LAYER2_RAIN_THICKNESS": "圆角云二层雨天厚度",
    "NL_CLOUD2_LAYER2_STEPS": "圆角云二层步进",
    "NL_CLOUD2_LAYER2_DENSITY": "圆角云二层锐度",
    "NL_CLOUD2_LAYER2_VELOCITY": "圆角云二层速度",
    "NL_CLOUD3_SPEED": "写实云速度",
    "NL_CLOUD3_SHADOW": "写实云阴影",
    "NL_CLOUD3_SHADOW_OFFSET": "写实云阴影偏移",
    "NL_AURORA": "极光强度",
    "NL_AURORA_VELOCITY": "极光速度",
    "NL_AURORA_SCALE": "极光尺度",
    "NL_AURORA_WIDTH": "极光宽度",
    "NL_SHOOTING_STAR_PERIOD": "流星周期",
    "NL_SHOOTING_STAR_DELAY": "流星延迟",
    "NL_GALAXY_STARS": "银河亮度",
    "NL_GALAXY_VIBRANCE": "银河色彩浓度",
    "NL_GALAXY_SPEED": "银河速度",
    "NL_GALAXY_DAY_VISIBILITY": "白天银河可见度",
    "NL_SUN_SIZE": "太阳大小",
    "NL_MOON_SIZE": "月亮大小",
    "NL_SUN_PATH_YAW": "太阳轨迹偏航",
    "NL_MOON_PATH_YAW": "月亮轨迹偏航",
    "NL_SUN_PATH_TILT": "太阳轨迹倾角",
    "NL_MOON_PATH_TILT": "月亮轨迹倾角",
    "NL_SUN_TILT": "太阳倾角",
    "NL_MOON_TILT": "月亮倾角",
    "NL_GODRAY": "上帝光",
    "NL_GROUND_REFL": "地面反射",
    "NL_GROUND_RAIN_WETNESS": "雨天地面湿润",
    "NL_GROUND_RAIN_PUDDLES": "雨天地面积水",
    "NL_ENTITY_BRIGHTNESS": "实体亮度",
    "NL_WEATHER_RAIN_SLANT": "雨丝倾斜",
    "NL_WEATHER_PARTICLE_SIZE": "天气粒子大小",
    "NL_LAVA_NOISE_BUMP": "岩浆噪波凹凸",
    "NL_LAVA_NOISE_SPEED": "岩浆噪波速度",
}

# Mirai 3.0 的分组 / 宏名 -> 中文（宏没有 NL_ 前缀，单独放一份避免和 NewBx 混在一起）
MIRAI_GROUP_ZH = {
    "LIGHTING": "光照",
    "ATMOSPHERE": "大气",
    "CLOUDS": "云",
}

MIRAI_OPTION_ZH = {
    # 光照
    "MIDDLE_GRAY": "中灰值",
    "EXPOSURE_MULTIPLIER": "曝光倍数",
    "SKY_AMBIENT_MULTIPLIER": "天空环境光倍数",
    "BLOCK_AMBIENT_MULTIPLIER": "方块光环境倍数",
    "EMISSIVE_MATERIAL_INTENSITY": "自发光强度",
    "BORDER_FOG_INTENSITY": "边界雾强度",
    "SUN_RADIANCE_MULTIPLIER": "太阳辐射倍数",
    "MOON_RADIANCE_MULTIPLIER": "月亮辐射倍数",
    # 大气
    "AERIAL_SCALE": "大气散射强度",
    "ATMOSPHERE_HEIGHT": "大气层高度",
    "ATMOSPHERE_DENSITY": "大气密度",
    "RAYLEIGH_MAX_LUM": "瑞利散射上限",
    "MIE_MAX_LUM": "米氏散射强度",
    "M_OZONE": "臭氧吸收",
    "CAM_ALTITUDE": "相机海拔",
    # 云
    "CIRRUS_HEIGHT": "卷云高度",
}

# 英文标签（human_name 会把 M_OZONE 拆成 "M Ozone" 这种，手动修一下）
MIRAI_OPTION_EN = {
    "MIDDLE_GRAY": "Middle Gray",
    "EXPOSURE_MULTIPLIER": "Exposure Multiplier",
    "SKY_AMBIENT_MULTIPLIER": "Sky Ambient Multiplier",
    "BLOCK_AMBIENT_MULTIPLIER": "Block Ambient Multiplier",
    "EMISSIVE_MATERIAL_INTENSITY": "Emissive Material Intensity",
    "BORDER_FOG_INTENSITY": "Border Fog Intensity",
    "SUN_RADIANCE_MULTIPLIER": "Sun Radiance",
    "MOON_RADIANCE_MULTIPLIER": "Moon Radiance",
    "AERIAL_SCALE": "Aerial Scattering",
    "ATMOSPHERE_HEIGHT": "Atmosphere Height",
    "ATMOSPHERE_DENSITY": "Atmosphere Density",
    "RAYLEIGH_MAX_LUM": "Rayleigh Scattering",
    "MIE_MAX_LUM": "Mie Scattering",
    "M_OZONE": "Ozone Absorption",
    "CAM_ALTITUDE": "Camera Altitude",
    "CIRRUS_HEIGHT": "Cirrus Height",
}

# 第一批连续参数迁移：源宏 -> (运行时 uniform, 源码默认值)
MIRAI_RUNTIME_UNIFORMS = {
    "MIDDLE_GRAY": ("RDL_MiddleGray", 0.18),
    "EXPOSURE_MULTIPLIER": ("RDL_ExposureMultiplier", 100.0),
    "BORDER_FOG_INTENSITY": ("RDL_BorderFog", 0.1),
    "AERIAL_SCALE": ("RDL_AerialScale", 1.0),
    "ATMOSPHERE_DENSITY": ("RDL_AtmosphereDensity", 1.0),
    "CIRRUS_HEIGHT": ("RDL_CirrusHeight", 580.0),
    "SKY_AMBIENT_MULTIPLIER": ("RDL_SkyAmbient", 2.0),
    "SUN_RADIANCE_MULTIPLIER": ("RDL_SunRadiance", 1.0),
    "MOON_RADIANCE_MULTIPLIER": ("RDL_MoonRadiance", 0.0),
    "BLOCK_AMBIENT_MULTIPLIER": ("RDL_BlockAmbient", 0.01),
    "EMISSIVE_MATERIAL_INTENSITY": ("RDL_Emissive", 0.1),
    "ATMOSPHERE_HEIGHT": ("RDL_AtmosphereHeight", 100000.0),
    "RAYLEIGH_MAX_LUM": ("RDL_RayleighLum", 2.5),
    "MIE_MAX_LUM": ("RDL_MieLum", 0.5),
    "M_OZONE": ("RDL_Ozone", 5.0),
    "CAM_ALTITUDE": ("RDL_CamAltitude", 100.0),
}

# Runtime uniforms do not need compile-time macro values, so their UI steps can
# be tuned independently from the generic 0.1 macro step.
MIRAI_RUNTIME_STEPS = {
    "MIDDLE_GRAY": 0.01,
    "BORDER_FOG_INTENSITY": 0.01,
    "AERIAL_SCALE": 0.05,
    "ATMOSPHERE_DENSITY": 0.05,
    "CIRRUS_HEIGHT": 10.0,
    "SKY_AMBIENT_MULTIPLIER": 0.05,
    "SUN_RADIANCE_MULTIPLIER": 0.05,
    "MOON_RADIANCE_MULTIPLIER": 0.05,
    "BLOCK_AMBIENT_MULTIPLIER": 0.01,
    "EMISSIVE_MATERIAL_INTENSITY": 0.01,
    "ATMOSPHERE_HEIGHT": 1000.0,
    "RAYLEIGH_MAX_LUM": 0.05,
    "MIE_MAX_LUM": 0.05,
    "M_OZONE": 0.1,
    "CAM_ALTITUDE": 0.1,
}

# 只挂在最终后处理材质上，避免为了一个曝光滑条重编整包所有 shader。
MIRAI_LOCAL_UNIFORMS = {
    "ColorPostProcessing": {
        "RDL_OutputExposure": 1.0,
    },
}


MIRAI_GAMMA_SCREEN_FILES = {
    "materials/GammaScreenColorPostProcessing/config.json": """{
  "compiler": {
    "type": "albite"
  }
}
""",
    "materials/GammaScreenColorPostProcessing/shaders/shader.vert": """#version 450

uniform vec4 RDL_MiddleGray;
uniform vec4 RDL_ExposureMultiplier;
uniform vec4 RDL_BorderFog;
uniform vec4 RDL_AerialScale;
uniform vec4 RDL_AtmosphereDensity;
uniform vec4 RDL_CirrusHeight;
uniform vec4 RDL_SkyAmbient;
uniform vec4 RDL_SunRadiance;
uniform vec4 RDL_MoonRadiance;
uniform vec4 RDL_BlockAmbient;
uniform vec4 RDL_Emissive;
uniform vec4 RDL_AtmosphereHeight;
uniform vec4 RDL_RayleighLum;
uniform vec4 RDL_MieLum;
uniform vec4 ViewportScale;

in vec3 a_position;
in vec2 a_texcoord0;

out vec4 v_texcoord0;

void main() {
    vec2 scaled = a_texcoord0 * ViewportScale.xy;
    v_texcoord0 = vec4(scaled, a_texcoord0);
    gl_Position = vec4(a_position.xy * 2.0 - 1.0, 0.0, 1.0);
}
""",
    "materials/GammaScreenColorPostProcessing/shaders/shader.frag": """#version 450

uniform vec4 RDL_MiddleGray;
uniform vec4 RDL_ExposureMultiplier;
uniform vec4 RDL_BorderFog;
uniform vec4 RDL_AerialScale;
uniform vec4 RDL_AtmosphereDensity;
uniform vec4 RDL_CirrusHeight;
uniform vec4 RDL_SkyAmbient;
uniform vec4 RDL_SunRadiance;
uniform vec4 RDL_MoonRadiance;
uniform vec4 RDL_BlockAmbient;
uniform vec4 RDL_Emissive;
uniform vec4 RDL_AtmosphereHeight;
uniform vec4 RDL_RayleighLum;
uniform vec4 RDL_MieLum;
uniform vec4 ColorGrading_Misc;
uniform vec4 RDL_OutputExposure;

#include "lib/binding_helper.glsl"

SAMPLER2D(s_ColorTexture);

in vec4 v_texcoord0;
out vec4 fragColor;

vec3 toLinear(vec3 color) {
    if (ColorGrading_Misc.y != 0.0) {
        return pow(max(color, vec3(0.0)), vec3(2.2));
    }
    vec3 low = color * 0.0773993805;
    vec3 high = pow((color + 0.055) * 0.9478673, vec3(2.4));
    return mix(low, high, step(vec3(0.04045), color));
}

vec3 fromLinear(vec3 color) {
    if (ColorGrading_Misc.y != 0.0) {
        return pow(
            max(color, vec3(0.0)),
            vec3(1.0 / (2.2 * ColorGrading_Misc.x))
        );
    }
    vec3 low = color * 12.92;
    vec3 high = pow(abs(color), vec3(0.41666666)) * 1.055 - 0.055;
    vec3 srgb = mix(low, high, step(vec3(0.0031308), color));
    return pow(srgb, vec3(2.2 / (2.2 * ColorGrading_Misc.x)));
}

void main() {
    vec3 inputColor = texture(s_ColorTexture, v_texcoord0.xy).rgb;
    vec3 outputColor = fromLinear(toLinear(inputColor));
    fragColor = vec4(
        clamp(outputColor * RDL_OutputExposure.r, 0.0, 1.0),
        1.0
    );
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_MiddleGray.json": """{
  "name": "RDL_MiddleGray",
  "type": "vec4",
  "count": 1,
  "default": [0.18, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_ExposureMultiplier.json": """{
  "name": "RDL_ExposureMultiplier",
  "type": "vec4",
  "count": 1,
  "default": [100.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_BorderFog.json": """{
  "name": "RDL_BorderFog",
  "type": "vec4",
  "count": 1,
  "default": [0.1, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_AerialScale.json": """{
  "name": "RDL_AerialScale",
  "type": "vec4",
  "count": 1,
  "default": [1.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_AtmosphereDensity.json": """{
  "name": "RDL_AtmosphereDensity",
  "type": "vec4",
  "count": 1,
  "default": [1.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_CirrusHeight.json": """{
  "name": "RDL_CirrusHeight",
  "type": "vec4",
  "count": 1,
  "default": [580.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_SkyAmbient.json": """{
  "name": "RDL_SkyAmbient",
  "type": "vec4",
  "count": 1,
  "default": [2.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_SunRadiance.json": """{
  "name": "RDL_SunRadiance",
  "type": "vec4",
  "count": 1,
  "default": [1.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_MoonRadiance.json": """{
  "name": "RDL_MoonRadiance",
  "type": "vec4",
  "count": 1,
  "default": [0.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_BlockAmbient.json": """{
  "name": "RDL_BlockAmbient",
  "type": "vec4",
  "count": 1,
  "default": [0.01, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_Emissive.json": """{
  "name": "RDL_Emissive",
  "type": "vec4",
  "count": 1,
  "default": [0.1, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_AtmosphereHeight.json": """{
  "name": "RDL_AtmosphereHeight",
  "type": "vec4",
  "count": 1,
  "default": [100000.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_RayleighLum.json": """{
  "name": "RDL_RayleighLum",
  "type": "vec4",
  "count": 1,
  "default": [2.5, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_MieLum.json": """{
  "name": "RDL_MieLum",
  "type": "vec4",
  "count": 1,
  "default": [0.5, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_Ozone.json": """{
  "name": "RDL_Ozone",
  "type": "vec4",
  "count": 1,
  "default": [5.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_CamAltitude.json": """{
  "name": "RDL_CamAltitude",
  "type": "vec4",
  "count": 1,
  "default": [100.0, 0.0, 0.0, 0.0]
}
""",
    "materials/GammaScreenColorPostProcessing/uniforms/RDL_OutputExposure.json": """{
  "name": "RDL_OutputExposure",
  "type": "vec4",
  "count": 1,
  "default": [1.0, 0.0, 0.0, 0.0]
}
""",
}


def parse_mirai_properties(text: str):
    options = []
    groups = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        if key.startswith("option."):
            name = key[7:]
            tokens = [part for part in re.split(r"[\s,]+", value) if part]
            if not tokens:
                option = {
                    "type": "toggle",
                    "macro": name,
                    "default": False,
                    "group": "Main",
                    "langKey": f"option.{name}",
                }
            elif all(re.fullmatch(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)", token) for token in tokens):
                option = {
                    "type": "slider",
                    "macro": name,
                    "min": float(tokens[0]),
                    "max": float(tokens[1]),
                    "default": float(tokens[2] if len(tokens) > 2 else tokens[0]),
                    "step": 0.1,
                    "group": "Main",
                    "langKey": f"option.{name}",
                }
            elif len(tokens) > 1 and all(
                re.fullmatch(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)", token) for token in tokens[1:]
            ):
                option = {
                    "type": "select",
                    "macro": name,
                    "default": tokens[0],
                    "macroMap": {token: float(index) for index, token in enumerate(tokens[1:])},
                    "group": "Main",
                    "langKey": f"option.{name}",
                }
            else:
                option = {
                    "type": "select",
                    "macro": name,
                    "default": tokens[0],
                    "macroMap": {token: float(index) for index, token in enumerate(tokens)},
                    "group": "Main",
                    "langKey": f"option.{name}",
                }
            options.append(option)
        elif key.startswith("screen.MAIN."):
            group = key.split(".", 2)[2]
            groups[group] = [part for part in re.split(r"[\s,]+", value) if part]

    for option in options:
        for group, names in groups.items():
            if option["macro"] in names:
                option["group"] = group
                break
    return options, groups


_NEWB_DEFINE = re.compile(
    r"^\s*(//)?#define\s+(NL_[A-Z0-9_]+)(?:\s+([^/\n]+?))?\s*(?://\s*(.*))?$"
)

# 通用版（不限 NL_ 前缀）：Mirai 的宏叫 SKY_AMBIENT_INTENSITY / CLOUD_HEIGHT 这种裸名。
_ANY_DEFINE = re.compile(
    r"^\s*(//)?#define\s+([A-Za-z_][A-Za-z0-9_]*)\b[ \t]*(.*)$"
)

# 这些宏由 subpack（pack_config.toml 的 [[subpack]] 开关）而不是用户参数控制，
# 不能从源码里当普通选项扫出来，否则会重复/互相覆盖。
_NEWB_SUBPACK_MACROS = {
    "NL_CLOUD_TYPE",
    "NL_CHUNK_LOAD_ANIM",
    "NL_UNDERWATER_STREAKS",
    "NL_GLOW_SHIMMER",
    "NL_LAVA_NOISE",
    "NL_WEATHER_SPECK",
    "NL_SHOOTING_STAR",
    "NL_CLOUD_AURORA_REFLECTION",
    "NL_RAIN_MIST_OPACITY",
    "NL_CLOUDY_FOG",
    "NL_ENTITY_EDGE_HIGHLIGHT",
    "NL_FOG",
}

# Continuous NewBx parameters migrated from compile-time macros to runtime vec4
# uniforms. `material: "*"` patches every material in the compiled preset.
NEWB_RUNTIME_UNIFORMS = {
    "NL_EXPOSURE": {
        "uniform": "RDL_Exposure",
        "components": [0],
    },
    "NL_GAMMA": {
        "uniform": "RDL_Gamma",
        "components": [0],
    },
    "NL_MIST_DENSITY": {
        "uniform": "RDL_MistDensity",
        "components": [0],
    },
    "NL_WAVE_SPEED": {
        "uniform": "RDL_WindSpeed",
        "components": [0],
    },
    "NL_GROUND_RAIN_PUDDLES": {
        "uniform": "RDL_WaterPuddles",
        "components": [0],
    },
}

NEWB_RUNTIME_EXTRA_OPTIONS = [
    {
        "type": "vec4",
        "control": "slider",
        "kind": "uniform",
        "name": "NL_FOG_STRENGTH",
        "macro": "NL_FOG_STRENGTH",
        "material": "*",
        "uniform": "RDL_FogStrength",
        "components": [0],
        "min": 0.0,
        "max": 1.5,
        "default": 1.0,
        "step": 0.01,
        "group": "Fog",
        "after": "NL_MIST_DENSITY",
        "langKey": "option.NL_FOG_STRENGTH",
    },
    {
        "type": "vec4",
        "control": "slider",
        "kind": "uniform",
        "name": "NL_CLOUD_HEIGHT",
        "macro": "NL_CLOUD_HEIGHT",
        "material": "*",
        "uniform": "RDL_CloudHeight",
        "components": [0],
        "min": -128.0,
        "max": 128.0,
        "default": 0.0,
        "step": 1.0,
        "group": "Soft cloud settings",
        "after": "NL_CLOUD1_OPACITY",
        "langKey": "option.NL_CLOUD_HEIGHT",
    },
]


def parse_newb_source(text: str):
    """把 config.h 拆成基础段（用户可调参数）和 subpack 段。

    config.h 的前半段是作者给的默认值，后半段是 `#ifdef <SUBPACK>` 下的
    #undef/#define 覆盖 —— 后者必须交给源码自己的条件编译处理，不能当成
    用户参数再注入一遍。
    """
    base, _, subpack = text.partition("SUBPACK CONFIG")
    return base, subpack


def parse_newb_options(text: str):
    base, _ = parse_newb_source(text)
    options = []
    seen: set[str] = set()
    current_group = "Main"
    for raw in base.splitlines():
        group_match = re.search(r"/\*+\s*([^*]+?)\s*\*+/", raw)
        if group_match:
            current_group = group_match.group(1).strip()
            continue
        match = _NEWB_DEFINE.match(raw)
        if not match:
            continue
        commented, macro, value, comment = match.groups()
        if macro == "NL_CONFIG_H" or macro in _NEWB_SUBPACK_MACROS:
            continue
        if macro in seen:
            continue
        seen.add(macro)
        value = (value or "").strip()
        comment = (comment or "").strip()
        lang_key = f"option.{macro}"
        if value.startswith(("vec2", "vec3", "vec4", "ivec")):
            continue
        if not value:
            options.append(
                {
                    "type": "toggle",
                    "macro": macro,
                    "default": not bool(commented),
                    "group": current_group,
                    "langKey": lang_key,
                }
            )
            continue
        if not re.fullmatch(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)", value):
            continue

        number = float(value)
        range_match = re.search(
            r"([-+]?\d+(?:\.\d+)?)\s*(?:low|subtle|weak|dark|minimal|near|slow|thin|transparent|invisible)?\s*[~-]\s*([-+]?\d+(?:\.\d+)?)",
            comment,
            re.IGNORECASE,
        )
        enum_pairs = re.findall(r"(\d+)\s*:\s*([^,]+)", comment)
        if enum_pairs and number.is_integer():
            macro_map = {}
            default_label = str(int(number))
            for enum_value, label in enum_pairs:
                label = label.strip()
                macro_map[label] = float(enum_value)
                if int(enum_value) == int(number):
                    default_label = label
            options.append(
                {
                    "type": "select",
                    "macro": macro,
                    "default": default_label,
                    "macroMap": macro_map,
                    "group": current_group,
                    "langKey": lang_key,
                }
            )
        else:
            minimum = float(range_match.group(1)) if range_match else 0.0
            maximum = float(range_match.group(2)) if range_match else max(abs(number) * 2.0, 1.0)
            # 注释里写的"范围下限"往往是视觉建议值，不是"关闭"值。
            # NewBx 的摆动系列用 `#ifdef NL_XXX` 判断开关，滑条如果卡在 0.05，
            # 玩家永远关不掉摆动 —— 所以这类项一律把下限放到 0。
            if macro in ("NL_LANTERN_WAVE", "NL_PLANTS_WAVE"):
                minimum = 0.0
            options.append(
                {
                    "type": "slider",
                    "macro": macro,
                    "min": minimum,
                    "max": maximum,
                    "default": number,
                    "step": 1.0 if number.is_integer() else 0.01,
                    "group": current_group,
                    "langKey": lang_key,
                }
            )
    return options


def migrate_newb_options_to_uniforms(options):
    for option in options:
        spec = NEWB_RUNTIME_UNIFORMS.get(option["macro"])
        if spec is None:
            continue
        option["kind"] = "uniform"
        option["control"] = option.get("type", "slider")
        option["type"] = "vec4"
        option["material"] = "*"
        option["uniform"] = spec["uniform"]
        option["components"] = spec["components"]
    extras = [json.loads(json.dumps(option)) for option in NEWB_RUNTIME_EXTRA_OPTIONS]
    result = []
    for option in options:
        result.append(option)
        for extra in list(extras):
            if extra.get("after") != option["macro"]:
                continue
            extra = dict(extra)
            extra.pop("after", None)
            result.append(extra)
            extras.remove(next(item for item in extras if item["uniform"] == extra["uniform"]))
    for extra in extras:
        extra = dict(extra)
        extra.pop("after", None)
        result.append(extra)
    options[:] = result
    return options


def newb_user_header(text: str, options) -> str:
    """生成 config.user.h：把作者默认值 + 用户当前值补齐给编译器。

    为什么要这个文件：config.h 里被注释掉的开关（以及 vec3 颜色）默认是
    “未定义”，而源码里会直接引用（例如 NL_UNDERWATER_TINT）。只把
    options.json 的参数塞进命令行，缺值的地方就会
    `error X3004: undeclared identifier`。这里按源码默认值补全。

    关键：必须用 `#ifndef` 守卫，不能写 `#undef` + `#define`。
    shaderc 的命令行 `-d NAME VALUE` 等价于在**最前面** `#define NAME VALUE`，
    而 config.user.h 是在 config.h 之后展开的；如果这里 `#undef` 再 `#define`
    默认值，就会把玩家在界面里改的值整个盖掉 —— 表现就是"怎么调参数画面都
    不动"。带上守卫之后：命令行定义了就用玩家的值，没定义才落作者默认值。
    """
    base, _ = parse_newb_source(text)
    defaults: dict[str, str] = {}
    for raw in base.splitlines():
        match = _NEWB_DEFINE.match(raw)
        if not match:
            continue
        _, macro, value, _ = match.groups()
        defaults.setdefault(macro, (value or "").strip())

    lines = [
        "// Generated by tools/adapt_stage8.py - do not edit by hand.",
        "#ifndef NL_CONFIG_USER_H",
        "#define NL_CONFIG_USER_H",
        "",
    ]
    for option in options:
        if option.get("kind") == "uniform":
            continue
        macro = option["macro"]
        # 由 subpack 开关（config.h 的 #ifdef 段）决定的宏不在这里补，
        # 否则会先把宏定义出来，把 config.h 自己的分支判反。
        if macro in _NEWB_SUBPACK_MACROS:
            continue
        raw = defaults.get(macro, "")
        if not raw:
            continue
        lines.append(f"#ifndef {macro}")
        lines.append(f"#define {macro} {raw}")
        lines.append("#endif")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


def make_lang(options, groups, chinese: bool):
    lines = []
    for group in groups:
        label = (NEWB_GROUP_ZH.get(group) or MIRAI_GROUP_ZH.get(group) or group) if chinese else group
        lines.append(f"group.{group}={label}")
    for option in options:
        macro = option["macro"]
        label = (
            NEWB_OPTION_ZH.get(macro) or MIRAI_OPTION_ZH.get(macro) or human_name(macro)
        ) if chinese else (MIRAI_OPTION_EN.get(macro) or human_name(macro))
        lines.append(f"option.{macro}={label}")
    return "\n".join(lines) + "\n"


# NewBx 的 config.h 把这些开关写成**无条件** `#define`，于是
# `#ifdef NL_PLANTS_WAVE` / `#ifdef NL_LANTERN_WAVE` 永远成立 —— 玩家把滑条
# 拉到 0，我们按"0 值不注入宏"处理，可是 config.h 自己又把它定义了回来，
# 摆动代码照旧编译进去。官方作者是靠 subpack 的 `#undef` 关的，而适配包固定用
# DEFAULT subpack，那段永远不激活。
#
# 所以这里把 config.h 里这些宏**注释掉**：定义权完全交给 config.user.h
# （= 玩家选项）。默认关闭，玩家在界面里打开才生效。
NEWB_CHOICE_DRIVEN_MACROS = (
    "NL_PLANTS_WAVE",
    "NL_LANTERN_WAVE",
    "NL_WAVE_SPEED",
)


def patch_newb_config_defaults(text: str) -> str:
    """把"开关型"宏在 config.h 里的定义注释掉，交给 config.user.h 决定。"""
    out: list[str] = []
    for raw in text.splitlines():
        stripped = raw.lstrip()
        if stripped.startswith("//"):
            out.append(raw)
            continue
        m = _NEWB_DEFINE.match(raw)
        if not m or m.group(2) not in NEWB_CHOICE_DRIVEN_MACROS:
            out.append(raw)
            continue
        indent = raw[: len(raw) - len(raw.lstrip())]
        # 整行注释掉，注释里保留原值，方便对照。
        out.append(f"{indent}//{stripped}   // disabled by adapt_stage8: choice-driven")
    return "\n".join(out) + "\n"


def add_newb_runtime_uniform_declarations(text: str) -> str:
    names = []
    for spec in NEWB_RUNTIME_UNIFORMS.values():
        if spec["uniform"] not in names:
            names.append(spec["uniform"])
    for option in NEWB_RUNTIME_EXTRA_OPTIONS:
        if option["uniform"] not in names:
            names.append(option["uniform"])
    declarations = "\n".join(
        f"uniform vec4 {name};"
        for name in names
    )
    marker = "#define NL_CONFIG_H\n"
    if marker not in text:
        raise ValueError("NewBx config.h has no NL_CONFIG_H guard")
    return text.replace(marker, marker + "\n" + declarations + "\n", 1)


def patch_newb_runtime_uniform_usage(text: str, file_name: str) -> str:
    if file_name == "tonemap.h":
        text = text.replace(
            "  #ifdef NL_EXPOSURE\n"
            "    col *= NL_EXPOSURE;\n"
            "  #endif\n",
            "  col *= RDL_Exposure.r;\n",
        )
        text = text.replace(
            "  #ifdef NL_EXPOSURE\n"
            "    col /= NL_EXPOSURE;\n"
            "  #endif\n",
            "  col /= RDL_Exposure.r;\n",
        )
        text = text.replace("NL_GAMMA", "RDL_Gamma.r")
    elif file_name == "fog.h":
        text = text.replace("NL_MIST_DENSITY", "RDL_MistDensity.r")
        text = text.replace(
            "return NL_FOG * fade;",
            "return NL_FOG * clamp(fade * RDL_FogStrength.r, 0.0, 1.0);",
        )
    elif file_name == "wave.h":
        text = text.replace("NL_WAVE_SPEED", "RDL_WindSpeed.r")
    elif file_name == "rain.h":
        text = text.replace("NL_GROUND_RAIN_PUDDLES", "RDL_WaterPuddles.r")
    return text


def patch_newb_cloud_height(text: str) -> str:
    text = text.replace(
        "      worldPos = mul(model, vec4(pos, 1.0)).xyz;\n",
        "      worldPos = mul(model, vec4(pos, 1.0)).xyz;\n"
        "      worldPos.y += RDL_CloudHeight.r;\n",
        1,
    )
    text = text.replace(
        "      worldPos.xyz = mul(model, vec4(pos, 1.0)).xyz;\n",
        "      worldPos.xyz = mul(model, vec4(pos, 1.0)).xyz;\n"
        "      worldPos.y += RDL_CloudHeight.r;\n",
        1,
    )
    return text


def guard_defines(text: str, macros: set[str]) -> str:
    """给源码里这些宏的**无条件** `#define` 套上 `#ifndef` 守卫。

    背景：Mirai 把全部可调参数都写成了源码里的无条件 `#define`
    （`#define SKY_AMBIENT_INTENSITY 4.0`），命令行注入的玩家值会被直接覆盖 ——
    "调了没反应"。NewBx 的摆动也是同一类问题。

    这里不去删默认值，而是改成
        #ifndef SKY_AMBIENT_INTENSITY
        #define SKY_AMBIENT_INTENSITY 4.0
        #endif
    这样：
      - 玩家调了 → 命令行先定义 → 源码默认值让位 → 生效；
      - 玩家没调 → 源码默认值仍然生效 → 不会变成"未定义"。
    """
    out: list[str] = []
    pending_guard: str | None = None
    for raw in text.splitlines():
        stripped = raw.strip()
        m = re.match(r"^#\s*ifndef\s+([A-Za-z_][A-Za-z0-9_]*)$", stripped)
        if m:
            pending_guard = m.group(1)
            out.append(raw)
            continue
        # 值后面常有多余空格（`#define X 4.0   // 注释`），
        # _NEWB_DEFINE 的贪婪匹配会因此失败 —— 先去掉行尾空格再匹配。
        m = _ANY_DEFINE.match(raw.rstrip())
        if m and m.group(2) in macros and not m.group(1) and pending_guard != m.group(2):
            out.append("#ifndef " + m.group(2))
            out.append(raw)
            out.append("#endif")
            pending_guard = None
            continue
        if stripped.startswith("#endif"):
            pending_guard = None
        out.append(raw)
    return "\n".join(out) + "\n"


def apply_mirai_uniform_options(options):
    for option in options:
        spec = MIRAI_RUNTIME_UNIFORMS.get(option["macro"])
        if spec is None:
            continue
        uniform, _ = spec
        if option["macro"] == "EXPOSURE_MULTIPLIER":
            option.update(
                {
                    "type": "vec4",
                    "min": 0.4,
                    "max": 2.0,
                    "default": 1.0,
                    "step": 0.01,
                    "kind": "uniform",
                    "control": "slider",
                    "components": [0],
                    "material": "ColorPostProcessing",
                    "uniform": "RDL_OutputExposure",
                }
            )
        else:
            option.update(
                {
                    "type": "vec4",
                    "kind": "uniform",
                    "control": "slider",
                    "components": [0],
                    "step": MIRAI_RUNTIME_STEPS[option["macro"]],
                }
            )
            option["material"] = "*"
            option["uniform"] = uniform


def inject_mirai_uniform_declarations(
    text: str,
    local_uniforms: dict[str, float] | None = None,
) -> str:
    names = [
        uniform for uniform, _ in MIRAI_RUNTIME_UNIFORMS.values()
    ]
    names.extend((local_uniforms or {}).keys())
    declarations = "\n".join(
        f"uniform vec4 {uniform};" for uniform in names
    )
    if "uniform vec4 RDL_MiddleGray;" in text:
        return text

    lines = text.splitlines(keepends=True)
    insert_at = 0
    for index, line in enumerate(lines):
        if line.lstrip().startswith("#version"):
            insert_at = index + 1
            break
    lines.insert(insert_at, "\n" + declarations + "\n")
    return "".join(lines)


def patch_mirai_uniform_usage(text: str, file_name: str = "") -> str:
    replacements = {
        macro: uniform
        for macro, (uniform, _) in MIRAI_RUNTIME_UNIFORMS.items()
    }
    out: list[str] = []
    for line in text.splitlines(keepends=True):
        if not line.lstrip().startswith("#"):
            for macro, uniform in replacements.items():
                line = re.sub(rf"\b{macro}\b", f"{uniform}.r", line)
        out.append(line)
    result = "".join(out)
    if file_name == "atmosphere.glsl":
        # Keep the original look at the default 0.5, but make the slider a
        # linear Mie-scattering scale instead of a cap that mostly cancels out.
        result = result.replace(
            "    atm.M = (1.0 - exp(-atm.opticalDepth\n"
            "        * atm.densityM\n"
            "        * params.cMie / RDL_MieLum.r)) * RDL_MieLum.r;",
            "    atm.M = (1.0 - exp(-atm.opticalDepth\n"
            "        * atm.densityM\n"
            "        * params.cMie / MIE_MAX_LUM)) * max(RDL_MieLum.r, 0.0);",
            1,
        )
    if file_name == "color_post_processing.glsl":
        result = result.replace(
            "    float exposure = (RDL_MiddleGray.r / refLuminance) * ExposureCompensation.g;\n"
            "    inputColor *= exposure;\n",
            "    float exposure = (RDL_MiddleGray.r / refLuminance) * ExposureCompensation.g\n"
            "        * RDL_OutputExposure.r;\n"
            "    inputColor *= exposure;\n",
            1,
        )
    return result


def add_overlay(zf, prefix: str, options, variants, backend, preset, profiles=("windows", "shading")):
    manifest = {
        "packVersion": 1,
        "name": preset,
        "gameVersion": "1.26.51",
        "defaultPreset": "default",
        "profiles": list(profiles),
        "backend": backend,
        "presets": [
            {
                "id": "default",
                "label": "Default",
                "settings": "presets/default/settings.txt",
                "bins": "presets/default/renderer/materials",
            }
        ],
    }
    zf.writestr(f"{prefix}rdloader.pack.json", json_bytes(manifest))
    zf.writestr(f"{prefix}options.json", json_bytes({"options": options}))
    zf.writestr(f"{prefix}variants.json", json_bytes({"variants": variants}))
    groups = []
    for option in options:
        if option.get("group") and option["group"] not in groups:
            groups.append(option["group"])
    zf.writestr(f"{prefix}lang/zh_CN.lang", make_lang(options, groups, True).encode("utf-8"))
    zf.writestr(f"{prefix}lang/en_US.lang", make_lang(options, groups, False).encode("utf-8"))
    zf.writestr(f"{prefix}presets/default/settings.txt", b"# Generated preset; values come from options.json.\n")


MIRAI_SHADER_SUFFIXES = (".glsl", ".sc", ".h", ".frag", ".vert", ".comp")


def mirai_project_prefix(names) -> str:
    """Mirai 2.x 的 zip 是平铺的；3.0 起源码在 <top>/src/ 下。"""
    if "project.json" in names:
        return ""
    for name in names:
        if name.endswith("/src/project.json"):
            return name[: -len("project.json")]
    raise ValueError("mirai pack has no project.json")


def adapt_mirai(source: Path, destination: Path, properties: Path) -> int:
    options, _ = parse_mirai_properties(properties.read_text(encoding="utf-8"))
    apply_mirai_uniform_options(options)
    option_macros = {option["macro"] for option in options}
    # Mirai 3.0 已经不再用 VOLUMETRIC_CLOUDS_ENABLED 这个开关，只有一个 base 变体。
    variants = [{"id": "base", "macros": []}]
    kept_vanilla = 0
    material_names: set[str] = set()
    source_rels: set[str] = set()
    with zipfile.ZipFile(source, "r") as src, zipfile.ZipFile(
        destination, "w", zipfile.ZIP_DEFLATED
    ) as dst:
        names = src.namelist()
        prefix = mirai_project_prefix(names)
        for info in src.infolist():
            if not info.filename.startswith(prefix):
                continue
            rel = info.filename[len(prefix):]
            if not rel or info.is_dir():
                continue
            source_rels.add(rel)
            parts = rel.split("/")
            if len(parts) >= 3 and parts[0] == "materials":
                material_names.add(parts[1])
            if "vanilla" in parts[:-1]:
                # 合并源只保留新一代 .material.json（format 2 / 材质 v26）。
                # - 旧 Mirai 包里的 vanilla 是 format 1 / v25，compat 会直接拒绝
                #   （Unsupported material.json format version），所以必须丢掉；
                # - .material.bin 也不随包分发：shaderc 路径下模组会从游戏目录按名暂存。
                if not rel.endswith(".material.json"):
                    continue
                try:
                    head = json.loads(src.read(info).decode("utf-8"))
                except Exception:
                    continue
                if not (isinstance(head, list) and len(head) >= 2 and head[0] == 2):
                    continue
                dst.writestr(rel, src.read(info))
                kept_vanilla += 1
                continue
            data = src.read(info)
            # Mirai 把可调参数写成源码里的无条件 #define，会把命令行注入的
            # 玩家值覆盖掉（"调了没反应"）。给它们套上 #ifndef 守卫：
            # 默认值保留，玩家调过就让位。
            if rel.endswith(MIRAI_SHADER_SUFFIXES):
                text = data.decode("utf-8", "replace")
                guarded = guard_defines(text, option_macros)
                if guarded != text:
                    data = guarded.encode("utf-8")
                text = data.decode("utf-8", "replace")
                text = patch_mirai_uniform_usage(text, Path(rel).name)
                if (
                    rel.startswith("materials/")
                    and rel.endswith((".frag", ".vert", ".comp"))
                ):
                    material = parts[1]
                    text = inject_mirai_uniform_declarations(
                        text,
                        MIRAI_LOCAL_UNIFORMS.get(material),
                    )
                data = text.encode("utf-8")
            dst.writestr(rel, data)
        for material in sorted(material_names):
            for macro, (uniform, source_default) in MIRAI_RUNTIME_UNIFORMS.items():
                rel = f"materials/{material}/uniforms/{uniform}.json"
                if rel in source_rels:
                    continue
                option = next(
                    (item for item in options if item["macro"] == macro),
                    None,
                )
                default = float(option["default"]) if option else source_default
                dst.writestr(
                    rel,
                    json_bytes(
                        {
                            "name": uniform,
                            "type": "vec4",
                            "count": 1,
                            "default": [default, 0.0, 0.0, 0.0],
                        }
                    ),
                )
            for uniform, default in MIRAI_LOCAL_UNIFORMS.get(material, {}).items():
                rel = f"materials/{material}/uniforms/{uniform}.json"
                if rel in source_rels:
                    continue
                dst.writestr(
                    rel,
                    json_bytes(
                        {
                            "name": uniform,
                            "type": "vec4",
                            "count": 1,
                            "default": [default, 0.0, 0.0, 0.0],
                        }
                    ),
                )
        # Mirai 3.0 的材质 config.json 全部是 albite（shaders/shader.{frag,vert,comp}），
        # 所以 overlay 的 backend 也要是 albite；profile 只有 windows。
        add_overlay(dst, "", options, variants, "albite", "Mirai", profiles=["windows"])
        if "shaders.properties" not in names:
            dst.writestr("shaders.properties", properties.read_bytes())

        # Mirai 3.0 的效果不只在材质里：源包的 pack/ 还带着雾、群系、
        # atmospherics / lighting / water 等资源包数据。适配包先原样收进
        # resource_pack/，模组以后可以把这层挂成游戏资源包；现在至少不会丢。
        pack_prefix = next(
            (name[: -len("manifest.json")] for name in names if name.endswith("/pack/manifest.json")),
            None,
        )
        if pack_prefix is not None:
            for info in src.infolist():
                if not info.filename.startswith(pack_prefix) or info.is_dir():
                    continue
                rel = "resource_pack/" + info.filename[len(pack_prefix):]
                dst.writestr(rel, src.read(info))
    return kept_vanilla


NEWB_FUNCTION_ORDER = [
    "tonemap.h",
    "detection.h",
    "fog.h",
    "sky.h",
    "clouds.h",
    "lighting.h",
    "water.h",
    "rain.h",
    "wave.h",
    "glow.h",
]

NEWB_IDENT = re.compile(r"[A-Za-z_]\w*")
NEWB_DEF_FUNC = re.compile(r"^\s*(?:[A-Za-z_][\w<>,\s\*]*?)\b([A-Za-z_]\w*)\s*\(", re.M)
NEWB_DEF_MACRO = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)", re.M)
NEWB_MAIN_INCLUDE = "#include <newb/main.sh>"


def plan_material_includes(pack_root: Path):
    """Work out which functions/*.h each material actually needs.

    Upstream every material includes newb/main.sh, which pulls in all 12
    function headers. That makes every material depend on every shader option
    macro, so changing one slider recompiles the whole pack (~20 s). The
    headers include their own dependencies and use include guards, so a
    material can include just the subset it references and still compile.
    """
    func_dir = pack_root / "src" / "newb" / "functions"
    defs = {}
    uses = {}
    for name in NEWB_FUNCTION_ORDER:
        path = func_dir / name
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        defs[name] = set(NEWB_DEF_MACRO.findall(text)) | set(NEWB_DEF_FUNC.findall(text))
        uses[name] = set(NEWB_IDENT.findall(text))

    result = {}
    mats_dir = pack_root / "src" / "materials"
    if not mats_dir.is_dir():
        return result
    for mat_dir in sorted(mats_dir.iterdir()):
        if not mat_dir.is_dir():
            continue
        needed = set()
        for file in mat_dir.rglob("*"):
            if file.is_file() and file.suffix in (".sc", ".glsl", ".h", ".inc", ".frag", ".vert", ".comp"):
                needed |= set(NEWB_IDENT.findall(file.read_text(encoding="utf-8", errors="replace")))
        chosen = set()
        while True:
            progress = False
            for name in NEWB_FUNCTION_ORDER:
                if name in chosen or name not in defs:
                    continue
                if defs[name] & needed:
                    chosen.add(name)
                    needed |= uses[name]
                    progress = True
            if not progress:
                break
        result[mat_dir.name] = [n for n in NEWB_FUNCTION_ORDER if n in chosen]
    return result


def rewrite_main_include(text: str, headers):
    if NEWB_MAIN_INCLUDE not in text:
        return text
    lines = ["#include <newb/config.h>", "#include <newb/config.user.h>"]
    lines += [f"#include <newb/functions/{name}>" for name in headers]
    return text.replace(NEWB_MAIN_INCLUDE, "\n".join(lines))


def adapt_newb(source: Path, destination: Path):
    if source.is_dir():
        root_dir = source
        temp_dir = None
        shutil.rmtree(destination.parent / "_newb_extract", ignore_errors=True)
    else:
        temp_dir = destination.parent / "_newb_extract"
        shutil.rmtree(temp_dir, ignore_errors=True)
        temp_dir.mkdir(parents=True, exist_ok=True)
        root_dir = temp_dir
        subprocess.run(
            ["tar.exe", "-xf", str(source), "-C", str(root_dir)],
            check=True,
        )
    try:
        config_file = next(root_dir.glob("*/src/newb/config.h"))
        pack_root = config_file.parent.parent.parent
        pack_config_file = pack_root / "src" / "newb" / "pack_config.toml"
        config_h = config_file.read_text(encoding="utf-8", errors="replace")
        pack_config = tomllib.loads(pack_config_file.read_text(encoding="utf-8"))
        options = migrate_newb_options_to_uniforms(parse_newb_options(config_h))
        user_header = newb_user_header(config_h, options)
        variants = []
        for index, subpack in enumerate(pack_config.get("subpack", [])):
            define = subpack.get("define")
            if not define:
                continue
            variants.append(
                {
                    "id": define.lower(),
                    "label": subpack.get("description", define).strip(),
                    "macros": [define],
                    "materials": subpack.get("materials", []),
                }
            )

        project = {
            "base_profile": {
                "platforms": ["Direct3D_SM50"],
                "merge_source": ["vanilla"],
                "include_patterns": ["materials/*"],
                "exclude_patterns": [".*", "_*"],
                "include_search_paths": ["src", "include"],
            },
            "profiles": {
                "windows": {"platforms": ["Direct3D_SM50"]},
                "windows65": {"platforms": ["Direct3D_SM65"]},
            },
        }

        with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as dst:
            for file in root_dir.rglob("*"):
                if not file.is_file():
                    continue
                # 跳过原始解压目录（newb-x-mcbe-16.55/…）：那是一整棵原始源码树，
                # 编译时会被一起复制进暂存目录，lazurite 扫描项目时要多走几万个文件，
                # 实测能把编译时间从十几秒推到三十多秒。下面已经有拍平后的
                # materials/ include/ src/newb/，这份原始副本对编译没用。
                if any(part.startswith("newb-x-mcbe") for part in file.relative_to(root_dir).parts[:-1]):
                    continue
                dst.write(file, file.relative_to(root_dir).as_posix())

            dst.writestr("src/newb/config.user.h", user_header.encode("utf-8"))

            material_includes = plan_material_includes(pack_root)
            for file in (pack_root / "src" / "materials").rglob("*"):
                if file.is_file():
                    rel = file.relative_to(pack_root / "src" / "materials")
                    arch = "materials/" + rel.as_posix()
                    material = rel.parts[0] if rel.parts else ""
                    if file.suffix in (".sc", ".glsl", ".frag", ".vert", ".comp") and material in material_includes:
                        text = file.read_text(encoding="utf-8", errors="replace")
                        trimmed = rewrite_main_include(text, material_includes[material])
                        if material == "Clouds" and file.name == "vertex.sc":
                            trimmed = patch_newb_cloud_height(trimmed)
                        dst.writestr(arch, trimmed.encode("utf-8"))
                    else:
                        dst.write(file, arch)
            for file in (pack_root / "include").rglob("*"):
                if file.is_file():
                    arch = "include/" + file.relative_to(pack_root / "include").as_posix()
                    dst.write(file, arch)
            for file in (pack_root / "src" / "newb").rglob("*"):
                if file.is_file():
                    if file.name in ("main.sh", "config.h"):
                        continue  # 下面会写补丁过的版本
                    arch = "src/newb/" + file.relative_to(pack_root / "src" / "newb").as_posix()
                    if file.name in ("tonemap.h", "fog.h", "wave.h", "rain.h"):
                        text = file.read_text(encoding="utf-8", errors="replace")
                        dst.writestr(
                            arch,
                            patch_newb_runtime_uniform_usage(text, file.name).encode("utf-8"),
                        )
                    else:
                        dst.write(file, arch)

            # config.h：把"开关型"宏注释掉，定义权交给 config.user.h（玩家选项）。
            patch_newb = patch_newb_config_defaults(
                (pack_root / "src" / "newb" / "config.h").read_text(
                    encoding="utf-8", errors="replace"
                )
            )
            patch_newb = add_newb_runtime_uniform_declarations(patch_newb)
            dst.writestr("src/newb/config.h", patch_newb.encode("utf-8"))

            # 最后再写补丁过的 main.sh，覆盖上面 rglob 带进来的原文件：
            # 紧跟作者 config.h 之后加载用户配置，源码里的条件编译（subpack 段）
            # 仍然由 config.h 自己处理。
            main_sh = (pack_root / "src" / "newb" / "main.sh").read_text(
                encoding="utf-8", errors="replace"
            )
            main_sh = main_sh.replace(
                '#include "config.h"',
                '#include "config.h"\n#include "config.user.h"',
                1,
            )
            dst.writestr("src/newb/main.sh", main_sh.encode("utf-8"))

            uniform_defaults = {
                option["uniform"]: float(option["default"])
                for option in options
                if option.get("kind") == "uniform"
            }
            for mat_dir in (pack_root / "src" / "materials").iterdir():
                if not mat_dir.is_dir():
                    continue
                for uniform_name, default in uniform_defaults.items():
                    dst.writestr(
                        f"materials/{mat_dir.name}/uniforms/{uniform_name}.json",
                        json_bytes(
                            {
                                "name": uniform_name,
                                "type": "vec4",
                                "count": 1,
                                "default": [default, 0.0, 0.0, 0.0],
                            }
                        ),
                    )

            dst.writestr("project.json", json_bytes(project))
            add_overlay(dst, "", options, variants, "shaderc", "NewbX")
    finally:
        if temp_dir is not None:
            shutil.rmtree(temp_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mirai", type=Path, required=True)
    parser.add_argument("--newb", type=Path, required=True)
    parser.add_argument("--mirai-properties", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--only",
        choices=["both", "mirai", "newb"],
        default="both",
        help="只适配其中一个包（默认两个都做）",
    )
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    if args.only in ("both", "mirai"):
        kept = adapt_mirai(args.mirai, args.out / "Mirai-adapted.zip", args.mirai_properties)
        print(f"wrote {args.out / 'Mirai-adapted.zip'} (vanilla merge source: {kept} json)")
    if args.only in ("both", "newb"):
        adapt_newb(args.newb, args.out / "NewbX-adapted.zip")
        print(f"wrote {args.out / 'NewbX-adapted.zip'}")


if __name__ == "__main__":
    main()
