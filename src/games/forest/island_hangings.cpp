#include "games/forest/island_hangings.h"

#include "games/forest/forest_terrain.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>

namespace {
    // A strand turns down over this many metres (the share pointing down 1 - exp(-s / bend)).
    constexpr float hangingBendMetres = 4.0f;

    // How far it wanders sideways, as a share of its step, and over how many metres the wander changes.
    constexpr float hangingWander = 0.3f;
    constexpr float hangingWanderMetres = 7.0f;

    // Roots: their radius where they leave the rock grows with their length, up to the thickest; all taper to the tip.
    constexpr float thinnestRootStart = 0.15f;
    constexpr float thickestRootExtra = 0.6f;
    constexpr float rootTipRadius = 0.04f;
    constexpr int rootSides = 8;

    constexpr float vineRadius = 0.06f;
    constexpr float vineTipRadius = 0.03f;
    constexpr int vineSides = 4;

    // Each root and vine is this share of the longest at least.
    constexpr float shortestShare = 0.2f;

    // The bark's texture repeats every this many metres along a tube (once round it).
    constexpr float barkTileMetres = 2.0f;

    // Anchors lie this far below the rim at least (a share of the radius) and are sunk this far into the rock.
    constexpr float anchorBelowRim = 0.02f;
    constexpr float anchorSink = 0.2f;

    // The ivy atlas: two leaves across, three up.
    constexpr int leafColumns = 2;
    constexpr int leafRows = 3;
    constexpr float leafWidthShare = 1.5f;

    // How much a leaf droops from pointing straight out.
    constexpr float leafDroop = 0.6f;

    const Vec3 down{0.0f, -1.0f, 0.0f};

    // A buttress's angle may stray this share of the spacing either way from even, and its height is 60-100% of the
    // amplitude.
    constexpr float lobeAngleJitter = 0.15f;
    constexpr float lowestLobeShare = 0.6f;

    // The strand's direction at point i: across its neighbours (one-sided at the ends).
    Vec3 strandDirection(const HangingStrand& strand, std::size_t i) {
        const std::size_t last = strand.points.size() - 1;
        const Vec3 across = strand.points[std::min(i + 1, last)] - strand.points[i == 0 ? 0 : i - 1];
        return across.lengthSquared() > 1e-12f ? across.normalized() : down;
    }
}

