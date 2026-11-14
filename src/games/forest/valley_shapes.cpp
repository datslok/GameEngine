#include "games/forest/valley_shapes.h"

#include "core/hash.h"
#include "games/forest/forest_terrain.h"
#include "math/angles.h"
#include "math/quat.h"
#include "math/smooth_step.h"

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
    constexpr float pillarLean = 0.35f;
    constexpr float pillarTaper = 0.33f;
    constexpr float islandJag = 0.25f;
    constexpr float islandDome = 0.08f;

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

SurfaceGrid surfaceGrid(int rows, int columns, bool wrapRows, const std::function<Vec3(int row, int column)>& point) {
    if (rows < 1 || columns < 3 || (wrapRows && rows < 3)) {
        throw std::invalid_argument("A wrapped surface needs at least 3 columns and a row, or 3 rows when they wrap");
    }

    SurfaceGrid grid{.rows = rows, .columns = columns, .wrapRows = wrapRows, .points = {}};
    const int pointRows = wrapRows ? rows : rows + 1;
    grid.points.reserve(static_cast<std::size_t>(pointRows * columns));

    for (int row = 0; row < pointRows; ++row) {
        for (int column = 0; column < columns; ++column) {
            grid.points.push_back(point(row, column));
        }
    }

    return grid;
}

Mesh wrappedSurfaceMesh(int rows, int columns, bool wrapRows, const std::function<Vec3(int row, int column)>& point) {
    return wrappedSurfaceMesh(surfaceGrid(rows, columns, wrapRows, point));
}

namespace {
    // A wrapped surface's triangles (each corner's point) and each point's smooth normal, shared by both builders.
    struct SurfaceFaces {
        std::vector<std::array<std::size_t, 3>> triangles;
        std::vector<Vec3> normals;
    };

    SurfaceFaces surfaceFaces(const SurfaceGrid& grid) {
        const int pointRows = grid.wrapRows ? grid.rows : grid.rows + 1;
        const auto index = [&](int row, int column) {
            return static_cast<std::size_t>((row % pointRows) * grid.columns + (column % grid.columns));
        };

        SurfaceFaces faces;
        faces.triangles.reserve(static_cast<std::size_t>(2 * grid.rows * grid.columns));
        std::vector<Vec3> sums(grid.points.size(), Vec3{0.0f, 0.0f, 0.0f});

        // Each triangle's unnormalised normal is twice its area long, so adding them weights each by its area.
        const auto addFace = [&](std::size_t first, std::size_t second, std::size_t third) {
            const Vec3& a = grid.points[first];
            const Vec3 areaNormal = (grid.points[second] - a).cross(grid.points[third] - a);

            if (areaNormal.lengthSquared() <= 0.0f) {
                return;
            }

            faces.triangles.push_back({first, second, third});

            for (const std::size_t corner : {first, second, third}) {
                sums[corner] = sums[corner] + areaNormal;
            }
        };

        for (int row = 0; row < grid.rows; ++row) {
            for (int column = 0; column < grid.columns; ++column) {
                const std::size_t here = index(row, column);
                const std::size_t around = index(row, column + 1);
                const std::size_t up = index(row + 1, column);
                const std::size_t both = index(row + 1, column + 1);
                addFace(here, around, both);
                addFace(here, both, up);
            }
        }

        faces.normals.reserve(sums.size());

        for (const Vec3& sum : sums) {
            faces.normals.push_back(sum.lengthSquared() > 0.0f ? sum.normalized() : Vec3{0.0f, 0.0f, 0.0f});
        }

        return faces;
    }

    Vec2 gridUv(const SurfaceGrid& grid, std::size_t point) {
        const int row = static_cast<int>(point) / grid.columns;
        const int column = static_cast<int>(point) % grid.columns;
        return Vec2{static_cast<float>(column) / static_cast<float>(grid.columns), static_cast<float>(row) / static_cast<float>(grid.rows)};
    }
}

Mesh wrappedSurfaceMesh(const SurfaceGrid& grid) {
    const SurfaceFaces faces = surfaceFaces(grid);
    Mesh mesh;
    mesh.vertices.reserve(grid.points.size());

    for (const Vec3& point : grid.points) {
        mesh.vertices.push_back(Vec4{point.x, point.y, point.z, 1.0f});
    }

    mesh.triangles.reserve(faces.triangles.size());

    for (const std::array<std::size_t, 3>& corners : faces.triangles) {
        Triangle triangle{corners[0], corners[1], corners[2]};

        for (std::size_t slot = 0; slot < 3; ++slot) {
            triangle.normals[slot] = faces.normals[corners[slot]];
            triangle.uvs[slot] = gridUv(grid, corners[slot]);
        }

        mesh.triangles.push_back(triangle);
    }

    return mesh;
}

