#include "games/forest/epiphytes.h"
#include "games/forest/forest_ground.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/palms.h"
#include "math/angles.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
    const Vec3 up{0.0f, 1.0f, 0.0f};

    // The golden angle, pi (3 - sqrt 5): each leaf turned this far round from the last, so none lines up with another.
    constexpr float goldenAngle = 2.39996323f;

    // A crotch's rosette sits this share of the node's radius above it, among the limbs leaving it.
    constexpr float crotchLift = 0.25f;

    // A leaf leaves this share of the rosette's length out from its middle; each is 85-100% of the rosette's length.
    constexpr float leafBaseShare = 0.04f;
    constexpr float leastLeafShare = 0.85f;

    /*
    * Each kind's leaves (docs/research/VINES.md, Epiphytes): the youngest's and the oldest's elevation (degrees above level)
    * and droop (StrapFold's), the fold of the halves, the widest across as a share of the length and the segments
    * along. A bird's-nest fern's fronds open as a funnel, the outer ones arching over and down, nearly flat across; a
    * tank bromeliad's are stiffer and channelled, so they hold water.
    */
    struct LeafHabit {
        float youngestElevationDegrees;
        float oldestElevationDegrees;
        float youngestDroop;
        float oldestDroop;
        float foldDegrees;
        float widthShare;
        int segments;
    };

    constexpr LeafHabit fernHabit{75.0f, 45.0f, 0.4f, 0.9f, 12.0f, 0.15f, 6};
    constexpr LeafHabit bromeliadHabit{80.0f, 30.0f, 0.15f, 0.45f, 35.0f, 0.14f, 4};
    constexpr float elevationJitterDegrees = 5.0f;

    std::uint8_t channelByte(float value) {
        return static_cast<std::uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
    }

    // Moss's shell: how far out from the bark, and its pieces round.
    constexpr float mossThickness = 0.01f;
    constexpr int mossArcPieces = 6;

    // The thickness a tree's limbs need, metres: a share of its trunk's radius, at least the least.
    float limbThreshold(const TreeSkeleton& skeleton, float share, float least) {
        return std::max(share * skeleton.trunkRadius, least);
    }

    // A chain point's radius as its tube was built: a side chain's first point (its parent's node) takes the chain's own.
    float chainRadius(const TreeSkeleton& skeleton, const std::vector<int>& chain, std::size_t point) {
        return skeleton.radii[static_cast<std::size_t>(chain[std::max<std::size_t>(point, 1)])];
    }

    // The way out of a limb's top square to its direction: up made square to it (straight up for a level limb).
    Vec3 topOf(const Vec3& along) {
        const Vec3 top = up - along * up.dot(along);
        return top.lengthSquared() > 1e-8f ? top.normalized() : Vec3{1.0f, 0.0f, 0.0f};
    }

    // Whether a direction is no steeper than an angle above or below level.
    bool levelEnough(const Vec3& along, float steepestDegrees) {
        return std::abs(along.y) <= std::sin(radiansFromDegrees(steepestDegrees));
    }

    // A vertex whose tangent runs along +u and whose bitangent (w cross(normal, tangent)) runs along +v.
    MeshVertex mossVertex(const Vec3& position, const Vec3& normal, const Vec3& tangent, const Vec3& alongV, const Vec2& uv) {
        const float handedness = normal.cross(tangent).dot(alongV) >= 0.0f ? 1.0f : -1.0f;
        return MeshVertex{position, normal, uv, Vec4{tangent.x, tangent.y, tangent.z, handedness}};
    }

    // A triangle wound to face the way its corners' normals do.
    void appendFacing(IndexedMesh& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        const Vec3 facing = (mesh.vertices[b].position - mesh.vertices[a].position).cross(mesh.vertices[c].position - mesh.vertices[a].position);

        if (facing.dot(mesh.vertices[a].normal + mesh.vertices[b].normal + mesh.vertices[c].normal) >= 0.0f) {
            mesh.indices.insert(mesh.indices.end(), {a, b, c});
        } else {
            mesh.indices.insert(mesh.indices.end(), {a, c, b});
        }
    }

    // One run of a chain's points (first to last, inclusive) covered by a moss shell.
    void appendMossRun(IndexedMesh& mesh, const TreeSkeleton& skeleton, const std::vector<int>& chain, std::size_t first, std::size_t last,
                       float arcRadians, float tileMetres) {
        const std::uint32_t start = static_cast<std::uint32_t>(mesh.vertices.size());
        const std::size_t count = last - first + 1;
        float travelled = 0.0f;

        for (std::size_t point = first; point <= last; ++point) {
            const Vec3& place = skeleton.nodes[static_cast<std::size_t>(chain[point])].position;
            const Vec3 before = skeleton.nodes[static_cast<std::size_t>(chain[point == first ? point : point - 1])].position;
            const Vec3 after = skeleton.nodes[static_cast<std::size_t>(chain[point == last ? point : point + 1])].position;
            const Vec3 along = (after - before).normalized();
            const Vec3 top = topOf(along);
            const Vec3 side = along.cross(top).normalized();
            const float radius = chainRadius(skeleton, chain, point) + mossThickness;

            if (point > first) {
                travelled += (place - skeleton.nodes[static_cast<std::size_t>(chain[point - 1])].position).length();
            }

            // Narrowing to nothing at the run's ends, full over its middle third.
            const float share = static_cast<float>(point - first) / static_cast<float>(count - 1);
            const float halfArc = 0.5f * arcRadians * std::min(1.0f, 3.0f * std::min(share, 1.0f - share));

            for (int piece = 0; piece <= mossArcPieces; ++piece) {
                const float angle = halfArc * (2.0f * static_cast<float>(piece) / static_cast<float>(mossArcPieces) - 1.0f);
                const Vec3 out = top * std::cos(angle) + side * std::sin(angle);
                const Vec3 round = side * std::cos(angle) - top * std::sin(angle);
                mesh.vertices.push_back(mossVertex(place + out * radius, out, round, along, Vec2{angle * radius / tileMetres, travelled / tileMetres}));
            }
        }

        const std::uint32_t across = static_cast<std::uint32_t>(mossArcPieces) + 1;

        for (std::uint32_t ring = 0; ring + 1 < static_cast<std::uint32_t>(count); ++ring) {
            for (std::uint32_t piece = 0; piece < static_cast<std::uint32_t>(mossArcPieces); ++piece) {
                const std::uint32_t a = start + ring * across + piece;
                const std::uint32_t b = a + across;
                appendFacing(mesh, a, a + 1, b + 1);
                appendFacing(mesh, a, b + 1, b);
            }
        }
    }
}

