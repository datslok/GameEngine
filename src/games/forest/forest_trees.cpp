#include "games/forest/forest_trees.h"

#include "games/forest/forest_terrain.h"
#include "games/forest/island_hangings.h"
#include "math/angles.h"
#include "math/closest_points.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {
    constexpr std::uint64_t treePlaceSeed = 0x5452'4545ull;
    constexpr int triesPerTree = 60;

    // The smallest and largest tree against its variant.
    constexpr float smallestTree = 0.85f;
    constexpr float treeSizeRange = 0.3f;

    // How far round the trunk the foot looks for the lowest ground (the buttresses' reach, at the variant's size).
    constexpr float footReachMetres = 2.5f;

    // About one side of a tube every this many metres round it, between the fewest and the most.
    constexpr float tubeSideMetres = 0.15f;
    constexpr int fewestTubeSides = 3;
    constexpr int mostTubeSides = 16;

    // Leaves: how far a card's stem may stray from its tip (a share of its size), how far its facing strays from
    // straight out of the crown, and which way it reaches (out, down and a random share).
    constexpr float leafStray = 0.3f;
    constexpr float leafFacingStray = 0.8f;
    constexpr float leafReachOut = 0.5f;
    constexpr float leafReachDown = 0.6f;
    constexpr float leafReachStray = 0.5f;

    // The sight lines' targets: a point this high on the Monolith's face, and the Ring's top. The viewpoints round the
    // meadow and the clearing: this many, at these shares of their radii.
    constexpr float monolithSightHeight = 250.0f;
    constexpr int viewpointsRound = 8;
    constexpr float meadowViewShare = 0.5f;
    constexpr float clearingViewShare = 0.6f;

    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 down{0.0f, -1.0f, 0.0f};

    // A random unit direction, the same for (seed, index), spread evenly over the sphere (Archimedes: a uniform height
    // on the axis gives a uniform area).
    Vec3 randomDirection(std::uint64_t seed, std::uint64_t index) {
        const float height = 2.0f * forestUnitRandom(seed, index) - 1.0f;
        const float turn = fullTurnRadians * forestUnitRandom(seed, index + 1);
        const float across = std::sqrt(std::max(1.0f - height * height, 0.0f));
        return Vec3{across * std::cos(turn), height, across * std::sin(turn)};
    }

    /*
    * The tree's nodes in a grid of cells as wide as the reach asked of it, over the crown's box grown by the reach and a
    * step: a nearest-node question looks only in the 27 cells round the point instead of at every node. Places outside
    * the box are held to its edge cells, which keeps two places within a cell's width of each other in neighbouring
    * cells, so the answers stay exact.
    */
    class NodeGrid {
    public:
        NodeGrid(const CrownEnvelope& crown, float cellMetres, float margin):
            cell(cellMetres),
            low(crown.centre - Vec3{crown.radius + margin, crown.halfHeight + margin, crown.radius + margin})
        {
            const Vec3 size = Vec3{crown.radius + margin, crown.halfHeight + margin, crown.radius + margin} * 2.0f;
            counts[0] = std::max(1, static_cast<int>(std::ceil(size.x / cell)));
            counts[1] = std::max(1, static_cast<int>(std::ceil(size.y / cell)));
            counts[2] = std::max(1, static_cast<int>(std::ceil(size.z / cell)));
            cells.resize(static_cast<std::size_t>(counts[0] * counts[1] * counts[2]));
        }

        void insert(const std::vector<TreeNode>& nodes, int node) {
            const std::array<int, 3> at = cellOf(nodes[static_cast<std::size_t>(node)].position);
            cells[index(at[0], at[1], at[2])].push_back(node);
        }

        // The nearest node closer than reach (at most a cell), the lowest index on a tie; -1 for none.
        int nearest(const std::vector<TreeNode>& nodes, const Vec3& point, float reach, float& distance) const {
            const std::array<int, 3> at = cellOf(point);
            int best = -1;
            float bestSquared = reach * reach;

            for (int x = std::max(at[0] - 1, 0); x <= std::min(at[0] + 1, counts[0] - 1); ++x) {
                for (int y = std::max(at[1] - 1, 0); y <= std::min(at[1] + 1, counts[1] - 1); ++y) {
                    for (int z = std::max(at[2] - 1, 0); z <= std::min(at[2] + 1, counts[2] - 1); ++z) {
                        for (const int node : cells[index(x, y, z)]) {
                            const float squared = (nodes[static_cast<std::size_t>(node)].position - point).lengthSquared();

                            if (squared < bestSquared || (squared == bestSquared && best >= 0 && node < best)) {
                                best = node;
                                bestSquared = squared;
                            }
                        }
                    }
                }
            }

            distance = std::sqrt(bestSquared);
            return best;
        }

    private:
        std::array<int, 3> cellOf(const Vec3& point) const {
            const Vec3 local = point - low;
            return {std::clamp(static_cast<int>(std::floor(local.x / cell)), 0, counts[0] - 1),
                    std::clamp(static_cast<int>(std::floor(local.y / cell)), 0, counts[1] - 1),
                    std::clamp(static_cast<int>(std::floor(local.z / cell)), 0, counts[2] - 1)};
        }

        std::size_t index(int x, int y, int z) const {
            return static_cast<std::size_t>((y * counts[2] + z) * counts[0] + x);
        }

        float cell;
        Vec3 low;
        std::array<int, 3> counts{};
        std::vector<std::vector<int>> cells;
    };

    // The lowest ground under a trunk: at its foot and round it at reach metres.
    float lowestGroundRound(float x, float z, float reach) {
        float lowest = forestHeight(x, z);

        for (int k = 0; k < viewpointsRound; ++k) {
            const float turn = fullTurnRadians * static_cast<float>(k) / static_cast<float>(viewpointsRound);
            lowest = std::min(lowest, forestHeight(x + reach * std::cos(turn), z + reach * std::sin(turn)));
        }

        return lowest;
    }

    Vec3 eyeAt(float x, float z) {
        return Vec3{x, forestHeight(x, z) + sightEyeHeight, z};
    }
}

