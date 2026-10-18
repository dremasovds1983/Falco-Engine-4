name "Forward/Nature/Standard Terrain"
render_mode forward

params
{
	float "Specular" specIntensity 0.1 : 0.0 1.0
	define bool "Has albedo map" HAS_ALBEDO true
	define bool "Has normal map" HAS_NORMAL_MAP true
	define bool "Realtime global illumination" ENABLE_GI true
}

pass //Ambient pass
{
	tags
	{
		iteration_mode default
	}
	
	varying
	{
		vec3 a_position  : POSITION;
		vec3 a_normal    : NORMAL;
		vec3 a_tangent   : TANGENT;
		vec3 a_bitangent : BITANGENT;
		vec2 a_texcoord0 : TEXCOORD0;

		vec3 v_position    : POSITION1 = vec3(0.0, 0.0, 0.0);
		vec3 v_ls_position : POSITION2 = vec3(0.0, 0.0, 0.0);
		vec3 v_normal      : NORMAL    = vec3(0.0, 0.0, 0.0);
		vec3 v_tangent     : TANGENT   = vec3(0.0, 0.0, 0.0);
		vec3 v_bitangent   : BITANGENT   = vec3(0.0, 0.0, 0.0);
		vec2 v_texcoord0   : TEXCOORD0 = vec2(0.0, 0.0);
		vec4 v_texcoord1   : TEXCOORD1 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_texcoord2   : TEXCOORD2 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_texcoord3   : TEXCOORD3 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_texcoord4   : TEXCOORD4 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_worldpos    : TEXCOORD6 = vec4(0.0, 0.0, 0.0, 0.0);
	}
	
	vertex
	{
		$input a_position, a_normal, a_tangent, a_bitangent, a_texcoord0
		$output v_position, v_normal, v_tangent, v_bitangent, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_ls_position, v_worldpos

		#include "common.sh"
        #include "camera_space.sh"
        #include "color_space.sh"

		uniform mat3 u_normalMatrix;
		uniform mat4 u_shadowMatrix[4];
		
		uniform vec4 u_lightPosition;
		uniform vec4 u_lightType;
		uniform vec4 u_lightCastShadows;

		void main()
		{
			mat4 model = u_model[0];
			
			gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
			v_normal = mul(u_normalMatrix, a_normal);
			v_tangent = mul(u_normalMatrix, a_tangent);
			v_bitangent = mul(u_normalMatrix, a_bitangent);

			v_position = mul(model, vec4(a_position, 1.0)).xyz;
			v_texcoord0 = a_texcoord0;
			v_worldpos = gl_Position;
			
			vec4 wpos = vec4(0.0, 0.0, 0.0, 1.0);
			
			if (u_lightCastShadows.x == 1)
			{
				if (u_lightType.x == 2) // Directional light
				{
					wpos = mul(model, vec4(a_position, 1.0));
					
					v_texcoord1 = mul(u_shadowMatrix[0], wpos);
					v_texcoord2 = mul(u_shadowMatrix[1], wpos);
					v_texcoord3 = mul(u_shadowMatrix[2], wpos);
					v_texcoord4 = mul(u_shadowMatrix[3], wpos);
				}
				else if (u_lightType.x == 1) // Spot light
				{
					wpos = mul(model, vec4(a_position, 1.0));
					v_texcoord1 = mul(u_shadowMatrix[0], wpos);
				}
				else // Point light
				{
					wpos = vec4(-u_lightPosition.xyz, 0.0) + mul(model, vec4(a_position, 1.0));
					v_ls_position = wpos.xyz;
					v_texcoord1 = mul(u_shadowMatrix[0], wpos);
					v_texcoord2 = mul(u_shadowMatrix[1], wpos);
					v_texcoord3 = mul(u_shadowMatrix[2], wpos);
					v_texcoord4 = mul(u_shadowMatrix[3], wpos);
				}
			}
		}
	}
	
	fragment
	{
		$input v_position, v_normal, v_tangent, v_bitangent, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_ls_position, v_worldpos
		
		#include "common.sh"
        #include "camera_space.sh"
        #include "color_space.sh"
		
		uniform vec4 u_ambientColor;
        uniform vec4 specIntensity;
		
		uniform vec4 u_textureCount;
		uniform vec4 u_textureSizes[8];
		
		uniform vec4 u_giParams; // x - enabled, y - power
		uniform vec4 u_camPos;
        uniform mat4 u_camProj;
        uniform mat4 u_camInvView;
		
#if HAS_ALBEDO == 1
		SAMPLER2D(u_texture0, 0);
		SAMPLER2D(u_texture1, 1);
		SAMPLER2D(u_texture2, 2);
		SAMPLER2D(u_texture3, 3);
		SAMPLER2D(u_texture4, 4);
#endif

#if HAS_NORMAL_MAP == 1
		SAMPLER2D(u_texture0Normal, 5);
		SAMPLER2D(u_texture1Normal, 6);
		SAMPLER2D(u_texture2Normal, 7);
		SAMPLER2D(u_texture3Normal, 8);
		SAMPLER2D(u_texture4Normal, 9);
#endif
#if ENABLE_GI == 1
        SAMPLERCUBE(u_envMap, 15);
        #define FALCO_IBL_TERRAIN 1
        #include "ibl_material.sh"
#endif

		SAMPLER2D(u_texture0Splat, 10);
		SAMPLER2D(u_texture1Splat, 11);
		
		void main()
		{
			vec4 albedo = vec4(1.0);
			// Use the rendered world-space surface itself as the source of the
			// Terrain normal. This keeps Forward and Deferred Terrain on the same
			// geometric contract and avoids interpolation/winding differences in
			// the legacy CPU normal stream on the modern D3D path.
			vec3 normal = cross(dFdx(v_position), dFdy(v_position));
			if (dot(normal, normal) <= 0.00000001)
				normal = vec3(0.0, 1.0, 0.0);
			else
				normal = normalize(normal);
			if (normal.y < 0.0)
				normal = -normal;
			vec3 terrainNormal = normal;

#if HAS_NORMAL_MAP == 1
			vec3 positionDx = dFdx(v_position);
			vec3 positionDy = dFdy(v_position);
			vec2 uvDx = dFdx(v_texcoord0);
			vec2 uvDy = dFdy(v_texcoord0);
			float uvDeterminant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
			vec3 rawTangent = abs(uvDeterminant) > 0.00000001
				? (positionDx * uvDy.y - positionDy * uvDx.y) / uvDeterminant
				: v_tangent;
			vec3 rawBitangent = abs(uvDeterminant) > 0.00000001
				? (positionDy * uvDx.x - positionDx * uvDy.x) / uvDeterminant
				: v_bitangent;
			vec3 terrainTangent = normalize(rawTangent - terrainNormal * dot(rawTangent, terrainNormal));
			float terrainHandedness = dot(cross(terrainNormal, terrainTangent), rawBitangent) < 0.0 ? -1.0 : 1.0;
			vec3 terrainBitangent = normalize(cross(terrainNormal, terrainTangent)) * terrainHandedness;
#endif

			if (u_textureCount.x > 0)
			{
				vec2 uvs = v_texcoord0 * u_textureSizes[0].xy;
				
#if HAS_ALBEDO == 1
				albedo = falcoSRGBToLinear(texture2D(u_texture0, uvs));
#endif
#if HAS_NORMAL_MAP == 1
				if (u_textureSizes[0].z > 0.5)
				{
					vec3 tangentNormal = normalize(texture2D(u_texture0Normal, uvs).rgb * 2.0 - 1.0);
					normal = normalize(terrainTangent * tangentNormal.x + terrainBitangent * tangentNormal.y + terrainNormal * tangentNormal.z);
				}
#endif
			}
			
			if (u_textureCount.x > 0)
			{
				for (int i = 1; i < u_textureCount.x; ++i)
				{
					vec2 uvs = v_texcoord0 * u_textureSizes[i].xy;
					
#if HAS_ALBEDO == 1
					float color_mask = 1.0;
					vec3 color = vec3(1.0);
					
					//Mix textures together
					if (i == 1)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).g;
						color = falcoSRGBToLinear(texture2D(u_texture1, uvs).rgb);
					}
					
					if (i == 2)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).b;
						color = falcoSRGBToLinear(texture2D(u_texture2, uvs).rgb);
					}
					
					if (i == 3)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).a;
						color = falcoSRGBToLinear(texture2D(u_texture3, uvs).rgb);
					}
					
					if (i == 4)
					{
						color_mask = texture2D(u_texture1Splat, v_texcoord0).r;
						color = falcoSRGBToLinear(texture2D(u_texture4, uvs).rgb);
					}
					
					albedo = ((albedo * vec4(vec3(1.0 - color_mask), 1.0)) + vec4(color * color_mask, 1.0));
