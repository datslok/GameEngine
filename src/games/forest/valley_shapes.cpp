#include "games/forest/valley_shapes.h"

#include "core/hash.h"
#include "games/forest/forest_terrain.h"
#include "math/angles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
    // The seeds of the placements' random numbers, so pillars and islands do not share them.
    constexpr std::uint64_t pillarPlaceSeed = 0x5049'4c4cull;
    constexpr std::uint64_t islandPlaceSeed = 0x4953'4c41ull;

    /*
    * The pillars' sizes and spread, from the concept art: 200-350 m tall, 12-45 m in radius (most slender), thrown over
    * 2.5 km either side of the valley's axis, thickest near it, from 5 km north to the rim behind the start, at least
    * 70 m of air between two, and none within 40 m of the path (plus a third of its own radius) or near the Ring, the
    * clearing, the Monolith or the start.
    */
    constexpr float pillarShortest = 200.0f;
    constexpr float pillarTallestExtra = 150.0f;
    constexpr float pillarThinnest = 12.0f;
    constexpr float pillarThickestExtra = 33.0f;
    constexpr float pillarSpread = 2500.0f;
    constexpr float pillarNorthmost = -5000.0f;
    constexpr float pillarSouthmost = 700.0f;
    constexpr float pillarGap = 70.0f;
    constexpr float pillarPathGap = 40.0f;
    constexpr float pillarStartGap = 250.0f;
    constexpr int triesPerShape = 60;

    // The islands': 25-150 m in radius, their tips 380-700 m over the ground below, 120 m of air between two, over 2.2 km
    // either side of the axis from 4.5 km north to 400 m south of the Ring, none within 400 m of the start.
    constexpr float islandSmallest = 25.0f;
    constexpr float islandLargestExtra = 125.0f;
    constexpr float islandLowest = 380.0f;
    constexpr float islandHighestExtra = 320.0f;
    constexpr float islandSpread = 2200.0f;
    constexpr float islandNorthmost = -4500.0f;
    constexpr float islandSouthmost = 400.0f;
    constexpr float islandGap = 120.0f;
    constexpr float islandStartGap = 400.0f;

    // The shapes' noise: how much a pillar's radius bulges and its axis leans, and how jagged an island's underside is.
    constexpr float pillarBulge = 0.22f;
    constexpr float pillarRidges = 0.08f;
    constexpr float pillarLean = 0.35f;
    constexpr float pillarTaper = 0.33f;
    constexpr float islandJag = 0.25f;
    constexpr float islandDome = 0.08f;

    // A random number in 0..1 for (seed, index), the same every run on every machine.
    float unitRandom(std::uint64_t seed, std::uint64_t index) {
        return static_cast<float>(mixBits(seed ^ mixBits(index + 0x9e3779b97f4a7c15ull)) >> 40) * (1.0f / 16777216.0f);
    }

    std::uint32_t shapeSeed(std::uint32_t seed, std::uint32_t use) {
        return static_cast<std::uint32_t>(mixBits((static_cast<std::uint64_t>(seed) << 8) | use));
    }

    // A pillar's or island's place off the axis: thickest near it (the square of a random number is mostly small).
    float offAxis(float spread, float random, float side) {
        return (side < 0.5f ? -1.0f : 1.0f) * spread * random * random;
    }

    // The lowest ground under a pillar's foot (its centre and eight points round its edge), where it stands, so on a
    // slope no gap shows under its downhill side.
    float lowestGround(float x, float z, float radius) {
        float lowest = forestHeight(x, z);

        for (int point = 0; point < 8; ++point) {
            const float angle = fullTurnRadians * static_cast<float>(point) / 8.0f;
            lowest = std::min(lowest, forestHeight(x + radius * std::cos(angle), z + radius * std::sin(angle)));
        }

        return lowest;
    }

    bool clearOfLandmarks(float x, float z, float radius) {
        const bool nearPath = forestPathDistance(x, z) < pillarPathGap + radius * 1.3f;
        const bool nearRing = std::fabs(z - forestRingPlace.y) < radius + pillarPathGap &&
                              std::fabs(x - forestRingPlace.x) < forestRingRadius + forestRingTubeRadius + radius + pillarPathGap;
        const bool nearClearing = (Vec2{x, z} - forestClearingCentre).length() < forestClearingRadius + forestClearingBlend + radius;
        const bool nearMonolith = std::fabs(x - forestMonolithPlace.x) < 500.0f + radius && z < forestMonolithPlace.y + 100.0f &&
                                  z > forestMonolithPlace.y - 250.0f - radius;
        const bool nearStart = (Vec2{x, z} - forestStartPlace).length() < pillarStartGap + radius;
        return !nearPath && !nearRing && !nearClearing && !nearMonolith && !nearStart;
    }
}

