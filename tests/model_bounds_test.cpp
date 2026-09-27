#include "model.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }

    void assertVector(const Vec3& actual, const Vec3& expected) {
        assert(nearlyEqual(actual.x, expected.x));
        assert(nearlyEqual(actual.y, expected.y));
        assert(nearlyEqual(actual.z, expected.z));
    }
}

void testModelBounds() {
    auto mesh = std::make_shared<Mesh>();

    mesh->vertices = {
        Vec4{-1.0f, -1.0f, -1.0f, 1.0f},
        Vec4{ 1.0f,  1.0f,  1.0f, 1.0f}
    };

    ModelPart first;
    first.mesh = mesh;
    first.transform =
        Mat4::translation(2.0f, 0.0f, 0.0f) *
        Mat4::scaling(2.0f, 1.0f, 1.0f);

    ModelPart second;
    second.mesh = mesh;
    second.transform = Mat4::translation(-4.0f, 2.0f, 0.0f);

    Model model;
    model.parts = {first, second};

    // Bounds must include both instances and their transforms.
    const ModelBounds bounds = model.getBounds();

    assertVector(bounds.minimum, Vec3{-5.0f, -1.0f, -1.0f});
    assertVector(bounds.maximum, Vec3{ 4.0f,  3.0f,  1.0f});
    assertVector(bounds.centre(), Vec3{-0.5f, 1.0f, 0.0f});
    assertVector(bounds.size(), Vec3{9.0f, 4.0f, 2.0f});

    const Mat4 normalization = model.getNormalizationMatrix(2.0f);

    Model normalized = model;

    for (ModelPart& part : normalized.parts) {
        part.transform = normalization * part.transform;
    }

    const ModelBounds normalizedBounds = normalized.getBounds();

    assertVector(
        normalizedBounds.centre(),
        Vec3{0.0f, 0.0f, 0.0f}
    );

    // Uniform scaling preserves the original proportions.
    assertVector(
        normalizedBounds.size(),
        Vec3{2.0f, 8.0f / 9.0f, 4.0f / 9.0f}
    );

    // Flat geometry must also normalize successfully.
    auto flatMesh = std::make_shared<Mesh>();
    flatMesh->vertices = {
        Vec4{-2.0f, -1.0f, 0.0f, 1.0f},
        Vec4{ 2.0f,  1.0f, 0.0f, 1.0f}
    };

    ModelPart flatPart;
    flatPart.mesh = flatMesh;

    Model flatModel;
    flatModel.parts.push_back(flatPart);

    const Vec4 corner =
        flatModel.getNormalizationMatrix(2.0f) *
        flatMesh->vertices[1];

    assert(nearlyEqual(corner.x, 1.0f));
    assert(nearlyEqual(corner.y, 0.5f));
    assert(nearlyEqual(corner.z, 0.0f));

    bool rejectedEmpty = false;

    try {
        Model{}.getBounds();
    } catch (const std::invalid_argument&) {
        rejectedEmpty = true;
    }

    assert(rejectedEmpty);

    bool rejectedZeroTarget = false;

    try {
        model.getNormalizationMatrix(0.0f);
    } catch (const std::invalid_argument&) {
        rejectedZeroTarget = true;
    }

    assert(rejectedZeroTarget);
}