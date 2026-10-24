#include "cloth/cloth.h"

#include "scene/deforming_mesh.h"

void visit(ClothSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Area density", settings.areaDensity,
                  {.min = 0.001f, .max = 10.0f, .sliderMin = 0.02f, .sliderMax = 1.0f, .unit = "kg/m2", .applies = FieldApplies::AtStartup});
    visitor.field("Gravity", settings.gravity, {.min = 0.0f, .max = 100.0f, .sliderMax = 30.0f, .unit = "m/s2"});
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
    return cloth;
}

/*
* The points are copied into the mesh rather than shared, so the simulation never depends on drawing; for a 100 x 100
* sheet that is 120 KB a tick.
*/
void stepCloths(World& world, float seconds) {
    world.each<Cloth>([&](Entity entity, Cloth& cloth) {
        predictCloth(cloth.particles, Vec3{0.0f, -cloth.settings.gravity, 0.0f}, seconds);
        finishClothStep(cloth.particles, seconds);

        if (DeformingMesh* mesh = world.tryGet<DeformingMesh>(entity)) {
            mesh->positions = cloth.particles.positions;
        }
    });
}