Mesh wrappedSurfaceMesh(int rows, int columns, bool wrapRows, const std::function<Vec3(int row, int column)>& point) {
    if (rows < 1 || columns < 3 || (wrapRows && rows < 3)) {
        throw std::invalid_argument("A wrapped surface needs at least 3 columns and a row, or 3 rows when they wrap");
    }

    Mesh mesh;
    const int pointRows = wrapRows ? rows : rows + 1;
    const auto index = [&](int row, int column) {
        return static_cast<std::size_t>((row % pointRows) * columns + (column % columns));
    };

    mesh.vertices.reserve(static_cast<std::size_t>(pointRows * columns));

    for (int row = 0; row < pointRows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const Vec3 place = point(row, column);
            mesh.vertices.push_back(Vec4{place.x, place.y, place.z, 1.0f});
        }
    }

    // Each triangle's corners and its unnormalised normal (twice its area long), kept to share out to its corners.
    struct Face {
        std::array<std::size_t, 3> corners;
        Vec3 areaNormal;
    };

    std::vector<Face> faces;
    std::vector<Vec3> normals(mesh.vertices.size(), Vec3{0.0f, 0.0f, 0.0f});
    const auto addFace = [&](std::size_t first, std::size_t second, std::size_t third) {
        const Vec4& a = mesh.vertices[first];
        const Vec4& b = mesh.vertices[second];
        const Vec4& c = mesh.vertices[third];
        const Vec3 areaNormal = Vec3{b.x - a.x, b.y - a.y, b.z - a.z}.cross(Vec3{c.x - a.x, c.y - a.y, c.z - a.z});

        if (areaNormal.lengthSquared() <= 0.0f) {
            return;
        }

        faces.push_back(Face{{first, second, third}, areaNormal});

        for (const std::size_t corner : {first, second, third}) {
            normals[corner] = normals[corner] + areaNormal;
        }
    };

    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const std::size_t here = index(row, column);
            const std::size_t around = index(row, column + 1);
            const std::size_t up = index(row + 1, column);
            const std::size_t both = index(row + 1, column + 1);
            addFace(here, around, both);
            addFace(here, both, up);
        }
    }

    mesh.triangles.reserve(faces.size());

    for (const Face& face : faces) {
        Triangle triangle{face.corners[0], face.corners[1], face.corners[2]};

        for (std::size_t slot = 0; slot < 3; ++slot) {
            const std::size_t corner = face.corners[slot];
            const int row = static_cast<int>(corner) / columns;
            const int column = static_cast<int>(corner) % columns;
            triangle.normals[slot] = normals[corner].normalized();
            triangle.uvs[slot] = Vec2{static_cast<float>(column) / static_cast<float>(columns), static_cast<float>(row) / static_cast<float>(rows)};
        }

        mesh.triangles.push_back(triangle);
    }

    return mesh;
}

