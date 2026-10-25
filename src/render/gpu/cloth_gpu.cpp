#include "render/gpu/cloth_gpu.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>

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

std::vector<GpuPointEdge> packPointEdges(const Cloth& cloth) {
    const ClothPointConstraints& lists = cloth.pointConstraints;
    std::vector<GpuPointEdge> packed(lists.edges.size());
    for (std::uint32_t point = 0; point + 1 < lists.edgeStarts.size(); ++point) {
        for (std::uint32_t at = lists.edgeStarts[point]; at < lists.edgeStarts[point + 1]; ++at) {
            const StretchConstraint& edge = cloth.stretch[lists.edges[at]];
            packed[at] = GpuPointEdge{edge.first == point ? edge.second : edge.first, edge.restLength};
        }
    }
    return packed;
}

std::vector<GpuPointHinge> packPointHinges(const Cloth& cloth) {
    const ClothPointConstraints& lists = cloth.pointConstraints;
    std::vector<GpuPointHinge> packed;
    packed.reserve(lists.hinges.size());
    for (const std::uint32_t entry : lists.hinges) {
        const BendConstraint& hinge = cloth.bend[entry / 4];
        GpuPointHinge corner;
        corner.wing0 = hinge.wing0;
        corner.wing1 = hinge.wing1;
        corner.edgeStart = hinge.edgeStart;
        corner.edgeEnd = hinge.edgeEnd;
        for (int weight = 0; weight < 4; ++weight) {
            corner.isometricWeights[weight] = hinge.isometricWeights[weight];
        }
        corner.restAngle = hinge.restAngle;
        corner.stiffnessShare = hinge.stiffnessShare;
        corner.corner = entry % 4;
        packed.push_back(corner);
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

// The same expressions as the CPU solver's (cloth.cpp's stepTick, solveStretch, solveBend, solveGrab, clothPointForce), in float.
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
    tick.perSecondSquared = 1.0f / (substep * substep);
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
    tick.stretchStiffness = energyStiffness(settings.stretchCompliance);
    tick.grabStiffness = energyStiffness(settings.grabCompliance);
    tick.method = static_cast<std::uint32_t>(settings.method);
    return tick;
}

GpuClothLevels packClothLevels(const ClothHierarchy& hierarchy, std::size_t pointCount) {
    GpuClothLevels packed;
    const auto links = [](std::vector<GpuClothLink>& into, const std::vector<ClothLink>& from) {
        for (const ClothLink& link : from) {
            into.push_back(GpuClothLink{link.point, link.weight});
        }
    };
    // Starts counted from the front of their list's buffer: the level's own plus where its list begins there.
    const auto starts = [](std::vector<std::uint32_t>& into, const std::vector<std::uint32_t>& from, std::size_t listFirst) {
        for (const std::uint32_t start : from) {
            into.push_back(static_cast<std::uint32_t>(listFirst + start));
        }
    };

    for (const ClothLevel& level : hierarchy.levels) {
        GpuClothLevel record;
        record.pointsFirst = static_cast<std::uint32_t>(packed.points.size());
        record.pointCount = static_cast<std::uint32_t>(level.points.size());
        record.neighbourStartsFirst = static_cast<std::uint32_t>(packed.neighbourStarts.size());
        record.gatherStartsFirst = static_cast<std::uint32_t>(packed.gatherStarts.size());
        record.spreadStartsFirst = static_cast<std::uint32_t>(packed.spreadStarts.size());
        packed.records.push_back(record);

        packed.points.insert(packed.points.end(), level.points.begin(), level.points.end());
        packed.masses.insert(packed.masses.end(), level.masses.begin(), level.masses.end());
        starts(packed.neighbourStarts, level.neighbourStarts, packed.neighbours.size());
        packed.neighbours.insert(packed.neighbours.end(), level.neighbours.begin(), level.neighbours.end());
        starts(packed.gatherStarts, level.gatherStarts, packed.gather.size());
        links(packed.gather, level.gather);
        starts(packed.spreadStarts, level.spreadStarts, packed.spread.size());
        links(packed.spread, level.spread);
    }

    packed.pointLevels.assign(pointCount, 0);
    for (std::size_t point = 0; point < std::min(pointCount, hierarchy.pointLevels.size()); ++point) {
        packed.pointLevels[point] = static_cast<std::uint32_t>(hierarchy.pointLevels[point]);
    }
    return packed;
}

std::uint32_t clothLevelsUsed(const Cloth& cloth, const ClothSettings& settings) {
    if (settings.method != ClothMethod::VertexBlockDescent || settings.hierarchyLevels < 1) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::min(static_cast<std::size_t>(settings.hierarchyLevels), cloth.hierarchy.levels.size()));
}

std::uint32_t clothDispatchesPerTick(const Cloth& cloth, const ClothSettings& settings) {
    const auto colours = [](const std::vector<std::uint32_t>& starts) {
        return starts.empty() ? 0u : static_cast<std::uint32_t>(starts.size() - 1);
    };
    const std::uint32_t substeps = static_cast<std::uint32_t>(settings.substeps);
    if (settings.method == ClothMethod::VertexBlockDescent) {
        const std::uint32_t sweep = colours(cloth.vertexColours.colours);
        const std::uint32_t levels = clothLevelsUsed(cloth, settings);
        const std::uint32_t layered = levels > 0 ? 1u + clothDispatchesPerLevel * levels + sweep : 0u;
        return substeps * (2u + sweep + layered);
    }
    const std::uint32_t bendColours = settings.bendingStiffness > 0.0f ? colours(cloth.bendColours) : 0u;
    return substeps * (2u + colours(cloth.stretchColours) + bendColours);
}
