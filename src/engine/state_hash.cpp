#include "engine/state_hash.h"
#include "cloth/cloth.h"
#include "core/hash.h"
#include "gameplay/character.h"
#include "gameplay/first_person_body.h"
#include "gameplay/grenade.h"
#include "math/transform.h"
#include "physics/character_controller.h"
#include "physics/particle.h"
#include "scene/deforming_mesh.h"
#include "scene/interpolation.h"

namespace {
    void add(Fnv1aHash& hash, const Vec3& vector) {
        hash.add(vector.x);
        hash.add(vector.y);
        hash.add(vector.z);
    }

    void add(Fnv1aHash& hash, const Quat& rotation) {
        hash.add(rotation.x);
        hash.add(rotation.y);
        hash.add(rotation.z);
        hash.add(rotation.w);
    }

    void add(Fnv1aHash& hash, const Transform& transform) {
        add(hash, transform.position);
        add(hash, transform.rotation);
        add(hash, transform.scale);
    }

    void add(Fnv1aHash& hash, float number) {
        hash.add(number);
    }

    void add(Fnv1aHash& hash, Entity entity) {
        hash.add(entity.index);
        hash.add(entity.generation);
    }

    // Whether there is a value, then the value, so "none" never matches a value whose bits happen to be zero.
    template <typename Value>
    void add(Fnv1aHash& hash, const std::optional<Value>& optional) {
        hash.add(optional.has_value());

        if (optional) {
            add(hash, *optional);
        }
    }
}

/*
* Each component type is walked in turn and tagged with its own number first, so the same values in two different
* components cannot cancel out or swap places unnoticed.
*/
std::uint64_t hashSimulationState(const World& world) {
    Fnv1aHash hash;
    hash.add(static_cast<std::uint64_t>(world.getEntityCount()));

    hash.add(1u);
    world.each<Transform>([&](Entity entity, const Transform& transform) {
        add(hash, entity);
        add(hash, transform);
    });

    hash.add(2u);
    world.each<PreviousTransform>([&](Entity entity, const PreviousTransform& previous) {
        add(hash, entity);
        add(hash, previous.transform);
    });

    hash.add(3u);
    world.each<CharacterController>([&](Entity entity, const CharacterController& controller) {
        add(hash, entity);
        add(hash, controller.wishVelocity);
        hash.add(controller.wantsJump);
        add(hash, controller.velocity);
        hash.add(controller.grounded);
        add(hash, controller.groundNormal);
        hash.add(controller.lastSubsteps);
        hash.add(controller.groundKnown);
    });

    hash.add(4u);
    world.each<CharacterMovement>([&](Entity entity, const CharacterMovement& movement) {
        add(hash, entity);
        add(hash, movement.movement.getTarget());
        hash.add(movement.yaw);
        add(hash, movement.targetYaw);
    });

    hash.add(5u);
    world.each<FirstPersonBody>([&](Entity entity, const FirstPersonBody& body) {
        add(hash, entity);
        hash.add(body.wantsCrouch);
        hash.add(body.crouching);
        hash.add(body.eyeHeight);
    });

    hash.add(6u);
    world.each<Particle>([&](Entity entity, const Particle& particle) {
        add(hash, entity);
        add(hash, particle.velocity);
    });

    hash.add(7u);
    world.each<Grenade>([&](Entity entity, const Grenade& grenade) {
        add(hash, entity);
        hash.add(grenade.secondsLeft);
    });

    hash.add(8u);
    // A GPU cloth's mesh is shaped from its late points (see the cloths below).
    world.each<DeformingMesh>([&](Entity entity, const DeformingMesh& mesh) {
        const Cloth* cloth = world.tryGet<Cloth>(entity);
        if (cloth != nullptr && cloth->onGpu) {
            return;
        }
        add(hash, entity);
        hash.add(static_cast<std::uint64_t>(mesh.positions.size()));
        for (const Vec3& position : mesh.positions) {
            add(hash, position);
        }
    });

    hash.add(9u);
    // A cloth on the GPU holds the GPU's points read back a few frames late, repeatable only on one GPU and driver (as
    // the golden images are), so they are left out; what the simulation decides for it (its pins, its hand) is not.
    world.each<Cloth>([&](Entity entity, const Cloth& cloth) {
        const ClothParticles& particles = cloth.particles;
        add(hash, entity);
        hash.add(cloth.onGpu);
        hash.add(static_cast<std::uint64_t>(particles.size()));
        for (std::size_t index = 0; index < particles.size() && !cloth.onGpu; ++index) {
            add(hash, particles.positions[index]);
            add(hash, particles.velocities[index]);
            hash.add(particles.inverseMasses[index]);
        }
        hash.add(static_cast<std::uint64_t>(cloth.pins.size()));
        for (const ClothPin& pin : cloth.pins) {
            hash.add(pin.particle);
            add(hash, pin.position);
        }
        hash.add(cloth.grab.has_value());
        if (cloth.grab) {
            hash.add(cloth.grab->particle);
            add(hash, cloth.grab->target);
        }
    });

    return hash.value;
}
