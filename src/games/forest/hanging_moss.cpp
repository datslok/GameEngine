#include "games/forest/hanging_moss.h"

#include "games/forest/forest_terrain.h"
#include "games/forest/island_hangings.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>

namespace {
    // Strands a texel across the texture: about 1.2 covers the top edge 70% (1 - exp(-1.2)), leaving gaps.
    constexpr float strandsPerTexel = 0.6f;

    // A strand's length as a share of the height: the shortest, and how the rest spread (a random share squared, so
    // most are short and few reach the bottom).
    constexpr float shortestStrand = 0.15f;

    // A strand's width at the top (texels), thinning to 40% of it at the tip; how far it waves and drifts sideways.
    constexpr float thinnestStrand = 1.0f;
    constexpr float thickestStrandExtra = 2.0f;
    constexpr float strandWave = 4.0f;
    constexpr float strandDrift = 0.08f;

    // Each strand is its colour times 0.65-1.15, darker at the top (75%) than at the tip.
    constexpr float darkestStrand = 0.65f;
    constexpr float strandShadeRange = 0.5f;
    constexpr float topShade = 0.75f;

    // Clumps on roots' tips are this share of the longest; ledge and underside clumps hang their tops this far into
    // the rock, so no gap shows above them.
    constexpr float tipMossShare = 0.5f;
    constexpr float mossTuck = 0.1f;

    const Vec3 up{0.0f, 1.0f, 0.0f};

    std::uint8_t shaded(std::uint8_t channel, float shade) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<float>(channel) * shade + 0.5f, 0.0f, 255.0f));
    }

    // A clump of 30-100% of longest under top, turned at random.
    void addClump(IndexedMesh& mesh, const Vec3& top, float longest, std::uint32_t seed, std::uint64_t index) {
        const float length = longest * (0.3f + 0.7f * forestUnitRandom(seed, 3 * index + 1));
        appendMossClump(mesh, top, length, fullTurnRadians * forestUnitRandom(seed, 3 * index + 2));
    }
}

ImageData mossStrandImage(int width, int height, Pixel colour, std::uint32_t seed) {
    ImageData image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.encoding = TextureEncoding::Srgb;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);

    for (std::size_t texel = 0; texel < image.pixels.size() / 4; ++texel) {
        image.pixels[texel * 4] = colour.r;
        image.pixels[texel * 4 + 1] = colour.g;
        image.pixels[texel * 4 + 2] = colour.b;
        image.pixels[texel * 4 + 3] = 0;
    }

    const int strands = std::max(1, static_cast<int>(strandsPerTexel * static_cast<float>(width)));

    for (int strand = 0; strand < strands; ++strand) {
        const std::uint64_t base = static_cast<std::uint64_t>(strand) * 8;
        const float start = forestUnitRandom(seed, base) * static_cast<float>(width);
        const float reach = forestUnitRandom(seed, base + 1);
        const float length = shortestStrand + (1.0f - shortestStrand) * reach * reach;
        const float thickness = thinnestStrand + thickestStrandExtra * forestUnitRandom(seed, base + 2);
        const float wave = strandWave * forestUnitRandom(seed, base + 3);
        const float waves = 2.0f + 3.0f * forestUnitRandom(seed, base + 4);
        const float phase = fullTurnRadians * forestUnitRandom(seed, base + 5);
        const float drift = strandDrift * static_cast<float>(width) * (2.0f * forestUnitRandom(seed, base + 6) - 1.0f);
        const float shade = darkestStrand + strandShadeRange * forestUnitRandom(seed, base + 7);
        const int lowestRow = std::max(0, static_cast<int>(std::floor((1.0f - length) * static_cast<float>(height))));

        for (int row = height - 1; row >= lowestRow; --row) {
            // How far down the strand this row is, 0 at the top edge and 1 at its tip.
            const float down = (1.0f - (static_cast<float>(row) + 0.5f) / static_cast<float>(height)) / length;

            if (down > 1.0f) {
                break;
            }

            const float centre = start + drift * down + wave * std::sin(fullTurnRadians * waves * down * length + phase);
            const float halfWidth = 0.5f * thickness * (1.0f - 0.6f * down);
            const float rowShade = shade * (topShade + (1.0f - topShade) * down);
            const int first = std::max(0, static_cast<int>(std::floor(centre - halfWidth - 1.0f)));
            const int last = std::min(width - 1, static_cast<int>(std::ceil(centre + halfWidth + 1.0f)));

            for (int column = first; column <= last; ++column) {
                // The share of the texel the strand covers, its edges anti-aliased.
                const float cover = std::clamp(halfWidth + 0.5f - std::abs(static_cast<float>(column) + 0.5f - centre), 0.0f, 1.0f);
                const std::uint8_t alpha = static_cast<std::uint8_t>(cover * 255.0f + 0.5f);
                std::uint8_t* texel = &image.pixels[(static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column)) * 4];

                if (alpha > texel[3]) {
                    texel[0] = shaded(colour.r, rowShade);
                    texel[1] = shaded(colour.g, rowShade);
                    texel[2] = shaded(colour.b, rowShade);
                    texel[3] = alpha;
                }
            }
        }
    }

    return image;
}

