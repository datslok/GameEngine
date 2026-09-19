#include "mesh.h"

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

    // Vertices are ordered so face normals point outward.
    mesh.triangles = {
        // Negative Z face
        Triangle{0, 3, 2},
        Triangle{0, 2, 1},

        // Positive Z face
        Triangle{4, 5, 6},
        Triangle{4, 6, 7},

        // Negative X face
        Triangle{0, 4, 7},
        Triangle{0, 7, 3},

        // Positive X face
        Triangle{1, 2, 6},
        Triangle{1, 6, 5},

        // Positive Y face
        Triangle{3, 7, 6},
        Triangle{3, 6, 2},

        // Negative Y face
        Triangle{0, 1, 5},
        Triangle{0, 5, 4}
    };

    return mesh;
}