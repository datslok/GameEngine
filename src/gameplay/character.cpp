#include "gameplay/character.h"
#include "math/transform.h"
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
Entity spawnCharacter(World& world, const Model& model,
                      const CharacterConfig& config, const Vec3& groundPosition) {
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
    const ModelBounds bounds = model.getBounds();
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
    ModelRenderer renderer = makeModelRenderer(model, groundedNormalization);

    Transform placement;
    placement.position = groundPosition;

    const Entity entity = world.create();
    world.add(entity, placement);
    world.add(entity, PreviousTransform{placement});
    world.add(entity, std::move(renderer));
    world.add(entity, std::move(movement));

    return entity;
}

/*
* Step towards the destination, then turn towards the direction actually travelled, at most turnSpeed radians per second.
*/
void updateCharacters(World& world, float deltaTime) {
    world.each<CharacterMovement, Transform>([deltaTime](Entity, CharacterMovement& character, Transform& transform) {
        const Vec3 previousPosition = transform.position;
        character.movement.update(transform.position, deltaTime);

        const Vec3 displacement = transform.position - previousPosition;
        if (displacement.x != 0.0f || displacement.z != 0.0f) {
            const float heading = std::atan2(displacement.x, displacement.z);
            character.targetYaw = heading - character.modelForwardYaw;
        }

        // Finish facing the last movement direction even after arrival.
        if (!character.targetYaw) {
            return;
        }

        constexpr float fullTurn = 2.0f * std::numbers::pi_v<float>;
        float& currentYaw = transform.rotation.y;
        const float difference = std::remainder(*character.targetYaw - currentYaw, fullTurn);
        const float maximumTurn = character.turnSpeed * deltaTime;

        if (std::abs(difference) <= maximumTurn) {
            currentYaw = std::remainder(*character.targetYaw, fullTurn);
            character.targetYaw.reset();
        } else {
            currentYaw = std::remainder(
                currentYaw + std::clamp(difference, -maximumTurn, maximumTurn),
                fullTurn
            );
        }
    });
}

Vec3 getCharacterVisualCentre(const World& world, Entity character, float alpha) {
    const Transform rendered = getRenderTransform(world, character, alpha);
    const float height = world.get<CharacterMovement>(character).visualCentreHeight;

    return rendered.position + Vec3{0.0f, height, 0.0f};
}
