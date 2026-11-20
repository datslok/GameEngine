#include "games/forest/lianas.h"
#include "games/forest/forest_terrain.h"
#include "scene/liana_shape.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
    // The census's bands: the share of lianas up to each band's top, and its stems across (metres).
    struct DiameterBand {
        float shareEnd;
        float least;
        float most;
    };

    constexpr DiameterBand diameterBands[] = {
        {0.50f, 0.01f, 0.02f},
        {0.92f, 0.02f, 0.05f},
        {0.99f, 0.05f, 0.10f},
        {1.00f, 0.10f, 0.20f},
    };


    // A catenary's length for a sag below its chord of share of its span, from the shallow-sag series
    // L = span (1 + 8/3 share^2), close enough for choosing how much a loop hangs.
    float loopLength(float span, float share) {
        return span * (1.0f + 8.0f / 3.0f * share * share);
    }
}

bool insideLianaCrown(const LianaCrown& crown, const Vec3& point) {
    const Vec3 offset = point - crown.centre;
    const float across = (offset.x * offset.x + offset.z * offset.z) / std::max(crown.radius * crown.radius, 1e-12f);
    const float up = offset.y * offset.y / std::max(crown.halfHeight * crown.halfHeight, 1e-12f);
    return across + up <= 1.0f;
}

/*
* Measured on the envelope itself, not a sphere round it: a sphere's radius is the crown's width, so holds a share of
* it down hung metres below a flat crown's underside, in the open (the ends seen floating).
*/
Vec3 lianaCrownHold(const LianaCrown& crown, float turn, float out, float down) {
    const float angle = turn * fullTurnRadians;
    const float depth = 0.25f + 0.4f * down;
    const float widthThere = crown.radius * std::sqrt(1.0f - depth * depth);
    const float outward = (0.3f + 0.4f * out) * widthThere;
    return crown.centre + Vec3{std::cos(angle) * outward, -depth * crown.halfHeight, std::sin(angle) * outward};
}

float lianaDiameter(float share) {
    const float clamped = std::clamp(share, 0.0f, 1.0f);
    float bandStart = 0.0f;

    for (const DiameterBand& band : diameterBands) {
        if (clamped < band.shareEnd || band.shareEnd >= 1.0f) {
            const float within = (clamped - bandStart) / (band.shareEnd - bandStart);
            return band.least + (band.most - band.least) * std::clamp(within, 0.0f, 1.0f);
        }

        bandStart = band.shareEnd;
    }

    return diameterBands[3].most;
}

/*
* In the upright plane through the holds, x across (d of it) and y up (h of it): the catenary's a and lowest point x0
* (fitCatenary, scene/liana_shape.h), and its length from the first hold s(x) = a (sinh((x - x0) / a) - sinh(-x0 / a)),
* which inverts in closed form, so the points are spaced evenly along the line itself.
*/
HangingStrand catenaryStrand(const Vec3& from, const Vec3& to, float length, float radius, float step) {
    const Vec3 offset = to - from;
    const Vec3 across{offset.x, 0.0f, offset.z};
    const double d = across.length();
    const double h = offset.y;
    const double straight = offset.length();
    const int segments = std::max(1, static_cast<int>(std::ceil(std::max(static_cast<double>(length), straight) / step)));
    HangingStrand strand;

    if (d < 1e-3 || length <= straight * (1.0 + 1e-6)) {
        for (int point = 0; point <= segments; ++point) {
            const float share = static_cast<float>(point) / static_cast<float>(segments);
            strand.points.push_back(from + offset * share);
            strand.radii.push_back(radius);
        }

        strand.points.back() = to;
        return strand;
    }

    const double total = length;
    const CatenaryFit fit = fitCatenary(d, h, total);
    const double a = fit.a;
    const double x0 = fit.x0;
    const double startSinh = std::sinh(-x0 / a);
    const double startCosh = std::cosh(-x0 / a);
    const Vec3 direction = across * static_cast<float>(1.0 / d);

    for (int point = 0; point <= segments; ++point) {
        const double s = total * static_cast<double>(point) / static_cast<double>(segments);
        const double x = x0 + a * std::asinh(s / a + startSinh);
        const double y = a * (std::cosh((x - x0) / a) - startCosh);
        strand.points.push_back(from + direction * static_cast<float>(x) + Vec3{0.0f, static_cast<float>(y), 0.0f});
        strand.radii.push_back(radius);
    }

    strand.points.back() = to;
    return strand;
}