#endif
#if HAS_NORMAL_MAP == 1
					if (u_textureSizes[i].z > 0.5)
					{
					float normal_mask = 1.0;
					vec3 normal_color = vec3(1.0);
					
					if (i == 1)
					{
						normal_mask = texture2D(u_texture0Splat, v_texcoord0).g;
						normal_color = texture2D(u_texture1Normal, uvs).rgb;
					}
					
					if (i == 2)
					{
						normal_mask = texture2D(u_texture0Splat, v_texcoord0).b;
						normal_color = texture2D(u_texture2Normal, uvs).rgb;
					}
					
					if (i == 3)
					{
						normal_mask = texture2D(u_texture0Splat, v_texcoord0).a;
						normal_color = texture2D(u_texture3Normal, uvs).rgb;
					}
					
					if (i == 4)
					{
						normal_mask = texture2D(u_texture1Splat, v_texcoord0).r;
						normal_color = texture2D(u_texture4Normal, uvs).rgb;
					}
					
					normal_color = normalize(normal_color * 2.0 - 1.0);
					normal_color = normalize(terrainTangent * normal_color.x + terrainBitangent * normal_color.y + terrainNormal * normal_color.z);
					
					normal = normalize((normal * vec3(1.0 - normal_mask)) + (normal_color * normal_mask));
					}