/*
* The tangent is the way round the surface at each point (to the next column less the last, so it is the same either
* side of a point), made square to its normal; at a pole, where a row is one point, any direction square to the normal.
*/
IndexedMesh wrappedSurfaceIndexedMesh(const SurfaceGrid& grid) {
    const SurfaceFaces faces = surfaceFaces(grid);
    IndexedMesh mesh;
    mesh.vertices.reserve(grid.points.size());

    for (std::size_t point = 0; point < grid.points.size(); ++point) {
        const std::size_t row = point / static_cast<std::size_t>(grid.columns);
        const std::size_t column = point % static_cast<std::size_t>(grid.columns);
        const std::size_t columns = static_cast<std::size_t>(grid.columns);
        const Vec3 round = grid.points[row * columns + (column + 1) % columns] - grid.points[row * columns + (column + columns - 1) % columns];
        const Vec3& normal = faces.normals[point];
        Vec3 tangent = round - normal * round.dot(normal);

        if (tangent.lengthSquared() <= 1e-12f) {
            const Vec3 side = std::fabs(normal.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
            tangent = side - normal * side.dot(normal);
        }

        tangent = tangent.lengthSquared() > 0.0f ? tangent.normalized() : Vec3{1.0f, 0.0f, 0.0f};
        mesh.vertices.push_back(MeshVertex{grid.points[point], normal, gridUv(grid, point), Vec4{tangent.x, tangent.y, tangent.z, 1.0f}});
    }

    mesh.indices.reserve(faces.triangles.size() * 3);

    for (const std::array<std::size_t, 3>& corners : faces.triangles) {
        for (const std::size_t corner : corners) {
            mesh.indices.push_back(static_cast<std::uint32_t>(corner));
        }
    }

    return mesh;
}

float carvingWarp(const RockCarving& carving, float x, float z) {
    const int size = static_cast<int>(carving.warp.width);

    if (size == 0 || carving.warp.height != carving.warp.width || carving.warpTileMetres <= 0.0f) {
        return 0.0f;
    }

    // Texel centres at k + 1/2, so the place in texels less a half is between centres; wrap round the repeat.
    const float u = x / carving.warpTileMetres * static_cast<float>(size) - 0.5f;
    const float v = z / carving.warpTileMetres * static_cast<float>(size) - 0.5f;
    const float cellU = std::floor(u);
    const float cellV = std::floor(v);
    const float shareU = u - cellU;
    const float shareV = v - cellV;
    const auto texel = [&](float column, float row) {
        const int wrappedColumn = (static_cast<int>(column) % size + size) % size;
        const int wrappedRow = (static_cast<int>(row) % size + size) % size;
        const std::size_t index = (static_cast<std::size_t>(wrappedRow) * static_cast<std::size_t>(size) + static_cast<std::size_t>(wrappedColumn)) * 4;
        return static_cast<float>(carving.warp.pixels[index]) / 255.0f;
    };
    const float bottom = texel(cellU, cellV) + (texel(cellU + 1.0f, cellV) - texel(cellU, cellV)) * shareU;
    const float top = texel(cellU, cellV + 1.0f) + (texel(cellU + 1.0f, cellV + 1.0f) - texel(cellU, cellV + 1.0f)) * shareU;
    const float warpTexel = bottom + (top - bottom) * shareV;
    return (2.0f * warpTexel - 1.0f) * carving.warpMetres;
}

float strataRelief(const std::vector<StrataBand>& bands, float height) {
    for (std::size_t index = 0; index < bands.size(); ++index) {
        const StrataBand& band = bands[index];

        if (height >= band.top && index + 1 < bands.size()) {
            continue;
        }

        if (band.hard) {
            return rockLedgeMetres;
        }

        // Soft rock: cut back under a hard bed above it, deepest right under it.
        const float under = band.top - height;

        if (index + 1 < bands.size() && bands[index + 1].hard && under < rockUndercutReach) {
            return -rockUndercutMetres * (1.0f - std::max(under, 0.0f) / rockUndercutReach);
        }

        return 0.0f;
    }

    return 0.0f;
}

/*
* Noise read round a circle as many lattice cells round as the joints are (the radius over their spacing), slowly
* along the height, so it wanders as it rises; a joint is where the noise crosses its middle (1 - |n| near 1), narrowed
* by a smoothstep to a crack.
*/
float jointGroove(float angleRadians, float height, float radius, std::uint32_t seed) {
    const float cells = std::max(radius / rockJointSpacing, 1.0f);
    const float noise = forestFractalNoise(cells * std::cos(angleRadians) + 11.0f, cells * std::sin(angleRadians) + height / 250.0f, 2, seed);
    return smoothStep(0.82f, 1.0f, 1.0f - std::fabs(noise));
}

namespace {
    // The world place of a point of a shape placed at origin and turned about the up axis, as its Transform places it.
    Vec3 placed(const Vec3& origin, float turnRadians, const Vec3& local) {
        return origin + Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, turnRadians).rotate(local);
    }

    /*
    * The carved radius at a point of a rock's surface: its plain radius plus its beds' relief (at the world height of the
    * point on the plain surface, waved as the shader waves the beds), less its joints, never under a third of the plain
    * radius. carve (0..1) scales the carving, to fade it out where a shape narrows to a point.
    */
    float carvedRadius(float plain, float angle, float height, const Vec3& worldPlace, float jointRadius, std::uint32_t seed, const RockCarving& carving,
                       float carve) {
        const float bedHeight = worldPlace.y + carvingWarp(carving, worldPlace.x, worldPlace.z);
        const float relief = strataRelief(carving.bands, bedHeight) - rockJointDepth * jointGroove(angle, height, jointRadius, seed);
        return std::max(plain + carve * relief, plain / 3.0f);
    }
}

