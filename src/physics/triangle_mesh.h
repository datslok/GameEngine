#pragma once

#include "math/vec3.h"

#include <cstdint>
#include <vector>

/*
* Triangles for collision: corner positions only, and three indices per triangle (counterclockwise seen from the front).
* Unlike the render mesh it has no normals or UVs, and it is usually stored already in world space, since level geometry
* does not move. Build one from a scene mesh with collisionMesh (scene/collision_mesh).
*/
struct TriangleMesh {
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> indices;
};