#endif
				}
			}
			
            vec3 ambient = u_ambientColor.rgb * albedo.rgb;
#if ENABLE_GI == 1
            if (u_giParams.x > 0.5)
            {
                if (u_giParams.z > 0.5)
                {
                    vec3 N = normalize(normal);
                    vec3 V = falcoSurfaceToCamera(v_position, u_camPos.xyz, u_camProj, u_camInvView);
                    if (dot(N, V) < 0.0) N = -N;
                    // Match the direct pass: dielectric terrain, roughness one.
                    ambient = iblDiffuse(N) * albedo.rgb * 0.96
                        + iblSpecular(N, V, vec3_splat(0.04), 1.0) * max(specIntensity.x, 0.0);
                }
                ambient *= max(u_giParams.y, 0.0);
            }
#endif
			gl_FragColor = vec4(ambient, 1.0);
		}
	}
}

pass //Lighting pass. Use varyings and vertex program from previous pass to avoid z-fighting
{
	tags
	{
		blend_mode add
		iteration_mode per_light
		depth_write off
	}
	
	fragment
	{
		$input v_position, v_normal, v_tangent, v_bitangent, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_ls_position, v_worldpos

		#include "common.sh"
        #include "camera_space.sh"
        #include "color_space.sh"
		#include "pbr.sh"
		#include "clouds/shadow.sh"
		#include "shadows.sh"

		uniform vec4 specIntensity;
		uniform vec4 u_camPos;
        uniform mat4 u_camProj;
        uniform mat4 u_camInvView;
		uniform vec4 u_lightPosition;
		uniform vec4 u_lightColor;
		uniform vec4 u_lightIntensity;
		uniform vec4 u_lightRadius;
		uniform vec4 u_lightType; // 0 - Point, 1 - Spot, 2 - Directional
		uniform vec4 u_lightRenderMode; // 0 - Realtime, 1 - Mixed, 2 - Baked
		uniform vec4 u_lightDirection;
		uniform vec4 u_lightShadowBias;
		uniform vec4 u_lightCastShadows;
		uniform vec4 u_shadowMapTexelSize;
		uniform vec4 u_shadowSamplingParams;
		uniform vec4 u_shadowCascadeSplits;
		uniform vec4 u_camDir;
		
		uniform vec4 u_textureCount;
		uniform vec4 u_textureSizes[5];
		
		uniform vec4 u_giParams; // x - enabled, y - power
		
#if HAS_ALBEDO == 1
		SAMPLER2D(u_texture0, 0);
		SAMPLER2D(u_texture1, 1);
		SAMPLER2D(u_texture2, 2);
		SAMPLER2D(u_texture3, 3);
		SAMPLER2D(u_texture4, 4);
#endif

#if HAS_NORMAL_MAP == 1
		SAMPLER2D(u_texture0Normal, 5);
		SAMPLER2D(u_texture1Normal, 6);
		SAMPLER2D(u_texture2Normal, 7);
		SAMPLER2D(u_texture3Normal, 8);
		SAMPLER2D(u_texture4Normal, 9);
#endif
		SAMPLER2D(u_texture0Splat, 10);
		SAMPLER2D(u_texture1Splat, 11);
		
		SAMPLER2DSHADOW(u_shadowMap0, 12);
		SAMPLER2DSHADOW(u_shadowMap1, 13);
		SAMPLER2DSHADOW(u_shadowMap2, 14);
		SAMPLER2DSHADOW(u_shadowMap3, 15);

		const float PI = 3.14159265359;

		void main()
		{
			vec4 albedo = vec4(1.0);
			// Use the rendered world-space surface itself as the source of the
			// Terrain normal. This keeps Forward and Deferred Terrain on the same
			// geometric contract and avoids interpolation/winding differences in
			// the legacy CPU normal stream on the modern D3D path.
			vec3 normal = cross(dFdx(v_position), dFdy(v_position));
			if (dot(normal, normal) <= 0.00000001)
				normal = vec3(0.0, 1.0, 0.0);
			else
				normal = normalize(normal);
			if (normal.y < 0.0)
				normal = -normal;
			vec3 terrainNormal = normal;

#if HAS_NORMAL_MAP == 1
			vec3 positionDx = dFdx(v_position);
			vec3 positionDy = dFdy(v_position);
			vec2 uvDx = dFdx(v_texcoord0);
			vec2 uvDy = dFdy(v_texcoord0);
			float uvDeterminant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
			vec3 rawTangent = abs(uvDeterminant) > 0.00000001
				? (positionDx * uvDy.y - positionDy * uvDx.y) / uvDeterminant
				: v_tangent;
			vec3 rawBitangent = abs(uvDeterminant) > 0.00000001
				? (positionDy * uvDx.x - positionDx * uvDy.x) / uvDeterminant
				: v_bitangent;
			vec3 terrainTangent = normalize(rawTangent - terrainNormal * dot(rawTangent, terrainNormal));
			float terrainHandedness = dot(cross(terrainNormal, terrainTangent), rawBitangent) < 0.0 ? -1.0 : 1.0;
			vec3 terrainBitangent = normalize(cross(terrainNormal, terrainTangent)) * terrainHandedness;
#endif

			if (u_textureCount.x > 0)
			{
				vec2 uvs = v_texcoord0 * u_textureSizes[0].xy;
				
#if HAS_ALBEDO == 1
				albedo = falcoSRGBToLinear(texture2D(u_texture0, uvs));
#endif
#if HAS_NORMAL_MAP == 1
				if (u_textureSizes[0].z > 0.5)
				{
					vec3 tangentNormal = normalize(texture2D(u_texture0Normal, uvs).rgb * 2.0 - 1.0);
					normal = normalize(terrainTangent * tangentNormal.x + terrainBitangent * tangentNormal.y + terrainNormal * tangentNormal.z);
				}
#endif
			}
			
			if (u_textureCount.x > 0)
			{
				for (int i = 1; i < u_textureCount.x; ++i)
				{
					vec2 uvs = v_texcoord0 * u_textureSizes[i].xy;
					
#if HAS_ALBEDO == 1
					float color_mask = 1.0;
					vec3 color = vec3(1.0);
					
					//Mix textures together
					if (i == 1)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).g;
						color = falcoSRGBToLinear(texture2D(u_texture1, uvs).rgb);
					}
					
					if (i == 2)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).b;
						color = falcoSRGBToLinear(texture2D(u_texture2, uvs).rgb);
					}
					
					if (i == 3)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).a;
						color = falcoSRGBToLinear(texture2D(u_texture3, uvs).rgb);
					}
					
					if (i == 4)
					{
						color_mask = texture2D(u_texture1Splat, v_texcoord0).r;
						color = falcoSRGBToLinear(texture2D(u_texture4, uvs).rgb);
					}
					
					albedo = ((albedo * vec4(vec3(1.0 - color_mask), 1.0)) + vec4(color * color_mask, 1.0));
