#include "scene/mesh.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

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

/*
* Between two profile points each segment is a quad, lower ring a, b (b a step further round) and upper ring c, d above
* them, split into a, b, c and a, c, d, anticlockwise seen from outside; a band whose upper or lower ring sits on the axis
* keeps only the triangle with area. A band's outward normal in the (radius, height) plane is its direction turned a
* quarter clockwise, (dy, -dr); a point's is the mean of its bands' (one band at the ends). The caps' triangles fan from
* their centres, turned so the bottom faces down and the top up.
*/
Mesh Mesh::lathe(const std::vector<Vec2>& profile, int segments) {
    if (profile.size() < 2 || segments < 3) {
        throw std::invalid_argument("A lathe needs at least 2 profile points and 3 segments");
    }

    for (const Vec2& point : profile) {
        if (!(point.x >= 0.0f)) {
            throw std::invalid_argument("A lathe's profile radii must not be negative");
        }
    }

    const float pi = std::numbers::pi_v<float>;
    const std::size_t points = profile.size();
    const std::size_t around = static_cast<std::size_t>(segments);
    const float bottom = profile.front().y;
    const float height = profile.back().y - bottom;
    Mesh mesh;

    const auto angleOf = [&](std::size_t segment) { return 2.0f * pi * static_cast<float>(segment) / static_cast<float>(segments); };

    for (const Vec2& point : profile) {
        for (std::size_t segment = 0; segment < around; ++segment) {
            const float angle = angleOf(segment);
            mesh.vertices.push_back(Vec4{point.x * std::sin(angle), point.y, point.x * std::cos(angle), 1.0f});
        }
    }

    const std::size_t bottomCentre = mesh.vertices.size();
    mesh.vertices.push_back(Vec4{0.0f, profile.front().y, 0.0f, 1.0f});
    const std::size_t topCentre = mesh.vertices.size();
    mesh.vertices.push_back(Vec4{0.0f, profile.back().y, 0.0f, 1.0f});

    // Each band's outward normal in the profile's plane, then each point's.
    std::vector<Vec2> bandNormals;

    for (std::size_t band = 0; band + 1 < points; ++band) {
        const Vec2 along = profile[band + 1] - profile[band];
        const float length = std::sqrt(along.x * along.x + along.y * along.y);
        bandNormals.push_back(length > 0.0f ? Vec2{along.y / length, -along.x / length} : Vec2{1.0f, 0.0f});
    }

    std::vector<Vec2> pointNormals;

    for (std::size_t point = 0; point < points; ++point) {
        Vec2 sum{0.0f, 0.0f};

        if (point > 0) {
            sum = sum + bandNormals[point - 1];
        }

        if (point + 1 < points) {
            sum = sum + bandNormals[point];
        }

        const float length = std::sqrt(sum.x * sum.x + sum.y * sum.y);
        pointNormals.push_back(length > 0.0f ? Vec2{sum.x / length, sum.y / length} : bandNormals[std::min(point, bandNormals.size() - 1)]);
    }

    const auto ringVertex = [&](std::size_t point, std::size_t segment) { return point * around + segment % around; };

    // A side corner: its normal turned to its angle, u round the circle (1 at the seam's far side), v up the height.
    const auto sideCorner = [&](Triangle& triangle, int corner, std::size_t point, std::size_t segment) {
        const float angle = angleOf(segment);
        const Vec2 normal = pointNormals[point];
        triangle.normals[static_cast<std::size_t>(corner)] = Vec3{normal.x * std::sin(angle), normal.y, normal.x * std::cos(angle)};
        triangle.uvs[static_cast<std::size_t>(corner)] =
            Vec2{static_cast<float>(segment) / static_cast<float>(segments), height > 0.0f ? (profile[point].y - bottom) / height : 0.0f};
    };

    // A cap corner: the cap's flat normal, u and v across the cap's disc.
    const auto capCorner = [&](Triangle& triangle, int corner, std::size_t vertex, float normalY) {
        const Vec4& position = mesh.vertices[vertex];
        const float radius = std::max(normalY < 0.0f ? profile.front().x : profile.back().x, 1e-6f);
        triangle.normals[static_cast<std::size_t>(corner)] = Vec3{0.0f, normalY, 0.0f};
        triangle.uvs[static_cast<std::size_t>(corner)] = Vec2{0.5f + 0.5f * position.x / radius, 0.5f + 0.5f * position.z / radius};
    };

    if (profile.front().x > 0.0f) {
        for (std::size_t segment = 0; segment < around; ++segment) {
            Triangle triangle{bottomCentre, ringVertex(0, segment + 1), ringVertex(0, segment)};
            capCorner(triangle, 0, triangle.first, -1.0f);
            capCorner(triangle, 1, triangle.second, -1.0f);
            capCorner(triangle, 2, triangle.third, -1.0f);
            mesh.triangles.push_back(triangle);
        }
    }

    // One side triangle from three (profile point, segment) corners.
    using Corner = std::pair<std::size_t, std::size_t>;
    const auto addSide = [&](Corner first, Corner second, Corner third) {
        Triangle triangle{ringVertex(first.first, first.second), ringVertex(second.first, second.second), ringVertex(third.first, third.second)};
        sideCorner(triangle, 0, first.first, first.second);
        sideCorner(triangle, 1, second.first, second.second);
        sideCorner(triangle, 2, third.first, third.second);
        mesh.triangles.push_back(triangle);
    };

    for (std::size_t band = 0; band + 1 < points; ++band) {
        const bool lowerOnAxis = profile[band].x <= 0.0f;
        const bool upperOnAxis = profile[band + 1].x <= 0.0f;

        for (std::size_t segment = 0; segment < around; ++segment) {
            const Corner a{band, segment};
            const Corner b{band, segment + 1};
            const Corner c{band + 1, segment + 1};
            const Corner d{band + 1, segment};

            if (!lowerOnAxis && !upperOnAxis) {
                addSide(a, b, c);
                addSide(a, c, d);
            } else if (!lowerOnAxis) {
                addSide(a, b, d); // c and d meet on the axis
            } else if (!upperOnAxis) {
                addSide(a, c, d); // a and b meet on the axis
            }
        }
    }

    if (profile.back().x > 0.0f) {
        for (std::size_t segment = 0; segment < around; ++segment) {
            Triangle triangle{topCentre, ringVertex(points - 1, segment), ringVertex(points - 1, segment + 1)};
            capCorner(triangle, 0, triangle.first, 1.0f);
            capCorner(triangle, 1, triangle.second, 1.0f);
            capCorner(triangle, 2, triangle.third, 1.0f);
            mesh.triangles.push_back(triangle);
        }
    }

    return mesh;
}

