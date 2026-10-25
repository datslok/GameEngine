#include "render/gpu/cloth_gpu.h"

#include <cmath>
#include <stdexcept>
#include <type_traits>

ClothTickCommands clothTickCommands(const Cloth& cloth, float seconds, const std::vector<Shape>& colliders) {
    ClothTickCommands commands;
    commands.settings = cloth.settings;
    commands.seconds = seconds;
    for (const ClothPin& pin : cloth.pins) {
        commands.pinTargets.push_back(pin.position);
    }
    commands.grab = cloth.grab;
    commands.colliders = colliders;
    return commands;
}

std::vector<Vec4> packClothPoints(const ClothParticles& particles) {
    std::vector<Vec4> points;
    points.reserve(particles.size());
    for (std::size_t index = 0; index < particles.size(); ++index) {
        const Vec3& position = particles.positions[index];
        points.push_back(Vec4{position.x, position.y, position.z, particles.inverseMasses[index]});
    }
    return points;
}

std::vector<GpuStretch> packStretch(const std::vector<StretchConstraint>& constraints) {
    std::vector<GpuStretch> packed;
    packed.reserve(constraints.size());
    for (const StretchConstraint& edge : constraints) {
        packed.push_back(GpuStretch{edge.first, edge.second, edge.restLength});
    }
    return packed;
}

std::vector<GpuBend> packBend(const std::vector<BendConstraint>& constraints) {
    std::vector<GpuBend> packed;
    packed.reserve(constraints.size());
    for (const BendConstraint& hinge : constraints) {
        GpuBend bend;
        bend.wing0 = hinge.wing0;
        bend.wing1 = hinge.wing1;
        bend.edgeStart = hinge.edgeStart;
        bend.edgeEnd = hinge.edgeEnd;
        bend.restAngle = hinge.restAngle;
        bend.stiffnessShare = hinge.stiffnessShare;
        for (int weight = 0; weight < 4; ++weight) {
            bend.isometricWeights[weight] = hinge.isometricWeights[weight];
        }
        packed.push_back(bend);
    }
    return packed;
}

GpuCollider packCollider(const Shape& shape) {
    return std::visit([](const auto& held) {
        using Held = std::decay_t<decltype(held)>;
        GpuCollider collider;
        if constexpr (std::is_same_v<Held, Sphere>) {
            collider.kind = CLOTH_COLLIDER_SPHERE;
            collider.radius = held.radius;
            collider.a = Vec4{held.centre.x, held.centre.y, held.centre.z, 1.0f};
        } else if constexpr (std::is_same_v<Held, Capsule>) {
            collider.kind = CLOTH_COLLIDER_CAPSULE;
            collider.radius = held.radius;
            collider.a = Vec4{held.a.x, held.a.y, held.a.z, 1.0f};
            collider.b = Vec4{held.b.x, held.b.y, held.b.z, 1.0f};
        } else {
            collider.kind = CLOTH_COLLIDER_BOX;
            collider.a = Vec4{held.minimum.x, held.minimum.y, held.minimum.z, 1.0f};
            collider.b = Vec4{held.maximum.x, held.maximum.y, held.maximum.z, 1.0f};
        }
        return collider;
    }, shape);
}

std::vector<std::uint32_t> clothPinSlots(const Cloth& cloth) {
    std::vector<std::uint32_t> slots(cloth.particles.size(), clothNoPin);
    for (std::size_t pin = 0; pin < cloth.pins.size(); ++pin) {
        slots.at(cloth.pins[pin].particle) = static_cast<std::uint32_t>(pin);
    }
    return slots;
}

// The same expressions as the CPU solver's (cloth.cpp's stepTick, solveStretch, solveBend, solveGrab), in float.
GpuClothTick packClothTick(const ClothTickCommands& commands, std::uint32_t pinFirst, std::uint32_t colliderFirst) {
    const ClothSettings& settings = commands.settings;
    if (settings.substeps < 1) {
        throw std::invalid_argument("A cloth needs at least one substep a tick");
    }

    const float substep = commands.seconds / static_cast<float>(settings.substeps);
    GpuClothTick tick;
    tick.substepSeconds = substep;
    tick.gravity = settings.gravity;
    tick.stretchCompliance = settings.stretchCompliance / (substep * substep);
    tick.bendingStiffness = settings.bendingStiffness;
    tick.bendPerSecondSquared = 1.0f / (substep * substep);
    tick.grabCompliance = settings.grabCompliance / (substep * substep);
    tick.kept = std::exp(-settings.drag * substep);
    tick.thickness = settings.thickness;
    tick.staticFriction = settings.staticFriction;
    tick.kineticFriction = settings.kineticFriction;
    tick.bendingModel = static_cast<std::uint32_t>(settings.bendingModel);
    tick.substeps = static_cast<std::uint32_t>(settings.substeps);
    tick.pinFirst = pinFirst;
    tick.colliderFirst = colliderFirst;
    tick.colliderCount = static_cast<std::uint32_t>(commands.colliders.size());
    if (commands.grab) {
        tick.grabParticle = commands.grab->particle;
        tick.grabTarget = Vec4{commands.grab->target.x, commands.grab->target.y, commands.grab->target.z, 1.0f};
    }
    return tick;
}

std::uint32_t clothDispatchesPerTick(const Cloth& cloth, const ClothSettings& settings) {
    const auto colours = [](const std::vector<std::uint32_t>& starts) {
        return starts.empty() ? 0u : static_cast<std::uint32_t>(starts.size() - 1);
    };
    const std::uint32_t bendColours = settings.bendingStiffness > 0.0f ? colours(cloth.bendColours) : 0u;
    return static_cast<std::uint32_t>(settings.substeps) * (2u + colours(cloth.stretchColours) + bendColours);
}
