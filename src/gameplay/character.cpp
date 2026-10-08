#include "gameplay/character.h"
#include "scene/model.h"
#include "scene/scene.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

Character::Character(Scene& scene, const Model& model,
                     const CharacterConfig& config, const Vec3& groundPosition)
    : movement(config.movementSpeed),
      turnSpeed(config.turnSpeed),
      modelForwardYaw(config.modelForwardYaw) {
    if (!std::isfinite(turnSpeed) || turnSpeed <= 0.0f) {
        throw std::invalid_argument("Character turn speed must be finite and positive");
    }
    if (!std::isfinite(modelForwardYaw)) {
        throw std::invalid_argument("Character forward yaw must be finite");
    }
    if (!std::isfinite(groundPosition.x) ||
        !std::isfinite(groundPosition.y) ||
        !std::isfinite(groundPosition.z)) {
        throw std::invalid_argument("Character position must be finite");
    }

    const Mat4 normalization = model.getNormalizationMatrix(config.modelSize);
    const ModelBounds bounds = model.getBounds();
    const Vec4 bottom = normalization * Vec4{
        bounds.minimum.x, bounds.minimum.y, bounds.minimum.z, 1.0f
    };
    visualCentreHeight = -bottom.y;

    // Keep the gameplay origin on the ground. Lift only the visual model.
    const Mat4 groundedNormalization =
        Mat4::translation(0.0f, visualCentreHeight, 0.0f) * normalization;

    Transform placement;
    placement.position = groundPosition;
    modelInstance = scene.addModel(model, placement, groundedNormalization);
}

void Character::moveTo(const Vec3& destination) {
    movement.setTarget(destination);
}

void Character::stop() {
    movement.stop();
    targetYaw.reset();
}

bool Character::isMoving() const {
    return movement.isMoving();
}

const Vec3& Character::getPosition() const {
    return modelInstance->transform.position;
}

Vec3 Character::getVisualCentre() const {
    return getPosition() + Vec3{0.0f, visualCentreHeight, 0.0f};
}

Vec3 Character::getInterpolatedVisualCentre(float alpha) const {
    const Transform rendered = interpolate(
        modelInstance->previousTransform, modelInstance->transform, alpha
    );

    return rendered.position + Vec3{0.0f, visualCentreHeight, 0.0f};
}

void Character::update(float deltaTime) {
    const Vec3 previousPosition = getPosition();
    movement.update(modelInstance->transform.position, deltaTime);

    const Vec3 displacement = getPosition() - previousPosition;
    if (displacement.x != 0.0f || displacement.z != 0.0f) {
        const float heading = std::atan2(displacement.x, displacement.z);
        targetYaw = heading - modelForwardYaw;
    }

    // Finish facing the last movement direction even after arrival.
    if (!targetYaw) {
        return;
    }

    constexpr float fullTurn = 2.0f * std::numbers::pi_v<float>;
    float& currentYaw = modelInstance->transform.rotation.y;
    const float difference = std::remainder(*targetYaw - currentYaw, fullTurn);
    const float maximumTurn = turnSpeed * deltaTime;

    if (std::abs(difference) <= maximumTurn) {
        currentYaw = std::remainder(*targetYaw, fullTurn);
        targetYaw.reset();
    } else {
        currentYaw = std::remainder(
            currentYaw + std::clamp(difference, -maximumTurn, maximumTurn),
            fullTurn
        );
    }
}
