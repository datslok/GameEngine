#pragma once

#include "scene/indexed_mesh.h"
#include "scene/mesh.h"

#include <cmath>

// A sheet 1 m square of squares x squares (40 by default), rippled so there is shape to keep: as a scanned leaf or
// ground is, a curved surface with an open border.
inline IndexedMesh rippledSheet(int squares = 40) {
    IndexedMesh sheet = buildIndexedMesh(Mesh::grid(squares, squares, 1.0f, 1.0f));

    for (MeshVertex& vertex : sheet.vertices) {
        vertex.position.z = 0.05f * std::sin(6.0f * vertex.position.x) * std::cos(4.0f * vertex.position.y);
    }

    return sheet;
}
