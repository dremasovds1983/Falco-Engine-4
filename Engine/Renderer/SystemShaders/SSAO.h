#pragma once
#include <string>
namespace shaders
{
// Depth-neighborhood occlusion in world units. Caller supplies depth/normal/UV
// conventions; no color or direct-light buffer is used by the estimator.
static const std::string ssaoFunctions = R"SHADER(
uniform vec4 u_ssaoParams; // world radius, strength, world bias, sample count (0 disables)
uniform mat4 u_camView;
uniform mat4 u_camProj;
uniform mat4 u_camInvView;
vec3 sceneViewDirection(vec3 worldPosition)
{
    // Orthographic rays are parallel: camera position must not bend them.
    vec3 direction = abs(u_camProj[3][3]) > 0.5
        ? mul(u_camInvView, vec4(0.0, 0.0, 1.0, 0.0)).xyz
        : u_camPos.xyz - worldPosition;
    float magnitudeSquared = dot(direction, direction);
    return magnitudeSquared > 0.000001 ? direction / sqrt(magnitudeSquared) : vec3(0.0, 0.0, 1.0);
}
vec3 aoWorldPosition(vec2 uv, float depth)
{
    vec2 clipUV = vec2(uv.x, mix(uv.y, 1.0 - uv.y, u_rtOrigin.x));
    vec4 p = mul(u_invVP, vec4(clipUV * 2.0 - 1.0, mix(depth, depth * 2.0 - 1.0, u_rtOrigin.y), 1.0));
    return p.xyz / p.w;
}
float sceneOcclusion(vec2 uv, float depth)
{
    if (u_ssaoParams.w < 0.5 || depth >= 1.0) return 1.0;
    vec3 origin = aoWorldPosition(uv, depth);
    vec3 normal = texture2D(u_normalMap, uv).xyz * 2.0 - 1.0;
    float normalLength = dot(normal, normal);
    if (normalLength < 0.00001) return 1.0;
    normal /= sqrt(normalLength);
    // G-buffer stores oriented normals; face towards the visible hemisphere.
    if (dot(normal, sceneViewDirection(origin)) < 0.0) normal = -normal;
    vec4 viewPosition = mul(u_camView, vec4(origin, 1.0));
    vec4 projected = mul(u_camProj, viewPosition);
    float radius = u_ssaoParams.x;
    vec2 screenRadius = vec2(abs(u_camProj[0][0]), abs(u_camProj[1][1])) * radius * 0.5 / max(abs(projected.w), 0.0001);
    screenRadius = min(screenRadius, vec2_splat(0.25));
    float occlusion = 0.0;
    float sampleCount = clamp(u_ssaoParams.w, 8.0, 32.0);
    for (int i = 0; i < 32; ++i)
    {
        if (float(i) >= sampleCount) break;
        float fraction = (float(i) + 0.5) / sampleCount;
        float angle = float(i) * 2.39996323;
        vec2 sampleUV = uv + vec2(cos(angle), sin(angle)) * sqrt(fraction) * screenRadius;
        if (sampleUV.x <= 0.0 || sampleUV.x >= 1.0 || sampleUV.y <= 0.0 || sampleUV.y >= 1.0) continue;
        float sampleDepth = texture2D(u_depthMap, sampleUV).r;
        if (sampleDepth >= 1.0) continue;
        vec3 delta = aoWorldPosition(sampleUV, sampleDepth) - origin;
        float distanceSquared = dot(delta, delta);
        if (distanceSquared < 0.000001 || distanceSquared >= radius * radius) continue;
        float distance = sqrt(distanceSquared);
        float horizon = max((dot(normal, delta) - u_ssaoParams.z) / distance, 0.0);
        occlusion += horizon * (1.0 - distanceSquared / (radius * radius));
    }
    return clamp(1.0 - u_ssaoParams.y * occlusion * (2.0 / sampleCount), 0.0, 1.0);
}
)SHADER";
}
