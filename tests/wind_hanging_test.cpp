#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "games/forest/forest_game.h"
#include "games/forest/hanging_moss.h"
#include "games/forest/island_hangings.h"
#include "games/forest/lianas.h"
#include "input/input.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/ray_tracing.h"
#include "render/gpu/wind.h"
#include "scene/culling.h"
#include "scene/frame_description.h"
#include "scene/indexed_mesh.h"
#include "scene/liana_shape.h"
#include "scene/wind.h"

#include "forest_helpers.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    // Leaves' flutter rate: 3 Hz, its phase changing every half metre.
    const Vec2 leafRate{3.0f, 0.5f};

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

    // The record: anchors in two 16-bit halves, the share and arm as half floats, the swing's length as a half float.
    void testThePackedVertex() {
        const std::array<std::uint32_t, 3> packed = packHangingVertex(HangingVertex{3, windAnchorFixed, 0.5f, 2.0f, 4.0f});
        assert((packed[0] & 0xffffu) == 3u && (packed[0] >> 16u) == windAnchorFixed);
        assert(packed[1] == 0x40003800u); // 2.0 high, 0.5 low, as GLSL's packHalf2x16
        assert(packed[2] == 0x00004400u); // 4.0 low
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

    /*
    * Hung from one anchor, each point's arm is its length along from the first and its swing's length the strand's
    * whole length; between two, its share along runs 0 to 1, its arm is its sag below the chord, nothing at the ends and
    * most in a loop's middle, and its swing's length the deepest sag (a sagging rope swings about its chord).
    */
    void testHangingStrands() {
        HangingStrand down = straightDown(5);
        hangFrom(down, windAnchorFixed);
        assert(down.hanging.size() == 5);

        for (std::size_t point = 0; point < 5; ++point) {
            assert(down.hanging[point].first == windAnchorFixed && down.hanging[point].second == windAnchorFixed);
            assert(near(down.hanging[point].arm, static_cast<float>(point), 1e-5f));
            assert(near(down.hanging[point].swingLength, 4.0f, 1e-5f));
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

        for (const HangingVertex& entry : loop.hanging) {
            assert(entry.swingLength == deepest);
        }
    }

    // A moss clump swings as long as its foot hangs: the arm above its top and its own length.
    void testMossSwingsItsLength() {
        IndexedMesh moss;
        appendMossClump(moss, Vec3{0.0f, 10.0f, 0.0f}, 2.0f, 0.3f, 6.0f);
        assert(!moss.hanging.empty() && moss.hanging.size() == moss.vertices.size());

        for (const HangingVertex& entry : moss.hanging) {
            assert(near(entry.swingLength, 8.0f, 1e-5f));
        }
    }

    // A hanging chain's slowest swing: 1 / 5.2255 sqrt(g / L) Hz, about 0.6 Hz for a metre, half as fast four times as long.
    void testTheChainsFrequency() {
        assert(near(windChainHertz(1.0f), std::sqrt(9.81f) / 5.2255f, 1e-5f));
        assert(near(windChainHertz(4.0f), 0.5f * windChainHertz(1.0f), 1e-6f));
        assert(windChainHertz(0.0f) > windChainHertz(0.1f) && std::isfinite(windChainHertz(0.0f)));
    }

    // The length of chain that swings at a frequency: g (1 / (5.2255 f))^2.
    float swingingAt(float hertz) {
        const float inverse = 1.0f / (5.2255f * hertz);
        return 9.81f * inverse * inverse;
    }

    /*
    * Each strand's share of the three hanging springs (short, medium, long; their hertz falling): all on a band whose
    * hertz is its own, two neighbouring bands between them blended by log frequency (never the third), the shortest
    * band past its hertz and the longest below; summing to 1, the long band's share growing with the length. The
    * defaults: a metre's root mostly short, a 9 m vine mostly medium, a 50 m vine mostly long.
    */
    void testTheBandShares() {
        const Vec3 bands{0.6f, 0.2f, 0.08f};
        const Vec3 onMedium = windChainShares(swingingAt(0.2f), bands);
        assert(near(onMedium.y, 1.0f, 1e-4f) && near(onMedium.x, 0.0f, 1e-4f) && near(onMedium.z, 0.0f, 1e-4f));
        const Vec3 shortest = windChainShares(0.1f, bands);
        assert(shortest.x == 1.0f && shortest.y == 0.0f && shortest.z == 0.0f);
        const Vec3 longest = windChainShares(500.0f, bands);
        assert(longest.z == 1.0f && longest.x == 0.0f && longest.y == 0.0f);

        // Half way between the short and medium bands in log frequency: half each.
        const Vec3 between = windChainShares(swingingAt(std::sqrt(0.6f * 0.2f)), bands);
        assert(near(between.x, 0.5f, 1e-3f) && near(between.y, 0.5f, 1e-3f) && between.z == 0.0f);

        float lastLong = 0.0f;
        float lastShort = 1.0f;

        for (float length = 0.2f; length < 200.0f; length *= 1.1f) {
            const Vec3 shares = windChainShares(length, bands);
            assert(near(shares.x + shares.y + shares.z, 1.0f, 1e-5f));
            assert(shares.x >= 0.0f && shares.y >= 0.0f && shares.z >= 0.0f);
            assert(shares.x == 0.0f || shares.z == 0.0f);
            assert(shares.z >= lastLong - 1e-6f && shares.x <= lastShort + 1e-6f);
            lastLong = shares.z;
            lastShort = shares.x;
        }

        const WindSettings settings;
        const Vec3 defaults = windHangingBands(settings);
        assert(defaults.x > defaults.y && defaults.y > defaults.z);
        assert(windChainShares(1.0f, defaults).x > 0.8f);
        assert(windChainShares(9.0f, defaults).y > 0.8f);
        assert(windChainShares(50.0f, defaults).z > 0.8f);
    }

    // The field holds the three hanging springs after the plants' and grass's, each with its own frequency; their
    // frequencies must fall from the short to the long.
    void testTheHangingSprings() {
        const WindSettings settings;
        static_assert(WIND_SPRINGS == WIND_CATEGORIES + WIND_HANGING_BANDS - 1 && WIND_SPRING_HANGING_SHORT == WIND_CATEGORY_HANGING);
        const Vec3 bands = windHangingBands(settings);
        assert(bands.x == settings.springs[WIND_SPRING_HANGING_SHORT].frequency && bands.z == settings.springs[WIND_SPRING_HANGING_SHORT + 2].frequency);
        Wind wind;
        const std::array<WindWindow, WIND_CASCADES> windows{};
        const GpuWindField field = packWindField(&wind, settings, windows, windows, true, 1.0f, 0.1f);
        assert(field.springs[WIND_SPRING_HANGING_SHORT + 2][0] == bands.z);
        assert(field.springs[WIND_SPRING_HANGING_SHORT + 1][1] == settings.springs[WIND_SPRING_HANGING_SHORT + 1].damping);

        WindSettings backwards = settings;
        backwards.springs[WIND_SPRING_HANGING_SHORT + 2].frequency = 1.0f;
        bool threw = false;

        try {
            validateWind(backwards);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
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
    * The swing: nothing without wind or arm; out along the spring's lean times the arm (softened), rising so the vertex
    * stays its arm from where it hangs, as a pendulum; further for a longer arm; and flutter on top within its share.
    */
    void testTheSwing() {
        assert(windSwingOffset(5.0f, Vec3{}, 1.0f, 0.5f, Vec2{0.0f, 0.0f}, 1.0f, leafRate, 3.0f).length() == 0.0f);
        assert(windSwingOffset(0.0f, Vec3{}, 1.0f, 0.0f, Vec2{0.1f, 0.0f}, 1.0f, leafRate, 3.0f).length() == 0.0f);
        assert(windSwingOffset(5.0f, Vec3{}, 1.0f, 0.0f, Vec2{0.1f, 0.0f}, 0.0f, leafRate, 3.0f).length() == 0.0f);

        const Vec2 spring{0.1f, 0.05f};
        const Vec3 swung = windSwingOffset(5.0f, Vec3{}, 0.5f, 0.0f, spring, 1.0f, leafRate, 3.0f);
        const Vec2 lean = windSoftLean(spring * 0.5f);
        assert(near(swung.x, lean.x * 5.0f, 1e-5f) && near(swung.z, lean.y * 5.0f, 1e-5f) && swung.y > 0.0f);
        const Vec3 bob = Vec3{0.0f, -5.0f, 0.0f} + swung;
        assert(near(bob.length(), 5.0f, 1e-4f));
        assert(windSwingOffset(10.0f, Vec3{}, 0.5f, 0.0f, spring, 1.0f, leafRate, 3.0f).x > swung.x);

        const Vec3 fluttering = windSwingOffset(5.0f, Vec3{1.0f, 2.0f, 3.0f}, 0.5f, 0.2f, spring, 1.0f, leafRate, 3.0f) - swung;
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

    // A draw's mesh's hanging entry, its swing and its liana's record reach its instance; the frame takes the first lianas
    // placed; no ray meets a liana.
    void testTheRecordsReachTheGpu() {
        std::vector<DrawItem> draws(2);
        draws[0].mesh = MeshHandle{1};
        draws[1].mesh = MeshHandle{5};
        attachVertexEntries(draws, {VertexEntries{}, VertexEntries{.hanging = 17u}});
        assert(draws[0].hangingEntry == 17u && draws[1].hangingEntry == 0u);

        draws[0].sway = WindSway{.kind = SwayKind::Hanging, .swing = 0.4f};
        const GpuInstance instance = packInstance(draws[0], 0);
        assert(instance.hanging[0] == 17u && instance.hanging[1] == std::bit_cast<std::uint32_t>(0.4f));

        draws[1].lianaShape = 9u;
        assert(packInstance(draws[1], 0).hanging[3] == 9u && instance.hanging[3] == 0u);
        assert(rayMaskFor(draws[1]) == 0u && rayMaskFor(draws[0]) != 0u);

        World world;
        const auto block = std::make_shared<const std::vector<LianaShape>>(1);
        world.add(world.create(), LianaShapes{{block}, 4});
        world.add(world.create(), LianaShapes{{}, 2});
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.0f, 0.1f, 10.0f};
        const FrameDescription frame = buildFrame(world, camera, 0.0f);
        assert(frame.lianaShapes.perSlot == 4u && frame.lianaShapes.slots.size() == 1 && frame.lianaShapes.slots.front() == block);
        World empty;
        assert(buildFrame(empty, camera, 0.0f).lianaShapes.slots.empty());
    }

    /*
    * The forest: its lianas and the islands' roots, vines and moss hang (Hanging); the islands' and moss's every vertex
    * with an entry, hanging from fixed rock; each liana's record hangs it from its first tree's bones (on the air
    * entity), a rope's far end on no tree, fixed on the ground; some climbers' roots hang from their tree in mid air.
    * The swing applies live.
    */
    void testTheForestHangs() {
        TestForest forest{testValley(ForestStart::Trees)};
        const ForestSwaySettings defaults;
        assert(forest.world.has<LianaShapes>(forest.game.getAir()));
        const LianaShapes& shapes = forest.world.get<LianaShapes>(forest.game.getAir());
        bool fixedEnd = false;
        assert(!forest.game.getLianas().empty());

        for (const Entity entity : forest.game.getLianas()) {
            const ModelRenderer& renderer = forest.world.get<ModelRenderer>(entity);
            assert(renderer.sway.kind == SwayKind::Hanging && renderer.sway.swing == defaults.hangingSwing && renderer.lianaShape > 0u);
            const std::size_t record = lianaRecordOf(renderer.lianaShape);
            const LianaShape& shape = (*shapes.slots[record / shapes.perSlot])[record % shapes.perSlot];
            assert(shape.anchors[2] != LIANA_NO_TREE && shape.from[1] > shape.firstTree[1]);
            fixedEnd = fixedEnd || shape.anchors[3] == LIANA_NO_TREE;
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

        for (const ModelRenderer& climber : forest.game.getClimberModels()) {
            const IndexedMesh& mesh = forest.assets.getDrawnMesh(climber.parts.front().mesh);

            for (const HangingVertex& entry : mesh.hanging) {
                assert(entry.first == windAnchorOwn);
                rootInMidAir = rootInMidAir || entry.arm > 0.5f;
            }
        }

        assert(rootInMidAir);

        Input input;
        forest.game.getSwaySettings().hangingSwing = 0.7f;
        forest.game.onUpdate(forest.world, input, 1.0f / 60.0f, 1.0f);
        assert(forest.world.get<ModelRenderer>(forest.game.getLianas().front()).sway.swing == 0.7f);
    }
}

void testWindHanging() {
    testThePackedVertex();
    testTheEntriesStayAligned();
    testHangingStrands();
    testMossSwingsItsLength();
    testTheChainsFrequency();
    testTheBandShares();
    testTheHangingSprings();
    testTubesAndLeavesHang();
    testTheSwing();
    testTheBlend();
    testTheRecordsReachTheGpu();
    testTheForestHangs();
}
