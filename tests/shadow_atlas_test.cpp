#include "render/gpu/shadow_atlas.h"
#include "render/gpu/shadow_cache.h"
#include "render/gpu/shadow_map.h"
#include "scene/frame_description.h"

#include <cassert>
#include <optional>
#include <vector>

namespace {
    constexpr std::uint64_t atlasTexels = static_cast<std::uint64_t>(shadowAtlasWidth) * shadowAtlasHeight;

    bool overlap(const AtlasSquare& a, const AtlasSquare& b) {
        return a.x < b.x + b.size && b.x < a.x + a.size && a.y < b.y + b.size && b.y < a.y + a.size;
    }

    void assertSeparateAndInside(const std::vector<AtlasSquare>& squares) {
        for (std::size_t i = 0; i < squares.size(); ++i) {
            assert(squares[i].x + squares[i].size <= shadowAtlasWidth && squares[i].y + squares[i].size <= shadowAtlasHeight);

            for (std::size_t j = i + 1; j < squares.size(); ++j) {
                assert(!overlap(squares[i], squares[j]));
            }
        }
    }

    // Squares of mixed sizes never overlap, and an atlas full of the smallest ones has no room for another.
    void testSquaresAreHandedOut() {
        ShadowAtlasAllocator allocator;
        assert(allocator.freeTexels() == atlasTexels);

        std::vector<AtlasSquare> squares;
        squares.push_back(*allocator.allocate(1024));
        squares.push_back(*allocator.allocate(256));
        squares.push_back(*allocator.allocate(512));
        squares.push_back(*allocator.allocate(256));
        squares.push_back(*allocator.allocate(2048));
        assertSeparateAndInside(squares);
        assert(squares[1].size == 256 && squares[2].size == 512 && squares[4].size == 2048);
        assert(allocator.freeTexels() == atlasTexels - 2048 * 2048 - 1024 * 1024 - 512 * 512 - 2 * 256 * 256);

        ShadowAtlasAllocator full;

        for (int i = 0; i < 512; ++i) {
            assert(full.allocate(256).has_value());
        }

        assert(!full.allocate(256).has_value());
        assert(full.freeTexels() == 0);
    }

    // Four freed quarters join back into their parent square, so the atlas does not crumble into small pieces.
    void testFreedSquaresMerge() {
        ShadowAtlasAllocator allocator;
        std::vector<AtlasSquare> small;

        for (int i = 0; i < 512; ++i) {
            small.push_back(*allocator.allocate(256));
        }

        for (const AtlasSquare& square : small) {
            allocator.release(square);
        }

        assert(allocator.freeTexels() == atlasTexels);

        // They merge all the way up to the largest squares: the 8192 x 4096 atlas holds exactly eight of 2048.
        std::vector<AtlasSquare> largest;
        for (int i = 0; i < 8; ++i) {
            largest.push_back(*allocator.allocate(2048));
        }
        assertSeparateAndInside(largest);
        assert(!allocator.allocate(2048).has_value());
        assert(allocator.freeTexels() == 0);
    }

    ShadowSquareRequest request(std::uint32_t index, std::uint32_t size) {
        return ShadowSquareRequest{ShadowTileKey{1, Entity{index, 1}, 0}, size};
    }

    // A light asking again for the same size keeps its square, whatever comes and goes around it.
    void testSquaresStayPut() {
        ShadowAtlasLayout layout;
        const std::vector<AtlasSquare> first = layout.place({request(1, 512), request(2, 256), request(3, 1024)});
        assertSeparateAndInside(first);

        const std::vector<AtlasSquare> same = layout.place({request(1, 512), request(2, 256), request(3, 1024)});
        assert(same == first);
        assert(!layout.repackedLastTime());

        // The second light leaves and a new one arrives; the others stay where they were.
        const std::vector<AtlasSquare> changed = layout.place({request(3, 1024), request(4, 256), request(1, 512)});
        assert(changed[0] == first[2]);
        assert(changed[2] == first[0]);
        assertSeparateAndInside(changed);

        // A light whose tile size changes gets a square of the new size.
        const std::vector<AtlasSquare> resized = layout.place({request(3, 1024), request(4, 256), request(1, 256)});
        assert(resized[0] == first[2]);
        assert(resized[1] == changed[1]);
        assert(resized[2].size == 256);
        assertSeparateAndInside(resized);
    }

