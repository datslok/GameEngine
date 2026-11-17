#include "games/forest/palms.h"

#include "games/forest/forest_terrain.h"
#include "games/forest/island_hangings.h"
#include "games/forest/understorey.h"
#include "math/angles.h"
#include "math/smooth_step.h"

#include <algorithm>
#include <cmath>

namespace {
    const Vec3 up{0.0f, 1.0f, 0.0f};

    // The palms' random numbers: their seed, and each cell's numbers (three placing it, three for its palm).
    constexpr std::uint64_t palmSeed = 0x50414c4dull;
    constexpr std::uint64_t palmDrawsPerCell = plantingDrawsUsed + 3;

    enum PalmDraw : std::uint64_t {
        Variant,
        Scale,
        Turn
    };

    // The golden angle, pi (3 - sqrt 5): each frond turned this far round from the last, so none lines up with another.
    constexpr float goldenAngle = 2.39996323f;

    // The youngest frond (the crown's top) leaves this steeply, the oldest (round the outside) this, each within
    // frondElevationJitter; their droops a share of the settings' between these (the mean 1), each within frondDroopJitter.
    constexpr float youngestElevationDegrees = 72.0f;
    constexpr float oldestElevationDegrees = 35.0f;
    constexpr float frondElevationJitterDegrees = 6.0f;
    constexpr float youngestDroopShare = 0.6f;
    constexpr float oldestDroopShare = 1.4f;
    constexpr float frondDroopJitter = 0.15f;
    constexpr float frondLengthJitter = 0.08f;

    // A frond's rachis: its points, its radius at the crown (a share of its length) tapering by palmRachisTaper, and how
    // far below the crown's top the oldest leaves (a share of its length).
    constexpr int frondPoints = 16;
    constexpr float rachisRadiusShare = 0.01f;
    constexpr float oldestFrondDrop = 0.04f;

    /*
    * A leaflet leaves the rachis this far forward of square to it, lifted this far out of the frond's plane (so the
    * frond is a shallow V), folded along its midrib with its halves this far below level (reduplicate, as most
    * understorey palms), and droops under its own weight, its tip falling this share of its length; each varies its
    * angle and length within these.
    */
    constexpr float leafletAngleDegrees = 60.0f;
    constexpr float leafletLiftDegrees = 15.0f;
    constexpr float leafletFoldDegrees = 25.0f;
    constexpr float leafletDroop = 0.2f;
    constexpr float leafletAngleJitterDegrees = 8.0f;
    constexpr float leafletLengthJitter = 0.1f;

    // No leaflet wider than this share of its length, however few pairs a coarse level keeps.
    constexpr float widestLeafletShare = 0.5f;

    // The stem: its radius a share of the palm's height (at least the least), how far it starts below the ground, how
    // far the crown sits under the top (a share of a frond's length: the youngest frond's tip, drooping, about that
    // high), and the tubes' texture tile along them (metres).
    constexpr float stemRadiusShare = 0.008f;
    constexpr float leastStemRadius = 0.012f;
    constexpr float stemSinkMetres = 0.1f;
    constexpr float crownBelowTop = 0.75f;
    constexpr float stalkTileMetres = 1.0f;
    constexpr float stemStepMetres = 0.5f;

    // The shadow's stand-in: the pairs kept, segments, sides round.
    constexpr PalmDetail shadowDetail{.pairShare = 0.25f, .leafletSegments = 2, .rachisSides = 3, .stemSides = 3};

    // The leaflet's picture: its parallel veins either side of the midrib, the midrib's and a vein's half-widths (shares
    // of the half-leaf, and of the gap between veins), the streaks along it (their changes along v and their strength),
    // the edge's darkening and where the dry tip begins.
    constexpr float veinsPerHalf = 7.0f;
    constexpr float midribHalfWidth = 0.035f;
    constexpr float veinHalfWidth = 0.08f;
    constexpr float streaksAlong = 6.0f;
    constexpr float streakStrength = 0.08f;
    constexpr float edgeDarkening = 0.1f;
    constexpr float dryTipStart = 0.88f;

    std::uint8_t channelByte(float value) {
        return static_cast<std::uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
    }

    // The point and direction at share along of a polyline of equal steps.
    struct SpinePoint {
        Vec3 point;
        Vec3 direction;
    };

