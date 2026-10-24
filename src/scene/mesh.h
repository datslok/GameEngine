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
    // One optional tangent per corner, as a file supplies it (xyz: the direction of increasing u; w: +1 or -1, the side
    // the bitangent is on, see MeshVertex). Empty means: generate one (scene/tangents.h).
    std::array<std::optional<Vec4>, 3> tangents{};
};

// Where UV (0, 0) is in the image: OBJ counts v up from the bottom row, glTF down from the top row. Tangents point the
// bitangent up the picture, so generating them needs to know which way v runs.
enum class UvOrigin {
    BottomLeft,
    TopLeft
};

struct Mesh {
    std::vector<Vec4> vertices;
    std::vector<Edge> edges;
    std::vector<Triangle> triangles;
    UvOrigin uvOrigin = UvOrigin::BottomLeft;

    static Mesh cube();
    static Mesh plane(float halfSize = 20.0f);

    // A unit sphere with smooth normals: segments around the equator, rings from pole to pole.
    static Mesh sphere(int segments = 32, int rings = 16);

    /*
    * A flat sheet of columns x rows squares, width by height, centred on the origin and facing +z, two triangles a
    * square: a cloth's rest shape. Vertex row * (columns + 1) + column, rows counted up from the bottom, UV (0, 0) at
    * the bottom-left and (1, 1) at the top-right. Throws std::invalid_argument for no squares or a size that is not
    * finite and positive.
    */
    static Mesh grid(int columns, int rows, float width, float height);
};

// The unit normal of the triangle first, second, third (counterclockwise seen from the front), or zero for a triangle with no area.
Vec3 faceNormal(const Vec4& first, const Vec4& second, const Vec4& third);

