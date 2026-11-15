#include "games/forest/lianas.h"
#include "games/forest/forest_terrain.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>
#include <map>
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

    // A hold in a crown's lower part: round its centre at a random turn, 30-70% of its radius out and 25-65% down, so
    // always inside it and below its centre, where its leaves hide the liana's end.
    Vec3 crownHold(const LianaCrown& crown, float turn, float out, float down) {
        const float angle = turn * fullTurnRadians;
        const float outward = (0.3f + 0.4f * out) * crown.radius;
        const float below = (0.25f + 0.4f * down) * crown.radius;
        return crown.centre + Vec3{std::cos(angle) * outward, -below, std::sin(angle) * outward};
    }

    // A catenary's length for a sag below its chord of share of its span, from the shallow-sag series
    // L = span (1 + 8/3 share^2), close enough for choosing how much a loop hangs.
    float loopLength(float span, float share) {
        return span * (1.0f + 8.0f / 3.0f * share * share);
    }
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
* In the upright plane through the holds, x across (d of it) and y up (h of it): 2 a sinh(d / 2a) = sqrt(L^2 - h^2) fixes
* the catenary's a (the left side falls from infinity to d as a grows, so halving finds it, in doubles since sinh of
* d / 2a overflows floats for tight curves), the lowest point x0 = d / 2 - a atanh(h / L), and its length from the first
* hold s(x) = a (sinh((x - x0) / a) - sinh(-x0 / a)), which inverts in closed form, so the points are spaced evenly
* along the line itself.
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
    const double vertical = std::sqrt(total * total - h * h);
    double low = std::log(d / 1400.0);
    double high = std::log(d * 1e6);

    for (int halving = 0; halving < 200; ++halving) {
        const double middle = 0.5 * (low + high);
        const double a = std::exp(middle);
        (2.0 * a * std::sinh(d / (2.0 * a)) > vertical ? low : high) = middle;
    }

    const double a = std::exp(0.5 * (low + high));
    const double x0 = 0.5 * d - a * std::atanh(h / total);
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
    visitor.field("Rope share", settings.ropeShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Neighbour span", settings.neighbourSpan, {.min = 1.0f, .max = 100.0f, .sliderMax = 60.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Least sag", settings.leastSag, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Most sag", settings.mostSag, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Rope slack", settings.ropeSlack, {.min = 0.01f, .max = 0.5f, .applies = FieldApplies::AtStartup});
    visitor.field("Leaf spacing", settings.leafSpacing, {.min = 0.05f, .max = 5.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Leaf size", settings.leafSize, {.min = 0.02f, .max = 2.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
}

/*
* Random numbers drawn in a fixed order from (seed, a running count), so the same crowns and settings always give the
* same lianas. A crown's neighbours are found by walking every crown: a patch has a few hundred.
*/
std::vector<Liana> chooseLianas(const std::vector<LianaCrown>& crowns, const ForestLianaSettings& settings, std::uint64_t seed,
                                const std::function<float(float, float)>& groundHeight, const std::function<float(float, float)>& pathDistance) {
    std::vector<Liana> lianas;
    std::uint64_t draw = 0;
    const auto next = [&] {
        return forestUnitRandom(seed, draw++);
    };
    const int most = std::max(settings.mostPerTree, 1);
    const float leastSag = std::min(settings.leastSag, settings.mostSag);
    const float mostSag = std::max(settings.leastSag, settings.mostSag);

    for (std::size_t index = 0; index < crowns.size(); ++index) {
        const LianaCrown& crown = crowns[index];
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
            const float turn = next();
            const float out = next();
            const float down = next();
            hung.from = crownHold(crown, turn, out, down);
            hung.radius = 0.5f * lianaDiameter(next());
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
                hung.to = crownHold(crowns[static_cast<std::size_t>(other)], next(), next(), next());
                const float sag = leastSag + (mostSag - leastSag) * next();
                hung.length = loopLength((hung.to - hung.from).length(), sag);
            }

            lianas.push_back(hung);
        }
    }

    return lianas;
}

namespace {
    // The runs of a strand's points inside a crown, each as a strand of its own (two points at least).
    std::vector<HangingStrand> runsInside(const HangingStrand& strand, const LianaCrown* first, const LianaCrown* second) {
        std::vector<HangingStrand> runs;
        HangingStrand run;
        const auto inside = [&](const Vec3& point) {
            return (first != nullptr && (point - first->centre).length() < first->radius) ||
                   (second != nullptr && (point - second->centre).length() < second->radius);
        };

        for (std::size_t point = 0; point < strand.points.size(); ++point) {
            if (inside(strand.points[point])) {
                run.points.push_back(strand.points[point]);
                run.radii.push_back(strand.radii[point]);
                continue;
            }

            if (run.points.size() >= 2) {
                runs.push_back(std::move(run));
            }

            run = HangingStrand{};
        }

        if (run.points.size() >= 2) {
            runs.push_back(std::move(run));
        }

        return runs;
    }

    // Each liana's seed for its leaves.
    constexpr std::uint32_t lianaLeafSeed = 0x4c49414eu;
}

std::vector<LianaMeshes> lianaMeshes(const std::vector<Liana>& lianas, const std::vector<LianaCrown>& crowns, const ForestLianaSettings& settings,
                                     float barkTileMetres, float chunkMetres, JobSystem& jobs) {
    // Chunks in the order of their squares, each listing its lianas in their own order.
    std::map<std::pair<int, int>, std::vector<std::size_t>> squares;

    for (std::size_t index = 0; index < lianas.size(); ++index) {
        const Vec3& hold = lianas[index].from;
        squares[{static_cast<int>(std::floor(hold.x / chunkMetres)), static_cast<int>(std::floor(hold.z / chunkMetres))}].push_back(index);
    }

    std::vector<std::vector<std::size_t>> chunks;

    for (auto& [square, members] : squares) {
        chunks.push_back(std::move(members));
    }

    std::vector<LianaMeshes> meshes(chunks.size());
    jobs.parallelFor(chunks.size(), 1, [&](std::size_t chunk) {
        LianaMeshes& built = meshes[chunk];

        for (const std::size_t index : chunks[chunk]) {
            const Liana& liana = lianas[index];
            const HangingStrand strand = catenaryStrand(liana.from, liana.to, liana.length, liana.radius);
            appendTube(built.bark, strand, 2.0f * liana.radius >= lianaThickDiameter ? 6 : 4, barkTileMetres);

            const LianaCrown* first = &crowns[static_cast<std::size_t>(liana.crown)];
            const LianaCrown* second = liana.otherCrown >= 0 ? &crowns[static_cast<std::size_t>(liana.otherCrown)] : nullptr;
            std::uint32_t run = 0;

            for (const HangingStrand& leafy : runsInside(strand, first, second)) {
                appendLeafCards(built.leaves, leafy, settings.leafSpacing, settings.leafSize,
                                forestShapeSeed(lianaLeafSeed + static_cast<std::uint32_t>(index), ++run));
            }
        }
    });

    return meshes;
}