bool insideCrown(const CrownEnvelope& crown, const Vec3& point) {
    const Vec3 offset = point - crown.centre;
    const float across = (offset.x * offset.x + offset.z * offset.z) / (crown.radius * crown.radius);
    return across + offset.y * offset.y / (crown.halfHeight * crown.halfHeight) <= 1.0f + 1e-5f;
}

Vec3 crownOutward(const CrownEnvelope& crown, const Vec3& point) {
    const Vec3 offset = point - crown.centre;
    const float across = 1.0f / (crown.radius * crown.radius);
    const Vec3 gradient{offset.x * across, offset.y / (crown.halfHeight * crown.halfHeight), offset.z * across};
    return gradient.lengthSquared() > 1e-12f ? gradient.normalized() : up;
}

std::vector<Vec3> crownAttractors(const CrownEnvelope& crown, int count, std::uint32_t seed) {
    std::vector<Vec3> points;
    points.reserve(static_cast<std::size_t>(std::max(count, 0)));

    // An ellipsoid fills pi / 6 of its box, so about half the tries land; each try uses three numbers.
    for (std::uint64_t attempt = 0; static_cast<int>(points.size()) < count; ++attempt) {
        const Vec3 point = crown.centre + Vec3{crown.radius * (2.0f * forestUnitRandom(seed, 3 * attempt) - 1.0f),
                                               crown.halfHeight * (2.0f * forestUnitRandom(seed, 3 * attempt + 1) - 1.0f),
                                               crown.radius * (2.0f * forestUnitRandom(seed, 3 * attempt + 2) - 1.0f)};

        if (insideCrown(crown, point)) {
            points.push_back(point);
        }
    }

    return points;
}

