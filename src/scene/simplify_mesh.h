#pragma once

#include "scene/indexed_mesh.h"

/*
* Fewer triangles for the same shape (meshoptimizer's simplifier, external/meshoptimizer): edges collapse onto the
* mesh's own vertices, cheapest first by the error they add (a quadric of the surfaces round each vertex), until
* keepShare of the triangles are left or the next collapse would move the surface more than maxError, a share of the
* mesh's size. With lockBorder, vertices on an open edge never move, so a cut-out leaf's outline and a seam keep their
* place (the seen mesh); without it the border goes too (a shadow's stand-in). The vertices no triangle uses any more are
* dropped. For scanned meshes (the forest's understorey) and, later, level of detail.
*/
IndexedMesh simplifyMesh(const IndexedMesh& mesh, float keepShare, float maxError, bool lockBorder);
