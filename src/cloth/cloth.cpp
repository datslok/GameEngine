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

    // In the order of ClothMethod's values.
    constexpr std::array<const char*, 2> methodNames{"XPBD", "Vertex block descent"};

    static_assert(static_cast<int>(ClothMethod::Xpbd) == 0 && static_cast<int>(ClothMethod::VertexBlockDescent) == 1,
                  "methodNames lists the methods by their values");
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
    visitor.field("Bending every", settings.bendingInterval, {.min = 1, .max = 16, .unit = "substeps"});
    visitor.field("Thickness", settings.thickness, {.min = 0.0f, .max = 0.1f, .sliderMax = 0.02f, .unit = "m"});
    visitor.field("Static friction", settings.staticFriction, {.min = 0.0f, .max = 2.0f});
    visitor.field("Kinetic friction", settings.kineticFriction, {.min = 0.0f, .max = 2.0f});
    visitor.choice("Solver", settings.method, methodNames);
    visitor.field("Coarse levels", settings.hierarchyLevels, {.min = 0, .max = maxClothLevels});
    visitor.field("Tethers", settings.tethers);
    visitor.field("Tether slack", settings.tetherSlack, {.min = 0.0f, .max = 0.5f, .sliderMax = 0.02f, .unit = "of the rest length"});
    visitor.field("Seam speed", settings.seamSpeed, {.min = 0.01f, .max = 100.0f, .sliderMax = 5.0f, .unit = "m/s"});
}

// The mesh is placed first, so the masses come from the placed sheet's areas (a scaling placement changes them).
Cloth makeCloth(const Mesh& mesh, const Mat4& placement, const ClothSettings& settings) {
    Mesh placed = mesh;
    std::vector<Vec3> start;
    start.reserve(placed.vertices.size());
    for (Vec4& vertex : placed.vertices) {
        const Vec3 position = transformPoint(placement, Vec3{vertex.x, vertex.y, vertex.z});
        vertex = Vec4{position.x, position.y, position.z, 1.0f};
        start.push_back(position);
    }
    return makeCloth(placed, start, settings);
}

Cloth makeCloth(const Mesh& rest, const std::vector<Vec3>& start, const ClothSettings& settings, const ClothSewing& sewing) {
    if (start.size() != rest.vertices.size()) {
        throw std::invalid_argument("A cloth needs one starting position per vertex");
    }

    Cloth cloth;
    cloth.settings = settings;
    cloth.particles = makeClothParticles(rest, settings.areaDensity);
    cloth.stretch = stretchConstraints(rest);
    cloth.bend = bendConstraints(rest);
    cloth.bend.insert(cloth.bend.end(), sewing.hinges.begin(), sewing.hinges.end());
    cloth.stitches = sewing.stitches;
    cloth.pieces = sewing.pieces;
    cloth.stretchColours = colourStretch(cloth.stretch, cloth.particles.size());
    cloth.bendColours = colourBend(cloth.bend, cloth.particles.size());
    for (const Triangle& triangle : rest.triangles) {
        cloth.triangles.push_back({static_cast<std::uint32_t>(triangle.first), static_cast<std::uint32_t>(triangle.second),
                                   static_cast<std::uint32_t>(triangle.third)});
    }
    cloth.restPositions = cloth.particles.positions;
    cloth.particles.positions = start;
    cloth.particles.previousPositions = start;
    cloth.hierarchy = buildClothHierarchy(cloth.restPositions, cloth.stretch, cloth.particles.inverseMasses);
    cloth.pointConstraints = clothPointConstraints(cloth.stretch, cloth.bend, cloth.particles.size());
    cloth.vertexColours = clothVertexColours(cloth.stretch, cloth.bend, cloth.pointConstraints);
    return cloth;
}

int bendingSubsteps(int step, int substeps, int interval) {
    if (interval < 1) {
        throw std::invalid_argument("Bending needs an interval of at least one substep");
    }
    if (step + 1 == substeps) {
        return substeps - (substeps - 1) / interval * interval;
    }
    return (step + 1) % interval == 0 ? interval : 0;
}

Vec3 slidPinPosition(const Vec3& start, const Vec3& target, int step, int substeps) {
    if (step + 1 == substeps) {
        return target;
    }
    const float share = static_cast<float>(step + 1) / static_cast<float>(substeps);
    return start + (target - start) * share;
}

ClothTickCommands clothTickCommands(const Cloth& cloth, float seconds, const std::vector<Shape>& colliders, const ClothFields& fields) {
    ClothTickCommands commands;
    commands.settings = cloth.settings;
    commands.seconds = seconds;
    for (const ClothPin& pin : cloth.pins) {
        commands.pinTargets.push_back(pin.position);
    }
    commands.grab = cloth.grab;
    commands.colliders = colliders;
    commands.fields = fields;
    return commands;
}