void visit(ForestLianaSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Tree share", settings.treeShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Path share", settings.pathShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Path reach", settings.pathReach, {.min = 0.0f, .max = 200.0f, .sliderMax = 60.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Most per tree", settings.mostPerTree, {.min = 1, .max = 20, .sliderMax = 10, .applies = FieldApplies::AtStartup});
    visitor.field("Thickness", settings.thickness, {.min = 0.1f, .max = 10.0f, .sliderMax = 5.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Rope share", settings.ropeShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Neighbour span", settings.neighbourSpan, {.min = 1.0f, .max = 100.0f, .sliderMax = 60.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Least sag", settings.leastSag, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Most sag", settings.mostSag, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Rope slack", settings.ropeSlack, {.min = 0.01f, .max = 0.5f, .applies = FieldApplies::AtStartup});
    visitor.field("Leaf spacing", settings.leafSpacing, {.min = 0.05f, .max = 5.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Sparse leaf spacing", settings.sparseLeafSpacing, {.min = 0.05f, .max = 20.0f, .sliderMax = 5.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Leaf size", settings.leafSize, {.min = 0.02f, .max = 2.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Ivy thinning distance", settings.ivyThinDistance, {.min = 0.0f, .max = 1000.0f, .sliderMax = 300.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Ivy distance", settings.ivyDistance, {.min = 0.0f, .max = 1000.0f, .sliderMax = 300.0f, .unit = "m", .applies = FieldApplies::AtStartup});
}

/*
* A step can jump over a thin slice of crown only where the liana grazes its edge, which the leaves there hardly show;
* a liana enters a crown only at its hold, so walking from the hold finds the first crossing.
*/
float lianaRunInCrown(const LianaShape& shape, const LianaCrown& crown, bool fromFarEnd) {
    const Vec3 from{shape.from[0], shape.from[1], shape.from[2]};
    const float length = shape.to[3];
    const auto pointAt = [&](float metres) {
        const float share = std::clamp(fromFarEnd ? 1.0f - metres / length : metres / length, 0.0f, 1.0f);
        return from + lianaCentre(shape, share).offset;
    };

    if (length <= 0.0f || !insideLianaCrown(crown, pointAt(0.0f))) {
        return 0.0f;
    }

    float inside = 0.0f;

    while (inside < length) {
        const float next = std::min(inside + lianaStepMetres, length);

        if (!insideLianaCrown(crown, pointAt(next))) {
            float outside = next;

            while (outside - inside > 0.001f) {
                const float middle = 0.5f * (inside + outside);
                (insideLianaCrown(crown, pointAt(middle)) ? inside : outside) = middle;
            }

            return 0.5f * (inside + outside);
        }

        inside = next;
    }

    return length;
}

LianaLeaves lianaIvy(const LianaShape& shape, const LianaCrown& first, const LianaCrown* second, const ForestLianaSettings& settings, std::uint32_t seed) {
    return LianaLeaves{.firstCrownMetres = lianaRunInCrown(shape, first, false),
                       .lastCrownMetres = second != nullptr ? lianaRunInCrown(shape, *second, true) : 0.0f,
                       .spacing = settings.leafSpacing,
                       .sparseSpacing = settings.sparseLeafSpacing,
                       .size = settings.leafSize,
                       .thinFrom = settings.ivyThinDistance,
                       .goneBy = settings.ivyDistance,
                       .seed = seed};
}

/*
* Random numbers drawn in a fixed order from (the crown's own seed, a running count), so the same crowns and settings
* always give the same lianas. A crown's neighbours are found by walking every crown: a cell holds a few dozen.
*/
std::vector<Liana> chooseLianas(const std::vector<LianaCrown>& crowns, const ForestLianaSettings& settings, std::uint64_t seed,
                                const std::function<float(float, float)>& groundHeight, const std::function<float(float, float)>& pathDistance) {
    std::vector<Liana> lianas;
    std::uint64_t crownSeed = 0;
    std::uint64_t draw = 0;
    const auto next = [&] {
        return forestUnitRandom(crownSeed, draw++);
    };
    const int most = std::max(settings.mostPerTree, 1);
    const float leastSag = std::min(settings.leastSag, settings.mostSag);
    const float mostSag = std::max(settings.leastSag, settings.mostSag);

    for (std::size_t index = 0; index < crowns.size(); ++index) {
        const LianaCrown& crown = crowns[index];

        if (!crown.carries) {
            continue;
        }

        // The crown's own stream: its identity spread over the bits (an odd multiplier, so no two identities meet).
        crownSeed = seed ^ (crown.identity * 0x9E3779B97F4A7C15ull);
        draw = 0;
        const std::uint32_t lianaSeed = static_cast<std::uint32_t>(crownSeed ^ (crownSeed >> 32));
        const float share = pathDistance(crown.centre.x, crown.centre.z) <= settings.pathReach ? settings.pathShare : settings.treeShare;

        if (next() >= share) {
            continue;
        }

        std::vector<int> neighbours;

        for (std::size_t other = 0; other < crowns.size(); ++other) {
            const Vec3 apart = crowns[other].centre - crown.centre;
            const float across = Vec3{apart.x, 0.0f, apart.z}.length();

            if (other != index && across > 0.0f && across <= settings.neighbourSpan) {
                neighbours.push_back(static_cast<int>(other));
            }
        }

        const int count = 1 + std::min(most - 1, static_cast<int>(next() * static_cast<float>(most)));

        for (int liana = 0; liana < count; ++liana) {
            Liana hung;
            hung.crown = static_cast<int>(index);
            hung.seed = lianaSeed + static_cast<std::uint32_t>(liana);
            const float turn = next();
            const float out = next();
            const float down = next();
            hung.from = lianaCrownHold(crown, turn, out, down);
            hung.radius = 0.5f * lianaDiameter(next()) * settings.thickness;
            const bool rope = neighbours.empty() || next() < settings.ropeShare;

            if (rope) {
                const float angle = next() * fullTurnRadians;
                const float off = 1.0f + 5.0f * next();
                const float x = hung.from.x + std::cos(angle) * off;
                const float z = hung.from.z + std::sin(angle) * off;
                hung.to = Vec3{x, groundHeight(x, z) - lianaGroundSink, z};
                const float slack = 0.01f + (std::max(settings.ropeSlack, 0.01f) - 0.01f) * next();
                hung.length = (hung.to - hung.from).length() * (1.0f + slack);
            } else {
                const int other = neighbours[std::min(neighbours.size() - 1, static_cast<std::size_t>(next() * static_cast<float>(neighbours.size())))];
                hung.otherCrown = other;
                hung.to = lianaCrownHold(crowns[static_cast<std::size_t>(other)], next(), next(), next());
                const float sag = leastSag + (mostSag - leastSag) * next();
                hung.length = loopLength((hung.to - hung.from).length(), sag);
            }

            lianas.push_back(hung);
        }
    }

    return lianas;
}
