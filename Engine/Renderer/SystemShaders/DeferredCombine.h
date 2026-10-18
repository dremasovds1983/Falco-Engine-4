#include <string>
#include "SSAO.h"
#include "IBL.h"

namespace shaders
{
	static std::string deferredCombineVarying = 
		"vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);\n"
		"\n"
		"vec3 a_position : POSITION;\n"
		"vec2 a_texcoord0 : TEXCOORD0;\n";

	static std::string deferredCombineVertex =
		"$input a_position, a_texcoord0\n"
		"$output v_texcoord0\n"
		"\n"
		"uniform mat4 u_modelViewProj;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));\n"
		"	v_texcoord0 = a_texcoord0;\n"
		"}\n";

    static std::string deferredCombineFragment = R"SHADER($input v_texcoord0
uniform sampler2D u_albedoMap;
uniform sampler2D u_depthMap;
uniform sampler2D u_lightBuffer;
uniform sampler2D u_lightMap;
uniform sampler2D u_normalMap;
uniform sampler2D u_mraMap;
uniform samplerCube u_envMap;
uniform vec4 u_ambientColor;
uniform vec4 u_giParams;
uniform vec4 u_camPos;
uniform vec4 u_rtOrigin;
uniform mat4 u_invVP;
)SHADER" + ssaoFunctions + iblFunctions + R"SHADER(
void main()
{
    vec2 uv = vec2(v_texcoord0.x, mix(v_texcoord0.y, 1.0 - v_texcoord0.y, u_rtOrigin.x));
    float depth = texture2D(u_depthMap, uv).r;
    if (depth >= 1.0) discard;
    vec4 color = texture2D(u_albedoMap, uv);
    vec3 direct = texture2D(u_lightBuffer, uv).rgb;
    vec4 mra = texture2D(u_mraMap, uv);
    vec4 lightmap = texture2D(u_lightMap, uv);
    float metallic = clamp(mra.x, 0.0, 1.0);
    float roughness = clamp(mra.y, 0.04, 1.0);
    vec3 n = texture2D(u_normalMap, uv).rgb * 2.0 - 1.0;
    n = dot(n,n) > 0.00001 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    vec3 v = sceneViewDirection(aoWorldPosition(uv, depth));
    if (dot(n,v) < 0.0) n = -n;
    vec3 indirect = color.rgb * u_ambientColor.rgb;
    if (u_giParams.x > 0.5)
    {
        if (u_giParams.z > 0.5)
        {
            vec3 f0 = mix(vec3_splat(0.04), color.rgb, metallic);
            vec3 diffuseWeight = (vec3_splat(1.0) - f0) * (1.0 - metallic);
            vec3 diffuse = color.rgb * diffuseWeight * iblDiffuse(n);
            vec3 specular = iblSpecular(n, v, f0, roughness) * clamp(mra.w, 0.0, 1.0);
            indirect = (diffuse + specular) * max(u_giParams.y, 0.0);
        }
        else indirect *= max(u_giParams.y, 0.0);
    }
    indirect *= clamp(mra.z, 0.0, 1.0) * sceneOcclusion(uv, depth);
    vec3 emission = color.rgb * max(color.a, 0.0);
    gl_FragColor = vec4(indirect + emission + color.rgb * lightmap.rgb * lightmap.a + direct, 1.0);
    gl_FragDepth = depth;
}
)SHADER";
}