/*
* Runions et al.'s order: each round every live attractor finds its nearest node; one already within the kill distance
* is used up, one within the influence distance pulls that node by its unit direction. Every pulled node then grows a
* child one step along the sum of its pulls. Attractors and nodes are walked in index order and children appended in
* their parents' order, so the tree is the same on every run.
*/
std::vector<TreeNode> growTree(const Vec3& base, const CrownEnvelope& crown, const std::vector<Vec3>& attractors, const TreeGrowth& growth) {
    std::vector<TreeNode> nodes{TreeNode{base, -1}};

    // The straight trunk up to the crown's bottom, in equal steps of at most a step.
    const float rise = crown.centre.y - crown.halfHeight - base.y;

    if (rise > 0.0f) {
        const int steps = static_cast<int>(std::ceil(rise / growth.step));

        for (int step = 1; step <= steps; ++step) {
            nodes.push_back(TreeNode{base + up * (rise * static_cast<float>(step) / static_cast<float>(steps)), step - 1});
        }
    }

    NodeGrid grid(crown, growth.influence, growth.influence + growth.step);

    for (int node = 0; node < static_cast<int>(nodes.size()); ++node) {
        grid.insert(nodes, node);
    }

    std::vector<bool> alive(attractors.size(), true);
    std::vector<Vec3> pulls;
    std::vector<int> pulled;
    const float tooClose = 0.1f * growth.step;

    for (int round = 0; round < growth.maxRounds; ++round) {
        pulls.assign(nodes.size(), Vec3{0.0f, 0.0f, 0.0f});
        pulled.assign(nodes.size(), 0);
        bool anyPull = false;

        for (std::size_t attractor = 0; attractor < attractors.size(); ++attractor) {
            if (!alive[attractor]) {
                continue;
            }

            float distance = 0.0f;
            const int node = grid.nearest(nodes, attractors[attractor], growth.influence, distance);

            if (node < 0) {
                continue;
            }

            if (distance < growth.kill) {
                alive[attractor] = false;
                continue;
            }

            pulls[static_cast<std::size_t>(node)] = pulls[static_cast<std::size_t>(node)] + (attractors[attractor] - nodes[static_cast<std::size_t>(node)].position) / distance;
            ++pulled[static_cast<std::size_t>(node)];
            anyPull = true;
        }

        if (!anyPull) {
            break;
        }

        bool grew = false;
        const std::size_t existing = nodes.size();

        for (std::size_t node = 0; node < existing; ++node) {
            if (pulled[node] == 0 || pulls[node].lengthSquared() < 1e-12f) {
                continue;
            }

            const Vec3 child = nodes[node].position + pulls[node].normalized() * growth.step;
            float distance = 0.0f;

            if (grid.nearest(nodes, child, tooClose, distance) >= 0) {
                continue;
            }

            nodes.push_back(TreeNode{child, static_cast<int>(node)});
            grid.insert(nodes, static_cast<int>(nodes.size() - 1));
            grew = true;
        }

        if (!grew) {
            break;
        }
    }

    return nodes;
}

std::vector<bool> treeTips(const std::vector<TreeNode>& nodes) {
    std::vector<bool> tips(nodes.size(), true);

    for (const TreeNode& node : nodes) {
        if (node.parent >= 0) {
            tips[static_cast<std::size_t>(node.parent)] = false;
        }
    }

    return tips;
}

// Children come after their parents, so walking backwards finishes every child before its parent is reached.
std::vector<float> leonardoRadii(const std::vector<TreeNode>& nodes, float tipRadius, float exponent) {
    std::vector<double> carried(nodes.size(), 0.0);
    std::vector<float> radii(nodes.size(), tipRadius);

    for (std::size_t i = nodes.size(); i-- > 0;) {
        if (carried[i] > 0.0) {
            radii[i] = static_cast<float>(std::pow(carried[i], 1.0 / static_cast<double>(exponent)));
        }

        if (nodes[i].parent >= 0) {
            carried[static_cast<std::size_t>(nodes[i].parent)] += std::pow(static_cast<double>(radii[i]), static_cast<double>(exponent));
        }
    }

    return radii;
}

