#pragma once

#include "scene/indexed_mesh.h"
#include "scene/mesh.h"

#include <span>

/*
* Write a tangent into every corner (three per triangle, in order, not shared) with MikkTSpace, the algorithm baking
* tools use, so a normal map is read in exactly the frame it was baked in. Positions, normals and UVs must already be
* set. The bitangent points up the picture whichever way the mesh's UVs count v (origin). A mesh without UVs gets any
* unit tangent across each normal, with sign +1; corners whose UVs are squashed to a line get one across their normal too.
* Throws if MikkTSpace cannot allocate its working memory.
*/
void generateTangents(std::span<MeshVertex> corners, UvOrigin origin);
