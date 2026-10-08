#include "gameplay/move_to_controller.h"

#include <cmath>
#include <stdexcept>

MoveToController::MoveToController(float speed)
    : speed(speed) {
    if (!std::isfinite(speed) || speed <= 0.0f) {
        throw std::invalid_argument(
            "Movement speed must be finite and positive"
        );
    }
}

void MoveToController::setTarget(const Vec3& destination) {
    if (!std::isfinite(destination.x) ||
        !std::isfinite(destination.y) ||
        !std::isfinite(destination.z)) {
        throw std::invalid_argument(
            "Movement target must be finite"
        );
    }

    target = destination;
}

void MoveToController::stop() {
    target.reset();
}

bool MoveToController::isMoving() const {
    return target.has_value();
}

void MoveToController::update(Vec3& position, float deltaTime) {
    if (!std::isfinite(deltaTime) || deltaTime < 0.0f) {
        throw std::invalid_argument(
            "Movement delta time must be finite and nonnegative"
        );
    }

    if (!target) {
        return;
    }

    // Move only across the ground; preserve the model's height.
    const Vec3 displacement{
        target->x - position.x,
        0.0f,
        target->z - position.z
    };

    const float distance = displacement.length();
    const float step = speed * deltaTime;

    if (distance <= step) {
        position.x = target->x;
        position.z = target->z;

        stop();
        return;
    }

    position = position + displacement * (step / distance);
}