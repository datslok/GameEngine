#include "assets/asset_manager.h"
#include "engine/static_geometry.h"
#include "math/mat4.h"
#include "physics/static_level.h"
#include "scene/frame_description.h"
#include "scene/mesh.h"

#include <cassert>
#include <cmath>
#include <optional>
#include <vector>

namespace {
    DrawItem staticDraw(MeshHandle mesh, const Mat4& model) {
        DrawItem draw;
        draw.mesh = mesh;
        draw.model = model;
        draw.isStatic = true;
        return draw;
    }

    // Static, solid, opaque or cut-out draws block the sky; moving ones, lamps' glowing stand-ins and glass do not.
    void testWhatBlocksTheSky() {
        const DrawItem wall = staticDraw(MeshHandle{0}, Mat4::identity());
        assert(blocksSky(wall));

        DrawItem moving = wall;
        moving.isStatic = false;
        assert(!blocksSky(moving));

        DrawItem bulb = wall;
        bulb.castsShadows = false;
        assert(!blocksSky(bulb));

        DrawItem glass = wall;
        glass.material.transmission = 1.0f;
        assert(!blocksSky(glass));

        DrawItem blended = wall;
        blended.material.alphaMode = AlphaMode::Blend;
        assert(!blocksSky(blended));

        DrawItem leaves = wall;
        leaves.material.alphaMode = AlphaMode::Mask;
        assert(blocksSky(leaves));
    }

    /*
    * The level holds the blocking draws' triangles where they stand, and is rebuilt only when they change: the same
    * draws keep it (and its version), a moved block or another draw rebuilds it, and changes to what does not block
    * leave it alone.
    */
    void testTheLevelFollowsTheDraws() {
        AssetManager assets;
        const MeshHandle cube = assets.addMesh(Mesh::cube());
        StaticGeometry geometry;
        assert(geometry.getVersion() == 0 && geometry.getLevel() == nullptr);

        std::vector<DrawItem> draws{staticDraw(cube, Mat4::translation(10.0f, 0.0f, 0.0f))};
        DrawItem mover = staticDraw(cube, Mat4::translation(0.0f, 5.0f, 0.0f));
        mover.isStatic = false;
        draws.push_back(mover);

        geometry.update(draws, assets);
        const std::shared_ptr<const StaticLevel> first = geometry.getLevel();
        const std::uint64_t version = geometry.getVersion();
        assert(first != nullptr && version != 0);
        assert(first->getTriangleCount() == 12);

        // A ray down the x axis meets the block's west face at x = 9.
        const std::optional<LevelHit> hit = raycast(*first, Ray{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}});
        assert(hit && std::abs(hit->hit.distance - 9.0f) < 1e-4f);

        draws[1].model = Mat4::translation(3.0f, 5.0f, 0.0f);
        geometry.update(draws, assets);
        assert(geometry.getLevel() == first && geometry.getVersion() == version);

        draws[0].model = Mat4::translation(11.0f, 0.0f, 0.0f);
        geometry.update(draws, assets);
        assert(geometry.getLevel() != first && geometry.getVersion() != version);
        const std::uint64_t moved = geometry.getVersion();

        draws.push_back(staticDraw(cube, Mat4::translation(-10.0f, 0.0f, 0.0f)));
        geometry.update(draws, assets);
        assert(geometry.getLevel()->getTriangleCount() == 24 && geometry.getVersion() != moved);

