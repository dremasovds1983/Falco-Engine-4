uniform mat4 worldViewProj;
uniform mat4 world;
uniform mat4 shadowViewProj;

attribute vec4 vertex;
attribute vec3 normal;
attribute vec3 tangent;
attribute vec4 uv0;
attribute vec2 uv1;

varying vec2 vUV0;
varying vec2 vUV1;
varying vec4 fragPos;
varying vec3 vNormal;
varying vec3 vTangent;
varying vec4 oUv;
varying mat4 modelMatrix;

void main()
{
	gl_Position = worldViewProj * vertex;
	fragPos = vertex;
	vNormal = normal;
	vTangent = tangent;
	vUV0 = uv0.xy;
	vUV1 = uv1;
	vec4 worldPos = world * vertex;
	oUv = shadowViewProj * worldPos;
	modelMatrix = world;
}
