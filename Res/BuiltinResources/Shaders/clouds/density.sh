#ifndef FALCO_CLOUD_DENSITY
#define FALCO_CLOUD_DENSITY
uniform vec4 u_cloudLayer; // base height, thickness, spatial scale, animation speed
        float cloudHash(vec3 p)
        {
            p = fract(p * 0.1031);
            p += dot(p, p.yzx + 33.33);
            return fract((p.x + p.y) * p.z);
        }
        float cloudNoise(vec3 p)
        {
            vec3 cell = floor(p);
            vec3 f = fract(p);
            f = f * f * (3.0 - 2.0 * f);
            return mix(
                mix(mix(cloudHash(cell), cloudHash(cell + vec3(1,0,0)), f.x),
                    mix(cloudHash(cell + vec3(0,1,0)), cloudHash(cell + vec3(1,1,0)), f.x), f.y),
                mix(mix(cloudHash(cell + vec3(0,0,1)), cloudHash(cell + vec3(1,0,1)), f.x),
                    mix(cloudHash(cell + vec3(0,1,1)), cloudHash(cell + vec3(1,1,1)), f.x), f.y), f.z);
        }
        float cloudField(vec3 world, float coverage, float densityScale, vec3 wind, float time)
        {
            // A world-space layer: height in engine units, independent of camera rotation.
            float height = (world.y - u_cloudLayer.x) / max(u_cloudLayer.y, 1.0);
            float profile = smoothstep(0.0, 0.16, height) * (1.0 - smoothstep(0.65, 1.0, height));
            vec3 p = (world - wind * time * u_cloudLayer.w) * u_cloudLayer.z;
            // Differently advected detail bands deform the body as it moves;
            // the shadow pass evaluates this same field at the same time.
            vec3 drift = wind * time * u_cloudLayer.w * u_cloudLayer.z;
            float shape = cloudNoise(p) * 0.57 + cloudNoise(p * 2.03 + drift * 0.21 + 17.0) * 0.29
                        + cloudNoise(p * 4.07 - drift * 0.13 + 31.0) * 0.14;
            float threshold = mix(0.88, 0.18, clamp(coverage, 0.0, 1.0));
            float body = smoothstep(threshold, threshold + 0.22, shape);
            return body * profile * clamp(densityScale, 0.0, 1.0);
        }

#endif