std::vector<std::vector<int>> branchChains(const std::vector<TreeNode>& nodes, const std::vector<float>& radii) {
    std::vector<std::vector<int>> children(nodes.size());

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].parent >= 0) {
            children[static_cast<std::size_t>(nodes[i].parent)].push_back(static_cast<int>(i));
        }
    }

    std::vector<std::vector<int>> chains;

    if (nodes.empty()) {
        return chains;
    }

    // Chains still to draw, each as its first two nodes (the parent and the child it leaves by), in the order found.
    std::vector<std::pair<int, int>> waiting{{-1, 0}};

    for (std::size_t next = 0; next < waiting.size(); ++next) {
        std::vector<int> chain;

        if (waiting[next].first >= 0) {
            chain.push_back(waiting[next].first);
        }

        int current = waiting[next].second;
        chain.push_back(current);

        while (!children[static_cast<std::size_t>(current)].empty()) {
            const std::vector<int>& from = children[static_cast<std::size_t>(current)];
            int thickest = from.front();

            for (const int child : from) {
                if (radii[static_cast<std::size_t>(child)] > radii[static_cast<std::size_t>(thickest)]) {
                    thickest = child;
                }
            }

            for (const int child : from) {
                if (child != thickest) {
                    waiting.emplace_back(current, child);
                }
            }

            chain.push_back(thickest);
            current = thickest;
        }

        chains.push_back(std::move(chain));
    }

    return chains;
}

int tubeSides(float radius) {
    const int sides = static_cast<int>(std::ceil(fullTurnRadians * radius / tubeSideMetres));
    return std::clamp(sides, fewestTubeSides, mostTubeSides);
}

void appendTreeLeaves(IndexedMesh& mesh, const std::vector<TreeNode>& nodes, const CrownEnvelope& crown, int leavesPerTip, float size,
                      std::uint32_t seed) {
    if (leavesPerTip <= 0 || size <= 0.0f) {
        return;
    }

    const std::vector<bool> tips = treeTips(nodes);
    constexpr int cells = treeLeafColumns * treeLeafRows;

    for (std::size_t node = 0; node < nodes.size(); ++node) {
        if (!tips[node]) {
            continue;
        }

        for (int leaf = 0; leaf < leavesPerTip; ++leaf) {
            const std::uint64_t base = (static_cast<std::uint64_t>(node) * static_cast<std::uint64_t>(leavesPerTip) + static_cast<std::uint64_t>(leaf)) * 8;
            const Vec3 stem = nodes[node].position + randomDirection(seed, base) * (leafStray * size * forestUnitRandom(seed, base + 2));
            const Vec3 out = crownOutward(crown, stem);

            // Its front out of the crown: out plus a shorter random direction never turns past square to out.
            const Vec3 facing = (out + randomDirection(seed, base + 3) * leafFacingStray).normalized();
            const Vec3 reach = out * leafReachOut + down * leafReachDown + randomDirection(seed, base + 5) * leafReachStray;
            const Vec3 inPlane = reach - facing * reach.dot(facing);
            const Vec3 towardsTip = inPlane.lengthSquared() > 1e-8f ? inPlane.normalized() : squareTo(facing);
            const Vec3 normal = (facing * (1.0f - crownNormalBend) + out * crownNormalBend).normalized();

            const int cell = std::min(static_cast<int>(forestUnitRandom(seed, base + 7) * static_cast<float>(cells)), cells - 1);
            const AtlasCell picture = atlasCell(cell, treeLeafColumns, treeLeafRows);
            appendLeafCard(mesh, stem, towardsTip, facing, normal, size, size, picture.uvMin, picture.uvMax);
        }
    }
}

