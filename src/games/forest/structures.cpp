#include "games/forest/structures.h"

#include "games/forest/forest_terrain.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    // The Ring's section: a square halfThickness either side with its corners chamfered this share back, the gap each
    // segment's end stops short of its corner (metres), and the drifted segments' reach, drift and turn.
    constexpr float ringChamferShare = 0.42f;
    constexpr float ringGapMetres = 1.5f;
    constexpr float ringDriftDegrees = 30.0f;
    constexpr float ringDriftLeast = 3.0f;
    constexpr float ringDriftExtra = 7.0f;
    constexpr float ringTurnDegrees = 1.0f;

    constexpr float degreesToRadians = fullTurnRadians / 360.0f;

    // The next pair of facet numbers from a seed, counting.
    Vec2 nextNumbers(std::uint32_t seed, std::uint64_t& facet) {
        const Vec2 numbers{forestUnitRandom(seed, 2 * facet), forestUnitRandom(seed, 2 * facet + 1)};
        ++facet;
        return numbers;
    }

    // A triangle with no normals of its own (flat), every corner given the facet's numbers and the first edge's
    // direction as its tangent (the numbers are no texture, so MikkTSpace has nothing to follow).
    void addFacet(Mesh& mesh, std::size_t a, std::size_t b, std::size_t c, Vec2 numbers) {
        const Vec4& first = mesh.vertices[a];
        const Vec4& second = mesh.vertices[b];
        const Vec3 along = Vec3{second.x - first.x, second.y - first.y, second.z - first.z}.normalized();
        Triangle triangle{.first = a, .second = b, .third = c};
        triangle.uvs = {numbers, numbers, numbers};
        triangle.tangents = {Vec4{along.x, along.y, along.z, 1.0f}, Vec4{along.x, along.y, along.z, 1.0f}, Vec4{along.x, along.y, along.z, 1.0f}};
        mesh.triangles.push_back(triangle);
    }
}

/*
* Each side between neighbouring sections is a quad of two triangles, each cap a fan; per face, a quad's two triangles and
* a cap's whole fan take one pair of numbers.
*/
void appendLoft(Mesh& mesh, const std::vector<std::vector<Vec3>>& sections, std::uint32_t seed, std::uint64_t& facet, FacetNumbers numbers) {
    if (sections.size() < 2 || sections.front().size() < 3) {
        throw std::invalid_argument("A loft needs two sections of three points or more");
    }

    const std::size_t points = sections.front().size();
    const std::size_t base = mesh.vertices.size();

    for (const std::vector<Vec3>& section : sections) {
        if (section.size() != points) {
            throw std::invalid_argument("A loft's sections need the same number of points");
        }

        for (const Vec3& point : section) {
            mesh.vertices.push_back(Vec4{point.x, point.y, point.z, 1.0f});
        }
    }

    const auto at = [&](std::size_t section, std::size_t point) {
        return base + section * points + point % points;
    };
    const bool perFace = numbers == FacetNumbers::PerFace;

    for (std::size_t section = 0; section + 1 < sections.size(); ++section) {
        for (std::size_t point = 0; point < points; ++point) {
            const Vec2 first = nextNumbers(seed, facet);
            addFacet(mesh, at(section, point), at(section, point + 1), at(section + 1, point + 1), first);
            addFacet(mesh, at(section, point), at(section + 1, point + 1), at(section + 1, point), perFace ? first : nextNumbers(seed, facet));
        }
    }

    const std::size_t last = sections.size() - 1;
    // Per triangle, the numbers are drawn in the same order as before faces shared them, so the Ring keeps its tones.
    const Vec2 bottom = perFace ? nextNumbers(seed, facet) : Vec2{};
    const Vec2 top = perFace ? nextNumbers(seed, facet) : Vec2{};

    for (std::size_t point = 1; point + 1 < points; ++point) {
        addFacet(mesh, at(0, 0), at(0, point + 1), at(0, point), perFace ? bottom : nextNumbers(seed, facet));
        addFacet(mesh, at(last, 0), at(last, point), at(last, point + 1), perFace ? top : nextNumbers(seed, facet));
    }
}

