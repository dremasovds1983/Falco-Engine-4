name "Forward/Terrain/Standard"
render_mode forward

params
{
	float "Specular" specIntensity 0.5 : 0.0 1.0
	define bool "Has albedo map" HAS_ALBEDO true
	define bool "Has normal map" HAS_NORMAL_MAP true
}

pass //Ambient pass
{
	tags
	{
		//blend_mode alpha
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
		vec4 v_worldpos    : TEXCOORD6 = vec2(0.0, 0.0, 0.0, 0.0);
	}
	
	vertex
	{
		$input a_position, a_normal, a_tangent, a_bitangent, a_texcoord0
		$output v_position, v_normal, v_tangent, v_bitangent, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_ls_position, v_worldpos

		#include "common.sh"

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
		
		uniform vec4 u_ambientColor;
		
		uniform vec4 u_textureCount;
		uniform vec4 u_textureSizes[8];
		
#if HAS_ALBEDO == 1
		SAMPLER2D(u_texture0, 0);
		SAMPLER2D(u_texture1, 1);
		SAMPLER2D(u_texture2, 2);
		SAMPLER2D(u_texture3, 3);
		SAMPLER2D(u_texture4, 4);
#endif
		SAMPLER2D(u_texture0Splat, 10);
		SAMPLER2D(u_texture1Splat, 11);
		
		void main()
		{
			vec4 albedo = vec4(1.0, 1.0, 1.0, 1.0);
			
			if (u_textureCount.x > 0)
			{
				vec2 uvs = v_texcoord0 * u_textureSizes[0].xy;
				
#if HAS_ALBEDO == 1
				albedo = texture2D(u_texture0, uvs);
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
						color = texture2D(u_texture1, uvs).rgb;
					}
					
					if (i == 2)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).b;
						color = texture2D(u_texture2, uvs).rgb;
					}
					
					if (i == 3)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).a;
						color = texture2D(u_texture3, uvs).rgb;
					}
					
					if (i == 4)
					{
						color_mask = texture2D(u_texture1Splat, v_texcoord0).r;
						color = texture2D(u_texture4, uvs).rgb;
					}
					
					albedo = (albedo * vec4(vec3(1.0 - color_mask), 1.0)) + vec4(color * color_mask, 1.0);
#endif
				}
			}
			
			vec3 ambient = u_ambientColor.rgb * albedo.rgb;
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
		#include "pbr.sh"
		#include "shadows.sh"

		uniform vec4 specIntensity;

		uniform vec4 u_camPos;
		uniform vec4 u_lightPosition;
		uniform vec4 u_lightColor;
		uniform vec4 u_lightIntensity;
		uniform vec4 u_lightRadius;
		uniform vec4 u_lightType;
		uniform vec4 u_lightDirection;
		uniform vec4 u_lightShadowBias;
		uniform vec4 u_lightCastShadows;
		uniform vec4 u_shadowMapTexelSize;
		uniform vec4 u_shadowSamplingParams;
		
		uniform vec4 u_textureCount;
		uniform vec4 u_textureSizes[5];
		
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
			vec3 normal = normalize(v_normal);

			if (u_textureCount.x > 0)
			{
				vec2 uvs = v_texcoord0 * u_textureSizes[0].xy;
				
#if HAS_ALBEDO == 1
				albedo = texture2D(u_texture0, uvs);
#endif
#if HAS_NORMAL_MAP == 1
				normal = texture2D(u_texture0Normal, uvs).rgb;
				normal = normalize(normal * 2.0 - 1.0);
				normal = normalize(v_tangent * normal.x
					+ v_bitangent * normal.y
					+ v_normal * normal.z);
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
						color = texture2D(u_texture1, uvs).rgb;
					}
					
					if (i == 2)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).b;
						color = texture2D(u_texture2, uvs).rgb;
					}
					
					if (i == 3)
					{
						color_mask = texture2D(u_texture0Splat, v_texcoord0).a;
						color = texture2D(u_texture3, uvs).rgb;
					}
					
					if (i == 4)
					{
						color_mask = texture2D(u_texture1Splat, v_texcoord0).r;
						color = texture2D(u_texture4, uvs).rgb;
					}
					
					albedo = ((albedo * vec4(vec3(1.0 - color_mask), 1.0)) + vec4(color * color_mask, 1.0));
#endif
#if HAS_NORMAL_MAP == 1
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
					normal_color = normalize(v_tangent * normal_color.x
						+ v_bitangent * normal_color.y
						+ v_normal * normal_color.z);
					
					normal = (normal * vec3(1.0 - normal_mask)) + (normal_color * normal_mask);