/*
* Rows rise up the column; its last few rows round its top over to a point. The radius bulges by noise read round a
* circle (so the seam where the columns meet matches), finer ridges run up it, and its axis leans by slow noise.
*/
Mesh pillarMesh(std::uint32_t seed, int rings, int segments) {
    const int domeRows = std::max(2, rings / 10);
    const int bodyRows = rings - domeRows;
    const float bodyTop = 0.97f;

    return wrappedSurfaceMesh(rings, segments, false, [&](int row, int column) {
        const float angle = fullTurnRadians * static_cast<float>(column) / static_cast<float>(segments);
        float height = 0.0f;
        float shape = 1.0f;

        if (row <= bodyRows) {
            height = -pillarBuriedDepth + (bodyTop + pillarBuriedDepth) * static_cast<float>(row) / static_cast<float>(bodyRows);
        } else {
            const float dome = static_cast<float>(row - bodyRows) / static_cast<float>(domeRows) * (0.5f * fullTurnRadians / 2.0f);
            height = bodyTop + (1.0f - bodyTop) * std::sin(dome);
            shape = std::max(0.0f, std::cos(dome)); // exactly 0 at the top: cos rounds to a hair below it
        }

        const float around = std::cos(angle);
        const float across = std::sin(angle);
        const float bulge = pillarBulge * forestFractalNoise(1.3f * around + 5.0f, 1.3f * across + 4.0f * height, 3, shapeSeed(seed, 1));
        const float ridges = pillarRidges * forestFractalNoise(3.0f * around, 3.0f * across + 12.0f * height, 2, shapeSeed(seed, 2));
        const float radius = shape * (1.0f - pillarTaper * std::max(height, 0.0f)) * (1.0f + bulge + ridges);
        const float leanX = pillarLean * forestFractalNoise(1.5f * height, 0.5f, 2, shapeSeed(seed, 3));
        const float leanZ = pillarLean * forestFractalNoise(0.5f, 1.5f * height, 2, shapeSeed(seed, 4));
        return Vec3{leanX + radius * across, height, leanZ + radius * around};
    });
}

/*
* Rows rise from the tip to the rim (the underside, widening fast, then slowly) and on inwards over the top to its
* centre. The rim's jag continues over the top's edge, so the two meet.
*/
Mesh islandMesh(std::uint32_t seed, int rings, int segments) {
    const int underRows = std::max(2, rings * 7 / 10);
    const int topRows = rings - underRows;

    return wrappedSurfaceMesh(rings, segments, false, [&](int row, int column) {
        const float angle = fullTurnRadians * static_cast<float>(column) / static_cast<float>(segments);
        const float around = std::cos(angle);
        const float across = std::sin(angle);
        const auto jag = [&](float rise) {
            return 1.0f + islandJag * forestFractalNoise(2.5f * around + 3.0f, 2.5f * across + 6.0f * rise, 3, shapeSeed(seed, 5));
        };

        if (row <= underRows) {
            const float rise = static_cast<float>(row) / static_cast<float>(underRows);
            const float radius = std::pow(rise, 0.55f) * jag(rise);
            const float height = -islandDepth * std::pow(1.0f - rise, 1.3f);
            return Vec3{radius * across, height, radius * around};
        }

        const float inward = static_cast<float>(row - underRows) / static_cast<float>(topRows) * (0.25f * fullTurnRadians);
        const float radius = std::max(0.0f, std::cos(inward)) * jag(1.0f); // exactly 0 at the centre
        const float bumps = 0.02f * forestFractalNoise(4.0f * radius * around, 4.0f * radius * across, 2, shapeSeed(seed, 6));
        return Vec3{radius * across, islandDome * std::sin(inward) + bumps, radius * around};
    });
}

// Columns round the ring (anticlockwise seen from +z), rows round its tube, so "around" crossed with "up" points out of the tube.
Mesh ringMesh(float tubeShare, int segments, int sides) {
    return wrappedSurfaceMesh(sides, segments, true, [&](int row, int column) {
        const float ringAngle = fullTurnRadians * static_cast<float>(column) / static_cast<float>(segments);
        const float tubeAngle = fullTurnRadians * static_cast<float>(row) / static_cast<float>(sides);
        const float reach = 1.0f + tubeShare * std::cos(tubeAngle);
        return Vec3{std::cos(ringAngle) * reach, std::sin(ringAngle) * reach, tubeShare * std::sin(tubeAngle)};
    });
}