void visit(ForestTreeSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Trees", settings.count, {.min = 0, .max = 500, .sliderMax = 50, .applies = FieldApplies::AtStartup});
    visitor.field("Variants", settings.variants, {.min = 1, .max = 32, .sliderMax = 8, .applies = FieldApplies::AtStartup});
    visitor.field("Crown height", settings.crownHeight, {.min = 2.0f, .max = 100.0f, .sliderMax = 60.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Crown radius", settings.crownRadius, {.min = 1.0f, .max = 50.0f, .sliderMax = 30.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Crown half depth", settings.crownHalfHeight, {.min = 0.5f, .max = 30.0f, .sliderMax = 15.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Attractors", settings.attractors, {.min = 10, .max = 50000, .sliderMax = 10000, .applies = FieldApplies::AtStartup});
    visitor.field("Twig radius", settings.twigRadius, {.min = 0.002f, .max = 0.2f, .sliderMax = 0.05f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Buttresses", settings.buttress, {.min = 0.0f, .max = 5.0f, .sliderMax = 3.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Leaf size", settings.leafSize, {.min = 0.05f, .max = 5.0f, .sliderMax = 2.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Leaves per tip", settings.leavesPerTip, {.min = 0, .max = 20, .sliderMax = 8, .applies = FieldApplies::AtStartup});
}

CrownEnvelope treeCrown(const ForestTreeSettings& settings) {
    return CrownEnvelope{.centre = Vec3{0.0f, settings.crownHeight, 0.0f}, .radius = settings.crownRadius, .halfHeight = settings.crownHalfHeight};
}

/*
* Each chain one tube: the trunk's lobed into buttresses with sides enough for their thin fins, every other as many
* sides as its thickest (first) radius asks for. A side chain's first point is its parent's node at the chain's own
* radius, so the branch starts inside the limb it leaves.
*/
TreeMeshes growTreeMeshes(const ForestTreeSettings& settings, std::uint32_t seed, float barkTileMetres) {
    const CrownEnvelope crown = treeCrown(settings);
    const std::vector<TreeNode> nodes =
        growTree(Vec3{0.0f, -treeSinkMetres, 0.0f}, crown, crownAttractors(crown, settings.attractors, forestShapeSeed(seed, 1)), TreeGrowth{});
    const std::vector<float> radii = leonardoRadii(nodes, settings.twigRadius, leonardoExponent);
    const std::vector<bool> tips = treeTips(nodes);
    const std::vector<std::vector<int>> chains = branchChains(nodes, radii);

    TreeMeshes meshes;
    meshes.tipCount = static_cast<int>(std::count(tips.begin(), tips.end(), true));
    meshes.trunkRadius = radii.front();

    for (std::size_t index = 0; index < chains.size(); ++index) {
        const std::vector<int>& chain = chains[index];

        if (chain.size() < 2) {
            continue;
        }

        HangingStrand strand;

        for (std::size_t k = 0; k < chain.size(); ++k) {
            const std::size_t node = static_cast<std::size_t>(chain[k]);
            strand.points.push_back(nodes[node].position);
            // A side chain's first point (its parent's node) takes the chain's own radius.
            strand.radii.push_back(index > 0 && k == 0 ? radii[static_cast<std::size_t>(chain[1])] : radii[node]);
        }

        TubeOptions options{.wholeTilesRound = true, .lobes = {}};
        int sides = tubeSides(strand.radii.front());

        if (index == 0 && settings.buttress > 0.0f) {
            options.lobes = TubeLobes{.count = buttressCount, .amplitude = settings.buttress, .heightMetres = buttressHeightMetres,
                                      .sharpness = buttressSharpness, .groundHeight = 0.0f, .seed = forestShapeSeed(seed, 2)};
            sides = lobedTrunkSides;
        }

        appendTube(meshes.bark, strand, sides, barkTileMetres, options);
    }

    appendTreeLeaves(meshes.leaves, nodes, crown, settings.leavesPerTip, settings.leafSize, forestShapeSeed(seed, 3));
    return meshes;
}

std::vector<SightLine> forestSightLines() {
    std::vector<Vec3> viewpoints;
    const std::vector<Vec2>& path = forestPathPoints();

    // Along the path every spacing metres of its length, and its end.
    float sinceLast = 0.0f;
    viewpoints.push_back(eyeAt(path.front().x, path.front().y));

    for (std::size_t i = 1; i < path.size(); ++i) {
        const Vec2 from = path[i - 1];
        const float length = (path[i] - from).length();
        float along = sightLineSpacing - sinceLast;

        while (along <= length) {
            const Vec2 at = from + (path[i] - from) * (along / length);
            viewpoints.push_back(eyeAt(at.x, at.y));
            along += sightLineSpacing;
        }

        sinceLast = length - (along - sightLineSpacing);
    }

    viewpoints.push_back(eyeAt(path.back().x, path.back().y));

    // Round the meadow and the clearing.
    viewpoints.push_back(eyeAt(forestClearingCentre.x, forestClearingCentre.y));

    for (int k = 0; k < viewpointsRound; ++k) {
        const float turn = fullTurnRadians * static_cast<float>(k) / static_cast<float>(viewpointsRound);
        const Vec2 way{std::cos(turn), std::sin(turn)};
        const Vec2 meadow = forestStartPlace + way * (meadowViewShare * forestMeadowRadius);
        const Vec2 clearing = forestClearingCentre + way * (clearingViewShare * forestClearingRadius);
        viewpoints.push_back(eyeAt(meadow.x, meadow.y));
        viewpoints.push_back(eyeAt(clearing.x, clearing.y));
    }

    const float clearingLevel = forestHeight(forestClearingCentre.x, forestClearingCentre.y);
    const Vec3 monolith{forestMonolithPlace.x, clearingLevel + monolithSightHeight, forestMonolithPlace.y};
    const Vec3 ringTop{forestRingPlace.x, forestHeight(forestRingPlace.x, forestRingPlace.y) + forestRingRadius, forestRingPlace.y};

    std::vector<SightLine> lines;
    lines.reserve(2 * viewpoints.size());

    for (const Vec3& viewpoint : viewpoints) {
        lines.push_back(SightLine{viewpoint, monolith});
        lines.push_back(SightLine{viewpoint, ringTop});
    }

    return lines;
}

bool blocksSight(const Vec3& centre, float radius, const std::vector<SightLine>& lines) {
    return std::any_of(lines.begin(), lines.end(), [&](const SightLine& line) {
        const Vec3 span = line.to - line.from;
        const float length = span.length();
        return length > 0.0f ? segmentMeetsSphere(line.from, span / length, length, centre, radius)
                             : (line.from - centre).lengthSquared() <= radius * radius;
    });
}

Vec3 placedCrownCentre(const TreePlacement& tree, const ForestTreeSettings& settings) {
    return tree.foot + up * (settings.crownHeight * tree.scale);
}

float placedCrownRadius(const TreePlacement& tree, const ForestTreeSettings& settings) {
    return std::max(settings.crownRadius, settings.crownHalfHeight) * tree.scale;
}

std::vector<TreePlacement> placeTrees(const ForestTreeSettings& settings, const std::vector<PillarPlacement>& pillars,
                                      const std::vector<SightLine>& sightLines) {
    std::vector<TreePlacement> trees;
    const int variants = std::max(settings.variants, 1);
    const int tries = std::max(settings.count, 0) * triesPerTree;

    for (int attempt = 0; attempt < tries && static_cast<int>(trees.size()) < settings.count; ++attempt) {
        const std::uint64_t base = static_cast<std::uint64_t>(attempt) * 8u;
        const float z = treeNorthmost + (treeSouthmost - treeNorthmost) * forestUnitRandom(treePlaceSeed, base);
        const float x = valleyAxis(z) + treeSpread * (2.0f * forestUnitRandom(treePlaceSeed, base + 1) - 1.0f);

        if (forestPathDistance(x, z) < treePathGap) {
            continue;
        }

        TreePlacement tree{
            .foot = Vec3{x, 0.0f, z},
            .scale = smallestTree + treeSizeRange * forestUnitRandom(treePlaceSeed, base + 2),
            .turnRadians = fullTurnRadians * forestUnitRandom(treePlaceSeed, base + 3),
            .variant = std::min(static_cast<int>(forestUnitRandom(treePlaceSeed, base + 4) * static_cast<float>(variants)), variants - 1)
        };
        const float radius = placedCrownRadius(tree, settings);

        // Each trunk outside the other's crown's middle, and every crown clear of the pillars (which bulge and lean).
        const bool crowded = std::any_of(trees.begin(), trees.end(), [&](const TreePlacement& other) {
            return (Vec2{x, z} - Vec2{other.foot.x, other.foot.z}).length() < std::max(radius, placedCrownRadius(other, settings));
        });
        const bool byPillar = std::any_of(pillars.begin(), pillars.end(), [&](const PillarPlacement& pillar) {
            return (Vec2{x, z} - Vec2{pillar.foot.x, pillar.foot.z}).length() < 1.3f * pillar.radius + radius;
        });

        if (crowded || byPillar) {
            continue;
        }

        tree.foot.y = lowestGroundRound(x, z, footReachMetres * tree.scale);

        if (blocksSight(placedCrownCentre(tree, settings), radius, sightLines)) {
            continue;
        }

        trees.push_back(tree);
    }

    return trees;
}
