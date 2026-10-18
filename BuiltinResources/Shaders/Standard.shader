name "Forward/Standard"
render_mode forward

params
{
	define bool "Transparent" TRANSPARENT false
	define bool "Cutout" CUTOUT false
	float "Cutout threshold" cutoutVal 0.25 : 0.0 1.0
	color "Color" vColor 1 1 1 1
	float "Specular" specIntensity 0.5 : 0.0 1.0
	define bool "Has albedo map" HAS_ALBEDO true
	sampler2D "Albedo map" albedoMap 0
	define bool "Has normal map" HAS_NORMAL_MAP false
	sampler2D "Normal map" normalMap 1
	float "Normal map scale" normalMapScale 1.0
	define bool "Has metallic map" HAS_METALLIC_MAP false
	sampler2D "Metallic map" metallicMap 2
	float "Metalness" metalness 0.5 : 0.0 1.0
	define bool "Has roughness map" HAS_ROUGHNESS_MAP false
	sampler2D "Roughness map" roughnessMap 3
	define bool "Has AO map" HAS_AO_MAP false
	sampler2D "AO map" aoMap 4
	define bool "Has emission map" HAS_EMISSION_MAP false
	sampler2D "Emission map" emissionMap 5
	float "Emission" emissionVal 0.0
	vec2 "UV scale" uvScale 1 1
	define bool "Double sided" DOUBLE_SIDED false
}

