#include "games/forest/forest_terrain.h"

#include "core/hash.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <cmath>

namespace {
    // The noise's seeds: one per use, so the hills, the lumps, the openings and the paint's breakup do not line up.
    constexpr std::uint32_t hillSeed = 11u;
    constexpr std::uint32_t lumpSeed = 23u;
    constexpr std::uint32_t openingSeed = 31u;
    constexpr std::uint32_t breakupSeed = 47u;

    /*
    * The land's shape, picked by eye against the walk: broad hills and hollows about 100 m across standing up to about
    * 9 m from the mean, rockier knolls a quarter that size, and lumps a few metres across. The mist pools in the hollows.
    */
    constexpr float hillMetres = 100.0f;
    constexpr float hillHeight = 9.0f;
    constexpr float knollMetres = 28.0f;
    constexpr float knollHeight = 7.0f;
    constexpr float lumpMetres = 6.0f;
    constexpr float lumpHeight = 0.25f;

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

    // The path's level: the land averaged over this many metres either way along it, in this many samples each way.
    constexpr float pathSmoothingMetres = 12.0f;
    constexpr int pathSmoothingSamples = 4;

    // The step for the normal's central differences, in metres.
    constexpr float normalStep = 0.5f;

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

    // How much the clearing levels a place: 1 within its radius, fading to 0 across its blend.
    float clearingShare(float x, float z) {
        return 1.0f - smoothStep(forestClearingRadius, forestClearingRadius + forestClearingBlend, std::sqrt(x * x + z * z));
    }

    // The land before the path: the hills set so the clearing's centre is at 0, and the clearing levelled.
    float landHeight(float x, float z) {
        static const float clearingLevel = hills(0.0f, 0.0f);
        return (hills(x, z) - clearingLevel) * (1.0f - clearingShare(x, z));
    }

    /*
    * The path's level a distance along it: the land's height averaged over pathSmoothingMetres either way along the
    * path, so it follows the hills but cuts through the knolls and over the dips between them, as a trodden path does,
    * and stays walkable however rocky the ground beside it. The clearing levels it as it levels the land.
    */
    float pathLevel(float metresAlong) {
        float sum = 0.0f;

        for (int sample = -pathSmoothingSamples; sample <= pathSmoothingSamples; ++sample) {
            const float offset = pathSmoothingMetres * static_cast<float>(sample) / static_cast<float>(pathSmoothingSamples);
            const Vec2 point = pathPointAlong(metresAlong + offset);
            sum += landHeight(point.x, point.y);
        }

        const Vec2 centre = pathPointAlong(metresAlong);
        const float smoothed = sum / static_cast<float>(2 * pathSmoothingSamples + 1);
        return smoothed * (1.0f - clearingShare(centre.x, centre.y)) - forestPathSink;
    }

    float pathShare(float distance) {
        return 1.0f - smoothStep(forestPathHalfWidth, forestPathHalfWidth + forestPathBlend, distance);
    }
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
        sum += amplitude * (2.0f * forestValueNoise(x * frequency, z * frequency, seed + static_cast<std::uint32_t>(octave) * 101u) - 1.0f);
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return total > 0.0f ? sum / total : 0.0f;
}

const std::vector<Vec2>& forestPathPoints() {
    static const std::vector<Vec2> points{
        Vec2{0.0f, 0.0f}, Vec2{4.0f, 30.0f}, Vec2{-7.0f, 60.0f}, Vec2{3.0f, 92.0f}, Vec2{19.0f, 122.0f}, forestMeadowCentre
    };
    return points;
}

float forestPathDistance(float x, float z) {
    return nearestPathPoint(x, z).distance;
}

/*
* Near the path the ground takes the height of the nearest point on its centre line, so it is level across; along it,
* it follows the land, smoothed (pathLevel).
*/
float forestHeight(float x, float z) {
    const PathPoint nearest = nearestPathPoint(x, z);
    const float share = pathShare(nearest.distance);
    const float land = landHeight(x, z);

    if (share <= 0.0f) {
        return land;
    }

    return land + (pathLevel(nearest.along) - land) * share;
}

Vec3 forestNormal(float x, float z) {
    const float slopeX = (forestHeight(x + normalStep, z) - forestHeight(x - normalStep, z)) / (2.0f * normalStep);
    const float slopeZ = (forestHeight(x, z + normalStep) - forestHeight(x, z - normalStep)) / (2.0f * normalStep);
    return Vec3{-slopeX, 1.0f, -slopeZ}.normalized();
}

