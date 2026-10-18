#ifndef FALCO_CAMERA_SPACE_SH
#define FALCO_CAMERA_SPACE_SH
// RH camera convention: the inverse-view +Z axis points toward the viewer.
// Explicit arguments keep this helper usable in both vertex and fragment stages.
vec3 falcoSurfaceToCamera(vec3 position, vec3 cameraPosition, mat4 projection, mat4 inverseView)
{
    vec3 direction = abs(projection[3][3]) > 0.5
        ? mul(inverseView, vec4(0.0, 0.0, 1.0, 0.0)).xyz
        : cameraPosition - position;
    float lengthSquared = dot(direction, direction);
    return lengthSquared > 0.000001 ? direction / sqrt(lengthSquared) : vec3(0.0, 0.0, 1.0);
}
#endif
