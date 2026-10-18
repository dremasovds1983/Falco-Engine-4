#ifndef FALCO_CLOUD_SHADOW
#define FALCO_CLOUD_SHADOW
#include "density.sh"
uniform vec4 u_cloudShadowParams; // coverage, density, enabled, scaled time
uniform vec4 u_cloudShadowWind;
float cloudSunVisibility(vec3 world, vec3 toSun)
{
    if (u_cloudShadowParams.z < 0.5 || u_cloudShadowParams.x <= 0.0 || u_cloudShadowParams.y <= 0.0) return 1.0;
    float len = length(toSun);
    if (len < 0.0001) return 1.0;
    vec3 ray = toSun / len;
    if (ray.y <= 0.001 || world.y >= u_cloudLayer.x + u_cloudLayer.y) return 1.0;
    float enter = max((u_cloudLayer.x - world.y) / ray.y, 0.0);
    float leave = (u_cloudLayer.x + u_cloudLayer.y - world.y) / ray.y;
    float stepLength = (leave - enter) / 16.0;
    float opticalDepth = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        vec3 samplePosition = world + ray * (enter + (float(i) + 0.5) * stepLength);
        opticalDepth += cloudField(samplePosition, u_cloudShadowParams.x, u_cloudShadowParams.y,
                                  u_cloudShadowWind.xyz, u_cloudShadowParams.w) * stepLength;
    }
    return exp(-opticalDepth * 0.018);
}
#endif