#endif
#if HAS_NORMAL_MAP == 1
					if (u_textureSizes[i].z > 0.5)
					{
					float normal_mask = 1.0;
					vec3 normal_color = vec3(1.0);
					
					if (i == 1)
					{
						normal_mask = texture2D(u_texture0Splat, v_texcoord0).g;
						normal_color = texture2D(u_texture1Normal, uvs).rgb;
					}
					
					if (i == 2)
					{
						normal_mask = texture2D(u_texture0Splat, v_texcoord0).b;
						normal_color = texture2D(u_texture2Normal, uvs).rgb;
					}
					
					if (i == 3)
					{
						normal_mask = texture2D(u_texture0Splat, v_texcoord0).a;
						normal_color = texture2D(u_texture3Normal, uvs).rgb;
					}
					
					if (i == 4)
					{
						normal_mask = texture2D(u_texture1Splat, v_texcoord0).r;
						normal_color = texture2D(u_texture4Normal, uvs).rgb;
					}
					
					normal_color = normalize(normal_color * 2.0 - 1.0);
					normal_color = normalize(terrainTangent * normal_color.x + terrainBitangent * normal_color.y + terrainNormal * normal_color.z);
					
					normal = normalize((normal * vec3(1.0 - normal_mask)) + (normal_color * normal_mask));
					}
