#include "scene/culling.h"

#include <algorithm>

/*
* The box's centre goes through the model matrix; its half-diagonal is stretched by the largest of the matrix's three
* axis lengths, so the sphere still covers the box however the object is rotated or unevenly scaled.
*/
BoundingSphere boundingSphere(const ModelBounds& localBounds, const Mat4& model) {
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

void attachBoundingSpheres(std::vector<DrawItem>& draws, const std::vector<ModelBounds>& boundsByMesh) {
    for (DrawItem& draw : draws) {
        if (draw.mesh.isValid() && draw.mesh.index < boundsByMesh.size()) {
            draw.bounds = boundingSphere(boundsByMesh[draw.mesh.index], draw.model);
        }
    }
}

namespace {
    // Unknown sizes are always kept: drawing something unneeded is cheap, missing something is a bug.
    bool mightBeSeen(const DrawItem& draw, const Frustum& frustum) {
        return draw.bounds.radius < 0.0f || frustum.intersectsSphere(draw.bounds.centre, draw.bounds.radius);
    }
}

std::vector<std::size_t> visibleDraws(const std::vector<DrawItem>& draws, const Frustum& cameraFrustum) {
    std::vector<std::size_t> visible;

    for (std::size_t i = 0; i < draws.size(); ++i) {
        if (mightBeSeen(draws[i], cameraFrustum)) {
            visible.push_back(i);
        }
    }

    return visible;
}

std::vector<std::size_t> shadowCasters(const std::vector<DrawItem>& draws, const Frustum& lightFrustum) {
    std::vector<std::size_t> casters;

    for (std::size_t i = 0; i < draws.size(); ++i) {
        if (draws[i].castsShadows && mightBeSeen(draws[i], lightFrustum)) {
            casters.push_back(i);
        }
    }

    return casters;
}