pass //Ambient pass
{
	tags
	{
		iteration_mode default
		blend_mode = TRANSPARENT ? alpha : replace
		depth_write = TRANSPARENT ? off : on
		backface_culling = DOUBLE_SIDED ? off : cw
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
		vec3 v_bitangent   : BITANGENT = vec3(0.0, 0.0, 0.0);
		vec2 v_texcoord0   : TEXCOORD0 = vec2(0.0, 0.0);
		vec4 v_texcoord1   : TEXCOORD1 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_texcoord2   : TEXCOORD2 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_texcoord3   : TEXCOORD3 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_texcoord4   : TEXCOORD4 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_worldpos    : TEXCOORD6 = vec2(0.0, 0.0, 0.0, 0.0);
		vec4 a_weight      : BLENDWEIGHT;
		vec4 a_indices     : BLENDINDICES;
	}
	
	vertex
	{
		$input a_position, a_normal, a_tangent, a_bitangent, a_texcoord0, a_weight, a_indices
		$output v_position, v_normal, v_tangent, v_bitangent, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_ls_position, v_worldpos

		#include "common.sh"

		uniform mat3 u_normalMatrix;
		uniform mat4 u_shadowMatrix[4];
		uniform vec4 u_skinned;
		uniform mat4 u_boneMatrix[128];
		
		uniform vec4 u_lightPosition;
		uniform vec4 u_lightType;
		uniform vec4 u_lightCastShadows;

		void main()
		{
			mat4 model = u_model[0];
			
			if (u_skinned.x == 1.0)
			{
				model = mul(u_model[0], a_weight.x * u_boneMatrix[int(a_indices.x)] + 
										a_weight.y * u_boneMatrix[int(a_indices.y)] +
										a_weight.z * u_boneMatrix[int(a_indices.z)] +
										a_weight.w * u_boneMatrix[int(a_indices.w)]);
											
				mat3 norm = mul(u_normalMatrix, a_weight.x * mat3(u_boneMatrix[int(a_indices.x)]) + 
												a_weight.y * mat3(u_boneMatrix[int(a_indices.y)]) +
												a_weight.z * mat3(u_boneMatrix[int(a_indices.z)]) +
												a_weight.w * mat3(u_boneMatrix[int(a_indices.w)]));
											
				vec3 vwpos = mul(model, vec4(a_position, 1.0)).xyz;
				gl_Position = mul(u_viewProj, vec4(vwpos, 1.0));
				
				v_normal = mul(norm, a_normal);
				v_tangent = mul(norm, a_tangent);
				v_bitangent = mul(norm, a_bitangent);
			}
			else
			{
				gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
				v_normal = mul(u_normalMatrix, a_normal);
				v_tangent = mul(u_normalMatrix, a_tangent);
				v_bitangent = mul(u_normalMatrix, a_bitangent);
			}
			
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
		
		uniform vec4 vColor;
#if HAS_ALBEDO == 1
		SAMPLER2D(albedoMap, 0);
#endif
		
#if HAS_EMISSION_MAP == 1
		SAMPLER2D(emissionMap, 5);
#endif
		
#if CUTOUT == 1
		uniform vec4 cutoutVal;
#endif
		uniform vec4 u_ambientColor;
		uniform vec4 uvScale;
		uniform vec4 emissionVal;
		
		void main()
		{
			vec2 uvs = v_texcoord0 * uvScale.xy;
			
			vec3 albedo = vec3(1, 1, 1);
			float alpha = 1.0f;
#if HAS_ALBEDO == 1
			vec4 tex = texture2D(albedoMap, uvs);
			albedo = tex.rgb * vColor.rgb;
			
	#if CUTOUT == 1
			if (tex.a < cutoutVal.x)
				discard;
	#else
			alpha = tex.a;
	#endif
#endif

#if HAS_EMISSION_MAP == 1
			float emission = texture2D(emissionMap, uvs).r * emissionVal.x;
#else
			float emission = emissionVal.x;
#endif
			vec3 ambient = (u_ambientColor.rgb * albedo) + (albedo * emission);
			gl_FragColor = vec4(ambient, alpha * vColor.a);
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
		backface_culling = DOUBLE_SIDED ? off : cw
	}
	
	fragment
	{
		$input v_position, v_normal, v_tangent, v_bitangent, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_ls_position, v_worldpos

		#include "common.sh"
		#include "shadows.sh"
		#include "pbr.sh"

		uniform vec4 vColor;
		uniform vec4 specIntensity;

#if HAS_ALBEDO == 1
		SAMPLER2D(albedoMap, 0);
#endif
#if HAS_NORMAL_MAP == 1
		SAMPLER2D(normalMap, 1);
		uniform vec4 normalMapScale;
#endif
#if HAS_METALLIC_MAP == 1
		SAMPLER2D(metallicMap, 2);
		uniform vec4 metalness;
#endif
#if HAS_ROUGHNESS_MAP == 1
		SAMPLER2D(roughnessMap, 3);
#endif
#if HAS_AO_MAP == 1
		SAMPLER2D(aoMap, 4);
#endif

		SAMPLER2DSHADOW(u_shadowMap0, 12);
		SAMPLER2DSHADOW(u_shadowMap1, 13);
		SAMPLER2DSHADOW(u_shadowMap2, 14);
		SAMPLER2DSHADOW(u_shadowMap3, 15);
		
#if CUTOUT == 1
		uniform vec4 cutoutVal;
#endif

		uniform vec4 uvScale;

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

		const float PI = 3.14159265359;

		void main()
		{
			vec2 uvs = v_texcoord0 * uvScale.xy;
			
#if HAS_ALBEDO == 1
			vec4 tex = texture2D(albedoMap, uvs);
			vec3 albedo = tex.rgb * vColor.rgb * 2.2;
			
	#if CUTOUT == 1
				if (tex.a < cutoutVal.x)
					discard;
	#endif
#else
			vec3 albedo = vColor.rgb * 2.2;
#endif
#if HAS_NORMAL_MAP == 1
			vec3 normal = texture2D(normalMap, uvs).rgb;
			normal = normalize(normal * 2.0 - 1.0);
			normal.xy *= normalMapScale.x;
			
			// Expand the tangent basis explicitly. This keeps the transform
			// consistent across GLSL and the D3D shader translation path.
			normal = normalize(v_tangent * normal.x
				+ v_bitangent * normal.y
				+ v_normal * normal.z);
#else
			vec3 normal = normalize(v_normal);
#endif
			// Keep the forward normal convention aligned with StandardDeferred.
			// The renderer's current winding/front-face contract requires the
			// visible front side to be inverted before lighting.
			if (gl_FrontFacing)
				normal = -normal;
			
#if HAS_METALLIC_MAP == 1
			float metallicVal  = texture2D(metallicMap, uvs).r * metalness.x;
#else
			float metallicVal = 0.0;
#endif
			metallicVal = clamp(metallicVal, 0.0, 1.0);
#if HAS_ROUGHNESS_MAP == 1
			float roughness = texture2D(roughnessMap, uvs).r;
#else
			float roughness = 0.5;
#endif
			roughness = clamp(roughness, 0.04, 1.0);
#if HAS_AO_MAP == 1
			float ao = texture2D(aoMap, uvs).r;
#else
			float ao = 1.0;
#endif

			vec3 N = normalize(normal);
			vec3 viewVector = u_camPos.xyz - v_position;
			float viewLengthSq = dot(viewVector, viewVector);
			vec3 V = viewLengthSq > 0.00000001
				? viewVector / sqrt(viewLengthSq)
				: N;

			vec3 F0 = vec3(0.04);
			F0 = mix(F0, albedo, vec3(metallicVal));
			
			// выражение отражающей способности
			vec3 Lo = vec3(0.0);

			float radius = u_lightRadius.x;
			float innerRadius = u_lightRadius.y;
			float outerRadius = u_lightRadius.z;
			float intensity = u_lightIntensity.x;

			// расчет энергетической яркости для каждого источника света
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
			vec3 specular = clamp(
				numerator / denominator * clamp(specIntensity.x, 0.0, 1.0) * 20.0,
				vec3(0.0),
				vec3(0.10));

			vec3 directDiffuse = kD * albedo;
			directDiffuse *= radiance;
			directDiffuse *= NdotL;
			vec3 directSpecular = specular * radiance;
			directSpecular *= NdotL;
			Lo += directDiffuse + directSpecular;

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

			// Material AO belongs to the ambient/indirect pass, as in Deferred.
			vec3 color = Lo * visibility;

			gl_FragColor = vec4(color * vColor.a, 0.0);
		}
	}
}
