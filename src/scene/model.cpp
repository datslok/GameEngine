#include "scene/model.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

Vec3 ModelBounds::centre() const {
    return Vec3{
        std::midpoint(minimum.x, maximum.x),
        std::midpoint(minimum.y, maximum.y),
        std::midpoint(minimum.z, maximum.z)
    };
}

Vec3 ModelBounds::size() const {
    return maximum - minimum;
}

ModelBounds Model::getBounds() const {
    ModelBounds bounds{};
    bool foundVertex = false;

    for (const ModelPart& part : parts) {
        if (!part.mesh) {
            throw std::invalid_argument(
                "Cannot calculate bounds for a null mesh"
            );
        }

        for (const Vec4& vertex : part.mesh->vertices) {
            // Include the imported node and parent transforms.
            const Vec4 point = part.transform * vertex;

            if (!std::isfinite(point.x) ||
                !std::isfinite(point.y) ||
                !std::isfinite(point.z)) {
                throw std::runtime_error(
                    "Model bounds require finite positions"
                );
            }

            const Vec3 position{point.x, point.y, point.z};

            if (!foundVertex) {
                bounds.minimum = position;
                bounds.maximum = position;
                foundVertex = true;
                continue;
            }

            bounds.minimum.x = std::min(bounds.minimum.x, position.x);
            bounds.minimum.y = std::min(bounds.minimum.y, position.y);
            bounds.minimum.z = std::min(bounds.minimum.z, position.z);

            bounds.maximum.x = std::max(bounds.maximum.x, position.x);
            bounds.maximum.y = std::max(bounds.maximum.y, position.y);
            bounds.maximum.z = std::max(bounds.maximum.z, position.z);
        }
    }

    if (!foundVertex) {
        throw std::invalid_argument(
            "Cannot calculate bounds for a model without vertices"
        );
    }

    return bounds;
}

Mat4 Model::getNormalizationMatrix(float targetSize) const {
    if (!std::isfinite(targetSize) || targetSize <= 0.0f) {
        throw std::invalid_argument(
            "Model target size must be finite and positive"
        );
    }

    const ModelBounds bounds = getBounds();
    const Vec3 dimensions = bounds.size();

    if (!std::isfinite(dimensions.x) ||
        !std::isfinite(dimensions.y) ||
        !std::isfinite(dimensions.z)) {
        throw std::runtime_error(
            "Model dimensions are too large"
        );
    }

    const float longestSide = std::max({
        dimensions.x,
        dimensions.y,
        dimensions.z
    });

    if (longestSide <= 0.0f) {
        throw std::invalid_argument(
            "Cannot normalize a model with zero size"
        );
    }

    const float scale = targetSize / longestSide;

    if (!std::isfinite(scale) || scale <= 0.0f) {
        throw std::runtime_error(
            "Model normalization scale is out of range"
        );
    }

    const Vec3 centre = bounds.centre();

    // Move the centre to the origin, then apply uniform scaling.
    return
        Mat4::scaling(scale, scale, scale) *
        Mat4::translation(-centre.x, -centre.y, -centre.z);
}
/*
* Every vertex counts, used by a triangle or not, which matches what the renderer uploads.
*/
ModelBounds meshBounds(const Mesh& mesh) {
    if (mesh.vertices.empty()) {
        return ModelBounds{};
    }

    const Vec4& first = mesh.vertices.front();
    ModelBounds bounds{Vec3{first.x, first.y, first.z}, Vec3{first.x, first.y, first.z}};

    for (const Vec4& vertex : mesh.vertices) {
        bounds.minimum = Vec3{std::min(bounds.minimum.x, vertex.x), std::min(bounds.minimum.y, vertex.y), std::min(bounds.minimum.z, vertex.z)};
        bounds.maximum = Vec3{std::max(bounds.maximum.x, vertex.x), std::max(bounds.maximum.y, vertex.y), std::max(bounds.maximum.z, vertex.z)};
    }

    return bounds;
}