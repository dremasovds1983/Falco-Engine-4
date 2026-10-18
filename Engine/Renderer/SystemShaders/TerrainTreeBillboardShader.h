#include <string>
#include "TerrainTreeLighting.h"

namespace shaders
{
    // Capture only linear base color; scene lighting is applied at runtime.
    static const std::string terrainTreeCaptureFragment = R"SHADER($input v_texcoord0, v_position
uniform sampler2D u_albedoMap;
uniform vec4 u_color; // linear material tint
float captureSRGB(float x)
{
    x = max(x, 0.0);
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}
void main()
{
    vec4 base = texture2D(u_albedoMap, v_texcoord0);
    base.rgb = vec3(captureSRGB(base.r), captureSRGB(base.g), captureSRGB(base.b));
    base *= u_color;
    if (base.a < 0.3) discard;
    gl_FragColor = base;
}
)SHADER";

	static std::string terrainTreeBillboardFragment =
		"$input v_texcoord0, v_position\n"
		"\n"
		"uniform sampler2D u_albedoMap;\n"
		"uniform vec4 u_ambientColor;\n"
		"\n"
		"uniform samplerCube u_envMap;\n"
		"uniform vec4 u_camPos;\n"
		"uniform vec4 u_giParams;\n"
		"uniform vec4 u_normal;\n"
		"\n"
        + terrainTreeLighting +
		"void main()\n"
		"{\n"
		"	vec4 albedo = texture2D(u_albedoMap, v_texcoord0);\n"
		"	if (albedo.a < 0.3)\n"
		"		discard;\n"
		"	\n"
		"	vec3 color = albedo.rgb * treeIndirectLight(v_position);\n"
		"	gl_FragColor = vec4(color, 1.0);\n"
		"}\n";
}
