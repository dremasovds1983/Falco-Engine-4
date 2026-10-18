uniform samplerCube cubemap;
uniform vec3 tintColor;

varying vec3 uv;

void main()
{
	vec4 skyColor = textureCube(cubemap, uv);
	
	gl_FragColor = vec4(skyColor.xyz * tintColor, 1.0);
}
