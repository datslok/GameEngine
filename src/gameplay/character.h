#pragma once

#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "gameplay/character_config.h"
#include "gameplay/move_to_controller.h"
#include "physics/static_level.h"
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

    // The facing about the vertical axis, in radians (-pi..pi). The character's own angle: updateCharacters turns it and
    // writes the Transform's rotation from it each tick, so set this, not the Transform, to face a character somewhere.
    float yaw = 0.0f;

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

// System: for every character, turn its move command into a wished velocity, move it through the level with its character
// controller, and turn it towards where it went.
void updateCharacters(World& world, const StaticLevel& level, float deltaTime);

// The character's visual centre where it is drawn, blended between the last two ticks.
Vec3 getCharacterVisualCentre(const World& world, Entity character, float alpha);

// The unit direction the character faces where it is drawn (on the ground plane), blended between the last two ticks.
Vec3 getCharacterFacing(const World& world, Entity character, float alpha);
