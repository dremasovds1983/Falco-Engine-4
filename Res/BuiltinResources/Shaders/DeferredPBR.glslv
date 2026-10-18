#version 150

in vec4 vertex;
in vec3 normal;
in vec2 uv0;
in vec2 uv1;
in vec3 tangent;

out vec3 oViewPos;
out vec2 oUv0;
out vec2 oUv1;

uniform mat4 worldViewProj;
uniform mat4 worldView;
uniform mat4 world;

out mat3 v_TBNv;
out mat3 v_TBNw;

void main()
{
	gl_Position = worldViewProj * vertex;
	oViewPos = (worldView * vertex).xyz;
	oUv0 = uv0;
	oUv1 = uv1;
	
	vec3 oNormal1 = normalize((worldView * vec4(normal,0)).xyz);
	vec3 oTangent1 = normalize((worldView * vec4(tangent,0)).xyz);
	vec3 oBiNormal1 = cross(oNormal1, oTangent1);
	v_TBNv = mat3(oTangent1, oBiNormal1, oNormal1);
	
	vec3 normalW = normalize(vec3(world * vec4(normal, 0.0)));
	vec3 tangentW = normalize(vec3(world * vec4(tangent, 0.0)));
	vec3 bitangentW = cross(normalW, tangentW);
	v_TBNw = mat3(tangentW, bitangentW, normalW);
}