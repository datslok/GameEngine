#include "games/forest/forest_trees.h"

#include "games/forest/forest_terrain.h"
#include "games/forest/island_hangings.h"
#include "games/forest/leaf_clusters.h"
#include "math/angles.h"
#include "math/closest_points.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace {
    constexpr std::uint64_t treePlaceSeed = 0x5452'4545ull;
    constexpr int triesPerTree = 60;

    // Clumps: 75-125% of the setting's radius, 55-100% of the way out, and how many tries each gets.
    constexpr float smallestClump = 0.75f;
    constexpr float clumpSizeRange = 0.5f;
    constexpr float innermostClump = 0.55f;
    constexpr int triesPerClump = 40;

    // Cluster cards: how far a card's facing strays from out of its clump, how its twig reaches (out, up and a random
    // share), and the fillers' reach into their clump (a share of its radius) and size (against a shell card's).
    constexpr float cardFacingStray = 0.8f;
    constexpr float cardReachUp = 0.4f;
    constexpr float cardReachStray = 0.5f;
    constexpr float fillerReach = 0.4f;
    constexpr float fillerSize = 1.5f;

    // The cone under a clump: its downward part kept at this share.
    constexpr float coneFlattening = 0.5f;

    // The least a foliage normal leans to its card's front (the cosine between them, before making it unit again).
    constexpr float leastFrontShare = 0.1f;

    // The smallest and largest tree against its variant.
    constexpr float smallestTree = 0.85f;
    constexpr float treeSizeRange = 0.3f;

    // How far round the trunk the foot looks for the lowest ground (the buttresses' reach, at the variant's size).
    constexpr float footReachMetres = 2.5f;

    // About one side of a tube every this many metres round it, between the fewest and the most.
    constexpr float tubeSideMetres = 0.15f;
    constexpr int fewestTubeSides = 3;
    constexpr int mostTubeSides = 16;

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

int appendTrunk(std::vector<TreeNode>& nodes, int parent, const Vec3& base, float rise, const Vec3& lean, float step) {
    if (rise <= 0.0f) {
        return parent;
    }

    // The curve's speed, |(rise, 2 lean s)|, is at most |(rise, 2 lean)|, so that over the steps keeps every step within
    // a step.
    const float longest = std::sqrt(rise * rise + 4.0f * lean.lengthSquared());
    const int steps = static_cast<int>(std::ceil(longest / step));
    int last = parent;

    for (int index = 1; index <= steps; ++index) {
        const float share = static_cast<float>(index) / static_cast<float>(steps);
        nodes.push_back(TreeNode{base + up * (rise * share) + lean * (share * share), last});
        last = static_cast<int>(nodes.size() - 1);
    }

    return last;
}

std::vector<TreeNode> growTree(const Vec3& base, const CrownEnvelope& crown, const std::vector<Vec3>& attractors, const TreeGrowth& growth,
                               const Vec3& lean) {
    std::vector<TreeNode> nodes{TreeNode{base, -1}};
    appendTrunk(nodes, 0, base, crown.centre.y - crown.halfHeight - base.y, lean, growth.step);
    return coloniseTree(std::move(nodes), crown, attractors, growth);
}

