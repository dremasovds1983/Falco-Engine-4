#pragma once
#include <string>
namespace shaders
{
// Integrate the linear captured environment, rather than treating one cubemap
// texel as diffuse irradiance. Diffuse and GGX-prefiltered specular use caches
// when available; runtime environment quadrature remains the fallback.
static const std::string iblFunctions = R"SHADER(
uniform samplerCube u_irradianceMap;
uniform samplerCube u_prefilteredMap;
uniform sampler2D u_brdfMap;
uniform vec4 u_brdfCache; // valid, reserved
uniform vec4 u_environmentCache; // diffuse valid, bake samples, specular valid, maximum specular mip

vec3 iblTangent(vec3 n)
{
    vec3 axis = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    return normalize(cross(axis, n));
}
vec3 iblDiffuse(vec3 n)
{
    if (u_environmentCache.x > 0.5) return textureCube(u_irradianceMap, n).rgb;
    float sampleCount = clamp(u_environmentCache.y, 32.0, 128.0);
    vec3 t = iblTangent(n);
    vec3 b = cross(n, t);
    vec3 sum = vec3_splat(0.0);
    for (int i = 0; i < 128; ++i)
    {
        if (float(i) >= sampleCount) break;
        float u = (float(i) + 0.5) / sampleCount;
        float phi = float(i) * 2.39996323;
        vec3 l = t * (sqrt(u) * cos(phi)) + b * (sqrt(u) * sin(phi)) + n * sqrt(1.0 - u);
        sum += max(textureCube(u_envMap, l).rgb, vec3_splat(0.0));
    }
    // Cosine-weighted samples cancel the Lambert 1/pi and cosine factors.
    return sum / sampleCount;
}
float iblSmithG1(float nv, float a2)
{
    return 2.0 * nv / max(nv + sqrt(a2 + (1.0 - a2) * nv * nv), 0.00001);
}
vec3 iblSpecularIntegrated(vec3 n, vec3 v, vec3 f0, float roughness, out vec3 integratedBRDF)
{
    vec3 t = iblTangent(n);
    vec3 b = cross(n, t);
    float nv = max(dot(n, v), 0.0001);
    if (u_environmentCache.z > 0.5 && u_brdfCache.x > 0.5)
    {
        vec2 response = texture2D(u_brdfMap, vec2(nv, roughness)).rg;
        integratedBRDF = f0 * response.x + vec3_splat(response.y);
        vec3 radiance = textureCubeLod(u_prefilteredMap, reflect(-v, n), roughness * u_environmentCache.w).rgb;
        return max(radiance, vec3_splat(0.0)) * integratedBRDF;
    }
    float a = max(roughness * roughness, 0.0016);
    float a2 = a * a;
    vec3 sum = vec3_splat(0.0);
    integratedBRDF = vec3_splat(0.0);
    for (int i = 0; i < 32; ++i)
    {
        float u = (float(i) + 0.5) / 32.0;
        float phi = float(i) * 2.39996323;
        float nh = sqrt((1.0 - u) / (1.0 - u + a2 * u));
        float sinTheta = sqrt(max(1.0 - nh * nh, 0.0));
        vec3 h = t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + n * nh;
        float vh = max(dot(v, h), 0.0);
        vec3 l = 2.0 * vh * h - v;
        float nl = max(dot(n, l), 0.0);
        if (nl > 0.0 && vh > 0.0)
        {
            vec3 f = f0 + (vec3_splat(1.0) - f0) * pow(1.0 - vh, 5.0);
            float g = iblSmithG1(nv, a2) * iblSmithG1(nl, a2);
            // GGX NDF importance-sampling PDF cancels D and 4*V.H.
            vec3 weight = f * (g * vh / max(nh * nv, 0.00001));
            integratedBRDF += weight;
            if (u_environmentCache.z < 0.5)
                sum += max(textureCube(u_envMap, l).rgb, vec3_splat(0.0)) * weight;
        }
    }
    integratedBRDF /= 32.0;
    if (u_environmentCache.z > 0.5)
    {
        vec3 radiance = textureCubeLod(u_prefilteredMap, reflect(-v, n), roughness * u_environmentCache.w).rgb;
        return max(radiance, vec3_splat(0.0)) * integratedBRDF;
    }
    return sum / 32.0;
}
vec3 iblSpecular(vec3 n, vec3 v, vec3 f0, float roughness)
{
    vec3 integratedBRDF;
    return iblSpecularIntegrated(n, v, f0, roughness, integratedBRDF);
}
)SHADER";
}
