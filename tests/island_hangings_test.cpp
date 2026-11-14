#include "games/forest/island_hangings.h"
#include "games/forest/forest_game.h"
#include "games/forest/valley_shapes.h"
#include "games/forest/valley_strata.h"
#include "assets/asset_manager.h"
#include "core/settings_visitor.h"
#include "ecs/world.h"
#include "scene/model_renderer.h"

#include "forest_helpers.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {
    // The length of a strand along its points.
    float strandLength(const HangingStrand& strand) {
        float length = 0.0f;

        for (std::size_t i = 1; i < strand.points.size(); ++i) {
            length += (strand.points[i] - strand.points[i - 1]).length();
        }

        return length;
    }

    // Every triangle of a mesh faces the way its corners' normals say (wound counterclockwise seen from outside).
    bool trianglesFaceTheirNormals(const IndexedMesh& mesh) {
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const MeshVertex& a = mesh.vertices[mesh.indices[i]];
            const MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
            const MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
            const Vec3 facing = (b.position - a.position).cross(c.position - a.position);

            if (facing.lengthSquared() < 1e-12f) {
                continue;
            }

            if (facing.dot(a.normal + b.normal + c.normal) <= 0.0f) {
                return false;
            }
        }

        return true;
    }

    /*
    * A strand hangs from its anchor: it starts there leaning out, turns down under its own weight within a few metres,
    * never climbs, and its points are a step apart over the length asked for. It tapers from its start radius to its
    * tip's. The same seed grows the same strand, another a different one.
    */
    void testAStrandHangsAndTapers() {
        const Vec3 anchor{10.0f, -5.0f, 3.0f};
        const Vec3 outward{1.0f, 0.0f, 0.0f};
        const HangingStrand strand = hangingStrand(anchor, outward, 40.0f, 0.6f, 0.1f, 7u);

        assert(strand.points.size() == strand.radii.size() && strand.points.size() >= 41);
        assert(strand.points.front().x == anchor.x && strand.points.front().y == anchor.y && strand.points.front().z == anchor.z);
        assert(near(strandLength(strand), 40.0f, 0.5f));
        assert(strand.points.back().y < anchor.y - 0.7f * 40.0f);
        assert(strand.points[2].x > anchor.x);

        for (std::size_t i = 1; i < strand.points.size(); ++i) {
            const float step = (strand.points[i] - strand.points[i - 1]).length();
            assert(step <= hangingStepMetres + 1e-3f);
            assert(strand.points[i].y <= strand.points[i - 1].y + 1e-4f);
            assert(strand.radii[i] <= strand.radii[i - 1]);
        }

        // Well below the anchor it hangs nearly straight down.
        const Vec3 low = (strand.points.back() - strand.points[strand.points.size() - 6]).normalized();
        assert(low.y < -0.85f);

        assert(strand.radii.front() == 0.6f && near(strand.radii.back(), 0.1f, 1e-5f));

        const HangingStrand again = hangingStrand(anchor, outward, 40.0f, 0.6f, 0.1f, 7u);
        const HangingStrand other = hangingStrand(anchor, outward, 40.0f, 0.6f, 0.1f, 8u);
        assert(again.points.back().x == strand.points.back().x && again.points.back().z == strand.points.back().z);
        assert(other.points.back().x != strand.points.back().x || other.points.back().z != strand.points.back().z);
    }

    /*
    * The tube round a strand: a ring of sides + 1 vertices at each point (the seam's twice, u running 0 to 1 round it),
    * its normals out from the strand, its triangles facing them, v along it in tiles of tileMetres, the tangent round it
    * (+u) with w +1 so the bitangent runs down the strand (+v).
    */
    void testATubeIsSweptRoundAStrand() {
        const HangingStrand strand = hangingStrand(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 12.0f, 0.5f, 0.1f, 3u);
        IndexedMesh tube;
        appendTube(tube, strand, 8, 2.0f);

        const std::size_t rings = strand.points.size();
        assert(tube.vertices.size() == rings * 9);
        assert(tube.indices.size() == (rings - 1) * 8 * 6);
        assert(trianglesFaceTheirNormals(tube));

        for (std::size_t ring = 0; ring < rings; ++ring) {
            for (int side = 0; side <= 8; ++side) {
                const MeshVertex& vertex = tube.vertices[ring * 9 + static_cast<std::size_t>(side)];
                const Vec3 fromAxis = vertex.position - strand.points[ring];
                assert(near(fromAxis.length(), strand.radii[ring], 1e-3f));
                assert(near(vertex.normal.length(), 1.0f, 1e-4f) && vertex.normal.dot(fromAxis) > 0.0f);
                assert(near(vertex.uv.x, static_cast<float>(side) / 8.0f, 1e-6f));

                const Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
                assert(near(tangent.length(), 1.0f, 1e-4f) && std::abs(tangent.dot(vertex.normal)) < 1e-3f && vertex.tangent.w == 1.0f);
            }
        }

        // v grows along the strand, a tile every two metres, and the bitangent points the same way.
        assert(tube.vertices.front().uv.y == 0.0f && near(tube.vertices.back().uv.y, strandLength(strand) / 2.0f, 1e-3f));
        const MeshVertex& first = tube.vertices[9];
        const Vec3 along = strand.points[2] - strand.points[0];
        const Vec3 bitangent = first.normal.cross(Vec3{first.tangent.x, first.tangent.y, first.tangent.z});
        assert(bitangent.dot(along) > 0.0f);

        // Appending keeps what was there and numbers the new vertices after it.
        IndexedMesh two = tube;
        appendTube(two, strand, 8, 2.0f);
        assert(two.vertices.size() == 2 * tube.vertices.size() && two.indices[tube.indices.size()] == tube.indices[0] + tube.vertices.size());
    }

    /*
    * Leaf cards along a vine: one every spacing metres or so, each a quad of four vertices and two triangles facing its
    * normal, hung by its stem's edge from the vine, its UVs inside one of the atlas's six leaves (two across, three up),
    * its tangent across the leaf (+u) and the bitangent from the stem to the tip (+v).
    */
    void testLeafCardsHangAlongAVine() {
        const HangingStrand vine = hangingStrand(Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 30.0f, 0.06f, 0.03f, 11u);
        IndexedMesh leaves;
        appendLeafCards(leaves, vine, 0.5f, 0.4f, 11u);

        const std::size_t cards = leaves.vertices.size() / 4;
        assert(leaves.vertices.size() == cards * 4 && leaves.indices.size() == cards * 6);
        assert(cards >= 45 && cards <= 75);
        assert(trianglesFaceTheirNormals(leaves));
        std::set<int> cellsUsed;

        for (std::size_t card = 0; card < cards; ++card) {
            const MeshVertex* corner = &leaves.vertices[card * 4];
            const int column = static_cast<int>(std::floor((corner[0].uv.x + corner[2].uv.x) * 0.5f * 2.0f));
            const int row = static_cast<int>(std::floor((corner[0].uv.y + corner[2].uv.y) * 0.5f * 3.0f));
            cellsUsed.insert(row * 2 + column);

            for (int k = 0; k < 4; ++k) {
                assert(corner[k].uv.x >= column * 0.5f - 1e-6f && corner[k].uv.x <= (column + 1) * 0.5f + 1e-6f);
                assert(corner[k].uv.y >= row / 3.0f - 1e-6f && corner[k].uv.y <= (row + 1) / 3.0f + 1e-6f);
                const Vec3 tangent{corner[k].tangent.x, corner[k].tangent.y, corner[k].tangent.z};
                assert(near(tangent.length(), 1.0f, 1e-4f) && corner[k].tangent.w == 1.0f);
            }

            // The stem's edge (v lowest) touches the vine; the tip lies the leaf's length out from it.
            float nearest = 1e9f;
            const Vec3 stem = (corner[0].position + corner[1].position) * 0.5f;

            for (const Vec3& point : vine.points) {
                nearest = std::min(nearest, (point - stem).length());
            }

            assert(nearest < 0.6f);
            const Vec3 tip = (corner[2].position + corner[3].position) * 0.5f;
            assert(near((tip - stem).length(), 0.4f, 1e-3f));
            const Vec3 bitangent = corner[0].normal.cross(Vec3{corner[0].tangent.x, corner[0].tangent.y, corner[0].tangent.z});
            assert(bitangent.dot(tip - stem) > 0.0f);
        }

        assert(cellsUsed.size() == 6);
    }

    RockCarving testCarving() {
        return RockCarving{.bands = valleyStrata(), .warp = strataWarpImage(64), .warpTileMetres = 1500.0f, .warpMetres = 6.0f};
    }

    /*
    * An island's roots and vines: as many as its settings give for its radius, each anchored on its underside (below
    * its rim, at a point of its surface), all hanging below their anchors; the roots and vines in one mesh of bark, the
    * leaves (on the vines only) in another. The same island gives the same meshes.
    */
    void testAnIslandGrowsRootsAndVines() {
        const IslandPlacement island{.rim = Vec3{100.0f, 500.0f, -200.0f}, .radius = 60.0f, .turnRadians = 0.4f, .seed = 21u};
        const SurfaceGrid surface = islandSurface(island, testCarving());
        const ForestHangingSettings settings;
        const IslandHangings hangings = islandHangings(island, surface, settings);

        assert(hangings.rootCount == static_cast<int>(std::lround(settings.rootsPerTenMetres * 6.0f)));
        assert(hangings.vineCount == static_cast<int>(std::lround(settings.vinesPerTenMetres * 6.0f)));
        assert(hangings.anchors.size() == static_cast<std::size_t>(hangings.rootCount + hangings.vineCount));
        assert(!hangings.bark.vertices.empty() && !hangings.leaves.vertices.empty());

        for (const Vec3& anchor : hangings.anchors) {
            assert(anchor.y < -0.02f * island.radius);
            float nearest = 1e9f;

            for (const Vec3& point : surface.points) {
                nearest = std::min(nearest, (point - anchor).length());
            }

            assert(nearest < 1.0f);
        }

        for (const MeshVertex& vertex : hangings.bark.vertices) {
            assert(vertex.position.y < 0.5f);
        }

        const IslandHangings again = islandHangings(island, surface, settings);
        assert(again.bark.vertices.size() == hangings.bark.vertices.size() && again.leaves.vertices.size() == hangings.leaves.vertices.size());
        assert(again.bark.vertices.back().position.x == hangings.bark.vertices.back().position.x);

        ForestHangingSettings none = settings;
        none.rootsPerTenMetres = 0.0f;
        none.vinesPerTenMetres = 0.0f;
        const IslandHangings bare = islandHangings(island, surface, none);
        assert(bare.bark.vertices.empty() && bare.leaves.vertices.empty() && bare.anchors.empty());
    }

    // Counts the fields a visit names, and whether all of them apply at start-up.
    class FieldCounter : public SettingsVisitor {
    public:
        int fields = 0;
        bool allAtStartup = true;

    protected:
        void visitFloat(const char*, float&, const FloatLimits& limits) override {
            ++fields;
            allAtStartup = allAtStartup && limits.applies == FieldApplies::AtStartup;
        }

        void visitInt(const char*, int&, const IntLimits& limits) override {
            ++fields;
            allAtStartup = allAtStartup && limits.applies == FieldApplies::AtStartup;
        }

        void visitBool(const char*, bool&, FieldApplies applies) override {
            ++fields;
            allAtStartup = allAtStartup && applies == FieldApplies::AtStartup;
        }

        void visitChoice(const char*, int&, const char* const*, int, FieldApplies applies) override {
            ++fields;
            allAtStartup = allAtStartup && applies == FieldApplies::AtStartup;
        }

        void visitGroup(const char*, const std::function<void()>& contents) override {
            contents();
        }
    };

    // The settings, each read at start-up (the meshes are built once).
    void testTheSettings() {
        ForestHangingSettings settings;
        FieldCounter counter;
        visit(settings, counter);
        assert(counter.fields == 6 && counter.allAtStartup);
    }

    /*
    * The game hangs them: each island its bark and its leaves, drawn only, placed and turned as its rock; the bark the
    * scanned bark's stand-in without the assets, the leaves a double-sided cut-out.
    */
    void testTheGameHangsThem() {
        TestForest forest{ForestValleySettings{.groundCoarseness = 2, .pillarCount = 10, .islandCount = 4}};
        World& world = forest.world;
        AssetManager& assets = forest.assets;
        ForestGame& game = forest.game;

        const std::vector<Entity>& hung = game.getIslandHangings();
        assert(hung.size() == 2 * game.getIslands().size());

        for (std::size_t island = 0; island < game.getIslands().size(); ++island) {
            const Entity bark = hung[2 * island];
            const Entity leaves = hung[2 * island + 1];
            const Transform& rock = world.get<Transform>(game.getRockShapes()[game.getPillars().size() + island]);
            assert(world.get<Transform>(bark).position.x == rock.position.x && world.get<Transform>(leaves).position.y == rock.position.y);
            assert(assets.isDrawnOnly(world.get<ModelRenderer>(bark).parts.front().mesh));

            const Material& leaf = world.get<ModelRenderer>(leaves).parts.front().material;
            assert(isCutOut(leaf) && leaf.doubleSided);
            assert(!isCutOut(world.get<ModelRenderer>(bark).parts.front().material));
        }
    }
}

void testIslandHangings() {
    testAStrandHangsAndTapers();
    testATubeIsSweptRoundAStrand();
    testLeafCardsHangAlongAVine();
    testAnIslandGrowsRootsAndVines();
    testTheSettings();
    testTheGameHangsThem();
}