/*
* Rows rise up the column, one every rockRowMetres, then four close the top: shrinking to its centre over the last
* metre and a half, their heights jagged, a broken, nearly flat top where trees can stand. Columns go round, one every
* rockColumnMetres. The large-scale shape (taper, bulges, lean) is read in shares of the height, as the first pillars'
* was; the joints and beds in metres, the same size on every pillar.
*/
SurfaceGrid pillarSurface(const PillarPlacement& pillar, const RockCarving& carving) {
    const float radius = pillar.radius;
    const float height = pillar.height;
    const float buried = pillarBuriedDepth * height;
    const int bodyRows = std::max(8, static_cast<int>(std::ceil((height + buried) / rockRowMetres)));
    const std::array<float, 4> capShrink{0.9f, 0.65f, 0.3f, 0.0f};
    const std::array<float, 4> capRise{0.5f, 1.0f, 1.4f, 1.6f};
    const int rows = bodyRows + static_cast<int>(capShrink.size());
    const int columns = std::clamp(static_cast<int>(std::lround(fullTurnRadians * radius / rockColumnMetres)), 24, 160);
    const std::uint32_t seed = pillar.seed;

    // The axis leaning by slow noise along the height, in metres.
    const auto lean = [&](float share) {
        return Vec2{pillarLean * radius * forestFractalNoise(1.5f * share, 0.5f, 2, forestShapeSeed(seed, 3)),
                    pillarLean * radius * forestFractalNoise(0.5f, 1.5f * share, 2, forestShapeSeed(seed, 4))};
    };

    // A point of the body at a height (metres above the foot) and column.
    const auto bodyPoint = [&](float y, int column) {
        const float angle = fullTurnRadians * static_cast<float>(column) / static_cast<float>(columns);
        const float around = std::cos(angle);
        const float across = std::sin(angle);
        const float share = std::clamp(y / height, 0.0f, 1.0f);
        const float bulge = pillarBulge * forestFractalNoise(1.3f * around + 5.0f, 1.3f * across + 4.0f * share, 3, forestShapeSeed(seed, 1));
        const float plain = radius * (1.0f - pillarTaper * share) * (1.0f + bulge);
        const Vec2 axis = lean(share);
        const Vec3 local{axis.x + plain * across, y, axis.y + plain * around};
        const float carved = carvedRadius(plain, angle, y, placed(pillar.foot, pillar.turnRadians, local), plain, forestShapeSeed(seed, 2), carving, 1.0f);
        return Vec3{axis.x + carved * across, y, axis.y + carved * around};
    };

    return surfaceGrid(rows, columns, false, [&](int row, int column) {
        if (row <= bodyRows) {
            return bodyPoint(-buried + (height + buried) * static_cast<float>(row) / static_cast<float>(bodyRows), column);
        }

        const std::size_t cap = static_cast<std::size_t>(row - bodyRows - 1);
        const Vec3 rim = bodyPoint(height, column);
        const Vec2 axis = lean(1.0f);
        const float angle = fullTurnRadians * static_cast<float>(column) / static_cast<float>(columns);
        const float jag = cap + 1 < capShrink.size() ? 1.2f * forestFractalNoise(3.0f * std::cos(angle), 3.0f * std::sin(angle) + static_cast<float>(cap), 2, forestShapeSeed(seed, 6)) : 0.0f;
        const float shrink = capShrink[cap];
        return Vec3{axis.x + (rim.x - axis.x) * shrink, height + capRise[cap] + jag, axis.y + (rim.z - axis.y) * shrink};
    });
}