#endif
				}
			}
			
			albedo = albedo * 2.2;
			float metallicVal = albedo.r * 0.2;
			float roughness = 0.5;
			float ao = 1.0;

			vec3 N = normalize(normal);
			vec3 V = normalize(u_camPos.xyz - v_position);

			vec3 F0 = vec3(0.04);
			F0 = mix(F0, albedo.rgb, vec3(metallicVal));
			
			// выражение отражающей способности
			vec3 Lo = vec3(0.0);

			float radius = u_lightRadius.x;
			float innerRadius = u_lightRadius.y;
			float outerRadius = u_lightRadius.z;
			float intensity = u_lightIntensity.x * 10.0 / 2.2;

			// расчет энергетической яркости для каждого источника света
			vec3 L;
			if (u_lightType.x != 2)
				L = normalize(u_lightPosition.xyz - v_position);
			else
				L = normalize(-u_lightDirection.xyz);

			vec3 H = normalize(V + L);

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

			// Cook-Torrance BRDF
			float NDF = DistributionGGX(N, H, roughness);
			float G   = GeometrySmith(N, V, L, roughness);
			vec3 F    = fresnelSchlick(max(dot(H, V), 0.0), F0);

			vec3 kS = F;
			vec3 kD = vec3(1.0) - kS;
			kD *= 1.0 - metallicVal;

			vec3 numerator    = NDF * G * F;
			float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
			vec3 specular = numerator / max(denominator, 0.001) * specIntensity.x;

			// прибавляем результат к исходящей энергетической яркости Lo
			float NdotL = max(dot(N, L), 0.0);
			Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL;
			
			float visibility = 1.0;
			vec3 colorCoverage = vec3(1, 1, 1);

			if (u_lightCastShadows.x == 1)
			{
				if (u_lightType.x == 2) // Directional light shadow
				{
					vec2 texelSize = vec2_splat(u_shadowMapTexelSize.x);
					vec2 texcoord1 = v_texcoord1.xy/v_texcoord1.w;
					vec2 texcoord2 = v_texcoord2.xy/v_texcoord2.w;
					vec2 texcoord3 = v_texcoord3.xy/v_texcoord3.w;
					vec2 texcoord4 = v_texcoord4.xy/v_texcoord4.w;
					
					bool selection0 = all(lessThan(texcoord1, vec2_splat(0.99))) && all(greaterThan(texcoord1, vec2_splat(0.01)));
					bool selection1 = all(lessThan(texcoord2, vec2_splat(0.99))) && all(greaterThan(texcoord2, vec2_splat(0.01)));
					bool selection2 = all(lessThan(texcoord3, vec2_splat(0.99))) && all(greaterThan(texcoord3, vec2_splat(0.01)));
					bool selection3 = all(lessThan(texcoord4, vec2_splat(0.99))) && all(greaterThan(texcoord4, vec2_splat(0.01)));

					if (selection0)
					{
						vec4 shadowcoord = v_texcoord1;

						float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.4;
						colorCoverage = vec3(-coverage, coverage, -coverage);
						visibility = computeVisibility(u_shadowMap0
										, shadowcoord
										, u_lightShadowBias.x
										, u_shadowSamplingParams
										, texelSize
										);
					}
					else if (selection1)
					{
						vec4 shadowcoord = v_texcoord2;

						float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.4;
						colorCoverage = vec3(coverage, coverage, -coverage);
						visibility = computeVisibility(u_shadowMap1
										, shadowcoord
										, u_lightShadowBias.y
										, u_shadowSamplingParams
										, texelSize/2.0
										);
					}
					else if (selection2)
					{
						vec4 shadowcoord = v_texcoord3;

						float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.4;
						colorCoverage = vec3(-coverage, -coverage, coverage);
						visibility = computeVisibility(u_shadowMap2
										, shadowcoord
										, u_lightShadowBias.z
										, u_shadowSamplingParams
										, texelSize/3.0
										);
					}
					else //selection3
					{
						vec4 shadowcoord = v_texcoord4;

						float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.4;
						colorCoverage = vec3(coverage, -coverage, -coverage);
						visibility = computeVisibility(u_shadowMap3
										, shadowcoord
										, u_lightShadowBias.w
										, u_shadowSamplingParams
										, texelSize/4.0
										);
					}
				}
				else if (u_lightType.x == 1) // Spot light shadow
				{
					vec2 texelSize = vec2_splat(u_shadowMapTexelSize.x);

					float coverage = texcoordInRange(v_texcoord1.xy/v_texcoord1.w) * 0.3;
					colorCoverage = vec3(coverage, -coverage, -coverage);

					visibility = computeVisibility(u_shadowMap0
									, v_texcoord1
									, u_lightShadowBias.x
									, u_shadowSamplingParams
									, texelSize
									);
				}
				else // Point light shadow
				{
					vec3 m_tetraNormalGreen = vec3(0.0, -0.57735026, 0.81649661);
					vec3 m_tetraNormalYellow = vec3(0.0, -0.57735026, -0.81649661);
					vec3 m_tetraNormalBlue = vec3(-0.81649661, 0.57735026, 0.0);
					vec3 m_tetraNormalRed = vec3(0.81649661, 0.57735026, 0.0);

					vec2 texelSize = vec2_splat(u_shadowMapTexelSize.x/4.0);

					vec4 faceSelection;
					vec3 pos = v_ls_position.xyz;
					faceSelection.x = dot(m_tetraNormalGreen,  pos);
					faceSelection.y = dot(m_tetraNormalYellow, pos);
					faceSelection.z = dot(m_tetraNormalBlue,   pos);
					faceSelection.w = dot(m_tetraNormalRed,    pos);

					vec4 shadowcoord;
					float faceMax = max(max(faceSelection.x, faceSelection.y), max(faceSelection.z, faceSelection.w));
					if (faceSelection.x == faceMax)
					{
						shadowcoord = v_texcoord1;

						float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.3;
						colorCoverage = vec3(-coverage, coverage, -coverage);
					}
					else if (faceSelection.y == faceMax)
					{
						shadowcoord = v_texcoord2;

						float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.3;
						colorCoverage = vec3(coverage, coverage, -coverage);
					}
					else if (faceSelection.z == faceMax)
					{
						shadowcoord = v_texcoord3;

						float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.3;
						colorCoverage = vec3(-coverage, -coverage, coverage);
					}
					else // (faceSelection.w == faceMax)
					{
						shadowcoord = v_texcoord4;

						float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.3;
						colorCoverage = vec3(coverage, -coverage, -coverage);
					}

					visibility = computeVisibility(u_shadowMap0
									, shadowcoord
									, u_lightShadowBias.x
									, u_shadowSamplingParams
									, texelSize
									);
				}
			}

			vec3 color = Lo * ao * visibility;
			color = color * vec3(1.0/2.2);

			gl_FragColor = vec4(color, 0.0);
		}
	}
}
