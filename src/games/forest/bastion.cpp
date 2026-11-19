#include "games/forest/bastion.h"

#include "core/settings_visitor.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/structures.h"
#include "games/forest/valley_shapes.h"
#include "math/angles.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace {
    // The Bastion sinks this far into the ground, so uneven ground never shows under it; a far tower twice that.
    constexpr float bastionSink = 10.0f;
    constexpr float farTowerSink = 20.0f;

    // The tiers' tops as shares of the height and their widths as shares of the shaft's (the approved concept sheet, A2),
    // their depth this share of their width, their corners chamfered by this share.
    constexpr std::array<float, 3> tierTops{0.49f, 0.77f, 0.92f};
    constexpr std::array<float, 3> tierWidths{1.0f, 0.75f, 0.55f};
    constexpr float bastionDepthShare = 0.75f;

    // The fins' widths (metres) and how often a major one comes, counted from the middle.
    constexpr float finWidth = 1.5f;
    constexpr float majorFinWidth = 6.0f;
    constexpr int majorFinEvery = 4;

    // Fins and corbels start this far inside the face they stand on, so no crack shows along it.
    constexpr float partInset = 0.2f;
    // A corbel is this much narrower than the major fin it stands on, so their sides never lie in one plane (which flickers).
    constexpr float corbelNarrower = 1.0f;

    // The far towers at the old spires' places in the haze (x, z, height), each 4.5-7.5 times as tall as wide; their
    // tiers, glacis and head as shares of their height and width; their fins only the major kind, metres.
    constexpr std::array<std::array<float, 3>, 10> farTowerLayout{{
        {-1800.0f, -3000.0f, 520.0f}, {1500.0f, -3800.0f, 760.0f}, {300.0f, -5200.0f, 640.0f}, {-900.0f, -6500.0f, 800.0f},
        {2600.0f, -2500.0f, 380.0f}, {-2800.0f, -4500.0f, 450.0f}, {900.0f, -7000.0f, 560.0f}, {-3500.0f, -2000.0f, 300.0f},
        {3300.0f, -5600.0f, 700.0f}, {-1500.0f, -4100.0f, 340.0f}
    }};
    constexpr float farLeastSlenderness = 4.5f;
    constexpr float farSlendernessRange = 3.0f;
    constexpr std::array<float, 3> farTierTops{0.5f, 0.78f, 0.9f};
    constexpr std::array<float, 3> farTierWidths{1.0f, 0.72f, 0.5f};
    constexpr float farGlacisHeight = 0.08f;
    constexpr float farGlacisShare = 1.25f;
    constexpr float farHeadOverhang = 0.15f;
    constexpr float farDepthShare = 0.8f;
    constexpr BastionFins farFins{.pitch = 24.0f, .width = 9.0f, .depth = 10.0f, .majorEvery = 1, .majorWidth = 9.0f, .majorDepth = 10.0f};

    // The bridge: its deck's height above the clearing (the old strut's), its width and depth, its parapets' height and
    // thickness, and how far its end goes into the Bastion's face.
    constexpr float bridgeHeight = 331.0f;
    constexpr float bridgeWidth = 14.0f;
    constexpr float bridgeDepth = 8.0f;
    constexpr float parapetHeight = 1.2f;
    constexpr float parapetThickness = 0.6f;
    constexpr float bridgeInside = 4.0f;

    // The window rows (bastionWindowRows): a pair's rows this far apart, the first pair this far above its tier's bottom,
    // none closer than this to its top; their height, shares lit (the command pair over a setback more), cool and fill.
    constexpr float pairApart = 3.0f;
    constexpr float firstPairAbove = 8.0f;
    constexpr float rowsBelowTop = 10.0f;
    constexpr WindowRow tierRow{.height = 0.6f, .lit = 0.45f, .cool = 0.35f, .fill = 0.5f};
    constexpr float commandLit = 0.6f;
    // The head's visor band: three rows at these shares of the head's height, taller, mostly lit, long slits.
    constexpr std::array<float, 3> visorRows{0.3f, 0.5f, 0.7f};
    constexpr WindowRow visorRow{.height = 1.0f, .lit = 0.7f, .cool = 0.3f, .fill = 0.75f};

    // The light strips: their width (at most this share of a major fin's), how far proud of the fin and how far into it
    // they stand, how far in from their tier's ends they stop, and the least tier height that takes one.
    constexpr float stripWidth = 1.5f;
    constexpr float stripFinShare = 0.4f;
    constexpr float stripProud = 0.2f;
    constexpr float stripInto = 0.05f;
    constexpr float stripEndInset = 8.0f;

    // The floodlights: how far out from the wall above they stand (on a ledge; on the ground, from the glacis's foot), how
    // far they lean towards it (degrees), and where the two before a long face stand across it (shares of its width).
    constexpr float floodOutOnLedge = 4.0f;
    constexpr float floodOutOnGround = 6.0f;
    constexpr float floodLeanOnLedge = 8.0f;
    constexpr float floodLeanOnGround = 15.0f;
    constexpr float floodAcross = 0.3f;

    // A rectangle at a height with its corners chamfered, round from -x through +z to +x and -z (rising lofts face out).
    std::vector<Vec3> chamferedRectangle(float y, float width, float depth, float chamfer) {
        const float halfWidth = 0.5f * width;
        const float halfDepth = 0.5f * depth;
        const float cut = std::min(chamfer, 0.45f * std::min(width, depth));
        return {{-halfWidth, y, halfDepth - cut}, {-halfWidth + cut, y, halfDepth}, {halfWidth - cut, y, halfDepth}, {halfWidth, y, halfDepth - cut},
                {halfWidth, y, -halfDepth + cut}, {halfWidth - cut, y, -halfDepth}, {-halfWidth + cut, y, -halfDepth}, {-halfWidth, y, -halfDepth + cut}};
    }

    // A face of a tier: the way it faces (level), the way across it, how far it stands from the middle, its flat span.
    struct TierFace {
        Vec3 normal;
        Vec3 across;
        float distance;
        float span;
    };

    std::array<TierFace, 4> tierFaces(float width, float depth, float chamfer) {
        const float flatWidth = width - 2.0f * chamfer;
        const float flatDepth = depth - 2.0f * chamfer;
        return {{{Vec3{0.0f, 0.0f, 1.0f}, Vec3{1.0f, 0.0f, 0.0f}, 0.5f * depth, flatWidth},
                 {Vec3{0.0f, 0.0f, -1.0f}, Vec3{1.0f, 0.0f, 0.0f}, 0.5f * depth, flatWidth},
                 {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 0.5f * width, flatDepth},
                 {Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 0.5f * width, flatDepth}}};
    }

    // A box standing on a face from a height up: centred across it, a width wide, out from just inside it to depth beyond.
    void appendFin(Mesh& mesh, const TierFace& face, float centre, float width, float depth, float bottom, float top, std::uint32_t seed,
                   std::uint64_t& facet) {
        const Vec3 base{0.0f, bottom, 0.0f};
        const Vec3 left = face.across * (centre - 0.5f * width);
        const Vec3 right = face.across * (centre + 0.5f * width);
        const Vec3 inner = face.normal * (face.distance - partInset);
        const Vec3 outer = face.normal * (face.distance + depth);
        appendExtrusion(mesh, {base + left + inner, base + right + inner, base + right + outer, base + left + outer}, Vec3{0.0f, top - bottom, 0.0f}, seed,
                        facet, FacetNumbers::PerFace);
    }

    // A 45-degree corbel under the head: a right triangle from an overhang below the head on the face out to the head's
    // edge under it, as wide as asked, centred across the face.
    void appendCorbel(Mesh& mesh, const TierFace& face, float centre, float width, float headBottom, float overhang, std::uint32_t seed,
                      std::uint64_t& facet) {
        const Vec3 side = face.across * (centre - 0.5f * width);
        const Vec3 inner = face.normal * (face.distance - partInset);
        const Vec3 outer = face.normal * (face.distance + overhang);
        appendExtrusion(mesh, {side + inner + Vec3{0.0f, headBottom - overhang, 0.0f}, side + inner + Vec3{0.0f, headBottom, 0.0f}, side + outer + Vec3{0.0f, headBottom, 0.0f}},
                        face.across * width, seed, facet, FacetNumbers::PerFace);
    }

    // A tower of the kit from its numbers: shares of its height for the tiers' tops and of its width for theirs.
    BastionShape towerShape(Vec3 foot, float height, float width, float depthShare, float sink, float glacisTop, float glacisWidth,
                            const std::array<float, 3>& tops, const std::array<float, 3>& widths, float overhang, const BastionFins& fins, std::uint32_t seed) {
        BastionShape shape;
        shape.foot = foot;
        shape.sink = sink;
        shape.depthShare = depthShare;
        shape.glacisTop = glacisTop;
        shape.glacisWidth = glacisWidth;
        shape.headOverhang = overhang;
        shape.fins = fins;
        shape.seed = seed;
        float bottom = glacisTop;

        for (std::size_t tier = 0; tier < tops.size(); ++tier) {
            // Each tier at least a metre tall, whatever the settings ask, so the shape stays in order.
            const float top = std::max(tops[tier] * height, bottom + 1.0f);
            shape.tiers.push_back(BastionTier{.bottom = bottom, .top = top, .width = widths[tier] * width});
            bottom = top;
        }

        shape.headBottom = bottom;
        shape.headTop = std::max(height, bottom + 1.0f);
        return shape;
    }
}

