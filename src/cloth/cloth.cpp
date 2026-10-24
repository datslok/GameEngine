#include "cloth/cloth.h"

#include "scene/deforming_mesh.h"

#include <cmath>
#include <stdexcept>

void visit(ClothSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Area density", settings.areaDensity,
                  {.min = 0.001f, .max = 10.0f, .sliderMin = 0.02f, .sliderMax = 1.0f, .unit = "kg/m2", .applies = FieldApplies::AtStartup});
    visitor.field("Gravity", settings.gravity, {.min = 0.0f, .max = 100.0f, .sliderMax = 30.0f, .unit = "m/s2"});
    visitor.field("Stretch compliance", settings.stretchCompliance, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.01f, .unit = "m/N"});
    visitor.field("Substeps", settings.substeps, {.min = 1, .max = 100, .sliderMax = 40, .unit = "per tick"});
    visitor.field("Drag", settings.drag, {.min = 0.0f, .max = 100.0f, .sliderMax = 5.0f, .unit = "per second"});
}

// The mesh is placed first, so the masses come from the placed sheet's areas (a scaling placement changes them).
Cloth makeCloth(const Mesh& mesh, const Mat4& placement, const ClothSettings& settings) {
    Mesh placed = mesh;
    for (Vec4& vertex : placed.vertices) {
        const Vec3 position = transformPoint(placement, Vec3{vertex.x, vertex.y, vertex.z});
        vertex = Vec4{position.x, position.y, position.z, 1.0f};
    }

    Cloth cloth;
    cloth.settings = settings;
    cloth.particles = makeClothParticles(placed, settings.areaDensity);
    cloth.stretch = stretchConstraints(placed);
    return cloth;
}

void stepClothParticles(ClothParticles& particles, const std::vector<StretchConstraint>& stretch, const ClothSettings& settings, float seconds) {
    if (settings.substeps < 1) {
        throw std::invalid_argument("A cloth needs at least one substep a tick");
    }

    const float substep = seconds / static_cast<float>(settings.substeps);
    const Vec3 gravity{0.0f, -settings.gravity, 0.0f};
    const float kept = std::exp(-settings.drag * substep);

    for (int step = 0; step < settings.substeps; ++step) {
        predictCloth(particles, gravity, substep);
        solveStretch(particles, stretch, settings.stretchCompliance, substep);
        finishClothStep(particles, substep);
        for (Vec3& velocity : particles.velocities) {
            velocity = velocity * kept;
        }
    }
}

void stepCloth(Cloth& cloth, float seconds) {
    stepClothParticles(cloth.particles, cloth.stretch, cloth.settings, seconds);
}

/*
* The points are copied into the mesh rather than shared, so the simulation never depends on drawing; for a 100 x 100
* sheet that is 120 KB a tick.
*/
void stepCloths(World& world, float seconds) {
    world.each<Cloth>([&](Entity entity, Cloth& cloth) {
        stepCloth(cloth, seconds);

        if (DeformingMesh* mesh = world.tryGet<DeformingMesh>(entity)) {
            mesh->positions = cloth.particles.positions;
        }
    });
}
