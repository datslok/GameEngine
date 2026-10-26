#include "physics/signed_distance_field.h"

#include "math/closest_points.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace {
    // Samples along y a slab: one job's share of the grid. Cut along y, a standing body's longest side, so there are
    // enough slabs for every core (the mannequin's 365 samples tall make 46; its 79 deep would make only 10).
    constexpr int slabSamples = 8;

    // A sample no triangle has reached yet.
    constexpr float untouched = std::numeric_limits<float>::infinity();

    /*
    * Each triangle's normals for the inside-outside test (Baerentzen & Aanaes 2005): its face's, its edges' (the sum of
    * the two faces' that share each, ab, bc, ca) and its corners' (the faces' round each corner, weighted by their angle
    * there). The point's side is the sign of its offset from the nearest point along the nearest feature's normal.
    */
    struct TriangleNormals {
        Vec3 face;
        std::array<Vec3, 3> edges;
        std::array<Vec3, 3> corners;
    };

    Vec3 corner(const TriangleMesh& mesh, std::size_t triangle, int which) {
        return mesh.positions[mesh.indices[triangle * 3 + static_cast<std::size_t>(which)]];
    }

    // The angle at corner which between the triangle's two edges there, 0 for a squashed one.
    float cornerAngle(const Vec3& at, const Vec3& next, const Vec3& previous) {
        const Vec3 toNext = next - at;
        const Vec3 toPrevious = previous - at;
        const float lengths = toNext.length() * toPrevious.length();
        if (lengths == 0.0f) {
            return 0.0f;
        }
        return std::acos(std::clamp(toNext.dot(toPrevious) / lengths, -1.0f, 1.0f));
    }

    std::vector<TriangleNormals> triangleNormals(const TriangleMesh& mesh) {
        const std::size_t triangleCount = mesh.indices.size() / 3;
        std::vector<TriangleNormals> normals(triangleCount);
        std::vector<Vec3> cornerSums(mesh.positions.size(), Vec3{0.0f, 0.0f, 0.0f});
        std::map<std::pair<std::uint32_t, std::uint32_t>, Vec3> edgeSums;

        for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
            const Vec3 a = corner(mesh, triangle, 0);
            const Vec3 b = corner(mesh, triangle, 1);
            const Vec3 c = corner(mesh, triangle, 2);
            const Vec3 across = (b - a).cross(c - a);
            const float area = across.length();
            const Vec3 face = area > 0.0f ? across * (1.0f / area) : Vec3{0.0f, 0.0f, 0.0f};
            normals[triangle].face = face;

            const std::array<float, 3> angles{cornerAngle(a, b, c), cornerAngle(b, c, a), cornerAngle(c, a, b)};
            for (int which = 0; which < 3; ++which) {
                const std::uint32_t first = mesh.indices[triangle * 3 + static_cast<std::size_t>(which)];
                const std::uint32_t second = mesh.indices[triangle * 3 + static_cast<std::size_t>((which + 1) % 3)];
                cornerSums[first] = cornerSums[first] + face * angles[static_cast<std::size_t>(which)];
                Vec3& edge = edgeSums[{std::min(first, second), std::max(first, second)}];
                edge = edge + face;
            }
        }

        for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
            for (int which = 0; which < 3; ++which) {
                const std::uint32_t first = mesh.indices[triangle * 3 + static_cast<std::size_t>(which)];
                const std::uint32_t second = mesh.indices[triangle * 3 + static_cast<std::size_t>((which + 1) % 3)];
                normals[triangle].edges[static_cast<std::size_t>(which)] = edgeSums.at({std::min(first, second), std::max(first, second)});
                normals[triangle].corners[static_cast<std::size_t>(which)] = cornerSums[first];
            }
        }
        return normals;
    }

    const Vec3& featureNormal(const TriangleNormals& normals, TriangleFeature feature) {
        switch (feature) {
        case TriangleFeature::Face:
            return normals.face;
        case TriangleFeature::EdgeAB:
            return normals.edges[0];
        case TriangleFeature::EdgeBC:
            return normals.edges[1];
        case TriangleFeature::EdgeCA:
            return normals.edges[2];
        case TriangleFeature::CornerA:
            return normals.corners[0];
        case TriangleFeature::CornerB:
            return normals.corners[1];
        case TriangleFeature::CornerC:
            return normals.corners[2];
        }
        return normals.face;
    }

    // The samples' index range along one axis within reach of [low, high]: those within band of it, clamped to the grid.
    std::pair<int, int> sampleRange(float low, float high, float origin, float spacing, float band, int count) {
        const int first = static_cast<int>(std::floor((low - band - origin) / spacing));
        const int last = static_cast<int>(std::ceil((high + band - origin) / spacing));
        return {std::max(first, 0), std::min(last, count - 1)};
    }

    std::size_t sampleIndex(const SignedDistanceField& field, int x, int y, int z) {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(field.counts[1]) + static_cast<std::size_t>(y)) *
                   static_cast<std::size_t>(field.counts[0]) +
               static_cast<std::size_t>(x);
    }

    /*
    * Samples no triangle reached are a band or more from the surface. Those joined to the box's edge through other such
    * samples are outside: the band's samples, at least two bands thick, wall the inside off (6-neighbour steps cannot
    * slip through it). The rest are inside.
    */
    void signFarSamples(SignedDistanceField& field) {
        const int nx = field.counts[0];
        const int ny = field.counts[1];
        const int nz = field.counts[2];
        std::vector<std::uint8_t> reached(field.distances.size(), 0);
        std::vector<std::uint32_t> front;

        const auto visit = [&](int x, int y, int z) {
            const std::size_t index = sampleIndex(field, x, y, z);
            if (!reached[index] && field.distances[index] == untouched) {
                reached[index] = 1;
                field.distances[index] = field.band;
                front.push_back(static_cast<std::uint32_t>(index));
            }
        };

        for (int z = 0; z < nz; ++z) {
            for (int y = 0; y < ny; ++y) {
                for (int x = 0; x < nx; ++x) {
                    if (x == 0 || y == 0 || z == 0 || x == nx - 1 || y == ny - 1 || z == nz - 1) {
                        visit(x, y, z);
                    }
                }
            }
        }

        while (!front.empty()) {
            const std::uint32_t index = front.back();
            front.pop_back();
            const int x = static_cast<int>(index % static_cast<std::uint32_t>(nx));
            const int y = static_cast<int>((index / static_cast<std::uint32_t>(nx)) % static_cast<std::uint32_t>(ny));
            const int z = static_cast<int>(index / (static_cast<std::uint32_t>(nx) * static_cast<std::uint32_t>(ny)));
            if (x > 0) {
                visit(x - 1, y, z);
            }
            if (x + 1 < nx) {
                visit(x + 1, y, z);
            }
            if (y > 0) {
                visit(x, y - 1, z);
            }
            if (y + 1 < ny) {
                visit(x, y + 1, z);
            }
            if (z > 0) {
                visit(x, y, z - 1);
            }
            if (z + 1 < nz) {
                visit(x, y, z + 1);
            }
        }

        for (float& distance : field.distances) {
            if (distance == untouched) {
                distance = -field.band;
            }
        }
    }
}

