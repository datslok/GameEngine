#pragma once

#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "gameplay/character_config.h"
#include "gameplay/move_to_controller.h"
#include "math/vec3.h"

#include <optional>

/*
* Component: a ground-moving character that receives move commands from a player or AI and turns towards where it is heading.
* The small helpers only touch this component's own data. Moving the entity's Transform happens in updateCharacters().
*/
struct CharacterMovement {
    MoveToController movement;
    float turnSpeed;
    float modelForwardYaw;

    // Height of the model's visual centre above its ground position, for cameras that follow it.
    float visualCentreHeight = 0.0f;

    // Yaw still being turned towards, kept after arrival so the turn can finish.
    std::optional<float> targetYaw;

    void moveTo(const Vec3& destination);
    void stop();
    bool isMoving() const;
};

/*
* Create a character entity with Transform, PreviousTransform, ModelRenderer and CharacterMovement.
* The entity's position is on the ground; the model is lifted so its lowest point touches it.
*/
Entity spawnCharacter(World& world, AssetManager& assets, const Model& model,
                      const CharacterConfig& config, const Vec3& groundPosition);

// System: move and turn every character by one step.
void updateCharacters(World& world, float deltaTime);

// The character's visual centre where it is drawn, blended between the last two ticks.
Vec3 getCharacterVisualCentre(const World& world, Entity character, float alpha);

// The unit direction the character faces where it is drawn (on the ground plane), blended between the last two ticks.
Vec3 getCharacterFacing(const World& world, Entity character, float alpha);
