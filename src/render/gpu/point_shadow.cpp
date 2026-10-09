#include "render/gpu/point_shadow.h"
#include "render/gpu/gpu_depth_range.h"

#include <cmath>

namespace {
    // Soft shadows read the depth texels around a point (3x3), and filtering touches one more. Keeping every point
    // this many texels away from its face's edge means those reads never spill onto a neighbouring face.
    constexpr float edgeMarginTexels = 2.0f;

    // tan of half the field of view. A 90 degree face would have 1; the margin makes it slightly wider.
    float faceHalfWidth() {
        return 1.0f / (1.0f - 2.0f * edgeMarginTexels / static_cast<float>(pointShadowFaceSize));
    }
}

/*
* The cube face a direction pierces is the one for its largest component, so ties at edges and corners go to the earlier face.
*/
int pointShadowFace(const Vec3& fromLight) {
    const float x = std::abs(fromLight.x);
    const float y = std::abs(fromLight.y);
    const float z = std::abs(fromLight.z);

    if (x >= y && x >= z) {
        return fromLight.x > 0.0f ? 0 : 1;
    }

    if (y >= z) {
        return fromLight.y > 0.0f ? 2 : 3;
    }

    return fromLight.z > 0.0f ? 4 : 5;
}

/*
* A square perspective camera at the light. The up direction only needs to differ from the looking direction: the shader
* looks points up with these same matrices, so any consistent choice works.
*/
Mat4 pointShadowFaceMatrix(const Vec3& lightPosition, int face, float farPlane) {
    static const Vec3 forwards[6] = {
        Vec3{1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f},
        Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f},
        Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f}
    };

    static const Vec3 ups[6] = {
        Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
        Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 0.0f, 1.0f},
        Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}
    };

    const float fieldOfView = 2.0f * std::atan(faceHalfWidth());
    const Mat4 projection = toGpuDepthRange(Mat4::perspective(fieldOfView, 1.0f, pointShadowNearPlane, farPlane));
    const Mat4 view = Mat4::lookAt(lightPosition, lightPosition + forwards[face], ups[face]);

    return projection * view;
}

/*
* Shadow acne: a surface compared against its own stored depth comes out half in shadow, in stripes, because each shadow
* texel covers a patch of surface at one depth. Moving the test point off the surface along its normal by about a texel
* fixes it. A texel covers 2 * distance * halfWidth / faceSize of world, so the offset is stored per unit of distance.
*/
ShadowUniformData packPointShadow(const LightUniformData& lights) {
    ShadowUniformData data{};
    const int slot = lights.counts[3];

    if (slot < 0 || slot >= lights.counts[1]) {
        return data;
    }

    const float (&positionRange)[4] = lights.points[slot].positionRange;
    const Vec3 position{positionRange[0], positionRange[1], positionRange[2]};

    for (int face = 0; face < 6; ++face) {
        const Mat4 matrix = pointShadowFaceMatrix(position, face, positionRange[3]);

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                data.faceMatrices[face][column * 4 + row] = matrix.values[row][column];
            }
        }
    }

    const float faceSize = static_cast<float>(pointShadowFaceSize);
    data.settings[0] = 1.5f * 2.0f * faceHalfWidth() / faceSize;
    data.settings[1] = 1.0f / (faceSize * static_cast<float>(pointShadowAtlasColumns));
    data.settings[2] = 1.0f / (faceSize * static_cast<float>(pointShadowAtlasRows));

    return data;
}
