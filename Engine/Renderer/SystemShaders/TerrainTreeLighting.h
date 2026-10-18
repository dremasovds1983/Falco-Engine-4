#pragma once
#include <string>
#include "IBL.h"
namespace shaders
{
    // Shared by runtime billboard passes; capture stores unlit linear albedo.
    static const std::string terrainTreeLighting = iblFunctions + R"SHADER(
vec3 treeIndirectLight(vec3 worldPosition)
{
    vec3 ambient = u_ambientColor.rgb;
    if (u_giParams.x > 0.5)
    {
        if (u_giParams.z > 0.5)
        {
            vec3 n = u_normal.xyz;
            n = dot(n, n) > 0.00001 ? normalize(n) : vec3(0.0, 1.0, 0.0);
            if (dot(n, u_camPos.xyz - worldPosition) < 0.0) n = -n;
            ambient = iblDiffuse(n) * 0.96;
        }
        ambient *= max(u_giParams.y, 0.0);
    }
    return ambient;
}
)SHADER";
}