#endif
				}
			}
			
			float metallicVal = 0.0;
			float roughness = clamp(1.0, 0.04, 1.0);
			float ao = 1.0;

			vec3 N = normalize(normal);
			vec3 V = falcoSurfaceToCamera(v_position, u_camPos.xyz, u_camProj, u_camInvView);

			vec3 F0 = vec3(0.04);
			F0 = mix(F0, albedo.rgb, vec3(metallicVal));
			
			// Р Р†РЎвЂ№РЎР‚Р В°Р В¶Р ВµР Р…Р С‘Р Вµ Р С•РЎвЂљРЎР‚Р В°Р В¶Р В°РЎР‹РЎвЂ°Р ВµР в„– РЎРѓР С—Р С•РЎРѓР С•Р В±Р Р…Р С•РЎРѓРЎвЂљР С‘
			vec3 Lo = vec3(0.0);

			float radius = u_lightRadius.x;
			float innerRadius = u_lightRadius.y;
			float outerRadius = u_lightRadius.z;
			float intensity = u_lightIntensity.x;

			// РЎР‚Р В°РЎРѓРЎвЂЎР ВµРЎвЂљ РЎРЊР Р…Р ВµРЎР‚Р С–Р ВµРЎвЂљР С‘РЎвЂЎР ВµРЎРѓР С”Р С•Р в„– РЎРЏРЎР‚Р С”Р С•РЎРѓРЎвЂљР С‘ Р Т‘Р В»РЎРЏ Р С”Р В°Р В¶Р Т‘Р С•Р С–Р С• Р С‘РЎРѓРЎвЂљР С•РЎвЂЎР Р…Р С‘Р С”Р В° РЎРѓР Р†Р ВµРЎвЂљР В°
			vec3 L;
			if (u_lightType.x != 2)
				L = normalize(u_lightPosition.xyz - v_position);
			else
				L = normalize(-u_lightDirection.xyz);
			
			vec3 halfVector = V + L;
			float halfLengthSq = dot(halfVector, halfVector);
			vec3 H = halfLengthSq > 0.00000001
				? halfVector / sqrt(halfLengthSq)
				: N;

			float distance = length(u_lightPosition.xyz - v_position);

			//Compute attenuation
			float attenuation = 0.0;
			if (u_lightType.x == 0) // Point light
			{
				attenuation = smoothstep(radius, 0.0, distance) * intensity;
			}
			else if (u_lightType.x == 1) // Spot light
			{
				float spotAttenuation = 1.0;
				float spotDot = dot(L, -u_lightDirection.xyz);

				float spot = clamp((spotDot - outerRadius) / max(innerRadius - outerRadius, 0.0001), 0.0, 1.0);
				attenuation = smoothstep(radius, 0.0, distance) * intensity * spot;
			}
			else // Directional light
			{
				attenuation = 1.0 * intensity;
			}

			vec3 radiance = u_lightColor.xyz * attenuation;
            if (u_lightType.x == 2) radiance *= cloudSunVisibility(v_position, -u_lightDirection.xyz);

			// Cook-Torrance BRDF
			float NDF = DistributionGGX(N, H, roughness);
			float G   = GeometrySmith(N, V, L, roughness);
			vec3 F    = fresnelSchlick(clamp(dot(H, V), 0.0, 1.0), F0);

			vec3 kS = F;
			vec3 kD = vec3(1.0) - kS;
			kD *= 1.0 - metallicVal;

			float NdotV = max(dot(N, V), 0.0);
			float NdotL = max(dot(N, L), 0.0);
			vec3 numerator = NDF * G * F;
			float denominator = max(4.0 * NdotV * NdotL, 0.001);
			vec3 specular = hdrSpecularBRDF(numerator, denominator, specIntensity.x);

			vec3 directDiffuse = lambertBRDF(albedo.rgb, kD);
			directDiffuse *= radiance;
			directDiffuse *= NdotL;

			vec3 directSpecular = specular * radiance;
			directSpecular *= NdotL;
			Lo += directDiffuse + directSpecular;
			
			float visibility = 1.0;

			if (u_lightCastShadows.x == 1)
			{
				if (u_lightRenderMode.x < 2)
				{
					visibility = getShadow(int(u_lightType.x),
											v_texcoord1,
											v_texcoord2,
											v_texcoord3,
											v_texcoord4,
											u_lightShadowBias,
											u_shadowSamplingParams,
											u_shadowCascadeSplits,
											dot(v_position - u_camPos.xyz, normalize(u_camDir.xyz)),
											v_ls_position,
											u_shadowMapTexelSize.x,
											u_shadowMap0,
											u_shadowMap1,
											u_shadowMap2,
											u_shadowMap3);
				}
			}

			// Material AO belongs to the ambient/indirect pass, as in Deferred.
			vec3 color = Lo * visibility;

			gl_FragColor = vec4(color, 0.0);
		}
	}
}
