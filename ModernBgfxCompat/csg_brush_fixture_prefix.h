// Headless declarations for verbatim CSGBrush primitive methods.
#include "../../../Engine/glm/glm.hpp"
#include <algorithm>
#include <cmath>
constexpr double M_PI = 3.14159265358979323846;
struct CSGBrush {
    using Material = void;
    struct FaceInfo {
        std::vector<uint32_t> indices;
        std::vector<glm::vec2> texCoords;
        Material* material = nullptr;
    };
    std::vector<glm::vec3> vertices;
    std::vector<FaceInfo> faces;
    int segments = 128, stacks = 16;
    void makeCube(); void makeSphere(); void makeCone(); void makeCylinder();
};