    // When the free space is scattered so a big square does not fit although the area would, everything is packed again.
    void testCrumbledAtlasIsRepacked() {
        ShadowAtlasLayout layout;
        std::vector<ShadowSquareRequest> requests;

        for (std::uint32_t i = 0; i < 512; ++i) {
            requests.push_back(request(i + 1, 256));
        }

        layout.place(requests);

        // Keep one small square in every 1024 block (32 of them), and ask for one big square.
        std::vector<ShadowSquareRequest> scattered;

        for (std::uint32_t i = 0; i < 512; i += 16) {
            scattered.push_back(requests[i]);
        }

        scattered.push_back(request(1000, 1024));
        const std::vector<AtlasSquare> squares = layout.place(scattered);

        assert(layout.repackedLastTime());
        assert(squares.size() == scattered.size());
        assert(squares.back().size == 1024);
        assertSeparateAndInside(squares);

        // After that, things settle again.
        assert(layout.place(scattered) == squares);
        assert(!layout.repackedLastTime());
    }

    DrawItem drawAt(float x, std::uint32_t mesh) {
        DrawItem draw;
        draw.mesh = MeshHandle{mesh};
        draw.model = Mat4::translation(x, 0.0f, 0.0f);
        return draw;
    }

    // The fingerprint of a tile's casters changes when any of them moves, changes mesh, or joins or leaves the tile.
    void testCasterSignature() {
        std::vector<DrawItem> draws = {drawAt(0.0f, 0), drawAt(2.0f, 1), drawAt(4.0f, 2)};
        const std::uint64_t before = shadowCasterSignature(draws, {0, 1});

        assert(shadowCasterSignature(draws, {0, 1}) == before);
        assert(shadowCasterSignature(draws, {0, 1, 2}) != before);
        assert(shadowCasterSignature(draws, {0}) != before);

        std::vector<DrawItem> moved = draws;
        moved[1].model = Mat4::translation(2.001f, 0.0f, 0.0f);
        assert(shadowCasterSignature(moved, {0, 1}) != before);

        std::vector<DrawItem> swapped = draws;
        swapped[1].mesh = MeshHandle{7};
        assert(shadowCasterSignature(swapped, {0, 1}) != before);

        // Things outside the tile do not count.
        std::vector<DrawItem> elsewhere = draws;
        elsewhere[2].model = Mat4::translation(50.0f, 0.0f, 0.0f);
        assert(shadowCasterSignature(elsewhere, {0, 1}) == before);
    }

    ShadowTile tileAt(std::uint32_t x, std::uint32_t y, std::uint32_t size, float shift = 0.0f) {
        return ShadowTile{Mat4::translation(shift, 0.0f, 0.0f), x, y, size, BoundingSphere{}};
    }

    // A tile is still good when the same view of the same casters was the last thing drawn into its square.
    void testTileMemory() {
        ShadowTileMemory memory;
        assert(!memory.isCurrent(tileAt(0, 0, 512), 5));

        memory.remember(tileAt(0, 0, 512), 5);
        assert(memory.isCurrent(tileAt(0, 0, 512), 5));
        assert(!memory.isCurrent(tileAt(0, 0, 512), 6));          // something moved inside it
        assert(!memory.isCurrent(tileAt(0, 0, 512, 0.5f), 5));    // the light moved
        assert(!memory.isCurrent(tileAt(512, 0, 512), 5));        // another square
        assert(!memory.isCurrent(tileAt(0, 0, 256), 5));          // the corner of it

        // Drawing into part of the square spoils it.
        memory.remember(tileAt(256, 256, 256), 9);
        assert(!memory.isCurrent(tileAt(0, 0, 512), 5));
        assert(memory.isCurrent(tileAt(256, 256, 256), 9));

        // Squares beside each other do not spoil each other.
        memory.remember(tileAt(1024, 0, 1024), 3);
        assert(memory.isCurrent(tileAt(256, 256, 256), 9));
        assert(memory.isCurrent(tileAt(1024, 0, 1024), 3));

        memory.forgetAll();
        assert(!memory.isCurrent(tileAt(1024, 0, 1024), 3));
    }

