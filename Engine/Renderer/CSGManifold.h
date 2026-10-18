#pragma once

// CPU-only adapter shared by the renderer and the regression corpus.
#include <manifold/manifold.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace GX { namespace CSGManifold {
using Position = std::array<double, 3>;
using UV = std::array<double, 2>;
struct Attributes {
    uintptr_t material = 0;
    uint64_t brush = 0;
    int layer = 0;
    bool castShadows = true;
    bool smoothNormals = false;
};
struct Face {
    std::vector<uint32_t> indices;
    std::vector<UV> uv;
    Attributes attributes;
};
struct Brush {
    std::vector<Position> positions;
    std::vector<Face> faces;
    bool subtract = false;
    bool reverseWinding = false; // Negative world-transform determinant.
};
struct Triangle {
    std::array<Position, 3> positions;
    std::array<UV, 3> uv;
    Attributes attributes;
};

class Builder {
    manifold::Manifold solid;
    // Discrete values never enter interpolated vertex properties.
    std::map<uint32_t, std::vector<Attributes>> originals;
public:
    const manifold::Manifold& geometry() const { return solid; }

    bool apply(const Brush& brush, std::string& error) {
        error.clear();
        if (brush.positions.empty() || brush.faces.empty()) {
            error = "empty brush"; return false;
        }
        for (const auto& p : brush.positions)
            for (double v : p) if (!std::isfinite(v)) {
                error = "non-finite position"; return false;
            }
        manifold::MeshGL64 mesh;
        mesh.numProp = 5; // XYZ and per-corner UV only.
        const uint32_t original = manifold::Manifold::ReserveIDs(1);
        mesh.runOriginalID = {original};
        mesh.runIndex = {0};
        std::vector<uint64_t> first(brush.positions.size(), UINT64_MAX);
        std::vector<Attributes> attributes;
        for (size_t f = 0; f < brush.faces.size(); ++f) {
            const auto& face = brush.faces[f];
            const size_t n = face.indices.size();
            // Falco's existing brush format emits triangles and quads.
            if ((n != 3 && n != 4) || face.uv.size() != n) {
                error = "unsupported face or missing corner UV"; return false;
            }
            const uint64_t base = mesh.NumVert();
            for (size_t c = 0; c < n; ++c) {
                const auto index = face.indices[c];
                if (index >= brush.positions.size()) {
                    error = "vertex index out of bounds"; return false;
                }
                const auto& p = brush.positions[index];
                const auto& uv = face.uv[c];
                if (!std::isfinite(uv[0]) || !std::isfinite(uv[1])) {
                    error = "non-finite UV"; return false;
                }
                if (first[index] != UINT64_MAX) {
                    mesh.mergeFromVert.push_back(base + c);
                    mesh.mergeToVert.push_back(first[index]);
                } else first[index] = base + c;
                mesh.vertProperties.insert(mesh.vertProperties.end(), {p[0], p[1], p[2], uv[0], uv[1]});
            }
            for (size_t c = 1; c + 1 < n; ++c) {
                mesh.triVerts.insert(mesh.triVerts.end(), {base, base + (brush.reverseWinding ? c + 1 : c), base + (brush.reverseWinding ? c : c + 1)});
                mesh.faceID.push_back(f);
            }
            attributes.push_back(face.attributes);
        }
        mesh.runIndex.push_back(mesh.triVerts.size());
        manifold::Manifold operand(mesh);
        if (operand.Status() != manifold::Manifold::Error::NoError) {
            error = "Manifold input status " + std::to_string(static_cast<int>(operand.Status())); return false;
        }
        if (operand.IsEmpty() || !std::isfinite(operand.Volume()) || operand.Volume() <= 0) {
            error = "zero-volume or inverted brush"; return false;
        }
        auto next = solid.Boolean(operand, brush.subtract ? manifold::OpType::Subtract : manifold::OpType::Add);
        if (next.Status() != manifold::Manifold::Error::NoError) {
            error = "Manifold boolean status " + std::to_string(static_cast<int>(next.Status())); return false;
        }
        // Commit only a successful operation; empty subtraction is valid.
        originals.emplace(original, std::move(attributes));
        solid = std::move(next);
        return true;
    }

    bool triangles(std::vector<Triangle>& result, std::string& error) const {
        result.clear(); error.clear();
        auto mesh = solid.GetMeshGL64();
        for (size_t r = 0; r < mesh.runOriginalID.size(); ++r) {
            const auto source = originals.find(mesh.runOriginalID[r]);
            if (source == originals.end()) { error = "missing brush provenance"; return false; }
            for (uint64_t i = mesh.runIndex[r]; i < mesh.runIndex[r+1]; i += 3) {
                const auto face = mesh.faceID.at(i / 3);
                if (face >= source->second.size()) { error = "missing face provenance"; return false; }
                Triangle t;
                t.attributes = source->second[face];
                for (size_t c = 0; c < 3; ++c) {
                    const auto v = mesh.triVerts[i+c] * mesh.numProp;
                    t.positions[c] = {mesh.vertProperties[v], mesh.vertProperties[v+1], mesh.vertProperties[v+2]};
                    t.uv[c] = {mesh.vertProperties[v+3], mesh.vertProperties[v+4]};
                }
                // Canonical cyclic corner order preserves winding.
                size_t first = 0;
                for (size_t c = 1; c < 3; ++c)
                    if (t.positions[c] < t.positions[first] ||
                        (t.positions[c] == t.positions[first] && t.uv[c] < t.uv[first])) first = c;
                std::rotate(t.positions.begin(), t.positions.begin()+first, t.positions.end());
                std::rotate(t.uv.begin(), t.uv.begin()+first, t.uv.end());
                result.push_back(t);
            }
        }
        // Geometry order is independent of Manifold's process-global original IDs.
        std::stable_sort(result.begin(), result.end(), [](const Triangle& a, const Triangle& b) {
            if (a.positions != b.positions) return a.positions < b.positions;
            if (a.uv != b.uv) return a.uv < b.uv;
            return a.attributes.brush < b.attributes.brush;
        });
        return true;
    }
};
}}