/*
* Rows rise from the tip to the rim (the underside, widening fast, then slowly), one every rockRowMetres of its depth,
* then on inwards over the top to its centre. The rim's jag continues over the top's edge, so the two meet. The
* underside is carved like a pillar, the carving fading out towards the tip, where the shape narrows to a point.
*/
SurfaceGrid islandSurface(const IslandPlacement& island, const RockCarving& carving) {
    const float radius = island.radius;
    const int underRows = std::clamp(static_cast<int>(std::ceil(islandDepth * radius / rockRowMetres)), 12, 120);
    const int topRows = std::clamp(static_cast<int>(std::ceil(radius / (2.0f * rockRowMetres))), 6, 40);
    const int rows = underRows + topRows;
    const int columns = std::clamp(static_cast<int>(std::lround(fullTurnRadians * radius / (1.2f * rockColumnMetres))), 32, 240);
    const std::uint32_t seed = island.seed;

    return surfaceGrid(rows, columns, false, [&](int row, int column) {
        const float angle = fullTurnRadians * static_cast<float>(column) / static_cast<float>(columns);
        const float around = std::cos(angle);
        const float across = std::sin(angle);
        const auto jag = [&](float rise) {
            return 1.0f + islandJag * forestFractalNoise(2.5f * around + 3.0f, 2.5f * across + 6.0f * rise, 3, forestShapeSeed(seed, 5));
        };

        if (row <= underRows) {
            const float rise = static_cast<float>(row) / static_cast<float>(underRows);
            const float plain = radius * std::pow(rise, 0.55f) * jag(rise);
            const float y = -islandDepth * radius * std::pow(1.0f - rise, 1.3f);
            const Vec3 local{plain * across, y, plain * around};
            const float carved = carvedRadius(plain, angle, y, placed(island.rim, island.turnRadians, local), radius, forestShapeSeed(seed, 2), carving,
                                              smoothStep(0.05f, 0.35f, rise));
            return Vec3{carved * across, y, carved * around};
        }

        const float inward = static_cast<float>(row - underRows) / static_cast<float>(topRows) * (0.25f * fullTurnRadians);
        const float plain = radius * std::max(0.0f, std::cos(inward)) * jag(1.0f); // exactly 0 at the centre
        const float bumps = 0.02f * radius * forestFractalNoise(4.0f * plain / radius * around, 4.0f * plain / radius * across, 2, forestShapeSeed(seed, 6));
        return Vec3{plain * across, islandDome * radius * std::sin(inward) + bumps, plain * around};
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
        const float z = pillarNorthmost + (pillarSouthmost - pillarNorthmost) * forestUnitRandom(pillarPlaceSeed, base);
        const float x = valleyAxis(z) + offAxis(pillarSpread, forestUnitRandom(pillarPlaceSeed, base + 1), forestUnitRandom(pillarPlaceSeed, base + 2));
        const float radius = pillarThinnest + pillarThickestExtra * std::pow(forestUnitRandom(pillarPlaceSeed, base + 3), 1.5f);

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
            .height = pillarShortest + pillarTallestExtra * forestUnitRandom(pillarPlaceSeed, base + 4),
            .turnRadians = fullTurnRadians * forestUnitRandom(pillarPlaceSeed, base + 5),
            .seed = forestShapeSeed(static_cast<std::uint32_t>(attempt), 7)
        });
    }

    return pillars;
}

std::vector<IslandPlacement> placeIslands(const ValleyLayout& layout, const std::vector<PillarPlacement>& pillars) {
    std::vector<IslandPlacement> islands;
    const int tries = std::max(layout.islandCount, 0) * triesPerShape;

    for (int attempt = 0; attempt < tries && static_cast<int>(islands.size()) < layout.islandCount; ++attempt) {
        const std::uint64_t base = static_cast<std::uint64_t>(attempt) * 8u;
        const float z = islandNorthmost + (islandSouthmost - islandNorthmost) * forestUnitRandom(islandPlaceSeed, base);
        const float x = valleyAxis(z) + offAxis(islandSpread, std::sqrt(forestUnitRandom(islandPlaceSeed, base + 1)), forestUnitRandom(islandPlaceSeed, base + 2));
        const float radius = islandSmallest + islandLargestExtra * std::pow(forestUnitRandom(islandPlaceSeed, base + 3), 2.0f);
        const float tip = std::max(forestHeight(x, z), 0.0f) + islandLowest + islandHighestExtra * forestUnitRandom(islandPlaceSeed, base + 4);

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
            .turnRadians = fullTurnRadians * forestUnitRandom(islandPlaceSeed, base + 5),
            .seed = forestShapeSeed(static_cast<std::uint32_t>(attempt), 8)
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
    return PillarPlacement{.foot = Vec3{x, foot, z}, .radius = 45.0f, .height = groundLevel + 470.0f - foot, .turnRadians = 0.0f, .seed = forestShapeSeed(0, 9)};
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
