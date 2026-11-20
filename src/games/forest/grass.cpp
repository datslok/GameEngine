#include "games/forest/grass.h"
#include "games/forest/forest_terrain.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>

namespace {
    const Vec3 up{0.0f, 1.0f, 0.0f};

    // A patch's blades in its full mesh, its level and its far level: segments, and the share of the blades each keeps.
    constexpr int fullSegments = 4;
    constexpr int levelSegments = 3;
    constexpr int farSegments = 2;
    constexpr float levelShare = 0.5f;
    constexpr float farShare = 0.25f;

    // How far a blade's normals lean up (the up direction's weight against the face's unit normal), and out towards its
    // edges (the across direction's weight).
    constexpr float normalUpWeight = 1.5f;
    constexpr float normalRounding = 0.5f;

    // A blade's control point: this share of its height straight above its root, so it rises before it bends.
    constexpr float controlRise = 0.6f;

    // A clump's lean (the tip's sideways share of the height) between these, each blade's 60-140% of it, its direction
    // within bladeLeanJitter (radians) of the clump's.
    constexpr float leastClumpLean = 0.1f;
    constexpr float mostClumpLean = 0.45f;
    constexpr float bladeLeanJitter = 0.6f;

    // The forest's grass cells' seed: their variants and turns.
    constexpr std::uint64_t grassCellSeed = 0x47524153ull;

    // Each blade's random draws (root across, root along, height, lean share, lean turn, facing, width).
    constexpr std::uint64_t drawsPerBlade = 8;
    constexpr std::uint64_t bladeDrawsStart = 1u << 20;

    // A clump: a site, its height share, lean direction and amount, and tone band.
    struct Clump {
        Vec2 site;
        float height = 1.0f;
        float leanAngle = 0.0f;
        float lean = 0.2f;
        int band = 0;
    };

    // The clumps over a patch's square: one jittered site in each square of clumpMetres (a column count a side).
    struct Clumps {
        std::vector<Clump> clumps;
        int across = 1;
        float spacing = 1.0f;
        float half = 0.0f;
    };

    Clumps makeClumps(const ForestGrassSettings& settings, std::uint32_t seed) {
        Clumps result;
        result.half = 0.5f * settings.cellMetres;
        result.across = std::max(1, static_cast<int>(std::ceil(settings.cellMetres / std::max(settings.clumpMetres, 0.05f))));
        result.spacing = settings.cellMetres / static_cast<float>(result.across);

        for (int row = 0; row < result.across; ++row) {
            for (int column = 0; column < result.across; ++column) {
                const std::uint64_t index = static_cast<std::uint64_t>(row * result.across + column) * 6;
                Clump clump;
                clump.site = Vec2{-result.half + (static_cast<float>(column) + forestUnitRandom(seed, index)) * result.spacing,
                                  -result.half + (static_cast<float>(row) + forestUnitRandom(seed, index + 1)) * result.spacing};
                clump.height = grassLeastClumpHeight + (grassMostClumpHeight - grassLeastClumpHeight) * forestUnitRandom(seed, index + 2);
                clump.leanAngle = fullTurnRadians * forestUnitRandom(seed, index + 3);
                clump.lean = leastClumpLean + (mostClumpLean - leastClumpLean) * forestUnitRandom(seed, index + 4);
                clump.band = std::min(grassToneBands - 1, static_cast<int>(forestUnitRandom(seed, index + 5) * static_cast<float>(grassToneBands)));
                result.clumps.push_back(clump);
            }
        }

        return result;
    }

    // The clump nearest a place (its site is within its own square, so the nearest is among the 3 x 3 round it).
    int nearestClump(const Clumps& clumps, const Vec2& place) {
        const int column = std::clamp(static_cast<int>((place.x + clumps.half) / clumps.spacing), 0, clumps.across - 1);
        const int row = std::clamp(static_cast<int>((place.y + clumps.half) / clumps.spacing), 0, clumps.across - 1);
        int nearest = row * clumps.across + column;
        float best = (clumps.clumps[static_cast<std::size_t>(nearest)].site - place).lengthSquared();

        for (int r = std::max(row - 1, 0); r <= std::min(row + 1, clumps.across - 1); ++r) {
            for (int c = std::max(column - 1, 0); c <= std::min(column + 1, clumps.across - 1); ++c) {
                const int index = r * clumps.across + c;
                const float distance = (clumps.clumps[static_cast<std::size_t>(index)].site - place).lengthSquared();

                if (distance < best) {
                    best = distance;
                    nearest = index;
                }
            }
        }

        return nearest;
    }

