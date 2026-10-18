#ifdef CUTOUT
uniform float cutoutValue;
#endif
uniform vec4 diffuseColor;
#ifdef HAS_DIFFUSE
uniform sampler2D diffuseMap;
#endif
#ifdef HAS_NORMAL
uniform sampler2D normalMap;
#endif
uniform sampler2D shadowMap0;
uniform sampler2D shadowMap1;
uniform sampler2D shadowMap2;
uniform sampler2D shadowMap3;
uniform samplerCube shadowMapPoint;
//uniform vec4 ambientColor;
uniform vec4 lightDiffuse;
uniform vec4 lightSpecular;
uniform vec4 lightPosition;
uniform vec4 lightAttenuation;
uniform vec4 spotlightParams;
uniform vec4 lightDirection;
uniform float lightPower;
uniform float shininess;
uniform mat4 worldViewProj;
uniform vec3 cameraPosition;
uniform float nearClipDistance;

uniform float shadowBias;
uniform int castShadows;
uniform int lightType; //0 - Directional, 1 - Spot, 2 - Point
uniform int lightMode; //0 - Realtime, 1 - Baked, 2 - Mixed
uniform int lightEnabled; //0 - disabled, 1 - enabled
uniform vec4 texMatrixScaleBias1;
uniform vec4 texMatrixScaleBias2;
uniform vec4 texMatrixScaleBias3;
uniform float shadowAASamples;
uniform int shadowCascadesBlending;
uniform int shadowCascadesCount;
uniform int useLightMap;
uniform sampler2D lightMap;
uniform mat4 shadowViewProj;

varying vec2 vUV0;
varying vec2 vUV1;
varying vec4 fragPos;
varying vec3 vNormal;
varying vec3 vTangent;
varying vec4 oUv;
varying mat4 modelMatrix;

float inverseShadowmapSize = 0.0007765625;
float fixedDepthBias = shadowBias;
float gradientClamp = 0.0098;
float gradientScaleBias = 0.0;

vec4 shadowMapSize = vec4(0.001, 0.001, 0.001, 0.001);
vec4 invShadowMapSize = vec4(0.0009765625, 0.0009765625, 0.0009765625, 0.0009765625);
vec4 fixedDepthBiasDir = vec4(0.0002, 0.0005, 0.00075, 0.001) + vec4(shadowBias);
vec4 gradientScaleBiasDir = vec4(0.0, 0.0, 0.0, 0.0);

vec3 expand(vec3 v)
{
	return (v - 0.5) * 2.0;
}

#ifdef RECEIVE_SHADOWS
float getShadowFactor(sampler2D shadowMapUnit, vec4 lightSpacePos, float shadowmapSize, float inverseShadowmapSize, float fixedDepthBias, float gradientScaleBias, float shadowLightDotLN)
{
	// point on shadowmap
	float depthAdjust = fixedDepthBias + (1.0 - abs(shadowLightDotLN)) * gradientScaleBias;
	lightSpacePos.z -= depthAdjust; // lightSpacePos.z contains lightspace position of current object

	float shadow  = 0.0;
	float samples = shadowAASamples;
	float offset  = 0.1;
	
	for(float x = -offset; x < offset; x += offset / (samples * 0.5))
	{
		for(float y = -offset; y < offset; y += offset / (samples * 0.5))
		{
			vec4 shadowData = texture2D(shadowMapUnit, lightSpacePos.xy + vec2(x, y) * 0.0075);
			float sampledDistance = shadowData.r;

			shadow += (shadowData.x + fixedDepthBias > lightSpacePos.z) ? 1.0 : 0.0;
		}
	}
	shadow /= (samples * samples);

	// Hack to prevent these getting optimized out, thereby preventing OGRE errors
	shadow += 0.001 * (0.001 * shadowmapSize + inverseShadowmapSize);

	// Get the average
	return shadow;
}