Aabb SignedDistanceField::bounds() const {
    const Vec3 last{static_cast<float>(counts[0] - 1), static_cast<float>(counts[1] - 1), static_cast<float>(counts[2] - 1)};
    return Aabb{origin, origin + last * spacing};
}

SignedDistanceField buildSignedDistanceField(const TriangleMesh& mesh, float spacing, float band, JobSystem& jobs) {
    if (!(spacing > 0.0f) || !std::isfinite(spacing)) {
        throw std::invalid_argument("A distance field's spacing must be finite and positive");
    }
    if (!(band >= spacing) || !std::isfinite(band)) {
        throw std::invalid_argument("A distance field's band must be finite and at least its spacing");
    }
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
        throw std::invalid_argument("A distance field needs a mesh of whole triangles");
    }
    for (const std::uint32_t index : mesh.indices) {
        if (index >= mesh.positions.size()) {
            throw std::invalid_argument("A triangle refers to a corner the mesh does not have");
        }
    }

    Vec3 low = mesh.positions[mesh.indices[0]];
    Vec3 high = low;
    for (const std::uint32_t index : mesh.indices) {
        const Vec3& position = mesh.positions[index];
        low = Vec3{std::min(low.x, position.x), std::min(low.y, position.y), std::min(low.z, position.z)};
        high = Vec3{std::max(high.x, position.x), std::max(high.y, position.y), std::max(high.z, position.z)};
    }

    SignedDistanceField field;
    field.spacing = spacing;
    field.band = band;
    const float margin = band + spacing;
    field.origin = low - Vec3{margin, margin, margin};
    const Vec3 extent = high - low + Vec3{2.0f * margin, 2.0f * margin, 2.0f * margin};
    field.counts = {static_cast<int>(std::ceil(extent.x / spacing)) + 1, static_cast<int>(std::ceil(extent.y / spacing)) + 1,
                    static_cast<int>(std::ceil(extent.z / spacing)) + 1};
    field.distances.assign(static_cast<std::size_t>(field.counts[0]) * static_cast<std::size_t>(field.counts[1]) * static_cast<std::size_t>(field.counts[2]),
                           untouched);

    const std::vector<TriangleNormals> normals = triangleNormals(mesh);
    const std::size_t triangleCount = mesh.indices.size() / 3;

    // Each slab's triangles, in order: those reaching any of its samples.
    const int slabCount = (field.counts[1] + slabSamples - 1) / slabSamples;
    std::vector<std::vector<std::uint32_t>> slabTriangles(static_cast<std::size_t>(slabCount));
    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
        const float yLow = std::min({corner(mesh, triangle, 0).y, corner(mesh, triangle, 1).y, corner(mesh, triangle, 2).y});
        const float yHigh = std::max({corner(mesh, triangle, 0).y, corner(mesh, triangle, 1).y, corner(mesh, triangle, 2).y});
        const auto [first, last] = sampleRange(yLow, yHigh, field.origin.y, spacing, band, field.counts[1]);
        for (int slab = first / slabSamples; slab <= last / slabSamples; ++slab) {
            slabTriangles[static_cast<std::size_t>(slab)].push_back(static_cast<std::uint32_t>(triangle));
        }
    }

    jobs.parallelFor(static_cast<std::size_t>(slabCount), 1, [&](std::size_t slab) {
        const int slabFirst = static_cast<int>(slab) * slabSamples;
        const int slabLast = std::min(slabFirst + slabSamples, field.counts[1]) - 1;
        for (const std::uint32_t triangle : slabTriangles[slab]) {
            const Vec3 a = corner(mesh, triangle, 0);
            const Vec3 b = corner(mesh, triangle, 1);
            const Vec3 c = corner(mesh, triangle, 2);
            const auto [x0, x1] = sampleRange(std::min({a.x, b.x, c.x}), std::max({a.x, b.x, c.x}), field.origin.x, spacing, band, field.counts[0]);
            const auto [yFirst, yLast] = sampleRange(std::min({a.y, b.y, c.y}), std::max({a.y, b.y, c.y}), field.origin.y, spacing, band, field.counts[1]);
            const auto [z0, z1] = sampleRange(std::min({a.z, b.z, c.z}), std::max({a.z, b.z, c.z}), field.origin.z, spacing, band, field.counts[2]);
            const int y0 = std::max(yFirst, slabFirst);
            const int y1 = std::min(yLast, slabLast);

            for (int z = z0; z <= z1; ++z) {
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        const Vec3 sample = field.origin + Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)} * spacing;
                        const TriangleClosest closest = closestTriangleFeature(sample, a, b, c);
                        const Vec3 away = sample - closest.point;
                        const float distance = away.length();
                        float& held = field.distances[sampleIndex(field, x, y, z)];
                        if (distance < band && distance < std::abs(held)) {
                            const bool inside = away.dot(featureNormal(normals[triangle], closest.feature)) < 0.0f;
                            held = inside ? -distance : distance;
                        }
                    }
                }
            }
        }
    });

    signFarSamples(field);
    return field;
}

