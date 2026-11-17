#include "render/gpu/gpu_scene.h"
#include "scene/card_thinning.h"
#include "scene/culling.h"
#include "scene/frame_description.h"
#include "scene/indexed_mesh.h"
#include "scene/level_of_detail.h"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    // Cards 1 m a side thinned to half, their sides growing by share^-0.5 (Cook 2007's cover kept).
    const CardThinning halfCards{.cardSize = 1.0f, .growth = 0.5f, .leastShare = 0.5f};

    /*
    * The kept share's error: the kept cards' sides grow by share^-growth, so the cloud strays by cardSize times
    * (share^-growth - 1); none at a full share. thinnedShare is its inverse: the share whose error is the error allowed,
    * held between the least share and 1, so the error is spent exactly as the discrete levels spend it.
    */
    void testTheShareKeepsTheErrorWithinTheBudget() {
        assert(thinningError(halfCards, 1.0f) == 0.0f);
        assert(near(thinningError(halfCards, 0.5f), std::sqrt(2.0f) - 1.0f, 1e-6f));
        assert(thinnedShare(halfCards, 0.0f) == 1.0f);

        for (const float share : {0.95f, 0.8f, 0.6f, 0.5f}) {
            assert(near(thinnedShare(halfCards, thinningError(halfCards, share)), share, 1e-5f));
        }

        // Never below the least share, however large the error allowed; less allowed keeps more.
        assert(thinnedShare(halfCards, 100.0f) == 0.5f);
        assert(thinnedShare(halfCards, 0.05f) > thinnedShare(halfCards, 0.1f));

        // A slower growth strays less at the same share, so the same error lets more cards go.
        const CardThinning slower{.cardSize = 1.0f, .growth = 0.3f, .leastShare = 0.5f};
        assert(thinningError(slower, 0.7f) < thinningError(halfCards, 0.7f));
        assert(thinnedShare(slower, 0.1f) < thinnedShare(halfCards, 0.1f));

        // Nothing to thin: the whole mesh, whatever is allowed.
        assert(thinnedShare(CardThinning{}, 10.0f) == 1.0f);
    }

    /*
    * A card of rank r is whole while the share is at least r and gone once it is cardThinningBand below, shrinking
    * smoothly between, so no card pops. Rank 0 is a card the coarsest share keeps: always whole. The ranks of the cards
    * that go lie in [least share + band, 1], so every one of them is gone at the least share.
    */
    void testEachCardShrinksAwayPastItsRank() {
        assert(cardPresence(0.0f, 0.5f) == 1.0f && cardPresence(0.0f, 0.0f) == 1.0f);
        assert(cardPresence(0.8f, 0.8f) == 1.0f && cardPresence(0.8f, 1.0f) == 1.0f);
        assert(cardPresence(0.8f, 0.8f - cardThinningBand) == 0.0f);
        const float halfway = cardPresence(0.8f, 0.8f - 0.5f * cardThinningBand);
        assert(halfway > 0.0f && halfway < 1.0f);

        for (const float unit : {0.0f, 0.3f, 0.999f}) {
            const float rank = removedCardRank(unit, 0.5f);
            assert(rank >= 0.5f + cardThinningBand - 1e-6f && rank <= 1.0f);
            assert(cardPresence(rank, 0.5f) == 0.0f && cardPresence(rank, 1.0f) == 1.0f);
        }
    }

    // A card's size: grown by share^-growth while whole, times its presence; its corners scaled about its anchor (the stem).
    void testCardsScaleAboutTheirAnchors() {
        assert(thinnedCardScale(halfCards, 0.0f, 1.0f) == 1.0f && thinnedCardScale(halfCards, 0.9f, 1.0f) == 1.0f);
        assert(near(thinnedCardScale(halfCards, 0.0f, 0.5f), std::sqrt(2.0f), 1e-6f));
        assert(thinnedCardScale(halfCards, 0.9f, 0.5f) == 0.0f);

        const ThinningCard card{.anchor = Vec3{1.0f, 2.0f, 3.0f}, .rank = 0.0f};
        const Vec3 moved = thinnedPosition(Vec3{2.0f, 2.0f, 3.0f}, card, 1.5f);
        assert(near(moved.x, 2.5f, 1e-6f) && moved.y == 2.0f && moved.z == 3.0f);
        const Vec3 gone = thinnedPosition(Vec3{5.0f, 6.0f, 7.0f}, card, 0.0f);
        assert(gone.x == card.anchor.x && gone.y == card.anchor.y && gone.z == card.anchor.z);
    }

    /*
    * A view allows pixelError pixels of error: at d metres, d times pixelError / pixelsPerMetre metres. Without levels
    * (or in an orthographic view, which thins nothing) none is allowed, so every card stays.
    */
    void testAViewAllowsErrorInProportionToDistance() {
        const LevelView view = levelView(Vec3{}, 1000.0f, 1.5707963f, LevelOfDetailSettings{.enabled = true, .pixelError = 2.0f});
        assert(near(thinningErrorPerMetre(view), 2.0f / 500.0f, 1e-7f));

        LevelView off = view;
        off.enabled = false;
        assert(thinningErrorPerMetre(off) == 0.0f);

        LevelView flat = view;
        flat.orthographic = true;
        assert(thinningErrorPerMetre(flat) == 0.0f);
    }

    /*
    * The fade meets the discrete level exactly: a mesh's coarser level of the kept cards alone, its error the least
    * share's, is chosen from the distance at which the fade reaches the least share, so the swap changes nothing seen.
    */
    void testTheFadeEndsWhereTheCoarserLevelBegins() {
        const LevelView view = levelView(Vec3{}, 1000.0f, 1.5707963f, LevelOfDetailSettings{.enabled = true, .pixelError = 2.0f});
        const std::vector<MeshLevel> levels{MeshLevel{MeshHandle{4}, thinningError(halfCards, 0.5f), true}};
        // The level is chosen where its error covers 2 pixels: 0.414 m x 500 / 2 = 103.6 m.
        const float switchDistance = thinningError(halfCards, 0.5f) * 500.0f / 2.0f;

        assert(chooseLevel(levels, switchDistance * 0.99f, view) == 0);
        assert(chooseLevel(levels, switchDistance * 1.01f, view) == 1);
        assert(near(thinnedShare(halfCards, switchDistance * thinningErrorPerMetre(view)), 0.5f, 1e-4f));
        assert(thinnedShare(halfCards, 0.5f * switchDistance * thinningErrorPerMetre(view)) > 0.6f);
    }

    // A mesh's first vertices and triangles as a mesh of their own, its thinning records left with the mesh.
    void testAPrefixIsTheMeshsFirstVerticesAndTriangles() {
        IndexedMesh mesh;

        for (int vertex = 0; vertex < 6; ++vertex) {
            mesh.vertices.push_back(MeshVertex{Vec3{static_cast<float>(vertex), 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec2{}, Vec4{1.0f, 0.0f, 0.0f, 1.0f}});
            mesh.thinningCards.push_back(ThinningCard{});
        }

        mesh.indices = {0, 1, 2, 3, 4, 5};
        mesh.thinning = halfCards;
        const IndexedMesh prefix = meshPrefix(mesh, 3, 3);
        assert(prefix.vertices.size() == 3 && prefix.vertices[2].position.x == 2.0f);
        assert((prefix.indices == std::vector<std::uint32_t>{0, 1, 2}));
        assert(prefix.thinningCards.empty() && prefix.thinning.cardSize == 0.0f);

        // Triangles reaching past the kept vertices, or more than the mesh has, are mistakes.
        bool threw = false;

        try {
            (void)meshPrefix(mesh, 2, 3);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);

        // A mesh made kept cards first: its level is the vertices before the first card ranked to go, and the triangles
        // before the first reaching them.
        mesh.thinningCards[3].rank = 0.7f;
        mesh.thinningCards[4].rank = 0.8f;
        mesh.thinningCards[5].rank = 0.9f;
        mesh.indices = {0, 1, 2, 2, 1, 0, 3, 4, 5};
        const IndexedMesh kept = thinnedLevel(mesh);
        assert(kept.vertices.size() == 3 && (kept.indices == std::vector<std::uint32_t>{0, 1, 2, 2, 1, 0}));

        // Nothing to thin: the whole mesh.
        IndexedMesh plain;
        plain.vertices = mesh.vertices;
        plain.indices = mesh.indices;
        assert(thinnedLevel(plain).indices.size() == 9);
    }

    /*
    * The draw list marks each instance a view draws with its thinned mesh (the mesh itself, or a level sharing its first
    * vertices), so the vertex shaders thin those and no other mesh: a coarser level of its own, which the records do not
    * describe, is drawn whole.
    */
    void testTheDrawListMarksThinnedDraws() {
        const LevelView view = levelView(Vec3{}, 1000.0f, 1.5707963f, LevelOfDetailSettings{.enabled = true, .pixelError = 2.0f});
        const std::vector<MeshLevel> levels{MeshLevel{MeshHandle{4}, 0.4f, true}, MeshLevel{MeshHandle{5}, 1.0f, false}};

        std::vector<DrawItem> draws(4);

        for (std::size_t index = 0; index < draws.size(); ++index) {
            draws[index].mesh = MeshHandle{3};
            draws[index].levels = levels;
            draws[index].thinningEntry = 7;
        }

        draws[0].bounds = BoundingSphere{Vec3{0.0f, 0.0f, -11.0f}, 1.0f};  // the mesh itself
        draws[1].bounds = BoundingSphere{Vec3{0.0f, 0.0f, -151.0f}, 1.0f}; // the level sharing its vertices
        draws[2].bounds = BoundingSphere{Vec3{0.0f, 0.0f, -401.0f}, 1.0f}; // a level of its own
        draws[3].bounds = BoundingSphere{Vec3{0.0f, 0.0f, -11.0f}, 1.0f};
        draws[3].thinningEntry = 0;                                         // nothing to thin

        const BatchedDraws batched = batchByMesh(draws, {0, 1, 2, 3}, 0, &view);
        std::vector<std::uint32_t> list;
        appendDrawList(list, batched);
        assert(list.size() == 4);

        for (std::size_t position = 0; position < list.size(); ++position) {
            const std::size_t index = batched.order[position];
            const bool thinned = index == 0 || index == 1;
            assert((list[position] & drawListIndexMask) == index);
            assert(((list[position] & drawListThinnedBit) != 0) == thinned);
        }

        // Without a view each draw is drawn with its mesh itself: thinned wherever it has records.
        std::vector<std::uint32_t> plain;
        appendDrawList(plain, batchByMesh(draws, {0, 3}));
        assert(plain.size() == 2 && (plain[0] & drawListThinnedBit) != 0 && plain[1] == 3u);
    }

    /*
    * The GPU's records: a mesh's header (card size, growth, least share) and then one record a vertex (its card's
    * anchor and rank); a mesh's entry is one past its header. Its instance carries the entry and the sphere its level is
    * chosen by, in the world.
    */
    void testTheRecordsAndInstanceCarryTheThinning() {
        IndexedMesh mesh;
        mesh.thinning = halfCards;
        mesh.vertices.resize(2);
        mesh.thinningCards = {ThinningCard{.anchor = Vec3{1.0f, 2.0f, 3.0f}, .rank = 0.0f}, ThinningCard{.anchor = Vec3{4.0f, 5.0f, 6.0f}, .rank = 0.75f}};
        const std::vector<GpuThinningRecord> records = packCardThinning(mesh);
        assert(records.size() == 3);
        assert(records[0].values[0] == 1.0f && records[0].values[1] == 0.5f && records[0].values[2] == 0.5f);
        assert(records[2].values[0] == 4.0f && records[2].values[2] == 6.0f && records[2].values[3] == 0.75f);
        assert(packCardThinning(IndexedMesh{}).empty());

        // One record a vertex, or it is a mistake.
        mesh.thinningCards.pop_back();
        bool threw = false;

        try {
            (void)packCardThinning(mesh);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);

        DrawItem draw;
        draw.mesh = MeshHandle{2};
        draw.thinningEntry = 9;
        draw.bounds = BoundingSphere{Vec3{1.0f, 2.0f, 3.0f}, 4.0f};
        draw.levelBounds = BoundingSphere{Vec3{5.0f, 6.0f, 7.0f}, 8.0f};
        const GpuInstance instance = packInstance(draw, 0);
        assert(instance.hanging[2] == 9u);
        assert(instance.levelSphere[0] == 5.0f && instance.levelSphere[1] == 6.0f && instance.levelSphere[2] == 7.0f && instance.levelSphere[3] == 8.0f);

        // Without a level box the draw's own bounds choose.
        draw.levelBounds = BoundingSphere{};
        draw.levelBounds.radius = -1.0f;
        assert(packInstance(draw, 0).levelSphere[3] == 4.0f);
    }

    // Each draw gets its mesh's entries, as the renderer gave them when the meshes were uploaded.
    void testDrawsGetTheirMeshesEntries() {
        std::vector<DrawItem> draws(3);
        draws[0].mesh = MeshHandle{1};
        draws[1].mesh = MeshHandle{0};
        draws[2].mesh = MeshHandle{6};
        attachVertexEntries(draws, {VertexEntries{}, VertexEntries{.hanging = 3, .thinning = 5}});
        assert(draws[0].hangingEntry == 3 && draws[0].thinningEntry == 5);
        assert(draws[1].hangingEntry == 0 && draws[1].thinningEntry == 0);
        assert(draws[2].hangingEntry == 0 && draws[2].thinningEntry == 0);
    }
}

void testCardThinning() {
    testTheShareKeepsTheErrorWithinTheBudget();
    testEachCardShrinksAwayPastItsRank();
    testCardsScaleAboutTheirAnchors();
    testAViewAllowsErrorInProportionToDistance();
    testTheFadeEndsWhereTheCoarserLevelBegins();
    testAPrefixIsTheMeshsFirstVerticesAndTriangles();
    testTheDrawListMarksThinnedDraws();
    testTheRecordsAndInstanceCarryTheThinning();
    testDrawsGetTheirMeshesEntries();
}
