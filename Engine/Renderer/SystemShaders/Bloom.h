#pragma once
#include <string>
#include "Fog.h"
namespace shaders
{
static const std::string bloomFragment = R"SHADER($input v_texcoord0
uniform sampler2D u_albedoMap;
uniform vec4 u_bloomParams; // threshold, knee, intensity, unused
uniform vec4 u_bloomPass; // extraction=0/blur=1, texel step x/y, unused
)SHADER" + fogFunctions + R"SHADER(
vec3 bright(vec2 uv)
{
    vec3 c = max(texture2D(u_albedoMap, uv).rgb, vec3_splat(0.0));
    c = max(applySceneFog(c, uv), vec3_splat(0.0));
    float peak = max(c.r, max(c.g, c.b));
    float knee = max(u_bloomParams.y, 0.00001);
    float soft = clamp(peak - u_bloomParams.x + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    return c * (max(peak - u_bloomParams.x, soft) / max(peak, 0.00001));
}
void main()
{
    vec2 uv = vec2(v_texcoord0.x, mix(v_texcoord0.y, 1.0 - v_texcoord0.y, u_rtOrigin.x));
    vec2 stepUV = u_bloomPass.yz;
    vec3 c;
    if (u_bloomPass.x < 0.5)
    {
        // Cover the entire destination pixel footprint, including quarter-size
        // quality. Sampling only the central source texels loses small highlights.
        c = vec3_splat(0.0);
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 4; ++x)
            {
                vec2 offset = (vec2(float(x), float(y)) + vec2_splat(0.5)) * 0.25 - vec2_splat(0.5);
                c += bright(uv + stepUV * offset);
            }
        }
        c *= 0.0625;
    }
    else
    {
        c = texture2D(u_albedoMap, uv).rgb * 0.227027;
        c += (texture2D(u_albedoMap, uv + stepUV * 1.384615).rgb + texture2D(u_albedoMap, uv - stepUV * 1.384615).rgb) * 0.316216;
        c += (texture2D(u_albedoMap, uv + stepUV * 3.230769).rgb + texture2D(u_albedoMap, uv - stepUV * 3.230769).rgb) * 0.070270;
    }
    gl_FragColor = vec4(c, 1.0);
}
)SHADER";
}
