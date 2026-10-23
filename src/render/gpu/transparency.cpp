#include "render/gpu/transparency.h"

#include <algorithm>
#include <utility>

TransparentLight transparentBlend(float alpha, float transmission, float metallic, const Vec3& diffuseLight, const Vec3& specularLight,
                                  const Vec3& emissive, const Vec3& baseColour, const Vec3& specularAlbedo) {
    const float passing = transmission * (1.0f - metallic);
    const Vec3 surface = diffuseLight * (1.0f - passing) + specularLight + emissive;
    const Vec3 through = baseColour * (Vec3{1.0f, 1.0f, 1.0f} - specularAlbedo) * passing;
    return TransparentLight{surface * alpha, through * alpha + Vec3{1.0f, 1.0f, 1.0f} * (1.0f - alpha)};
}

namespace {
    std::vector<std::size_t> chosenWhere(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, bool transparent) {
        std::vector<std::size_t> kept;

        for (std::size_t index : chosen) {
            if (isTransparent(draws[index].material) == transparent) {
                kept.push_back(index);
            }
        }

        return kept;
    }

    Vec3 placeOf(const DrawItem& draw) {
        if (draw.bounds.radius >= 0.0f) {
            return draw.bounds.centre;
        }

        return Vec3{draw.model.values[0][3], draw.model.values[1][3], draw.model.values[2][3]};
    }
}

std::vector<std::size_t> opaqueDraws(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen) {
    return chosenWhere(draws, chosen, false);
}

std::vector<std::size_t> transparentDraws(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen) {
    return chosenWhere(draws, chosen, true);
}

SortedTransparentDraws sortTransparentDraws(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& chosen, const Vec3& camera,
                                            std::uint32_t firstInstance) {
    std::vector<std::pair<float, std::size_t>> byDistance;
    byDistance.reserve(chosen.size());

    for (std::size_t index : chosen) {
        byDistance.emplace_back((placeOf(draws[index]) - camera).lengthSquared(), index);
    }

    std::stable_sort(byDistance.begin(), byDistance.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    SortedTransparentDraws sorted;

    for (const auto& [distance, index] : byDistance) {
        const DrawItem& draw = draws[index];
        const std::uint32_t entry = firstInstance + static_cast<std::uint32_t>(sorted.order.size());
        sorted.order.push_back(index);

        if (draw.material.doubleSided) {
            sorted.batches.push_back(TransparentBatch{draw.mesh, entry, true});
        }

        sorted.batches.push_back(TransparentBatch{draw.mesh, entry, false});
    }

    return sorted;
}

GpuTransparentView packTransparentView(bool readsProbe, std::uint32_t probeLevels) {
    return GpuTransparentView{{readsProbe ? 1.0f : 0.0f, static_cast<float>(probeLevels), 0.0f, 0.0f}};
}
