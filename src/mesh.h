#pragma once

#include "vec4.h"
#include <cstddef>
#include <vector>

/*
* Represents a connection between two vertices so the mesh can define its edges.
*/
struct Edge{
    std::size_t start;
    std::size_t end;
};

/*
* Represents a triangle using three vertex indices so the mesh can define its surfaces.
*/
struct Triangle{
    std::size_t first;
    std::size_t second;
    std::size_t third;
};

/*
* Stores the vertices, edges, and triangles that make up a 3D object so it can be represented and rendered by the engine.
*/
struct Mesh{
    std::vector<Vec4> vertices;
    std::vector<Edge> edges;
    std::vector<Triangle> triangles;

    static Mesh cube();
};