/*
* The blend is written as nested steps a + (b - a) t, so equal corners give exactly their value and no slope. The
* slope along each axis is the blend's derivative: the difference across the cell, blended over the other two axes,
* over the spacing.
*/
FieldPoint sampleField(const SignedDistanceField& field, const Vec3& point) {
    const Vec3 local = (point - field.origin) * (1.0f / field.spacing);
    const std::array<float, 3> coordinates{local.x, local.y, local.z};
    std::array<int, 3> cell{};
    std::array<float, 3> share{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float coordinate = coordinates[axis];
        if (!(coordinate >= 0.0f) || coordinate > static_cast<float>(field.counts[axis] - 1)) {
            return FieldPoint{field.band, Vec3{0.0f, 0.0f, 0.0f}};
        }
        cell[axis] = std::min(static_cast<int>(coordinate), field.counts[axis] - 2);
        share[axis] = coordinate - static_cast<float>(cell[axis]);
    }

    const int x = cell[0];
    const int y = cell[1];
    const int z = cell[2];
    const float c000 = field.at(x, y, z);
    const float c100 = field.at(x + 1, y, z);
    const float c010 = field.at(x, y + 1, z);
    const float c110 = field.at(x + 1, y + 1, z);
    const float c001 = field.at(x, y, z + 1);
    const float c101 = field.at(x + 1, y, z + 1);
    const float c011 = field.at(x, y + 1, z + 1);
    const float c111 = field.at(x + 1, y + 1, z + 1);
    const float fx = share[0];
    const float fy = share[1];
    const float fz = share[2];

    const auto blend = [](float from, float to, float t) {
        return from + (to - from) * t;
    };
    const float x00 = blend(c000, c100, fx);
    const float x10 = blend(c010, c110, fx);
    const float x01 = blend(c001, c101, fx);
    const float x11 = blend(c011, c111, fx);
    const float y0 = blend(x00, x10, fy);
    const float y1 = blend(x01, x11, fy);

    const float perMetre = 1.0f / field.spacing;
    const float slopeX = blend(blend(c100 - c000, c110 - c010, fy), blend(c101 - c001, c111 - c011, fy), fz);
    const float slopeY = blend(x10 - x00, x11 - x01, fz);
    const float slopeZ = y1 - y0;
    return FieldPoint{blend(y0, y1, fz), Vec3{slopeX, slopeY, slopeZ} * perMetre};
}
