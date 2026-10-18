#include "gameplay/character.h"
#include "math/angles.h"
#include "math/transform.h"
#include "physics/character_controller.h"
#include "physics/collider.h"
#include "scene/interpolation.h"
#include "scene/model.h"
#include "scene/model_renderer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

void CharacterMovement::moveTo(const Vec3& destination) {
    movement.setTarget(destination);
}

void CharacterMovement::stop() {
    movement.stop();
    targetYaw.reset();
}

bool CharacterMovement::isMoving() const {
    return movement.isMoving();
}

/*
* Validate and build every component before creating the entity, so bad input cannot leave a half-built character in the world.
*/
Entity spawnCharacter(World& world, AssetManager& assets, const Model& model,
                      const CharacterConfig& config, const Vec3& groundPosition) {
    const CharacterControllerSettings& body = config.controller;
    if (!std::isfinite(body.radius) || body.radius <= 0.0f || !std::isfinite(body.height) || body.height < 2.0f * body.radius) {
        throw std::invalid_argument("A character's capsule needs a positive radius and a height of at least two radii");
    }
    if (!std::isfinite(config.turnSpeed) || config.turnSpeed <= 0.0f) {
        throw std::invalid_argument("Character turn speed must be finite and positive");
    }
    if (!std::isfinite(config.modelForwardYaw)) {
        throw std::invalid_argument("Character forward yaw must be finite");
    }
    if (!std::isfinite(groundPosition.x) ||
        !std::isfinite(groundPosition.y) ||
        !std::isfinite(groundPosition.z)) {
        throw std::invalid_argument("Character position must be finite");
    }

    const Mat4 normalization = model.getNormalizationMatrix(config.modelSize);
    const Aabb bounds = model.getBounds();
    const Vec4 bottom = normalization * Vec4{
        bounds.minimum.x, bounds.minimum.y, bounds.minimum.z, 1.0f
    };
    const float visualCentreHeight = -bottom.y;

    // Keep the gameplay origin on the ground. Lift only the visual model.
    const Mat4 groundedNormalization =
        Mat4::translation(0.0f, visualCentreHeight, 0.0f) * normalization;

    CharacterMovement movement{
        MoveToController{config.movementSpeed},
        config.turnSpeed,
        config.modelForwardYaw,
        visualCentreHeight,
        std::nullopt
    };
    ModelRenderer renderer = assets.makeModelRenderer(model, groundedNormalization);

    // An upright capsule around the model as drawn: as wide as its wider horizontal side, from its feet to the top of its
    // head. Generous rather than exact (the corners of the model's box poke out), which is what clicking a unit wants.
    const Vec3 lowCorner = transformPoint(groundedNormalization, bounds.minimum);
    const Vec3 highCorner = transformPoint(groundedNormalization, bounds.maximum);
    const Aabb drawn{
        Vec3{std::min(lowCorner.x, highCorner.x), std::min(lowCorner.y, highCorner.y), std::min(lowCorner.z, highCorner.z)},
        Vec3{std::max(lowCorner.x, highCorner.x), std::max(lowCorner.y, highCorner.y), std::max(lowCorner.z, highCorner.z)}
    };
    const float radius = std::max(drawn.size().x, drawn.size().z) * 0.5f;
    const Vec3 middle = drawn.centre();
    const float lowEnd = drawn.minimum.y + radius;
    const float highEnd = std::max(lowEnd, drawn.maximum.y - radius);
    const Collider collider{Capsule{Vec3{middle.x, lowEnd, middle.z}, Vec3{middle.x, highEnd, middle.z}, radius}, nullptr};

    Transform placement;
    placement.position = groundPosition;

    const Entity entity = world.create();
    world.add(entity, placement);
    world.add(entity, PreviousTransform{placement});
    world.add(entity, std::move(renderer));
    world.add(entity, std::move(movement));
    world.add(entity, collider);
    CharacterController controller;
    controller.settings = config.controller;
    world.add(entity, controller);

    return entity;
}

/*
* The move command becomes a wished velocity, the character controller moves the character through the level with it
* (walls stop it, stairs and ramps lift it), and then it turns towards the direction it actually travelled, at most
* turnSpeed radians per second. Every character has a controller (spawnCharacter adds one).
*/
void updateCharacters(World& world, const StaticLevel& level, float deltaTime) {
    world.each<CharacterMovement, Transform>([&](Entity entity, CharacterMovement& character, Transform& transform) {
        const Vec3 previousPosition = transform.position;
        CharacterController& controller = world.get<CharacterController>(entity);
        controller.wishVelocity = character.movement.desiredVelocity(transform.position, deltaTime);
        moveCharacter(controller, transform.position, level, deltaTime);
        character.movement.updateArrival(transform.position);

        const Vec3 displacement = transform.position - previousPosition;
        if (displacement.x != 0.0f || displacement.z != 0.0f) {
            const float heading = std::atan2(displacement.x, displacement.z);
            character.targetYaw = heading - character.modelForwardYaw;
        }

        // Finish facing the last movement direction even after arrival.
        if (!character.targetYaw) {
            return;
        }

        float& currentYaw = transform.rotation.y;
        const float difference = wrapAngle(*character.targetYaw - currentYaw);
        const float maximumTurn = character.turnSpeed * deltaTime;

        if (std::abs(difference) <= maximumTurn) {
            currentYaw = wrapAngle(*character.targetYaw);
            character.targetYaw.reset();
        } else {
            currentYaw = wrapAngle(currentYaw + std::clamp(difference, -maximumTurn, maximumTurn));
        }
    });
}

Vec3 getCharacterVisualCentre(const World& world, Entity character, float alpha) {
    const Transform rendered = getRenderTransform(world, character, alpha);
    const float height = world.get<CharacterMovement>(character).visualCentreHeight;

    return rendered.position + Vec3{0.0f, height, 0.0f};
}

/*
* The model's own forward axis is at modelForwardYaw from +Z, and the entity's yaw turns it further, so the facing is the
* sum of the two. Using the drawn (interpolated) yaw keeps anything aimed along it, like a carried flashlight, in step with the model.
*/
Vec3 getCharacterFacing(const World& world, Entity character, float alpha) {
    const float yaw = getRenderTransform(world, character, alpha).rotation.y + world.get<CharacterMovement>(character).modelForwardYaw;

    return Vec3{std::sin(yaw), 0.0f, std::cos(yaw)};
}
