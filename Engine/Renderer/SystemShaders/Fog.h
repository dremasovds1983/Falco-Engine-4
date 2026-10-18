#pragma once
#include <string>
namespace shaders
{
// Shared linear-HDR fog, evaluated before bright extraction and tonemapping.
// uv is in render-target sampling coordinates, not clip-space coordinates.
static const std::string fogFunctions = R"SHADER(
uniform sampler2D u_depthMap;
uniform vec4 u_rtOrigin;
uniform vec4 u_fogEnabled;
uniform vec4 u_fogParams;
uniform vec4 u_fogColor;
uniform mat4 u_camInvProj;
uniform mat4 u_camProj;
vec3 applySceneFog(vec3 color, vec2 uv)
{
    if (u_fogEnabled.x < 0.5) return color;
    uv = clamp(uv, vec2_splat(0.0), vec2_splat(1.0));
    float depth = texture2D(u_depthMap, uv).r;
    if (depth >= 1.0 && u_fogEnabled.y < 0.5) return color;
    vec2 clipUV = vec2(uv.x, mix(uv.y, 1.0 - uv.y, u_rtOrigin.x));
    vec4 clip = vec4(clipUV * 2.0 - 1.0, mix(depth, depth * 2.0 - 1.0, u_rtOrigin.y), 1.0);
    vec4 position = mul(u_camInvProj, clip);
    float rayDistance = abs(u_camProj[3][3]) > 0.5 ? abs(position.z) : length(position.xyz);
    float distance = rayDistance / max(abs(position.w), 0.000001);
    float fog;
    if (u_fogParams.w < 0.5)
        fog = 1.0 - clamp(((u_fogParams.y - distance) / max(u_fogParams.y - u_fogParams.x, 0.0001)) * (1.0 - u_fogParams.z), 0.0, 1.0);
    else if (u_fogParams.w < 1.5)
        fog = 1.0 - exp2(-max(distance * u_fogParams.z, 0.0));
    else
    {
        float opticalDepth = max(distance * u_fogParams.z, 0.0);
        fog = 1.0 - exp2(-opticalDepth * opticalDepth);
    }
    return mix(color, u_fogColor.rgb, clamp(fog * u_fogColor.a, 0.0, 1.0));
}
)SHADER";
}