/*
* appendLoft wants the polygon's winding normal, (b - a) x (c - b), along the loft; a convex polygon's first three points
* give it, so a polygon wound the other way is taken backwards.
*/
void appendExtrusion(Mesh& mesh, const std::vector<Vec3>& polygon, const Vec3& offset, std::uint32_t seed, std::uint64_t& facet, FacetNumbers numbers) {
    if (polygon.size() < 3) {
        throw std::invalid_argument("An extrusion needs a polygon of three points or more");
    }

    std::vector<Vec3> start = polygon;
    const Vec3 winding = (start[1] - start[0]).cross(start[2] - start[1]);

    if (winding.dot(offset) < 0.0f) {
        std::reverse(start.begin(), start.end());
    }

    std::vector<Vec3> end;

    for (const Vec3& point : start) {
        end.push_back(point + offset);
    }

    appendLoft(mesh, {start, end}, seed, facet, numbers);
}

std::vector<Vec3> sectionAt(Vec3 centre, Vec3 u, Vec3 v, const std::vector<Vec2>& offsets) {
    std::vector<Vec3> section;

    for (const Vec2& offset : offsets) {
        section.push_back(centre + u * offset.x + v * offset.y);
    }

    return section;
}

Mesh ringSegmentsMesh(float radius, float halfThickness, std::uint32_t seed) {
    const float step = fullTurnRadians / static_cast<float>(ringSegments);
    const float gap = ringGapMetres / (radius - halfThickness);
    const float chamfer = ringChamferShare * halfThickness;
    const std::vector<Vec2> section{{-halfThickness, -chamfer}, {-halfThickness, chamfer}, {-chamfer, halfThickness}, {chamfer, halfThickness},
                                    {halfThickness, chamfer}, {halfThickness, -chamfer}, {chamfer, -halfThickness}, {-chamfer, -halfThickness}};
    const std::uint32_t facetSeed = forestShapeSeed(seed, 1);
    const std::uint32_t driftSeed = forestShapeSeed(seed, 2);
    Mesh mesh;
    std::uint64_t facet = 0;

    for (int segment = 0; segment < ringSegments; ++segment) {
        // Angles from the top towards +x: the segment's middle, its outward direction u and the way it runs, d.
        const float middle = step * static_cast<float>(segment);
        const Vec3 u{std::sin(middle), std::cos(middle), 0.0f};
        const Vec3 runs{std::cos(middle), -std::sin(middle), 0.0f};
        const Vec3 v = u.cross(runs);
        const Vec3 centre = u * radius;

        // Each corner line of the prism meets the radial plane at an end angle (its normal the way the circle runs there).
        const auto endSection = [&](float angle) {
            const Vec3 planeNormal{std::cos(angle), -std::sin(angle), 0.0f};
            std::vector<Vec3> points;

            for (const Vec3& point : sectionAt(centre, u, v, section)) {
                points.push_back(point - runs * (planeNormal.dot(point) / planeNormal.dot(runs)));
            }

            return points;
        };

        std::vector<std::vector<Vec3>> ends{endSection(middle - 0.5f * step + gap), endSection(middle + 0.5f * step - gap)};

        // From the top within the drift's reach: pushed outwards and turned about the ring's axis round its middle.
        const float fromTop = std::fabs(std::remainder(middle, fullTurnRadians));

        if (fromTop <= ringDriftDegrees * degreesToRadians) {
            const float drift = ringDriftLeast + ringDriftExtra * forestUnitRandom(driftSeed, 2 * static_cast<std::uint64_t>(segment));
            const float turn = ringTurnDegrees * degreesToRadians * (2.0f * forestUnitRandom(driftSeed, 2 * static_cast<std::uint64_t>(segment) + 1) - 1.0f);
            const float cosine = std::cos(turn);
            const float sine = std::sin(turn);

            for (std::vector<Vec3>& end : ends) {
                for (Vec3& point : end) {
                    const Vec3 offset = point - centre;
                    point = centre + u * drift + Vec3{cosine * offset.x - sine * offset.y, sine * offset.x + cosine * offset.y, offset.z};
                }
            }
        }

        appendLoft(mesh, ends, facetSeed, facet, FacetNumbers::PerTriangle);
    }

    return mesh;
}
