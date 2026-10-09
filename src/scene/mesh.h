#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"

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
    static Mesh plane(float halfSize = 20.0f);

    // A unit sphere with smooth normals: segments around the equator, rings from pole to pole.
    static Mesh sphere(int segments = 32, int rings = 16);
};

// The unit normal of the triangle first, second, third (counterclockwise seen from the front), or zero for a triangle with no area.
Vec3 faceNormal(const Vec4& first, const Vec4& second, const Vec4& third);

