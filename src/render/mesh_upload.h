#pragma once

#include "scene/indexed_mesh.h"

#include <cstdint>

/*
* Throws unless a mesh can be uploaded as it is: vertices and whole triangles of indices, every index naming a vertex
* (an index past the end would make the GPU read outside the vertex buffer), and the vertex and index data together
* small enough to count in 32 bits.
*/
void checkMeshUpload(const IndexedMesh& mesh);

// The vertex and index data's sizes in bytes, once checkMeshUpload has passed.
std::uint32_t vertexBytes(const IndexedMesh& mesh);
std::uint32_t indexBytes(const IndexedMesh& mesh);