    // A vertex whose tangent runs along +u (across) and whose bitangent runs along +v (up the blade).
    MeshVertex bladeVertex(const Vec3& position, const Vec3& normal, const Vec3& across, const Vec3& along, const Vec2& uv) {
        const float handedness = normal.cross(across).dot(along) >= 0.0f ? 1.0f : -1.0f;
        return MeshVertex{position, normal, uv, Vec4{across.x, across.y, across.z, handedness}};
    }

    // A triangle wound counterclockwise seen from the side facing points to.
    void appendWound(IndexedMesh& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c, const Vec3& facing) {
        const Vec3 normal = (mesh.vertices[b].position - mesh.vertices[a].position).cross(mesh.vertices[c].position - mesh.vertices[a].position);

        if (normal.dot(facing) >= 0.0f) {
            mesh.indices.insert(mesh.indices.end(), {a, b, c});
        } else {
            mesh.indices.insert(mesh.indices.end(), {a, c, b});
        }
    }

    // The patch's meshes from its blades: the first share of them, widened, in segments each.
    IndexedMesh bladesMesh(const std::vector<GrassBlade>& blades, float share, float widen, int segments) {
        IndexedMesh mesh;
        const std::size_t count = std::max<std::size_t>(blades.empty() ? 0 : 1, static_cast<std::size_t>(std::lround(share * static_cast<float>(blades.size()))));

        for (std::size_t index = 0; index < count; ++index) {
            GrassBlade blade = blades[index];
            blade.width *= widen;
            appendGrassBlade(mesh, blade, segments);
        }

        return mesh;
    }

    std::uint8_t channelByte(float value) {
        return static_cast<std::uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
    }
}

