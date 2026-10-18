#version 150

out vec4 fragData[8];
in vec3 oViewPos;
in vec2 oUv0;
in vec2 oUv1;
in mat3 v_TBNv;
in mat3 v_TBNw;

#ifdef CUTOUT
uniform float cCutoutValue;
#endif

uniform vec3 cDiffuseColor;
uniform float cSpecularity;
uniform vec2 cUVScale;

#ifdef HAS_DIFFUSE
	uniform sampler2D sDiffuseMap;
#endif

#ifdef HAS_NORMAL
	uniform sampler2D sNormalMap;
#endif

#ifdef HAS_METALLIC
	uniform sampler2D sMetallicMap;
#endif

#ifndef HAS_METALLIC
	uniform float cMetalness;
#endif

uniform float cReflectance;

#ifdef HAS_OCCLUSION
	uniform sampler2D sOcclusionMap;
	uniform float cOcclusionFactor;
#endif

#ifdef USE_IBL
	uniform samplerCube sEnvMap;
	uniform float cIBLFactor;
#endif

#ifdef HAS_EMISSION
	uniform sampler2D sEmissionMap;
	uniform float cEmissionFactor;
#endif

#ifndef HAS_EMISSION
	uniform float cEmissionFactor;
#endif

uniform int useLightMap;
uniform sampler2D lightMap;
uniform float farClipDistance;

void main()
{	
	vec2 _uv = cUVScale;
	
	if (_uv.x == 0.0)
		_uv.x = 1.0;
	if (_uv.y == 0.0)
		_uv.y = 1.0;
	
	_uv = oUv0 * _uv;
	
	vec3 lightmap = vec3(0.0, 0.0, 0.0);
	if (useLightMap == 1)
	{
		lightmap = texture(lightMap, oUv1).rgb * 0.5;
	}
	
	#ifdef HAS_DIFFUSE
		vec4 diffuse = texture(sDiffuseMap, _uv);
		
		#ifdef CUTOUT
			if (diffuse.a < cCutoutValue)
				discard;
		#endif
	
		fragData[0].rgb = diffuse.rgb * cDiffuseColor;
	#else
		fragData[0].rgb = cDiffuseColor;
	#endif

	fragData[0].a = cSpecularity;
	
	#ifdef HAS_NORMAL
		vec3 texNormal = (texture(sNormalMap, _uv).rgb);
		fragData[1].rgb = normalize(v_TBNv * (2.0 * texNormal - 1.0));
		//fragData[2].rgb = normalize(v_TBNw * (2.0 * texNormal - 1.0));
	#else
		fragData[1].rgb = v_TBNv[2].xyz;
		//fragData[2].rgb = v_TBNw[2].xyz;
	#endif

	fragData[1].a = length(oViewPos) / farClipDistance;
	
	///Reflectance
	fragData[2].r = cReflectance;
	
	#ifdef HAS_METALLIC
		//fragData[2].rgb = texture(sMetallicMap, _uv).rgb;
		fragData[2].a = texture(sMetallicMap, _uv).r;
	#else
		//fragData[2].rgb = vec3(0.5, 0.5, 0.5);
		fragData[2].a = cMetalness;
	#endif
	
	#ifdef HAS_OCCLUSION
		fragData[3].rgb = texture(sOcclusionMap, _uv).rgb;
		fragData[3].a = cOcclusionFactor;
	#else
		fragData[3].rgb = vec3(1);
		fragData[3].a = 1.0;
	#endif
	
	#ifdef USE_IBL
		#ifdef HAS_NORMAL
			vec3 n = normalize(v_TBNw * (2.0 * texNormal - 1.0));
			fragData[4].rgb = texture(sEnvMap, n).rgb;
			fragData[4].a = cIBLFactor;
		#else
			fragData[4].rgb = vec3(0);
			fragData[4].a = 0;
		#endif
	#else
		fragData[4].rgb = vec3(0);
		fragData[4].a = 0;
	#endif
	
	#ifdef HAS_EMISSION
		fragData[5].rgb = texture(sEmissionMap, _uv).rgb;
		fragData[5].a = cEmissionFactor;
	#else
		fragData[5].rgb = vec3(cDiffuseColor);
		fragData[5].a = cEmissionFactor;
	#endif

	fragData[6].rgb = oViewPos;
	
	fragData[7].rgb = lightmap;
	fragData[7].a = useLightMap;
}