#pragma once

#include "math/vec3.h"

#include <optional>

class MoveToController {
public:
    explicit MoveToController(float speed);

    void setTarget(const Vec3& target);
    void stop();

    bool isMoving() const;

    void update(Vec3& position, float deltaTime);

private:
    float speed;
    std::optional<Vec3> target;
};