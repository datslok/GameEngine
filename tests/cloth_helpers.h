#pragma once

#include "cloth/cloth.h"

#include <algorithm>
#include <cmath>

/*
* The default fabric with the tethers off: for tests that measure a solver itself (XPBD, vertex block descent, the
* coarse levels), whose stretch the tethers would otherwise hold at their slack whatever the solver does.
*/
inline ClothSettings untetheredSettings() {
    ClothSettings settings;
    settings.tethers = false;
    return settings;
}

/*
* Two level sheets in one mesh, so one cloth: each squares x squares squares of size metres a side, in the x-z plane
* centred on the y axis, the lower at y = 0 and the upper gap metres above it (vertices of the lower first). For
* self-collision, which acts within one cloth.
*/
inline Mesh stackedSheets(int squares, float size, float gap) {
    const Mesh sheet = Mesh::grid(squares, squares, size, size);
    Mesh stacked;
    for (int layer = 0; layer < 2; ++layer) {
        const std::size_t offset = stacked.vertices.size();
        for (const Vec4& vertex : sheet.vertices) {
            stacked.vertices.push_back(Vec4{vertex.x, gap * static_cast<float>(layer), -vertex.y, 1.0f});
        }
        for (Triangle triangle : sheet.triangles) {
            triangle.first += offset;
            triangle.second += offset;
            triangle.third += offset;
            stacked.triangles.push_back(triangle);
        }
        for (Edge edge : sheet.edges) {
            edge.start += offset;
            edge.end += offset;
            stacked.edges.push_back(edge);
        }
    }
    return stacked;
}

// The smallest corner of any of a flat mesh's triangles (in its x-y plane, as a pattern is cut), in degrees.
inline double smallestAngleDegrees(const Mesh& mesh) {
    double smallest = 180.0;
    for (const Triangle& triangle : mesh.triangles) {
        const std::size_t corners[3]{triangle.first, triangle.second, triangle.third};
        for (int corner = 0; corner < 3; ++corner) {
            const Vec4& at = mesh.vertices[corners[corner]];
            const Vec4& next = mesh.vertices[corners[(corner + 1) % 3]];
            const Vec4& last = mesh.vertices[corners[(corner + 2) % 3]];
            const Vec2 toNext{next.x - at.x, next.y - at.y};
            const Vec2 toLast{last.x - at.x, last.y - at.y};
            const double cosine = toNext.dot(toLast) / (static_cast<double>(toNext.length()) * toLast.length());
            smallest = std::min(smallest, std::acos(std::clamp(cosine, -1.0, 1.0)) * 180.0 / 3.14159265358979323846);
        }
    }
    return smallest;
}
