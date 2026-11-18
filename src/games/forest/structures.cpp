#include "games/forest/structures.h"

#include "games/forest/forest_terrain.h"
#include "games/forest/valley_shapes.h"
#include "math/angles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
    // A slab sinks this far into the ground, so uneven ground never shows under it; a spire twice that.
    constexpr float slabSink = 10.0f;
    constexpr float spireSink = 20.0f;
    // A slab's depth as a share of its width, and its sections' shares: nearly upright to the shoulder, a quarter in
    // over the shoulder, then slowly in to the start of the chisel top, which leaves a thin edge across most of the depth.
    constexpr float slabDepthShare = 0.45f;
    constexpr float belowShoulder = 0.97f;
    constexpr float aboveShoulder = 0.75f;
    constexpr float chiselStart = 0.86f;
    constexpr float chiselBase = 0.73f;
    constexpr float chiselEdge = 0.04f;
    // How far the chisel's edge stands to the cut's side, as a share of the slab's width.
    constexpr float chiselOffset = 0.33f;
    // The knee between the front ridge and the right edge: this share of the way from the ridge, at this share of the
    // depth (outside the straight line between them, so the section stays convex).
    constexpr float kneeAcross = 0.6f;
    constexpr float kneeDepth = 0.55f;
    // The extra levels between the foot and the shoulder (shares of the shoulder's height) and between the shoulder and
    // the chisel, with their widths and how far the ridge zigzags: a band every 60-120 m on the Monolith.
    constexpr std::array<std::array<float, 3>, 2> belowShoulderBands{{{0.33f, 0.99f, 0.12f}, {0.67f, 0.98f, -0.08f}}};
    constexpr float aboveShoulderBand = 0.5f;
    constexpr float aboveShoulderShift = 0.1f;

    /*
    * The Monolith's slabs, from the concept sheets (v4): how far across from forestMonolithPlace, height, width, how far
    * behind (+) or in front of (-) the tallest's foot, the side the top is cut to, and the shoulder's share of the height.
    */
    struct MonolithSlab {
        float across;
        float height;
        float width;
        float behind;
        float cut;
        float shoulder;
    };

    constexpr std::array<MonolithSlab, 5> monolithLayout{{
        {-156.0f, 300.0f, 86.0f, -10.0f, -1.0f, 0.55f},
        {-79.0f, 468.0f, 99.0f, -15.0f, 1.0f, 0.62f},
        {0.0f, 600.0f, 110.0f, 0.0f, -1.0f, 0.5f},
        {84.0f, 372.0f, 95.0f, -20.0f, 1.0f, 0.58f},
        {158.0f, 240.0f, 79.0f, -12.0f, -1.0f, 0.6f}
    }};
    // The front ridge's share across the width, towards the cut.
    constexpr float monolithRidgeShare = 0.25f;

    // The strut: the slab it reaches (the second, tall enough to hold it), its height above the clearing (the old
    // bridge's), its thickness, and how far its end goes into the slab.
    constexpr std::size_t strutSlab = 1;
    constexpr float strutHeight = 331.0f;
    constexpr float strutThickness = 8.0f;
    constexpr float strutInside = 4.0f;

    // The far spires, placed by hand in the haze north of the walk as the towers were: (x, z, height, footprint width).
    constexpr std::array<std::array<float, 4>, 10> spireLayout{{
        {-1800.0f, -3000.0f, 520.0f, 60.0f}, {1500.0f, -3800.0f, 760.0f, 75.0f}, {300.0f, -5200.0f, 640.0f, 90.0f},
        {-900.0f, -6500.0f, 800.0f, 70.0f}, {2600.0f, -2500.0f, 380.0f, 45.0f}, {-2800.0f, -4500.0f, 450.0f, 55.0f},
        {900.0f, -7000.0f, 560.0f, 80.0f}, {-3500.0f, -2000.0f, 300.0f, 40.0f}, {3300.0f, -5600.0f, 700.0f, 65.0f},
        {-1500.0f, -4100.0f, 340.0f, 30.0f}
    }};

    // The Ring's section: a square halfThickness either side with its corners chamfered this share back, the gap each
    // segment's end stops short of its corner (metres), and the drifted segments' reach, drift and turn.
    constexpr float ringChamferShare = 0.42f;
    constexpr float ringGapMetres = 1.5f;
    constexpr float ringDriftDegrees = 30.0f;
    constexpr float ringDriftLeast = 3.0f;
    constexpr float ringDriftExtra = 7.0f;
    constexpr float ringTurnDegrees = 1.0f;

    constexpr float degreesToRadians = fullTurnRadians / 360.0f;

    // A triangle with no normals of its own (flat), every corner given the facet's two numbers and the first edge's
    // direction as its tangent (the numbers are no texture, so MikkTSpace has nothing to follow).
    void addFacet(Mesh& mesh, std::size_t a, std::size_t b, std::size_t c, std::uint32_t seed, std::uint64_t& facet) {
        const Vec4& first = mesh.vertices[a];
        const Vec4& second = mesh.vertices[b];
        const Vec3 along = Vec3{second.x - first.x, second.y - first.y, second.z - first.z}.normalized();
        const Vec2 numbers{forestUnitRandom(seed, 2 * facet), forestUnitRandom(seed, 2 * facet + 1)};
        ++facet;
        Triangle triangle{.first = a, .second = b, .third = c};
        triangle.uvs = {numbers, numbers, numbers};
        triangle.tangents = {Vec4{along.x, along.y, along.z, 1.0f}, Vec4{along.x, along.y, along.z, 1.0f}, Vec4{along.x, along.y, along.z, 1.0f}};
        mesh.triangles.push_back(triangle);
    }

    /*
    * Sections lofted into a closed surface: each a convex polygon of the same number of points, which go round from -u
    * through +v to +u and -v where u x v points back along the loft (as round a slab's hexagon seen from above, the loft
    * rising), so the sides between neighbouring sections face out; both ends capped by fans. The vertices are shared, the
    * triangles flat.
    */
    void appendLoft(Mesh& mesh, const std::vector<std::vector<Vec3>>& sections, std::uint32_t seed, std::uint64_t& facet) {
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

        for (std::size_t section = 0; section + 1 < sections.size(); ++section) {
            for (std::size_t point = 0; point < points; ++point) {
                addFacet(mesh, at(section, point), at(section, point + 1), at(section + 1, point + 1), seed, facet);
                addFacet(mesh, at(section, point), at(section + 1, point + 1), at(section + 1, point), seed, facet);
            }
        }

        const std::size_t last = sections.size() - 1;

        for (std::size_t point = 1; point + 1 < points; ++point) {
            addFacet(mesh, at(0, 0), at(0, point + 1), at(0, point), seed, facet);
            addFacet(mesh, at(last, 0), at(last, point), at(last, point + 1), seed, facet);
        }
    }

    // A slab's hexagon at one level, in its own frame: the left edge, the front ridge, the knee, the right edge, then the
    // ridge and the knee turned half round about the level's middle.
    std::vector<Vec3> slabSection(const SlabShape& slab, const SlabLevel& level) {
        const float halfWidth = 0.5f * slab.width * level.widthShare;
        const float halfDepth = 0.5f * slab.depth * level.depthShare;
        const float ridge = std::clamp(slab.ridgeShare + level.ridgeShift, -0.9f, 0.9f);
        const float knee = ridge + (1.0f - ridge) * kneeAcross;
        const auto at = [&](float across, float depth) {
            return Vec3{level.offset + across * halfWidth, level.height, depth * halfDepth};
        };
        return {at(-1.0f, 0.0f), at(ridge, 1.0f), at(knee, kneeDepth), at(1.0f, 0.0f), at(-ridge, -1.0f), at(-knee, -kneeDepth)};
    }

    // How far the slab's left edge stands from its middle at a height (metres), between the levels round it.
    float slabLeftEdge(const SlabShape& slab, float height) {
        for (std::size_t level = 0; level + 1 < slab.levels.size(); ++level) {
            const SlabLevel& below = slab.levels[level];
            const SlabLevel& above = slab.levels[level + 1];

            if (height <= above.height) {
                const float share = std::clamp((height - below.height) / (above.height - below.height), 0.0f, 1.0f);
                const float widthShare = below.widthShare + (above.widthShare - below.widthShare) * share;
                const float offset = below.offset + (above.offset - below.offset) * share;
                return offset - 0.5f * slab.width * widthShare;
            }
        }

        return slab.levels.back().offset;
    }

    // A section at a point of a loft: octagon-like or rhombus offsets (along -u .. +v .. +u .. -v) round a centre.
    std::vector<Vec3> sectionAt(Vec3 centre, Vec3 u, Vec3 v, const std::vector<Vec2>& offsets) {
        std::vector<Vec3> section;

        for (const Vec2& offset : offsets) {
            section.push_back(centre + u * offset.x + v * offset.y);
        }

        return section;
    }
}