    SpinePoint spineAt(const std::vector<Vec3>& spine, float along) {
        const int steps = static_cast<int>(spine.size()) - 1;
        const float place = std::clamp(along, 0.0f, 1.0f) * static_cast<float>(steps);
        const int step = std::min(static_cast<int>(place), steps - 1);
        const float within = place - static_cast<float>(step);
        const Vec3 from = spine[static_cast<std::size_t>(step)];
        const Vec3 to = spine[static_cast<std::size_t>(step) + 1];
        return SpinePoint{from + (to - from) * within, (to - from).normalized()};
    }

    // Two triangles over a quad, each wound to face the way its corners' normals do.
    void appendFacingTriangle(IndexedMesh& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        const MeshVertex& first = mesh.vertices[a];
        const MeshVertex& second = mesh.vertices[b];
        const MeshVertex& third = mesh.vertices[c];
        const Vec3 facing = (second.position - first.position).cross(third.position - first.position);

        if (facing.dot(first.normal + second.normal + third.normal) >= 0.0f) {
            mesh.indices.insert(mesh.indices.end(), {a, b, c});
        } else {
            mesh.indices.insert(mesh.indices.end(), {a, c, b});
        }
    }

    // A vertex whose tangent runs along u and whose bitangent (w cross(normal, tangent)) runs along along, as the
    // shaders' normal mapping takes them.
    MeshVertex leafletVertex(const Vec3& position, const Vec3& normal, const Vec3& tangent, const Vec3& along, const Vec2& uv) {
        const float handedness = normal.cross(tangent).dot(along) >= 0.0f ? 1.0f : -1.0f;
        return MeshVertex{position, normal, uv, Vec4{tangent.x, tangent.y, tangent.z, handedness}};
    }

    // A palm's leaflets' fold and droop.
    StrapFold leafletFold() {
        return StrapFold{.foldRadians = radiansFromDegrees(leafletFoldDegrees), .droop = leafletDroop};
    }

    // A tube appended to mesh reading the texture's midrib column (u 0.5) all round.
    void appendStalk(IndexedMesh& mesh, const HangingStrand& strand, int sides) {
        const std::size_t first = mesh.vertices.size();
        appendTube(mesh, strand, sides, stalkTileMetres);

        for (std::size_t vertex = first; vertex < mesh.vertices.size(); ++vertex) {
            mesh.vertices[vertex].uv.x = 0.5f;
        }
    }

    // The leafy part's spacing between pairs (metres) of a frond length long with pairs pairs.
    // Card thinning's records for the vertices from first on that never move (a thinned palm's stem and stalks): each its
    // own anchor, so no scale moves it.
    void recordStillVertices(IndexedMesh& mesh, std::size_t first) {
        mesh.thinningCards.resize(first, ThinningCard{});

        for (std::size_t vertex = first; vertex < mesh.vertices.size(); ++vertex) {
            mesh.thinningCards.push_back(ThinningCard{mesh.vertices[vertex].position, 0.0f});
        }
    }

    float pairSpacing(float frondLength, int pairs) {
        return frondLength * (1.0f - palmPetioleShare) / static_cast<float>(std::max(pairs, 1));
    }

    int keptPairs(int pairs, float share) {
        return std::max(2, static_cast<int>(std::lround(static_cast<float>(pairs) * share)));
    }

    // Smooth noise along v for one streak: random values at streaksAlong points, eased between.
    float streakNoise(std::uint32_t seed, std::uint64_t strip, float v) {
        const float place = v * streaksAlong;
        const float cell = std::floor(place);
        const float within = place - cell;
        const std::uint64_t index = strip * 64 + static_cast<std::uint64_t>(cell);
        const float from = forestUnitRandom(seed, index);
        const float to = forestUnitRandom(seed, index + 1);
        return from + (to - from) * smoothStep(0.0f, 1.0f, within);
    }
}

