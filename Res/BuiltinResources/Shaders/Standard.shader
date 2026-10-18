name "Forward/Standard"
render_mode forward

params
{
        define bool "Mesh decals" ENABLE_MESH_DECALS false
	float "Decal normal bias" DECAL_NORMAL_BIAS 0.0020 : 0.0010 0.0035
	float "Decal viewdir bias" DECAL_VIEWDIR_BIAS 0.0005 : 0.0001 0.0010
	float "Edge softness width" DECAL_EDGE_WIDTH 0.0 : 0.0 32768.0
	define bool "Transparent" TRANSPARENT false
	define bool "Cutout" CUTOUT false
	float "Cutout threshold" cutoutVal 0.25 : 0.0 1.0
	color "Color" vColor 1 1 1 1
	float "Specular" specIntensity 0.5 : 0.0 1.0
	define bool "Has albedo map" HAS_ALBEDO_MAP true
	sampler2D "Albedo map" albedoMap 0
	define bool "Linear albedo texture" ALBEDO_LINEAR false
	define bool "Has normal map" HAS_NORMAL_MAP false
	sampler2D "Normal map" normalMap 1
	float "Normal map scale" normalMapScale 1.0
	define bool "Has metallic map" HAS_METALLIC_MAP false
	sampler2D "Metallic map" metallicMap 2
	float "Metalness" metalness 0.0 : 0.0 1.0
	define bool "Has roughness map" HAS_ROUGHNESS_MAP false
	sampler2D "Roughness map" roughnessMap 3
	float "Roughness" roughnessVal 0.5 : 0.0 1.0
	define bool "Has AO map" HAS_AO_MAP false
	sampler2D "AO map" aoMap 4
	define bool "Has emission map" HAS_EMISSION_MAP false
	sampler2D "Emission map" emissionMap 5
	float "Emission" emissionVal 0.0
	vec2 "UV scale" uvScale 1 1
	define bool "Realtime global illumination" ENABLE_GI true
	define bool "Double sided" DOUBLE_SIDED false
}

