#pragma once

#include "math/mat4.h"
#include "physics/triangle_mesh.h"
#include "scene/mesh.h"

/*
* The collision copy of a mesh: its corner positions moved by transform (usually the object's model matrix, so static
* level geometry is placed in the world once) and its triangles as index triples.
* Throws std::invalid_argument when a triangle refers to a corner the mesh does not have.
*/
TriangleMesh collisionMesh(const Mesh& mesh, const Mat4& transform = Mat4::identity());

// Adds the mesh's triangles to an existing collision mesh (moved by transform), so a model of several parts becomes one mesh.
void appendCollisionMesh(TriangleMesh& into, const Mesh& mesh, const Mat4& transform);
