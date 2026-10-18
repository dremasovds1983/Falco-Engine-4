pass
{
	varying
	{
		vec3 v_ls_position : POSITION2 = vec3(0.0, 0.0, 0.0);
		vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);

		vec3 a_position  : POSITION;
		vec2 a_texcoord0 : TEXCOORD0;
	}
	
	vertex
	{
		$input a_position, a_texcoord0
		$output v_texcoord0
		
		#include "common.sh"

		void main()
		{
			gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0) );
			v_texcoord0 = a_texcoord0;
		}
	}
	
	fragment
	{
		$input v_texcoord0
		
		#include "common.sh"
		#include "shaderlib.sh"
		#include "shadows.sh"
		#include "pbr.sh"
		
		SAMPLER2D(u_albedoMap, 0); // Rgb, 1.0
		SAMPLER2D(u_normalMap, 1); //Rgb, 1.0
		SAMPLER2D(u_mraMap, 2); //Metallic, roughness, ao, specularity
		SAMPLER2D(u_depthMap, 3); //Depth
		
		SAMPLER2DSHADOW(u_shadowMap0, 12);
		SAMPLER2DSHADOW(u_shadowMap1, 13);
		SAMPLER2DSHADOW(u_shadowMap2, 14);
		SAMPLER2DSHADOW(u_shadowMap3, 15);
		
		uniform mat4 u_invVP;
		
		uniform mat4 u_shadowMatrix[4];
		
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
			vec3  normal      = decodeNormalUint(texture2D(u_normalMap, v_texcoord0).xyz);
			float deviceDepth = texture2D(u_depthMap, v_texcoord0).r;
			float depth       = toClipSpaceDepth(deviceDepth);

			vec3 clip = vec3(v_texcoord0 * 2.0 - 1.0, depth);
			vec3 wpos = clipToWorld(u_invVP, clip);
			
			vec3 v_ls_position = vec3(0.0, 0.0, 0.0);
			
			vec4 v_texcoord1 = vec4(0.0, 0.0, 0.0, 0.0);
			vec4 v_texcoord2 = vec4(0.0, 0.0, 0.0, 0.0);
			vec4 v_texcoord3 = vec4(0.0, 0.0, 0.0, 0.0);
			vec4 v_texcoord4 = vec4(0.0, 0.0, 0.0, 0.0);
		
			if (u_lightCastShadows.x == 1)
			{
				if (u_lightType.x == 2) // Directional light
				{
					v_texcoord1 = mul(u_shadowMatrix[0], vec4(wpos, 1.0));
					v_texcoord2 = mul(u_shadowMatrix[1], vec4(wpos, 1.0));
					v_texcoord3 = mul(u_shadowMatrix[2], vec4(wpos, 1.0));
					v_texcoord4 = mul(u_shadowMatrix[3], vec4(wpos, 1.0));
				}
				else if (u_lightType.x == 1) // Spot light
				{
					v_texcoord1 = mul(u_shadowMatrix[0], vec4(wpos, 1.0));
				}
				else // Point light
				{
					vec3 _position = -u_lightPosition.xyz + wpos;
					v_ls_position = _position;
					v_texcoord1 = mul(u_shadowMatrix[0], vec4(_position, 1.0));
					v_texcoord2 = mul(u_shadowMatrix[1], vec4(_position, 1.0));
					v_texcoord3 = mul(u_shadowMatrix[2], vec4(_position, 1.0));
					v_texcoord4 = mul(u_shadowMatrix[3], vec4(_position, 1.0));
				}
			}

			vec3 albedo = texture2D(u_albedoMap, v_texcoord0).rgb * 2.2;
			
			vec4 mra = texture2D(u_mraMap, v_texcoord0);
			float metallicVal  = mra.x;
			float roughness = mra.y;
			float ao = mra.z;

			vec3 N = normalize(normal);
			vec3 V = normalize(u_camPos.xyz - wpos);

			vec3 F0 = vec3(0.04);
			F0 = mix(F0, albedo, vec3(metallicVal));
			
			// выражение отражающей способности
			vec3 Lo = vec3(0.0);

			float radius = u_lightRadius.x;
			float innerRadius = u_lightRadius.y;
			float outerRadius = u_lightRadius.z;
			float intensity = u_lightIntensity.x * 10.0 / 2.2;

			// расчет энергетической яркости для каждого источника света
			vec3 L;
			if (u_lightType.x != 2)
				L = normalize(u_lightPosition.xyz - wpos);
			else
				L = normalize(-u_lightDirection.xyz);

			vec3 H = normalize(V + L);

			float distance = length(u_lightPosition.xyz - wpos);

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
			vec3 specular = numerator / max(denominator, 0.001) * mra.w;

			// прибавляем результат к исходящей энергетической яркости Lo
			float NdotL = max(dot(N, L), 0.0);
			Lo += (kD * albedo / PI + specular) * radiance * NdotL; 

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