float getCsmShadowFactor(sampler2D shadowTexture0, sampler2D shadowTexture1, sampler2D shadowTexture2, sampler2D shadowTexture3, vec4 lightSpacePos0, float shadowLightDotLN)
{	
	float factor = 1.0;

	if (lightSpacePos0.x > 0.01 && lightSpacePos0.y > 0.01 && lightSpacePos0.x < 0.99 && lightSpacePos0.y < 0.99)
	{
		factor = getShadowFactor(shadowTexture0, lightSpacePos0, shadowMapSize.x, invShadowMapSize.x, fixedDepthBiasDir.x, gradientScaleBiasDir.x, shadowLightDotLN);

		if (shadowCascadesBlending == 1)
		{
			vec4 lightSpacePos1;
			lightSpacePos1.xyz = lightSpacePos0.xyz + texMatrixScaleBias1.xyz;
			lightSpacePos1.xy *= texMatrixScaleBias1.w;

			float blend = getShadowFactor(shadowTexture1, lightSpacePos1, shadowMapSize.y, invShadowMapSize.y, fixedDepthBiasDir.y, gradientScaleBiasDir.y, shadowLightDotLN);

			float weight = clamp((max(abs(lightSpacePos0.x-0.5), abs(lightSpacePos0.y-0.5)) -0.375) * 8.0, 0.0, 1.0);
			factor = mix(factor, blend, weight);
		}
	}
	else
	{
		vec4 lightSpacePos1;
		lightSpacePos1.xyz = lightSpacePos0.xyz + texMatrixScaleBias1.xyz;
		lightSpacePos1.xy *= texMatrixScaleBias1.w;

		if (lightSpacePos1.x > 0.01 && lightSpacePos1.y > 0.01 && lightSpacePos1.x < 0.99 && lightSpacePos1.y < 0.99)
		{
			factor = getShadowFactor(shadowTexture1, lightSpacePos1, shadowMapSize.y, invShadowMapSize.y, fixedDepthBiasDir.y, gradientScaleBiasDir.y, shadowLightDotLN);

			if (shadowCascadesBlending == 1)
			{
				if (shadowCascadesCount > 2)
				{
					vec4 lightSpacePos2;
					lightSpacePos2.xyz = lightSpacePos0.xyz + texMatrixScaleBias2.xyz;
					lightSpacePos2.xy *= texMatrixScaleBias2.w;

					float blend = getShadowFactor(shadowTexture2, lightSpacePos2, shadowMapSize.z, invShadowMapSize.z, fixedDepthBiasDir.z, gradientScaleBiasDir.z, shadowLightDotLN);
					float weight = clamp((max( abs(lightSpacePos1.x-0.5), abs(lightSpacePos1.y-0.5) ) -0.4375) * 16.0, 0.0, 1.0);
					factor = mix(factor, blend, weight);
				}
			}
		}
		else
		{
			vec4 lightSpacePos2;
			lightSpacePos2.xyz = lightSpacePos0.xyz + texMatrixScaleBias2.xyz;
			lightSpacePos2.xy *= texMatrixScaleBias2.w;

			if (lightSpacePos2.x > 0.01 && lightSpacePos2.y > 0.01 && lightSpacePos2.x < 0.99 && lightSpacePos2.y < 0.99)
			{
				factor = getShadowFactor(shadowTexture2, lightSpacePos2, shadowMapSize.z, invShadowMapSize.z, fixedDepthBiasDir.z, gradientScaleBiasDir.z, shadowLightDotLN);

				if (shadowCascadesBlending == 1)
				{
					if (shadowCascadesCount > 3)
					{
						vec4 lightSpacePos3;
						lightSpacePos3.xyz = lightSpacePos0.xyz + texMatrixScaleBias3.xyz;
						lightSpacePos3.xy *= texMatrixScaleBias3.w;

						float blend = getShadowFactor(shadowTexture3, lightSpacePos3, shadowMapSize.w, invShadowMapSize.w, fixedDepthBiasDir.w, gradientScaleBiasDir.w, shadowLightDotLN);
				
						float weight = clamp((max( abs(lightSpacePos2.x-0.5), abs(lightSpacePos2.y-0.5)) -0.375) * 8.0, 0.0, 1.0);
						factor = mix(factor, blend, weight);
					}
				}
			}
			else
			{
				vec4 lightSpacePos3;
				lightSpacePos3.xyz = lightSpacePos0.xyz + texMatrixScaleBias3.xyz;
				lightSpacePos3.xy *= texMatrixScaleBias3.w;

				factor = getShadowFactor(shadowTexture3, lightSpacePos3, shadowMapSize.w, invShadowMapSize.w, fixedDepthBiasDir.w, gradientScaleBiasDir.w, shadowLightDotLN);

				// Fade out to edges
				float weight = clamp((max( abs(lightSpacePos3.x-0.5), abs(lightSpacePos3.y-0.5)) -0.375) * 8.0, 0.0, 1.0);
				factor = mix(factor, 1.0, weight);
			}
		}
	}

	return factor;
}

