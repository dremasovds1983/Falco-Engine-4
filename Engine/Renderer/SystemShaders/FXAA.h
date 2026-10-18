#pragma once
#include <string>
#include "FXAACommon.h"
namespace shaders
{
static const std::string fxaaVarying = R"SHADER(
vec3 a_position : POSITION;
vec2 a_texcoord0 : TEXCOORD0;
vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
)SHADER";
static const std::string fxaaVertex = R"SHADER($input a_position, a_texcoord0
$output v_texcoord0
uniform mat4 u_modelViewProj;
void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_texcoord0 = a_texcoord0;
}
)SHADER";
    static std::string fxaaFragment = R"SHADER($input v_texcoord0
uniform sampler2D u_albedoMap;
uniform vec4 u_screenParams;
uniform vec4 u_useFXAA;
uniform vec4 u_rtOrigin;
)SHADER" + fxaaFunctions + R"SHADER(void main()
{
    vec2 uv = vec2(v_texcoord0.x, mix(v_texcoord0.y, 1.0-v_texcoord0.y, u_rtOrigin.x));
    vec4 color = texture2D(u_albedoMap, uv);
    if (u_useFXAA.x > 0.5) color = falcoFXAA(u_albedoMap, uv, u_screenParams.xy);
    gl_FragColor = color;
}
)SHADER";
}