float forestMeadowShare(float x, float z) {
    const float fromCentre = std::sqrt(x * x + z * z);
    const float clearing = 1.0f - smoothStep(forestClearingRadius, forestClearingRadius + 5.0f, fromCentre);
    const float fromMeadow = (Vec2{x, z} - forestMeadowCentre).length();
    const float meadow = 1.0f - smoothStep(0.7f * forestMeadowRadius, forestMeadowRadius, fromMeadow);
    const float opening = smoothStep(openingFrom, openingTo, forestFractalNoise(x / openingMetres, z / openingMetres, 2, openingSeed));
    return std::max({clearing, meadow, opening});
}

/*
* The path's mud first, then mossy rock on the steep ground left, then grass where the canopy is open and leaf litter
* where it is closed, their edge made ragged by fine noise.
*/
std::array<float, 4> forestGroundPaint(float x, float z) {
    const float pathDistance = forestPathDistance(x, z);
    const float mud = 1.0f - smoothStep(mudInnerShare * forestPathHalfWidth, forestPathHalfWidth + mudOuterMetres, pathDistance);
    const float slope = 1.0f - forestNormal(x, z).y;
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

float forestTextureU(float x) {
    return (x + forestHalfSize) / (2.0f * forestHalfSize);
}

float forestTextureV(float z) {
    return (z + forestHalfSize) / (2.0f * forestHalfSize);
}

float forestTexelCentre(int texel, int size) {
    return -forestHalfSize + (static_cast<float>(texel) + 0.5f) * (2.0f * forestHalfSize / static_cast<float>(size));
}

/*
* Rows run north to south (z rising), columns west to east; each square's two triangles turn counterclockwise seen from
* above, so they face up.
*/
Mesh forestTerrainMesh(int squares) {
    Mesh mesh;
    const int across = squares + 1;
    const float spacing = 2.0f * forestHalfSize / static_cast<float>(squares);
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    mesh.vertices.reserve(static_cast<std::size_t>(across * across));
    normals.reserve(mesh.vertices.capacity());
    uvs.reserve(mesh.vertices.capacity());

    for (int row = 0; row < across; ++row) {
        for (int column = 0; column < across; ++column) {
            const float x = -forestHalfSize + spacing * static_cast<float>(column);
            const float z = -forestHalfSize + spacing * static_cast<float>(row);
            mesh.vertices.push_back(Vec4{x, forestHeight(x, z), z, 1.0f});
            normals.push_back(forestNormal(x, z));
            uvs.push_back(Vec2{forestTextureU(x), forestTextureV(z)});
        }
    }

    const auto corner = [&](std::size_t vertex, Triangle& triangle, std::size_t slot) {
        triangle.normals[slot] = normals[vertex];
        triangle.uvs[slot] = uvs[vertex];
    };
    const auto addTriangle = [&](std::size_t first, std::size_t second, std::size_t third) {
        Triangle triangle{first, second, third};
        corner(first, triangle, 0);
        corner(second, triangle, 1);
        corner(third, triangle, 2);
        mesh.triangles.push_back(triangle);
    };

    mesh.triangles.reserve(static_cast<std::size_t>(2 * squares * squares));

    for (int row = 0; row < squares; ++row) {
        for (int column = 0; column < squares; ++column) {
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

ImageData forestWeightsImage(int size) {
    ImageData image;
    image.width = static_cast<std::uint32_t>(size);
    image.height = static_cast<std::uint32_t>(size);
    image.encoding = TextureEncoding::Linear;
    image.pixels.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);

    for (int row = 0; row < size; ++row) {
        for (int column = 0; column < size; ++column) {
            const std::array<float, 4> paint = forestGroundPaint(forestTexelCentre(column, size), forestTexelCentre(row, size));
            const std::size_t texel = (static_cast<std::size_t>(row) * static_cast<std::size_t>(size) + static_cast<std::size_t>(column)) * 4;

            for (std::size_t layer = 0; layer < 4; ++layer) {
                image.pixels[texel + layer] = static_cast<std::uint8_t>(std::lround(std::clamp(paint[layer], 0.0f, 1.0f) * 255.0f));
            }
        }
    }

    return image;
}