float getShadow(vec3 worldPos, vec3 normal)
{
	float shadow = 1.0;
	
	if (lightType == 0)
	{
		vec3 lightDir = -lightDirection.xyz;
		float lDotN = dot(normal, lightDir);
		vec4 shadowProjPos = shadowViewProj * vec4(worldPos,1);
		shadowProjPos /= shadowProjPos.w;
		shadow = getCsmShadowFactor(shadowMap0, shadowMap1, shadowMap2, shadowMap3, shadowProjPos, lDotN);
	}
	if (lightType == 1) // Spot
	{
		float pixeloffset = inverseShadowmapSize;
		
		vec4 shadowUV = oUv;
		shadowUV = shadowUV / shadowUV.w;

		float samples = shadowAASamples;
		float offset  = 0.1;
		for(float x = -offset; x < offset; x += offset / (samples * 0.5))
		{
			for(float y = -offset; y < offset; y += offset / (samples * 0.5))
			{
				vec4 shadowData = texture2D(shadowMap0, shadowUV.xy + vec2(x, y) * 0.02);
				float sampledDistance = shadowData.r;
				
				shadow += (shadowData.x + fixedDepthBias > shadowUV.z) ? 1.0 : 0.0;
			}
		}
		shadow /= (samples * samples);
	}
	if (lightType == 2) // Point
	{
		vec3 lightDir = worldPos - lightPosition.xyz;

		float samples = shadowAASamples;
		float offset  = 0.1;

		for(float x = -offset; x < offset; x += offset / (samples * 0.5))
		{
			for(float y = -offset; y < offset; y += offset / (samples * 0.5))
			{
				for(float z = -offset; z < offset; z += offset / (samples * 0.5))
				{
					vec4 shadowData = textureCube(shadowMapPoint, vec3(-lightDir.x, lightDir.y, lightDir.z) + vec3(x, y, z));
					float sampledDistance = shadowData.r;
					
					vec3 fromLightToFragment = lightPosition.xyz - worldPos;
					float distanceToLight = length(fromLightToFragment);
					float currentDistanceToLight = (distanceToLight - nearClipDistance) / (1000.0 - nearClipDistance);
					currentDistanceToLight = clamp(currentDistanceToLight, 0.0, 1.0);

					if(sampledDistance + fixedDepthBias > currentDistanceToLight)
						shadow += 1.0;
				}
			}
		}
		shadow /= (samples * samples * samples);
	}

	return shadow;
}
#endif

