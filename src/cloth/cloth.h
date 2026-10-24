#pragma once

#include "cloth/cloth_constraints.h"
#include "cloth/cloth_particles.h"
#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "math/mat4.h"
#include "scene/mesh.h"

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
*/
struct ClothSettings {
    float areaDensity = 0.2f;          // kg per square metre; read when the cloth is made
    float gravity = 9.81f;             // downward acceleration, m/s2
    float stretchCompliance = 0.0005f; // m/N
    int substeps = 15;                 // per tick: 1.8 kHz at 120 ticks a second
    float drag = 0.5f;                 // per second
};

void visit(ClothSettings& settings, SettingsVisitor& visitor);

/*
* Component: a piece of cloth, simulated in world space (its entity's Transform is not used). With a DeformingMesh on the
* same entity, drawn from a mesh at the identity, each tick hands the mesh its points, point i being the mesh's vertex i.
*/
struct Cloth {
    ClothSettings settings;
    ClothParticles particles;
    std::vector<StretchConstraint> stretch;
};

// A cloth from a mesh's vertices, placed in the world by placement, at rest, with the settings' fabric and an edge
// constraint along every edge of the placed mesh.
Cloth makeCloth(const Mesh& mesh, const Mat4& placement, const ClothSettings& settings);

/*
* One tick of points held by stretch constraints, in settings.substeps substeps: each predicts under gravity, makes one
* pass over the constraints, works out the velocities from the motion and applies the drag. Throws
* std::invalid_argument for fewer than one substep.
*/
void stepClothParticles(ClothParticles& particles, const std::vector<StretchConstraint>& stretch, const ClothSettings& settings, float seconds);

// One tick of a cloth.
void stepCloth(Cloth& cloth, float seconds);

// One tick for every cloth, then each cloth's points handed to its DeformingMesh, if it has one.
void stepCloths(World& world, float seconds);
