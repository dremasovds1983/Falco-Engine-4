#pragma once
#include <string>
#include "IBL.h"
namespace shaders
{
static const std::string irradianceFragment = R"SHADER($input v_texcoord0
uniform samplerCube u_envMap;
uniform vec4 u_cubeFace; // face, framebuffer origin flip, roughness, specular bake
)SHADER" + iblFunctions + R"SHADER(
vec3 prefilterEnvironment(vec3 n, float roughness)
{
    if (roughness < 0.0001) return max(textureCube(u_envMap, n).rgb, vec3_splat(0.0));
    vec3 t = iblTangent(n), b = cross(n, t);
    float a = roughness * roughness;
    float a2 = a * a;
    vec3 sum = vec3_splat(0.0);
    float totalWeight = 0.0;
    // Split-sum radiance convolution assumes V=N. The view-dependent BRDF
    // integral stays separate and is shared with the SSR replacement response.
    for (int i = 0; i < 128; ++i)
    {
        float u = (float(i) + 0.5) / 128.0;
        float phi = float(i) * 2.39996323;
        float nh = sqrt((1.0-u) / (1.0-u+a2*u));
        float sinTheta = sqrt(max(1.0-nh*nh, 0.0));
        vec3 h = t*(sinTheta*cos(phi)) + b*(sinTheta*sin(phi)) + n*nh;
        vec3 l = 2.0*nh*h-n;
        float weight = max(dot(n,l), 0.0);
        sum += max(textureCube(u_envMap,l).rgb, vec3_splat(0.0))*weight;
        totalWeight += weight;
    }
    return sum / max(totalWeight, 0.00001);
}
void main()
{
    vec2 uv = vec2(v_texcoord0.x, mix(v_texcoord0.y, 1.0-v_texcoord0.y, u_cubeFace.y));
    vec2 p = uv * 2.0 - 1.0;
    vec3 n;
    if (u_cubeFace.x < 0.5) n = vec3(1.0,-p.y,-p.x);
    else if (u_cubeFace.x < 1.5) n = vec3(-1.0,-p.y,p.x);
    else if (u_cubeFace.x < 2.5) n = vec3(p.x,1.0,p.y);
    else if (u_cubeFace.x < 3.5) n = vec3(p.x,-1.0,-p.y);
    else if (u_cubeFace.x < 4.5) n = vec3(p.x,-p.y,1.0);
    else n = vec3(-p.x,-p.y,-1.0);
    n = normalize(n);
    gl_FragColor = vec4(u_cubeFace.w > 0.5 ? prefilterEnvironment(n, u_cubeFace.z) : iblDiffuse(n), 1.0);
}
)SHADER";
}
