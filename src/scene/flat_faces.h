#pragma once

#include "math/aabb.h"
#include "math/vec3.h"
#include "scene/mesh.h"

#include <vector>

/*
* A mesh's flat faces, found once when it is loaded, so the renderer can tell which surfaces could be planar mirrors with
* nothing marked by hand. A face is the triangles lying in one plane, facing one way, that hold a fair share of the mesh's
* area: a box has six, a sheet one, a sphere none. Measured in the mesh's own space, with the normal out of the surface.
*/
struct FlatFace {
    Vec3 normal;
    Aabb bounds;
};

// A mesh's flat faces, none if it has more distinct planes than could be a flat object's.
std::vector<FlatFace> findFlatFaces(const Mesh& mesh);