/*
* Runions et al.'s order: each round every live attractor finds its nearest node; one already within the kill distance
* is used up, one within the influence distance pulls that node by its unit direction. Every pulled node then grows a
* child one step along the sum of its pulls (bent down by gravity). Attractors and nodes are walked in index order and
* children appended in their parents' order, so the tree is the same on every run.
*/
std::vector<TreeNode> coloniseTree(std::vector<TreeNode> nodes, const CrownEnvelope& crown, const std::vector<Vec3>& attractors,
                                   const TreeGrowth& growth) {
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

            Vec3 direction = pulls[node].normalized();

            if (growth.gravity != 0.0f) {
                direction = (direction + down * growth.gravity).normalized();
            }

            const Vec3 child = nodes[node].position + direction * growth.step;
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

/*
* Each clump's centre is a random direction pushed 55-100% of the way out of the envelope shrunk by the clump's own
* radius, so the clumps sit in the crown's outer part, where the light is. Sorted by their angle round the trunk and
* cut into equal runs, one run a limb, every limb gets clumps on its own side.
*/
std::vector<TreeClump> treeClumps(const CrownEnvelope& crown, int count, float radius, int limbs, std::uint32_t seed) {
    std::vector<TreeClump> clumps;
    const int tries = std::max(count, 0) * triesPerClump;

    for (int attempt = 0; attempt < tries && static_cast<int>(clumps.size()) < count; ++attempt) {
        const std::uint64_t base = static_cast<std::uint64_t>(attempt) * 8u;
        const float size = radius * (smallestClump + clumpSizeRange * forestUnitRandom(seed, base));
        const float out = innermostClump + (1.0f - innermostClump) * forestUnitRandom(seed, base + 1);
        const Vec3 direction = randomDirection(seed, base + 2);
        const float across = std::max(crown.radius - size, 0.0f);
        const float upDown = std::max(crown.halfHeight - size, 0.0f);
        const Vec3 centre = crown.centre + Vec3{direction.x * across, direction.y * upDown, direction.z * across} * out;

        const bool crowded = std::any_of(clumps.begin(), clumps.end(), [&](const TreeClump& other) {
            return (centre - other.centre).length() < clumpSpacing * (size + other.radius);
        });

        if (!crowded) {
            clumps.push_back(TreeClump{centre, size, 0});
        }
    }

    std::vector<std::size_t> byAngle(clumps.size());

    for (std::size_t i = 0; i < byAngle.size(); ++i) {
        byAngle[i] = i;
    }

    const auto angle = [&](std::size_t i) {
        return std::atan2(clumps[i].centre.z - crown.centre.z, clumps[i].centre.x - crown.centre.x);
    };
    std::stable_sort(byAngle.begin(), byAngle.end(), [&](std::size_t a, std::size_t b) { return angle(a) < angle(b); });
    const std::size_t limbCount = static_cast<std::size_t>(std::clamp(limbs, 1, std::max(static_cast<int>(clumps.size()), 1)));

    for (std::size_t k = 0; k < byAngle.size(); ++k) {
        clumps[byAngle[k]].limb = static_cast<int>(k * limbCount / byAngle.size());
    }

    return clumps;
}

std::vector<Vec3> clumpAttractors(const std::vector<TreeClump>& clumps, int count, std::uint32_t seed) {
    std::vector<Vec3> points;

    if (clumps.empty() || count <= 0) {
        return points;
    }

    double volume = 0.0;

    for (const TreeClump& clump : clumps) {
        volume += std::pow(static_cast<double>(clump.radius), 3.0);
    }

    std::vector<int> shares(clumps.size());
    int given = 0;

    for (std::size_t i = 0; i < clumps.size(); ++i) {
        shares[i] = static_cast<int>(static_cast<double>(count) * std::pow(static_cast<double>(clumps[i].radius), 3.0) / volume);
        given += shares[i];
    }

    for (std::size_t i = 0; given < count; i = (i + 1) % clumps.size()) {
        ++shares[i];
        ++given;
    }

    points.reserve(static_cast<std::size_t>(count));
    std::uint64_t attempt = 0;

    for (std::size_t i = 0; i < clumps.size(); ++i) {
        const TreeClump& clump = clumps[i];

        for (int kept = 0; kept < shares[i]; ++attempt) {
            const Vec3 offset{2.0f * forestUnitRandom(seed, 3 * attempt) - 1.0f, 2.0f * forestUnitRandom(seed, 3 * attempt + 1) - 1.0f,
                              2.0f * forestUnitRandom(seed, 3 * attempt + 2) - 1.0f};

            if (offset.lengthSquared() <= 1.0f) {
                points.push_back(clump.centre + offset * clump.radius);
                ++kept;
            }
        }
    }

    return points;
}

/*
* Each limb steers for its target, its direction the unit way there plus a sideways wander from smooth noise along its
* length (each limb its own), so it kinks as real limbs do rather than running straight. A limb that has not arrived in
* three times its straight distance stops anyway.
*/
std::vector<TreeNode> growScaffold(const Vec3& base, const Vec3& fork, const std::vector<Vec3>& targets, float stopMetres, float step,
                                   std::uint32_t seed) {
    std::vector<TreeNode> nodes{TreeNode{base, -1}};
    const Vec3 lean{fork.x - base.x, 0.0f, fork.z - base.z};
    const int forkNode = appendTrunk(nodes, 0, base, fork.y - base.y, lean, step);

    for (std::size_t limb = 0; limb < targets.size(); ++limb) {
        const Vec3& target = targets[limb];
        const std::uint32_t limbSeed = forestShapeSeed(seed, static_cast<std::uint32_t>(limb) + 1);
        const int most = static_cast<int>(std::ceil(3.0f * (target - fork).length() / step)) + 1;
        int last = forkNode;
        float along = 0.0f;

        for (int index = 0; index < most; ++index) {
            const Vec3 at = nodes[static_cast<std::size_t>(last)].position;
            const Vec3 toTarget = target - at;
            const float distance = toTarget.length();

            if (distance <= stopMetres) {
                break;
            }

            const Vec3 way = toTarget / distance;
            const Vec3 across = squareTo(way);
            const Vec3 second = way.cross(across);
            const float wanderAt = along / limbWanderMetres;
            const Vec3 wander = across * (2.0f * forestValueNoise(wanderAt, 0.5f, forestShapeSeed(limbSeed, 1)) - 1.0f) +
                                second * (2.0f * forestValueNoise(wanderAt, 0.5f, forestShapeSeed(limbSeed, 2)) - 1.0f);
            const Vec3 direction = (way + wander * limbWander).normalized();
            nodes.push_back(TreeNode{at + direction * step, last});
            last = static_cast<int>(nodes.size() - 1);
            along += step;
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

std::vector<bool> leafyNodes(const std::vector<float>& radii, float leafyRadius) {
    std::vector<bool> leafy(radii.size(), false);

    for (std::size_t node = 1; node < radii.size(); ++node) {
        leafy[node] = radii[node] <= leafyRadius;
    }

    return leafy;
}

int clumpOf(const Vec3& point, const std::vector<TreeClump>& clumps) {
    int best = -1;
    float deepest = std::numeric_limits<float>::max();

    for (std::size_t clump = 0; clump < clumps.size(); ++clump) {
        const float depth = (point - clumps[clump].centre).length() / clumps[clump].radius;

        if (depth < deepest) {
            deepest = depth;
            best = static_cast<int>(clump);
        }
    }

    return best;
}

Vec3 coneFlattened(const Vec3& direction) {
    if (direction.y >= 0.0f || std::fabs(direction.y) > 0.999999f) {
        return direction;
    }

    return Vec3{direction.x, direction.y * coneFlattening, direction.z}.normalized();
}

// Never past square to the card's own front: a normal behind the card would light it from the wrong side.
Vec3 foliageNormal(const Vec3& facing, const Vec3& clumpOut, const Vec3& crownOut) {
    const Vec3 blend = (facing * cardNormalShare + coneFlattened(clumpOut) * clumpNormalShare + coneFlattened(crownOut) * crownNormalShare).normalized();
    const float front = blend.dot(facing);
    return front >= leastFrontShare ? blend : (blend + facing * (leastFrontShare - front)).normalized();
}

/*
* A card: its stem at a node, its front a random turn from the way out of its clump (out plus a shorter random
* direction, never past square), the twig reaching out of the clump and a little up within the card's plane.
*/
int appendClusterCards(IndexedMesh& mesh, const std::vector<TreeNode>& nodes, const std::vector<float>& radii, const CrownEnvelope& crown,
                       const std::vector<TreeClump>& clumps, const ForestTreeSettings& settings, std::uint32_t seed) {
    if (clumps.empty()) {
        return 0;
    }

    const std::vector<bool> leafy = leafyNodes(radii, settings.leafyRadius);
    constexpr int cells = clusterColumns * clusterRows;
    int made = 0;

    const auto addCard = [&](const Vec3& stem, const Vec3& facing, const Vec3& reach, const Vec3& normal, float size, std::uint64_t base) {
        const Vec3 inPlane = reach - facing * reach.dot(facing);
        const Vec3 towardsTip = inPlane.lengthSquared() > 1e-8f ? inPlane.normalized() : squareTo(facing);
        const int cell = std::min(static_cast<int>(forestUnitRandom(seed, base) * static_cast<float>(cells)), cells - 1);
        const AtlasCell picture = atlasCell(cell, clusterColumns, clusterRows);
        appendLeafCard(mesh, stem, towardsTip, facing, normal, size, size, picture.uvMin, picture.uvMax);
        ++made;
    };

    for (std::size_t node = 0; node < nodes.size(); ++node) {
        const int clump = clumpOf(nodes[node].position, clumps);

        if (!leafy[node] || clump < 0) {
            continue;
        }

        const TreeClump& home = clumps[static_cast<std::size_t>(clump)];
        const Vec3 fromCentre = nodes[node].position - home.centre;

        if (fromCentre.length() < settings.shellShare * home.radius) {
            continue;
        }

        const Vec3 clumpOut = fromCentre.normalized();
        const Vec3 crownOut = crownOutward(crown, nodes[node].position);

        for (int card = 0; card < settings.cardsPerNode; ++card) {
            const std::uint64_t base = (static_cast<std::uint64_t>(node) * static_cast<std::uint64_t>(std::max(settings.cardsPerNode, 1)) +
                                        static_cast<std::uint64_t>(card)) * 8;
            const Vec3 facing = (clumpOut + randomDirection(seed, base) * cardFacingStray).normalized();
            const Vec3 reach = clumpOut + up * cardReachUp + randomDirection(seed, base + 2) * cardReachStray;
            addCard(nodes[node].position, facing, reach, foliageNormal(facing, clumpOut, crownOut), settings.clusterSize, base + 4);
        }
    }

    // The fillers: inside each clump near its middle, facing every way, shading with the crown's way out there.
    for (std::size_t clump = 0; clump < clumps.size(); ++clump) {
        for (int card = 0; card < settings.fillCards; ++card) {
            const std::uint64_t base = (1ull << 40) + (static_cast<std::uint64_t>(clump) * 64 + static_cast<std::uint64_t>(card)) * 8;
            const TreeClump& home = clumps[clump];
            const Vec3 stem = home.centre + randomDirection(seed, base) * (fillerReach * home.radius * forestUnitRandom(seed, base + 2));
            // Any way round, but its front turned to the crown's outside, which its normals lean towards.
            const Vec3 crownOut = crownOutward(crown, stem);
            const Vec3 turned = randomDirection(seed, base + 3);
            const Vec3 facing = turned.dot(crownOut) >= 0.0f ? turned : turned * -1.0f;
            addCard(stem, facing, randomDirection(seed, base + 5), foliageNormal(facing, crownOut, crownOut), fillerSize * settings.clusterSize, base + 7);
        }
    }

    return made;
}

void visit(ForestTreeSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Trees", settings.count, {.min = 0, .max = 500, .sliderMax = 50, .applies = FieldApplies::AtStartup});
    visitor.field("Variants", settings.variants, {.min = 1, .max = 32, .sliderMax = 8, .applies = FieldApplies::AtStartup});
    visitor.field("Height", settings.height, {.min = 2.0f, .max = 120.0f, .sliderMax = 60.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Fork height", settings.forkHeight, {.min = 0.5f, .max = 100.0f, .sliderMax = 30.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Crown radius", settings.crownRadius, {.min = 1.0f, .max = 50.0f, .sliderMax = 30.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Limbs", settings.limbs, {.min = 1, .max = 12, .sliderMax = 8, .applies = FieldApplies::AtStartup});
    visitor.field("Clumps", settings.clumps, {.min = 1, .max = 60, .sliderMax = 20, .applies = FieldApplies::AtStartup});
    visitor.field("Clump radius", settings.clumpRadius, {.min = 0.3f, .max = 20.0f, .sliderMax = 8.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Attractors", settings.attractors, {.min = 10, .max = 50000, .sliderMax = 10000, .applies = FieldApplies::AtStartup});
    visitor.field("Twig radius", settings.twigRadius, {.min = 0.002f, .max = 0.2f, .sliderMax = 0.05f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Buttresses", settings.buttress, {.min = 0.0f, .max = 5.0f, .sliderMax = 3.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Lean", settings.lean, {.min = 0.0f, .max = 20.0f, .sliderMax = 8.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Cluster size", settings.clusterSize, {.min = 0.1f, .max = 10.0f, .sliderMax = 4.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Leaf length", settings.leafLength, {.min = 0.02f, .max = 2.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Leafy radius", settings.leafyRadius, {.min = 0.0f, .max = 1.0f, .sliderMax = 0.15f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Cards per node", settings.cardsPerNode, {.min = 0, .max = 20, .sliderMax = 6, .applies = FieldApplies::AtStartup});
    visitor.field("Shell share", settings.shellShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Filler cards", settings.fillCards, {.min = 0, .max = 50, .sliderMax = 10, .applies = FieldApplies::AtStartup});
    visitor.field("Cluster texels", settings.clusterTexels, {.min = 16, .max = 4096, .sliderMax = 2048, .applies = FieldApplies::AtStartup});
}

// A fork above the top is taken as a fork just under it, so the crown keeps some depth.
CrownEnvelope treeCrown(const ForestTreeSettings& settings) {
    const float fork = std::min(settings.forkHeight, settings.height - 1.0f);
    return CrownEnvelope{.centre = Vec3{0.0f, 0.5f * (fork + settings.height), 0.0f}, .radius = settings.crownRadius,
                         .halfHeight = 0.5f * (settings.height - fork)};
}

TreeVariantShape treeVariantShape(const ForestTreeSettings& settings, std::uint32_t seed) {
    const std::uint32_t shapeSeed = forestShapeSeed(seed, 4);
    const float lean = settings.lean * (leastLeanShare + (1.0f - leastLeanShare) * forestUnitRandom(shapeSeed, 1));
    const float turn = fullTurnRadians * forestUnitRandom(shapeSeed, 2);
    const int moreOrFewer = std::min(static_cast<int>(3.0f * forestUnitRandom(shapeSeed, 3)), 2) - 1;
    return TreeVariantShape{.crown = treeCrown(settings), .lean = Vec3{lean * std::cos(turn), 0.0f, lean * std::sin(turn)},
                            .limbs = std::max(settings.limbs + moreOrFewer, 2)};
}

/*
* The scaffold first, each limb aimed at the middle of its clumps and stopping a clump's radius short of it, then space
* colonisation from every node of it into the clumps. Each chain one tube: the trunk's lobed into buttresses with sides
* enough for their thin fins, every other as many sides as its thickest (first) radius asks for. A side chain's first
* point is its parent's node at the chain's own radius, so the branch starts inside the limb it leaves.
*/
TreeMeshes growTreeMeshes(const ForestTreeSettings& settings, std::uint32_t seed, float barkTileMetres) {
    const TreeVariantShape shape = treeVariantShape(settings, seed);
    const CrownEnvelope& crown = shape.crown;
    const Vec3 base{0.0f, -treeSinkMetres, 0.0f};
    const float forkHeight = crown.centre.y - crown.halfHeight;
    const Vec3 fork = Vec3{0.0f, forkHeight, 0.0f} + shape.lean;
    const std::vector<TreeClump> clumps = treeClumps(crown, settings.clumps, settings.clumpRadius, shape.limbs, forestShapeSeed(seed, 5));

    // Each limb's target: the middle of its clumps.
    std::vector<Vec3> targets;

    for (const TreeClump& clump : clumps) {
        if (clump.limb >= static_cast<int>(targets.size())) {
            targets.resize(static_cast<std::size_t>(clump.limb) + 1, Vec3{0.0f, 0.0f, 0.0f});
        }
    }

    std::vector<int> clumpsOf(targets.size(), 0);

    for (const TreeClump& clump : clumps) {
        targets[static_cast<std::size_t>(clump.limb)] = targets[static_cast<std::size_t>(clump.limb)] + clump.centre;
        ++clumpsOf[static_cast<std::size_t>(clump.limb)];
    }

    for (std::size_t limb = 0; limb < targets.size(); ++limb) {
        targets[limb] = targets[limb] / static_cast<float>(std::max(clumpsOf[limb], 1));
    }

    TreeGrowth growth;
    growth.gravity = treeGravity;
    const std::vector<TreeNode> nodes =
        coloniseTree(growScaffold(base, fork, targets, settings.clumpRadius, growth.step, forestShapeSeed(seed, 6)), crown,
                     clumpAttractors(clumps, settings.attractors, forestShapeSeed(seed, 1)), growth);
    std::vector<float> radii = leonardoRadii(nodes, settings.twigRadius, leonardoExponent);
    const std::vector<bool> tips = treeTips(nodes);
    const std::vector<std::vector<int>> chains = branchChains(nodes, radii);

    TreeMeshes meshes;
    meshes.tipCount = static_cast<int>(std::count(tips.begin(), tips.end(), true));
    meshes.trunkRadius = radii.front();

    // The trunk (the leading run of nodes up to the fork) tapers: Leonardo's rule alone keeps an unbranched trunk one
    // thickness all the way up, a pole; real trunks thicken towards the foot, carrying the weight above.
    const float rise = forkHeight - base.y;

    for (std::size_t node = 0; node < nodes.size() && rise > 0.0f; ++node) {
        if (node > 0 && (nodes[node].parent != static_cast<int>(node) - 1 || nodes[node].position.y > forkHeight + 1e-4f)) {
            break;
        }

        const float share = std::clamp((nodes[node].position.y - base.y) / rise, 0.0f, 1.0f);
        radii[node] *= 1.0f + trunkTaper * (1.0f - share);
    }

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

    const std::vector<bool> leafy = leafyNodes(radii, settings.leafyRadius);
    meshes.leafyCount = static_cast<int>(std::count(leafy.begin(), leafy.end(), true));
    meshes.cardCount = appendClusterCards(meshes.leaves, nodes, radii, crown, clumps, settings, forestShapeSeed(seed, 3));
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

Vec2 forestTreesStart(const std::vector<TreePlacement>& trees) {
    if (trees.empty()) {
        // The path from the Ring to its next point, 100 m along northwards.
        const std::vector<Vec2>& path = forestPathPoints();
        const std::size_t ring = static_cast<std::size_t>(std::find_if(path.begin(), path.end(), [](const Vec2& point) {
            return point.x == forestRingPlace.x && point.y == forestRingPlace.y;
        }) - path.begin());
        const Vec2 from = path[ring];
        const Vec2 to = path[std::min(ring + 1, path.size() - 1)];
        const float share = to.y != from.y ? (-100.0f - from.y) / (to.y - from.y) : 0.0f;
        return from + (to - from) * share;
    }

    std::size_t best = 0;
    int mostNeighbours = -1;

    for (std::size_t i = 0; i < trees.size(); ++i) {
        int neighbours = 0;

        for (std::size_t j = 0; j < trees.size(); ++j) {
            const float apart = (Vec2{trees[i].foot.x, trees[i].foot.z} - Vec2{trees[j].foot.x, trees[j].foot.z}).length();
            neighbours += j != i && apart <= treesStartNeighbourhood ? 1 : 0;
        }

        if (neighbours > mostNeighbours) {
            mostNeighbours = neighbours;
            best = i;
        }
    }

    return Vec2{trees[best].foot.x, trees[best].foot.z + treesStartDistance};
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
    return tree.foot + up * (treeCrown(settings).centre.y * tree.scale);
}

float placedCrownRadius(const TreePlacement& tree, const ForestTreeSettings& settings) {
    const CrownEnvelope crown = treeCrown(settings);
    return std::max(crown.radius, crown.halfHeight) * tree.scale;
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