void visit(ForestEpiphyteSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Least per big tree", settings.leastPerBigTree, {.min = 0, .max = 500, .sliderMax = 100, .applies = FieldApplies::AtStartup});
    visitor.field("Most per big tree", settings.mostPerBigTree, {.min = 0, .max = 500, .sliderMax = 150, .applies = FieldApplies::AtStartup});
    visitor.field("Most per small tree", settings.mostPerSmallTree, {.min = 0, .max = 100, .sliderMax = 20, .applies = FieldApplies::AtStartup});
    visitor.field("Limb share", settings.limbShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Least limb radius", settings.leastLimbRadius, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.2f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Steepest limb", settings.steepestLimbDegrees, {.min = 0.0f, .max = 90.0f, .unit = "deg", .applies = FieldApplies::AtStartup});
    visitor.field("Spacing", settings.spacing, {.min = 0.1f, .max = 20.0f, .sliderMax = 5.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Fern share", settings.fernShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Fern length", settings.fernLength, {.min = 0.05f, .max = 5.0f, .sliderMax = 2.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Fern fronds", settings.fernFronds, {.min = 1, .max = 60, .sliderMax = 30, .applies = FieldApplies::AtStartup});
    visitor.field("Bromeliad length", settings.bromeliadLength, {.min = 0.05f, .max = 5.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Bromeliad leaves", settings.bromeliadLeaves, {.min = 1, .max = 60, .sliderMax = 30, .applies = FieldApplies::AtStartup});
    visitor.field("Moss", settings.moss, FieldApplies::AtStartup);
    visitor.field("Moss limb share", settings.mossLimbShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Moss steepest", settings.mossSteepestDegrees, {.min = 0.0f, .max = 90.0f, .unit = "deg", .applies = FieldApplies::AtStartup});
    visitor.field("Moss arc", settings.mossArcDegrees, {.min = 10.0f, .max = 360.0f, .sliderMax = 240.0f, .unit = "deg", .applies = FieldApplies::AtStartup});
}

std::vector<EpiphyteSite> epiphyteSites(const TreeSkeleton& skeleton, const ForestEpiphyteSettings& settings) {
    const float thick = limbThreshold(skeleton, settings.limbShare, settings.leastLimbRadius);
    std::vector<EpiphyteSite> sites;

    // The crotches: nodes with two children or more.
    std::vector<int> children(skeleton.nodes.size(), 0);

    for (const TreeNode& node : skeleton.nodes) {
        if (node.parent >= 0) {
            ++children[static_cast<std::size_t>(node.parent)];
        }
    }

    for (std::size_t node = 0; node < skeleton.nodes.size(); ++node) {
        if (children[node] >= 2 && skeleton.radii[node] >= thick) {
            sites.push_back(EpiphyteSite{.base = skeleton.nodes[node].position + up * (crotchLift * skeleton.radii[node]), .limbUp = up,
                                         .kind = EpiphyteSiteKind::Crotch, .limbRadius = skeleton.radii[node]});
        }
    }

    // The limbs' tops: each thick, nearly level segment's middle.
    for (const std::vector<int>& chain : skeleton.chains) {
        for (std::size_t point = 1; point < chain.size(); ++point) {
            const Vec3& from = skeleton.nodes[static_cast<std::size_t>(chain[point - 1])].position;
            const Vec3& to = skeleton.nodes[static_cast<std::size_t>(chain[point])].position;
            const float radius = chainRadius(skeleton, chain, point);
            const Vec3 along = (to - from).normalized();

            if (radius < thick || !levelEnough(along, settings.steepestLimbDegrees)) {
                continue;
            }

            const Vec3 top = topOf(along);
            sites.push_back(EpiphyteSite{.base = (from + to) * 0.5f + top * radius, .limbUp = top, .kind = EpiphyteSiteKind::Limb, .limbRadius = radius});
        }
    }

    return sites;
}

std::vector<EpiphyteSite> chooseEpiphyteSites(const std::vector<EpiphyteSite>& sites, int count, float spacing, std::uint32_t seed) {
    std::vector<std::size_t> order(sites.size());
    std::iota(order.begin(), order.end(), 0);
    std::vector<float> keys(sites.size());

    for (std::size_t site = 0; site < sites.size(); ++site) {
        keys[site] = forestUnitRandom(seed, site) + (sites[site].kind == EpiphyteSiteKind::Crotch ? 0.0f : 1.0f);
    }

    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return keys[a] < keys[b];
    });

    std::vector<EpiphyteSite> chosen;
    const float spacingSquared = spacing * spacing;

    for (const std::size_t site : order) {
        if (static_cast<int>(chosen.size()) >= count) {
            break;
        }

        const bool roomy = std::none_of(chosen.begin(), chosen.end(), [&](const EpiphyteSite& taken) {
            return (taken.base - sites[site].base).lengthSquared() < spacingSquared;
        });

        if (roomy) {
            chosen.push_back(sites[site]);
        }
    }

    return chosen;
}

int epiphytesOnTree(const ForestEpiphyteSettings& settings, bool bigTree, std::uint32_t seed) {
    const float draw = forestUnitRandom(seed, 0);
    const int least = bigTree ? std::min(settings.leastPerBigTree, settings.mostPerBigTree) : 0;
    const int most = bigTree ? std::max(settings.leastPerBigTree, settings.mostPerBigTree) : std::max(settings.mostPerSmallTree, 0);
    return least + std::min(most - least, static_cast<int>(draw * static_cast<float>(most - least + 1)));
}

RosetteShape rosetteShape(const ForestEpiphyteSettings& settings, EpiphyteKind kind, std::uint32_t seed) {
    const bool fern = kind == EpiphyteKind::Fern;
    const float length = (fern ? settings.fernLength : settings.bromeliadLength) * (0.5f + forestUnitRandom(seed, 0));
    return RosetteShape{.kind = kind, .length = length, .leaves = fern ? settings.fernFronds : settings.bromeliadLeaves, .seed = seed};
}

void appendRosette(IndexedMesh& mesh, const Vec3& base, const RosetteShape& shape) {
    const LeafHabit& habit = shape.kind == EpiphyteKind::Fern ? fernHabit : bromeliadHabit;
    const StrapFold baseFold{.foldRadians = radiansFromDegrees(habit.foldDegrees), .droop = 0.0f};
    const float phase = fullTurnRadians * forestUnitRandom(shape.seed, 1);
    const std::size_t first = mesh.vertices.size();
    std::uint64_t draw = 2;
    const auto random = [&] {
        return forestUnitRandom(shape.seed, draw++);
    };

    for (int leaf = 0; leaf < shape.leaves; ++leaf) {
        const float age = shape.leaves > 1 ? static_cast<float>(leaf) / static_cast<float>(shape.leaves - 1) : 0.0f;
        const float azimuth = phase + goldenAngle * static_cast<float>(leaf);
        const float elevation = radiansFromDegrees(habit.youngestElevationDegrees + (habit.oldestElevationDegrees - habit.youngestElevationDegrees) * age +
                                                   elevationJitterDegrees * (2.0f * random() - 1.0f));
        const Vec3 level{std::cos(azimuth), 0.0f, -std::sin(azimuth)};
        const Vec3 direction = level * std::cos(elevation) + up * std::sin(elevation);
        const float length = shape.length * (leastLeafShare + (1.0f - leastLeafShare) * random());
        StrapFold fold = baseFold;
        fold.droop = habit.youngestDroop + (habit.oldestDroop - habit.youngestDroop) * age;
        appendStrapLeaf(mesh, base + level * (leafBaseShare * shape.length), direction, up, length, habit.widthShare * length, habit.segments, fold);
    }

    // Into the kind's half of the atlas.
    const float offset = shape.kind == EpiphyteKind::Fern ? 0.0f : 0.5f;

    for (std::size_t vertex = first; vertex < mesh.vertices.size(); ++vertex) {
        mesh.vertices[vertex].uv.x = offset + 0.5f * mesh.vertices[vertex].uv.x;
    }
}

Pixel fernColour(Pixel leaf) {
    return Pixel{channelByte(1.0f * leaf.r), channelByte(1.35f * leaf.g), channelByte(0.7f * leaf.b)};
}

Pixel bromeliadColour(Pixel leaf) {
    return Pixel{channelByte(1.15f * leaf.r), channelByte(0.95f * leaf.g), channelByte(0.9f * leaf.b)};
}

/*
* The fern's veins leave the midrib slanted towards the tip: a vein's place goes with v less a share of how far out the
* texel is, so the lines run up and out, many of them, thin; the midrib dark, the blade a little darker at its edges
* and mottled faintly. The bromeliad's half is copied from the palms' picture, texel for texel.
*/
EpiphyteLeafImages epiphyteLeafImages(int texels, Pixel leaf, std::uint32_t seed) {
    constexpr float veinsAlong = 40.0f;
    constexpr float veinSlant = 6.0f;
    constexpr float veinHalfWidth = 0.18f;
    constexpr float veinShade = 0.12f;
    constexpr float midribHalfWidth = 0.045f;
    constexpr float midribDarkness = 0.75f;
    constexpr float edgeDarkening = 0.2f;
    constexpr float mottle = 0.05f;
    const std::uint32_t size = static_cast<std::uint32_t>(std::max(texels, 1));
    const std::uint32_t width = 2 * size;
    const Pixel fern = fernColour(leaf);
    const PalmLeafletImages bromeliad = palmLeafletImages(static_cast<int>(size), bromeliadColour(leaf), seed);

    EpiphyteLeafImages images;
    images.colour.width = width;
    images.colour.height = size;
    images.colour.encoding = TextureEncoding::Srgb;
    images.colour.pixels.resize(static_cast<std::size_t>(width) * size * 4);
    images.height.width = width;
    images.height.height = size;
    images.height.pixels.resize(static_cast<std::size_t>(width) * size * 4);

    for (std::uint32_t y = 0; y < size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);

        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float out = std::fabs(u - 0.5f) * 2.0f;
            const float midrib = std::max(0.0f, 1.0f - out / midribHalfWidth);
            const float veinPlace = v * veinsAlong - out * veinSlant;
            const float fromVein = std::fabs(veinPlace - std::round(veinPlace));
            const float vein = out > midribHalfWidth ? std::max(0.0f, 1.0f - fromVein / veinHalfWidth) : 0.0f;
            const std::uint64_t cell = static_cast<std::uint64_t>(u * 16.0f) + 16 * static_cast<std::uint64_t>(v * 32.0f);
            const float shade = (1.0f + mottle * (2.0f * forestUnitRandom(seed, cell) - 1.0f)) * (1.0f - edgeDarkening * out * out) *
                                (1.0f - veinShade * vein) * (1.0f - midribDarkness * midrib);

            // The fern's texel on the left, the bromeliad's beside it on the right.
            const std::size_t left = (static_cast<std::size_t>(y) * width + x) * 4;
            const std::size_t right = (static_cast<std::size_t>(y) * width + size + x) * 4;
            const std::size_t palm = (static_cast<std::size_t>(y) * size + x) * 4;
            const std::uint8_t height = channelByte(255.0f * (0.5f + 0.3f * midrib - 0.08f * vein));

            for (int channel = 0; channel < 4; ++channel) {
                const std::size_t c = static_cast<std::size_t>(channel);
                images.colour.pixels[right + c] = bromeliad.colour.pixels[palm + c];
                images.height.pixels[right + c] = bromeliad.height.pixels[palm + c];
                images.height.pixels[left + c] = channel < 3 ? height : 255;
            }

            images.colour.pixels[left] = channelByte(static_cast<float>(fern.r) * shade);
            images.colour.pixels[left + 1] = channelByte(static_cast<float>(fern.g) * shade);
            images.colour.pixels[left + 2] = channelByte(static_cast<float>(fern.b) * shade);
            images.colour.pixels[left + 3] = 255;
        }
    }

    return images;
}

IndexedMesh limbMoss(const TreeSkeleton& skeleton, const ForestEpiphyteSettings& settings, float tileMetres) {
    IndexedMesh mesh;

    if (!settings.moss) {
        return mesh;
    }

    const float thick = limbThreshold(skeleton, settings.mossLimbShare, settings.leastLimbRadius);
    const float arc = radiansFromDegrees(settings.mossArcDegrees);

    for (const std::vector<int>& chain : skeleton.chains) {
        // Runs of segments thick and level enough; a run of two segments or more gets a shell.
        std::size_t runStart = 0;
        bool inRun = false;

        for (std::size_t point = 1; point <= chain.size(); ++point) {
            bool mossy = false;

            if (point < chain.size()) {
                const Vec3& from = skeleton.nodes[static_cast<std::size_t>(chain[point - 1])].position;
                const Vec3& to = skeleton.nodes[static_cast<std::size_t>(chain[point])].position;
                mossy = chainRadius(skeleton, chain, point) >= thick && levelEnough((to - from).normalized(), settings.mossSteepestDegrees);
            }

            if (mossy && !inRun) {
                runStart = point - 1;
                inRun = true;
            } else if (!mossy && inRun) {
                if (point - 1 - runStart >= 2) {
                    appendMossRun(mesh, skeleton, chain, runStart, point - 1, arc, tileMetres);
                }

                inRun = false;
            }
        }
    }

    return mesh;
}

EpiphyteMeshes growEpiphytes(const TreeSkeleton& skeleton, const ForestEpiphyteSettings& settings, bool bigTree, std::uint32_t seed) {
    EpiphyteMeshes meshes;
    meshes.sites = chooseEpiphyteSites(epiphyteSites(skeleton, settings), epiphytesOnTree(settings, bigTree, seed), settings.spacing,
                                       forestShapeSeed(seed, 1));

    for (std::size_t site = 0; site < meshes.sites.size(); ++site) {
        const std::uint32_t rosetteSeed = forestShapeSeed(seed, 100 + static_cast<std::uint32_t>(site));
        const bool fern = bigTree && forestUnitRandom(rosetteSeed, 0x46) < settings.fernShare;
        appendRosette(meshes.leaves, meshes.sites[site].base, rosetteShape(settings, fern ? EpiphyteKind::Fern : EpiphyteKind::Bromeliad, rosetteSeed));
        ++(fern ? meshes.ferns : meshes.bromeliads);
    }

    meshes.moss = limbMoss(skeleton, settings, forestLimbMossLayer.tileMetres);
    return meshes;
}
