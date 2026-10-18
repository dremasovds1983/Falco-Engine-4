name "Deferred/Temporary Decal"
render_mode deferred

params
{
    define bool "Temporary decals" TEMPORARY_DECAL true
    int "Maximum count" decalMaxCount 200 : 1 200
    float "Lifetime (seconds)" decalLifetime 60.0 : 0.0 3600.0
	define bool "Cutout" CUTOUT true
	float "Cutout threshold" cutoutVal 0.2 : 0.0 1.0
	color "Color" vColor 1 1 1 1
	float "Specular" specIntensity 0.0 : 0.0 1.0
	define bool "Has albedo map" HAS_ALBEDO_MAP true
	sampler2D "Albedo map" t_albedoMap 5
	define bool "Linear albedo texture" ALBEDO_LINEAR false
	define bool "Has normal map" HAS_NORMAL_MAP false
	sampler2D "Normal map" t_normalMap 6
	float "Normal map scale" normalMapScale 1.0
	define bool "Has metallic map" HAS_METALLIC_MAP false
	sampler2D "Metallic map" t_metallicMap 7
	float "Metalness" metalness 0.0 : 0.0 1.0
	define bool "Has roughness map" HAS_ROUGHNESS_MAP false
	sampler2D "Roughness map" t_roughnessMap 8
	float "Roughness" roughnessVal 0.0 : 0.0 1.0
	define bool "Has AO map" HAS_AO_MAP false
	sampler2D "AO map" t_aoMap 9
	define bool "Has emission map" HAS_EMISSION_MAP false
	sampler2D "Emission map" t_emissionMap 10
	float "Emission" emissionVal 0.0
	vec2 "UV scale" uvScale 1 1
	vec2 "UV offset" uvOffset 0 0
}

