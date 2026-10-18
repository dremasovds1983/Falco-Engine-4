name "Forward/Clouds"
render_mode forward

params
{
    float "Coverage" cloudCoverage 0.5 : 0.0 1.0
    float "Density" cloudDensity 1.0 : 0.0 1.0
    vec3 "Wind" cloudWind 1 0 0
    vec3 "Sun direction" cloudSunDirection 0.3 0.8 0.2
}

pass
{
    tags
    {
        blend_mode alpha
        depth_write off
        backface_culling off
    }
    varying
    {
        vec3 a_position : POSITION;
        vec2 v_cloudNdc : TEXCOORD0 = vec2(0.0, 0.0);
    }
    vertex
    {
        $input a_position
        $output v_cloudNdc
        #include "common.sh"
        void main()
        {
            v_cloudNdc = a_position.xy * 2.0 - 1.0;
            gl_Position = vec4(v_cloudNdc, 1.0, 1.0);
        }
    }
    fragment
    {
        $input v_cloudNdc
        #include "common.sh"
        uniform vec4 cloudCoverage;
        uniform vec4 cloudDensity;
        uniform vec4 cloudWind;
        uniform vec4 cloudSunDirection;
        uniform vec4 u_camPos;
        uniform vec4 u_timeScaled;
        uniform vec4 u_rtOrigin;

        #include "clouds/density.sh"
        void main()
        {
            if (cloudCoverage.x <= 0.0 || cloudDensity.x <= 0.0) discard;
            vec4 farPosition = mul(u_invViewProj, vec4(v_cloudNdc, 1.0, 1.0));
            vec4 nearPosition = mul(u_invViewProj, vec4(v_cloudNdc, -u_rtOrigin.y, 1.0));
            vec3 nearWorld = nearPosition.xyz / nearPosition.w;
            vec3 farWorld = farPosition.xyz / farPosition.w;
            vec3 origin = abs(u_proj[3][3]) > 0.5 ? nearWorld : u_camPos.xyz;
            vec3 ray = normalize(farWorld - origin);
            if (abs(ray.y) < 0.001) discard;
            float a = (u_cloudLayer.x - origin.y) / ray.y;
            float b = (u_cloudLayer.x + u_cloudLayer.y - origin.y) / ray.y;
            float enter = max(min(a, b), 0.0);
            float leave = min(max(a, b), 12000.0);
            if (leave <= enter) discard;
            float stepLength = (leave - enter) / 48.0;
            float sunLength = length(cloudSunDirection.xyz);
            vec3 sun = sunLength > 0.0001 ? cloudSunDirection.xyz / sunLength : vec3(0,1,0);
            float transmittance = 1.0;
            vec3 accumulated = vec3(0.0);
            for (int i = 0; i < 48; ++i)
            {
                vec3 samplePosition = origin + ray * (enter + (float(i) + 0.5) * stepLength);
                float density = cloudField(samplePosition, cloudCoverage.x, cloudDensity.x, cloudWind.xyz, u_timeScaled.x);
                if (density > 0.001)
                {
                    // Short sunlight march approximates self-shadowing inside the layer.
                    float sunDepth = cloudField(samplePosition + sun * 35.0, cloudCoverage.x, cloudDensity.x, cloudWind.xyz, u_timeScaled.x) * 35.0
                                   + cloudField(samplePosition + sun * 110.0, cloudCoverage.x, cloudDensity.x, cloudWind.xyz, u_timeScaled.x) * 75.0;
                    float illumination = exp(-sunDepth * 0.018);
                    float silver = pow(max(dot(ray, sun), 0.0), 12.0) * 0.25;
                    vec3 lighting = vec3(0.32, 0.39, 0.50) + vec3(0.68, 0.64, 0.55) * illumination
                                  + vec3(silver * illumination);
                    float opacity = 1.0 - exp(-density * stepLength * 0.018);
                    accumulated += transmittance * opacity * lighting;
                    transmittance *= 1.0 - opacity;
                    if (transmittance < 0.015) break;
                }
            }
            float alpha = 1.0 - transmittance;
            if (alpha < 0.001) discard;
            // Straight alpha output for BGFX_STATE_BLEND_ALPHA.
            gl_FragColor = vec4(accumulated / alpha, alpha);
        }
    }
}
