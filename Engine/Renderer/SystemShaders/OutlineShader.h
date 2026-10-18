#include <string>

namespace shaders
{
	static std::string outlineVarying =
		"vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);\n"
		"vec3 v_normal : NORMAL = vec3(0.0, 0.0, 1.0);\n"
		"\n"
		"vec3 a_position  : POSITION;\n"
		"vec2 a_texcoord0 : TEXCOORD0;\n"
		"vec3 a_normal    : NORMAL;\n"
		"vec4 a_indices   : BLENDINDICES;\n"
		"vec4 a_weight    : BLENDWEIGHT;\n";

	static std::string outlineVertex =
		"$input a_position, a_texcoord0, a_normal, a_indices, a_weight\n"
		"$output v_texcoord0, v_normal\n"
		"\n"
		"uniform mat4 u_modelViewProj;\n"
		"uniform mat4 u_viewProj;\n"
		"uniform mat4 u_model[32];\n"
		"uniform vec4 u_skinned;\n"
		"uniform mat4 u_boneMatrix[128];\n"
		"\n"
		"void main()\n"
		"{\n"
		"	mat4 model = u_model[0];\n"
		"	if (u_skinned.x == 1.0)\n"
		"	{\n"
		"		mat4 skin = a_weight.x * u_boneMatrix[int(a_indices.x)] +\n"
		"			a_weight.y * u_boneMatrix[int(a_indices.y)] +\n"
		"			a_weight.z * u_boneMatrix[int(a_indices.z)] +\n"
		"			a_weight.w * u_boneMatrix[int(a_indices.w)];\n"
		"		model = mul(model, skin);\n"
		"	}\n"
		"	gl_Position = mul(u_viewProj, mul(model, vec4(a_position, 1.0)));\n"
		"	v_texcoord0 = a_texcoord0;\n"
		"	v_normal = normalize(mul(u_viewProj, normalize(vec4(mul(model, vec4(a_normal, 0.0)).xyz, 1.0))).xyz);\n"
		"}\n";

	static std::string outlineFragment =
		"$input v_texcoord0, v_normal\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = vec4(v_normal * 0.5 + 0.5, 1.0);\n"
		"}\n";
}
