#include "gameplay/move_to_controller.h"

#include <algorithm>
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

/*
* Across the ground only: the character controller deals with height (stairs, ramps), so the target's height never
* pulls the character up or down. On the last step the speed is cut to what reaches the target exactly.
*/
Vec3 MoveToController::desiredVelocity(const Vec3& position, float deltaTime) const {
    if (!target || !std::isfinite(deltaTime) || deltaTime <= 0.0f) {
        return Vec3{};
    }

    const Vec3 across{target->x - position.x, 0.0f, target->z - position.z};
    const float distance = across.length();
    if (distance <= arrivalDistance) {
        return Vec3{};
    }

    return across * (std::min(speed, distance / deltaTime) / distance);
}

void MoveToController::updateArrival(const Vec3& position) {
    if (!target) {
        return;
    }

    const Vec3 across{target->x - position.x, 0.0f, target->z - position.z};
    if (across.length() <= arrivalDistance) {
        stop();
    }
}
