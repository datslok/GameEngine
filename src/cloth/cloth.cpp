#include "cloth/cloth.h"

#include "physics/overlap.h"
#include "scene/deforming_mesh.h"

#include <array>
#include <cmath>
#include <stdexcept>

namespace {
    // In the order of BendingModel's values.
    constexpr std::array<const char*, 2> bendingModelNames{"Isometric", "Hinge"};

    static_assert(static_cast<int>(BendingModel::Isometric) == 0 && static_cast<int>(BendingModel::Hinge) == 1,
                  "bendingModelNames lists the models by their values");
}

void visit(ClothSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Area density", settings.areaDensity,
                  {.min = 0.001f, .max = 10.0f, .sliderMin = 0.02f, .sliderMax = 1.0f, .unit = "kg/m2", .applies = FieldApplies::AtStartup});
    visitor.field("Gravity", settings.gravity, {.min = 0.0f, .max = 100.0f, .sliderMax = 30.0f, .unit = "m/s2"});
    visitor.field("Stretch compliance", settings.stretchCompliance, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.01f, .unit = "m/N"});
    visitor.field("Substeps", settings.substeps, {.min = 1, .max = 100, .sliderMax = 40, .unit = "per tick"});
    visitor.field("Drag", settings.drag, {.min = 0.0f, .max = 100.0f, .sliderMax = 5.0f, .unit = "per second"});
    visitor.field("Grab compliance", settings.grabCompliance, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.01f, .unit = "m/N"});
    visitor.field("Bending stiffness", settings.bendingStiffness, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.0005f, .unit = "N m"});
    visitor.choice("Bending model", settings.bendingModel, bendingModelNames);
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
    cloth.stretchColours = colourStretch(cloth.stretch, cloth.particles.size());
    cloth.bendColours = colourBend(cloth.bend, cloth.particles.size());
    for (const Triangle& triangle : placed.triangles) {
        cloth.triangles.push_back({static_cast<std::uint32_t>(triangle.first), static_cast<std::uint32_t>(triangle.second),
                                   static_cast<std::uint32_t>(triangle.third)});
    }
    return cloth;
}

Vec3 slidPinPosition(const Vec3& start, const Vec3& target, int step, int substeps) {
    if (step + 1 == substeps) {
        return target;
    }
    const float share = static_cast<float>(step + 1) / static_cast<float>(substeps);
    return start + (target - start) * share;
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

        // Where each colour starts, or empty: the constraints solved in their order on the caller.
        const std::vector<std::uint32_t>& stretchColours;
        const std::vector<std::uint32_t>& bendColours;
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
    void stepTick(ClothParticles& particles, const TickConstraints& constraints, const ClothSettings& settings, float seconds, JobSystem& jobs) {
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

            for (std::size_t index = 0; index < constraints.pins.size(); ++index) {
                const ClothPin& pin = constraints.pins[index];
                particles.positions[pin.particle] = slidPinPosition(pinStarts[index], pin.position, step, settings.substeps);
            }

            if (constraints.stretchColours.empty()) {
                solveStretch(particles, constraints.stretch, settings.stretchCompliance, substep);
            } else {
                solveStretchColours(particles, constraints.stretch, constraints.stretchColours, settings.stretchCompliance, substep, jobs);
            }
            if (constraints.bendColours.empty()) {
                solveBend(particles, constraints.bend, settings.bendingStiffness, substep, settings.bendingModel);
            } else {
                solveBendColours(particles, constraints.bend, constraints.bendColours, settings.bendingStiffness, substep, settings.bendingModel, jobs);
            }
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
    const std::vector<std::uint32_t> noColours;
    const std::vector<ClothPin> noPins;
    stepTick(particles, TickConstraints{stretch, noBend, noColours, noColours, noPins, grab, colliders}, settings, seconds, serialJobs());
}

void stepCloth(Cloth& cloth, float seconds, const std::vector<Shape>& colliders, JobSystem& jobs) {
    stepTick(cloth.particles,
             TickConstraints{cloth.stretch, cloth.bend, cloth.stretchColours, cloth.bendColours, cloth.pins, cloth.grab, colliders},
             cloth.settings, seconds, jobs);
}

/*
* The points are copied into the mesh rather than shared, so the simulation never depends on drawing; for a 100 x 100
* sheet that is 120 KB a tick.
*/
/*
* The cloths are gathered first, in the world's order, then stepped a job each: each writes only its own points and
* only reads the bodies, so stepping them at once gives what stepping them in turn does.
*/
void stepCloths(World& world, float seconds, JobSystem& jobs) {
    const std::vector<Shape> colliders = worldColliderShapes(world);
    std::vector<Cloth*> cloths;
    std::vector<DeformingMesh*> meshes;
    world.each<Cloth>([&](Entity entity, Cloth& cloth) {
        cloths.push_back(&cloth);
        meshes.push_back(world.tryGet<DeformingMesh>(entity));
    });

    jobs.parallelFor(cloths.size(), 1, [&](std::size_t index) {
        stepCloth(*cloths[index], seconds, colliders, jobs);
        if (meshes[index] != nullptr) {
            meshes[index]->positions = cloths[index]->particles.positions;
        }
    });
}
