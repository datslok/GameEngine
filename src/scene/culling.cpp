#include "scene/culling.h"

#include <algorithm>

/*
* The box's centre goes through the model matrix; its half-diagonal is stretched by the largest of the matrix's three
* axis lengths, so the sphere still covers the box however the object is rotated or unevenly scaled.
*/
BoundingSphere boundingSphere(const Aabb& localBounds, const Mat4& model) {
    const Vec3 localCentre = localBounds.centre();
    const Vec4 centre = model * Vec4{localCentre.x, localCentre.y, localCentre.z, 1.0f};

    float largestScale = 0.0f;

    for (int column = 0; column < 3; ++column) {
        const Vec3 axis{model.values[0][column], model.values[1][column], model.values[2][column]};
        largestScale = std::max(largestScale, axis.length());
    }

    const float localRadius = (localBounds.size() * 0.5f).length();

    return BoundingSphere{Vec3{centre.x, centre.y, centre.z}, localRadius * largestScale};
}

void attachBoundingSpheres(std::span<DrawItem> draws, const std::vector<Aabb>& boundsByMesh) {
    for (DrawItem& draw : draws) {
        if (draw.mesh.isValid() && draw.mesh.index < boundsByMesh.size()) {
            draw.bounds = boundingSphere(boundsByMesh[draw.mesh.index], draw.model);
        }
    }
}

void attachFlatFaces(std::span<DrawItem> draws, const std::vector<std::vector<FlatFace>>& facesByMesh) {
    for (DrawItem& draw : draws) {
        if (draw.mesh.isValid() && draw.mesh.index < facesByMesh.size()) {
            draw.flatFaces = facesByMesh[draw.mesh.index];
        }
    }
}

void attachVertexEntries(std::span<DrawItem> draws, const std::vector<VertexEntries>& entriesByMesh) {
    for (DrawItem& draw : draws) {
        const VertexEntries entries = draw.mesh.isValid() && draw.mesh.index < entriesByMesh.size() ? entriesByMesh[draw.mesh.index] : VertexEntries{};
        draw.hangingEntry = entries.hanging;
        draw.thinningEntry = entries.thinning;
    }
}

namespace {
    // Unknown sizes are always kept: drawing something unneeded is cheap, missing something is a bug.
    bool mightBeSeen(const DrawItem& draw, const Frustum& frustum) {
        return draw.bounds.radius < 0.0f || frustum.intersectsSphere(draw.bounds.centre, draw.bounds.radius);
    }

    // The camera's views take neither shadow stand-ins nor hidden draws.
    bool cameraMightSee(const DrawItem& draw, const Frustum& frustum) {
        return !draw.shadowOnly && !draw.hidden && mightBeSeen(draw, frustum);
    }
}

std::vector<std::size_t> visibleDraws(const std::vector<DrawItem>& draws, const Frustum& cameraFrustum) {
    std::vector<std::size_t> visible;

    for (std::size_t i = 0; i < draws.size(); ++i) {
        if (cameraMightSee(draws[i], cameraFrustum)) {
            visible.push_back(i);
        }
    }

    return visible;
}

std::vector<std::size_t> visibleDraws(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& candidates, const Frustum& cameraFrustum) {
    std::vector<std::size_t> visible;

    for (const std::size_t i : candidates) {
        if (cameraMightSee(draws[i], cameraFrustum)) {
            visible.push_back(i);
        }
    }

    return visible;
}

// Every caster is a candidate: a reach without a radius limits nothing.
std::vector<std::size_t> shadowCasters(const std::vector<DrawItem>& draws, const Frustum& lightFrustum) {
    return shadowCasters(draws, castersInReach(draws, BoundingSphere{}), lightFrustum);
}
/*
* Two spheres touch when their centres are closer than their radii added up; squared, so there is no square root.
* Unknown sizes are kept, as everywhere in culling.
*/
namespace {
    // Whether a draw of one kind is within the reach: a solid shadow caster, or glass (all of it, as the glass rays see it).
    bool shadowDrawInReach(const DrawItem& draw, const BoundingSphere& reach, bool glass) {
        if (isTransparent(draw.material) != glass || (!glass && !draw.castsShadows) || draw.hidden) {
            return false;
        }

        if (reach.radius >= 0.0f && draw.bounds.radius >= 0.0f) {
            const float touching = reach.radius + draw.bounds.radius;
            return (draw.bounds.centre - reach.centre).lengthSquared() < touching * touching;
        }

        return true;
    }

    std::vector<std::size_t> shadowDrawsInReach(const std::vector<DrawItem>& draws, const BoundingSphere& reach, bool glass) {
        std::vector<std::size_t> casters;

        for (std::size_t i = 0; i < draws.size(); ++i) {
            if (shadowDrawInReach(draws[i], reach, glass)) {
                casters.push_back(i);
            }
        }

        return casters;
    }
}

// Glass and blended surfaces block no light in the atlas: they tint it, through the coloured shadow maps (glassInReach).
std::vector<std::size_t> castersForCascade(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters, int cascade) {
    std::vector<std::size_t> kept;
    kept.reserve(casters.size());

    for (std::size_t index : casters) {
        const DrawItem& draw = draws[index];
        const bool inCascades = cascade < 0 ? !draw.shadowHandedOn
                                            : (draw.shadowCascades <= 0 || cascade < draw.shadowCascades) && cascade >= draw.shadowFromCascade;

        if (inCascades) {
            kept.push_back(index);
        }
    }

    return kept;
}

std::vector<std::size_t> castersInReach(const std::vector<DrawItem>& draws, const BoundingSphere& reach) {
    return shadowDrawsInReach(draws, reach, false);
}

std::vector<std::size_t> glassInReach(const std::vector<DrawItem>& draws, const BoundingSphere& reach) {
    return shadowDrawsInReach(draws, reach, true);
}

std::vector<std::size_t> glassInReach(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& candidates, const BoundingSphere& reach) {
    std::vector<std::size_t> glass;

    for (std::size_t index : candidates) {
        if (shadowDrawInReach(draws[index], reach, true)) {
            glass.push_back(index);
        }
    }

    return glass;
}

std::vector<std::size_t> shadowCasters(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& candidates,
                                       const Frustum& lightFrustum) {
    std::vector<std::size_t> casters;

    for (std::size_t index : candidates) {
        if (mightBeSeen(draws[index], lightFrustum)) {
            casters.push_back(index);
        }
    }

    return casters;
}

std::vector<std::size_t> castersAtLeast(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& candidates, float smallestRadius) {
    std::vector<std::size_t> kept;

    for (const std::size_t index : candidates) {
        const float radius = draws[index].bounds.radius;

        if (radius < 0.0f || radius >= smallestRadius) {
            kept.push_back(index);
        }
    }

    return kept;
}
