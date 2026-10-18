#pragma once

#include "math/vec3.h"

#include <optional>

class MoveToController {
public:
    explicit MoveToController(float speed);

    void setTarget(const Vec3& target);
    void stop();

    bool isMoving() const;

    // Where it is heading, if anywhere.
    const std::optional<Vec3>& getTarget() const { return target; }

    // The horizontal velocity that heads straight for the target at full speed, slower on the last step so the character
    // lands on it exactly (the remaining distance over deltaTime). Zero with no target, at the target or with no time.
    Vec3 desiredVelocity(const Vec3& position, float deltaTime) const;

    // Stops once the character is within arrivalDistance of the target across the ground. Something that blocks the way
    // (a wall) means it never arrives and keeps pushing, until a new command or stop(): there is no path finding yet.
    void updateArrival(const Vec3& position);

    static constexpr float arrivalDistance = 1e-4f;

private:
    float speed;
    std::optional<Vec3> target;
};