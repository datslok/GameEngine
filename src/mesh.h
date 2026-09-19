#pragma once

#include "vec4.h"
#include <cstddef>
#include <vector>

struct Edge{
    std::size_t start;
    std::size_t end;
};

struct Triangle{
    std::size_t first;
    std::size_t second;
    std::size_t third;
};

struct Mesh{
    std::vector<Vec4> vertices;
    std::vector<Edge> edges;
    std::vector<Triangle> triangles;

    static Mesh cube();
};