    PlacedPointLight lampAt(float x, std::uint32_t entityIndex) {
        PlacedPointLight placed{Vec3{x, 1.0f, 0.0f}, PointLight{}};
        placed.light.range = 5.0f;
        placed.entity = Entity{entityIndex, 1};
        return placed;
    }

    // Planned frame after frame, lights keep their tiles, so their contents can be kept too.
    void testPlansKeepTilesBetweenFrames() {
        const Camera camera{Vec3{5.0f, 40.0f, 0.0f}, Vec3{5.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, 1.5f, 1.5f, 0.1f, 200.0f};
        ShadowAtlasLayout layout{shadowAtlasSizeFor(ShadowSettings{})};

        FrameLighting lighting;
        lighting.pointLights = {lampAt(0.0f, 1), lampAt(5.0f, 2), lampAt(10.0f, 3)};
        lighting.directionalLights.push_back(DirectionalLight{});

        const ShadowPlan first = planShadows(lighting, camera, layout);
        const ShadowPlan second = planShadows(lighting, camera, layout);
        // The sun's cascades first, then six faces per lamp.
        const std::size_t sunTiles = maxShadowCascades;
        assert(first.tiles.size() == sunTiles + 18 && second.tiles.size() == first.tiles.size());

        for (std::size_t i = 0; i < first.tiles.size(); ++i) {
            assert(first.tiles[i].x == second.tiles[i].x && first.tiles[i].y == second.tiles[i].y);
            assert(first.tiles[i].size == second.tiles[i].size);
        }

        // The first lamp goes out: the last lamp moves up a slot (its tiles are numbered earlier), but keeps its squares.
        lighting.pointLights.erase(lighting.pointLights.begin());
        const ShadowPlan third = planShadows(lighting, camera, layout);
        assert(third.tiles.size() == sunTiles + 12);

        for (std::size_t face = 0; face < 6; ++face) {
            const ShadowTile& before = first.tiles[sunTiles + 12 + face];
            const ShadowTile& after = third.tiles[sunTiles + 6 + face];
            assert(before.x == after.x && before.y == after.y && before.size == after.size);
        }
    }
    // The Ultra atlas (8192 x 12288) starts as six squares of 4096, which a 4096 request takes whole, and smaller ones
    // split; freed, the quarters merge back up to 4096.
    void testUltraAtlasHandsOutLargeSquares() {
        const ShadowAtlasSize ultra{8192, 12288, 4096};
        ShadowAtlasAllocator allocator{ultra};
        const std::uint64_t texels = 8192ull * 12288ull;
        assert(allocator.freeTexels() == texels);

        std::vector<AtlasSquare> squares;

        for (int i = 0; i < 5; ++i) {
            squares.push_back(*allocator.allocate(4096));
        }

        squares.push_back(*allocator.allocate(256));
        assert(!allocator.allocate(4096));

        for (const AtlasSquare& square : squares) {
            assert(square.x + square.size <= ultra.width && square.y + square.size <= ultra.height);
        }

        for (const AtlasSquare& square : squares) {
            allocator.release(square);
        }

        assert(allocator.freeTexels() == texels);

        for (int i = 0; i < 6; ++i) {
            assert(allocator.allocate(4096));
        }
    }
}

void testShadowAtlas() {
    testUltraAtlasHandsOutLargeSquares();
    testSquaresAreHandedOut();
    testFreedSquaresMerge();
    testSquaresStayPut();
    testCrumbledAtlasIsRepacked();
    testCasterSignature();
    testTileMemory();
    testPlansKeepTilesBetweenFrames();
}