std::vector<PillarPlacement> placePillars(const ValleyLayout& layout) {
    std::vector<PillarPlacement> pillars;
    const int tries = std::max(layout.pillarCount, 0) * triesPerShape;

    for (int attempt = 0; attempt < tries && static_cast<int>(pillars.size()) < layout.pillarCount; ++attempt) {
        const std::uint64_t base = static_cast<std::uint64_t>(attempt) * 8u;
        const float z = pillarNorthmost + (pillarSouthmost - pillarNorthmost) * unitRandom(pillarPlaceSeed, base);
        const float x = valleyAxis(z) + offAxis(pillarSpread, unitRandom(pillarPlaceSeed, base + 1), unitRandom(pillarPlaceSeed, base + 2));
        const float radius = pillarThinnest + pillarThickestExtra * std::pow(unitRandom(pillarPlaceSeed, base + 3), 1.5f);

        if (!clearOfLandmarks(x, z, radius)) {
            continue;
        }

        const bool crowded = std::any_of(pillars.begin(), pillars.end(), [&](const PillarPlacement& other) {
            return (Vec2{x, z} - Vec2{other.foot.x, other.foot.z}).length() < radius + other.radius + pillarGap;
        });

        if (crowded) {
            continue;
        }

        pillars.push_back(PillarPlacement{
            .foot = Vec3{x, lowestGround(x, z, radius), z},
            .radius = radius,
            .height = pillarShortest + pillarTallestExtra * unitRandom(pillarPlaceSeed, base + 4),
            .turnRadians = fullTurnRadians * unitRandom(pillarPlaceSeed, base + 5),
            .variant = static_cast<int>(pillars.size()) % std::max(layout.variants, 1)
        });
    }

    return pillars;
}

std::vector<IslandPlacement> placeIslands(const ValleyLayout& layout, const std::vector<PillarPlacement>& pillars) {
    std::vector<IslandPlacement> islands;
    const int tries = std::max(layout.islandCount, 0) * triesPerShape;

    for (int attempt = 0; attempt < tries && static_cast<int>(islands.size()) < layout.islandCount; ++attempt) {
        const std::uint64_t base = static_cast<std::uint64_t>(attempt) * 8u;
        const float z = islandNorthmost + (islandSouthmost - islandNorthmost) * unitRandom(islandPlaceSeed, base);
        const float x = valleyAxis(z) + offAxis(islandSpread, std::sqrt(unitRandom(islandPlaceSeed, base + 1)), unitRandom(islandPlaceSeed, base + 2));
        const float radius = islandSmallest + islandLargestExtra * std::pow(unitRandom(islandPlaceSeed, base + 3), 2.0f);
        const float tip = std::max(forestHeight(x, z), 0.0f) + islandLowest + islandHighestExtra * unitRandom(islandPlaceSeed, base + 4);

        if ((Vec2{x, z} - forestStartPlace).length() < islandStartGap + radius) {
            continue;
        }

        const bool crowded = std::any_of(islands.begin(), islands.end(), [&](const IslandPlacement& other) {
            return (Vec2{x, z} - Vec2{other.rim.x, other.rim.z}).length() < radius + other.radius + islandGap;
        });
        const bool onPillar = std::any_of(pillars.begin(), pillars.end(), [&](const PillarPlacement& pillar) {
            const bool over = (Vec2{x, z} - Vec2{pillar.foot.x, pillar.foot.z}).length() < radius + 1.5f * pillar.radius;
            return over && pillar.foot.y + pillar.height > tip - islandGap;
        });

        if (crowded || onPillar) {
            continue;
        }

        islands.push_back(IslandPlacement{
            .rim = Vec3{x, tip + islandDepth * radius, z},
            .radius = radius,
            .turnRadians = fullTurnRadians * unitRandom(islandPlaceSeed, base + 5),
            .variant = static_cast<int>(islands.size()) % std::max(layout.variants, 1)
        });
    }

    return islands;
}