void visit(ForestBastionSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Height", settings.height, {.min = 100.0f, .max = 2000.0f, .sliderMax = 1000.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Shaft width", settings.shaftWidth, {.min = 20.0f, .max = 600.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Glacis height", settings.glacisHeight, {.min = 1.0f, .max = 290.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Glacis share", settings.glacisShare, {.min = 1.0f, .max = 3.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Fin pitch", settings.finPitch, {.min = 0.0f, .max = 100.0f, .sliderMax = 30.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Fin depth", settings.finDepth, {.min = 0.0f, .max = 50.0f, .sliderMax = 10.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Major fin depth", settings.majorFinDepth, {.min = 0.0f, .max = 50.0f, .sliderMax = 20.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Head overhang", settings.headOverhang, {.min = 0.0f, .max = 100.0f, .sliderMax = 60.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Concrete albedo", settings.concreteAlbedo, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.6f});
    visitor.field("Concrete roughness", settings.concreteRoughness, {.min = 0.0f, .max = 1.0f});
    visitor.field("Formwork depth", settings.formworkDepth, {.min = 0.0f, .max = 10.0f, .sliderMax = 3.0f});
    visitor.field("Formwork tile", settings.formworkTile, {.min = 1.0f, .max = 200.0f, .sliderMax = 64.0f, .unit = "m"});
    visitor.field("Windows lit", settings.windowsLit, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Window brightness", settings.windowBrightness, {.min = 0.0f, .max = 1000.0f, .sliderMax = 50.0f});
    visitor.field("Window pitch", settings.windowPitch, {.min = 0.5f, .max = 50.0f, .sliderMax = 20.0f, .unit = "m"});
    visitor.field("Strip brightness", settings.stripBrightness, {.min = 0.0f, .max = 1000.0f, .sliderMax = 100.0f});
    visitor.field("Flood intensity", settings.floodIntensity, {.min = 0.0f, .max = 100000.0f, .sliderMax = 10000.0f});
    visitor.field("Flood range", settings.floodRange, {.min = 1.0f, .max = 2000.0f, .sliderMax = 800.0f, .unit = "m"});
}

BastionShape monolithBastion(float groundLevel, const ForestBastionSettings& settings) {
    const float width = settings.shaftWidth;
    const Vec3 foot{forestMonolithPlace.x, groundLevel, forestMonolithPlace.y - 0.5f * bastionDepthShare * width};
    const BastionFins fins{.pitch = settings.finPitch, .width = finWidth, .depth = settings.finDepth, .majorEvery = majorFinEvery, .majorWidth = majorFinWidth,
                           .majorDepth = settings.majorFinDepth};
    return towerShape(foot, settings.height, width, bastionDepthShare, bastionSink, settings.glacisHeight, settings.glacisShare * width, tierTops, tierWidths,
                      settings.headOverhang, fins, forestShapeSeed(0, 44));
}

std::vector<BastionShape> farTowers() {
    const std::uint32_t seed = forestShapeSeed(0, 45);
    std::vector<BastionShape> towers;

    for (std::size_t index = 0; index < farTowerLayout.size(); ++index) {
        const auto& [x, z, height] = farTowerLayout[index];
        const float width = height / (farLeastSlenderness + farSlendernessRange * forestUnitRandom(seed, index));
        const Vec3 foot{x, forestHeight(x, z), z};
        towers.push_back(towerShape(foot, height, width, farDepthShare, farTowerSink, farGlacisHeight * height, farGlacisShare * width, farTierTops,
                                    farTierWidths, farHeadOverhang * width, farFins, forestShapeSeed(static_cast<std::uint32_t>(index), 46)));
    }

    return towers;
}

/*
* n = 2 round((span / pitch - 1) / 2) + 1 is the odd count nearest span / pitch; each fin in the middle of its share.
*/
std::vector<FinPlace> finPlaces(float span, const BastionFins& fins) {
    std::vector<FinPlace> places;

    if (!(fins.pitch > 0.0f) || span < fins.pitch) {
        return places;
    }

    const int count = 2 * static_cast<int>(std::lround(0.5f * (span / fins.pitch - 1.0f))) + 1;
    const float step = span / static_cast<float>(count);
    const int middle = count / 2;

    for (int index = 0; index < count; ++index) {
        const bool major = fins.majorEvery > 0 && std::abs(index - middle) % fins.majorEvery == 0;
        places.push_back(FinPlace{.centre = -0.5f * span + (static_cast<float>(index) + 0.5f) * step, .major = major});
    }

    return places;
}

/*
* The glacis a loft from its foot's section to the first tier's, every tier and the head a chamfered box, the fins boxes
* on each tier's faces (none wider than its share of the span), the corbels under the head at the top tier's major fins.
*/
Mesh bastionMesh(const BastionShape& shape) {
    Mesh mesh;
    std::uint64_t facet = 0;
    const auto depthOf = [&](float width) {
        return shape.depthShare * width;
    };
    const auto chamferOf = [&](float width) {
        return shape.chamferShare * width;
    };

    const BastionTier& first = shape.tiers.front();
    appendLoft(mesh, {chamferedRectangle(-shape.sink, shape.glacisWidth, depthOf(shape.glacisWidth), chamferOf(shape.glacisWidth)),
                      chamferedRectangle(shape.glacisTop, first.width, depthOf(first.width), chamferOf(first.width))},
               shape.seed, facet, FacetNumbers::PerFace);

    for (const BastionTier& tier : shape.tiers) {
        const float depth = depthOf(tier.width);
        const float chamfer = chamferOf(tier.width);
        appendLoft(mesh, {chamferedRectangle(tier.bottom, tier.width, depth, chamfer), chamferedRectangle(tier.top, tier.width, depth, chamfer)}, shape.seed, facet,
                   FacetNumbers::PerFace);

        for (const TierFace& face : tierFaces(tier.width, depth, chamfer)) {
            const std::vector<FinPlace> places = finPlaces(face.span, shape.fins);
            const float share = places.empty() ? 0.0f : face.span / static_cast<float>(places.size());

            for (const FinPlace& place : places) {
                const float width = std::min(place.major ? shape.fins.majorWidth : shape.fins.width, 0.9f * share);
                const float depth = place.major ? shape.fins.majorDepth : shape.fins.depth;

                if (width > 0.0f && depth > 0.0f) {
                    appendFin(mesh, face, place.centre, width, depth, tier.bottom, tier.top, shape.seed, facet);
                }
            }
        }
    }

    const BastionTier& top = shape.tiers.back();
    const float headWidth = top.width + 2.0f * shape.headOverhang;
    const float headDepth = depthOf(top.width) + 2.0f * shape.headOverhang;
    appendLoft(mesh, {chamferedRectangle(shape.headBottom, headWidth, headDepth, chamferOf(headWidth)), chamferedRectangle(shape.headTop, headWidth, headDepth, chamferOf(headWidth))},
               shape.seed, facet, FacetNumbers::PerFace);

    if (shape.headOverhang > 0.0f) {
        for (const TierFace& face : tierFaces(top.width, depthOf(top.width), chamferOf(top.width))) {
            const std::vector<FinPlace> places = finPlaces(face.span, shape.fins);
            const float share = places.empty() ? 0.0f : face.span / static_cast<float>(places.size());

            for (const FinPlace& place : places) {
                const float width = std::min(shape.fins.majorWidth, 0.9f * share) - corbelNarrower;

                if (place.major && width > 0.0f) {
                    appendCorbel(mesh, face, place.centre, width, shape.headBottom, shape.headOverhang, shape.seed, facet);
                }
            }
        }
    }

    return mesh;
}

float bastionWidthAt(const BastionShape& shape, float height) {
    for (const BastionTier& tier : shape.tiers) {
        if (height >= tier.bottom && height < tier.top) {
            return tier.width;
        }
    }

    if (height >= shape.headBottom && height <= shape.headTop) {
        return shape.tiers.back().width + 2.0f * shape.headOverhang;
    }

    return 0.0f;
}

BridgeShape monolithBridge(float groundLevel, const BastionShape& bastion) {
    const PillarPlacement pillar = monolithPillar(groundLevel);
    const float deck = groundLevel + bridgeHeight;
    const float face = bastion.foot.x - 0.5f * bastionWidthAt(bastion, bridgeHeight + groundLevel - bastion.foot.y);
    return BridgeShape{.from = Vec3{pillar.foot.x + 0.6f * pillar.radius, deck, pillar.foot.z},
                       .to = Vec3{face + bridgeInside, deck, pillar.foot.z},
                       .width = bridgeWidth,
                       .depth = bridgeDepth,
                       .seed = forestShapeSeed(0, 42)};
}

/*
* The section square to the bridge's run: its deck on top, its sides straight down a third of its depth, then chamfered
* in to its narrower underside; a parapet on each edge of the deck.
*/
Mesh bridgeMesh(const BridgeShape& bridge) {
    const Vec3 run = bridge.to - bridge.from;
    const Vec3 across = run.normalized().cross(Vec3{0.0f, 1.0f, 0.0f}).normalized();
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const float half = 0.5f * bridge.width;
    const float depth = bridge.depth;
    Mesh mesh;
    std::uint64_t facet = 0;
    appendExtrusion(mesh, sectionAt(bridge.from, across, up, {{-half, -0.35f * depth}, {-half, 0.0f}, {half, 0.0f}, {half, -0.35f * depth}, {0.6f * half, -depth}, {-0.6f * half, -depth}}),
                    run, bridge.seed, facet, FacetNumbers::PerFace);

    for (const float side : {-1.0f, 1.0f}) {
        const float outer = side * half;
        const float inner = side * (half - parapetThickness);
        appendExtrusion(mesh, sectionAt(bridge.from, across, up, {{inner, 0.0f}, {inner, parapetHeight}, {outer, parapetHeight}, {outer, 0.0f}}), run, bridge.seed,
                        facet, FacetNumbers::PerFace);
    }

    return mesh;
}

/*
* Pairs up each tier from firstPairAbove until rowsBelowTop is left, the first over a setback (any tier but the first)
* the command level; then the visor rows round the head.
*/
std::vector<WindowRow> bastionWindowRows(const BastionShape& shape) {
    std::vector<WindowRow> rows;

    for (std::size_t tier = 0; tier < shape.tiers.size(); ++tier) {
        const BastionTier& level = shape.tiers[tier];

        for (float pair = level.bottom + firstPairAbove; pair + pairApart + tierRow.height <= level.top - rowsBelowTop; pair += windowPairSpacing) {
            const bool command = tier > 0 && pair == level.bottom + firstPairAbove;

            for (const float bottom : {pair, pair + pairApart}) {
                WindowRow row = tierRow;
                row.bottom = bottom;
                row.lit = command ? commandLit : tierRow.lit;
                rows.push_back(row);
            }
        }
    }

    const float headHeight = shape.headTop - shape.headBottom;

    for (const float share : visorRows) {
        WindowRow row = visorRow;
        row.bottom = shape.headBottom + share * headHeight - 0.5f * visorRow.height;
        rows.push_back(row);
    }

    return rows;
}

/*
* Each texel's cover is the share of its height under rows; its shares are the row nearest its middle (by the gap to the
* row's span), so the blank wall carries them and mips average only the cover.
*/
ImageData windowBandImage(const BastionShape& shape, const std::vector<WindowRow>& rows, float litScale) {
    const float span = shape.headTop + shape.sink;
    const auto texels = static_cast<std::uint32_t>(std::ceil(span * windowStripTexelsPerMetre));
    const float texelHeight = span / static_cast<float>(texels);
    ImageData image{texels, 1, std::vector<std::uint8_t>(static_cast<std::size_t>(texels) * 4, 0), TextureEncoding::Linear};
    image.storage = TextureStorage::Exact;

    if (rows.empty()) {
        return image;
    }

    const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };

    for (std::uint32_t texel = 0; texel < texels; ++texel) {
        const float low = -shape.sink + texelHeight * static_cast<float>(texel);
        const float high = low + texelHeight;
        const float middle = 0.5f * (low + high);
        float cover = 0.0f;
        const WindowRow* nearest = &rows.front();
        float nearestGap = 1e30f;

        for (const WindowRow& row : rows) {
            cover += std::max(std::min(high, row.bottom + row.height) - std::max(low, row.bottom), 0.0f) / texelHeight;
            const float gap = std::max({row.bottom - middle, middle - (row.bottom + row.height), 0.0f});

            if (gap < nearestGap) {
                nearestGap = gap;
                nearest = &row;
            }
        }

        std::uint8_t* pixel = &image.pixels[static_cast<std::size_t>(texel) * 4];
        pixel[0] = byte(cover);
        pixel[1] = byte(nearest->lit * litScale);
        pixel[2] = byte(nearest->cool);
        pixel[3] = byte(nearest->fill);
    }

    return image;
}

// A bar on each major fin's front, on every face of every tier tall enough for its insets.
Mesh bastionStripsMesh(const BastionShape& shape) {
    Mesh mesh;
    std::uint64_t facet = 0;

    for (const BastionTier& tier : shape.tiers) {
        const float bottom = tier.bottom + stripEndInset;
        const float top = tier.top - stripEndInset;

        if (top <= bottom) {
            continue;
        }

        const float depth = shape.depthShare * tier.width;
        const float chamfer = shape.chamferShare * tier.width;

        for (const TierFace& face : tierFaces(tier.width, depth, chamfer)) {
            const std::vector<FinPlace> places = finPlaces(face.span, shape.fins);
            const float share = places.empty() ? 0.0f : face.span / static_cast<float>(places.size());
            const float finWidth = std::min(shape.fins.majorWidth, 0.9f * share);
            const float width = std::min(stripWidth, stripFinShare * finWidth);

            for (const FinPlace& place : places) {
                if (!place.major || width <= 0.0f || shape.fins.majorDepth <= 0.0f) {
                    continue;
                }

                const Vec3 base{0.0f, bottom, 0.0f};
                const Vec3 left = face.across * (place.centre - 0.5f * width);
                const Vec3 right = face.across * (place.centre + 0.5f * width);
                const Vec3 inner = face.normal * (face.distance + shape.fins.majorDepth - stripInto);
                const Vec3 outer = face.normal * (face.distance + shape.fins.majorDepth + stripProud);
                appendExtrusion(mesh, {base + left + inner, base + right + inner, base + right + outer, base + left + outer}, Vec3{0.0f, top - bottom, 0.0f},
                                shape.seed, facet, FacetNumbers::PerFace);
            }
        }
    }

    return mesh;
}

/*
* A level's floods stand before the wall above it: on the ground before the glacis's foot for the first tier, on the
* ledge below for the others. Each aims up, turned towards the wall by its lean.
*/
std::vector<FloodPlace> bastionFloods(const BastionShape& shape) {
    std::vector<FloodPlace> floods;

    for (std::size_t tier = 0; tier < shape.tiers.size(); ++tier) {
        const bool onGround = tier == 0;
        const float wallWidth = onGround ? shape.glacisWidth : shape.tiers[tier].width;
        const float out = onGround ? floodOutOnGround : floodOutOnLedge;
        const float lean = radiansFromDegrees(onGround ? floodLeanOnGround : floodLeanOnLedge);
        const float y = onGround ? 0.0f : shape.tiers[tier].bottom;

        for (const TierFace& face : tierFaces(wallWidth, shape.depthShare * wallWidth, shape.chamferShare * wallWidth)) {
            const bool longFace = std::fabs(face.normal.z) > 0.5f;
            const std::vector<float> across = longFace ? std::vector<float>{-floodAcross * wallWidth, floodAcross * wallWidth} : std::vector<float>{0.0f};

            for (const float along : across) {
                const Vec3 position = face.normal * (face.distance + out) + face.across * along + Vec3{0.0f, y, 0.0f};
                const Vec3 direction = Vec3{0.0f, std::cos(lean), 0.0f} - face.normal * std::sin(lean);
                floods.push_back(FloodPlace{.position = position, .direction = direction});
            }
        }
    }

    return floods;
}
