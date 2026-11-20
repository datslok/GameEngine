#include "games/forest/floor_cover.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/island_hangings.h"
#include "games/forest/palms.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>

namespace {
    const Vec3 up{0.0f, 1.0f, 0.0f};

    // The golden angle: each leaf or frond turned this far round from the last.
    constexpr float goldenAngle = 2.39996323f;

    // A fallen leaf: its width a share of its length, its midrib's height off the ground, the share of fresh green ones.
    constexpr float fallenWidthShare = 0.55f;
    constexpr float fallenLift = 0.004f;
    constexpr float freshShare = 0.1f;
    constexpr float mostCurl = 0.012f;

    // A seedling's stem: its radius and how far under the ground it starts; its leaves' width share, fold and droop.
    constexpr float seedlingStemRadius = 0.003f;
    constexpr float seedlingSink = 0.02f;
    constexpr float seedlingLeafWidth = 0.4f;
    constexpr float seedlingLeafFoldDegrees = 10.0f;
    constexpr float seedlingLeafDroop = 0.15f;

    // A small fern's fronds: their leaflet pairs and width, droop (radians), and how far above the ground they leave.
    constexpr int fernPairs = 10;
    constexpr float fernLeafletWidth = 0.18f;
    constexpr float fernDroop = 1.0f;
    constexpr float fernLift = 0.02f;

    // The forest floor's cells' seed.
    constexpr std::uint64_t floorCellSeed = 0x464c4f4full;

    // Each piece's random draws start apart: leaves, seedlings and ferns.
    constexpr std::uint64_t drawsPerPiece = 8;
    constexpr std::uint64_t seedlingDrawsStart = 1u << 22;
    constexpr std::uint64_t fernDrawsStart = 1u << 23;

    // u within a band: its share across (0 to 1) moved into the band.
    float bandU(int band, float across) {
        return (static_cast<float>(band) + std::clamp(across, 0.0f, 1.0f)) / static_cast<float>(floorCoverBands);
    }

    // The vertices from first on moved into the green band (their u, 0 to 1 across, made the band's).
    void intoGreenBand(IndexedMesh& mesh, std::size_t first) {
        for (std::size_t vertex = first; vertex < mesh.vertices.size(); ++vertex) {
            mesh.vertices[vertex].uv.x = bandU(floorGreenBand, mesh.vertices[vertex].uv.x);
        }
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

    // The first share of pieces (at least one when there are any) appended by append.
    template <typename Piece, typename Append>
    void appendShare(IndexedMesh& mesh, const std::vector<Piece>& pieces, float share, Append append) {
        const std::size_t count = pieces.empty() ? 0
                                                 : std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(share * static_cast<float>(pieces.size()))));

        for (std::size_t index = 0; index < count; ++index) {
            append(mesh, pieces[index]);
        }
    }

    // A patch's mesh of a share of its leaves and of its plants.
    IndexedMesh patchMesh(const FloorPatch& patch, float leafShare, float plantShare) {
        IndexedMesh mesh;
        appendShare(mesh, patch.leaves, leafShare, appendFallenLeaf);
        appendShare(mesh, patch.seedlings, plantShare, appendSeedling);
        appendShare(mesh, patch.ferns, plantShare, appendFloorFern);
        return mesh;
    }

    std::uint8_t channelByte(float value) {
        return static_cast<std::uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
    }
}

