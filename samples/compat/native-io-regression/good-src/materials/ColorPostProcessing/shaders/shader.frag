#version 450

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
uniform vec4 RDL_Ozone;
uniform vec4 RDL_CamAltitude;
uniform vec4 RDL_OutputExposure;

#include "lib/binding_helper.glsl"
#include "color_post_processing.glsl"
