attribute vec2 uv0;
attribute vec2 uv1;
attribute vec4 vertex;

uniform mat4 worldViewProj;

varying vec2 vUV0;
varying vec2 vUV1;

void main()
{
	vUV0 = uv0;
	vUV1 = uv1;

    gl_Position = worldViewProj * vertex;
}