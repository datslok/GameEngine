#include "mesh.h"

#include <cmath>
#include <stdexcept>

/*
* Create a cube mesh with vertices, edges, and triangles so it can be represented and rendered as a complete 3D object.
*/
Mesh Mesh::cube() {
    Mesh mesh;

    mesh.vertices = {
        Vec4{-1.0f, -1.0f, -1.0f, 1.0f},
        Vec4{ 1.0f, -1.0f, -1.0f, 1.0f},
        Vec4{ 1.0f,  1.0f, -1.0f, 1.0f},
        Vec4{-1.0f,  1.0f, -1.0f, 1.0f},

        Vec4{-1.0f, -1.0f,  1.0f, 1.0f},
        Vec4{ 1.0f, -1.0f,  1.0f, 1.0f},
        Vec4{ 1.0f,  1.0f,  1.0f, 1.0f},
        Vec4{-1.0f,  1.0f,  1.0f, 1.0f}
    };

    mesh.edges = {
        Edge{0, 1}, Edge{1, 2}, Edge{2, 3}, Edge{3, 0},
        Edge{4, 5}, Edge{5, 6}, Edge{6, 7}, Edge{7, 4},
        Edge{0, 4}, Edge{1, 5}, Edge{2, 6}, Edge{3, 7}
    };

    // Order the vertices so the triangle winding produces outward-facing normals.
    mesh.triangles = {
        // Negative Z face.
        Triangle{0, 3, 2},
        Triangle{0, 2, 1},

        // Positive Z face.
        Triangle{4, 5, 6},
        Triangle{4, 6, 7},

        // Negative X face.
        Triangle{0, 4, 7},
        Triangle{0, 7, 3},

        // Positive X face.
        Triangle{1, 2, 6},
        Triangle{1, 6, 5},

        // Positive Y face.
        Triangle{3, 7, 6},
        Triangle{3, 6, 2},

        // Negative Y face.
        Triangle{0, 1, 5},
        Triangle{0, 5, 4}
    };
    
    // Each face consists of two consecutive triangles.
    // Map a complete texture square onto each face.
    for (std::size_t index = 0; index < mesh.triangles.size(); index += 2) {
        Triangle& first = mesh.triangles[index];
        Triangle& second = mesh.triangles[index + 1];

        first.uvs[0] = Vec2{0.0f, 0.0f};
        first.uvs[1] = Vec2{1.0f, 0.0f};
        first.uvs[2] = Vec2{1.0f, 1.0f};

        second.uvs[0] = Vec2{0.0f, 0.0f};
        second.uvs[1] = Vec2{1.0f, 1.0f};
        second.uvs[2] = Vec2{0.0f, 1.0f};
    }
    return mesh;
}

Mesh Mesh::plane(float halfSize) {
    if (!std::isfinite(halfSize) || halfSize <= 0.0f) {
        throw std::invalid_argument(
            "Plane half-size must be finite and positive"
        );
    }

    Mesh mesh;

    mesh.vertices = {
        Vec4{-halfSize, 0.0f, -halfSize, 1.0f},
        Vec4{-halfSize, 0.0f,  halfSize, 1.0f},
        Vec4{ halfSize, 0.0f,  halfSize, 1.0f},
        Vec4{ halfSize, 0.0f, -halfSize, 1.0f}
    };

    // Counter-clockwise when viewed from above.
    Triangle first{};
    first.first = 0;
    first.second = 1;
    first.third = 2;

    Triangle second{};
    second.first = 0;
    second.second = 2;
    second.third = 3;

    for (std::size_t corner = 0; corner < 3; ++corner) {
        first.normals[corner] = Vec3{0.0f, 1.0f, 0.0f};
        second.normals[corner] = Vec3{0.0f, 1.0f, 0.0f};
    }

    first.uvs = {
        Vec2{0.0f, 0.0f},
        Vec2{0.0f, 1.0f},
        Vec2{1.0f, 1.0f}
    };

    second.uvs = {
        Vec2{0.0f, 0.0f},
        Vec2{1.0f, 1.0f},
        Vec2{1.0f, 0.0f}
    };

    mesh.triangles = {first, second};

    mesh.edges = {
        Edge{0, 1},
        Edge{1, 2},
        Edge{2, 3},
        Edge{3, 0}
    };

    return mesh;
}