Vec3 squareTo(const Vec3& direction) {
    const Vec3 side = std::fabs(direction.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    return (side - direction * side.dot(direction)).normalized();
}

Vec3 levelOutward(const Vec3& direction) {
    const Vec3 level{direction.x, 0.0f, direction.z};
    return level.lengthSquared() > 1e-12f ? level.normalized() : Vec3{1.0f, 0.0f, 0.0f};
}

std::vector<std::size_t> islandUnderside(const SurfaceGrid& surface, float radius) {
    std::vector<std::size_t> underside;

    for (std::size_t point = 0; point < surface.points.size(); ++point) {
        if (surface.points[point].y < -anchorBelowRim * radius) {
            underside.push_back(point);
        }
    }

    return underside;
}

std::size_t towardsTheRim(const std::vector<std::size_t>& underside, float random) {
    const float place = std::sqrt(random);
    return underside[std::min(static_cast<std::size_t>(place * static_cast<float>(underside.size())), underside.size() - 1)];
}

HangingStrand hangingStrand(const Vec3& anchor, const Vec3& outward, float length, float startRadius, float tipRadius, std::uint32_t seed) {
    const int steps = std::max(1, static_cast<int>(std::ceil(length / hangingStepMetres)));
    const float segment = length / static_cast<float>(steps);
    const Vec3 out = levelOutward(outward);
    const std::uint32_t acrossSeed = forestShapeSeed(seed, 1);
    const std::uint32_t alongSeed = forestShapeSeed(seed, 2);

    HangingStrand strand;
    strand.points.reserve(static_cast<std::size_t>(steps) + 1);
    strand.radii.reserve(static_cast<std::size_t>(steps) + 1);
    strand.points.push_back(anchor);
    strand.radii.push_back(startRadius);

    for (int step = 1; step <= steps; ++step) {
        const float along = (static_cast<float>(step) - 0.5f) * segment;
        const float droop = 1.0f - std::exp(-along / hangingBendMetres);
        const float wanderAt = along / hangingWanderMetres;
        const Vec3 wander{2.0f * forestValueNoise(wanderAt, 0.5f, acrossSeed) - 1.0f, 0.0f, 2.0f * forestValueNoise(wanderAt, 0.5f, alongSeed) - 1.0f};
        // Level parts only, so the strand never climbs: its height falls by the droop's share of each step.
        const Vec3 direction = (out * (1.0f - droop) + down * droop + wander * (hangingWander * droop)).normalized();
        strand.points.push_back(strand.points.back() + direction * segment);

        const float toTip = 1.0f - static_cast<float>(step) / static_cast<float>(steps);
        strand.radii.push_back(tipRadius + (startRadius - tipRadius) * std::pow(toTip, 1.5f));
    }

    return strand;
}

float lobedRadius(float radius, float angle, float height, const TubeLobes& lobes) {
    if (lobes.count <= 0 || lobes.amplitude == 0.0f) {
        return radius;
    }

    const float fade = std::exp(-std::max(height - lobes.groundHeight, 0.0f) / lobes.heightMetres);
    const float spacing = fullTurnRadians / static_cast<float>(lobes.count);
    float standing = 0.0f;

    for (int lobe = 0; lobe < lobes.count; ++lobe) {
        const std::uint64_t index = 2 * static_cast<std::uint64_t>(lobe);
        const float lobeAngle = (static_cast<float>(lobe) + 2.0f * lobeAngleJitter * (forestUnitRandom(lobes.seed, index) - 0.5f)) * spacing;
        const float share = lowestLobeShare + (1.0f - lowestLobeShare) * forestUnitRandom(lobes.seed, index + 1);
        standing += share * std::pow(std::max(std::cos(angle - lobeAngle), 0.0f), lobes.sharpness);
    }

    return radius * (1.0f + lobes.amplitude * fade * standing);
}

void appendTube(IndexedMesh& mesh, const HangingStrand& strand, int sides, float tileMetres, const TubeOptions& options) {
    if (strand.points.size() < 2 || sides < 3) {
        return;
    }

    const std::uint32_t first = static_cast<std::uint32_t>(mesh.vertices.size());
    const std::uint32_t ringSize = static_cast<std::uint32_t>(sides) + 1;
    const bool lobed = options.lobes.count > 0 && options.lobes.amplitude != 0.0f;
    const float tilesRound = options.wholeTilesRound ? std::max(1.0f, std::round(fullTurnRadians * strand.radii.front() / tileMetres)) : 1.0f;
    Vec3 across = squareTo(strandDirection(strand, 0));
    float travelled = 0.0f;

    for (std::size_t i = 0; i < strand.points.size(); ++i) {
        const Vec3 along = strandDirection(strand, i);

        if (i > 0) {
            travelled += (strand.points[i] - strand.points[i - 1]).length();
        }

        // The last ring's frame made square to the strand here, so the tube does not twist.
        const Vec3 square = across - along * across.dot(along);
        across = square.lengthSquared() > 1e-12f ? square.normalized() : squareTo(along);
        const Vec3 second = along.cross(across);

        for (int side = 0; side <= sides; ++side) {
            const float angle = fullTurnRadians * static_cast<float>(side) / static_cast<float>(sides);
            const Vec3 normal = across * std::cos(angle) + second * std::sin(angle);
            const Vec3 round = second * std::cos(angle) - across * std::sin(angle);
            const float radius = lobed ? lobedRadius(strand.radii[i], angle, strand.points[i].y, options.lobes) : strand.radii[i];
            mesh.vertices.push_back(MeshVertex{strand.points[i] + normal * radius, normal,
                                               Vec2{tilesRound * static_cast<float>(side) / static_cast<float>(sides), travelled / tileMetres},
                                               Vec4{round.x, round.y, round.z, 1.0f}});
        }
    }

    // A lobed surface is no longer round: its normals and tangents come from its own differences round and along it,
    // round x along facing out as before.
    if (lobed) {
        const std::size_t rings = strand.points.size();
        const auto at = [&](std::size_t ring, int side) -> const Vec3& {
            return mesh.vertices[first + ring * ringSize + static_cast<std::size_t>(side)].position;
        };

        for (std::size_t ring = 0; ring < rings; ++ring) {
            for (int side = 0; side <= sides; ++side) {
                // The seam's two copies share a place, so the neighbours round it wrap past it.
                const int before = side == 0 ? sides - 1 : side - 1;
                const int after = side == sides ? 1 : side + 1;
                const Vec3 round = at(ring, after) - at(ring, before);
                const Vec3 along = at(std::min(ring + 1, rings - 1), side) - at(ring == 0 ? 0 : ring - 1, side);
                const Vec3 normal = round.cross(along);
                MeshVertex& vertex = mesh.vertices[first + ring * ringSize + static_cast<std::size_t>(side)];

                if (normal.lengthSquared() > 1e-20f) {
                    vertex.normal = normal.normalized();
                    const Vec3 tangent = round - vertex.normal * round.dot(vertex.normal);

                    if (tangent.lengthSquared() > 1e-20f) {
                        const Vec3 unit = tangent.normalized();
                        vertex.tangent = Vec4{unit.x, unit.y, unit.z, 1.0f};
                    }
                }
            }
        }
    }

    // Round then along makes round x along = out, so (a, b, c) faces out.
    for (std::uint32_t ring = 0; ring + 1 < strand.points.size(); ++ring) {
        for (std::uint32_t side = 0; side < static_cast<std::uint32_t>(sides); ++side) {
            const std::uint32_t a = first + ring * ringSize + side;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + ringSize;
            const std::uint32_t d = c + 1;
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
    }
}

AtlasCell atlasCell(int cell, int columns, int rows) {
    const float u0 = static_cast<float>(cell % columns) / static_cast<float>(columns);
    const float v0 = static_cast<float>(cell / columns) / static_cast<float>(rows);
    return AtlasCell{Vec2{u0, v0}, Vec2{u0 + 1.0f / static_cast<float>(columns), v0 + 1.0f / static_cast<float>(rows)}};
}

// side = towardsTip x facing, so the corners (the stem's left, its right, the tip's right, its left) turn
// counterclockwise seen from the front: side x towardsTip = facing.
void appendLeafCard(IndexedMesh& mesh, const Vec3& stem, const Vec3& towardsTip, const Vec3& facing, const Vec3& normal, float length,
                    float width, const Vec2& uvMin, const Vec2& uvMax) {
    const Vec3 side = towardsTip.cross(facing);
    const Vec3 square = side - normal * side.dot(normal);
    const Vec3 across = square.lengthSquared() > 1e-12f ? square.normalized() : side;
    const Vec3 halfSide = side * (0.5f * width);
    const Vec3 tip = stem + towardsTip * length;
    const Vec4 tangent{across.x, across.y, across.z, 1.0f};
    const std::uint32_t corner = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(MeshVertex{stem - halfSide, normal, Vec2{uvMin.x, uvMin.y}, tangent});
    mesh.vertices.push_back(MeshVertex{stem + halfSide, normal, Vec2{uvMax.x, uvMin.y}, tangent});
    mesh.vertices.push_back(MeshVertex{tip + halfSide, normal, Vec2{uvMax.x, uvMax.y}, tangent});
    mesh.vertices.push_back(MeshVertex{tip - halfSide, normal, Vec2{uvMin.x, uvMax.y}, tangent});
    mesh.indices.insert(mesh.indices.end(), {corner, corner + 1, corner + 2, corner, corner + 2, corner + 3});
}

void appendLeafCards(IndexedMesh& mesh, const HangingStrand& vine, float spacing, float size, std::uint32_t seed) {
    if (vine.points.size() < 2 || spacing <= 0.0f || size <= 0.0f) {
        return;
    }

    float length = 0.0f;
    std::vector<float> travelled{0.0f};

    for (std::size_t i = 1; i < vine.points.size(); ++i) {
        length += (vine.points[i] - vine.points[i - 1]).length();
        travelled.push_back(length);
    }

    const float width = leafWidthShare * size;

    for (std::uint64_t card = 0;; ++card) {
        const float at = (static_cast<float>(card) + 0.5f + 0.6f * (forestUnitRandom(seed, 4 * card) - 0.5f)) * spacing;

        if (at >= length) {
            break;
        }

        // The point and direction of the vine there.
        const std::size_t segment = static_cast<std::size_t>(std::upper_bound(travelled.begin(), travelled.end(), at) - travelled.begin()) - 1;
        const std::size_t next = std::min(segment + 1, vine.points.size() - 1);
        const float span = travelled[next] - travelled[segment];
        const float share = span > 0.0f ? (at - travelled[segment]) / span : 0.0f;
        const Vec3 point = vine.points[segment] + (vine.points[next] - vine.points[segment]) * share;
        const float radius = vine.radii[segment] + (vine.radii[next] - vine.radii[segment]) * share;
        const Vec3 along = strandDirection(vine, segment);

        // Out from the vine at a random turn, drooping.
        const Vec3 across = squareTo(along);
        const Vec3 second = along.cross(across);
        const float turn = fullTurnRadians * forestUnitRandom(seed, 4 * card + 1);
        const Vec3 radial = across * std::cos(turn) + second * std::sin(turn);
        const Vec3 out = (radial + down * leafDroop).normalized();
        Vec3 side = along.cross(out);
        side = side.lengthSquared() > 1e-12f ? side.normalized() : squareTo(out);
        const Vec3 normal = side.cross(out);

        const int cell = std::min(static_cast<int>(forestUnitRandom(seed, 4 * card + 2) * static_cast<float>(leafColumns * leafRows)), leafColumns * leafRows - 1);
        const AtlasCell picture = atlasCell(cell, leafColumns, leafRows);
        appendLeafCard(mesh, point + radial * radius, out, normal, normal, size, width, picture.uvMin, picture.uvMax);
    }
}

void visit(ForestHangingSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Roots per 10 m", settings.rootsPerTenMetres, {.min = 0.0f, .max = 50.0f, .sliderMax = 10.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Vines per 10 m", settings.vinesPerTenMetres, {.min = 0.0f, .max = 50.0f, .sliderMax = 10.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Longest root", settings.longestRoot, {.min = 1.0f, .max = 500.0f, .sliderMax = 150.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Longest vine", settings.longestVine, {.min = 1.0f, .max = 500.0f, .sliderMax = 200.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Leaf spacing", settings.leafSpacing, {.min = 0.05f, .max = 10.0f, .sliderMax = 2.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Leaf size", settings.leafSize, {.min = 0.05f, .max = 5.0f, .sliderMax = 1.5f, .unit = "m", .applies = FieldApplies::AtStartup});
}

IslandHangings islandHangings(const IslandPlacement& island, const SurfaceGrid& surface, const ForestHangingSettings& settings) {
    IslandHangings hangings;
    const float tens = island.radius / 10.0f;
    hangings.rootCount = std::max(0, static_cast<int>(std::lround(settings.rootsPerTenMetres * tens)));
    hangings.vineCount = std::max(0, static_cast<int>(std::lround(settings.vinesPerTenMetres * tens)));

    const std::vector<std::size_t> underside = islandUnderside(surface, island.radius);

    if (underside.empty()) {
        hangings.rootCount = 0;
        hangings.vineCount = 0;
        return hangings;
    }

    const std::uint32_t seed = forestShapeSeed(island.seed, 10);
    const int count = hangings.rootCount + hangings.vineCount;

    for (int strand = 0; strand < count; ++strand) {
        const std::uint64_t base = static_cast<std::uint64_t>(strand) * 4;
        const Vec3 point = surface.points[towardsTheRim(underside, forestUnitRandom(seed, base))];
        const Vec3 outward = levelOutward(point);
        const Vec3 anchor = point - outward * anchorSink;
        hangings.anchors.push_back(anchor);

        const bool root = strand < hangings.rootCount;
        const float share = shortestShare + (1.0f - shortestShare) * forestUnitRandom(seed, base + 1);
        const std::uint32_t strandSeed = forestShapeSeed(seed, static_cast<std::uint32_t>(strand) + 1);

        if (root) {
            const float length = share * settings.longestRoot;
            const HangingStrand hanging = hangingStrand(anchor, outward, length, thinnestRootStart + thickestRootExtra * share, rootTipRadius, strandSeed);
            appendTube(hangings.bark, hanging, rootSides, barkTileMetres);
            hangings.tips.push_back(hanging.points.back());
        } else {
            const float length = share * settings.longestVine;
            const HangingStrand hanging = hangingStrand(anchor, outward, length, vineRadius, vineTipRadius, strandSeed);
            appendTube(hangings.bark, hanging, vineSides, barkTileMetres);
            appendLeafCards(hangings.leaves, hanging, settings.leafSpacing, settings.leafSize, forestShapeSeed(strandSeed, 3));
            hangings.tips.push_back(hanging.points.back());
        }
    }

    return hangings;
}
