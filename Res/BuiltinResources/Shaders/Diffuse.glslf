varying vec2 vUV0;
varying vec2 vUV1;

#ifdef CUTOUT
uniform float cutoutValue;
#endif
uniform vec4 ambientColor;
uniform vec4 diffuseColor;
#ifdef HAS_DIFFUSE
uniform sampler2D diffuseMap;
#endif
#ifdef HAS_EMISSION
uniform float emissionFactor;
uniform vec4 emissionColor;
#endif

uniform int useLightMap;
uniform sampler2D lightMap;

void main()
{
#ifdef HAS_DIFFUSE
    vec4 color = texture2D(diffuseMap, vUV0);
#else
	vec4 color = vec4(1.0, 1.0, 1.0, 1.0);
#endif

    vec4 lightmap = texture2D(lightMap, vUV1);
	
#ifdef CUTOUT
	if (color.a < cutoutValue)
		discard;

#ifdef HAS_DIFFUSE
	//if (color.r + color.g + color.b > cutoutValue * 3.0)
	//	discard;
#endif
#endif

	vec4 oColour = ambientColor * color;
		
	if (useLightMap == 1)
		oColour = vec4((ambientColor + lightmap) * color);
	
#ifdef HAS_EMISSION
	vec3 emissive = color.rgb * emissionColor.rgb * emissionFactor;
	oColour += vec4(emissive, 0.0);
#endif

    gl_FragColor = vec4(oColour.rgb, color.a * diffuseColor.a);
}