pass //Ambient pass
{
	tags
	{
		iteration_mode default
		blend_mode = TRANSPARENT ? alpha : replace
		depth_write = TRANSPARENT ? off : on
		depth_func lequal
		backface_culling = DOUBLE_SIDED ? off : cw
	}
	
	varying
	{
		vec3 a_position  : POSITION;
		vec3 a_normal    : NORMAL;
		vec3 a_tangent   : TANGENT;
		vec3 a_bitangent : BITANGENT;
		vec2 a_texcoord0 : TEXCOORD0;
		vec2 a_texcoord1 : TEXCOORD1;
		vec4 a_weight    : BLENDWEIGHT;
		vec4 a_indices   : BLENDINDICES;

		vec3 v_position    : POSITION1 = vec3(0.0, 0.0, 0.0);
		vec3 v_ls_position : POSITION2 = vec3(0.0, 0.0, 0.0);
		vec3 v_normal      : NORMAL    = vec3(0.0, 0.0, 0.0);
		vec3 v_tangent     : TANGENT   = vec3(0.0, 0.0, 0.0);
		vec3 v_bitangent   : BITANGENT = vec3(0.0, 0.0, 0.0);
		vec2 v_texcoord0   : TEXCOORD0 = vec2(0.0, 0.0);
		vec2 v_texcoord1   : TEXCOORD1 = vec2(0.0, 0.0);
		vec4 v_texcoord2   : TEXCOORD2 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_texcoord3   : TEXCOORD3 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_texcoord4   : TEXCOORD4 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_texcoord5   : TEXCOORD5 = vec4(0.0, 0.0, 0.0, 0.0);
		vec4 v_worldpos    : TEXCOORD6 = vec4(0.0, 0.0, 0.0, 0.0);
		vec3 v_decalLocal : TEXCOORD7 = vec3(0.0, 0.0, 0.0);
	}
	
	vertex
	{
		$input a_position, a_normal, a_tangent, a_bitangent, a_texcoord0, a_texcoord1, a_weight, a_indices
		$output v_position, v_normal, v_tangent, v_bitangent, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_texcoord5, v_ls_position, v_worldpos, v_decalLocal

		#include "common.sh"
        #include "camera_space.sh"
        #include "color_space.sh"
		#include "shaderlib.sh"

		uniform mat3 u_normalMatrix;
		uniform mat4 u_shadowMatrix[4];
		uniform vec4 u_skinned;
		uniform mat4 u_boneMatrix[128];
		uniform vec4 u_camPos;
        uniform mat4 u_camProj;
        uniform mat4 u_camInvView;
		
		uniform vec4 u_lightPosition;
		uniform vec4 u_lightType;
		uniform vec4 u_lightCastShadows;
		uniform vec4 DECAL_NORMAL_BIAS;
		uniform vec4 DECAL_VIEWDIR_BIAS;

		void main()
		{
			v_decalLocal = a_position;
			mat4 model = u_model[0];
			vec3 worldPosition;
			
			if (u_skinned.x == 1.0)
			{
				mat4 skin = a_weight.x * u_boneMatrix[int(a_indices.x)]
					+ a_weight.y * u_boneMatrix[int(a_indices.y)]
					+ a_weight.z * u_boneMatrix[int(a_indices.z)]
					+ a_weight.w * u_boneMatrix[int(a_indices.w)];
				model = mul(u_model[0], skin);
				mat3 linearMtx = mat3(model);
				mat3 normalMtx = cofactor(model);
				v_normal = normalize(mul(normalMtx, a_normal));
				v_tangent = normalize(mul(linearMtx, a_tangent));
				v_tangent = normalize(v_tangent - v_normal * dot(v_normal, v_tangent));
				vec3 transformedBitangent = normalize(mul(linearMtx, a_bitangent));
				float handedness = dot(cross(v_normal, v_tangent), transformedBitangent) < 0.0 ? -1.0 : 1.0;
				v_bitangent = normalize(cross(v_normal, v_tangent)) * handedness;
				worldPosition = mul(model, vec4(a_position, 1.0)).xyz;
			}
			else
			{
				v_normal = mul(u_normalMatrix, a_normal);
				v_tangent = mul(u_normalMatrix, a_tangent);
				v_bitangent = mul(u_normalMatrix, a_bitangent);
				worldPosition = mul(model, vec4(a_position, 1.0)).xyz;
			}

#if ENABLE_MESH_DECALS == 1
			worldPosition += normalize(v_normal) * DECAL_NORMAL_BIAS.x;
			worldPosition += falcoSurfaceToCamera(worldPosition, u_camPos.xyz, u_camProj, u_camInvView) * DECAL_VIEWDIR_BIAS.x;
#endif

			gl_Position = mul(u_viewProj, vec4(worldPosition, 1.0));
			
			v_position = worldPosition;
			v_texcoord0 = a_texcoord0;
			v_texcoord1 = a_texcoord1;
			v_worldpos = gl_Position;
			
			vec4 wpos = vec4(0.0, 0.0, 0.0, 1.0);
			
			if (u_lightCastShadows.x == 1)
			{
				if (u_lightType.x == 2) // Directional light
				{
					wpos = mul(model, vec4(a_position, 1.0));
					
					v_texcoord2 = mul(u_shadowMatrix[0], wpos);
					v_texcoord3 = mul(u_shadowMatrix[1], wpos);
					v_texcoord4 = mul(u_shadowMatrix[2], wpos);
					v_texcoord5 = mul(u_shadowMatrix[3], wpos);
				}
				else if (u_lightType.x == 1) // Spot light
				{
					wpos = mul(model, vec4(a_position, 1.0));
					v_texcoord2 = mul(u_shadowMatrix[0], wpos);
				}
				else // Point light
				{
					wpos = vec4(-u_lightPosition.xyz, 0.0) + mul(model, vec4(a_position, 1.0));
					v_ls_position = wpos.xyz;
					v_texcoord2 = mul(u_shadowMatrix[0], wpos);
					v_texcoord3 = mul(u_shadowMatrix[1], wpos);
					v_texcoord4 = mul(u_shadowMatrix[2], wpos);
					v_texcoord5 = mul(u_shadowMatrix[3], wpos);
				}
			}
		}
	}
	
	fragment
	{
		$input v_position, v_normal, v_tangent, v_bitangent, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_texcoord5, v_ls_position, v_worldpos, v_decalLocal
		
		#include "common.sh"
        #include "camera_space.sh"
        #include "color_space.sh"
		
		uniform vec4 vColor;
#if HAS_ALBEDO_MAP == 1
		SAMPLER2D(albedoMap, 0);
#endif

#if HAS_NORMAL_MAP == 1
		SAMPLER2D(normalMap, 1);
		uniform vec4 normalMapScale;
#endif

#if HAS_ROUGHNESS_MAP == 1
		SAMPLER2D(roughnessMap, 3);
#endif
		
		uniform vec4 roughnessVal;
        uniform vec4 metalness;
        uniform vec4 specIntensity;
#if HAS_METALLIC_MAP == 1
        SAMPLER2D(metallicMap, 2);
#endif
#if HAS_AO_MAP == 1
        SAMPLER2D(aoMap, 4);
#endif
		
#if HAS_EMISSION_MAP == 1
		SAMPLER2D(emissionMap, 5);
#endif

#if ENABLE_GI == 1
		SAMPLERCUBE(u_envMap, 9);
        #include "ibl_material.sh"
#endif
		
#if CUTOUT == 1
		uniform vec4 cutoutVal;
#endif
		
		SAMPLER2D(u_lightMap, 8);
		uniform vec4 u_hasLightMap;
		
#if ENABLE_MESH_DECALS == 1
        uniform vec4 DECAL_EDGE_WIDTH;
        SAMPLER2D(u_decalBoundaryTree, 7);
        uniform vec4 u_decalBoundaryCount;

        vec4 decalBoundaryNode(float index)
        {
            vec2 uv = vec2(mod(index, 256.0) + 0.5, floor(index / 256.0) + 0.5);
            uv /= vec2(256.0, max(u_decalBoundaryCount.y, 1.0));
            return texture2DLod(u_decalBoundaryTree, uv, 0.0);
        }
        float decalEdgeOpacity(vec3 position)
        {
            if (DECAL_EDGE_WIDTH.x <= 0.0 || u_decalBoundaryCount.x <= 0.0) return 1.0;
            float distanceSquared = DECAL_EDGE_WIDTH.x * DECAL_EDGE_WIDTH.x;
            float node = 0.0;
            // Each step advances or skips a subtree; no stack and no truncated contour.
            while (node < u_decalBoundaryCount.x)
            {
                vec4 lo = decalBoundaryNode(node * 4.0);
                vec4 hi = decalBoundaryNode(node * 4.0 + 1.0);
                vec3 outside = max(max(lo.xyz - position, position - hi.xyz), vec3_splat(0.0));
                if (dot(outside, outside) >= distanceSquared) { node = lo.w; continue; }
                if (hi.w > 0.5)
                {
                    vec3 start = decalBoundaryNode(node * 4.0 + 2.0).xyz;
                    vec3 segment = decalBoundaryNode(node * 4.0 + 3.0).xyz - start;
                    float t = clamp(dot(position - start, segment) / max(dot(segment, segment), 1e-20), 0.0, 1.0);
                    vec3 offset = position - start - t * segment;
                    distanceSquared = min(distanceSquared, dot(offset, offset));
                }
                node += 1.0;
            }
            return smoothstep(0.0, DECAL_EDGE_WIDTH.x, sqrt(distanceSquared));
        }
#endif

		uniform vec4 u_ambientColor;
		uniform vec4 uvScale;
		uniform vec4 emissionVal;
		uniform vec4 u_giParams;
		uniform vec4 u_camPos;
        uniform mat4 u_camProj;
        uniform mat4 u_camInvView;
		
		void main()
		{
			vec2 uvs = v_texcoord0 * uvScale.xy;
			
			vec3 albedo = falcoSRGBToLinear(vColor.rgb);
			float alpha = vColor.a;
			
#if HAS_ALBEDO_MAP == 1
			vec4 tex = texture2D(albedoMap, uvs);
#if ALBEDO_LINEAR == 0
            tex.rgb = falcoSRGBToLinear(tex.rgb);
#endif
			albedo = tex.rgb * falcoSRGBToLinear(vColor.rgb);
			
	#if CUTOUT == 1
			if (tex.a < cutoutVal.x)
				discard;
	#else
			alpha = tex.a * vColor.a;
	#endif
#endif

#if ENABLE_MESH_DECALS == 1
            alpha *= decalEdgeOpacity(v_decalLocal);
#endif

#if HAS_EMISSION_MAP == 1
			float emission = texture2D(emissionMap, uvs).r * emissionVal.x;
#else
			float emission = emissionVal.x;
#endif

			vec3 lightMap = vec3(0.0);
			
			if (u_hasLightMap.x == 1)
			{
				lightMap = texture2D(u_lightMap, v_texcoord1).rgb;
			}
			
            vec3 ambient = u_ambientColor.rgb * albedo;
#if HAS_AO_MAP == 1
            float ao = clamp(texture2D(aoMap, uvs).r, 0.0, 1.0);
#else
            float ao = 1.0;
#endif
#if ENABLE_GI == 1
            if (u_giParams.x > 0.5)
            {
                if (u_giParams.z > 0.5)
                {
                    vec3 normal = normalize(v_normal);
#if HAS_NORMAL_MAP == 1
                    vec3 mapNormal = texture2D(normalMap, uvs).rgb * 2.0 - 1.0;
                    mapNormal.xy *= normalMapScale.x;
                    normal = normalize(v_tangent * mapNormal.x + v_bitangent * mapNormal.y + v_normal * mapNormal.z);
#endif
                    vec3 v = falcoSurfaceToCamera(v_position, u_camPos.xyz, u_camProj, u_camInvView);
                    if (dot(normal, v) < 0.0) normal = -normal;
                    float metallic = metalness.x;
#if HAS_METALLIC_MAP == 1
                    metallic *= texture2D(metallicMap, uvs).r;
#endif
                    metallic = clamp(metallic, 0.0, 1.0);
                    float roughness = roughnessVal.x;
#if HAS_ROUGHNESS_MAP == 1
                    roughness *= texture2D(roughnessMap, uvs).r;
#endif
                    roughness = clamp(roughness, 0.04, 1.0);
                    vec3 f0 = mix(vec3_splat(0.04), albedo, metallic);
                    vec3 diffuse = albedo * (vec3_splat(1.0)-f0) * (1.0-metallic) * iblDiffuse(normal);
                    vec3 specular = iblSpecular(normal, v, f0, roughness) * clamp(specIntensity.x, 0.0, 1.0);
                    ambient = diffuse + specular;
                }
                ambient *= max(u_giParams.y, 0.0);
            }
#endif
            ambient = ambient * ao + albedo * max(emission, 0.0);
			gl_FragColor = vec4(ambient + (albedo * lightMap), alpha);
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
		$input v_position, v_normal, v_tangent, v_bitangent, v_texcoord0, v_texcoord1, v_texcoord2, v_texcoord3, v_texcoord4, v_texcoord5, v_ls_position, v_worldpos, v_decalLocal

		#include "common.sh"
        #include "camera_space.sh"
        #include "color_space.sh"
		#include "shadows.sh"
		#include "pbr.sh"
		#include "clouds/shadow.sh"

		uniform vec4 vColor;
		uniform vec4 specIntensity;

#if HAS_ALBEDO_MAP == 1
		SAMPLER2D(albedoMap, 0);
#endif
#if HAS_NORMAL_MAP == 1
		SAMPLER2D(normalMap, 1);
		uniform vec4 normalMapScale;
#endif
#if HAS_METALLIC_MAP == 1
		SAMPLER2D(metallicMap, 2);
#endif
		uniform vec4 metalness;
#if HAS_ROUGHNESS_MAP == 1
		SAMPLER2D(roughnessMap, 3);
#endif
		uniform vec4 roughnessVal;
#if HAS_AO_MAP == 1
		SAMPLER2D(aoMap, 4);
#endif

		SAMPLER2D(u_lightMap, 8);
		uniform vec4 u_hasLightMap;
		
#if ENABLE_GI == 1
		SAMPLERCUBE(u_envMap, 11);
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
		
#if ENABLE_GI == 1
		uniform vec4 u_giParams; // x - enabled, y - power
#endif

		const float PI = 3.14159265359;

		void main()
		{
#if ENABLE_MESH_DECALS == 1
			// Mesh decals are composited over the already-lit deferred surface in
			// the ambient pass. Adding the per-light result again double-lights
			// the receiver and produces severe view-dependent overexposure.
			gl_FragColor = vec4(0.0);
			return;
#endif

			vec2 uvs = v_texcoord0 * uvScale.xy;
			
			float alpha = 1.0;
			
#if HAS_ALBEDO_MAP == 1
			vec4 tex = texture2D(albedoMap, uvs);
#if ALBEDO_LINEAR == 0
            tex.rgb = falcoSRGBToLinear(tex.rgb);
#endif
			vec3 albedo = tex.rgb * falcoSRGBToLinear(vColor.rgb);
			alpha = tex.a * vColor.a;
			
	#if CUTOUT == 1
				if (tex.a < cutoutVal.x)
					discard;
	#endif
#else
			vec3 albedo = falcoSRGBToLinear(vColor.rgb);
			alpha = vColor.a;
#endif

			vec4 lightMap = texture2D(u_lightMap, v_texcoord1);
			lightMap.a = u_hasLightMap.x;

#if HAS_NORMAL_MAP == 1
			vec3 normal = texture2D(normalMap, uvs).rgb;
			normal = normalize(normal * 2.0 - 1.0);
			normal.xy *= normalMapScale.x;
			
			// Expand T*x + B*y + N*z explicitly so the tangent basis keeps the
			// same interpretation through the D3D shader translation path.
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
			float metallicVal = metalness.x;
#endif
			metallicVal = clamp(metallicVal, 0.0, 1.0);
#if HAS_ROUGHNESS_MAP == 1
			float roughness = texture2D(roughnessMap, uvs).r * roughnessVal.x;
#else
			float roughness = roughnessVal.x;
#endif
			roughness = clamp(roughness, 0.04, 1.0);
#if HAS_AO_MAP == 1
			float ao = texture2D(aoMap, uvs).r;
#else
			float ao = 1.0;
#endif

			vec3 N = normalize(normal);
			vec3 V = falcoSurfaceToCamera(v_position, u_camPos.xyz, u_camProj, u_camInvView);

			vec3 F0 = vec3(0.04);
			F0 = mix(F0, albedo, vec3(metallicVal));
			
			vec3 Lo = vec3(0.0);

			float radius = u_lightRadius.x;
			float innerRadius = u_lightRadius.y;
			float outerRadius = u_lightRadius.z;
			float intensity = u_lightIntensity.x;

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
			
			// Direct light is independent of the ambient GI environment.
			if (lightMap.a != 1 || u_lightRenderMode.x == 0)
			{
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
											v_texcoord2,
											v_texcoord3,
											v_texcoord4,
											v_texcoord5,
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
			
			//Lightmap
			if (lightMap.a == 1)
			{
				if (u_lightRenderMode.x > 0) // Mixed or baked
				{
					float lightMapShadow = min(visibility, max(max(lightMap.r, lightMap.g), lightMap.b));
					visibility = clamp(lightMapShadow * 10.0, 0.0, 1.0);
				}
			}
				
			// Material AO belongs to the ambient/indirect pass, as in Deferred.
			vec3 color = Lo * visibility;
			
			gl_FragColor = vec4(color * alpha, 0.0);
		}
	}
}
