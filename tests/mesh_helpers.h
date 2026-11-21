#pragma once

#include "scene/indexed_mesh.h"
#include "scene/mesh.h"

#include <cmath>

// A sheet 1 m square of 40 x 40 squares, rippled so there is shape to keep: as a scanned leaf or ground is, a curved
// surface with an open border.
inline IndexedMesh rippledSheet() {
    IndexedMesh sheet = buildIndexedMesh(Mesh::grid(40, 40, 1.0f, 1.0f));

    for (MeshVertex& vertex : sheet.vertices) {
        vertex.position.z = 0.05f * std::sin(6.0f * vertex.position.x) * std::cos(4.0f * vertex.position.y);
    }

    return sheet;
}