        // Nothing blocking: no level, a new version.
        const std::uint64_t full = geometry.getVersion();
        geometry.update(std::vector<DrawItem>{mover}, assets);
        assert(geometry.getLevel() == nullptr && geometry.getVersion() != full);
    }

    void testTheFingerprint() {
        const std::vector<DrawItem> one{staticDraw(MeshHandle{1}, Mat4::translation(1.0f, 2.0f, 3.0f))};
        std::vector<DrawItem> same = one;
        assert(skyBlockersFingerprint(one) == skyBlockersFingerprint(same));
        same[0].mesh = MeshHandle{2};
        assert(skyBlockersFingerprint(one) != skyBlockersFingerprint(same));

        // Draws that do not block leave it as it was.
        std::vector<DrawItem> withMover = one;
        DrawItem mover = one[0];
        mover.isStatic = false;
        withMover.push_back(mover);
        assert(skyBlockersFingerprint(one) == skyBlockersFingerprint(withMover));

        const CollisionSettings collision = skyRayCollision();
        assert(collision.gridCellSize == 1.0f && collision.largeItemCells > 64);

        // What colours the bounce light changes it too.
        std::vector<DrawItem> recoloured = one;
        recoloured[0].material.colour = Pixel{10, 20, 30};
        assert(skyBlockersFingerprint(one) != skyBlockersFingerprint(recoloured));
        std::vector<DrawItem> metal = one;
        metal[0].material.metallic = 1.0f;
        assert(skyBlockersFingerprint(one) != skyBlockersFingerprint(metal));
        std::vector<DrawItem> textured = one;
        textured[0].material.texture = TextureHandle{3};
        assert(skyBlockersFingerprint(one) != skyBlockersFingerprint(textured));
    }

    bool nearColour(const Vec3& a, const Vec3& b, float tolerance) {
        return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
    }

    /*
    * Each triangle's diffuse albedo, in linear light, beside the level: the material's sRGB colour decoded, times its
    * texture's mean (decoded texel by texel), times one less the metallic share (metals scatter nothing diffusely).
    */
    void testEachTriangleHasItsAlbedo() {
        AssetManager assets;
        const MeshHandle cube = assets.addMesh(Mesh::cube());

        // A 2 x 1 texture, one white and one black texel: a mean of half in linear light.
        ImageData halfGrey;
        halfGrey.width = 2;
        halfGrey.height = 1;
        halfGrey.pixels = {255, 255, 255, 255, 0, 0, 0, 255};
        const TextureHandle texture = assets.addTexture(halfGrey);

        DrawItem red = staticDraw(cube, Mat4::identity());
        red.material.colour = Pixel{255, 0, 0};
        DrawItem metal = staticDraw(cube, Mat4::translation(5.0f, 0.0f, 0.0f));
        metal.material.metallic = 1.0f;
        DrawItem grey = staticDraw(cube, Mat4::translation(10.0f, 0.0f, 0.0f));
        grey.material.texture = texture;
        DrawItem halfMetal = staticDraw(cube, Mat4::translation(15.0f, 0.0f, 0.0f));
        halfMetal.material.metallic = 0.25f;

        StaticGeometry geometry;
        geometry.update(std::vector<DrawItem>{red, metal, grey, halfMetal}, assets);
        const std::shared_ptr<const std::vector<Vec3>> albedo = geometry.getAlbedo();
        assert(albedo != nullptr && albedo->size() == geometry.getLevel()->getTriangleCount() && albedo->size() == 48);

        for (std::size_t triangle = 0; triangle < 12; ++triangle) {
            assert(nearColour((*albedo)[triangle], Vec3{1.0f, 0.0f, 0.0f}, 1e-6f));
            assert(nearColour((*albedo)[12 + triangle], Vec3{0.0f, 0.0f, 0.0f}, 1e-6f));
            assert(nearColour((*albedo)[24 + triangle], Vec3{0.5f, 0.5f, 0.5f}, 1e-6f));
            assert(nearColour((*albedo)[36 + triangle], Vec3{0.75f, 0.75f, 0.75f}, 1e-6f));
        }

        // Nothing blocking: no albedo either.
        geometry.update(std::vector<DrawItem>{}, assets);
        assert(geometry.getAlbedo() == nullptr);
    }
}

void testStaticGeometry() {
    testWhatBlocksTheSky();
    testTheLevelFollowsTheDraws();
    testTheFingerprint();
    testEachTriangleHasItsAlbedo();
}