void main()
{
#ifdef HAS_DIFFUSE
	vec4 diffuse = texture2D(diffuseMap, vUV0);
#else
	vec4 diffuse = vec4(1.0, 1.0, 1.0, 1.0);
#endif
	vec4 lightmap = texture2D(lightMap, vUV1.xy);
	
#ifdef CUTOUT
	if (diffuse.a < cutoutValue)
		discard;
	
#ifdef HAS_DIFFUSE
	//if (diffuse.r + diffuse.g + diffuse.b > cutoutValue * 3.0)
	//	discard;
#endif
#endif

	if (useLightMap == 1)
		diffuse = lightmap * diffuse * diffuseColor;
	else
		diffuse = diffuse * diffuseColor;

	if (lightEnabled == 1)
	{
		vec3 lightVec;
		vec3 lightDir;
		float ndot = 0.0;
		float specFactor = 0.0;

		lightDir;
		vec3 oTSLightDir;
		vec3 oTSHalfAngle;
		mat3 rotation;
		vec3 eyeDir;
		vec3 bumpVec;

		// Calculate tangent space light vector
		// Get object space light direction
		lightDir = normalize((lightPosition - ((modelMatrix * fragPos) * lightPosition.w))).xyz;

		//Convert normals from object to world space
		vec3 normal = normalize((modelMatrix * vec4(vNormal, 0.0)).xyz);
		vec3 tangent = normalize((modelMatrix * vec4(vTangent, 0.0)).xyz);
		
		// Calculate the binormal (NB we assume both normal and tangent are already normalised)
		vec3 binormal = cross(normal, tangent);
		
		// Form a rotation matrix out of the vectors
		rotation = mat3(vec3(tangent[0], binormal[0], normal[0]),
						vec3(tangent[1], binormal[1], normal[1]),
						vec3(tangent[2], binormal[2], normal[2]));
		
		// Transform the light vector according to this matrix
		oTSLightDir = rotation * lightDir;

		// Calculate half-angle in tangent space
		eyeDir = normalize((vec4(cameraPosition, 1.0) - (modelMatrix * fragPos))).xyz;
		
#ifdef HAS_NORMAL
		// Get bump map vector, again expand from range-compressed
		bumpVec = expand(texture2D(normalMap, vUV0).xyz);
#endif
		// Retrieve normalised light vector, expand from range-compressed
		lightVec = normalize(oTSLightDir).xyz;
		
#ifdef HAS_NORMAL
		ndot = dot(bumpVec, lightVec);//clamp(dot(bumpVec, lightVec), 0.0, 1.0);
#else
		ndot = clamp(dot(lightVec, lightVec), 0.0, 1.0);
#endif

#ifdef HAS_NORMAL
		vec3 halfAngle = normalize(eyeDir + lightDir);
		oTSHalfAngle = rotation * halfAngle;
		
		// retrieve half angle and normalise
		halfAngle = normalize(oTSHalfAngle);
		
		// Pre-raise the specular exponent to the eight power
		specFactor = pow(clamp(dot(bumpVec, halfAngle), 0.0, 1.0), shininess);
#endif

		float final = 1.0;
		
#ifdef RECEIVE_SHADOWS
		if (castShadows == 1)
		{
			final = getShadow((modelMatrix * fragPos).xyz, normal);
		}
#endif

		float att = 1.0;
		if (lightPosition.w > 0.0)
		{
			att = smoothstep(lightAttenuation.x, 0.0, length(lightPosition - (modelMatrix * fragPos))) * lightPower;
		}
		else
		{
			att = lightPower;
		}
		
#ifdef HAS_NORMAL
		gl_FragColor = vec4((((diffuse * lightDiffuse * ndot) + (lightSpecular * specFactor)) * final).xyz, diffuse.a);
#else
		gl_FragColor = vec4((((diffuse * lightDiffuse * ndot)) * final).xyz, diffuse.a);
#endif
		
		gl_FragColor *= att;
		
		if (lightType == 1)
		{
			vec3 sdir = rotation * lightDirection.xyz;
			float spot = clamp((dot(oTSLightDir, normalize(-sdir)) - spotlightParams.y) / (spotlightParams.x - spotlightParams.y), 0.0, 1.0);
			
			gl_FragColor *= spot;
		}
	}
	else
	{
		gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
	}
}
