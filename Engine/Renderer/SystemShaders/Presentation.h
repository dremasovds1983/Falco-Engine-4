#pragma once
#include <string>
#include "Fog.h"
namespace shaders
{
    static const std::string presentationFragment = R"SHADER($input v_texcoord0
uniform sampler2D u_albedoMap;
uniform sampler2D u_bloomMap;
uniform vec4 u_bloomParams;
uniform vec4 u_presentation; // exposure, tonemap, brightness, contrast
uniform vec4 u_colorGrade; // saturation

)SHADER" + fogFunctions + R"SHADER(
// Same Uchimura curve parameters as DreamOwl's tonemapCS.hlsl.
float uchimura(float x)
{
    float m = 0.22;
    float shoulder = m + (1.0 - m) * 0.4;
    float toeWeight = 1.0 - smoothstep(0.0, m, x);
    float shoulderWeight = step(shoulder, x);
    float toe = m * pow(x / m, 1.33);
    float high = 1.0 - (1.0 - shoulder) * exp(-(x - shoulder) / (1.0 - shoulder));
    return toe * toeWeight + x * (1.0 - toeWeight - shoulderWeight) + high * shoulderWeight;
}
float encodeSRGB(float x)
{
    return x <= 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
}
void main()
{
    vec2 uv = vec2(v_texcoord0.x, mix(v_texcoord0.y, 1.0 - v_texcoord0.y, u_rtOrigin.x));
    vec4 source = texture2D(u_albedoMap, uv);
    vec3 color = max(source.rgb, vec3_splat(0.0));
    color = applySceneFog(color, uv);
    if (u_bloomParams.z > 0.0) color += texture2D(u_bloomMap, uv).rgb * u_bloomParams.z;
    color *= u_presentation.x;
    if (u_presentation.y < 0.5)
        color = color / (vec3_splat(1.0) + color);
    else if (u_presentation.y < 1.5)
        color = clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), vec3_splat(0.0), vec3_splat(1.0));
    else
        color = vec3(uchimura(color.r), uchimura(color.g), uchimura(color.b));
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3_splat(luminance), color, u_colorGrade.x);
    color = clamp((color - 0.18) * u_presentation.w + 0.18 + u_presentation.z, vec3_splat(0.0), vec3_splat(1.0));
    gl_FragColor = vec4(encodeSRGB(color.r), encodeSRGB(color.g), encodeSRGB(color.b), source.a);
}
)SHADER";
}
