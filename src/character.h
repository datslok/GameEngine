#pragma once

#include "character_config.h"
#include "model_instance.h"
#include "move_to_controller.h"

#include <memory>
#include <optional>

struct Model;
class Scene;

// A ground-moving character that can receive commands from a player or AI.
class Character {
public:
    Character(Scene& scene, const Model& model,
              const CharacterConfig& config, const Vec3& groundPosition);

    // Each character owns independent movement state for its scene instance.
    Character(const Character&) = delete;
    Character& operator=(const Character&) = delete;

    void moveTo(const Vec3& destination);
    void stop();
    void update(float deltaTime);

    bool isMoving() const;
    const Vec3& getPosition() const;
    Vec3 getVisualCentre() const;

    // The visual centre blended between the last two ticks, for cameras that follow the character.
    Vec3 getInterpolatedVisualCentre(float alpha) const;

private:
    std::shared_ptr<ModelInstance> modelInstance;
    MoveToController movement;
    float turnSpeed;
    float modelForwardYaw;
    float visualCentreHeight = 0.0f;
    std::optional<float> targetYaw;
};
