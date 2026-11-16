#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "games/forest/forest_game.h"
#include "games/forest/island_hangings.h"
#include "games/forest/lianas.h"
#include "input/input.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/wind.h"
#include "scene/culling.h"
#include "scene/frame_description.h"
#include "scene/indexed_mesh.h"
#include "scene/wind.h"

#include "forest_helpers.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    // A strand of points straight down from the origin, a metre apart.
    HangingStrand straightDown(int points) {
        HangingStrand strand;

        for (int point = 0; point < points; ++point) {
            strand.points.push_back(Vec3{0.0f, -static_cast<float>(point), 0.0f});
            strand.radii.push_back(0.05f);
        }

        return strand;
    }

    // A quad's worth of vertices appended to a mesh, entries or not.
    void addVertices(IndexedMesh& mesh, int count) {
        for (int vertex = 0; vertex < count; ++vertex) {
            mesh.vertices.push_back(MeshVertex{});
        }
    }

    // The record: anchors in two 16-bit halves, the share and arm as half floats.
    void testThePackedVertex() {
        const std::array<std::uint32_t, 2> packed = packHangingVertex(HangingVertex{3, windAnchorFixed, 0.5f, 2.0f});
        assert((packed[0] & 0xffffu) == 3u && (packed[0] >> 16u) == windAnchorFixed);
        assert(packed[1] == 0x40003800u); // 2.0 high, 0.5 low, as GLSL's packHalf2x16
        assert(windAnchorOwn == WIND_ANCHOR_OWN && windAnchorFixed == WIND_ANCHOR_FIXED);
    }

    /*
    * Entries stay one a vertex: a mesh with none gets none from appends without them; its first hanging append gives its
    * earlier vertices the default (their own sway, no arm); later appends without entries get the default too; and a
    * drawn mesh added to the assets is padded to its last vertex.
    */
    void testTheEntriesStayAligned() {
        IndexedMesh mesh;
        addVertices(mesh, 4);
        appendHanging(mesh, 0, nullptr);
        assert(mesh.hanging.empty());

        const HangingVertex hung{1, 2, 0.25f, 3.0f};
        const std::size_t first = mesh.vertices.size();
        addVertices(mesh, 3);
        appendHanging(mesh, first, &hung);
        assert(mesh.hanging.size() == 7 && mesh.hanging[0] == HangingVertex{} && mesh.hanging[3] == HangingVertex{} && mesh.hanging[4] == hung);

        const std::size_t plain = mesh.vertices.size();
        addVertices(mesh, 2);
        appendHanging(mesh, plain, nullptr);
        assert(mesh.hanging.size() == 9 && mesh.hanging[8] == HangingVertex{});

        addVertices(mesh, 3);
        mesh.indices = {0, 1, 2};
        AssetManager assets;
        const MeshHandle handle = assets.addDrawnMesh(mesh);
        assert(assets.getDrawnMesh(handle).hanging.size() == 12 && assets.getDrawnMesh(handle).hanging[11] == HangingVertex{});
    }

    // Hung from one anchor, each point's arm is its length along from the first; between two, its share along runs 0 to
    // 1 and its arm is its sag below the chord, nothing at the ends and most in a loop's middle.
    void testHangingStrands() {
        HangingStrand down = straightDown(5);
        hangFrom(down, windAnchorFixed);
        assert(down.hanging.size() == 5);

        for (std::size_t point = 0; point < 5; ++point) {
            assert(down.hanging[point].first == windAnchorFixed && down.hanging[point].second == windAnchorFixed);
            assert(near(down.hanging[point].arm, static_cast<float>(point), 1e-5f));
        }

        HangingStrand loop = catenaryStrand(Vec3{0.0f, 30.0f, 0.0f}, Vec3{20.0f, 30.0f, 0.0f}, 24.0f, 0.05f);
        hangBetween(loop, 4, 9);
        const std::size_t last = loop.points.size() - 1;
        assert(loop.hanging.front().along == 0.0f && near(loop.hanging[last].along, 1.0f, 1e-6f));
        assert(near(loop.hanging.front().arm, 0.0f, 1e-4f) && near(loop.hanging[last].arm, 0.0f, 1e-3f));
        float deepest = 0.0f;
        std::size_t deepestPoint = 0;

        for (std::size_t point = 0; point <= last; ++point) {
            assert(loop.hanging[point].first == 4 && loop.hanging[point].second == 9);
            assert(point == 0 || loop.hanging[point].along >= loop.hanging[point - 1].along);

            if (loop.hanging[point].arm > deepest) {
                deepest = loop.hanging[point].arm;
                deepestPoint = point;
            }
        }

        assert(near(deepest, 30.0f - loop.points[deepestPoint].y, 1e-3f) && deepestPoint > last / 4 && deepestPoint < 3 * last / 4);
    }

    // A tube's rings and a vine's leaves carry the entry of the strand where they are built; a strand without entries adds none.
    void testTubesAndLeavesHang() {
        HangingStrand down = straightDown(4);
        IndexedMesh plain;
        appendTube(plain, down, 4, 1.0f);
        assert(plain.hanging.empty());

        hangFrom(down, windAnchorFixed);
        IndexedMesh tube;
        appendTube(tube, down, 4, 1.0f);
        assert(tube.hanging.size() == tube.vertices.size());

        for (std::size_t vertex = 0; vertex < tube.vertices.size(); ++vertex) {
            assert(near(tube.hanging[vertex].arm, -tube.vertices[vertex].position.y, 1e-4f));
        }

        IndexedMesh leaves;
        appendLeafCards(leaves, down, 0.5f, 0.2f, 7u);
        assert(!leaves.vertices.empty() && leaves.hanging.size() == leaves.vertices.size());

        for (const HangingVertex& entry : leaves.hanging) {
            assert(entry.first == windAnchorFixed && entry.arm >= 0.0f && entry.arm <= 3.0f);
        }
    }

    /*
    * An anchor moves exactly as its tree's own vertex at its point does, so what hangs from it stays on: the same
    * offset windSwayOffset gives that vertex of a tree sway of the same height, scale and bend, no flutter.
    */
    void testAnchorsMoveWithTheirTree() {
        const WindAnchor anchor{.foot = Vec3{10.0f, -2.0f, 5.0f}, .height = 20.0f, .scale = 1.5f, .bend = 0.3f, .point = Vec3{14.0f, 22.0f, 6.0f}};
        const Vec2 spring{0.06f, -0.03f};
        const Vec3 moved = windAnchorOffset(packWindAnchor(anchor), spring, 0.8f, 7.0f);
        const Vec3 fromFoot = anchor.point - anchor.foot;
        const Vec4 sway = packSway(WindSway{.kind = SwayKind::Tree, .height = 20.0f, .bend = 0.3f, .flutter = 0.0f});
        const Vec3 vertex = windSwayOffset(fromFoot, fromFoot.y / 1.5f, anchor.point, sway, 1.5f, spring, 0.8f, 7.0f);
        assert(moved.length() > 0.0f && (moved - vertex).length() < 1e-6f);
    }

    /*
    * The swing: nothing without wind or arm; out along the spring's lean times the arm (softened), rising so the vertex
    * stays its arm from where it hangs, as a pendulum; further for a longer arm; and flutter on top within its share.
    */
    void testTheSwing() {
        assert(windSwingOffset(5.0f, Vec3{}, 1.0f, 0.5f, Vec2{0.0f, 0.0f}, 1.0f, 3.0f).length() == 0.0f);
        assert(windSwingOffset(0.0f, Vec3{}, 1.0f, 0.0f, Vec2{0.1f, 0.0f}, 1.0f, 3.0f).length() == 0.0f);
        assert(windSwingOffset(5.0f, Vec3{}, 1.0f, 0.0f, Vec2{0.1f, 0.0f}, 0.0f, 3.0f).length() == 0.0f);

        const Vec2 spring{0.1f, 0.05f};
        const Vec3 swung = windSwingOffset(5.0f, Vec3{}, 0.5f, 0.0f, spring, 1.0f, 3.0f);
        const Vec2 lean = windSoftLean(spring * 0.5f);
        assert(near(swung.x, lean.x * 5.0f, 1e-5f) && near(swung.z, lean.y * 5.0f, 1e-5f) && swung.y > 0.0f);
        const Vec3 bob = Vec3{0.0f, -5.0f, 0.0f} + swung;
        assert(near(bob.length(), 5.0f, 1e-4f));
        assert(windSwingOffset(10.0f, Vec3{}, 0.5f, 0.0f, spring, 1.0f, 3.0f).x > swung.x);

        const Vec3 fluttering = windSwingOffset(5.0f, Vec3{1.0f, 2.0f, 3.0f}, 0.5f, 0.2f, spring, 1.0f, 3.0f) - swung;
        const float most = 0.2f * lean.length() * 1.0001f;
        assert(std::abs(fluttering.x) <= most && std::abs(fluttering.y) <= most && std::abs(fluttering.z) <= most);
    }

    // The whole motion: the anchors' blended by the share along, plus the swing.
    void testTheBlend() {
        const Vec3 first{1.0f, 0.0f, 0.0f};
        const Vec3 second{0.0f, 0.0f, 2.0f};
        const Vec3 swing{0.0f, 0.5f, 0.0f};
        assert((windHangingOffset(first, second, 0.0f, swing) - Vec3{1.0f, 0.5f, 0.0f}).length() < 1e-6f);
        assert((windHangingOffset(first, second, 1.0f, swing) - Vec3{0.0f, 0.5f, 2.0f}).length() < 1e-6f);
        assert((windHangingOffset(first, second, 0.5f, swing) - Vec3{0.5f, 0.5f, 1.0f}).length() < 1e-6f);
    }

    // A draw's mesh's hanging entry and its swing reach its instance; the frame takes the first anchors placed.
    void testTheRecordsReachTheGpu() {
        std::vector<DrawItem> draws(2);
        draws[0].mesh = MeshHandle{1};
        draws[1].mesh = MeshHandle{5};
        attachHangingEntries(draws, {0u, 17u});
        assert(draws[0].hangingEntry == 17u && draws[1].hangingEntry == 0u);

        draws[0].sway = WindSway{.kind = SwayKind::Hanging, .swing = 0.4f};
        const GpuInstance instance = packInstance(draws[0], 0);
        assert(instance.hanging[0] == 17u && instance.hanging[1] == std::bit_cast<std::uint32_t>(0.4f));

        World world;
        const auto anchors = std::make_shared<const std::vector<WindAnchor>>(std::vector<WindAnchor>{WindAnchor{.height = 3.0f}});
        world.add(world.create(), WindAnchors{anchors});
        world.add(world.create(), WindAnchors{std::make_shared<const std::vector<WindAnchor>>()});
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 10.0f};
        assert(buildFrame(world, camera, 0.0f).windAnchors == anchors);
        World empty;
        assert(!buildFrame(empty, camera, 0.0f).windAnchors);

        const GpuWindAnchor packed = packWindAnchor(WindAnchor{.foot = Vec3{1.0f, 2.0f, 3.0f}, .height = 4.0f, .scale = 5.0f, .bend = 6.0f,
                                                               .point = Vec3{7.0f, 8.0f, 9.0f}});
        assert(packed.foot[3] == 4.0f && packed.point[0] == 7.0f && packed.point[3] == 5.0f && packed.bend[0] == 6.0f);
    }

    /*
    * The forest: its lianas and the islands' roots, vines and moss hang (Hanging), every vertex with an entry; each
    * liana's anchors are its trees' (on the air entity, their bend the trees'), or fixed on the ground; the islands'
    * hang from fixed rock; some climbers' roots hang from their tree in mid air. The swing and flutter apply live.
    */
    void testTheForestHangs() {
        TestForest forest;
        const ForestSwaySettings defaults;
        assert(forest.world.has<WindAnchors>(forest.game.getAir()));
        const std::vector<WindAnchor>& anchors = *forest.world.get<WindAnchors>(forest.game.getAir()).anchors;
        assert(!anchors.empty());

        for (const WindAnchor& anchor : anchors) {
            assert(anchor.bend == defaults.trees.bend && anchor.height > 1.0f && anchor.point.y > anchor.foot.y);
        }

        bool fixedEnd = false;

        for (const Entity entity : forest.game.getLianas()) {
            const ModelRenderer& renderer = forest.world.get<ModelRenderer>(entity);
            assert(renderer.sway.kind == SwayKind::Hanging && renderer.sway.swing == defaults.hangingSwing);
            const IndexedMesh& mesh = forest.assets.getDrawnMesh(renderer.parts.front().mesh);
            assert(mesh.hanging.size() == mesh.vertices.size());

            for (const HangingVertex& entry : mesh.hanging) {
                assert(entry.first < anchors.size());
                assert(entry.second < anchors.size() || entry.second == windAnchorFixed);
                fixedEnd = fixedEnd || entry.second == windAnchorFixed;
            }
        }

        assert(fixedEnd);

        for (const Entity entity : forest.game.getIslandHangings()) {
            const ModelRenderer& renderer = forest.world.get<ModelRenderer>(entity);
            assert(renderer.sway.kind == SwayKind::Hanging);
            const IndexedMesh& mesh = forest.assets.getDrawnMesh(renderer.parts.front().mesh);
            assert(mesh.hanging.size() == mesh.vertices.size() && mesh.hanging.front().first == windAnchorFixed);
        }

        for (const Entity entity : forest.game.getMoss()) {
            const IndexedMesh& mesh = forest.assets.getDrawnMesh(forest.world.get<ModelRenderer>(entity).parts.front().mesh);
            assert(forest.world.get<ModelRenderer>(entity).sway.kind == SwayKind::Hanging && mesh.hanging.size() == mesh.vertices.size());
        }

        bool rootInMidAir = false;

        for (const Entity entity : forest.game.getClimbers()) {
            const IndexedMesh& mesh = forest.assets.getDrawnMesh(forest.world.get<ModelRenderer>(entity).parts.front().mesh);

            for (const HangingVertex& entry : mesh.hanging) {
                assert(entry.first == windAnchorOwn);
                rootInMidAir = rootInMidAir || entry.arm > 0.5f;
            }
        }

        assert(rootInMidAir);

        Input input;
        forest.game.getSwaySettings().hangingSwing = 0.7f;
        forest.game.getSwaySettings().trees.bend = 0.2f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(forest.world.get<ModelRenderer>(forest.game.getLianas().front()).sway.swing == 0.7f);
        assert(forest.world.get<WindAnchors>(forest.game.getAir()).anchors->front().bend == 0.2f);
    }
}

void testWindHanging() {
    testThePackedVertex();
    testTheEntriesStayAligned();
    testHangingStrands();
    testTubesAndLeavesHang();
    testAnchorsMoveWithTheirTree();
    testTheSwing();
    testTheBlend();
    testTheRecordsReachTheGpu();
    testTheForestHangs();
}