void visit(ForestPalmSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Spacing", settings.spacing, {.min = 1.0f, .max = 50.0f, .sliderMax = 15.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Near path", settings.nearPathMetres, {.min = 0.0f, .max = 500.0f, .sliderMax = 60.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Far path", settings.farPathMetres, {.min = 0.0f, .max = 500.0f, .sliderMax = 150.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Far share", settings.farShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Path clearance", settings.pathClearance, {.min = 0.0f, .max = 20.0f, .sliderMax = 6.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Trunk clearance", settings.trunkClearance, {.min = 0.0f, .max = 10.0f, .sliderMax = 4.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Shortest", settings.shortest, {.min = 0.5f, .max = 20.0f, .sliderMax = 8.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Tallest", settings.tallest, {.min = 0.5f, .max = 20.0f, .sliderMax = 8.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Variants", settings.variants, {.min = 1, .max = 32, .sliderMax = 16, .applies = FieldApplies::AtStartup});
    visitor.field("Least fronds", settings.leastFronds, {.min = 1, .max = 40, .sliderMax = 20, .applies = FieldApplies::AtStartup});
    visitor.field("Most fronds", settings.mostFronds, {.min = 1, .max = 40, .sliderMax = 20, .applies = FieldApplies::AtStartup});
    visitor.field("Leaflet pairs", settings.leafletPairs, {.min = 4, .max = 80, .sliderMax = 50, .applies = FieldApplies::AtStartup});
    visitor.field("Leaflet width", settings.leafletWidthShare, {.min = 0.02f, .max = 0.5f, .sliderMax = 0.3f, .applies = FieldApplies::AtStartup});
    visitor.field("Leaflet growth", settings.leafletGrowth, {.min = 0.05f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Droop", settings.droopDegrees, {.min = 0.0f, .max = 150.0f, .sliderMax = 120.0f, .unit = "deg", .applies = FieldApplies::AtStartup});
    visitor.field("Shadow cascades", settings.shadowCascades, {.min = 0, .max = 4, .applies = FieldApplies::AtStartup});
    visitor.field("Impostor error", settings.impostorErrorMetres, {.min = 0.001f, .max = 10.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Impostor frames", settings.impostorFrames, {.min = 2, .max = 32, .sliderMax = 16, .applies = FieldApplies::AtStartup});
    visitor.field("Impostor texels", settings.impostorTexels, {.min = 8, .max = 512, .sliderMax = 256, .applies = FieldApplies::AtStartup});
}

/*
* With k = palmRachisTaper and g = 1 - k s, the curvature (1 - s)^2 / g^4 is ((k - 1 + g) / k)^2 / g^4, whose integral
* over s is (1 / k^3) times that of (a + g)^2 / g^4 = a^2 g^-4 + 2 a g^-3 + g^-2 over g (a = k - 1), taken from g down
* to 1: F(g) = -a^2 / (3 g^3) - a / g^2 - 1 / g, so the turn to s is F(1) - F(1 - k s), over the same to the tip.
*/
float frondBendShare(float along) {
    constexpr float k = palmRachisTaper;
    constexpr float a = k - 1.0f;
    const auto antiderivative = [](float g) {
        return -a * a / (3.0f * g * g * g) - a / (g * g) - 1.0f / g;
    };
    const float s = std::clamp(along, 0.0f, 1.0f);
    const float whole = antiderivative(1.0f) - antiderivative(1.0f - k);
    return (antiderivative(1.0f) - antiderivative(1.0f - k * s)) / whole;
}

float palmLeafletLength(float along) {
    return 1.0f / 3.0f + 2.0f / 3.0f * std::sin(std::clamp(along, 0.0f, 1.0f) * 0.5f * fullTurnRadians);
}

// sin(pi (0.08 + 0.92 t^0.7)): a quarter of the widest at the base, widest where t^0.7 is 0.4565 (t about 0.33), 0 at
// the tip.
float palmLeafletWidth(float along) {
    const float t = std::clamp(along, 0.0f, 1.0f);
    return std::max(0.0f, std::sin(0.5f * fullTurnRadians * (0.08f + 0.92f * std::pow(t, 0.7f))));
}

float palmVariantHeight(const ForestPalmSettings& settings, int variant) {
    const float share = (static_cast<float>(variant) + 0.5f) / static_cast<float>(std::max(settings.variants, 1));
    return settings.shortest + (settings.tallest - settings.shortest) * share;
}

PalmShape palmVariantShape(const ForestPalmSettings& settings, int variant, std::uint32_t seed) {
    const float share = (static_cast<float>(variant) + 0.5f) / static_cast<float>(std::max(settings.variants, 1));
    const float height = palmVariantHeight(settings, variant);
    const float frondLength = palmFrondLengthShare * height;
    const int fronds = settings.leastFronds + static_cast<int>(std::lround(static_cast<float>(settings.mostFronds - settings.leastFronds) * share));
    return PalmShape{
        .height = height,
        .stemHeight = std::max(0.0f, height - crownBelowTop * frondLength),
        .stemRadius = std::max(leastStemRadius, stemRadiusShare * height),
        .fronds = std::clamp(fronds, std::min(settings.leastFronds, settings.mostFronds), std::max(settings.leastFronds, settings.mostFronds)),
        .frondLength = frondLength,
        .droopRadians = radiansFromDegrees(settings.droopDegrees),
        .leafletPairs = settings.leafletPairs,
        .leafletWidthShare = settings.leafletWidthShare,
        .leafletGrowth = settings.leafletGrowth,
        .seed = forestShapeSeed(seed, static_cast<std::uint32_t>(variant) + 1)
    };
}

void appendStrapLeaf(IndexedMesh& mesh, const Vec3& base, const Vec3& direction, const Vec3& lift, float length, float width, int segments,
                     const StrapFold& fold) {
    const float foldAcross = std::cos(fold.foldRadians);
    const float foldDown = std::sin(fold.foldRadians);
    const std::uint32_t first = static_cast<std::uint32_t>(mesh.vertices.size());

    for (int segment = 0; segment <= segments; ++segment) {
        const float t = static_cast<float>(segment) / static_cast<float>(segments);
        const Vec3 midrib = base + direction * (t * length) - up * (fold.droop * length * t * t);
        const Vec3 along = (direction - up * (2.0f * fold.droop * t)).normalized();
        Vec3 normal = lift - along * lift.dot(along);
        normal = normal.lengthSquared() > 1e-10f ? normal.normalized() : squareTo(along);
        const Vec3 across = along.cross(normal);
        const float halfWidth = 0.5f * width * palmLeafletWidth(t);

        const Vec3 leftTangent = (across * foldAcross + normal * foldDown).normalized();
        const Vec3 rightTangent = (across * foldAcross - normal * foldDown).normalized();
        const Vec3 leftNormal = (normal * foldAcross - across * foldDown).normalized();
        const Vec3 rightNormal = (normal * foldAcross + across * foldDown).normalized();
        mesh.vertices.push_back(leafletVertex(midrib - leftTangent * halfWidth, leftNormal, leftTangent, along, Vec2{0.0f, t}));
        mesh.vertices.push_back(leafletVertex(midrib, normal, across, along, Vec2{0.5f, t}));
        mesh.vertices.push_back(leafletVertex(midrib + rightTangent * halfWidth, rightNormal, rightTangent, along, Vec2{1.0f, t}));
    }

    for (int segment = 0; segment < segments; ++segment) {
        const std::uint32_t row = first + 3 * static_cast<std::uint32_t>(segment);
        const std::uint32_t next = row + 3;
        const bool pointed = segment + 1 == segments;

        // Each half a quad, the last segment a triangle to the tip, where the width is gone.
        appendFacingTriangle(mesh, row, row + 1, next + 1);
        appendFacingTriangle(mesh, row + 1, row + 2, next + 1);

        if (!pointed) {
            appendFacingTriangle(mesh, row, next + 1, next);
            appendFacingTriangle(mesh, row + 2, next + 2, next + 1);
        }
    }
}

std::vector<Vec3> frondSpine(const Vec3& base, float azimuth, float elevation, float length, float droop, int count) {
    const Vec3 level{std::cos(azimuth), 0.0f, -std::sin(azimuth)};
    const float step = length / static_cast<float>(count);
    std::vector<Vec3> spine{base};

    for (int point = 0; point < count; ++point) {
        const float middle = (static_cast<float>(point) + 0.5f) / static_cast<float>(count);
        const float angle = elevation - droop * frondBendShare(middle);
        spine.push_back(spine.back() + (level * std::cos(angle) + up * std::sin(angle)) * step);
    }

    return spine;
}

IndexedMesh growPalm(const PalmShape& shape, const PalmDetail& detail) {
    IndexedMesh mesh;

    // The stem, from just under the ground to the crown, a ring every stemStepMetres.
    HangingStrand stem;
    const float stemLength = shape.stemHeight + stemSinkMetres;
    const int stemRings = std::max(1, static_cast<int>(std::ceil(stemLength / stemStepMetres)));

    for (int ring = 0; ring <= stemRings; ++ring) {
        stem.points.push_back(Vec3{0.0f, -stemSinkMetres + stemLength * static_cast<float>(ring) / static_cast<float>(stemRings), 0.0f});
        stem.radii.push_back(shape.stemRadius);
    }

    appendStalk(mesh, stem, detail.stemSides);

    // Thinned: the stem still (its vertices their own anchors), the leaflets the thinning leaves out gathered apart.
    IndexedMesh removed;
    PinnateThinning thinning{.removed = &removed, .keepEvery = palmKeepEvery, .leastShare = 1.0f / static_cast<float>(palmKeepEvery),
                             .rankSeed = forestShapeSeed(shape.seed, 7)};

    if (detail.thinned) {
        recordStillVertices(mesh, 0);
    }

    const int pairs = keptPairs(shape.leafletPairs, detail.pairShare);
    const float widthShare = std::min(widestLeafletShare, shape.leafletWidthShare * static_cast<float>(shape.leafletPairs) / static_cast<float>(pairs));
    const float phase = fullTurnRadians * forestUnitRandom(shape.seed, 0);
    std::uint64_t draw = 1;
    const auto random = [&] {
        return forestUnitRandom(shape.seed, draw++);
    };
    const auto spread = [&](float range) {
        return range * (2.0f * random() - 1.0f);
    };

    for (int frond = 0; frond < shape.fronds; ++frond) {
        const float age = shape.fronds > 1 ? static_cast<float>(frond) / static_cast<float>(shape.fronds - 1) : 0.0f;
        const float azimuth = phase + goldenAngle * static_cast<float>(frond);
        const float elevation = radiansFromDegrees(youngestElevationDegrees + (oldestElevationDegrees - youngestElevationDegrees) * age +
                                                   spread(frondElevationJitterDegrees));
        const float droop = shape.droopRadians * (youngestDroopShare + (oldestDroopShare - youngestDroopShare) * age) * (1.0f + spread(frondDroopJitter));
        const float length = shape.frondLength * (1.0f + spread(frondLengthJitter));
        const Vec3 level{std::cos(azimuth), 0.0f, -std::sin(azimuth)};
        const Vec3 base = Vec3{0.0f, shape.stemHeight - oldestFrondDrop * length * age, 0.0f} + level * (0.5f * shape.stemRadius);
        appendPinnateFrond(mesh,
                           PinnateFrond{.base = base, .azimuth = azimuth, .elevation = elevation, .length = length, .droop = droop, .pairs = pairs,
                                        .leafletWidthShare = widthShare, .rachisSides = detail.rachisSides, .leafletSegments = detail.leafletSegments},
                           random, detail.thinned ? &thinning : nullptr);
    }

    // The leaflets that go, after everything kept.
    if (detail.thinned) {
        const std::uint32_t offset = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), removed.vertices.begin(), removed.vertices.end());
        mesh.thinningCards.insert(mesh.thinningCards.end(), removed.thinningCards.begin(), removed.thinningCards.end());

        for (const std::uint32_t index : removed.indices) {
            mesh.indices.push_back(index + offset);
        }
    }

    return mesh;
}

void appendPinnateFrond(IndexedMesh& mesh, const PinnateFrond& frond, const std::function<float()>& random, PinnateThinning* thinning) {
    const auto spread = [&](float range) {
        return range * (2.0f * random() - 1.0f);
    };
    const Vec3 level{std::cos(frond.azimuth), 0.0f, -std::sin(frond.azimuth)};
    HangingStrand rachis;
    rachis.points = frondSpine(frond.base, frond.azimuth, frond.elevation, frond.length, frond.droop, frondPoints);

    for (int point = 0; point <= frondPoints; ++point) {
        const float along = static_cast<float>(point) / static_cast<float>(frondPoints);
        rachis.radii.push_back(rachisRadiusShare * frond.length * (1.0f - palmRachisTaper * along));
    }

    const std::size_t stalkStart = mesh.vertices.size();
    appendStalk(mesh, rachis, frond.rachisSides);

    if (thinning != nullptr) {
        recordStillVertices(mesh, stalkStart);
    }

    // The leaflets in pairs along the leafy part, each side's leaning forward and lifted out of the frond's plane.
    const float longest = palmLeafletShare * frond.length;

    for (int pair = 0; pair < frond.pairs; ++pair) {
        const float leafy = (static_cast<float>(pair) + 0.5f) / static_cast<float>(frond.pairs);
        const SpinePoint at = spineAt(rachis.points, palmPetioleShare + (1.0f - palmPetioleShare) * leafy);
        Vec3 side = at.direction.cross(up);
        side = side.lengthSquared() > 1e-8f ? side.normalized() : level.cross(up).normalized();
        const Vec3 frondUp = side.cross(at.direction).normalized();

        for (const float hand : {-1.0f, 1.0f}) {
            const float angle = radiansFromDegrees(leafletAngleDegrees + spread(leafletAngleJitterDegrees));
            const float lift = radiansFromDegrees(leafletLiftDegrees);
            const Vec3 out = side * (hand * std::cos(lift)) + frondUp * std::sin(lift);
            const Vec3 direction = (at.direction * std::cos(angle) + out * std::sin(angle)).normalized();
            const float leafletLength = longest * palmLeafletLength(leafy) * (1.0f + spread(leafletLengthJitter));
            // Thinned, a pair kept goes to the mesh, any other to the removed ones, each vertex its leaflet's base and rank.
            const bool kept = thinning == nullptr || pair % std::max(thinning->keepEvery, 1) == 0;
            IndexedMesh& into = kept ? mesh : *thinning->removed;
            const std::size_t first = into.vertices.size();
            appendStrapLeaf(into, at.point, direction, frondUp, leafletLength, frond.leafletWidthShare * leafletLength, frond.leafletSegments,
                            leafletFold());

            if (thinning != nullptr) {
                const float rank = kept ? 0.0f : removedCardRank(forestUnitRandom(thinning->rankSeed, thinning->leaflets), thinning->leastShare);
                into.thinningCards.resize(first, ThinningCard{});
                into.thinningCards.resize(into.vertices.size(), ThinningCard{at.point, rank});
                ++thinning->leaflets;
            }
        }
    }
}

PalmMeshes growPalmMeshes(const PalmShape& shape) {
    PalmMeshes meshes{growPalm(shape, PalmDetail{.thinned = true}), IndexedMesh{}, growPalm(shape, shadowDetail)};

    // Fitted to its height: its top, wherever its fronds rose to, at the shape's height (the leaflets' bases too).
    const float top = indexedMeshBounds(meshes.palm).maximum.y;
    const float fit = top > 0.0f ? shape.height / top : 1.0f;

    for (IndexedMesh* mesh : {&meshes.palm, &meshes.shadow}) {
        for (MeshVertex& vertex : mesh->vertices) {
            vertex.position = vertex.position * fit;
        }
    }

    for (ThinningCard& card : meshes.palm.thinningCards) {
        card.anchor = card.anchor * fit;
    }

    /*
    * Thinned leaflet by leaflet into every palmKeepEvery-th pair (scene/card_thinning.h), which is its coarser level (its
    * first vertices): that level strays about as far as the gap its dropped pairs leave, and the fade spends its error
    * on the way there, the kept leaflets growing by share^-leafletGrowth.
    */
    const float spacing = pairSpacing(shape.frondLength, shape.leafletPairs);
    const float leastShare = 1.0f / static_cast<float>(palmKeepEvery);
    const float gap = fit * spacing * static_cast<float>(palmKeepEvery - 1);
    const float growth = std::max(shape.leafletGrowth, 0.01f);
    meshes.palm.thinning = CardThinning{.cardSize = gap / (std::pow(leastShare, -growth) - 1.0f), .growth = growth, .leastShare = leastShare};
    meshes.level = thinnedLevel(meshes.palm);
    meshes.levelError = thinningError(meshes.palm.thinning, leastShare);
    meshes.shadowError = fit * (pairSpacing(shape.frondLength, keptPairs(shape.leafletPairs, shadowDetail.pairShare)) - spacing);
    return meshes;
}

std::vector<PalmPlacement> placePalms(const ForestPalmSettings& settings, const std::vector<StandingCrown>& trunks,
                                      const std::vector<SightLine>& sightLines) {
    const PlantingGrid grid{settings.spacing, settings.nearPathMetres, settings.farPathMetres, settings.farShare, settings.pathClearance,
                            settings.trunkClearance};
    const int variants = std::max(settings.variants, 1);
    std::vector<PalmPlacement> palms;

    for (const PlantedCell& cell : plantedCells(grid, trunks, palmSeed, palmDrawsPerCell)) {
        const auto draw = [&](PalmDraw which) {
            return forestUnitRandom(palmSeed, cell.firstDraw + which);
        };

        const PalmPlacement palm{
            .foot = Vec3{cell.place.x, forestHeight(cell.place.x, cell.place.y) - understoreySinkMetres, cell.place.y},
            .scale = 1.0f + palmScaleSpread * (2.0f * draw(Scale) - 1.0f),
            .turnRadians = fullTurnRadians * draw(Turn),
            .variant = std::min(static_cast<int>(draw(Variant) * static_cast<float>(variants)), variants - 1)
        };

        if (!blocksSight(palmCentre(settings, palm), palmRadius(settings, palm), sightLines)) {
            palms.push_back(palm);
        }
    }

    return palms;
}

float palmRadius(const ForestPalmSettings& settings, const PalmPlacement& palm) {
    return 0.5f * palmVariantHeight(settings, palm.variant) * palm.scale;
}

Vec3 palmCentre(const ForestPalmSettings& settings, const PalmPlacement& palm) {
    return palm.foot + up * palmRadius(settings, palm);
}

PalmLeafletImages palmLeafletImages(int texels, Pixel colour, std::uint32_t seed) {
    const std::uint32_t size = static_cast<std::uint32_t>(std::max(texels, 1));
    PalmLeafletImages images;
    images.colour.width = size;
    images.colour.height = size;
    images.colour.encoding = TextureEncoding::Srgb;
    images.colour.pixels.reserve(static_cast<std::size_t>(size) * size * 4);
    images.height.width = size;
    images.height.height = size;
    images.height.pixels.reserve(static_cast<std::size_t>(size) * size * 4);

    for (std::uint32_t y = 0; y < size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        const float dry = smoothStep(dryTipStart, 1.0f, v);

        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);

            // Out from the midrib, 0 on it and 1 at either edge; the veins at whole numbers of veinsPerHalf.
            const float out = std::fabs(u - 0.5f) * 2.0f;
            const float veinPlace = out * veinsPerHalf;
            const float fromVein = std::fabs(veinPlace - std::round(veinPlace));
            const float vein = std::max(0.0f, 1.0f - fromVein / veinHalfWidth);
            const float midrib = std::max(0.0f, 1.0f - out / midribHalfWidth);

            // Each strip between veins its own streak, either side of the midrib apart.
            const std::uint64_t strip = static_cast<std::uint64_t>(veinPlace) * 2 + (u < 0.5f ? 0 : 1);
            const float shade = (1.0f + streakStrength * (2.0f * streakNoise(seed, strip, v) - 1.0f)) * (1.0f + 0.08f * vein) *
                                (1.0f - edgeDarkening * out * out * out * out);

            // The midrib paler and yellower, the tip drying browner.
            const float red = static_cast<float>(colour.r) * shade * (1.0f + 0.6f * midrib) * (1.0f + 0.25f * dry);
            const float green = static_cast<float>(colour.g) * shade * (1.0f + 0.45f * midrib) * (1.0f - 0.05f * dry);
            const float blue = static_cast<float>(colour.b) * shade * (1.0f + 0.15f * midrib) * (1.0f - 0.4f * dry);
            images.colour.pixels.insert(images.colour.pixels.end(), {channelByte(red), channelByte(green), channelByte(blue), 255});

            const std::uint8_t height = channelByte(255.0f * (0.4f + 0.15f * vein + 0.4f * midrib));
            images.height.pixels.insert(images.height.pixels.end(), {height, height, height, 255});
        }
    }

    return images;
}
