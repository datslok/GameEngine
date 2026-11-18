#include "core/command_registry.h"
#include "games/forest/epiphytes.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_trees.h"
#include "games/forest/palms.h"
#include "math/angles.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"
#include "test_jobs.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    /*
    * A small tree's skeleton: a trunk up to a fork at 10 m (radius 0.5), one level limb 8 m out along +x (radius 0.3
    * falling to 0.2), a steep limb up along -x (0.3) and a thin level twig off the level limb's end (0.04). Only the
    * fork has two children, so it is the one crotch.
    */
    TreeSkeleton testSkeleton() {
        TreeSkeleton skeleton;
        std::vector<TreeNode>& nodes = skeleton.nodes;
        std::vector<float>& radii = skeleton.radii;

        // The trunk: nodes 0-10, one a metre, from the ground to the fork.
        for (int point = 0; point <= 10; ++point) {
            nodes.push_back(TreeNode{Vec3{0.0f, static_cast<float>(point), 0.0f}, point - 1});
            radii.push_back(0.5f);
        }

        // The level limb: nodes 11-18 out along +x, rising a little.
        int parent = 10;

        for (int point = 1; point <= 8; ++point) {
            nodes.push_back(TreeNode{Vec3{static_cast<float>(point), 10.0f + 0.1f * static_cast<float>(point), 0.0f}, parent});
            radii.push_back(0.3f - 0.0125f * static_cast<float>(point));
            parent = static_cast<int>(nodes.size()) - 1;
        }

        // The steep limb: nodes 19-24 up and out along -x.
        parent = 10;

        for (int point = 1; point <= 6; ++point) {
            nodes.push_back(TreeNode{Vec3{-0.3f * static_cast<float>(point), 10.0f + static_cast<float>(point), 0.0f}, parent});
            radii.push_back(0.3f);
            parent = static_cast<int>(nodes.size()) - 1;
        }

        // The thin twig off the level limb's end: nodes 25-28, level along +z.
        parent = 18;

        for (int point = 1; point <= 4; ++point) {
            nodes.push_back(TreeNode{Vec3{8.0f, 10.8f, static_cast<float>(point)}, parent});
            radii.push_back(0.04f);
            parent = static_cast<int>(nodes.size()) - 1;
        }

        skeleton.chains = branchChains(nodes, radii);
        skeleton.trunkRadius = 0.5f;
        return skeleton;
    }

    /*
    * The places a rosette may sit: in each crotch thick enough (the fork, here) and on top of each thick, nearly level
    * limb's segments, never on the steep limb, the trunk or the thin twig; crotches first. A limb's site sits on its
    * top, its radius above its centre line.
    */
    void testTheSites() {
        const TreeSkeleton skeleton = testSkeleton();
        const ForestEpiphyteSettings settings;
        const std::vector<EpiphyteSite> sites = epiphyteSites(skeleton, settings);
        assert(!sites.empty());
        assert(sites.front().kind == EpiphyteSiteKind::Crotch);
        assert(near(sites.front().base.x, 0.0f, 1e-4f) && sites.front().base.y >= 10.0f);

        bool sawLimb = false;

        for (std::size_t index = 0; index < sites.size(); ++index) {
            const EpiphyteSite& site = sites[index];

            if (site.kind == EpiphyteSiteKind::Limb) {
                sawLimb = true;
                // On the level limb (x 0 to 8, z 0), above its centre line by its radius.
                assert(site.base.x > 0.0f && site.base.x < 8.0f + 1e-4f && near(site.base.z, 0.0f, 1e-4f));
                const float centreY = 10.0f + 0.1f * site.base.x;
                assert(near(site.base.y - centreY, site.limbRadius, 0.02f));
                assert(site.limbUp.y > 0.9f);
            } else {
                // Crotches come before every limb site.
                assert(!sawLimb);
            }

            // Never on the trunk below the fork, the steep limb or the twig.
            assert(site.base.y > 9.9f && site.base.x > -0.1f && site.base.z < 0.5f);
        }

        assert(sawLimb);
    }

    /*
    * The rosettes chosen: as many as asked when there is room, at least spacing apart, crotches first, the same for a
    * seed; how many a tree carries: least to most on a big tree, at most mostPerSmallTree on a small one.
    */
    void testChoosingSites() {
        const TreeSkeleton skeleton = testSkeleton();
        ForestEpiphyteSettings settings;
        settings.spacing = 1.0f;
        const std::vector<EpiphyteSite> candidates = epiphyteSites(skeleton, settings);
        const std::vector<EpiphyteSite> chosen = chooseEpiphyteSites(candidates, 4, settings.spacing, 7);
        assert(chosen.size() == 4);
        assert(chosen.front().kind == EpiphyteSiteKind::Crotch);

        for (std::size_t a = 0; a < chosen.size(); ++a) {
            for (std::size_t b = a + 1; b < chosen.size(); ++b) {
                assert((chosen[a].base - chosen[b].base).length() >= settings.spacing - 1e-4f);
            }
        }

        // Asking for more than fit gives fewer, never two too close.
        const std::vector<EpiphyteSite> crowded = chooseEpiphyteSites(candidates, 1000, settings.spacing, 7);
        assert(crowded.size() < candidates.size() && crowded.size() >= 4);

        const std::vector<EpiphyteSite> again = chooseEpiphyteSites(candidates, 4, settings.spacing, 7);

        for (std::size_t index = 0; index < chosen.size(); ++index) {
            assert(again[index].base.x == chosen[index].base.x && again[index].base.y == chosen[index].base.y);
        }

        for (std::uint32_t seed = 1; seed < 200; ++seed) {
            const int big = epiphytesOnTree(settings, true, seed);
            const int small = epiphytesOnTree(settings, false, seed);
            assert(big >= settings.leastPerBigTree && big <= settings.mostPerBigTree);
            assert(small >= 0 && small <= settings.mostPerSmallTree);
        }
    }

    /*
    * A rosette: its leaves from round its base, rising (a bird's-nest fern's funnel at least a third of its length tall),
    * none reaching farther than its length, its leaf count as asked; a bromeliad smaller; triangles facing their normals;
    * the same for a seed.
    */
    void testARosette() {
        const ForestEpiphyteSettings settings;
        const Vec3 base{2.0f, 10.0f, -1.0f};

        for (const EpiphyteKind kind : {EpiphyteKind::Fern, EpiphyteKind::Bromeliad}) {
            const RosetteShape shape = rosetteShape(settings, kind, 11);
            const float nominal = kind == EpiphyteKind::Fern ? settings.fernLength : settings.bromeliadLength;
            assert(shape.length >= 0.5f * nominal - 1e-4f && shape.length <= 1.5f * nominal + 1e-4f);
            assert(shape.leaves == (kind == EpiphyteKind::Fern ? settings.fernFronds : settings.bromeliadLeaves));

            IndexedMesh mesh;
            appendRosette(mesh, base, shape);
            assert(!mesh.vertices.empty() && mesh.indices.size() % 3 == 0);
            assert(trianglesFaceTheirNormals(mesh));
            float highest = -1e9f;

            for (const MeshVertex& vertex : mesh.vertices) {
                const Vec3 offset = vertex.position - base;
                assert(offset.length() <= shape.length * 1.05f);
                highest = std::max(highest, offset.y);
            }

            assert(highest > shape.length / 3.0f);

            // Each kind reads its own half of the leaves' atlas: the fern the left, the bromeliad the right.
            for (const MeshVertex& vertex : mesh.vertices) {
                const float least = kind == EpiphyteKind::Fern ? 0.0f : 0.5f;
                assert(vertex.uv.x >= least - 1e-6f && vertex.uv.x <= least + 0.5f + 1e-6f);
            }

            IndexedMesh again;
            appendRosette(again, base, shape);
            assert(again.vertices.size() == mesh.vertices.size() && again.vertices.back().position.x == mesh.vertices.back().position.x);
        }

        assert(settings.bromeliadLength < settings.fernLength);
    }

    /*
    * Moss on the limbs: a shell over the top of each thick, nearly level limb only (every normal facing up, outside the
    * limb by its thickness), none on the trunk, the steep limb or the thin twig; triangles facing out.
    */
    void testLimbMoss() {
        const TreeSkeleton skeleton = testSkeleton();
        const ForestEpiphyteSettings settings;
        const IndexedMesh moss = limbMoss(skeleton, settings, 0.5f);
        assert(!moss.vertices.empty());
        assert(trianglesFaceTheirNormals(moss));

        for (const MeshVertex& vertex : moss.vertices) {
            assert(vertex.normal.y > 0.1f);
            // On the level limb: x 0 to 8, outside its centre line (y 10 + 0.1 x) by at least its thinnest radius.
            assert(vertex.position.x > -0.5f && vertex.position.x < 8.5f && std::abs(vertex.position.z) < 0.5f);
            const Vec3 centre{vertex.position.x, 10.0f + 0.1f * vertex.position.x, 0.0f};
            assert((vertex.position - centre).length() > 0.19f);
        }

        ForestEpiphyteSettings without = settings;
        without.moss = false;
        assert(limbMoss(skeleton, without, 0.5f).vertices.empty());
    }

    /*
    * The leaves' atlas: twice as wide as tall, opaque; its left half the fern's frond (a dark midrib down its middle, a
    * brighter green blade), its right half the palms' leaflet picture in the bromeliads' colour; the same every run.
    */
    void testTheLeavesAtlas() {
        constexpr int texels = 64;
        const EpiphyteLeafImages atlas = epiphyteLeafImages(texels, Pixel{60, 100, 45}, 5);
        assert(atlas.colour.width == 2 * texels && atlas.colour.height == texels);
        assert(atlas.height.width == 2 * texels && atlas.height.height == texels);
        const auto texel = [&](std::uint32_t x, std::uint32_t y, int channel) {
            return atlas.colour.pixels[(static_cast<std::size_t>(y) * atlas.colour.width + x) * 4 + static_cast<std::size_t>(channel)];
        };

        for (std::size_t alpha = 3; alpha < atlas.colour.pixels.size(); alpha += 4) {
            assert(atlas.colour.pixels[alpha] == 255);
        }

        // The fern's midrib (u 0.5 of its half) darker than its blade a quarter of the way out.
        assert(texel(32, 30, 1) < texel(16, 30, 1));

        // The right half: the palms' leaflet picture, drawn the same.
        const PalmLeafletImages palm = palmLeafletImages(texels, bromeliadColour(Pixel{60, 100, 45}), 5);

        for (std::uint32_t y = 0; y < texels; y += 7) {
            for (std::uint32_t x = 0; x < texels; x += 5) {
                for (int channel = 0; channel < 3; ++channel) {
                    assert(texel(texels + x, y, channel) == palm.colour.pixels[(static_cast<std::size_t>(y) * texels + x) * 4 + static_cast<std::size_t>(channel)]);
                }
            }
        }

        const EpiphyteLeafImages again = epiphyteLeafImages(texels, Pixel{60, 100, 45}, 5);
        assert(again.colour.pixels == atlas.colour.pixels && again.height.pixels == atlas.height.pixels);
    }

    // A grown tree keeps its skeleton: its nodes, their radii (the trunk's tapered) and its chains, the trunk's first.
    void testTreesKeepTheirSkeleton() {
        ForestTreeSettings settings;
        settings.attractors = 200;
        const TreeMeshes tree = growTreeMeshes(settings, 4, 1.5f);
        const TreeSkeleton& skeleton = tree.skeleton;
        assert(skeleton.nodes.size() > 10 && skeleton.radii.size() == skeleton.nodes.size());
        assert(!skeleton.chains.empty() && skeleton.chains.front().front() == 0);
        assert(skeleton.radii.front() > skeleton.trunkRadius && near(skeleton.trunkRadius, tree.trunkRadius, 1e-6f));

        // A big tree's epiphytes: rosettes and moss, as many as chosen, on it.
        const ForestEpiphyteSettings epiphytes;
        const EpiphyteMeshes grown = growEpiphytes(skeleton, epiphytes, true, 3);
        assert(!grown.leaves.vertices.empty() && !grown.moss.vertices.empty());
        assert(grown.ferns + grown.bromeliads == static_cast<int>(grown.sites.size()) && grown.ferns > 0);
        assert(grown.sites.size() <= static_cast<std::size_t>(epiphytes.mostPerBigTree));

        // A small tree's: bromeliads only.
        const EpiphyteMeshes small = growEpiphytes(skeleton, epiphytes, false, 3);
        assert(small.ferns == 0 && small.sites.size() <= static_cast<std::size_t>(epiphytes.mostPerSmallTree));
    }

    // The forest's trees carry them: still, drawn only, into the nearest cascades; their settings start-up only.
    void testTheForestGrowsThem() {
        TestForest forest;
        const std::vector<Entity>& epiphytes = forest.game.getEpiphytes();
        assert(!epiphytes.empty());

        for (const Entity entity : epiphytes) {
            const ModelRenderer& renderer = forest.world.get<ModelRenderer>(entity);
            assert(renderer.isStill() && renderer.shadowCascades == epiphyteShadowCascades);
            assert(forest.assets.isDrawnOnly(renderer.parts.front().mesh));
        }

        FieldCounter counter;
        visit(forest.game.getEpiphyteSettings(), counter);
        assert(counter.fields == 16 && counter.live == 0);

        // forest-epiphyte places the camera in the air beside a tree's first rosette, looking at it; none past the last.
        CommandRegistry registry;
        forest.game.addCommands(registry, forest.world);
        const std::vector<Vec3>& rosettes = forest.game.getEpiphyteViews();
        assert(!rosettes.empty());
        assert(registry.execute("forest-epiphyte").ok && registry.execute("forest-epiphyte 0").ok);
        const Vec3 eye = forest.game.getCamera().getPosition();
        const float apart = (eye - rosettes.front()).length();
        assert(apart > 3.0f && apart < 15.0f && eye.y > rosettes.front().y);
        assert(!registry.execute("forest-epiphyte " + std::to_string(rosettes.size())).ok);
        assert(!registry.execute("forest-epiphyte many").ok);
    }
}

void testEpiphytes() {
    testTheSites();
    testChoosingSites();
    testARosette();
    testLimbMoss();
    testTheLeavesAtlas();
    testTreesKeepTheirSkeleton();
    testTheForestGrowsThem();
}