SlabShape makeSlab(Vec3 foot, float width, float height, float cut, float shoulderShare, float ridgeShare, std::uint32_t seed) {
    const float shoulder = shoulderShare * height;
    return SlabShape{
        .foot = foot,
        .width = width,
        .depth = slabDepthShare * width,
        .height = height,
        .ridgeShare = ridgeShare,
        .levels = {SlabLevel{-slabSink, 1.0f, 1.0f, 0.0f},
                   SlabLevel{belowShoulderBands[0][0] * shoulder, belowShoulderBands[0][1], belowShoulderBands[0][1], 0.0f, belowShoulderBands[0][2]},
                   SlabLevel{belowShoulderBands[1][0] * shoulder, belowShoulderBands[1][1], belowShoulderBands[1][1], 0.0f, belowShoulderBands[1][2]},
                   SlabLevel{shoulder, belowShoulder, belowShoulder, 0.0f},
                   SlabLevel{shoulder + slabShoulderRise, aboveShoulder, aboveShoulder, 0.0f},
                   SlabLevel{shoulder + slabShoulderRise + aboveShoulderBand * (chiselStart * height - shoulder - slabShoulderRise), 0.5f * (aboveShoulder + chiselBase),
                             0.5f * (aboveShoulder + chiselBase), 0.0f, aboveShoulderShift},
                   SlabLevel{chiselStart * height, chiselBase, chiselBase, 0.0f},
                   SlabLevel{height, chiselEdge, chiselBase, chiselOffset * cut * width}},
        .seed = seed
    };
}