void visit(ForestGrassSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Cell", settings.cellMetres, {.min = 1.0f, .max = 64.0f, .sliderMax = 16.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Blades per m2", settings.bladesPerSquareMetre, {.min = 0.0f, .max = 1000.0f, .sliderMax = 200.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Shortest", settings.shortest, {.min = 0.01f, .max = 3.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Tallest", settings.tallest, {.min = 0.01f, .max = 3.0f, .sliderMax = 1.5f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Blade width", settings.bladeWidth, {.min = 0.001f, .max = 0.1f, .sliderMax = 0.04f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Clump", settings.clumpMetres, {.min = 0.05f, .max = 10.0f, .sliderMax = 3.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Variants", settings.variants, {.min = 1, .max = 16, .sliderMax = 8, .applies = FieldApplies::AtStartup});
    visitor.field("Density steps", settings.densitySteps, {.min = 1, .max = 16, .sliderMax = 8, .applies = FieldApplies::AtStartup});
    visitor.field("Reach", settings.reachMetres, {.min = 0.0f, .max = 2000.0f, .sliderMax = 500.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Least paint", settings.leastPaint, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Half distance", settings.halfDistance, {.min = 1.0f, .max = 500.0f, .sliderMax = 60.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Quarter distance", settings.quarterDistance, {.min = 1.0f, .max = 500.0f, .sliderMax = 100.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Farthest", settings.farthest, {.min = 1.0f, .max = 1000.0f, .sliderMax = 200.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Level cover", settings.levelCover, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
}

/*
* The curve is quadratic Bezier from the root through a control point straight above it to the tip, leaning: its
* points B(t) = (1 - t)^2 P0 + 2 t (1 - t) P1 + t^2 P2, its direction B'(t) = 2 (1 - t)(P1 - P0) + 2 t (P2 - P1).
*/
void appendGrassBlade(IndexedMesh& mesh, const GrassBlade& blade, int segments) {
    const int rows = std::max(segments, 1);
    const Vec3 lean = Vec3{blade.lean.x, 0.0f, blade.lean.z}.lengthSquared() > 1e-8f ? Vec3{blade.lean.x, 0.0f, blade.lean.z}.normalized()
                                                                                      : Vec3{1.0f, 0.0f, 0.0f};
    const float share = std::clamp(blade.leanShare, 0.0f, 0.95f);
    const Vec3 control = blade.root + up * (controlRise * blade.height);
    const Vec3 tip = blade.root + up * (blade.height * std::sqrt(1.0f - share * share)) + lean * (blade.height * share);
    const Vec3 across{std::cos(blade.facingRadians), 0.0f, -std::sin(blade.facingRadians)};
    const float uFrom = static_cast<float>(blade.band) / static_cast<float>(grassToneBands);
    const float uWidth = 1.0f / static_cast<float>(grassToneBands);
    const std::uint32_t first = static_cast<std::uint32_t>(mesh.vertices.size());
    Vec3 faceNormal = up;

    for (int row = 0; row <= rows; ++row) {
        const float s = static_cast<float>(row) / static_cast<float>(rows);
        const float t = 1.0f - std::pow(1.0f - s, 1.5f);
        const Vec3 point = blade.root * ((1.0f - t) * (1.0f - t)) + control * (2.0f * t * (1.0f - t)) + tip * (t * t);
        Vec3 along = (control - blade.root) * (2.0f * (1.0f - t)) + (tip - control) * (2.0f * t);
        along = along.lengthSquared() > 1e-12f ? along.normalized() : up;
        Vec3 face = across.cross(along);
        face = face.lengthSquared() > 1e-12f ? face.normalized() : Vec3{0.0f, 0.0f, 1.0f};

        if (row == 0) {
            faceNormal = face;
        }

        if (row == rows) {
            mesh.vertices.push_back(bladeVertex(point, (face + up * normalUpWeight).normalized(), across, along, Vec2{uFrom + 0.5f * uWidth, 1.0f}));
            break;
        }

        const float halfWidth = 0.5f * blade.width * (1.0f - t * t);
        const Vec3 leftNormal = ((face - across * normalRounding).normalized() + up * normalUpWeight).normalized();
        const Vec3 rightNormal = ((face + across * normalRounding).normalized() + up * normalUpWeight).normalized();
        mesh.vertices.push_back(bladeVertex(point - across * halfWidth, leftNormal, across, along, Vec2{uFrom, t}));
        mesh.vertices.push_back(bladeVertex(point + across * halfWidth, rightNormal, across, along, Vec2{uFrom + uWidth, t}));
    }

    // Each segment a quad of two triangles, the last one a triangle to the tip, all wound to face the blade's face.
    for (int row = 0; row < rows; ++row) {
        const std::uint32_t left = first + 2 * static_cast<std::uint32_t>(row);
        const std::uint32_t right = left + 1;

        if (row + 1 == rows) {
            appendWound(mesh, left, right, left + 2, faceNormal);
            continue;
        }

        appendWound(mesh, left, right, right + 2, faceNormal);
        appendWound(mesh, left, right + 2, left + 2, faceNormal);
    }
}

GrassPatch growGrassPatch(const ForestGrassSettings& settings, float density, std::uint32_t seed) {
    GrassPatch patch;
    const Clumps clumps = makeClumps(settings, seed);
    const float area = settings.cellMetres * settings.cellMetres;
    const int full = static_cast<int>(std::lround(std::max(settings.bladesPerSquareMetre, 0.0f) * area));
    const int count = static_cast<int>(std::lround(std::clamp(density, 0.0f, 1.0f) * static_cast<float>(full)));

    for (int index = 0; index < count; ++index) {
        const std::uint64_t base = bladeDrawsStart + static_cast<std::uint64_t>(index) * drawsPerBlade;
        const auto draw = [&](std::uint64_t k) {
            return forestUnitRandom(seed, base + k);
        };
        const Vec2 place{(draw(0) - 0.5f) * settings.cellMetres, (draw(1) - 0.5f) * settings.cellMetres};
        const int clumpIndex = nearestClump(clumps, place);
        const Clump& clump = clumps.clumps[static_cast<std::size_t>(clumpIndex)];
        const float leanAngle = clump.leanAngle + bladeLeanJitter * (2.0f * draw(4) - 1.0f);

        GrassBlade blade;
        blade.root = Vec3{place.x, 0.0f, place.y};
        blade.height = (settings.shortest + (settings.tallest - settings.shortest) * draw(2)) * clump.height;
        blade.lean = Vec3{std::cos(leanAngle), 0.0f, std::sin(leanAngle)};
        blade.leanShare = clump.lean * (0.6f + 0.8f * draw(3));
        blade.facingRadians = fullTurnRadians * draw(5);
        blade.width = settings.bladeWidth * (0.8f + 0.4f * draw(6));
        blade.band = clump.band;
        blade.clump = clumpIndex;
        patch.blades.push_back(blade);
    }

    patch.mesh = bladesMesh(patch.blades, 1.0f, 1.0f, fullSegments);
    patch.level = bladesMesh(patch.blades, levelShare, grassLevelWidening(settings.levelCover, levelShare), levelSegments);
    patch.farLevel = bladesMesh(patch.blades, farShare, grassLevelWidening(settings.levelCover, farShare), farSegments);
    return patch;
}

float grassLevelWidening(float cover, float share) {
    return std::max(cover, 0.0f) / std::max(share, 1e-6f);
}

int grassDensityStep(const ForestGrassSettings& settings, float paint) {
    return groundDensityStep(settings.densitySteps, settings.leastPaint, paint);
}

/*
* Each band's tone multiplies red, green and blue: from a deeper, bluer green to a lighter, yellower one. Up the blade
* the shade grows from the root's 55% to full, and past dryFrom the tip turns towards straw.
*/
ImageData grassBladeImage(int texels, Pixel colour) {
    constexpr float toneRed[grassToneBands] = {0.85f, 0.95f, 1.08f, 1.22f};
    constexpr float toneGreen[grassToneBands] = {0.82f, 0.95f, 1.05f, 1.15f};
    constexpr float toneBlue[grassToneBands] = {0.95f, 0.95f, 0.85f, 0.75f};
    constexpr float rootShade = 0.55f;
    constexpr float dryFrom = 0.85f;
    constexpr float streak = 0.06f;
    constexpr std::uint32_t streakSeed = 0x53545245u;
    const std::uint32_t size = static_cast<std::uint32_t>(std::max(texels, grassToneBands));
    ImageData image;
    image.width = size;
    image.height = size;
    image.encoding = TextureEncoding::Srgb;
    image.pixels.reserve(static_cast<std::size_t>(size) * size * 4);

    for (std::uint32_t y = 0; y < size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        const float shade = rootShade + (1.0f - rootShade) * std::pow(v, 0.7f);
        const float dry = std::clamp((v - dryFrom) / (1.0f - dryFrom), 0.0f, 1.0f);

        for (std::uint32_t x = 0; x < size; ++x) {
            const int band = std::min(grassToneBands - 1, static_cast<int>(x * static_cast<std::uint32_t>(grassToneBands) / size));
            const float lines = 1.0f + streak * (2.0f * forestUnitRandom(streakSeed, x) - 1.0f);
            const float red = static_cast<float>(colour.r) * toneRed[band] * shade * lines * (1.0f + 0.35f * dry);
            const float green = static_cast<float>(colour.g) * toneGreen[band] * shade * lines * (1.0f - 0.1f * dry);
            const float blue = static_cast<float>(colour.b) * toneBlue[band] * shade * lines * (1.0f - 0.2f * dry);
            image.pixels.insert(image.pixels.end(), {channelByte(red), channelByte(green), channelByte(blue), 255});
        }
    }

    return image;
}

std::vector<GrassCell> forestGrassCells(const ForestGrassSettings& settings, JobSystem& jobs) {
    return forestGroundCells(GroundCellOptions{.cellMetres = settings.cellMetres,
                                               .reachMetres = settings.reachMetres,
                                               .leastPaint = settings.leastPaint,
                                               .densitySteps = settings.densitySteps,
                                               .variants = settings.variants,
                                               .layer = forestGrassLayer,
                                               .seed = grassCellSeed},
                             jobs);
}