void visit(ForestFloorSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Cell", settings.cellMetres, {.min = 1.0f, .max = 64.0f, .sliderMax = 16.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Leaves per m2", settings.leavesPerSquareMetre, {.min = 0.0f, .max = 500.0f, .sliderMax = 150.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Leaf length", settings.leafLength, {.min = 0.01f, .max = 1.0f, .sliderMax = 0.4f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Seedlings per m2", settings.seedlingsPerSquareMetre, {.min = 0.0f, .max = 20.0f, .sliderMax = 4.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Shortest seedling", settings.shortestSeedling, {.min = 0.02f, .max = 3.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Tallest seedling", settings.tallestSeedling, {.min = 0.02f, .max = 3.0f, .sliderMax = 1.5f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Ferns per m2", settings.fernsPerSquareMetre, {.min = 0.0f, .max = 10.0f, .sliderMax = 2.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Fern length", settings.fernLength, {.min = 0.05f, .max = 3.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    // At most 8 x 4 patch kinds: a planting layer's (PLANTING_MOST_KINDS).
    visitor.field("Variants", settings.variants, {.min = 1, .max = 8, .applies = FieldApplies::AtStartup});
    visitor.field("Density steps", settings.densitySteps, {.min = 1, .max = 4, .applies = FieldApplies::AtStartup});
    visitor.field("Reach", settings.reachMetres, {.min = 0.0f, .max = 2000.0f, .sliderMax = 500.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Least paint", settings.leastPaint, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Half distance", settings.halfDistance, {.min = 1.0f, .max = 500.0f, .sliderMax = 60.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Quarter distance", settings.quarterDistance, {.min = 1.0f, .max = 500.0f, .sliderMax = 100.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Farthest", settings.farthest, {.min = 1.0f, .max = 1000.0f, .sliderMax = 200.0f, .unit = "m", .applies = FieldApplies::AtStartup});
}

/*
* The outline in the leaf's own units (x across, 0 to 1; y from the stalk, 0 to 1): stalk, two points either side and
* the tip, round a middle point on the midrib, each triangle a fan slice from it.
*/
void appendFallenLeaf(IndexedMesh& mesh, const FallenLeaf& leaf) {
    struct Corner {
        float x;
        float y;
        float lift;
    };

    const float curl = leaf.curl;
    const Corner corners[] = {{0.5f, 0.0f, 0.0f}, {0.08f, 0.35f, curl}, {0.2f, 0.72f, curl}, {0.5f, 1.0f, 0.5f * curl},
                              {0.8f, 0.72f, curl}, {0.92f, 0.35f, curl}, {0.5f, 0.5f, 0.0f}};
    const Vec3 along{std::sin(leaf.turnRadians), 0.0f, std::cos(leaf.turnRadians)};
    const Vec3 across{along.z, 0.0f, -along.x};
    const float width = fallenWidthShare * leaf.length;
    const std::uint32_t first = static_cast<std::uint32_t>(mesh.vertices.size());

    for (const Corner& corner : corners) {
        const Vec3 position = leaf.centre + across * ((corner.x - 0.5f) * width) + along * ((corner.y - 0.5f) * leaf.length) +
                              up * (fallenLift + corner.lift);
        const float handedness = up.cross(across).dot(along) >= 0.0f ? 1.0f : -1.0f;
        mesh.vertices.push_back(MeshVertex{position, up, Vec2{bandU(leaf.band, corner.x), corner.y}, Vec4{across.x, across.y, across.z, handedness}});
    }

    const std::uint32_t middle = first + 6;

    for (std::uint32_t edge = 0; edge < 6; ++edge) {
        appendWound(mesh, first + edge, first + (edge + 1) % 6, middle, up);
    }
}

void appendSeedling(IndexedMesh& mesh, const Seedling& seedling) {
    const std::size_t first = mesh.vertices.size();
    HangingStrand stem;
    stem.points = {seedling.root - up * seedlingSink, seedling.root + up * (0.5f * seedling.height), seedling.root + up * seedling.height};
    stem.radii = {seedlingStemRadius, 0.8f * seedlingStemRadius, 0.6f * seedlingStemRadius};
    appendTube(mesh, stem, 3, 1.0f);

    // The stem reads the band's middle, its midrib.
    for (std::size_t vertex = first; vertex < mesh.vertices.size(); ++vertex) {
        mesh.vertices[vertex].uv.x = 0.5f;
    }

    const StrapFold fold{.foldRadians = radiansFromDegrees(seedlingLeafFoldDegrees), .droop = seedlingLeafDroop};
    const int leaves = std::max(seedling.leaves, 1);

    for (int leaf = 0; leaf < leaves; ++leaf) {
        const float share = leaves > 1 ? static_cast<float>(leaf) / static_cast<float>(leaves - 1) : 1.0f;
        const float height = seedling.height * (0.5f + 0.5f * share);
        const float azimuth = seedling.turnRadians + goldenAngle * static_cast<float>(leaf);
        const float elevation = radiansFromDegrees(20.0f + 20.0f * forestUnitRandom(seedling.seed, static_cast<std::uint64_t>(leaf)));
        const Vec3 level{std::cos(azimuth), 0.0f, -std::sin(azimuth)};
        const Vec3 direction = level * std::cos(elevation) + up * std::sin(elevation);
        const float length = seedling.height * (0.25f + 0.15f * forestUnitRandom(seedling.seed, 100 + static_cast<std::uint64_t>(leaf)));
        appendStrapLeaf(mesh, seedling.root + up * height, direction, up, length, seedlingLeafWidth * length, 3, fold);
    }

    intoGreenBand(mesh, first);
}

void appendFloorFern(IndexedMesh& mesh, const FloorFern& fern) {
    const std::size_t first = mesh.vertices.size();
    std::uint64_t draw = 0;
    const std::function<float()> random = [&] {
        return forestUnitRandom(fern.seed, draw++);
    };
    const float phase = fullTurnRadians * random();

    for (int frond = 0; frond < fern.fronds; ++frond) {
        const float azimuth = phase + goldenAngle * static_cast<float>(frond);
        const float elevation = radiansFromDegrees(50.0f + 20.0f * random());
        const float length = fern.length * (0.8f + 0.2f * random());
        appendPinnateFrond(mesh,
                           PinnateFrond{.base = fern.root + up * fernLift, .azimuth = azimuth, .elevation = elevation, .length = length, .droop = fernDroop,
                                        .pairs = fernPairs, .leafletWidthShare = fernLeafletWidth, .rachisSides = 3, .leafletSegments = 2},
                           random);
    }

    intoGreenBand(mesh, first);
}

FloorPatch growFloorPatch(const ForestFloorSettings& settings, float density, std::uint32_t seed) {
    FloorPatch patch;
    const float area = settings.cellMetres * settings.cellMetres;
    const float share = std::clamp(density, 0.0f, 1.0f);
    const auto count = [&](float perSquareMetre) {
        return static_cast<int>(std::lround(share * std::max(perSquareMetre, 0.0f) * area));
    };
    const auto place = [&](float x, float z) {
        return Vec3{(x - 0.5f) * settings.cellMetres, 0.0f, (z - 0.5f) * settings.cellMetres};
    };

    for (int index = 0; index < count(settings.leavesPerSquareMetre); ++index) {
        const std::uint64_t base = static_cast<std::uint64_t>(index) * drawsPerPiece;
        const auto draw = [&](std::uint64_t k) {
            return forestUnitRandom(seed, base + k);
        };
        const bool fresh = draw(5) < freshShare;
        patch.leaves.push_back(FallenLeaf{.centre = place(draw(0), draw(1)),
                                          .length = settings.leafLength * (0.6f + draw(2)),
                                          .turnRadians = fullTurnRadians * draw(3),
                                          .curl = mostCurl * draw(4),
                                          .band = fresh ? floorGreenBand : std::min(floorGreenBand - 1, static_cast<int>(draw(6) * static_cast<float>(floorGreenBand)))});
    }

    for (int index = 0; index < count(settings.seedlingsPerSquareMetre); ++index) {
        const std::uint64_t base = seedlingDrawsStart + static_cast<std::uint64_t>(index) * drawsPerPiece;
        const auto draw = [&](std::uint64_t k) {
            return forestUnitRandom(seed, base + k);
        };
        patch.seedlings.push_back(Seedling{.root = place(draw(0), draw(1)),
                                           .height = settings.shortestSeedling + (settings.tallestSeedling - settings.shortestSeedling) * draw(2),
                                           .leaves = 2 + std::min(4, static_cast<int>(draw(3) * 5.0f)),
                                           .turnRadians = fullTurnRadians * draw(4),
                                           .seed = static_cast<std::uint32_t>(base)});
    }

    for (int index = 0; index < count(settings.fernsPerSquareMetre); ++index) {
        const std::uint64_t base = fernDrawsStart + static_cast<std::uint64_t>(index) * drawsPerPiece;
        const auto draw = [&](std::uint64_t k) {
            return forestUnitRandom(seed, base + k);
        };
        patch.ferns.push_back(FloorFern{.root = place(draw(0), draw(1)),
                                        .length = settings.fernLength * (0.8f + 0.2f * draw(2)),
                                        .fronds = 4 + std::min(3, static_cast<int>(draw(3) * 4.0f)),
                                        .seed = static_cast<std::uint32_t>(base)});
    }

    patch.mesh = patchMesh(patch, 1.0f, 1.0f);
    patch.level = patchMesh(patch, 0.5f, 1.0f);
    patch.farLevel = patchMesh(patch, 0.25f, 0.5f);
    return patch;
}

/*
* Each band's leaf: its tone the litter's darker (0.6), as it is, yellowed (more red and green, less blue), or the green;
* a midrib down its middle a little darker, veins slanting out from it towards the tip, mottled, edges darker.
*/
ImageData floorCoverImage(int texels, Pixel litter, Pixel green) {
    constexpr float veinsAlong = 7.0f;
    constexpr float veinSlant = 2.0f;
    constexpr float veinHalfWidth = 0.08f;
    constexpr float midribHalfWidth = 0.05f;
    constexpr float edgeDarkening = 0.3f;
    constexpr float mottle = 0.1f;
    constexpr std::uint32_t mottleSeed = 0x4c495454u;
    const float tones[floorCoverBands][3] = {{0.6f * litter.r, 0.6f * litter.g, 0.6f * litter.b},
                                             {1.0f * litter.r, 1.0f * litter.g, 1.0f * litter.b},
                                             {1.25f * litter.r, 1.1f * litter.g, 0.7f * litter.b},
                                             {1.0f * green.r, 1.0f * green.g, 1.0f * green.b}};
    const std::uint32_t size = static_cast<std::uint32_t>(std::max(texels, 1));
    const std::uint32_t width = size * static_cast<std::uint32_t>(floorCoverBands);
    ImageData image;
    image.width = width;
    image.height = size;
    image.encoding = TextureEncoding::Srgb;
    image.pixels.reserve(static_cast<std::size_t>(width) * size * 4);

    for (std::uint32_t y = 0; y < size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);

        for (std::uint32_t x = 0; x < width; ++x) {
            const int band = static_cast<int>(x / size);
            const float u = (static_cast<float>(x % size) + 0.5f) / static_cast<float>(size);
            const float out = std::fabs(u - 0.5f) * 2.0f;
            const float midrib = std::max(0.0f, 1.0f - out / midribHalfWidth);
            const float veinPlace = v * veinsAlong - out * veinSlant;
            const float vein = out > midribHalfWidth ? std::max(0.0f, 1.0f - std::fabs(veinPlace - std::round(veinPlace)) / veinHalfWidth) : 0.0f;
            const std::uint64_t cell = static_cast<std::uint64_t>(x / 4) + 4096 * static_cast<std::uint64_t>(y / 4);
            const float shade = (1.0f + mottle * (2.0f * forestUnitRandom(mottleSeed, cell) - 1.0f)) * (1.0f - edgeDarkening * out * out) *
                                (1.0f - 0.15f * midrib) * (1.0f - 0.1f * vein);
            const float* tone = tones[band];
            image.pixels.insert(image.pixels.end(), {channelByte(tone[0] * shade), channelByte(tone[1] * shade), channelByte(tone[2] * shade), 255});
        }
    }

    return image;
}

std::vector<GroundCell> forestFloorCells(const ForestFloorSettings& settings, JobSystem& jobs) {
    return forestGroundCells(GroundCellOptions{.cellMetres = settings.cellMetres,
                                               .reachMetres = settings.reachMetres,
                                               .leastPaint = settings.leastPaint,
                                               .densitySteps = settings.densitySteps,
                                               .variants = settings.variants,
                                               .layer = forestLitterLayer,
                                               .seed = floorCellSeed},
                             jobs);
}