void appendMossClump(IndexedMesh& mesh, const Vec3& top, float length, float turnRadians) {
    const float halfWidth = 0.5f * mossClumpWidthShare * length;
    const Vec3 bottom = top - up * length;

    for (int card = 0; card < 2; ++card) {
        const float turn = turnRadians + 0.25f * fullTurnRadians * static_cast<float>(card);
        const Vec3 across{std::cos(turn), 0.0f, std::sin(turn)};
        // across x up: across then up the card faces this way, and normal x across is up (the bitangent, +v).
        const Vec3 normal = across.cross(up);
        const Vec4 tangent{across.x, across.y, across.z, 1.0f};
        const std::uint32_t corner = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(MeshVertex{bottom - across * halfWidth, normal, Vec2{0.0f, 0.0f}, tangent});
        mesh.vertices.push_back(MeshVertex{bottom + across * halfWidth, normal, Vec2{1.0f, 0.0f}, tangent});
        mesh.vertices.push_back(MeshVertex{top + across * halfWidth, normal, Vec2{1.0f, 1.0f}, tangent});
        mesh.vertices.push_back(MeshVertex{top - across * halfWidth, normal, Vec2{0.0f, 1.0f}, tangent});
        mesh.indices.insert(mesh.indices.end(), {corner, corner + 1, corner + 2, corner, corner + 2, corner + 3});
    }
}

std::vector<MossPlace> overhangLips(const SurfaceGrid& surface, float least) {
    std::vector<MossPlace> lips;
    const std::size_t columns = static_cast<std::size_t>(surface.columns);

    for (std::size_t row = 1; row < static_cast<std::size_t>(surface.rows); ++row) {
        Vec3 middle{0.0f, 0.0f, 0.0f};

        for (std::size_t column = 0; column < columns; ++column) {
            middle = middle + surface.points[row * columns + column];
        }

        middle = middle * (1.0f / static_cast<float>(columns));

        for (std::size_t column = 0; column < columns; ++column) {
            const Vec3& point = surface.points[row * columns + column];
            const Vec3& below = surface.points[(row - 1) * columns + column];
            const Vec3 outward = levelOutward(point - middle);
            const Vec3 standing = point - below;

            if (standing.y > 0.0f && Vec3{standing.x, 0.0f, standing.z}.dot(outward) >= least) {
                lips.push_back(MossPlace{point, outward});
            }
        }
    }

    return lips;
}

void visit(ForestMossSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Longest moss", settings.longestMoss, {.min = 0.5f, .max = 50.0f, .sliderMax = 15.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Moss on tips", settings.tipShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Moss under islands per 10 m", settings.undersidePerTenMetres, {.min = 0.0f, .max = 100.0f, .sliderMax = 20.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Moss on ledges", settings.ledgeShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
}

IndexedMesh pillarMoss(const PillarPlacement& pillar, const SurfaceGrid& surface, const ForestMossSettings& settings) {
    IndexedMesh moss;
    const std::uint32_t seed = forestShapeSeed(pillar.seed, 20);
    const std::vector<MossPlace> lips = overhangLips(surface, mossLipMetres);

    for (std::size_t lip = 0; lip < lips.size(); ++lip) {
        if (forestUnitRandom(seed, 3 * lip) < settings.ledgeShare) {
            addClump(moss, lips[lip].point - lips[lip].outward * mossTuck, settings.longestMoss, seed, lip);
        }
    }

    return moss;
}

IndexedMesh islandMoss(const IslandPlacement& island, const SurfaceGrid& surface, const std::vector<Vec3>& tips, const ForestMossSettings& settings) {
    IndexedMesh moss;
    const std::uint32_t seed = forestShapeSeed(island.seed, 30);

    for (std::size_t tip = 0; tip < tips.size(); ++tip) {
        if (forestUnitRandom(seed, 3 * tip) < settings.tipShare) {
            addClump(moss, tips[tip], tipMossShare * settings.longestMoss, seed, tip);
        }
    }

    const std::vector<std::size_t> underside = islandUnderside(surface, island.radius);
    const int count = underside.empty() ? 0 : std::max(0, static_cast<int>(std::lround(settings.undersidePerTenMetres * island.radius / 10.0f)));
    const std::uint32_t undersideSeed = forestShapeSeed(seed, 1);

    for (int clump = 0; clump < count; ++clump) {
        const std::uint64_t index = static_cast<std::uint64_t>(clump);
        const Vec3 point = surface.points[towardsTheRim(underside, forestUnitRandom(undersideSeed, 3 * index))];
        addClump(moss, point - levelOutward(point) * mossTuck, settings.longestMoss, undersideSeed, index);
    }

    return moss;
}
