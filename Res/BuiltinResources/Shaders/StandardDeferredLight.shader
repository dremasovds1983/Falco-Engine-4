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
        #include "camera_space.sh"

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
        #include "camera_space.sh"
		#include "shaderlib.sh"
		#include "shadows.sh"
		#include "pbr.sh"
		#include "clouds/shadow.sh"
		
		SAMPLER2D(u_albedoMap, 0); // Rgb, 1.0
		SAMPLER2D(u_normalMap, 1); //Rgb, 1.0
		SAMPLER2D(u_mraMap, 2); //Metallic, roughness, ao, specularity
		SAMPLER2D(u_lightMap, 3); //LightMap
		SAMPLER2D(u_depthMap, 4); //Depth
		
		SAMPLER2DSHADOW(u_shadowMap0, 12);
		SAMPLER2DSHADOW(u_shadowMap1, 13);
		SAMPLER2DSHADOW(u_shadowMap2, 14);
		SAMPLER2DSHADOW(u_shadowMap3, 15);
		
		uniform mat4 u_invVP;
		
		uniform mat4 u_shadowMatrix[4];
		
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
		uniform vec4 u_rtOrigin; // x = flip render-target V, y = homogeneous clip-space depth

        // D24 camera depth quantization is magnified by inverse perspective.
        // Bound its error in each directional shadow map's normalized depth.
        float cameraDepthShadowError(mat4 shadowMatrix, vec3 nearWorld, vec3 farWorld)
        {
            vec4 a = mul(shadowMatrix, vec4(nearWorld, 1.0));
            vec4 b = mul(shadowMatrix, vec4(farWorld, 1.0));
            if (abs(a.w) < 0.000001 || abs(b.w) < 0.000001) return 0.0;
            return abs(a.z / a.w - b.z / b.w);
        }

		void main()
		{
			vec2 bufferUV = vec2(v_texcoord0.x, mix(v_texcoord0.y, 1.0 - v_texcoord0.y, u_rtOrigin.x));
			vec3  normal      = decodeNormalUint(texture2D(u_normalMap, bufferUV).xyz);
			
			float deviceDepth = texture2D(u_depthMap, bufferUV).r;
			float depth       = mix(deviceDepth, deviceDepth * 2.0 - 1.0, u_rtOrigin.y);

			// G-buffer sampling follows the render-target origin, while clip-space Y
			// follows the graphics API convention. D3D requires the NDC Y flip used
			// by the modern bgfx deferred examples.
			vec2 clipXY = v_texcoord0 * 2.0 - 1.0;
			clipXY.y = mix(-clipXY.y, clipXY.y, u_rtOrigin.x);
			vec3 clip = vec3(clipXY, depth);
			vec3 wpos = clipToWorld(u_invVP, clip);
            vec4 receiverShadowBias = u_lightShadowBias;
            if (u_lightCastShadows.x > 0.5 && u_lightType.x == 2)
            {
                // Two D24 units include depth quantization and reconstruction rounding.
                const float depthError = 2.0 / 16777215.0;
                float nearDepth = max(deviceDepth - depthError, 0.0);
                float farDepth = min(deviceDepth + depthError, 1.0);
                vec3 nearWorld = clipToWorld(u_invVP, vec3(clipXY, mix(nearDepth, nearDepth * 2.0 - 1.0, u_rtOrigin.y)));
                vec3 farWorld = clipToWorld(u_invVP, vec3(clipXY, mix(farDepth, farDepth * 2.0 - 1.0, u_rtOrigin.y)));
                receiverShadowBias += vec4(
                    cameraDepthShadowError(u_shadowMatrix[0], nearWorld, farWorld),
                    cameraDepthShadowError(u_shadowMatrix[1], nearWorld, farWorld),
                    cameraDepthShadowError(u_shadowMatrix[2], nearWorld, farWorld),
                    cameraDepthShadowError(u_shadowMatrix[3], nearWorld, farWorld));
            }

			
			vec3 v_ls_position = vec3(0.0, 0.0, 0.0);
			
			vec4 v_texcoord1 = vec4(0.0, 0.0, 0.0, 0.0);
			vec4 v_texcoord2 = vec4(0.0, 0.0, 0.0, 0.0);
			vec4 v_texcoord3 = vec4(0.0, 0.0, 0.0, 0.0);
			vec4 v_texcoord4 = vec4(0.0, 0.0, 0.0, 0.0);
		
			if (u_lightCastShadows.x == 1 && u_lightRenderMode.x < 2)
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

			vec3 albedo = texture2D(u_albedoMap, bufferUV).rgb;
			vec4 lightMap = texture2D(u_lightMap, bufferUV);
			
			vec4 mra = texture2D(u_mraMap, bufferUV);
			// CS209 measured the current modern G-buffer directly: x=0 for the
			// default non-metal material, while z=1 is material AO. Reading z as
			// metallic erases Lambert and leaves only the view-dependent GGX lobe.
			float metallicVal = clamp(mra.x, 0.0, 1.0);
			float roughness = clamp(mra.y, 0.04, 1.0);
			float specularStrength = clamp(mra.w, 0.0, 1.0);

			vec3 N = normalize(normal);
			vec3 V = falcoSurfaceToCamera(wpos, u_camPos.xyz, u_camProj, u_camInvView);

			vec3 F0 = vec3(0.04);
			F0 = mix(F0, albedo, vec3(metallicVal));
			
			vec3 Lo = vec3(0.0);

			float radius = u_lightRadius.x;
			float innerRadius = u_lightRadius.y;
			float outerRadius = u_lightRadius.z;
			float intensity = u_lightIntensity.x;

			vec3 L;
			if (u_lightType.x != 2)
				L = normalize(u_lightPosition.xyz - wpos);
			else
				L = normalize(-u_lightDirection.xyz);
			
			// V and L can be exact opposites. normalize(vec3(0)) is undefined and
			// poisons Fresnel/GGX on D3D, saturating the additive BGRA8 light buffer.
			vec3 halfVector = V + L;
			float halfLengthSq = dot(halfVector, halfVector);
			vec3 H = halfLengthSq > 0.00000001
				? halfVector / sqrt(halfLengthSq)
				: N;

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

			vec3 radiance = u_lightColor.rgb * attenuation;
            if (u_lightType.x == 2) radiance *= cloudSunVisibility(wpos, -u_lightDirection.xyz);

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
			vec3 specular = hdrSpecularBRDF(numerator, denominator, specularStrength);
			
			if (lightMap.a != 1 || u_lightRenderMode.x == 0)
			{
				// Realtime GI is an indirect/ambient term. It must not suppress or
				// tint the direct diffuse contribution when the environment is empty.
				vec3 directDiffuse = lambertBRDF(albedo, kD);
				directDiffuse *= radiance;
				directDiffuse *= NdotL;

				vec3 directSpecular = specular * radiance;
				directSpecular *= NdotL;

				Lo += directDiffuse + directSpecular;
			}
			else
			{
				vec3 lightInv = vec3(1.0) - lightMap.rgb;
				vec3 light = lightMap.rgb;

				vec3 mixedDiffuse = lambertBRDF(albedo, kD);
				mixedDiffuse *= 0.5;
				vec3 mixedBrdf = mixedDiffuse + specular;
				mixedBrdf *= radiance;
				mixedBrdf *= NdotL;
				Lo += mixedBrdf;
			}

			float visibility = 1.0;
			
			if (u_lightCastShadows.x == 1)
			{
				if (u_lightRenderMode.x < 2) // Realtime or mixed
				{
					visibility = getShadow(int(u_lightType.x),
											v_texcoord1,
											v_texcoord2,
											v_texcoord3,
											v_texcoord4,
											receiverShadowBias,
											u_shadowSamplingParams,
											u_shadowCascadeSplits,
											dot(wpos - u_camPos.xyz, normalize(u_camDir.xyz)),
											v_ls_position,
											u_shadowMapTexelSize.x,
											u_shadowMap0,
											u_shadowMap1,
											u_shadowMap2,
											u_shadowMap3);
				}
			}
			
			//Lightmap
			if (lightMap.a == 1)
			{
				if (u_lightRenderMode.x > 0) // Mixed or baked
				{
					float lightMapShadow = min(visibility, max(max(lightMap.r, lightMap.g), lightMap.b));
					visibility = clamp(lightMapShadow * 10.0, 0.0, 1.0);
				}
			}

			// Material AO belongs to the indirect/ambient term. Applying it here
			// can erase all direct lighting when the G-buffer AO channel is empty.
			vec3 color = Lo * visibility;

			
			gl_FragColor = vec4(color, 0.0);
		}
	}
}
