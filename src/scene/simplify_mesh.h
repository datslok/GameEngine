#pragma once

#include "scene/indexed_mesh.h"

#include <initializer_list>
#include <vector>

/*
* Fewer triangles for the same shape (meshoptimizer's simplifier, external/meshoptimizer): edges collapse onto the
* mesh's own vertices, cheapest first by the error they add (a quadric of the surfaces round each vertex), until
* keepShare of the triangles are left or the next collapse would move the surface more than maxError, a share of the
* mesh's size. With lockBorder, vertices on an open edge never move, so a cut-out leaf's outline and a seam keep their
* place (the seen mesh); without it the border goes too (a shadow's stand-in). The vertices no triangle uses any more are
* dropped (their sway bindings with them, the rest kept). For scanned meshes (the forest's understorey) and level of detail: errorMetres, when given, gets how far the
* surface moved (meshoptimizer's error times the mesh's size), the level's error to choose it by (scene/level_of_detail.h).
*/
IndexedMesh simplifyMesh(const IndexedMesh& mesh, float keepShare, float maxError, bool lockBorder, float* errorMetres = nullptr);

// One coarser level of a mesh and how far it strays (metres), as simplifiedLevels makes them.
struct SimplifiedLevel {
    IndexedMesh mesh;
    float errorMetres = 0.0f;
};

/*
* A generated shape's levels of detail (docs/research/LOD.md): keepShares of its triangles in turn, coarsest last, borders free
* (a closed shape has none, and a far level's outline may move), each within maxError (a share of the mesh's size) and
* simplified from the mesh itself; the errors made rising, as the asset manager wants them.
*/
std::vector<SimplifiedLevel> simplifiedLevels(const IndexedMesh& mesh, std::initializer_list<float> keepShares, float maxError);