Mesh slabMesh(const SlabShape& slab) {
    std::vector<std::vector<Vec3>> sections;

    for (const SlabLevel& level : slab.levels) {
        sections.push_back(slabSection(slab, level));
    }

    Mesh mesh;
    std::uint64_t facet = 0;
    appendLoft(mesh, sections, slab.seed, facet);
    return mesh;
}

std::vector<SlabShape> monolithSlabs(float groundLevel) {
    const float mainDepth = slabDepthShare * monolithLayout[2].width;
    const float mainZ = forestMonolithPlace.y - 0.5f * mainDepth;
    std::vector<SlabShape> slabs;

    for (std::size_t index = 0; index < monolithLayout.size(); ++index) {
        const MonolithSlab& slab = monolithLayout[index];
        const Vec3 foot{forestMonolithPlace.x + slab.across, groundLevel, mainZ - slab.behind};
        slabs.push_back(makeSlab(foot, slab.width, slab.height, slab.cut, slab.shoulder, monolithRidgeShare * slab.cut,
                                 forestShapeSeed(static_cast<std::uint32_t>(index), 40)));
    }

    return slabs;
}

std::vector<SlabShape> farSpires() {
    std::vector<SlabShape> spires;

    for (std::size_t index = 0; index < spireLayout.size(); ++index) {
        const auto& [x, z, height, width] = spireLayout[index];
        const float side = index % 2 == 0 ? 1.0f : -1.0f;
        spires.push_back(SlabShape{
            .foot = Vec3{x, forestHeight(x, z), z},
            .width = width,
            .depth = 0.8f * width,
            .height = height,
            .ridgeShare = 0.2f * side,
            .levels = {SlabLevel{-spireSink, 1.0f, 1.0f, 0.0f}, SlabLevel{0.6f * height, 0.55f, 0.55f, 0.0f},
                       SlabLevel{0.6f * height + 4.0f, 0.4f, 0.4f, 0.0f}, SlabLevel{height, 0.02f, 0.3f, 0.15f * side * width}},
            .seed = forestShapeSeed(static_cast<std::uint32_t>(index), 41)
        });
    }

    return spires;
}

StrutShape monolithStrut(float groundLevel) {
    const PillarPlacement pillar = monolithPillar(groundLevel);
    const SlabShape slab = monolithSlabs(groundLevel)[strutSlab];
    const float height = groundLevel + strutHeight;
    return StrutShape{
        .from = Vec3{pillar.foot.x + 0.6f * pillar.radius, height, pillar.foot.z},
        .to = Vec3{slab.foot.x + slabLeftEdge(slab, strutHeight) + strutInside, height, slab.foot.z},
        .thickness = strutThickness,
        .seed = forestShapeSeed(0, 42)
    };
}

Mesh strutMesh(const StrutShape& strut) {
    const Vec3 along = (strut.to - strut.from).normalized();
    const Vec3 across = along.cross(Vec3{0.0f, 1.0f, 0.0f}).normalized();
    const Vec3 up = across.cross(along);
    const float half = 0.5f * strut.thickness;
    const std::vector<Vec2> rhombus{{-half, 0.0f}, {0.0f, 1.4f * half}, {half, 0.0f}, {0.0f, -1.4f * half}};
    Mesh mesh;
    std::uint64_t facet = 0;
    appendLoft(mesh, {sectionAt(strut.from, across, up, rhombus), sectionAt(strut.to, across, up, rhombus)}, strut.seed, facet);
    return mesh;
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

        appendLoft(mesh, ends, facetSeed, facet);
    }

    return mesh;
}