namespace {
    // The coarse levels and tethers, anchored by whatever is pinned now.
    void rebuildAnchoring(Cloth& cloth, JobSystem& jobs) {
        cloth.hierarchy = buildClothHierarchy(cloth.restPositions, cloth.stretch, cloth.particles.inverseMasses);
        cloth.tethers = clothTethers(cloth.restPositions, cloth.particles.inverseMasses, cloth.pieces, jobs);
    }
}

void pinClothPoint(Cloth& cloth, std::uint32_t particle) {
    pinParticle(cloth.particles, particle);
    rebuildAnchoring(cloth, serialJobs());
}

void addClothPin(Cloth& cloth, std::uint32_t particle) {
    pinClothPoint(cloth, particle);
    cloth.pins.push_back(ClothPin{particle, cloth.particles.positions[particle]});
}

void addClothPins(Cloth& cloth, const std::vector<std::uint32_t>& particles, JobSystem& jobs) {
    for (const std::uint32_t particle : particles) {
        pinParticle(cloth.particles, particle);
        cloth.pins.push_back(ClothPin{particle, cloth.particles.positions[particle]});
    }
    rebuildAnchoring(cloth, jobs);
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
        const ClothFields& fields;
        const std::vector<ClothTether>& tethers;
        const std::vector<ClothStitch>& stitches;

        // Vertex block descent's per-point lists and colours and its coarse levels, or none (XPBD alone).
        const ClothPointConstraints* pointConstraints;
        const ClothVertexColours* vertexColours;
        const ClothHierarchy* hierarchy;
    };

    // The box round the cloth's points, grown by its thickness.
    Aabb clothReach(const ClothParticles& particles, float thickness) {
        Aabb around{particles.positions.front(), particles.positions.front()};
        for (const Vec3& position : particles.positions) {
            around = enclosing(around, Aabb{position, position});
        }
        const Vec3 margin{thickness, thickness, thickness};
        return Aabb{around.minimum - margin, around.maximum + margin};
    }

    // The shapes whose boxes meet the box round the cloth, grown by its thickness: most bodies are nowhere near it.
    std::vector<Shape> nearbyShapes(const ClothParticles& particles, const std::vector<Shape>& shapes, float thickness) {
        if (shapes.empty() || particles.size() == 0) {
            return {};
        }

        const Aabb around = clothReach(particles, thickness);
        std::vector<Shape> nearby;
        for (const Shape& shape : shapes) {
            if (overlaps(around, boundsOf(shape))) {
                nearby.push_back(shape);
            }
        }
        return nearby;
    }

    // The same for field bodies, by their fields' boxes.
    ClothFields nearbyFields(const ClothParticles& particles, const ClothFields& fields, float thickness) {
        if (fields.empty() || particles.size() == 0) {
            return {};
        }

        const Aabb around = clothReach(particles, thickness);
        ClothFields nearby;
        for (const std::shared_ptr<const SignedDistanceField>& field : fields) {
            if (overlaps(around, field->bounds())) {
                nearby.push_back(field);
            }
        }
        return nearby;
    }

    /*
    * Each substep: predict; slide the moving pins a share of the way to where they are going (the last substep exactly
    * there); then the solver: XPBD's stretch, bending (on its bending substeps) and hand, or vertex block descent's sweep, with its coarse levels
    * and a second sweep after them when there are any (smoothing on both sides of the coarse step, as multigrid does:
    * the coarse step's spreading leaves fine wrinkles a sweep removes); the tethers, when on, after either solver, so the
    * pins' strings have the last word on stretch; the stitches, closing their seams; the bodies last, so nothing is
    * left inside them, without friction for the points of seams still being sewn; velocities from the motion; drag on
    * the free points.
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

        // Vertex block descent's work arrays, kept for the whole tick, and what its energy is made of.
        const bool blockDescent = settings.method == ClothMethod::VertexBlockDescent && constraints.pointConstraints != nullptr;
        const bool coarseLevels = blockDescent && constraints.hierarchy != nullptr && settings.hierarchyLevels > 0 && !constraints.hierarchy->levels.empty();
        ClothHierarchyScratch scratch;
        std::vector<std::uint8_t> sewing;

        for (int step = 0; step < settings.substeps; ++step) {
            predictCloth(particles, gravity, substep);

            for (std::size_t index = 0; index < constraints.pins.size(); ++index) {
                const ClothPin& pin = constraints.pins[index];
                particles.positions[pin.particle] = slidPinPosition(pinStarts[index], pin.position, step, settings.substeps);
            }

            if (blockDescent) {
                const ClothEnergyTerms energyTerms{constraints.stretch,       constraints.bend,         *constraints.pointConstraints,
                                                   settings.stretchCompliance, settings.bendingStiffness, settings.bendingModel,
                                                   constraints.grab,          settings.grabCompliance};
                scratch.predicted = particles.positions;
                solveVertexBlocks(particles, scratch.predicted, *constraints.vertexColours, energyTerms, substep, jobs);
                if (coarseLevels) {
                    const std::vector<Shape> bodies = constraints.colliders.empty() ? std::vector<Shape>{}
                                                                                    : nearbyShapes(particles, constraints.colliders, settings.thickness);
                    solveClothHierarchy(particles, *constraints.hierarchy, settings.hierarchyLevels, energyTerms, bodies, settings.thickness, substep,
                                        scratch, jobs, nearbyFields(particles, constraints.fields, settings.thickness));
                    solveVertexBlocks(particles, scratch.predicted, *constraints.vertexColours, energyTerms, substep, jobs);
                }
            } else {
                if (constraints.stretchColours.empty()) {
                    solveStretch(particles, constraints.stretch, settings.stretchCompliance, substep);
                } else {
                    solveStretchColours(particles, constraints.stretch, constraints.stretchColours, settings.stretchCompliance, substep, jobs);
                }
                const int bendingCovered = bendingSubsteps(step, settings.substeps, settings.bendingInterval);
                if (bendingCovered > 0) {
                    if (constraints.bendColours.empty()) {
                        solveBend(particles, constraints.bend, settings.bendingStiffness, substep, settings.bendingModel, bendingCovered);
                    } else {
                        solveBendColours(particles, constraints.bend, constraints.bendColours, settings.bendingStiffness, substep,
                                         settings.bendingModel, jobs, bendingCovered);
                    }
                }
                if (constraints.grab) {
                    solveGrab(particles, *constraints.grab, settings.grabCompliance, substep);
                }
            }
            if (settings.tethers) {
                solveTethers(particles, constraints.tethers, settings.tetherSlack, jobs);
            }
            if (!constraints.stitches.empty()) {
                markSewing(particles, constraints.stitches, settings.seamSpeed * substep, sewing);
                solveStitches(particles, constraints.stitches, settings.seamSpeed * substep);
            }
            if (!constraints.colliders.empty() || !constraints.fields.empty()) {
                solveContacts(particles, nearbyShapes(particles, constraints.colliders, settings.thickness), settings,
                              nearbyFields(particles, constraints.fields, settings.thickness), sewing);
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
    const std::vector<ClothTether> noTethers;
    const std::vector<ClothStitch> noStitches;
    const ClothFields noFields;
    stepTick(particles,
             TickConstraints{stretch, noBend, noColours, noColours, noPins, grab, colliders, noFields, noTethers, noStitches, nullptr, nullptr, nullptr},
             settings, seconds, serialJobs());
}

void stepCloth(Cloth& cloth, float seconds, const std::vector<Shape>& colliders, JobSystem& jobs, const ClothFields& fields) {
    stepTick(cloth.particles,
             TickConstraints{cloth.stretch, cloth.bend, cloth.stretchColours, cloth.bendColours, cloth.pins, cloth.grab, colliders, fields, cloth.tethers,
                             cloth.stitches, &cloth.pointConstraints, &cloth.vertexColours, &cloth.hierarchy},
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
void stepCloths(World& world, float seconds, JobSystem& jobs, ClothSolver solver) {
    const std::vector<Shape> colliders = worldColliderShapes(world);
    const ClothFields fields = worldDistanceFields(world);
    if (solver == ClothSolver::Gpu) {
        world.each<Cloth>([&](Entity, Cloth& cloth) {
            cloth.onGpu = true;
            cloth.gpuTicks.push_back(clothTickCommands(cloth, seconds, colliders, fields));
        });
        return;
    }

    std::vector<Cloth*> cloths;
    std::vector<DeformingMesh*> meshes;
    world.each<Cloth>([&](Entity entity, Cloth& cloth) {
        cloth.onGpu = false;
        cloth.gpuTicks.clear();
        cloths.push_back(&cloth);
        meshes.push_back(world.tryGet<DeformingMesh>(entity));
    });

    jobs.parallelFor(cloths.size(), 1, [&](std::size_t index) {
        stepCloth(*cloths[index], seconds, colliders, jobs, fields);
        if (meshes[index] != nullptr) {
            meshes[index]->positions = cloths[index]->particles.positions;
        }
    });
}