/*
* From the concept art: a main slab 260 m wide, 120 deep and 600 tall, a 400 m block west and a 330 m annex east, a
* crown with masts, a deck standing out 160 m east at 420 m up, and a truss bridge 330 m up to the pillar on the
* western rim. Everything stands 10 m into the ground, so uneven ground never shows under it.
*/
std::vector<ValleyBox> monolithBoxes(float groundLevel) {
    const float x = forestMonolithPlace.x;
    const float front = forestMonolithPlace.y;
    const float sink = 10.0f;
    const auto standing = [&](float centreX, float width, float height, float depth, float frontZ) {
        return ValleyBox{Vec3{centreX, groundLevel - sink + 0.5f * (height + sink), frontZ - 0.5f * depth},
                         Vec3{0.5f * width, 0.5f * (height + sink), 0.5f * depth}};
    };

    std::vector<ValleyBox> boxes{
        standing(x, 260.0f, 600.0f, 120.0f, front),
        standing(x - 175.0f, 90.0f, 400.0f, 100.0f, front - 10.0f),
        standing(x + 175.0f, 90.0f, 330.0f, 100.0f, front - 10.0f),
        ValleyBox{Vec3{x - 20.0f, groundLevel + 625.0f, front - 60.0f}, Vec3{80.0f, 25.0f, 40.0f}},
        ValleyBox{Vec3{x + 290.0f, groundLevel + 420.0f, front - 40.0f}, Vec3{70.0f, 7.0f, 30.0f}}
    };

    for (const auto& [mastX, mastHeight] : {std::pair{-90.0f, 70.0f}, std::pair{20.0f, 110.0f}, std::pair{40.0f, 60.0f}}) {
        boxes.push_back(ValleyBox{Vec3{x + mastX, groundLevel + 650.0f + 0.5f * mastHeight, front - 60.0f}, Vec3{1.5f, 0.5f * mastHeight, 1.5f}});
    }

    // The truss: its two chords and a post every 20 m between them.
    const PillarPlacement pillar = monolithPillar(groundLevel);
    const float bridgeEast = x - 220.0f;
    const float bridgeWest = pillar.foot.x + 0.6f * pillar.radius;
    const float bridgeMiddle = 0.5f * (bridgeEast + bridgeWest);
    const float bridgeHalf = 0.5f * (bridgeEast - bridgeWest);
    const float bridgeZ = front - 50.0f;

    for (const float chord : {groundLevel + 322.0f, groundLevel + 340.0f}) {
        boxes.push_back(ValleyBox{Vec3{bridgeMiddle, chord, bridgeZ}, Vec3{bridgeHalf, 1.5f, 4.0f}});
    }

    for (float post = bridgeWest + 20.0f; post < bridgeEast; post += 20.0f) {
        boxes.push_back(ValleyBox{Vec3{post, groundLevel + 331.0f, bridgeZ}, Vec3{1.0f, 9.0f, 4.0f}});
    }

    return boxes;
}

PillarPlacement monolithPillar(float groundLevel) {
    const float x = forestMonolithPlace.x - 480.0f;
    const float z = forestMonolithPlace.y - 50.0f;
    const float foot = forestHeight(x, z);
    return PillarPlacement{.foot = Vec3{x, foot, z}, .radius = 45.0f, .height = groundLevel + 470.0f - foot, .turnRadians = 0.0f, .variant = 0};
}

/*
* Ten slabs placed by hand in the haze north of the walk, each with a narrower block on top, so the skyline is
* built rather than random: (x, z, height, width).
*/
std::vector<ValleyBox> farTowerBoxes() {
    constexpr std::array<std::array<float, 4>, 10> towers{{
        {-1800.0f, -3000.0f, 520.0f, 120.0f}, {1500.0f, -3800.0f, 760.0f, 150.0f}, {300.0f, -5200.0f, 640.0f, 180.0f},
        {-900.0f, -6500.0f, 800.0f, 140.0f}, {2600.0f, -2500.0f, 380.0f, 90.0f}, {-2800.0f, -4500.0f, 450.0f, 110.0f},
        {900.0f, -7000.0f, 560.0f, 160.0f}, {-3500.0f, -2000.0f, 300.0f, 80.0f}, {3300.0f, -5600.0f, 700.0f, 130.0f},
        {-1500.0f, -4100.0f, 340.0f, 60.0f}
    }};
    std::vector<ValleyBox> boxes;

    for (const auto& [x, z, height, width] : towers) {
        const float foot = forestHeight(x, z) - 20.0f;
        boxes.push_back(ValleyBox{Vec3{x, foot + 0.5f * height, z}, Vec3{0.5f * width, 0.5f * height, 0.4f * width}});
        boxes.push_back(ValleyBox{Vec3{x - 0.15f * width, foot + 1.04f * height, z}, Vec3{0.2f * width, 0.04f * height, 0.25f * width}});
    }

    return boxes;
}
