#include "games/forest/forest_trees.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_ground.h"
#include "games/forest/forest_planting.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/island_hangings.h"
#include "games/forest/leaf_clusters.h"
#include "games/forest/valley_shapes.h"
#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "math/angles.h"
#include "input/input.h"
#include "math/closest_points.h"
#include "scene/model_renderer.h"
#include "scene/planting_cells.h"

#include "forest_helpers.h"
#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <set>
#include <vector>

namespace {
    // A small crown, so the tests grow trees quickly.
    const CrownEnvelope smallCrown{.centre = Vec3{0.0f, 12.0f, 0.0f}, .radius = 5.0f, .halfHeight = 2.0f};
    const Vec3 smallBase{0.0f, -1.0f, 0.0f};

    std::vector<TreeNode> smallTree(std::uint32_t seed) {
        return growTree(smallBase, smallCrown, crownAttractors(smallCrown, 400, seed), TreeGrowth{});
    }

    float horizontal(const Vec3& vector) {
        return std::sqrt(vector.x * vector.x + vector.z * vector.z);
    }

    bool sameNodes(const std::vector<TreeNode>& a, const std::vector<TreeNode>& b) {
        if (a.size() != b.size()) {
            return false;
        }

        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].parent != b[i].parent || (a[i].position - b[i].position).lengthSquared() != 0.0f) {
                return false;
            }
        }

        return true;
    }

    /*
    * The attractors fill the envelope: every one inside it, their mean near its centre, reaching its rim and its top
    * and bottom; the same seed scatters the same ones. The envelope's outward direction is its surface's normal.
    */
    void testAttractorsFillTheCrown() {
        const CrownEnvelope crown{.centre = Vec3{3.0f, 30.0f, -2.0f}, .radius = 14.0f, .halfHeight = 5.0f};
        assert(insideCrown(crown, crown.centre) && insideCrown(crown, crown.centre + Vec3{0.0f, 5.0f, 0.0f}));
        assert(!insideCrown(crown, crown.centre + Vec3{14.1f, 0.0f, 0.0f}) && !insideCrown(crown, crown.centre + Vec3{0.0f, -5.1f, 0.0f}));
        assert(near(crownOutward(crown, crown.centre + Vec3{14.0f, 0.0f, 0.0f}).x, 1.0f, 1e-5f));
        assert(near(crownOutward(crown, crown.centre + Vec3{0.0f, 5.0f, 0.0f}).y, 1.0f, 1e-5f));
        assert(near(crownOutward(crown, crown.centre).y, 1.0f, 1e-5f));

        // On an oblate crown's shoulder the way out leans up more than the point does: the gradient divides by the
        // squared half sizes.
        const Vec3 shoulder = crown.centre + Vec3{14.0f * 0.7071f, 5.0f * 0.7071f, 0.0f};
        const Vec3 out = crownOutward(crown, shoulder);
        assert(near(out.length(), 1.0f, 1e-5f) && out.y > out.x);

        const std::vector<Vec3> points = crownAttractors(crown, 2000, 3u);
        assert(points.size() == 2000);
        Vec3 mean{0.0f, 0.0f, 0.0f};
        float widest = 0.0f;
        float highest = -1e9f;
        float lowest = 1e9f;

        for (const Vec3& point : points) {
            assert(insideCrown(crown, point));
            mean = mean + point * (1.0f / 2000.0f);
            widest = std::max(widest, horizontal(point - crown.centre));
            highest = std::max(highest, point.y);
            lowest = std::min(lowest, point.y);
        }

        assert((mean - crown.centre).length() < 1.0f);
        assert(widest > 13.0f && highest > crown.centre.y + 4.0f && lowest < crown.centre.y - 4.0f);

        const std::vector<Vec3> again = crownAttractors(crown, 2000, 3u);
        const std::vector<Vec3> other = crownAttractors(crown, 2000, 4u);
        assert((again[17] - points[17]).lengthSquared() == 0.0f && (other[17] - points[17]).lengthSquared() > 0.0f);
    }

    /*
    * A tree grows into its crown: the root at the base, parents before their children, every step at most the growth's
    * step, the trunk straight up to the crown, branches reaching its rim and top, nearly every attractor used up, no
    * two nodes on top of each other; the same seed grows the same tree, another a different one.
    */
    void testATreeGrowsIntoItsCrown() {
        const TreeGrowth growth;
        const std::vector<Vec3> attractors = crownAttractors(smallCrown, 400, 5u);
        const std::vector<TreeNode> nodes = growTree(smallBase, smallCrown, attractors, growth);
        assert(nodes.size() > 100);
        assert(nodes[0].parent == -1 && (nodes[0].position - smallBase).lengthSquared() == 0.0f);

        const float crownBottom = smallCrown.centre.y - smallCrown.halfHeight;
        float widest = 0.0f;
        float highest = -1e9f;

        for (std::size_t i = 1; i < nodes.size(); ++i) {
            assert(nodes[i].parent >= 0 && static_cast<std::size_t>(nodes[i].parent) < i);
            const float step = (nodes[i].position - nodes[static_cast<std::size_t>(nodes[i].parent)].position).length();
            assert(step > 0.0f && step <= growth.step + 1e-4f);

            if (nodes[i].position.y < crownBottom - 1.0f) {
                assert(horizontal(nodes[i].position) < 1e-5f);
            }

            widest = std::max(widest, horizontal(nodes[i].position - smallCrown.centre));
            highest = std::max(highest, nodes[i].position.y);
        }

        assert(widest > 0.8f * smallCrown.radius && highest > smallCrown.centre.y + 0.6f * smallCrown.halfHeight);

        int reached = 0;

        for (const Vec3& attractor : attractors) {
            const bool isNear = std::any_of(nodes.begin(), nodes.end(), [&](const TreeNode& node) {
                return (node.position - attractor).length() < growth.kill + growth.step;
            });
            reached += isNear ? 1 : 0;
        }

        assert(reached >= static_cast<int>(0.9f * static_cast<float>(attractors.size())));

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            for (std::size_t j = i + 1; j < nodes.size(); ++j) {
                assert((nodes[i].position - nodes[j].position).length() >= 0.1f * growth.step - 1e-5f);
            }
        }

        assert(sameNodes(nodes, smallTree(5u)) && !sameNodes(nodes, smallTree(6u)));

        // A leaning trunk follows base + up rise s + lean s^2, straight at the foot, its steps still at most a step.
        const Vec3 lean{2.0f, 0.0f, -1.0f};
        const std::vector<TreeNode> leaning = growTree(smallBase, smallCrown, attractors, growth, lean);
        const float rise = crownBottom - smallBase.y;
        bool reachedTop = false;

        for (std::size_t i = 1; i < leaning.size(); ++i) {
            const Vec3& at = leaning[i].position;
            assert((at - leaning[static_cast<std::size_t>(leaning[i].parent)].position).length() <= growth.step + 1e-4f);

            if (at.y < crownBottom - 1.0f) {
                const float share = (at.y - smallBase.y) / rise;
                assert(near(at.x, lean.x * share * share, 1e-4f) && near(at.z, lean.z * share * share, 1e-4f));
            }

            reachedTop = reachedTop || (at - (smallBase + Vec3{0.0f, rise, 0.0f} + lean)).length() < 1e-4f;
        }

        assert(reachedTop);
    }

    /*
    * Gravity in the growth: each step still exactly a step, the branches hanging lower than without it.
    */
    void testGravityDrooping() {
        const std::vector<Vec3> attractors = crownAttractors(smallCrown, 400, 5u);
        const std::vector<TreeNode> straight = growTree(smallBase, smallCrown, attractors, TreeGrowth{});
        TreeGrowth heavy;
        heavy.gravity = 0.5f;
        const std::vector<TreeNode> drooping = growTree(smallBase, smallCrown, attractors, heavy);
        const float crownBottom = smallCrown.centre.y - smallCrown.halfHeight;
        const auto meanCrownHeight = [&](const std::vector<TreeNode>& nodes) {
            double sum = 0.0;
            int count = 0;

            for (std::size_t i = 1; i < nodes.size(); ++i) {
                const float step = (nodes[i].position - nodes[static_cast<std::size_t>(nodes[i].parent)].position).length();
                assert(step <= heavy.step + 1e-4f);

                if (nodes[i].position.y > crownBottom - 1.0f) {
                    sum += nodes[i].position.y;
                    ++count;
                }
            }

            return sum / count;
        };

        assert(meanCrownHeight(drooping) < meanCrownHeight(straight));
    }

    /*
    * A variant's own shape: the settings' crown from the fork to the top, its lean 30-100% of the setting and level, its
    * limbs one fewer to one more than the setting (two at least); the same seed the same shape, others different.
    */
    void testVariantShapes() {
        const ForestTreeSettings settings;
        const CrownEnvelope crown = treeCrown(settings);
        assert(near(crown.centre.y, 0.5f * (settings.forkHeight + settings.height), 1e-5f));
        assert(near(crown.halfHeight, 0.5f * (settings.height - settings.forkHeight), 1e-5f) && crown.radius == settings.crownRadius);
        std::set<int> limbCounts;

        for (std::uint32_t seed = 1; seed <= 40; ++seed) {
            const TreeVariantShape shape = treeVariantShape(settings, seed);
            assert(shape.crown.radius == crown.radius && shape.crown.centre.y == crown.centre.y && shape.crown.halfHeight == crown.halfHeight);
            assert(shape.lean.y == 0.0f);
            const float lean = horizontal(shape.lean);
            assert(lean >= leastLeanShare * settings.lean - 1e-4f && lean <= settings.lean + 1e-4f);
            assert(shape.limbs >= std::max(settings.limbs - 1, 2) && shape.limbs <= settings.limbs + 1);
            limbCounts.insert(shape.limbs);
        }

        assert(limbCounts.size() == 3);
        assert(treeVariantShape(settings, 7u).lean.x == treeVariantShape(settings, 7u).lean.x);
        assert(treeVariantShape(settings, 7u).lean.x != treeVariantShape(settings, 8u).lean.x);
    }

    /*
    * The clumps: as many as asked (room allowing), each 75-125% of the clump radius, their centres inside the crown and
    * far enough apart to leave gaps, every limb given at least one, the clumps going round the trunk limb by limb; the
    * same seed the same clumps.
    */
    void testClumps() {
        const CrownEnvelope crown{.centre = Vec3{0.0f, 20.0f, 0.0f}, .radius = 13.0f, .halfHeight = 10.0f};
        const std::vector<TreeClump> clumps = treeClumps(crown, 9, 3.5f, 4, 11u);
        assert(clumps.size() == 9);
        std::vector<int> perLimb(4, 0);

        for (std::size_t i = 0; i < clumps.size(); ++i) {
            const TreeClump& clump = clumps[i];
            assert(insideCrown(crown, clump.centre));
            assert(clump.radius >= 0.75f * 3.5f - 1e-4f && clump.radius <= 1.25f * 3.5f + 1e-4f);
            assert(clump.limb >= 0 && clump.limb < 4);
            ++perLimb[static_cast<std::size_t>(clump.limb)];

            for (std::size_t j = 0; j < i; ++j) {
                assert((clump.centre - clumps[j].centre).length() >= clumpSpacing * (clump.radius + clumps[j].radius) - 1e-4f);
            }
        }

        assert(std::all_of(perLimb.begin(), perLimb.end(), [](int count) { return count >= 1; }));

        // Fewer clumps than limbs: each clump a limb of its own.
        const std::vector<TreeClump> few = treeClumps(crown, 2, 3.5f, 4, 11u);
        assert(few.size() == 2 && few[0].limb != few[1].limb);

        const std::vector<TreeClump> again = treeClumps(crown, 9, 3.5f, 4, 11u);
        assert((again[4].centre - clumps[4].centre).lengthSquared() == 0.0f);
    }

    /*
    * The attractors fill the clumps and nothing else, so the gaps between them stay empty: exactly as many as asked,
    * every one inside a clump, every clump with some, the bigger ones with more.
    */
    void testClumpAttractors() {
        const std::vector<TreeClump> clumps{{Vec3{0.0f, 10.0f, 0.0f}, 1.0f, 0}, {Vec3{6.0f, 10.0f, 0.0f}, 2.0f, 1}};
        const std::vector<Vec3> points = clumpAttractors(clumps, 900, 3u);
        assert(points.size() == 900);
        int inFirst = 0;
        int inSecond = 0;

        for (const Vec3& point : points) {
            const bool first = (point - clumps[0].centre).length() <= clumps[0].radius + 1e-4f;
            const bool second = (point - clumps[1].centre).length() <= clumps[1].radius + 1e-4f;
            assert(first || second);
            inFirst += first ? 1 : 0;
            inSecond += second ? 1 : 0;
        }

        // By volume, 1 : 8.
        assert(inFirst == 100 && inSecond == 800);
    }

    /*
    * The scaffold: the trunk up to the fork (leaning to it), then one limb a target, each leaving the fork's node and
    * walking, kinked, until it is within stopMetres of its target; every step at most a step; the same seed the same.
    */
    void testScaffold() {
        const Vec3 base{0.0f, -1.0f, 0.0f};
        const Vec3 fork{1.0f, 9.0f, 0.5f};
        const std::vector<Vec3> targets{Vec3{8.0f, 18.0f, 0.0f}, Vec3{-7.0f, 20.0f, 3.0f}, Vec3{0.0f, 17.0f, -9.0f}};
        const std::vector<TreeNode> nodes = growScaffold(base, fork, targets, 3.0f, 0.5f, 5u);
        assert((nodes[0].position - base).lengthSquared() == 0.0f && nodes[0].parent == -1);

        int forkNode = -1;

        for (std::size_t i = 1; i < nodes.size(); ++i) {
            assert(nodes[i].parent >= 0 && static_cast<std::size_t>(nodes[i].parent) < i);
            assert((nodes[i].position - nodes[static_cast<std::size_t>(nodes[i].parent)].position).length() <= 0.5f + 1e-4f);

            if ((nodes[i].position - fork).length() < 1e-4f) {
                forkNode = static_cast<int>(i);
            }
        }

        assert(forkNode > 0);
        const std::vector<bool> tips = treeTips(nodes);
        int limbs = 0;

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            limbs += nodes[i].parent == forkNode ? 1 : 0;
        }

        assert(limbs == 3 && std::count(tips.begin(), tips.end(), true) == 3);

        // Each limb ends near its own target, kinked on the way (not a straight line from the fork).
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (!tips[i]) {
                continue;
            }

            float nearestTarget = 1e9f;

            for (const Vec3& target : targets) {
                nearestTarget = std::min(nearestTarget, (nodes[i].position - target).length());
            }

            assert(nearestTarget <= 3.0f + 1e-4f);
        }

        float furthestOffLine = 0.0f;

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (static_cast<int>(i) > forkNode) {
                // Off the straight line from the fork to the nearest target.
                float offLine = 1e9f;

                for (const Vec3& target : targets) {
                    offLine = std::min(offLine, (nodes[i].position - closestPointOnSegment(nodes[i].position, fork, target)).length());
                }

                furthestOffLine = std::max(furthestOffLine, offLine);
            }
        }

        assert(furthestOffLine > 0.3f);
        assert(sameNodes(nodes, growScaffold(base, fork, targets, 3.0f, 0.5f, 5u)));
    }

    /*
    * Leonardo's rule: every tip has the tip radius and every parent's r^n is the sum of its children's, so the root's
    * is tipRadius times the tips' count to the 1/n.
    */
    void testLeonardosRule() {
        // 0 - 1 - {2, 3}, 3 - {4, 5}: tips 2, 4 and 5.
        const std::vector<TreeNode> small{{Vec3{0.0f, 0.0f, 0.0f}, -1}, {Vec3{0.0f, 1.0f, 0.0f}, 0}, {Vec3{1.0f, 1.0f, 0.0f}, 1},
                                          {Vec3{0.0f, 2.0f, 0.0f}, 1}, {Vec3{1.0f, 2.0f, 0.0f}, 3}, {Vec3{-1.0f, 2.0f, 0.0f}, 3}};
        const std::vector<bool> tips = treeTips(small);
        assert(!tips[0] && !tips[1] && tips[2] && !tips[3] && tips[4] && tips[5]);
        const std::vector<float> radii = leonardoRadii(small, 0.1f, 2.0f);
        assert(near(radii[2], 0.1f, 1e-6f) && near(radii[3], 0.1f * std::sqrt(2.0f), 1e-6f));
        assert(near(radii[1], 0.1f * std::sqrt(3.0f), 1e-6f) && near(radii[0], radii[1], 1e-6f));

        const std::vector<TreeNode> nodes = smallTree(5u);
        const std::vector<float> grown = leonardoRadii(nodes, 0.02f, leonardoExponent);
        const std::vector<bool> grownTips = treeTips(nodes);
        std::vector<double> sums(nodes.size(), 0.0);
        int tipCount = 0;

        for (std::size_t i = 1; i < nodes.size(); ++i) {
            sums[static_cast<std::size_t>(nodes[i].parent)] += std::pow(static_cast<double>(grown[i]), static_cast<double>(leonardoExponent));
        }

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (grownTips[i]) {
                ++tipCount;
                assert(grown[i] == 0.02f);
            } else {
                assert(std::fabs(std::pow(static_cast<double>(grown[i]), static_cast<double>(leonardoExponent)) - sums[i]) <= 1e-4 * sums[i]);
            }
        }

        assert(tipCount > 20 && near(grown[0], 0.02f * std::pow(static_cast<float>(tipCount), 1.0f / leonardoExponent), 1e-3f * grown[0]));
    }

    /*
    * The chains: the trunk's first, each following its thickest child (the lower index on a tie), the others starting
    * at their parent's node; one chain a tip, every node but the root once after a chain's start, consecutive nodes
    * parent and child, each ending at a tip, never thickening along the way.
    */
    void testBranchChains() {
        const std::vector<TreeNode> small{{Vec3{0.0f, 0.0f, 0.0f}, -1}, {Vec3{0.0f, 1.0f, 0.0f}, 0}, {Vec3{1.0f, 1.0f, 0.0f}, 1},
                                          {Vec3{0.0f, 2.0f, 0.0f}, 1}, {Vec3{1.0f, 2.0f, 0.0f}, 3}, {Vec3{-1.0f, 2.0f, 0.0f}, 3}};
        const std::vector<std::vector<int>> chains = branchChains(small, leonardoRadii(small, 0.1f, 2.0f));
        assert(chains.size() == 3);
        assert((chains[0] == std::vector<int>{0, 1, 3, 4}));
        assert((chains[1] == std::vector<int>{1, 2} && chains[2] == std::vector<int>{3, 5}) ||
               (chains[1] == std::vector<int>{3, 5} && chains[2] == std::vector<int>{1, 2}));

        const std::vector<TreeNode> nodes = smallTree(5u);
        const std::vector<float> radii = leonardoRadii(nodes, 0.02f, leonardoExponent);
        const std::vector<bool> tips = treeTips(nodes);
        const std::vector<std::vector<int>> grown = branchChains(nodes, radii);
        const long tipCount = std::count(tips.begin(), tips.end(), true);
        assert(static_cast<long>(grown.size()) == tipCount && grown[0].front() == 0);
        std::vector<int> seen(nodes.size(), 0);
        seen[0] = 1;

        for (const std::vector<int>& chain : grown) {
            assert(chain.size() >= 2 && tips[static_cast<std::size_t>(chain.back())]);

            for (std::size_t k = 1; k < chain.size(); ++k) {
                const std::size_t node = static_cast<std::size_t>(chain[k]);
                assert(nodes[node].parent == chain[k - 1]);
                ++seen[node];

                if (k > 1) {
                    assert(radii[node] <= radii[static_cast<std::size_t>(chain[k - 1])]);
                }
            }
        }

        assert(std::all_of(seen.begin(), seen.end(), [](int count) { return count == 1; }));
    }

    // A tube's sides: about one every 15 cm round it, 3 at least, 16 at most, never fewer for a thicker tube.
    void testTubeSides() {
        assert(tubeSides(0.02f) == 3 && tubeSides(1.0f) == 16 && tubeSides(0.1f) >= 4 && tubeSides(0.1f) <= 6);

        for (float radius = 0.01f; radius < 2.0f; radius += 0.01f) {
            assert(tubeSides(radius + 0.01f) >= tubeSides(radius));
        }
    }

    /*
    * Buttress lobes: count fins standing out up to the amplitude at the ground, back to the plain radius between them
    * and high up, full below the ground; a lobed tube's rings follow them, its normals and tangents square and facing
    * out, and its bark in whole tiles round it. Without options a tube is drawn as before (island_hangings_test).
    */
    void testButtressLobes() {
        const TubeLobes lobes{.count = 5, .amplitude = 1.5f, .heightMetres = 1.5f, .sharpness = 24.0f, .groundHeight = 0.0f, .seed = 9u};
        assert(lobedRadius(1.0f, 0.3f, 0.0f, TubeLobes{}) == 1.0f);

        float widest = 0.0f;
        float narrowest = 1e9f;
        int peaks = 0;
        constexpr int samples = 3600;

        for (int sample = 0; sample < samples; ++sample) {
            const float angle = fullTurnRadians * static_cast<float>(sample) / samples;
            const float radius = lobedRadius(1.0f, angle, 0.0f, lobes);
            const float before = lobedRadius(1.0f, angle - fullTurnRadians / samples, 0.0f, lobes);
            const float after = lobedRadius(1.0f, angle + fullTurnRadians / samples, 0.0f, lobes);
            widest = std::max(widest, radius);
            narrowest = std::min(narrowest, radius);
            peaks += radius > 1.3f && radius >= before && radius > after ? 1 : 0;

            assert(near(lobedRadius(1.0f, angle, 15.0f, lobes), 1.0f, 1e-3f));
            assert(lobedRadius(1.0f, angle, -1.0f, lobes) == radius);
        }

        assert(peaks == 5 && widest > 1.0f + 0.6f * 1.5f && widest < 1.0f + 1.5f + 0.01f && narrowest < 1.05f);

        HangingStrand trunk;

        for (int ring = 0; ring <= 12; ++ring) {
            trunk.points.push_back(Vec3{0.0f, -1.0f + 0.5f * static_cast<float>(ring), 0.0f});
            trunk.radii.push_back(1.0f);
        }

        IndexedMesh mesh;
        appendTube(mesh, trunk, 64, 1.5f, TubeOptions{.wholeTilesRound = true, .lobes = lobes});
        assert(mesh.vertices.size() == 13 * 65 && trianglesFaceTheirNormals(mesh));

        for (std::size_t ring = 0; ring < trunk.points.size(); ++ring) {
            for (int side = 0; side <= 64; ++side) {
                const MeshVertex& vertex = mesh.vertices[ring * 65 + static_cast<std::size_t>(side)];
                const Vec3 fromAxis = vertex.position - trunk.points[ring];
                const float angle = fullTurnRadians * static_cast<float>(side) / 64.0f;
                assert(near(fromAxis.length(), lobedRadius(1.0f, angle, trunk.points[ring].y, lobes), 1e-3f));
                assert(near(vertex.normal.length(), 1.0f, 1e-4f) && vertex.normal.dot(fromAxis) > 0.0f);

                const Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
                assert(near(tangent.length(), 1.0f, 1e-4f) && std::fabs(tangent.dot(vertex.normal)) < 1e-3f);
                assert(near(vertex.uv.x, 4.0f * static_cast<float>(side) / 64.0f, 1e-5f)); // round(2 pi / 1.5) tiles
            }
        }
    }

    /*
    * The leafy nodes: every node but the root no thicker than the leafy radius, so every tip and the twigs' last
    * stretches; more of them than tips.
    */
    void testLeafyNodes() {
        const std::vector<float> radii{0.5f, 0.04f, 0.02f, 0.05f, 0.041f};
        const std::vector<bool> leafy = leafyNodes(radii, 0.04f);
        assert(!leafy[0] && leafy[1] && leafy[2] && !leafy[3] && !leafy[4]);
        assert(!leafyNodes(std::vector<float>{0.02f}, 0.04f)[0]); // a root alone is no twig

        const std::vector<TreeNode> nodes = smallTree(5u);
        const std::vector<float> grown = leonardoRadii(nodes, 0.02f, leonardoExponent);
        const std::vector<bool> tips = treeTips(nodes);
        const std::vector<bool> grownLeafy = leafyNodes(grown, 0.04f);

        for (std::size_t node = 1; node < nodes.size(); ++node) {
            assert(!tips[node] || grownLeafy[node]);
        }

        assert(std::count(grownLeafy.begin(), grownLeafy.end(), true) > std::count(tips.begin(), tips.end(), true));
    }

    // A node belongs to the clump it is deepest in, not merely the nearest centre.
    void testClumpOf() {
        const std::vector<TreeClump> clumps{{Vec3{0.0f, 0.0f, 0.0f}, 1.0f, 0}, {Vec3{2.5f, 0.0f, 0.0f}, 3.0f, 1}};
        assert(clumpOf(Vec3{1.0f, 0.0f, 0.0f}, clumps) == 1);  // 1 radius out of the first, a half of the second
        assert(clumpOf(Vec3{-0.2f, 0.0f, 0.0f}, clumps) == 0);
        assert(clumpOf(Vec3{0.0f, 0.0f, 0.0f}, {}) == -1);
    }

    // The foliage's normals: the downward part halved, the blend unit and leaning out of the clump.
    void testFoliageNormals() {
        const Vec3 up{0.0f, 1.0f, 0.0f};
        const Vec3 cone = coneFlattened(Vec3{1.0f, -1.0f, 0.0f}.normalized());
        assert(near(cone.length(), 1.0f, 1e-5f) && near(cone.y / cone.x, -0.5f, 1e-5f));
        assert(coneFlattened(up).y == 1.0f && coneFlattened(Vec3{0.0f, -1.0f, 0.0f}).y == -1.0f);

        const Vec3 facing = Vec3{0.0f, 0.2f, 1.0f}.normalized();
        const Vec3 clumpOut{1.0f, 0.0f, 0.0f};
        const Vec3 normal = foliageNormal(facing, clumpOut, up);
        const Vec3 expected = (facing * cardNormalShare + clumpOut * clumpNormalShare + up * crownNormalShare).normalized();
        assert(near(normal.length(), 1.0f, 1e-5f) && near(normal.x, expected.x, 1e-5f) && near(normal.z, expected.z, 1e-5f));
        assert(near(cardNormalShare + clumpNormalShare + crownNormalShare, 1.0f, 1e-6f));
    }

    /*
    * Cluster cards on a small clumped tree: one a leafy node in a clump's shell and the fillers, every shell card facing
    * out of its clump, each clusterSize a side (fillers half as big again), its picture one of the four clusters, its
    * normals unit and the triangles facing them; the same seed the same cards.
    */
    void testClusterCards() {
        ForestTreeSettings settings;
        settings.clumpRadius = 1.5f;
        const CrownEnvelope crown{.centre = Vec3{0.0f, 9.5f, 0.0f}, .radius = 5.0f, .halfHeight = 4.5f};
        const std::vector<TreeClump> clumps = treeClumps(crown, 5, 1.5f, 3, 3u);
        std::vector<Vec3> targets(3, Vec3{0.0f, 0.0f, 0.0f});
        std::vector<int> counts(3, 0);

        for (const TreeClump& clump : clumps) {
            targets[static_cast<std::size_t>(clump.limb)] = targets[static_cast<std::size_t>(clump.limb)] + clump.centre;
            ++counts[static_cast<std::size_t>(clump.limb)];
        }

        for (std::size_t limb = 0; limb < 3; ++limb) {
            targets[limb] = targets[limb] / static_cast<float>(counts[limb]);
        }

        const std::vector<TreeNode> nodes =
            coloniseTree(growScaffold(Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 5.0f, 0.0f}, targets, 1.5f, 0.5f, 4u), crown,
                         clumpAttractors(clumps, 400, 5u), TreeGrowth{});
        const std::vector<float> radii = leonardoRadii(nodes, settings.twigRadius, leonardoExponent);
        const std::vector<bool> leafy = leafyNodes(radii, settings.leafyRadius);
        int shellNodes = 0;

        for (std::size_t node = 0; node < nodes.size(); ++node) {
            const int clump = clumpOf(nodes[node].position, clumps);

            if (leafy[node] && clump >= 0) {
                const TreeClump& home = clumps[static_cast<std::size_t>(clump)];
                shellNodes += (nodes[node].position - home.centre).length() >= settings.shellShare * home.radius ? 1 : 0;
            }
        }

        IndexedMesh cards;
        const int made = appendClusterCards(cards, nodes, radii, crown, clumps, settings, 9u);
        const int fillers = static_cast<int>(clumps.size()) * settings.fillCards;
        assert(shellNodes > 20 && made == shellNodes * settings.cardsPerNode + fillers);
        assert(cards.vertices.size() == static_cast<std::size_t>(made) * 4 && cards.indices.size() == static_cast<std::size_t>(made) * 6);
        assert(trianglesFaceTheirNormals(cards));

        for (int card = 0; card < made; ++card) {
            const MeshVertex* corner = &cards.vertices[static_cast<std::size_t>(card) * 4];
            const bool filler = card >= made - fillers;
            const float side = (corner[1].position - corner[0].position).length();
            assert(near(side, filler ? 1.5f * settings.clusterSize : settings.clusterSize, 1e-3f));

            if (!filler) {
                const Vec3 stem = (corner[0].position + corner[1].position) * 0.5f;
                const TreeClump& home = clumps[static_cast<std::size_t>(clumpOf(stem, clumps))];
                const Vec3 facing = (corner[1].position - corner[0].position).cross(corner[2].position - corner[0].position).normalized();
                assert(facing.dot((stem - home.centre).normalized()) > 0.0f);
            }

            const int column = static_cast<int>(std::floor((corner[0].uv.x + corner[2].uv.x) * 0.5f * clusterColumns));
            const int row = static_cast<int>(std::floor((corner[0].uv.y + corner[2].uv.y) * 0.5f * clusterRows));

            for (int k = 0; k < 4; ++k) {
                assert(near(corner[k].normal.length(), 1.0f, 1e-4f));
                assert(corner[k].uv.x >= column * 0.5f - 1e-6f && corner[k].uv.x <= (column + 1) * 0.5f + 1e-6f);
                assert(corner[k].uv.y >= row * 0.5f - 1e-6f && corner[k].uv.y <= (row + 1) * 0.5f + 1e-6f);
            }
        }

        // Outlined cells: each card that cell's outline, a fan of its corners.
        const std::vector<Vec2> diamond{{0.5f, 0.0f}, {1.0f, 0.5f}, {0.5f, 1.0f}, {0.0f, 0.5f}, {0.25f, 0.1f}};
        const ClusterOutlines outlines{diamond, diamond, diamond, diamond};
        IndexedMesh outlinedCards;
        const int outlinedMade = appendClusterCards(outlinedCards, nodes, radii, crown, clumps, settings, 9u, &outlines);
        assert(outlinedMade == made && outlinedCards.vertices.size() == static_cast<std::size_t>(made) * diamond.size());
        assert(outlinedCards.indices.size() == static_cast<std::size_t>(made) * 3 * (diamond.size() - 2));

        IndexedMesh again;
        appendClusterCards(again, nodes, radii, crown, clumps, settings, 9u);
        assert((again.vertices[5].position - cards.vertices[5].position).lengthSquared() == 0.0f);
    }

    ForestTreeSettings smallSettings() {
        ForestTreeSettings settings;
        settings.height = 14.0f;
        settings.forkHeight = 5.0f;
        settings.crownRadius = 5.0f;
        settings.limbs = 3;
        settings.clumps = 5;
        settings.clumpRadius = 1.5f;
        settings.attractors = 400;
        return settings;
    }

    /*
    * A whole tree: bark and leaves, a trunk as thick as Leonardo's rule makes it from the tips, sunk into the ground,
    * its buttresses standing out at the ground; the same seed builds the same tree.
    */
    void testATreeIsBuilt() {
        const ForestTreeSettings settings = smallSettings();
        const CrownEnvelope crown = treeCrown(settings);
        assert(crown.centre.x == 0.0f && crown.centre.y == 9.5f && crown.radius == 5.0f && crown.halfHeight == 4.5f);

        const TreeMeshes tree = growTreeMeshes(settings, 21u, 1.5f);
        assert(tree.tipCount > 20 && tree.leafyCount > tree.tipCount && !tree.bark.vertices.empty());
        assert(tree.cardCount > settings.clumps * settings.fillCards && tree.leaves.vertices.size() == static_cast<std::size_t>(tree.cardCount) * 4);
        assert(near(tree.trunkRadius, settings.twigRadius * std::pow(static_cast<float>(tree.tipCount), 1.0f / leonardoExponent), 1e-3f * tree.trunkRadius));
        assert(trianglesFaceTheirNormals(tree.bark));

        /*
        * The shadow leaves (the shadow passes' stand-in, drawn into shadows only): shadowCardsPerNode cards at the same
        * nodes and shadowFillCards in each clump, each shadowCardScale times as big, so a fraction of the cards (the
        * cost of turning the camera, which redraws the sun's cascades) cast most of the leaves' cover.
        */
        assert(tree.shadowCardCount > 0 && tree.shadowLeaves.vertices.size() == static_cast<std::size_t>(tree.shadowCardCount) * 4);
        assert(3 * tree.shadowCardCount <= tree.cardCount);
        const auto cardArea = [](const IndexedMesh& mesh) {
            float area = 0.0f;

            for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
                const Vec3& a = mesh.vertices[mesh.indices[index]].position;
                area += 0.5f * (mesh.vertices[mesh.indices[index + 1]].position - a).cross(mesh.vertices[mesh.indices[index + 2]].position - a).length();
            }

            return area;
        };
        const float coverShare = cardArea(tree.shadowLeaves) / cardArea(tree.leaves);
        assert(coverShare > 0.4f && coverShare < 1.0f);

        // The shadow bark: the bark simplified to about shadowBarkShare of its triangles (bark was nine tenths of what the
        // sun's cascades redrew as the camera turned).
        assert(!tree.shadowBark.indices.empty() && tree.shadowBark.indices.size() <= tree.bark.indices.size() / 4);

        /*
        * The levels of detail (docs/research/LOD.md): the leaves thinned to levelCardShare of the cards, each grown to keep their
        * area (SpeedTree's fewer, bigger cards; a simplifier cannot reduce separate cards), then the shadow leaves; the
        * bark simplified to levelBarkShare, then the shadow bark. Each level's error rises: the cards' by how much they
        * grew, the bark's by how far the simplifier moved it.
        */
        assert(tree.leafLevel.vertices.size() == static_cast<std::size_t>(tree.leafLevelCardCount) * 4);
        assert(2 * tree.leafLevelCardCount <= tree.cardCount + 2 * (settings.clumps + 1) && tree.leafLevelCardCount > tree.shadowCardCount);
        assert(tree.leafLevelError > 0.0f && tree.shadowLeafError > tree.leafLevelError);

        /*
        * The cards thin one by one on the way there (scene/card_thinning.h, docs/research/LOD.md, Hiding the switches): the kept
        * cards first in the leaves, so the level is the leaves' first vertices and triangles, drawn by the vertex shaders
        * grown as the fade left them; each vertex its card's stem and rank, rank 0 for the kept cards, the others' spread
        * past the least share so all are gone there. The level's error is the fade's at the least share.
        */
        const CardThinning& thinning = tree.leaves.thinning;
        assert(thinning.cardSize == settings.clusterSize && thinning.growth == settings.levelCardGrowth && thinning.leastShare == settings.levelCardShare);
        assert(tree.leaves.thinningCards.size() == tree.leaves.vertices.size());
        assert(near(tree.leafLevelError, thinningError(thinning, thinning.leastShare), 1e-6f));
        assert(std::equal(tree.leafLevel.indices.begin(), tree.leafLevel.indices.end(), tree.leaves.indices.begin()));

        for (std::size_t vertex = 0; vertex < tree.leaves.vertices.size(); ++vertex) {
            const float rank = tree.leaves.thinningCards[vertex].rank;

            if (vertex < tree.leafLevel.vertices.size()) {
                const Vec3 kept = tree.leafLevel.vertices[vertex].position - tree.leaves.vertices[vertex].position;
                assert(kept.x == 0.0f && kept.y == 0.0f && kept.z == 0.0f && rank == 0.0f);
            } else {
                assert(rank >= thinning.leastShare + cardThinningBand - 1e-6f && rank <= 1.0f);
            }
        }

        IndexedMesh grown = tree.leafLevel;

        for (std::size_t vertex = 0; vertex < grown.vertices.size(); ++vertex) {
            grown.vertices[vertex].position = thinnedPosition(grown.vertices[vertex].position, tree.leaves.thinningCards[vertex],
                                                              thinnedCardScale(thinning, 0.0f, thinning.leastShare));
        }

        const float levelCover = cardArea(grown) / cardArea(tree.leaves);
        assert(levelCover > 0.8f && levelCover < 1.25f);
        assert(near(tree.shadowLeafError, settings.clusterSize * (settings.shadowCardScale - 1.0f), 1e-5f));
        assert(tree.barkLevel.indices.size() < tree.bark.indices.size() && tree.barkLevel.indices.size() > tree.shadowBark.indices.size());
        assert(tree.barkLevelError > 0.0f && tree.shadowBarkError >= tree.barkLevelError);

        float lowest = 1e9f;
        float widestAtGround = 0.0f;

        for (const MeshVertex& vertex : tree.bark.vertices) {
            lowest = std::min(lowest, vertex.position.y);

            if (std::fabs(vertex.position.y) < 0.26f) {
                widestAtGround = std::max(widestAtGround, horizontal(vertex.position));
            }
        }

        // Within a centimetre: a leaning trunk's bottom ring tilts a little, square to the curve.
        assert(near(lowest, -treeSinkMetres, 1e-2f));
        assert(widestAtGround > tree.trunkRadius * (1.0f + 0.5f * settings.buttress));

        // Without buttresses the trunk tapers: at the foot trunkTaper thicker than Leonardo's radius, halfway to the fork
        // half that, at the fork Leonardo's own (the fork's ring is tilted towards a limb, so it is measured halfway).
        ForestTreeSettings plain = settings;
        plain.buttress = 0.0f;
        plain.lean = 0.0f;
        const TreeMeshes tapered = growTreeMeshes(plain, 21u, 1.5f);
        const float halfway = 0.5f * (plain.forkHeight - treeSinkMetres);
        float atFoot = 0.0f;
        float atHalfway = 0.0f;

        for (const MeshVertex& vertex : tapered.bark.vertices) {
            if (std::fabs(vertex.position.y + treeSinkMetres) < 1e-4f) {
                atFoot = std::max(atFoot, horizontal(vertex.position));
            }

            if (std::fabs(vertex.position.y - halfway) < 1e-4f) {
                atHalfway = std::max(atHalfway, horizontal(vertex.position));
            }
        }

        assert(near(atFoot, tapered.trunkRadius * (1.0f + trunkTaper), 1e-3f));
        assert(near(atHalfway, tapered.trunkRadius * (1.0f + 0.5f * trunkTaper), 1e-3f));

        const TreeMeshes again = growTreeMeshes(settings, 21u, 1.5f);
        assert(again.bark.vertices.size() == tree.bark.vertices.size() && again.leaves.vertices.size() == tree.leaves.vertices.size());
        assert((again.leaves.vertices[10].position - tree.leaves.vertices[10].position).lengthSquared() == 0.0f);
    }

    /*
    * The sight lines: from eye height along the path every 10 m or less, round the meadow and the clearing, to the
    * Monolith and the Ring; a sphere on one blocks it, one far off does not.
    */
    void testSightLines() {
        const std::vector<SightLine> lines = forestSightLines();
        const std::vector<Vec2>& path = forestPathPoints();
        float pathLength = 0.0f;

        for (std::size_t i = 1; i < path.size(); ++i) {
            pathLength += (path[i] - path[i - 1]).length();
        }

        assert(static_cast<float>(lines.size()) >= 2.0f * pathLength / sightLineSpacing);
        std::set<std::pair<float, float>> targets;

        for (const SightLine& line : lines) {
            assert(near(line.from.y, forestHeight(line.from.x, line.from.z) + sightEyeHeight, 1e-3f));
            targets.insert({line.to.x, line.to.y});
        }

        assert(targets.size() == 2);

        // Every point of the path has a viewpoint within half a spacing.
        for (std::size_t i = 1; i < path.size(); ++i) {
            const float length = (path[i] - path[i - 1]).length();

            for (float along = 0.0f; along < length; along += 1.0f) {
                const Vec2 at = path[i - 1] + (path[i] - path[i - 1]) * (along / length);
                const bool covered = std::any_of(lines.begin(), lines.end(), [&](const SightLine& line) {
                    return (Vec2{line.from.x, line.from.z} - at).length() <= 0.5f * sightLineSpacing + 0.01f;
                });
                assert(covered);
            }
        }

        const SightLine& line = lines[lines.size() / 2];
        assert(blocksSight((line.from + line.to) * 0.5f, 1.0f, lines));
        assert(!blocksSight(Vec3{0.0f, -5000.0f, 0.0f}, 1.0f, lines));
    }

    /*
    * The canopy trees' places, planted alone from the maps: within the patch, clear of the path, of each other's crowns'
    * middles and of the pillars, sized and turned at random, standing on the lowest ground round the trunk, blocking no
    * sight line, about the patch's area at the floor's density, the same every run; where the maps cap everything
    * under the crowns' height, none is planted.
    */
    void testTreesArePlaced() {
        const ForestTreeSettings settings;
        const ValleyMaps& maps = patchValleyMaps();
        const std::vector<PillarPlacement> pillars = placePillars(ValleyLayout{});
        const std::vector<SightLine> lines = forestSightLines();
        const PlantingLayerRecord layer = treePlantingLayer(settings, canopyPlaceSeed);
        const std::vector<PlantedPlant> planted = plantAlone(layer, maps);
        const float patchHectares = (treeSouthmost - treeNorthmost) * 2.0f * treeSpread / 10000.0f;
        assert(static_cast<float>(planted.size()) >= 0.5f * settings.planting.floorPerHectare * patchHectares);
        assert(static_cast<float>(planted.size()) <= 1.1f * settings.planting.floorPerHectare * patchHectares);
        std::set<int> variants;

        for (std::size_t i = 0; i < planted.size(); ++i) {
            const TreePlacement tree{planted[i].foot, planted[i].scale, planted[i].turnRadians, planted[i].kind};
            assert(tree.foot.z >= treeNorthmost && tree.foot.z <= treeSouthmost);
            assert(std::fabs(tree.foot.x - valleyAxis(tree.foot.z)) <= treeSpread);
            assert(forestPathDistance(tree.foot.x, tree.foot.z) >= settings.planting.pathClearance - 1e-3f);
            assert(tree.scale >= 0.85f && tree.scale <= 1.15f && tree.variant >= 0 && tree.variant < settings.variants);
            const float ground = terrainSurfaceHeight(patchTerrainHeights(), tree.foot.x, tree.foot.z);
            assert(tree.foot.y <= ground && tree.foot.y > ground - 2.0f);
            variants.insert(tree.variant);

            const float radius = placedCrownRadius(tree, settings);
            assert(radius == planted[i].reach && near(radius, settings.crownRadius * tree.scale, 1e-4f) && radius <= treeReach(settings) + 1e-4f);
            assert(near(placedCrownCentre(tree, settings).y, tree.foot.y + treeCrown(settings).centre.y * tree.scale, 1e-3f));
            assert(!blocksSight(placedCrownCentre(tree, settings), radius, lines));

            for (std::size_t j = 0; j < i; ++j) {
                const float apart = (Vec2{tree.foot.x, tree.foot.z} - Vec2{planted[j].foot.x, planted[j].foot.z}).length();
                assert(apart >= settings.spacingShare * std::max(radius, planted[j].reach) - 1e-4f);
            }

            for (const PillarPlacement& pillar : pillars) {
                assert((Vec2{tree.foot.x, tree.foot.z} - Vec2{pillar.foot.x, pillar.foot.z}).length() >= pillarBulgeShare * pillar.radius + radius);
            }
        }

        assert(variants.size() >= 2);
        const std::vector<PlantedPlant> again = plantAlone(layer, maps);
        assert(again.size() == planted.size() && again.back().foot.x == planted.back().foot.x && again.back().turnRadians == planted.back().turnRadians);

        // A wall of sight lines at crown height across the whole patch leaves no room.
        std::vector<SightLine> wall;

        for (float z = treeNorthmost - 20.0f; z <= treeSouthmost + 20.0f; z += 4.0f) {
            wall.push_back(SightLine{Vec3{-400.0f, treeCrown(settings).centre.y, z}, Vec3{400.0f, treeCrown(settings).centre.y, z}});
        }

        const ValleyMaps walled = buildValleyMaps(testPlantingSettings(), {}, wall, {treeReach(settings)}, testJobs());
        assert(plantAlone(layer, walled).empty());
    }

    /*
    * The start among the trees: treesStartDistance south of the tree with the most others within treesStartNeighbourhood
    * (the first on a tie); the path's point 100 m past the Ring when there are none.
    */
    void testTheStartAmongTheTrees() {
        const std::vector<TreePlacement> trees{
            {Vec3{100.0f, 0.0f, -50.0f}, 1.0f, 0.0f, 0},  // alone
            {Vec3{-40.0f, 0.0f, -120.0f}, 1.0f, 0.0f, 0}, // two neighbours
            {Vec3{-20.0f, 0.0f, -140.0f}, 1.0f, 0.0f, 0},
            {Vec3{-50.0f, 0.0f, -160.0f}, 1.0f, 0.0f, 0}};
        const Vec2 start = forestTreesStart(trees);
        assert(near(start.x, -40.0f, 1e-5f) && near(start.y, -120.0f + treesStartDistance, 1e-5f));

        const Vec2 none = forestTreesStart({});
        assert(near(none.y, -100.0f, 1e-4f) && forestPathDistance(none.x, none.y) < 0.1f);
    }

    /*
    * The canopy trees at the canopy's top (20-40 m, docs/research/JUNGLE.md), their crowns from about half their height (room for
    * the mid layer under them: docs/research/JUNGLE.md, The mid layer's shape), their leaves real-sized (15-25 cm, the
    * user's choice after the 30-40 cm ones made the trees read small), on cards that keep the clusters' twigs as full as
    * before (the leaf's share of a card unchanged).
    */
    void testTheSettings() {
        const ForestTreeSettings settings;
        assert(settings.height >= 35.0f && settings.height <= 40.0f && settings.forkHeight >= 0.45f * settings.height);
        assert(settings.planting.floorPerHectare >= 4.0f && settings.variants == 4 && settings.limbs == 4);
        assert(settings.leafLength >= 0.15f && settings.leafLength <= 0.25f);
        assert(settings.leafLength / settings.clusterSize >= 0.2f);
        ForestTreeSettings visited;
        FieldCounter counter;
        visit(visited, counter);
        assert(counter.fields == 39 && counter.live == 0);
        assert(settings.shadowBarkShare > 0.0f && settings.shadowBarkShare <= 0.2f);

        // Four times the first patch's crown cover (the user, Nov 14 2026): more trees, their crowns interlocking.
        assert(settings.planting.floorPerHectare >= 25.0f && settings.spacingShare < 0.6f);
        assert(settings.shadowCardsPerNode >= 1 && settings.shadowCardsPerNode < settings.cardsPerNode && settings.shadowCardScale > 1.0f);
    }

    /*
    * The emergents (docs/research/JUNGLE.md): 50-60 m, a clear trunk to over 60% of their height, a crown about half their height
    * across, a few of them, leaves as the canopy trees' so both share one clusters' texture.
    */
    void testTheEmergentSettings() {
        const ForestTreeSettings emergent = emergentTreeSettings();
        const ForestTreeSettings canopy;
        assert(emergent.height >= 50.0f && emergent.height <= 60.0f && emergent.forkHeight >= 0.6f * emergent.height);
        assert(2.0f * emergent.crownRadius >= 0.45f * emergent.height && 2.0f * emergent.crownRadius <= 0.55f * emergent.height);
        assert(emergent.planting.floorPerHectare >= 2.0f && emergent.planting.floorPerHectare <= 3.5f && emergent.spacingShare == 1.0f);
        assert(emergent.planting.floorPerHectare < canopy.planting.floorPerHectare && emergent.planting.footprintMetres == canopy.planting.footprintMetres);
        assert(emergent.leafLength == canopy.leafLength && emergent.clusterSize == canopy.clusterSize && emergent.clusterTexels == canopy.clusterTexels);
    }

    /*
    * The mid layer (docs/research/JUNGLE.md): 1200 slender trees 10-20 m tall, their crowns shallow (less deep than wide) and from
    * over half their height, under the canopy crowns' bottom, leaves as the canopy trees'.
    */
    void testTheMidLayerSettings() {
        const ForestTreeSettings mid = midTreeSettings();
        const ForestTreeSettings canopy;
        assert(mid.planting.floorPerHectare >= 400.0f && mid.planting.floorPerHectare <= 550.0f && mid.spacingShare < 1.0f && mid.height >= 10.0f && mid.height <= 20.0f && mid.forkHeight >= 0.55f * mid.height);
        assert(mid.height - mid.forkHeight < 2.0f * mid.crownRadius);
        assert(mid.height * 1.15f < canopy.forkHeight);
        assert(mid.leafLength == canopy.leafLength && mid.clusterSize == canopy.clusterSize && mid.clusterTexels == canopy.clusterTexels);
    }

    // A mid-layer tree grown: a slender trunk, 20-45 cm across at the foot (Leonardo's at the fork, trunkTaper more).
    void testAMidTreeIsSlender() {
        const TreeMeshes tree = growTreeMeshes(midTreeSettings(), 29u, 1.5f);
        const float footAcross = 2.0f * tree.trunkRadius * (1.0f + trunkTaper);
        assert(footAcross >= 0.2f && footAcross <= 0.45f);
    }

    /*
    * The mid layer is planted after the big trees, under their crowns as much as between them, but its trunks keep
    * midTrunkClearance from theirs.
    */
    void testMidTreesStandUnderTheCanopy() {
        const ForestPlanting& planted = defaultPatchPlanting();
        const ForestTreeSettings canopySettings;
        assert(planted.mid.size() > 900);
        int underACrown = 0;

        for (const TreePlacement& tree : planted.mid) {
            const Vec2 foot{tree.foot.x, tree.foot.z};

            for (const std::vector<TreePlacement>* big : {&planted.canopy, &planted.emergents}) {
                for (const TreePlacement& other : *big) {
                    const float apart = (foot - Vec2{other.foot.x, other.foot.z}).length();
                    assert(apart >= midTrunkClearance - 1e-4f);
                    underACrown += big == &planted.canopy && apart < placedCrownRadius(other, canopySettings) ? 1 : 0;
                }
            }
        }

        assert(underACrown > 20);
    }

    /*
    * An emergent grown: its bark reaching its height (within a little either way: the crown's tips), and no leaves on the
    * clear trunk, none lower than halfway up.
    */
    void testAnEmergentIsBuilt() {
        ForestTreeSettings settings = emergentTreeSettings();
        settings.attractors = 400;
        const TreeMeshes tree = growTreeMeshes(settings, 23u, 1.5f);
        float barkTop = -1e9f;
        float lowestLeaf = 1e9f;

        for (const MeshVertex& vertex : tree.bark.vertices) {
            barkTop = std::max(barkTop, vertex.position.y);
        }

        for (const MeshVertex& vertex : tree.leaves.vertices) {
            lowestLeaf = std::min(lowestLeaf, vertex.position.y);
        }

        assert(barkTop > 0.85f * settings.height && barkTop < 1.05f * settings.height);
        assert(lowestLeaf > 0.5f * settings.height);
    }

    /*
    * The canopy trees are planted after the emergents and keep clear of them: no canopy trunk inside an emergent's crown,
    * as none is inside another canopy tree's. They share the emergents' lattice, taking the thresholds above theirs; each
    * species jitters its trees by its own seed.
    */
    void testCanopyTreesKeepClearOfEmergents() {
        const ForestPlanting& planted = defaultPatchPlanting();
        const ForestTreeSettings emergentSettings = emergentTreeSettings();
        const ForestTreeSettings canopySettings;
        assert(!planted.emergents.empty() && !planted.canopy.empty());
        const std::vector<PlantingLayerRecord> layers = forestPlantingLayers(defaultSpecies());
        const PlantingLayerRecord& canopyLayer = layers[static_cast<std::size_t>(ForestLayer::Canopy)];
        assert(canopyLayer.under == static_cast<int>(ForestLayer::Emergent) && canopyLayer.keepAway.size() == 1 && canopyLayer.keepAway.front().crown);

        for (const TreePlacement& tree : planted.canopy) {
            for (const TreePlacement& emergent : planted.emergents) {
                assert((Vec2{tree.foot.x, tree.foot.z} - Vec2{emergent.foot.x, emergent.foot.z}).length() >= placedCrownRadius(emergent, emergentSettings) - 1e-4f);
            }
        }

        // Alone, the canopy stands elsewhere; the emergents' own seed jitters them apart from the canopy's.
        const std::vector<PlantedPlant> alone = plantAlone(treePlantingLayer(canopySettings, canopyPlaceSeed), patchValleyMaps());
        bool differs = alone.size() != planted.canopy.size();

        for (std::size_t i = 0; i < std::min(alone.size(), planted.canopy.size()) && !differs; ++i) {
            differs = alone[i].foot.x != planted.canopy[i].foot.x;
        }

        assert(differs);
        const std::vector<PlantedPlant> reseeded = plantAlone(treePlantingLayer(emergentSettings, canopyPlaceSeed), patchValleyMaps());
        assert(reseeded.empty() || reseeded.front().foot.x != planted.emergents.front().foot.x);
    }

    /*
    * The game plants them as looks, not entities (ROADMAP phase 18, stage 2c2): each layer's variants a look of its
    * bark, leaves, card and the shadow's stand-ins (one shadows-only model of two parts), then any climbers and
    * epiphytes; the variants' meshes shared; the leaves a double-sided cut-out, the bark not; no tree entity at all.
    */
    void testPlacedTrees(TestForest& forest);

    // A layer's variant's look.
    const PlantedKindLook& lookOf(const ForestGame& game, ForestLayer layer, int variant) {
        const std::vector<PlantedKindLook>& looks = game.getTreeLooks();
        const auto found = std::find_if(looks.begin(), looks.end(), [&](const PlantedKindLook& kind) {
            return kind.layer == static_cast<int>(layer) && kind.kind == variant;
        });
        assert(found != looks.end());
        return *found;
    }

    void testTheGamePlantsThem() {
        TestForest forest;
        World& world = forest.world;
        AssetManager& assets = forest.assets;
        ForestGame& game = forest.game;

        const std::vector<TreePlacement>& placed = game.getTreePlacements();
        const std::vector<TreePlacement>& emergents = game.getEmergentPlacements();
        const std::vector<TreePlacement>& mid = game.getMidPlacements();
        assert(!placed.empty() && !emergents.empty() && !mid.empty());
        const int variants = game.getTreeSettings().variants + game.getEmergentSettings().variants + game.getMidSettings().variants;
        assert(game.getTreeLooks().size() == static_cast<std::size_t>(variants));
        std::set<std::uint32_t> lookMeshes;

        for (const PlantedKindLook& look : game.getTreeLooks()) {
            assert(look.models.size() >= 4 && look.models.size() <= 8);
            const ModelRenderer& bark = look.models[0];
            const ModelRenderer& leaves = look.models[1];
            const ModelRenderer& card = look.models[2];
            const ModelRenderer& shadows = look.models[3];

            // The leaves and bark seen cast no shadow; their stand-ins cast it and are never seen: the shadow leaves and
            // the shadow bark, two parts of one shadows-only model.
            assert(!leaves.castsShadows && !leaves.shadowOnly && !bark.castsShadows && !bark.shadowOnly);
            assert(shadows.castsShadows && shadows.shadowOnly && shadows.parts.size() == 2);
            assert(isCutOut(shadows.parts[0].material) && !isCutOut(shadows.parts[1].material));
            assert(assets.isDrawnOnly(bark.parts.front().mesh) && !isCutOut(bark.parts.front().material));

            // The bark and leaves seen have two coarser levels each, the second the shadow's own stand-in (one mesh kept
            // for both).
            const std::vector<MeshLevel>& barkLevels = assets.getMeshLevels().at(bark.parts.front().mesh.index);
            const std::vector<MeshLevel>& leafLevels = assets.getMeshLevels().at(leaves.parts.front().mesh.index);
            assert(barkLevels.size() == 3 && barkLevels[1].mesh == shadows.parts[1].mesh && !barkLevels[2].mesh.isValid());
            assert(leafLevels.size() == 3 && leafLevels[1].mesh == shadows.parts[0].mesh && !leafLevels[2].mesh.isValid());
            // The first level is the leaves' own first vertices, thinned by the same records; the shadow leaves are not.
            assert(leafLevels[0].sharesVertices && !leafLevels[1].sharesVertices);

            // Past the last level the impostor takes over (scene/impostor.h): its card drawn only from the error the bark and
            // leaves end on, all three chosen by the tree's one box; a cut-out of its baked atlases, casting no shadow, still.
            const MeshLevelOptions& cardOptions = assets.getMeshLevelOptions().at(card.parts.front().mesh.index);
            const MeshLevelOptions& barkOptions = assets.getMeshLevelOptions().at(bark.parts.front().mesh.index);
            assert(cardOptions.hiddenBelowError == barkLevels[2].errorMetres && leafLevels[2].errorMetres == barkLevels[2].errorMetres);
            assert(cardOptions.chooseBounds && barkOptions.chooseBounds && cardOptions.chooseBounds->maximum.y == barkOptions.chooseBounds->maximum.y);
            assert(card.parts.front().material.impostorFrames > 0 && !card.castsShadows && card.sway.kind == SwayKind::Still);

            const Material& leaf = leaves.parts.front().material;
            assert(isCutOut(leaf) && leaf.doubleSided);

            // Light comes through the leaves (docs/research/JUNGLE.md, Light through leaves), and through their card alike.
            const ForestLeafLightSettings leafLight;
            assert(leaf.diffuseTransmission == leafLight.share && leaf.diffuseTransmissionTint.z == leafLight.tintBlue);
            assert(card.parts.front().material.diffuseTransmission == leaf.diffuseTransmission);
            assert(card.parts.front().material.diffuseTransmissionTint.x == leaf.diffuseTransmissionTint.x);

            // Only the stand-ins are shadows-only; the climbers and epiphytes after them cast.
            for (std::size_t model = 0; model < look.models.size(); ++model) {
                assert(look.models[model].shadowOnly == (model == 3));
                assert(model < 4 || look.models[model].castsShadows);

                for (const RenderPart& part : look.models[model].parts) {
                    lookMeshes.insert(part.mesh.index);
                }
            }
        }

        // Every canopy tree's variant has a look.
        for (const TreePlacement& tree : placed) {
            assert(lookOf(game, ForestLayer::Canopy, tree.variant).models[0].parts.front().mesh.isValid());
        }

        // The mid layer's trunks clear of every big tree's.
        for (const TreePlacement& small : mid) {
            for (const TreePlacement& big : emergents) {
                assert((Vec2{small.foot.x, small.foot.z} - Vec2{big.foot.x, big.foot.z}).length() >= midTrunkClearance);
            }
        }

        // No entity draws any of the looks' meshes: the GPU's cells draw the trees.
        world.each<ModelRenderer>([&](Entity, const ModelRenderer& renderer) {
            for (const RenderPart& part : renderer.parts) {
                assert(!lookMeshes.contains(part.mesh.index));
            }
        });

        testPlacedTrees(forest);
    }

    const PlantingSource& plantingSourceOf(World& world) {
        const PlantingSource* found = nullptr;
        world.each<PlantingSource>([&](Entity, PlantingSource& source) {
            found = &source;
        });
        assert(found != nullptr);
        return *found;
    }

    /*
    * The placed trees (ROADMAP phase 18, stages 2b2 to 2c2): from the start the field's looks draw every species'
    * variants over the patch, the shadow's stand-ins among them, swaying as trees; the same looks frame after frame; a
    * change to the sway gives new looks swaying by it.
    */
    void testPlacedTrees(TestForest& forest) {
        World& world = forest.world;
        ForestGame& game = forest.game;
        const auto frame = [&] {
            game.onUpdate(world, Input{}, 1.0f / 60.0f, 1.0f);
        };
        assert(plantingSourceOf(world).looks);
        frame();
        const std::shared_ptr<const PlantingLooks> looks = plantingSourceOf(world).looks;
        assert(looks);
        assert(looks->region.northmost == forestPatch.northmost && looks->region.southmost == forestPatch.southmost &&
               looks->region.spread == forestPatch.spread);
        // The trees' looks, then the understorey's and the palms'.
        assert(looks->kinds.size() == game.getTreeLooks().size() + game.getPlantLooks().size());

        for (std::size_t each = 0; each < looks->kinds.size(); ++each) {
            const PlantedKindLook& kind = looks->kinds[each];
            const bool big = kind.layer == static_cast<int>(ForestLayer::Emergent) || kind.layer == static_cast<int>(ForestLayer::Canopy);

            if (each >= game.getTreeLooks().size()) {
                assert(kind.layer == static_cast<int>(ForestLayer::Understorey) || kind.layer == static_cast<int>(ForestLayer::Palms));
                assert(kind.models[0].sway.kind == SwayKind::Plant);
                continue;
            }

            assert(big || kind.layer == static_cast<int>(ForestLayer::Mid));

            // Bark, leaves, stand-ins, climbers and epiphytes sway as trees, all by the tree's one height; the card holds still.
            const float height = kind.models[0].sway.height;
            assert(height > 1.0f);

            for (std::size_t model = 0; model < kind.models.size(); ++model) {
                assert(model == 2 ? kind.models[model].sway.kind == SwayKind::Still : kind.models[model].sway.kind == SwayKind::Tree);
                assert(model == 2 || kind.models[model].sway.height == height);
            }
        }

        // The first canopy tree's variant: the field's look is the game's.
        const TreePlacement& first = game.getTreePlacements().front();
        const auto look = std::find_if(looks->kinds.begin(), looks->kinds.end(), [&](const PlantedKindLook& kind) {
            return kind.layer == static_cast<int>(ForestLayer::Canopy) && kind.kind == first.variant;
        });
        assert(look != looks->kinds.end());
        assert(look->models[0].parts.front().mesh == lookOf(game, ForestLayer::Canopy, first.variant).models[0].parts.front().mesh);

        // The same looks frame after frame (not rebuilt each frame).
        frame();
        assert(plantingSourceOf(world).looks == looks);

        // A new sway: new looks swaying by it.
        const float bend = game.getSwaySettings().trees.bend;
        game.getSwaySettings().trees.bend = bend * 2.0f;
        frame();
        const std::shared_ptr<const PlantingLooks> swayed = plantingSourceOf(world).looks;
        assert(swayed && swayed != looks);
        const auto swayedLook = std::find_if(swayed->kinds.begin(), swayed->kinds.end(), [&](const PlantedKindLook& kind) {
            return kind.layer == static_cast<int>(ForestLayer::Canopy) && kind.kind == first.variant;
        });
        assert(swayedLook->models[0].sway.bend != look->models[0].sway.bend);
        game.getSwaySettings().trees.bend = bend;

        // The plants' looks sway by the plants' settings.
        const float plantBend = game.getSwaySettings().plants.bend;
        game.getSwaySettings().plants.bend = plantBend * 2.0f;
        frame();
        assert(plantingSourceOf(world).looks->kinds.back().models[0].sway.bend != swayed->kinds.back().models[0].sway.bend);
        game.getSwaySettings().plants.bend = plantBend;
        frame();
        assert(plantingSourceOf(world).looks->kinds.front().models[0].sway.bend == looks->kinds.front().models[0].sway.bend);
    }
}

