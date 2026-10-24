#pragma once

#include "cloth/cloth_collision.h"
#include "cloth/cloth_constraints.h"
#include "cloth/cloth_grab.h"
#include "cloth/cloth_particles.h"
#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "math/mat4.h"
#include "scene/mesh.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

/*
* A fabric and the world it hangs in, in real units. Mass per area tells fabrics apart: silk about 0.05-0.08 kg/m2, a
* light cotton 0.2, denim 0.3-0.4.
*
* Stretch compliance is 1 / the fabric's tensile stiffness (Young's modulus times thickness, about 2000 N/m for a woven
* cotton), the same for an edge of any length on a square mesh, so it does not change with the mesh's resolution: a
* hanging sheet stretches about 0.15%, as real cotton barely does.
*
* Each tick is split into substeps, one pass over the constraints each (Macklin et al. 2019, "Small Steps in Physics
* Simulation"): for the same work, smaller steps leave far less stretch than more passes over one big step.
*
* Drag stands in for the air, which slows cloth a lot for its weight: every velocity loses this share per second, a
* little each substep. Picked by eye; air that pushes each triangle by how it faces could come later.
*
* A hand holding the cloth (ClothGrab) is a stiff spring: a tug of a few newtons stretches it a few millimetres.
*
* Bending stiffness is the fabric's bending rigidity B, as the cantilever test measures it: silk about 1-5 micro N m,
* cotton shirting about 10, denim 50-200. Its bending length (B / (areaDensity g))^(1/3), how far a strip sticks out
* before it droops, is 1.7 cm for the default cotton, near real shirting's 2-3 cm. 0 bends freely.
*
* Against bodies the cloth keeps a thickness: real fabric is about a millimetre thick, but a small gap lets it rest on
* a body instead of flickering half inside it. Friction is cotton's on a smooth surface.
*/
struct ClothSettings {
    float areaDensity = 0.2f;          // kg per square metre; read when the cloth is made
    float gravity = 9.81f;             // downward acceleration, m/s2
    float stretchCompliance = 0.0005f; // m/N
    int substeps = 15;                 // per tick: 1.8 kHz at 120 ticks a second
    float drag = 0.5f;                 // per second
    float grabCompliance = 0.001f;     // m/N
    float bendingStiffness = 1e-5f;    // N m
    float thickness = 0.005f;          // m
    float staticFriction = 0.6f;
    float kineticFriction = 0.4f;
};

void visit(ClothSettings& settings, SettingsVisitor& visitor);

/*
* Component: a piece of cloth, simulated in world space (its entity's Transform is not used). With a DeformingMesh on the
* same entity, drawn from a mesh at the identity, each tick hands the mesh its points, point i being the mesh's vertex i.
*/
/*
* A pin the game can move (a hook on a rail, a hanger in a hand): its point is infinitely heavy, and each tick slides it
* evenly through the substeps from where it was to position, so it moves without a jolt and its speed is its motion.
*/
struct ClothPin {
    std::uint32_t particle = 0;
    Vec3 position;
};

struct Cloth {
    ClothSettings settings;
    ClothParticles particles;
    std::vector<StretchConstraint> stretch;
    std::vector<BendConstraint> bend;

    // Pins the game moves, by setting their positions from its commands before a tick.
    std::vector<ClothPin> pins;

    // The mesh's triangles, for finding the cloth with a ray.
    std::vector<std::array<std::uint32_t, 3>> triangles;

    // A hand holding one point, set by the game from its commands (never from input inside a tick).
    std::optional<ClothGrab> grab;
};

// A cloth from a mesh's vertices, placed in the world by placement, at rest, with the settings' fabric, a stretch
// constraint along every edge and a bending constraint on every inside edge of the placed mesh.
Cloth makeCloth(const Mesh& mesh, const Mat4& placement, const ClothSettings& settings);

// Pin a point where it is, as a pin the game can move. Throws std::out_of_range for a point the cloth does not have.
void addClothPin(Cloth& cloth, std::uint32_t particle);

/*
* One tick of points held by stretch constraints (and a hand, if one holds a point), in settings.substeps substeps: each
* predicts under gravity, makes one pass over the constraints, the hand last, works out the velocities from the motion
* and applies the drag. Throws std::invalid_argument for fewer than one substep. stepCloth adds the bending and the pins.
*/
void stepClothParticles(ClothParticles& particles, const std::vector<StretchConstraint>& stretch, const ClothSettings& settings, float seconds,
                        const std::optional<ClothGrab>& grab = std::nullopt, const std::vector<Shape>& colliders = {});

// One tick of a cloth, colliding with the given still bodies' shapes (in the world).
void stepCloth(Cloth& cloth, float seconds, const std::vector<Shape>& colliders = {});

// One tick for every cloth against every Collider in the world (worldColliderShapes), then each cloth's points handed
// to its DeformingMesh, if it has one.
void stepCloths(World& world, float seconds);
