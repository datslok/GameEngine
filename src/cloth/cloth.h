#pragma once

#include "cloth/cloth_particles.h"
#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "math/mat4.h"
#include "scene/mesh.h"

/*
* A fabric and the world it hangs in, in real units. Mass per area tells fabrics apart: silk about 0.05-0.08 kg/m2, a
* light cotton 0.2, denim 0.3-0.4.
*/
struct ClothSettings {
    float areaDensity = 0.2f; // kg per square metre; read when the cloth is made
    float gravity = 9.81f;    // downward acceleration, m/s2
};

void visit(ClothSettings& settings, SettingsVisitor& visitor);

/*
* Component: a piece of cloth, simulated in world space (its entity's Transform is not used). With a DeformingMesh on the
* same entity, drawn from a mesh at the identity, each tick hands the mesh its points, point i being the mesh's vertex i.
*/
struct Cloth {
    ClothSettings settings;
    ClothParticles particles;
};

// A cloth from a mesh's vertices, placed in the world by placement, at rest, with the settings' fabric.
Cloth makeCloth(const Mesh& mesh, const Mat4& placement, const ClothSettings& settings);

// One tick for every cloth: predict under gravity, (constraints, later), and finish the step; then hand each cloth's
// points to its DeformingMesh, if it has one.
void stepCloths(World& world, float seconds);
