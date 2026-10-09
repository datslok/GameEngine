#include "scene/mesh.h"

#include <cmath>
#include <numbers>
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

/*
* A unit sphere as a latitude/longitude grid: a vertex at each pole and segments vertices on each of the rings - 1 circles between.
* Rows near the poles would otherwise squash quads into slivers, so the band touching each pole is a fan of single triangles.
* Each corner's normal is simply its position, because on a unit sphere the outward direction is the point itself.
*/
Mesh Mesh::sphere(int segments, int rings) {
    if (segments < 3 || rings < 2) {
        throw std::invalid_argument("A sphere needs at least 3 segments and 2 rings");
    }

    const float pi = std::numbers::pi_v<float>;
    Mesh mesh;

    mesh.vertices.push_back(Vec4{0.0f, 1.0f, 0.0f, 1.0f});

    for (int ring = 1; ring < rings; ++ring) {
        const float polarAngle = pi * static_cast<float>(ring) / static_cast<float>(rings);
        const float circleRadius = std::sin(polarAngle);
        const float height = std::cos(polarAngle);

        for (int segment = 0; segment < segments; ++segment) {
            const float around = 2.0f * pi * static_cast<float>(segment) / static_cast<float>(segments);
            mesh.vertices.push_back(Vec4{circleRadius * std::sin(around), height, circleRadius * std::cos(around), 1.0f});
        }
    }

    mesh.vertices.push_back(Vec4{0.0f, -1.0f, 0.0f, 1.0f});

    const std::size_t northPole = 0;
    const std::size_t southPole = mesh.vertices.size() - 1;

    // Vertex index on a circle (1-based ring), wrapping around so the last segment joins the first.
    const auto onRing = [segments](int ring, int segment) {
        return static_cast<std::size_t>(1 + (ring - 1) * segments + segment % segments);
    };

    // Counterclockwise seen from outside, so face normals point away from the centre.
    const auto addTriangle = [&mesh](std::size_t a, std::size_t b, std::size_t c) {
        Triangle triangle{a, b, c};
        const std::size_t corners[3] = {a, b, c};

        for (std::size_t corner = 0; corner < 3; ++corner) {
            const Vec4& position = mesh.vertices[corners[corner]];
            triangle.normals[corner] = Vec3{position.x, position.y, position.z};
        }

        mesh.triangles.push_back(triangle);
    };

    for (int segment = 0; segment < segments; ++segment) {
        addTriangle(northPole, onRing(1, segment), onRing(1, segment + 1));

        for (int ring = 1; ring < rings - 1; ++ring) {
            addTriangle(onRing(ring, segment), onRing(ring + 1, segment), onRing(ring + 1, segment + 1));
            addTriangle(onRing(ring, segment), onRing(ring + 1, segment + 1), onRing(ring, segment + 1));
        }

        addTriangle(onRing(rings - 1, segment), southPole, onRing(rings - 1, segment + 1));
    }

    return mesh;
}

/*
* The face normal from the cross product of two edges. Doubles keep long thin triangles accurate; a degenerate triangle keeps a zero normal.
*/
Vec3 faceNormal(const Vec4& first, const Vec4& second, const Vec4& third) {
    const double edgeAX = static_cast<double>(second.x) - first.x;
    const double edgeAY = static_cast<double>(second.y) - first.y;
    const double edgeAZ = static_cast<double>(second.z) - first.z;

    const double edgeBX = static_cast<double>(third.x) - first.x;
    const double edgeBY = static_cast<double>(third.y) - first.y;
    const double edgeBZ = static_cast<double>(third.z) - first.z;

    const double normalX = edgeAY * edgeBZ - edgeAZ * edgeBY;
    const double normalY = edgeAZ * edgeBX - edgeAX * edgeBZ;
    const double normalZ = edgeAX * edgeBY - edgeAY * edgeBX;

    const double length = std::hypot(normalX, normalY, normalZ);

    if (length <= 0.0) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    return Vec3{
        static_cast<float>(normalX / length),
        static_cast<float>(normalY / length),
        static_cast<float>(normalZ / length)
    };
}
