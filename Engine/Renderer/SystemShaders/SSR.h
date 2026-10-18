#pragma once
#include <string>
#include "SSAO.h"
#include "IBL.h"
namespace shaders
{
static const std::string ssrFragment = R"SHADER($input v_texcoord0
uniform sampler2D u_albedoMap;
uniform sampler2D u_depthMap;
uniform sampler2D u_forwardDepth;
uniform sampler2D u_lightBuffer; // immutable opaque HDR scene copy
uniform sampler2D u_normalMap;
uniform sampler2D u_mraMap;
uniform samplerCube u_envMap;
uniform vec4 u_camPos;
uniform vec4 u_rtOrigin;
uniform vec4 u_giParams;
uniform vec4 u_ssrParams; // maximum distance, thickness, unused, unused
uniform mat4 u_invVP;
)SHADER" + ssaoFunctions + iblFunctions + R"SHADER(
bool projectRay(vec3 p, out vec2 uv)
{
    vec4 clip = mul(u_camProj, vec4(p, 1.0));
    if (clip.w <= 0.00001) return false;
    vec3 ndc = clip.xyz / clip.w;
    if (ndc.z < mix(0.0, -1.0, u_rtOrigin.y) || ndc.z > 1.0) return false;
    uv = ndc.xy * 0.5 + 0.5;
    uv.y = mix(uv.y, 1.0 - uv.y, u_rtOrigin.x);
    return uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0;
}
bool rayDepthDifference(vec3 rayPoint, vec2 uv, out float difference)
{
    difference = 0.0;
    // Explicit LOD avoids implicit derivatives inside the divergent ray-march loop.
    float depth = texture2DLod(u_forwardDepth, uv, 0.0).r;
    if (depth >= 1.0) return false;
    vec3 world = aoWorldPosition(uv, depth);
    float sceneZ = mul(u_camView, vec4(world, 1.0)).z;
    difference = sceneZ - rayPoint.z; // positive once ray is behind the visible surface
    return true;
}
void main()
{
    if (u_giParams.x < 0.5 || u_giParams.z < 0.5) discard;
    vec2 uv = vec2(v_texcoord0.x, mix(v_texcoord0.y, 1.0 - v_texcoord0.y, u_rtOrigin.x));
    float depth = texture2D(u_depthMap, uv).r;
    if (depth >= 1.0) discard;
    vec3 origin = aoWorldPosition(uv, depth);
    float visibleDepth = texture2D(u_forwardDepth, uv).r;
    if (visibleDepth >= 1.0 || length(aoWorldPosition(uv, visibleDepth) - origin) > 0.01) discard;
    vec4 mra = texture2D(u_mraMap, uv);
    float roughness = clamp(mra.y, 0.04, 1.0);
    float roughFade = 1.0 - smoothstep(0.2, 0.6, roughness);
    if (roughFade <= 0.0 || mra.w <= 0.0) discard;
    vec3 n = texture2D(u_normalMap, uv).rgb * 2.0 - 1.0;
    if (dot(n,n) < 0.00001) discard;
    n = normalize(n);
    vec3 v = sceneViewDirection(origin);
    if (dot(n,v) < 0.0) n = -n;
    vec3 direction = mul(u_camView, vec4(reflect(-v,n), 0.0)).xyz;
    vec3 start = mul(u_camView, vec4(origin + n * 0.02, 1.0)).xyz;
    float maxDistance = u_ssrParams.x;
    float previousT = 0.0;
    // Only two valid geometry samples can bracket a crossing. Sky is not a
    // surface in front of the ray and must not create a hit at a silhouette.
    float previousDifference = 0.0;
    vec2 startUV;
    bool previousValid = false;
    if (projectRay(start, startUV))
        previousValid = rayDepthDifference(start, startUV, previousDifference);
    float confidence = 0.0;
    vec2 hitUV = uv;
    for (int i = 1; i <= 64; ++i)
    {
        float t = maxDistance * (float(i) / 64.0);
        vec3 p = start + direction * t;
        vec2 sampleUV;
        if (!projectRay(p, sampleUV)) break;
        float difference;
        bool sampleValid = rayDepthDifference(p, sampleUV, difference);
        if (sampleValid && previousValid && difference >= 0.0 && previousDifference < 0.0)
        {
            float low = previousT, high = t;
            bool bracketValid = true;
            for (int j = 0; j < 5; ++j)
            {
                float mid = (low + high) * 0.5;
                vec3 candidate = start + direction * mid;
                vec2 candidateUV;
                float candidateDifference;
                if (!projectRay(candidate, candidateUV)) { bracketValid = false; break; }
                if (!rayDepthDifference(candidate, candidateUV, candidateDifference))
                { bracketValid = false; break; }
                if (candidateDifference >= 0.0) high = mid;
                else low = mid;
            }
            vec3 candidate = start + direction * high;
            if (bracketValid && projectRay(candidate, hitUV))
            {
                float error;
                bool hitValid = rayDepthDifference(candidate, hitUV, error);
                if (hitValid && error >= 0.0 && error <= u_ssrParams.y && length(hitUV - uv) > 0.002)
                {
                    float edge = min(min(hitUV.x, hitUV.y), min(1.0-hitUV.x, 1.0-hitUV.y));
                    confidence = smoothstep(0.0, 0.08, edge) * (1.0 - high / maxDistance) * roughFade;
                    break;
                }
            }
        }
        previousT = t; previousDifference = difference; previousValid = sampleValid;
    }
    if (confidence <= 0.0) discard; // original environment contribution stays intact
    vec3 albedo = texture2D(u_albedoMap, uv).rgb;
    vec3 f0 = mix(vec3_splat(0.04), albedo, clamp(mra.x, 0.0, 1.0));
    vec3 integratedBRDF;
    vec3 environmentReflection = iblSpecularIntegrated(n,v,f0,roughness,integratedBRDF) * max(u_giParams.y,0.0);
    // The screen color already contains scene lighting. Apply the same integrated
    // specular response as the environment, without applying GI intensity twice.
    vec3 screenReflection = max(texture2D(u_lightBuffer, hitUV).rgb, vec3_splat(0.0)) * integratedBRDF;
    float weight = confidence * clamp(mra.w,0.0,1.0)
        * clamp(mra.z,0.0,1.0) * sceneOcclusion(uv,depth);
    // Signed additive replacement prevents double-counting the environment specular.
    gl_FragColor = vec4((screenReflection-environmentReflection)*weight,0.0);
}
)SHADER";
}
