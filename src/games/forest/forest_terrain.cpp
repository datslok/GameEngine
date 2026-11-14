#include "games/forest/forest_terrain.h"

#include "core/hash.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    // The noise's seeds: one per use, so the hills, the lumps, the mountains, the openings and the paint's breakup do not
    // line up.
    constexpr std::uint32_t hillSeed = 11u;
    constexpr std::uint32_t lumpSeed = 23u;
    constexpr std::uint32_t openingSeed = 31u;
    constexpr std::uint32_t breakupSeed = 47u;
    constexpr std::uint32_t mountainSeed = 59u;

    /*
    * The small shapes over the whole land, picked by eye against the walk in the first forest: broad hills and hollows
    * about 100 m across standing up to about 9 m from the mean, rockier knolls a quarter that size, and lumps a few metres
    * across.
    */
    constexpr float hillMetres = 100.0f;
    constexpr float hillHeight = 9.0f;
    constexpr float knollMetres = 28.0f;
    constexpr float knollHeight = 7.0f;
    constexpr float lumpMetres = 6.0f;
    constexpr float lumpHeight = 0.25f;

    /*
    * The mountains beyond the rim: rising over the 300 m to 2.5 km past it to between 60% and 100% of 700 m by noise
    * about 2.5 km across, so the valley seen from its rim is ringed by a skyline the haze turns blue.
    */
    constexpr float mountainHeight = 700.0f;
    constexpr float mountainMetres = 2500.0f;
    constexpr float mountainFrom = 300.0f;
    constexpr float mountainTo = 2500.0f;

    // The valley's axis: straight along the walk, then swinging up to 400 m either way over about 11 km a turn further
    // north, so the valley's far end is not a corridor.
    constexpr float axisStraightUntil = -700.0f;
    constexpr float axisBlend = 800.0f;
    constexpr float axisSwing = 400.0f;
    constexpr float axisMetres = 1800.0f;

    // Where the canopy opens away from the clearing and the meadow: wide patches where the opening noise is high.
    constexpr float openingMetres = 70.0f;
    constexpr float openingFrom = 0.3f;
    constexpr float openingTo = 0.5f;

    // The grass's ragged edge against the leaf litter: the meadow's share moved by up to this much by fine noise.
    constexpr float breakupMetres = 3.0f;
    constexpr float breakupStrength = 0.3f;

    // Mossy rock takes over where the ground is steeper than about 23 degrees, entirely past about 33 (one less the
    // normal's up component).
    constexpr float rockFromSlope = 0.08f;
    constexpr float rockToSlope = 0.16f;

    // The mud's edge within the path's level strip, ragged by the blend's own height maps.
    constexpr float mudInnerShare = 0.7f;
    constexpr float mudOuterMetres = 0.8f;

    // The path's level: the land averaged over this many metres either way along it, in this many samples each way:
    // 30 m, so the knolls on the ramp do not make it steeper than about 23 degrees (12 m, as on the first forest's
    // gentle hills, left 33).
    constexpr float pathSmoothingMetres = 30.0f;
    constexpr int pathSmoothingSamples = 6;

    // The ramp's shape: mostly a straight slope (70%), eased in at its foot and out at the rim by the rest, so its
    // steepest part is a quarter less steep than an eased curve's.
    constexpr float rampStraightShare = 0.7f;

    // The rings' half sizes: each four times the last, the last twice (20.48 km), and how much coarser each ring's
    // squares are than the finest.
    constexpr std::array<float, 4> ringHalfSizes{forestNearHalfSize, 4.0f * forestNearHalfSize, 16.0f * forestNearHalfSize, 32.0f * forestNearHalfSize};
    constexpr std::array<float, 4> ringCoarseness{1.0f, 8.0f, 32.0f, 128.0f};

    /*
    * Each octave of the fractal noise is turned and shifted on its own, the first by atan(3/4) and each next by the
    * golden angle more: value noise interpolates along its lattice's axes, so its slopes (and the rock painted on them,
    * the openings' edges) run along x and z; summed unturned, a third of the slopes lay within 10 degrees of an axis and
    * the ground looked square (the user's look at the rough valley), turned about 22%, as random directions do.
    */
    constexpr float firstOctaveTurn = 0.6435011f;
    constexpr float octaveTurnStep = 2.3999632f;
    constexpr float octaveShiftX = 17.31f;
    constexpr float octaveShiftZ = -9.87f;

    // A place on a ring's border, within float rounding of grid points made from whole numbers of squares.
    constexpr float borderTolerance = 1e-3f;

    float latticeValue(int x, int z, std::uint32_t seed) {
        const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
        const std::uint64_t bits = mixBits(key ^ mixBits(seed + 0x9e3779b97f4a7c15ull));
        return static_cast<float>(bits >> 40) * (1.0f / 16777216.0f);
    }

    // The quintic fade 6t^5 - 15t^4 + 10t^3: flat at both ends in slope and curvature, so cells join without creases.
    float fade(float t) {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    struct PathPoint {
        Vec2 point;
        float distance;
        float along; // metres along the path from its start
    };

    PathPoint nearestPathPoint(float x, float z) {
        const std::vector<Vec2>& path = forestPathPoints();
        const Vec2 place{x, z};
        PathPoint nearest{path.front(), (place - path.front()).length(), 0.0f};
        float walked = 0.0f;

        for (std::size_t point = 0; point + 1 < path.size(); ++point) {
            const Vec2 start = path[point];
            const Vec2 along = path[point + 1] - start;
            const float share = std::clamp((place - start).dot(along) / along.lengthSquared(), 0.0f, 1.0f);
            const Vec2 closest = start + along * share;
            const float distance = (place - closest).length();

            if (distance < nearest.distance) {
                nearest = PathPoint{closest, distance, walked + share * along.length()};
            }

            walked += along.length();
        }

        return nearest;
    }

    // The point on the path a distance along it from its start, held at its ends.
    Vec2 pathPointAlong(float metres) {
        const std::vector<Vec2>& path = forestPathPoints();
        float left = std::max(metres, 0.0f);

        for (std::size_t point = 0; point + 1 < path.size(); ++point) {
            const Vec2 along = path[point + 1] - path[point];
            const float length = along.length();

            if (left <= length) {
                return path[point] + along * (left / length);
            }

            left -= length;
        }

        return path.back();
    }

    float hills(float x, float z) {
        return hillHeight * forestFractalNoise(x / hillMetres, z / hillMetres, 3, hillSeed) +
               knollHeight * forestFractalNoise(x / knollMetres, z / knollMetres, 2, hillSeed + 1u) +
               lumpHeight * forestFractalNoise(x / lumpMetres, z / lumpMetres, 2, lumpSeed);
    }

    /*
    * The valley's big shape: the floor at 0, the walls climbing to the rim either side of the axis, the ramp climbing to
    * it southwards, and the mountains beyond, by how far a place is outside the valley.
    */
    float valleyShape(float x, float z) {
        const float fromAxis = std::fabs(x - valleyAxis(z));
        const float wall = smoothStep(valleyFloorHalfWidth, valleyFloorHalfWidth + valleyWallWidth, fromAxis);
        const float rampLinear = std::clamp((z - valleyRampStart) / (valleyRampEnd - valleyRampStart), 0.0f, 1.0f);
        const float ramp = rampStraightShare * rampLinear + (1.0f - rampStraightShare) * smoothStep(0.0f, 1.0f, rampLinear);
        const float outside = std::max({fromAxis - valleyFloorHalfWidth - valleyWallWidth, z - valleyRampEnd, 0.0f});
        const float rise = 0.6f + 0.4f * (0.5f + 0.5f * forestFractalNoise(x / mountainMetres, z / mountainMetres, 4, mountainSeed));
        const float mountains = mountainHeight * smoothStep(mountainFrom, mountainTo, outside) * rise;
        return valleyRimHeight * std::max(wall, ramp) + mountains;
    }

    // How much the clearing levels a place: 1 within its radius, fading to 0 across its blend.
    float clearingShare(float x, float z) {
        const float distance = (Vec2{x, z} - forestClearingCentre).length();
        return 1.0f - smoothStep(forestClearingRadius, forestClearingRadius + forestClearingBlend, distance);
    }

    // The land before the path: the valley and its hills, levelled in the clearing to the height at its centre.
    float landHeight(float x, float z) {
        const float land = valleyShape(x, z) + hills(x, z);
        static const float clearingLevel = valleyShape(forestClearingCentre.x, forestClearingCentre.y) +
                                           hills(forestClearingCentre.x, forestClearingCentre.y);
        return land + (clearingLevel - land) * clearingShare(x, z);
    }

    /*
    * The path's level a distance along it: the land's height averaged over pathSmoothingMetres either way along the
    * path, so it follows the land but cuts through the knolls and over the dips between them, as a trodden path does,
    * and stays walkable however rocky the ground beside it.
    */
    float pathLevel(float metresAlong) {
        float sum = 0.0f;

        for (int sample = -pathSmoothingSamples; sample <= pathSmoothingSamples; ++sample) {
            const float offset = pathSmoothingMetres * static_cast<float>(sample) / static_cast<float>(pathSmoothingSamples);
            const Vec2 point = pathPointAlong(metresAlong + offset);
            sum += landHeight(point.x, point.y);
        }

        return sum / static_cast<float>(2 * pathSmoothingSamples + 1) - forestPathSink;
    }

    float pathShare(float distance) {
        return 1.0f - smoothStep(forestPathHalfWidth, forestPathHalfWidth + forestPathBlend, distance);
    }

    bool isWhole(float value) {
        return std::fabs(value - std::round(value)) < 1e-4f;
    }

    /*
    * A ring's height along one border: between the next ring's grid points either side (along, measured from the ring's
    * west or north edge), on the straight line joining their heights, as the coarser ring draws that edge.
    */
    float coarseEdgeHeight(float along, float across, bool alongX, const TerrainRing& ring) {
        const float start = -ring.outerHalfSize;
        const float steps = (along - start) / ring.outerSpacing;
        const float lower = std::floor(steps);
        const float share = steps - lower;
        const float first = start + lower * ring.outerSpacing;
        const float second = first + ring.outerSpacing;
        const auto heightAt = [&](float point) {
            return alongX ? forestHeight(forestTerrainCentre.x + point, forestTerrainCentre.y + across)
                          : forestHeight(forestTerrainCentre.x + across, forestTerrainCentre.y + point);
        };

        if (share < 1e-6f) {
            return heightAt(first);
        }

        return heightAt(first) + (heightAt(second) - heightAt(first)) * share;
    }
}