namespace {
    /*
    * The leaves' light (docs/research/JUNGLE.md, Light through leaves): about as much through as reflected, deeper and yellower
    * (least blue), read at start-up; withLeafLight gives a material that share and tint, and a share of nothing lets
    * nothing through.
    */
    void testTheLeafLight() {
        ForestLeafLightSettings settings;
        assert(settings.share > 0.5f && settings.share <= 1.0f);
        assert(settings.tintBlue < settings.tintRed && settings.tintRed <= settings.tintGreen);

        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 4 && counter.live == 0);

        const Material leaf = withLeafLight(Material{}, settings);
        assert(leaf.diffuseTransmission == settings.share);
        assert(leaf.diffuseTransmissionTint.x == settings.tintRed && leaf.diffuseTransmissionTint.y == settings.tintGreen &&
               leaf.diffuseTransmissionTint.z == settings.tintBlue);

        settings.share = 0.0f;
        assert(withLeafLight(Material{}, settings).diffuseTransmission == 0.0f);
    }
}

void testForestTrees() {
    testTheLeafLight();
    testAttractorsFillTheCrown();
    testATreeGrowsIntoItsCrown();
    testGravityDrooping();
    testVariantShapes();
    testClumps();
    testClumpAttractors();
    testScaffold();
    testLeonardosRule();
    testBranchChains();
    testTubeSides();
    testButtressLobes();
    testLeafyNodes();
    testClumpOf();
    testFoliageNormals();
    testClusterCards();
    testATreeIsBuilt();
    testSightLines();
    testTreesArePlaced();
    testTheStartAmongTheTrees();
    testTheSettings();
    testTheEmergentSettings();
    testAnEmergentIsBuilt();
    testCanopyTreesKeepClearOfEmergents();
    testTheMidLayerSettings();
    testAMidTreeIsSlender();
    testMidTreesStandUnderTheCanopy();
    testTheGamePlantsThem();
}
