#include "cloth/cloth.h"

#include "physics/overlap.h"
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
    visitor.field("Grab compliance", settings.grabCompliance, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.01f, .unit = "m/N"});
    visitor.field("Bending stiffness", settings.bendingStiffness, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.0005f, .unit = "N m"});
    visitor.field("Thickness", settings.thickness, {.min = 0.0f, .max = 0.1f, .sliderMax = 0.02f, .unit = "m"});
    visitor.field("Static friction", settings.staticFriction, {.min = 0.0f, .max = 2.0f});
    visitor.field("Kinetic friction", settings.kineticFriction, {.min = 0.0f, .max = 2.0f});
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
    cloth.bend = bendConstraints(placed);
    for (const Triangle& triangle : placed.triangles) {
        cloth.triangles.push_back({static_cast<std::uint32_t>(triangle.first), static_cast<std::uint32_t>(triangle.second),
                                   static_cast<std::uint32_t>(triangle.third)});
    }
    return cloth;
}

void addClothPin(Cloth& cloth, std::uint32_t particle) {
    pinParticle(cloth.particles, particle);
    cloth.pins.push_back(ClothPin{particle, cloth.particles.positions[particle]});
}

namespace {
    // What one tick solves besides gravity and drag.
    struct TickConstraints {
        const std::vector<StretchConstraint>& stretch;
        const std::vector<BendConstraint>& bend;
        const std::vector<ClothPin>& pins;
        const std::optional<ClothGrab>& grab;
        const std::vector<Shape>& colliders;
    };

    // The shapes whose boxes meet the box round the cloth, grown by its thickness: most bodies are nowhere near it.
    std::vector<Shape> nearbyShapes(const ClothParticles& particles, const std::vector<Shape>& shapes, float thickness) {
        if (shapes.empty() || particles.size() == 0) {
            return {};
        }

        Aabb around{particles.positions.front(), particles.positions.front()};
        for (const Vec3& position : particles.positions) {
            around = enclosing(around, Aabb{position, position});
        }
        const Vec3 margin{thickness, thickness, thickness};
        around = Aabb{around.minimum - margin, around.maximum + margin};

        std::vector<Shape> nearby;
        for (const Shape& shape : shapes) {
            if (overlaps(around, boundsOf(shape))) {
                nearby.push_back(shape);
            }
        }
        return nearby;
    }

    /*
    * Each substep: predict; slide the moving pins a share of the way to where they are going (the last substep exactly
    * there); stretch, then bending, then the hand, then the bodies, last so nothing is left inside them; velocities from
    * the motion; drag on the free points.
    */
    void stepTick(ClothParticles& particles, const TickConstraints& constraints, const ClothSettings& settings, float seconds) {
        if (settings.substeps < 1) {
            throw std::invalid_argument("A cloth needs at least one substep a tick");
        }

        const float substep = seconds / static_cast<float>(settings.substeps);
        const Vec3 gravity{0.0f, -settings.gravity, 0.0f};
        const float kept = std::exp(-settings.drag * substep);

        std::vector<Vec3> pinStarts;
        pinStarts.reserve(constraints.pins.size());
        for (const ClothPin& pin : constraints.pins) {
            pinStarts.push_back(particles.positions.at(pin.particle));
        }

        for (int step = 0; step < settings.substeps; ++step) {
            predictCloth(particles, gravity, substep);

            const float share = static_cast<float>(step + 1) / static_cast<float>(settings.substeps);
            for (std::size_t index = 0; index < constraints.pins.size(); ++index) {
                const ClothPin& pin = constraints.pins[index];
                particles.positions[pin.particle] = step + 1 == settings.substeps
                    ? pin.position
                    : pinStarts[index] + (pin.position - pinStarts[index]) * share;
            }

            solveStretch(particles, constraints.stretch, settings.stretchCompliance, substep);
            solveBend(particles, constraints.bend, settings.bendingStiffness, substep);
            if (constraints.grab) {
                solveGrab(particles, *constraints.grab, settings.grabCompliance, substep);
            }
            if (!constraints.colliders.empty()) {
                solveContacts(particles, nearbyShapes(particles, constraints.colliders, settings.thickness), settings);
            }
            finishClothStep(particles, substep);

            // The air slows free points; a pin's velocity stays its motion, which the game drives.
            for (std::size_t index = 0; index < particles.size(); ++index) {
                if (particles.inverseMasses[index] > 0.0f) {
                    particles.velocities[index] = particles.velocities[index] * kept;
                }
            }
        }
    }
}

void stepClothParticles(ClothParticles& particles, const std::vector<StretchConstraint>& stretch, const ClothSettings& settings, float seconds,
                        const std::optional<ClothGrab>& grab, const std::vector<Shape>& colliders) {
    const std::vector<BendConstraint> noBend;
    const std::vector<ClothPin> noPins;
    stepTick(particles, TickConstraints{stretch, noBend, noPins, grab, colliders}, settings, seconds);
}

void stepCloth(Cloth& cloth, float seconds, const std::vector<Shape>& colliders) {
    stepTick(cloth.particles, TickConstraints{cloth.stretch, cloth.bend, cloth.pins, cloth.grab, colliders}, cloth.settings, seconds);
}

/*
* The points are copied into the mesh rather than shared, so the simulation never depends on drawing; for a 100 x 100
* sheet that is 120 KB a tick.
*/
void stepCloths(World& world, float seconds) {
    const std::vector<Shape> colliders = worldColliderShapes(world);
    world.each<Cloth>([&](Entity entity, Cloth& cloth) {
        stepCloth(cloth, seconds, colliders);

        if (DeformingMesh* mesh = world.tryGet<DeformingMesh>(entity)) {
            mesh->positions = cloth.particles.positions;
        }
    });
}
