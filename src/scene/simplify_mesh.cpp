#include "scene/simplify_mesh.h"

#include "meshoptimizer/meshoptimizer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

IndexedMesh simplifyMesh(const IndexedMesh& mesh, float keepShare, float maxError, bool lockBorder, float* errorMetres) {
    if (errorMetres != nullptr) {
        *errorMetres = 0.0f;
    }

    if (mesh.indices.empty() || keepShare >= 1.0f) {
        return mesh;
    }

    // Positions are the first three floats of each MeshVertex, so the simplifier reads them in place.
    const std::size_t triangles = mesh.indices.size() / 3;
    const std::size_t target = std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<float>(triangles) * std::max(keepShare, 0.0f))) * 3;
    std::vector<std::uint32_t> kept(mesh.indices.size());
    float resultError = 0.0f;
    const std::size_t count = meshopt_simplify(kept.data(), mesh.indices.data(), mesh.indices.size(), &mesh.vertices[0].position.x,
                                               mesh.vertices.size(), sizeof(MeshVertex), target, maxError,
                                               lockBorder ? static_cast<unsigned int>(meshopt_SimplifyLockBorder) : 0u, &resultError);
    kept.resize(count);

    if (errorMetres != nullptr) {
        *errorMetres = resultError * meshopt_simplifyScale(&mesh.vertices[0].position.x, mesh.vertices.size(), sizeof(MeshVertex));
    }

    // Only the vertices still used, in the order the triangles first use them.
    IndexedMesh simpler;
    std::vector<std::uint32_t> remap(mesh.vertices.size(), UINT32_MAX);
    simpler.indices.reserve(kept.size());

    for (std::uint32_t index : kept) {
        if (remap[index] == UINT32_MAX) {
            remap[index] = static_cast<std::uint32_t>(simpler.vertices.size());
            simpler.vertices.push_back(mesh.vertices[index]);

            if (!mesh.swayBindings.empty()) {
                simpler.swayBindings.push_back(mesh.swayBindings[index]);
            }
        }

        simpler.indices.push_back(remap[index]);
    }

    return simpler;
}

std::vector<SimplifiedLevel> simplifiedLevels(const IndexedMesh& mesh, std::initializer_list<float> keepShares, float maxError) {
    std::vector<SimplifiedLevel> levels;
    float lastError = 0.0f;

    for (const float share : keepShares) {
        SimplifiedLevel level;
        level.mesh = simplifyMesh(mesh, share, maxError, false, &level.errorMetres);
        level.errorMetres = std::max(level.errorMetres, lastError);
        lastError = level.errorMetres;
        levels.push_back(std::move(level));
    }

    return levels;
}
