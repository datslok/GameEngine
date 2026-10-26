#include "engine/gpu_cloth.h"

#include "core/hash.h"

#include <algorithm>
#include <utility>

double gpuClothTickSeconds(double estimate, const GpuTimings& timings, std::uint32_t ticks) {
    if (!timings.valid || ticks == 0) {
        return estimate;
    }
    return timings.seconds(GpuPass::Cloth) / static_cast<double>(ticks);
}

std::uint64_t clothKey(Entity entity) {
    return (static_cast<std::uint64_t>(entity.generation) << 32) | entity.index;
}

std::vector<ClothWork> takeClothWork(World& world) {
    std::vector<ClothWork> work;
    world.each<Cloth>([&](Entity entity, Cloth& cloth) {
        if (!cloth.onGpu) {
            return;
        }
        const DeformingMesh* mesh = world.tryGet<DeformingMesh>(entity);
        work.push_back(ClothWork{clothKey(entity), &cloth, mesh != nullptr ? mesh->mesh : MeshHandle{}, nullptr, std::move(cloth.gpuTicks)});
        cloth.gpuTicks.clear();
    });
    return work;
}

void applyClothReadbacks(World& world, std::span<const ClothReadback> readbacks) {
    world.each<Cloth>([&](Entity entity, Cloth& cloth) {
        if (!cloth.onGpu) {
            return;
        }
        const std::uint64_t key = clothKey(entity);
        for (const ClothReadback& readback : readbacks) {
            if (readback.key != key || readback.positions.size() != cloth.particles.size() ||
                readback.velocities.size() != cloth.particles.size()) {
                continue;
            }
            cloth.particles.positions = readback.positions;
            cloth.particles.velocities = readback.velocities;
            if (DeformingMesh* mesh = world.tryGet<DeformingMesh>(entity)) {
                mesh->positions = readback.positions;
                mesh->previousPositions = readback.positions;
            }
        }
    });
}

Aabb lateClothBounds(const ClothParticles& particles, float lateSeconds, float thickness) {
    if (particles.size() == 0) {
        return Aabb{};
    }

    Aabb bounds{particles.positions.front(), particles.positions.front()};
    float fastest = 0.0f;
    for (std::size_t index = 0; index < particles.size(); ++index) {
        bounds = enclosing(bounds, Aabb{particles.positions[index], particles.positions[index]});
        fastest = std::max(fastest, particles.velocities[index].length());
    }
    const float grow = fastest * lateSeconds + thickness;
    const Vec3 margin{grow, grow, grow};
    return Aabb{bounds.minimum - margin, bounds.maximum + margin};
}

DeformedMesh gpuClothDeformedMesh(const Cloth& cloth, MeshHandle mesh, float lateSeconds, std::uint64_t frameNumber) {
    DeformedMesh deformed;
    deformed.mesh = mesh;
    deformed.bounds = lateClothBounds(cloth.particles, lateSeconds, cloth.settings.thickness);
    deformed.shape = mixBits(frameNumber) | 1u;
    deformed.shapedOnGpu = true;
    return deformed;
}
