#include "games/forest/forest_trees.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/island_hangings.h"
#include "games/forest/valley_shapes.h"
#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "math/angles.h"
#include "math/closest_points.h"
#include "scene/model_renderer.h"

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
            sums[static_cast<std::size_t>(nodes[i].parent)] += std::pow(static_cast<double>(grown[i]), 2.0);
        }

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (grownTips[i]) {
                ++tipCount;
                assert(grown[i] == 0.02f);
            } else {
                assert(std::fabs(std::pow(static_cast<double>(grown[i]), 2.0) - sums[i]) <= 1e-4 * sums[i]);
            }
        }

        assert(tipCount > 20 && near(grown[0], 0.02f * std::sqrt(static_cast<float>(tipCount)), 1e-3f * grown[0]));
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
    * Leaves at the tips: leavesPerTip cards each, every card's front facing out of the crown, its corners' normals
    * leaning further out, its picture one of the atlas's nine leaves, its stem near a tip.
    */
    void testLeavesFaceOutOfTheCrown() {
        const std::vector<TreeNode> nodes = smallTree(5u);
        const std::vector<bool> tips = treeTips(nodes);
        const std::size_t tipCount = static_cast<std::size_t>(std::count(tips.begin(), tips.end(), true));
        IndexedMesh leaves;
        appendTreeLeaves(leaves, nodes, smallCrown, 3, 0.7f, 4u);
        const std::size_t cards = tipCount * 3;
        assert(leaves.vertices.size() == cards * 4 && leaves.indices.size() == cards * 6);
        assert(trianglesFaceTheirNormals(leaves));
        std::set<int> cellsUsed;

        for (std::size_t card = 0; card < cards; ++card) {
            const MeshVertex* corner = &leaves.vertices[card * 4];
            const Vec3 stem = (corner[0].position + corner[1].position) * 0.5f;
            const Vec3 facing = (corner[1].position - corner[0].position).cross(corner[2].position - corner[0].position).normalized();
            const Vec3 out = crownOutward(smallCrown, stem);
            assert(facing.dot(out) > 0.0f);
            assert(near((corner[3].position - corner[0].position).length(), 0.7f, 1e-4f));

            const int column = static_cast<int>(std::floor((corner[0].uv.x + corner[2].uv.x) * 0.5f * treeLeafColumns));
            const int row = static_cast<int>(std::floor((corner[0].uv.y + corner[2].uv.y) * 0.5f * treeLeafRows));
            cellsUsed.insert(row * treeLeafColumns + column);

            for (int k = 0; k < 4; ++k) {
                assert(near(corner[k].normal.length(), 1.0f, 1e-4f) && corner[k].normal.dot(out) >= facing.dot(out) - 1e-4f);
                const Vec3 tangent{corner[k].tangent.x, corner[k].tangent.y, corner[k].tangent.z};
                assert(near(tangent.length(), 1.0f, 1e-4f) && std::fabs(tangent.dot(corner[k].normal)) < 1e-3f);
                assert(corner[k].uv.x >= column / 3.0f - 1e-6f && corner[k].uv.x <= (column + 1) / 3.0f + 1e-6f);
                assert(corner[k].uv.y >= row / 3.0f - 1e-6f && corner[k].uv.y <= (row + 1) / 3.0f + 1e-6f);
            }

            float nearest = 1e9f;

            for (std::size_t node = 0; node < nodes.size(); ++node) {
                if (tips[node]) {
                    nearest = std::min(nearest, (nodes[node].position - stem).length());
                }
            }

            assert(nearest < 0.5f * 0.7f);
        }

        assert(cellsUsed.size() >= 7);
    }

    ForestTreeSettings smallSettings() {
        ForestTreeSettings settings;
        settings.crownHeight = 12.0f;
        settings.crownRadius = 5.0f;
        settings.crownHalfHeight = 2.0f;
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
        assert(crown.centre.x == 0.0f && crown.centre.y == 12.0f && crown.radius == 5.0f && crown.halfHeight == 2.0f);

        const TreeMeshes tree = growTreeMeshes(settings, 21u, 1.5f);
        assert(tree.tipCount > 20 && !tree.bark.vertices.empty());
        assert(tree.leaves.vertices.size() == static_cast<std::size_t>(tree.tipCount * settings.leavesPerTip * 4));
        assert(near(tree.trunkRadius, settings.twigRadius * std::sqrt(static_cast<float>(tree.tipCount)), 1e-3f * tree.trunkRadius));
        assert(trianglesFaceTheirNormals(tree.bark));

        float lowest = 1e9f;
        float widestAtGround = 0.0f;

        for (const MeshVertex& vertex : tree.bark.vertices) {
            lowest = std::min(lowest, vertex.position.y);

            if (std::fabs(vertex.position.y) < 0.26f) {
                widestAtGround = std::max(widestAtGround, horizontal(vertex.position));
            }
        }

        assert(near(lowest, -treeSinkMetres, 1e-3f));
        assert(widestAtGround > tree.trunkRadius * (1.0f + 0.5f * settings.buttress));

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
    * The emergents' places: within their stretch of the floor, clear of the path, each other and the pillars, sized
    * and turned at random, standing on the lowest ground round the trunk, blocking no sight line, the same every run;
    * where every place would block a line, none is planted.
    */
    void testTreesArePlaced() {
        const ForestTreeSettings settings;
        const std::vector<PillarPlacement> pillars = placePillars(ValleyLayout{});
        const std::vector<SightLine> lines = forestSightLines();
        const std::vector<TreePlacement> trees = placeTrees(settings, pillars, lines);
        assert(trees.size() >= 8 && trees.size() <= static_cast<std::size_t>(settings.count));
        std::set<int> variants;

        for (std::size_t i = 0; i < trees.size(); ++i) {
            const TreePlacement& tree = trees[i];
            assert(tree.foot.z >= treeNorthmost && tree.foot.z <= treeSouthmost);
            assert(std::fabs(tree.foot.x - valleyAxis(tree.foot.z)) <= treeSpread);
            assert(forestPathDistance(tree.foot.x, tree.foot.z) >= treePathGap);
            assert(tree.scale >= 0.85f && tree.scale <= 1.15f && tree.variant >= 0 && tree.variant < settings.variants);
            assert(tree.foot.y <= forestHeight(tree.foot.x, tree.foot.z) + 1e-4f && tree.foot.y > forestHeight(tree.foot.x, tree.foot.z) - 2.0f);
            variants.insert(tree.variant);

            const float radius = placedCrownRadius(tree, settings);
            assert(near(radius, settings.crownRadius * tree.scale, 1e-4f));
            assert(near(placedCrownCentre(tree, settings).y, tree.foot.y + settings.crownHeight * tree.scale, 1e-3f));
            assert(!blocksSight(placedCrownCentre(tree, settings), radius, lines));

            for (std::size_t j = 0; j < i; ++j) {
                const float apart = (Vec2{tree.foot.x, tree.foot.z} - Vec2{trees[j].foot.x, trees[j].foot.z}).length();
                assert(apart >= std::max(radius, placedCrownRadius(trees[j], settings)));
            }

            for (const PillarPlacement& pillar : pillars) {
                assert((Vec2{tree.foot.x, tree.foot.z} - Vec2{pillar.foot.x, pillar.foot.z}).length() >= 1.3f * pillar.radius + radius);
            }
        }

        assert(variants.size() >= 2);
        const std::vector<TreePlacement> again = placeTrees(settings, pillars, lines);
        assert(again.size() == trees.size() && again.back().foot.x == trees.back().foot.x && again.back().turnRadians == trees.back().turnRadians);

        // A wall of sight lines at crown height across the whole stretch leaves no room.
        std::vector<SightLine> wall;

        for (float z = treeNorthmost - 20.0f; z <= treeSouthmost + 20.0f; z += 4.0f) {
            wall.push_back(SightLine{Vec3{-400.0f, settings.crownHeight, z}, Vec3{400.0f, settings.crownHeight, z}});
        }

        assert(placeTrees(settings, pillars, wall).empty());
    }

    void testTheSettings() {
        const ForestTreeSettings settings;
        assert(settings.count == 12 && settings.variants == 4 && settings.crownRadius == 14.0f && settings.leavesPerTip == 3);
        ForestTreeSettings visited;
        FieldCounter counter;
        visit(visited, counter);
        assert(counter.fields == 10 && counter.live == 0);
    }

    /*
    * The game plants them: each tree its bark and its leaves, placed, turned and scaled as its placement says, drawn
    * only, the variants' meshes shared; the leaves a double-sided cut-out, the bark not.
    */
    void testTheGamePlantsThem() {
        TestForest forest;
        World& world = forest.world;
        AssetManager& assets = forest.assets;
        ForestGame& game = forest.game;

        const std::vector<TreePlacement>& placed = game.getTreePlacements();
        const std::vector<Entity>& trees = game.getTrees();
        assert(!placed.empty() && trees.size() == 2 * placed.size());
        std::set<std::uint32_t> barkMeshes;

        for (std::size_t tree = 0; tree < placed.size(); ++tree) {
            const Entity bark = trees[2 * tree];
            const Entity leaves = trees[2 * tree + 1];
            const Transform& pose = world.get<Transform>(bark);
            assert(pose.position.x == placed[tree].foot.x && pose.position.y == placed[tree].foot.y && pose.scale.x == placed[tree].scale);
            assert(world.get<Transform>(leaves).position.z == placed[tree].foot.z && world.get<Transform>(leaves).scale.y == placed[tree].scale);

            const ModelRenderer& barkRenderer = world.get<ModelRenderer>(bark);
            assert(assets.isDrawnOnly(barkRenderer.parts.front().mesh) && !isCutOut(barkRenderer.parts.front().material));
            barkMeshes.insert(barkRenderer.parts.front().mesh.index);

            const Material& leaf = world.get<ModelRenderer>(leaves).parts.front().material;
            assert(isCutOut(leaf) && leaf.doubleSided);
        }

        assert(barkMeshes.size() <= static_cast<std::size_t>(game.getTreeSettings().variants));
    }
}

void testForestTrees() {
    testAttractorsFillTheCrown();
    testATreeGrowsIntoItsCrown();
    testLeonardosRule();
    testBranchChains();
    testTubeSides();
    testButtressLobes();
    testLeavesFaceOutOfTheCrown();
    testATreeIsBuilt();
    testSightLines();
    testTreesArePlaced();
    testTheSettings();
    testTheGamePlantsThem();
}