pass
{
	tags
	{
		
	}
	
	varying
	{
		vec3 a_position  : POSITION;
	}
	
	vertex
	{
		$input a_position
		#include "common.sh"
        #include "color_space.sh"
		
		uniform mat4 u_invVP;
		
		void main()
		{
			gl_Position = vec4(a_position.xy * 2.0 - 1.0, 0.0, 1.0);
		}
	}
	
	fragment
	{
		#include "common.sh"
        #include "color_space.sh"
		#include "shaderlib.sh"
		
		uniform vec4 vColor;
		uniform vec4 specIntensity;
		
		uniform mat4 u_VP;
		
		SAMPLER2D(u_albedoMap, 0);
		SAMPLER2D(u_normalMap, 1);
		SAMPLER2D(u_mraMap, 2);
		SAMPLER2D(u_lightMap, 3);
		SAMPLER2D(u_depthMap, 4);
		
#if CUTOUT == 1
		uniform vec4 cutoutVal;
#endif
		
#if HAS_ALBEDO_MAP == 1
		SAMPLER2D(t_albedoMap, 5);
#endif
#if HAS_NORMAL_MAP == 1
		SAMPLER2D(t_normalMap, 6);
		uniform vec4 normalMapScale;
#endif
#if HAS_METALLIC_MAP == 1
		SAMPLER2D(t_metallicMap, 7);
#endif
		uniform vec4 metalness;
#if HAS_ROUGHNESS_MAP == 1
		SAMPLER2D(t_roughnessMap, 8);
#endif
#if HAS_AO_MAP == 1
		SAMPLER2D(t_aoMap, 9);
#endif
#if HAS_EMISSION_MAP == 1
		SAMPLER2D(t_emissionMap, 10);
#endif
		
		uniform vec4 u_ambientColor;
		uniform vec4 u_rtOrigin;
		uniform vec4 uvScale;
		uniform vec4 uvOffset;
		uniform vec4 roughnessVal;
		uniform vec4 emissionVal;
		
		vec3 world_position_from_depth(vec2 screen_pos, float device_depth)
		{
			float depth = mix(device_depth, device_depth * 2.0 - 1.0, u_rtOrigin.y);
			vec2 clip_pos = screen_pos;
			clip_pos.y = mix(-clip_pos.y, clip_pos.y, u_rtOrigin.x);
			vec4 ndc_pos = vec4(clip_pos, depth, 1.0);
			vec4 world_pos = mul(u_invViewProj, ndc_pos);
			return world_pos.xyz / world_pos.w;
		}
		
		void main()
		{
			// Use the actual pixel coordinate for G-buffer sampling. Interpolating
			// clip position across the faces of the projector cube makes the UV depend
			// on the particular cube triangle and produces large triangular wedges.
			vec2 buffer_uv = gl_FragCoord.xy * u_viewTexel.xy;
			vec2 screen_uv = vec2(buffer_uv.x, mix(buffer_uv.y, 1.0 - buffer_uv.y, u_rtOrigin.x));
			vec2 screen_pos = screen_uv * 2.0 - 1.0;
			
			float depth = texture2D(u_depthMap, buffer_uv).x;
			vec4 albedoSrc = texture2D(u_albedoMap, buffer_uv);
			vec4 normalSrc = texture2D(u_normalMap, buffer_uv);
			vec4 mraSrc = texture2D(u_mraMap, buffer_uv);
			vec4 lightMapSrc = texture2D(u_lightMap, buffer_uv);
            // Cleared G-buffer pixels have no receiver. Preserve them instead of
            // projecting a decal onto the far plane or normalizing an empty normal.
            vec3 receiverNormal = decodeNormalUint(normalSrc.xyz);
            if (depth >= 1.0 || dot(receiverNormal, receiverNormal) < 0.00001)
            {
                gl_FragData[0] = albedoSrc;
                gl_FragData[1] = normalSrc;
                gl_FragData[2] = mraSrc;
                gl_FragData[3] = lightMapSrc;
                return;
            }
			vec3  world_pos = world_position_from_depth(screen_pos, depth);
			
			vec4 ndc_pos = mul(u_VP, vec4(world_pos, 1.0));
			ndc_pos.xyz /= ndc_pos.w;
			
			if (ndc_pos.x < -1.0 || ndc_pos.x > 1.0 || ndc_pos.y < -1.0 || ndc_pos.y > 1.0 || ndc_pos.z < -1.0 || ndc_pos.z > 1.0)
			{
				gl_FragData[0] = albedoSrc;
				gl_FragData[1] = normalSrc;
				gl_FragData[2] = mraSrc;
				gl_FragData[3] = lightMapSrc;
				return;
			}
			
			vec2 decal_tex_coord = ndc_pos.xy * 0.5 + 0.5;
			decal_tex_coord.x = 1.0 - decal_tex_coord.x;
			decal_tex_coord = decal_tex_coord * uvScale.xy + uvOffset.xy;
			
#if HAS_ALBEDO_MAP == 1
			vec4 diffuseColor = texture2D(t_albedoMap, decal_tex_coord);
#if ALBEDO_LINEAR == 0
            diffuseColor.rgb = falcoSRGBToLinear(diffuseColor.rgb);
#endif
            diffuseColor *= falcoSRGBToLinear(vColor);
			
	#if CUTOUT == 1
			if (diffuseColor.a < cutoutVal.x)
			{
				gl_FragData[0] = albedoSrc;
				gl_FragData[1] = normalSrc;
				gl_FragData[2] = mraSrc;
				gl_FragData[3] = lightMapSrc;
				return;
			}
	#endif
#else
			vec4 diffuseColor = falcoSRGBToLinear(vColor);
#endif
#if HAS_METALLIC_MAP == 1
			float metallicVal  = texture2D(t_metallicMap, decal_tex_coord).r * metalness.x;
#else
			float metallicVal = metalness.x;
#endif
#if HAS_ROUGHNESS_MAP == 1
			float roughness = texture2D(t_roughnessMap, decal_tex_coord).r * roughnessVal.x;
#else
			float roughness = roughnessVal.x;
#endif
#if HAS_AO_MAP == 1
			float ao = texture2D(t_aoMap, decal_tex_coord).r;
#else
			float ao = 1.0;
#endif
#if HAS_EMISSION_MAP == 1
			float emission = texture2D(t_emissionMap, decal_tex_coord).r * emissionVal.x;
#else
			float emission = emissionVal.x;
#endif
#if HAS_NORMAL_MAP == 1
			vec3 normal = texture2D(t_normalMap, decal_tex_coord).rgb;
			normal = normal * 2.0 - 1.0;
            normal = dot(normal, normal) > 0.00001 ? normalize(normal) : vec3_splat(0.0);
			normal.xy *= normalMapScale.x;
#else
			vec3 normal = vec3(0.0);
#endif
			
			float decalAlpha = clamp(diffuseColor.a, 0.0, 1.0);
			vec3 baseNormal = normalize(receiverNormal);
#if HAS_NORMAL_MAP == 1
			vec3 decalNormal = baseNormal + normal;
            decalNormal = dot(decalNormal, decalNormal) > 0.00001 ? normalize(decalNormal) : baseNormal;
            vec3 composedNormal = mix(baseNormal, decalNormal, decalAlpha);
            composedNormal = dot(composedNormal, composedNormal) > 0.00001 ? normalize(composedNormal) : baseNormal;
#else
			vec3 composedNormal = baseNormal;
#endif
			vec3 decalAlbedo = diffuseColor.rgb;
            emission = max(emission, 0.0);
			vec4 decalMra = vec4(metallicVal, roughness, ao, specIntensity.x);
			
			gl_FragData[0] = vec4(mix(albedoSrc.rgb, decalAlbedo, decalAlpha), mix(albedoSrc.a, emission, decalAlpha));
			gl_FragData[1] = vec4(encodeNormalUint(composedNormal), normalSrc.a);
			gl_FragData[2] = mix(mraSrc, decalMra, decalAlpha);
			gl_FragData[3] = lightMapSrc;
		}
	}
}
