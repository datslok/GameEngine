#pragma once

#include "vec2.h"
#include "vec3.h"
#include "vec4.h"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

struct Edge {
    std::size_t start;
    std::size_t end;
};

struct Triangle {
    std::size_t first;
    std::size_t second;
    std::size_t third;

    // One optional normal for each corner, in the same order.
    // An empty optional means: use the triangle's face normal.
    std::array<std::optional<Vec3>, 3> normals{};
    // One optional texture coordinate per triangle corner.
    std::array<std::optional<Vec2>, 3> uvs{};
};

struct Mesh {
    std::vector<Vec4> vertices;
    std::vector<Edge> edges;
    std::vector<Triangle> triangles;

    static Mesh cube();
};