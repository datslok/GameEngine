#include "render/gpu/transparency.h"
#include "render/gpu/brdf.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

TransparentLight transparentBlend(float alpha, float transmission, float metallic, const Vec3& diffuseLight, const Vec3& specularLight,
                                  const Vec3& emissive, const Vec3& baseColour, const Vec3& specularAlbedo) {
    const float passing = transmission * (1.0f - metallic);
    const Vec3 surface = diffuseLight * (1.0f - passing) + specularLight + emissive;
    return TransparentLight{surface * alpha, transparentTransmittance(alpha, transmission, metallic, baseColour, specularAlbedo)};
}

Vec3 transparentTransmittance(float alpha, float transmission, float metallic, const Vec3& baseColour, const Vec3& specularAlbedo) {
    const float passing = transmission * (1.0f - metallic);
    const Vec3 through = baseColour * (Vec3{1.0f, 1.0f, 1.0f} - specularAlbedo) * passing;
    return through * alpha + Vec3{1.0f, 1.0f, 1.0f} * (1.0f - alpha);
}

// The reflectance head-on is glass's 4% (metals' own colour), as the shading's prepareShading takes it.
Vec3 glassLightTransmittance(float alpha, float transmission, float metallic, const Vec3& baseColour, float cosine) {
    const Vec3 dielectric{BRDF_DIELECTRIC_REFLECTANCE, BRDF_DIELECTRIC_REFLECTANCE, BRDF_DIELECTRIC_REFLECTANCE};
    const Vec3 headOn = dielectric + (baseColour - dielectric) * metallic;
    return transparentTransmittance(alpha, transmission, metallic, baseColour, schlickFresnel(headOn, std::abs(cosine)));
}

bool glassFaceCounts(bool rayMeetsFront, bool doubleSided) {
    return !rayMeetsFront || doubleSided;
}

// The segment's nearest point to the centre: the centre's place along it, held within its ends.
bool segmentMeetsSphere(const Vec3& from, const Vec3& direction, float length, const Vec3& centre, float radius) {
    const float along = std::clamp((centre - from).dot(direction), 0.0f, length);
    return (from + direction * along - centre).lengthSquared() <= radius * radius;
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

void visit(TransparencySettings& settings, SettingsVisitor& visitor) {
    visitor.field("Glass shadows", settings.glassShadows);
    visitor.field("Reactive mask", settings.reactiveMask);
    visitor.field("Reactive scale", settings.reactiveScale, {.min = 0.0f, .max = 10.0f, .sliderMax = 3.0f});
    visitor.field("Reactive limit", settings.reactiveLimit, {.min = 0.0f, .max = 1.0f});
}

// The comparisons are written so not a number fails them too.
void validateTransparency(const TransparencySettings& settings) {
    if (!(settings.reactiveScale >= 0.0f)) {
        throw std::invalid_argument("The reactive mask's scale must not be negative");
    }

    if (!(settings.reactiveLimit >= 0.0f && settings.reactiveLimit <= 1.0f)) {
        throw std::invalid_argument("The reactive mask's limit must be within 0..1");
    }
}

/*
* Glass over a wall either takes light away (tint, the reflection's share of what passes) or adds it (its reflection,
* its glow); either way the share of the brighter picture that changed is how much of the pixel is the glass's own, so
* how little the history (which moved as the wall did) still fits.
*/
float reactiveShare(const Vec3& opaque, const Vec3& blended, float scale, float limit) {
    const auto channelShare = [](float before, float after) {
        before = std::max(before, 0.0f);
        after = std::max(after, 0.0f);
        return std::abs(after - before) / std::max(std::max(before, after), static_cast<float>(TRANSPARENCY_REACTIVE_FLOOR));
    };

    const float changed = std::max(channelShare(opaque.x, blended.x), std::max(channelShare(opaque.y, blended.y), channelShare(opaque.z, blended.z)));
    return std::clamp(changed * scale, 0.0f, limit);
}

GpuGlassBounds packGlassBounds(const std::vector<DrawItem>& draws, bool enabled) {
    GpuGlassBounds bounds{};

    if (!enabled) {
        return bounds;
    }

    for (const DrawItem& draw : draws) {
        if (!isTransparent(draw.material)) {
            continue;
        }

        if (draw.bounds.radius < 0.0f || bounds.counts[0] == glassBoundsLimit) {
            bounds.counts[1] = 1;
        }

        if (draw.bounds.radius >= 0.0f && bounds.counts[0] < glassBoundsLimit) {
            float* sphere = bounds.spheres[bounds.counts[0]++];
            sphere[0] = draw.bounds.centre.x;
            sphere[1] = draw.bounds.centre.y;
            sphere[2] = draw.bounds.centre.z;
            sphere[3] = draw.bounds.radius;
        }
    }

    return bounds;
}

bool glassMayLieBetween(const GpuGlassBounds& bounds, const Vec3& from, const Vec3& direction, float length) {
    if (bounds.counts[1] != 0) {
        return true;
    }

    for (std::uint32_t i = 0; i < bounds.counts[0]; ++i) {
        const float* sphere = bounds.spheres[i];

        if (segmentMeetsSphere(from, direction, length, Vec3{sphere[0], sphere[1], sphere[2]}, sphere[3])) {
            return true;
        }
    }

    return false;
}

GpuReactivePass packReactivePass(RenderSize render, const TransparencySettings& settings) {
    validateTransparency(settings);
    return GpuReactivePass{{static_cast<float>(render.width), static_cast<float>(render.height), 0.0f, 0.0f},
                           {settings.reactiveScale, settings.reactiveLimit, 0.0f, 0.0f}};
}