float forestUnitRandom(std::uint64_t seed, std::uint64_t index) {
    return static_cast<float>(mixBits(seed ^ mixBits(index + 0x9e3779b97f4a7c15ull)) >> 40) * (1.0f / 16777216.0f);
}

std::uint32_t forestShapeSeed(std::uint32_t seed, std::uint32_t use) {
    return static_cast<std::uint32_t>(mixBits((static_cast<std::uint64_t>(seed) << 8) | use));
}

float forestValueNoise(float x, float z, std::uint32_t seed) {
    const float cellX = std::floor(x);
    const float cellZ = std::floor(z);
    const int column = static_cast<int>(cellX);
    const int row = static_cast<int>(cellZ);
    const float u = fade(x - cellX);
    const float v = fade(z - cellZ);
    const float near = latticeValue(column, row, seed) + (latticeValue(column + 1, row, seed) - latticeValue(column, row, seed)) * u;
    const float far = latticeValue(column, row + 1, seed) + (latticeValue(column + 1, row + 1, seed) - latticeValue(column, row + 1, seed)) * u;
    return near + (far - near) * v;
}

float forestFractalNoise(float x, float z, int octaves, std::uint32_t seed) {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float total = 0.0f;
    float frequency = 1.0f;

    for (int octave = 0; octave < octaves; ++octave) {
        const float turn = firstOctaveTurn + octaveTurnStep * static_cast<float>(octave);
        const float cosine = std::cos(turn);
        const float sine = std::sin(turn);
        const float u = (cosine * x - sine * z) * frequency + octaveShiftX * static_cast<float>(octave);
        const float v = (sine * x + cosine * z) * frequency + octaveShiftZ * static_cast<float>(octave);
        sum += amplitude * (2.0f * forestValueNoise(u, v, seed + static_cast<std::uint32_t>(octave) * 101u) - 1.0f);
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return total > 0.0f ? sum / total : 0.0f;
}

// Down the ramp in wide bends, through the Ring and along the floor into the clearing.
const std::vector<Vec2>& forestPathPoints() {
    static const std::vector<Vec2> points{
        forestStartPlace, Vec2{25.0f, 660.0f}, Vec2{-20.0f, 530.0f}, Vec2{15.0f, 400.0f}, Vec2{-15.0f, 270.0f},
        Vec2{10.0f, 140.0f}, forestRingPlace, Vec2{-10.0f, -140.0f}, Vec2{0.0f, -270.0f}
    };
    return points;
}

float forestPathDistance(float x, float z) {
    return nearestPathPoint(x, z).distance;
}

float valleyAxis(float z) {
    const float winding = 1.0f - smoothStep(axisStraightUntil - axisBlend, axisStraightUntil, z);
    return axisSwing * winding * std::sin((z - axisStraightUntil) / axisMetres);
}

/*
* Near the path the ground takes the height of the nearest point on its centre line, so it is level across; along it,
* it follows the land, smoothed (pathLevel).
*/
float forestHeight(float x, float z) {
    const float land = landHeight(x, z);
    const PathPoint nearest = nearestPathPoint(x, z);
    const float share = pathShare(nearest.distance);

    if (share <= 0.0f) {
        return land;
    }

    return land + (pathLevel(nearest.along) - land) * share;
}

Vec3 forestNormal(float x, float z, float step) {
    const float slopeX = (forestHeight(x + step, z) - forestHeight(x - step, z)) / (2.0f * step);
    const float slopeZ = (forestHeight(x, z + step) - forestHeight(x, z - step)) / (2.0f * step);
    return Vec3{-slopeX, 1.0f, -slopeZ}.normalized();
}

float forestMeadowShare(float x, float z) {
    const float clearing = clearingShare(x, z);
    const float fromStart = (Vec2{x, z} - forestStartPlace).length();
    const float meadow = 1.0f - smoothStep(0.7f * forestMeadowRadius, forestMeadowRadius, fromStart);
    const float opening = smoothStep(openingFrom, openingTo, forestFractalNoise(x / openingMetres, z / openingMetres, 2, openingSeed));
    return std::max({clearing, meadow, opening});
}

/*
* The path's mud first, then mossy rock on the steep ground left, then grass where the canopy is open and leaf litter
* where it is closed, their edge made ragged by fine noise.
*/
std::array<float, 4> forestGroundPaint(float x, float z, float normalStep) {
    const float pathDistance = forestPathDistance(x, z);
    const float mud = 1.0f - smoothStep(mudInnerShare * forestPathHalfWidth, forestPathHalfWidth + mudOuterMetres, pathDistance);
    const float slope = 1.0f - forestNormal(x, z, normalStep).y;
    const float rock = smoothStep(rockFromSlope, rockToSlope, slope) * (1.0f - mud);
    const float rest = 1.0f - mud - rock;
    const float breakup = breakupStrength * forestFractalNoise(x / breakupMetres, z / breakupMetres, 2, breakupSeed);
    const float open = std::clamp(forestMeadowShare(x, z) + breakup, 0.0f, 1.0f);

    std::array<float, 4> paint{};
    paint[forestGrassLayer] = rest * open;
    paint[forestLitterLayer] = rest * (1.0f - open);
    paint[forestMudLayer] = mud;
    paint[forestRockLayer] = rock;
    return paint;
}

float forestTextureU(float x, const GroundExtent& extent) {
    return (x - extent.centre.x + extent.halfSize) / (2.0f * extent.halfSize);
}

float forestTextureV(float z, const GroundExtent& extent) {
    return (z - extent.centre.y + extent.halfSize) / (2.0f * extent.halfSize);
}

float forestTexelCentreX(int texel, int size, const GroundExtent& extent) {
    return extent.centre.x - extent.halfSize + (static_cast<float>(texel) + 0.5f) * (2.0f * extent.halfSize / static_cast<float>(size));
}

float forestTexelCentreZ(int texel, int size, const GroundExtent& extent) {
    return extent.centre.y - extent.halfSize + (static_cast<float>(texel) + 0.5f) * (2.0f * extent.halfSize / static_cast<float>(size));
}

std::vector<TerrainRing> forestTerrainRings(float nearSpacing) {
    std::vector<TerrainRing> rings;

    for (std::size_t ring = 0; ring < ringHalfSizes.size(); ++ring) {
        const bool last = ring + 1 == ringHalfSizes.size();
        rings.push_back(TerrainRing{
            .innerHalfSize = ring == 0 ? 0.0f : ringHalfSizes[ring - 1],
            .outerHalfSize = ringHalfSizes[ring],
            .spacing = nearSpacing * ringCoarseness[ring],
            .outerSpacing = nearSpacing * ringCoarseness[last ? ring : ring + 1]
        });
    }

    for (const TerrainRing& ring : rings) {
        if (!(nearSpacing > 0.0f) || !isWhole(ring.outerHalfSize / ring.outerSpacing) || !isWhole(ring.innerHalfSize / ring.spacing) ||
            !isWhole(ring.outerSpacing / ring.spacing)) {
            throw std::invalid_argument("The terrain rings need the finest squares 2, 4 or 8 m across");
        }
    }

    return rings;
}

GroundExtent forestFarExtent() {
    return GroundExtent{forestTerrainCentre, ringHalfSizes.back()};
}

GroundExtent forestNearExtent() {
    return GroundExtent{forestTerrainCentre, forestNearHalfSize};
}

float forestRingHeight(float x, float z, const TerrainRing& ring) {
    if (ring.outerSpacing == ring.spacing) {
        return forestHeight(x, z);
    }

    const float localX = x - forestTerrainCentre.x;
    const float localZ = z - forestTerrainCentre.y;

    if (std::fabs(std::fabs(localZ) - ring.outerHalfSize) < borderTolerance) {
        return coarseEdgeHeight(localX, localZ, true, ring);
    }

    if (std::fabs(std::fabs(localX) - ring.outerHalfSize) < borderTolerance) {
        return coarseEdgeHeight(localZ, localX, false, ring);
    }

    return forestHeight(x, z);
}

/*
* Rows run north to south (z rising), columns west to east; each square's two triangles turn counterclockwise seen from
* above, so they face up. Squares inside the hole are left out, and the grid points only they use are never measured.
*/
Mesh forestTerrainRingMesh(const TerrainRing& ring, const GroundExtent& extent, JobSystem& jobs) {
    Mesh mesh;
    const int squares = static_cast<int>(std::lround(2.0f * ring.outerHalfSize / ring.spacing));
    const int holeSquares = static_cast<int>(std::lround(2.0f * ring.innerHalfSize / ring.spacing));
    const int holeStart = (squares - holeSquares) / 2;
    const int holeEnd = holeStart + holeSquares;
    const int across = squares + 1;
    const auto inHole = [&](int row, int column) {
        return row >= holeStart && row < holeEnd && column >= holeStart && column < holeEnd;
    };
    const auto pointUsed = [&](int row, int column) {
        return !(row > holeStart && row < holeEnd && column > holeStart && column < holeEnd);
    };

    std::vector<Vec3> normals(static_cast<std::size_t>(across) * static_cast<std::size_t>(across));
    std::vector<Vec2> uvs(normals.size());
    mesh.vertices.resize(normals.size());

    jobs.parallelFor(static_cast<std::size_t>(across), 1, [&](std::size_t rowIndex) {
        const int row = static_cast<int>(rowIndex);

        for (int column = 0; column < across; ++column) {
            const float x = forestTerrainCentre.x - ring.outerHalfSize + ring.spacing * static_cast<float>(column);
            const float z = forestTerrainCentre.y - ring.outerHalfSize + ring.spacing * static_cast<float>(row);
            const std::size_t vertex = static_cast<std::size_t>(row * across + column);

            if (!pointUsed(row, column)) {
                mesh.vertices[vertex] = Vec4{x, 0.0f, z, 1.0f};
                normals[vertex] = Vec3{0.0f, 1.0f, 0.0f};
                continue;
            }

            mesh.vertices[vertex] = Vec4{x, forestRingHeight(x, z, ring), z, 1.0f};
            normals[vertex] = forestNormal(x, z, ring.spacing);
            uvs[vertex] = Vec2{forestTextureU(x, extent), forestTextureV(z, extent)};
        }
    });

    const auto addTriangle = [&](std::size_t first, std::size_t second, std::size_t third) {
        Triangle triangle{first, second, third};
        const std::size_t corners[3] = {first, second, third};

        for (std::size_t slot = 0; slot < 3; ++slot) {
            triangle.normals[slot] = normals[corners[slot]];
            triangle.uvs[slot] = uvs[corners[slot]];
        }

        mesh.triangles.push_back(triangle);
    };

    mesh.triangles.reserve(static_cast<std::size_t>(2 * (squares * squares - holeSquares * holeSquares)));

    for (int row = 0; row < squares; ++row) {
        for (int column = 0; column < squares; ++column) {
            if (inHole(row, column)) {
                continue;
            }

            const std::size_t northWest = static_cast<std::size_t>(row * across + column);
            const std::size_t northEast = northWest + 1;
            const std::size_t southWest = northWest + static_cast<std::size_t>(across);
            const std::size_t southEast = southWest + 1;
            addTriangle(northWest, southEast, northEast);
            addTriangle(northWest, southWest, southEast);
        }
    }

    return mesh;
}

ImageData forestWeightsImage(int size, const GroundExtent& extent, JobSystem& jobs) {
    ImageData image;
    image.width = static_cast<std::uint32_t>(size);
    image.height = static_cast<std::uint32_t>(size);
    image.encoding = TextureEncoding::Linear;
    image.pixels.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
    const float texelMetres = 2.0f * extent.halfSize / static_cast<float>(size);

    jobs.parallelFor(static_cast<std::size_t>(size), 1, [&](std::size_t rowIndex) {
        const int row = static_cast<int>(rowIndex);

        for (int column = 0; column < size; ++column) {
            const std::array<float, 4> paint = forestGroundPaint(forestTexelCentreX(column, size, extent), forestTexelCentreZ(row, size, extent),
                                                                 std::max(0.5f, texelMetres));
            const std::size_t texel = (static_cast<std::size_t>(row) * static_cast<std::size_t>(size) + static_cast<std::size_t>(column)) * 4;

            for (std::size_t layer = 0; layer < 4; ++layer) {
                image.pixels[texel + layer] = static_cast<std::uint8_t>(std::lround(std::clamp(paint[layer], 0.0f, 1.0f) * 255.0f));
            }
        }
    });

    return image;
}