/*
* Each square's two triangles share its diagonal from bottom-left to top-right, both anticlockwise seen from +z.
*/
Mesh Mesh::grid(int columns, int rows, float width, float height) {
    if (columns < 1 || rows < 1) {
        throw std::invalid_argument("A grid needs at least one square each way");
    }
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f) {
        throw std::invalid_argument("A grid's size must be finite and positive");
    }

    Mesh mesh;
    const auto vertexAt = [columns](int row, int column) {
        return static_cast<std::size_t>(row) * static_cast<std::size_t>(columns + 1) + static_cast<std::size_t>(column);
    };
    const auto uvAt = [columns, rows](int row, int column) {
        return Vec2{static_cast<float>(column) / static_cast<float>(columns), static_cast<float>(row) / static_cast<float>(rows)};
    };

    for (int row = 0; row <= rows; ++row) {
        for (int column = 0; column <= columns; ++column) {
            const Vec2 uv = uvAt(row, column);
            mesh.vertices.push_back(Vec4{(uv.x - 0.5f) * width, (uv.y - 0.5f) * height, 0.0f, 1.0f});
        }
    }

    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            Triangle lower{vertexAt(row, column), vertexAt(row, column + 1), vertexAt(row + 1, column + 1)};
            lower.uvs = {uvAt(row, column), uvAt(row, column + 1), uvAt(row + 1, column + 1)};
            Triangle upper{vertexAt(row, column), vertexAt(row + 1, column + 1), vertexAt(row + 1, column)};
            upper.uvs = {uvAt(row, column), uvAt(row + 1, column + 1), uvAt(row + 1, column)};
            mesh.triangles.push_back(lower);
            mesh.triangles.push_back(upper);
        }
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
