#include "render/gpu/gpu_scene.h"
#include "render/gpu/material_uniforms.h"
#include "render/gpu/material_shared.h"
#include "render/gpu/ray_tracing.h"
#include "scene/impostor.h"
#include "scene/indexed_mesh.h"
#include "test_jobs.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    /*
    * The upper hemisphere folded onto a square (hemi-octahedral, as Epic's Impostor Baker for trees): straight up is the
    * middle, the horizon the square's edge; a direction comes back as it went, and one below the horizon is taken as
    * on it.
    */
    void testTheHemisphereFoldsOntoASquare() {
        const Vec2 up = hemiOctahedralEncode(Vec3{0.0f, 1.0f, 0.0f});
        assert(near(up.x, 0.5f, 1e-6f) && near(up.y, 0.5f, 1e-6f));
        const Vec2 east = hemiOctahedralEncode(Vec3{1.0f, 0.0f, 0.0f});
        assert(near(east.x, 1.0f, 1e-6f) && near(east.y, 1.0f, 1e-6f));

        for (const Vec3& direction : {Vec3{0.3f, 0.8f, -0.5f}, Vec3{-0.9f, 0.1f, 0.4f}, Vec3{0.0f, 0.2f, 1.0f}, Vec3{-0.2f, 0.5f, -0.7f}}) {
            const Vec3 unit = direction.normalized();
            assert(nearVec(hemiOctahedralDecode(hemiOctahedralEncode(unit)), unit, 1e-5f));
        }

        const Vec3 below = hemiOctahedralDecode(hemiOctahedralEncode(Vec3{0.6f, -0.8f, 0.0f}));
        assert(near(below.y, 0.0f, 1e-5f) && near(below.x, 1.0f, 1e-5f));
    }

    /*
    * Frames on a frames x frames grid over the square, corner to corner: frame (c, r)'s direction is the square's point
    * (c, r) / (frames - 1) unfolded; its plane's axes (right, up) are square to it and to each other, right level with the
    * ground (from straight above, along x).
    */
    void testEachFrameHasADirectionAndAPlane() {
        const Vec3 top = impostorFrameDirection(Vec2{3.0f, 3.0f}, 7);
        assert(nearVec(top, Vec3{0.0f, 1.0f, 0.0f}, 1e-6f));
        const Vec3 side = impostorFrameDirection(Vec2{6.0f, 6.0f}, 7);
        assert(nearVec(side, Vec3{1.0f, 0.0f, 0.0f}, 1e-6f));

        for (const Vec3& direction : {top, side, impostorFrameDirection(Vec2{1.0f, 4.0f}, 7)}) {
            const ImpostorBasis basis = impostorFrameBasis(direction);
            assert(near(basis.right.length(), 1.0f, 1e-5f) && near(basis.up.length(), 1.0f, 1e-5f));
            assert(near(basis.right.dot(direction), 0.0f, 1e-5f) && near(basis.up.dot(direction), 0.0f, 1e-5f) && near(basis.right.dot(basis.up), 0.0f, 1e-5f));
            assert(near(basis.right.y, 0.0f, 1e-5f));
        }

        assert(nearVec(impostorFrameBasis(top).right, Vec3{1.0f, 0.0f, 0.0f}, 1e-6f));
        // From the side, up is the world's up.
        assert(nearVec(impostorFrameBasis(side).up, Vec3{0.0f, 1.0f, 0.0f}, 1e-6f));
    }

    /*
    * A view blends the four frames round it on the grid, weights bilinear (adding up to one); seen straight along a
    * frame's direction, that frame alone.
    */
    void testAViewBlendsTheFourNearestFrames() {
        const ImpostorBlend exact = impostorFrameBlend(impostorFrameDirection(Vec2{2.0f, 5.0f}, 8), 8);
        float total = 0.0f;
        float onFrame = 0.0f;

        for (int corner = 0; corner < 4; ++corner) {
            total += exact.weights[corner];

            if (exact.frames[corner].x == 2.0f && exact.frames[corner].y == 5.0f) {
                onFrame += exact.weights[corner];
            }
        }

        assert(near(total, 1.0f, 1e-5f) && near(onFrame, 1.0f, 1e-4f));

        const ImpostorBlend between = impostorFrameBlend(Vec3{0.4f, 0.7f, 0.3f}.normalized(), 8);
        total = 0.0f;

        for (int corner = 0; corner < 4; ++corner) {
            assert(between.frames[corner].x >= 0.0f && between.frames[corner].x <= 7.0f && between.weights[corner] >= 0.0f);
            total += between.weights[corner];
        }

        assert(near(total, 1.0f, 1e-5f));
    }

    /*
    * A point on the card (its offset from the tree's middle over the radius, in the tree's frame) is read in a frame at
    * its place on that frame's plane: the middle at the frame's middle, a radius along right at its right edge; the
    * frame's place in the atlas is its column and row over the frames.
    */
    void testAPointIsReadOnEachFramesPlane() {
        const Vec3 direction = impostorFrameDirection(Vec2{1.0f, 2.0f}, 4);
        const ImpostorBasis basis = impostorFrameBasis(direction);
        const Vec2 middle = impostorFrameUv(Vec3{0.0f, 0.0f, 0.0f}, direction);
        assert(near(middle.x, 0.5f, 1e-6f) && near(middle.y, 0.5f, 1e-6f));
        const Vec2 right = impostorFrameUv(basis.right, direction);
        assert(near(right.x, 1.0f, 1e-5f) && near(right.y, 0.5f, 1e-5f));
        const Vec2 atlas = impostorAtlasUv(Vec2{0.5f, 0.25f}, Vec2{1.0f, 2.0f}, 4);
        assert(near(atlas.x, 1.5f / 4.0f, 1e-6f) && near(atlas.y, 2.25f / 4.0f, 1e-6f));
    }

    // A red square 2 m across standing on the ground in the x-y plane, facing +z, its front's normals +z.
    IndexedMesh redSquare() {
        IndexedMesh mesh;

        for (const Vec2& corner : {Vec2{-1.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 2.0f}, Vec2{-1.0f, 2.0f}}) {
            MeshVertex vertex{};
            vertex.position = Vec3{corner.x, corner.y, 0.0f};
            vertex.normal = Vec3{0.0f, 0.0f, 1.0f};
            vertex.uv = Vec2{corner.x * 0.5f + 0.5f, corner.y * 0.5f};
            mesh.vertices.push_back(vertex);
        }

        mesh.indices = {0, 1, 2, 0, 2, 3};
        return mesh;
    }

    /*
    * Baking: each frame the parts seen along its direction, orthographic, filling the sphere round them: the colour (the
    * part's colour times its texture; alpha whether anything was there) and the normal in the tree's frame (0..1 bytes).
    * Seen from the front (+z), the square fills the frame's middle red with a +z normal; its corners, outside the
    * square, stay empty; from straight above it is edge-on and covers almost nothing.
    */
    void testTheBakeDrawsEachFrame() {
        const IndexedMesh square = redSquare();
        const ImpostorPart part{.mesh = &square, .colour = nullptr, .tint = Pixel{255, 0, 0}, .alphaCutoff = 0.0f};
        const ImpostorBake bake = bakeImpostor({part}, 3, 32, testJobs());
        assert(bake.colour.width == 96 && bake.colour.height == 96 && bake.normal.width == 96);
        assert(bake.colour.encoding == TextureEncoding::Srgb && bake.normal.encoding == TextureEncoding::Linear);
        assert(nearVec(bake.centre, Vec3{0.0f, 1.0f, 0.0f}, 1e-4f) && near(bake.radius, std::sqrt(2.0f), 1e-4f));

        // The front frame: direction +z is the square's point (1, 0), frame (2, 0).
        const auto pixel = [](const ImageData& image, int x, int y) { return &image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4]; };
        const std::uint8_t* middle = pixel(bake.colour, 2 * 32 + 16, 0 * 32 + 16);
        assert(middle[3] == 255 && middle[0] > 200 && middle[1] < 30);
        const std::uint8_t* normal = pixel(bake.normal, 2 * 32 + 16, 16);
        assert(normal[2] > 240 && std::abs(normal[0] - 128) < 4 && std::abs(normal[1] - 128) < 4);
        assert(pixel(bake.colour, 2 * 32 + 1, 1)[3] == 0);

        int seenFromAbove = 0;

        for (int y = 32; y < 64; ++y) {
            for (int x = 32; x < 64; ++x) {
                seenFromAbove += pixel(bake.colour, x, y)[3] > 0 ? 1 : 0;
            }
        }

        assert(seenFromAbove < 32 * 32 / 10);

        // A cut-out's holes stay empty: the square's texture transparent on its left half.
        ImageData halfClear{.width = 2, .height = 1, .pixels = {255, 255, 255, 0, 255, 255, 255, 255}};
        const ImpostorPart cutOut{.mesh = &square, .colour = &halfClear, .tint = Pixel{255, 255, 255}, .alphaCutoff = 0.5f};
        const ImpostorBake holed = bakeImpostor({cutOut}, 3, 32, testJobs());
        assert(pixel(holed.colour, 2 * 32 + 8, 16)[3] == 0 && pixel(holed.colour, 2 * 32 + 24, 16)[3] == 255);
    }

    /*
    * Depth over the radius goes into a byte (-1 to 0, 1 to 255) and comes back from the texture's 0..1 within half a step;
    * the surface behind a card point lies at its depth along the frame's direction, at the same place on the frame's plane.
    */
    void testDepthAndTheSurfaceBehindACardPoint() {
        assert(impostorDepthEncode(-1.0f) == 0 && impostorDepthEncode(1.0f) == 255 && impostorDepthEncode(5.0f) == 255);

        for (float depth : {-0.9f, -0.3f, 0.0f, 0.41f, 0.97f}) {
            // Half a step is 1 / 255 of depth (a step spans 2 / 255), and float rounding may add a hair.
            assert(near(impostorDepthDecode(static_cast<float>(impostorDepthEncode(depth)) / 255.0f), depth, 1.01f / 255.0f));
        }

        const Vec3 direction = impostorFrameDirection(Vec2{5.0f, 2.0f}, 8);
        const Vec3 cardPoint{0.3f, -0.2f, 0.5f};
        const Vec3 surface = impostorSurfacePoint(cardPoint, direction, -0.4f);
        assert(near(surface.dot(direction), -0.4f, 1e-5f));
        const Vec2 onCard = impostorFrameUv(cardPoint, direction);
        const Vec2 onSurface = impostorFrameUv(surface, direction);
        assert(near(onCard.x, onSurface.x, 1e-5f) && near(onCard.y, onSurface.y, 1e-5f));
    }

    /*
    * The sun's test over a 2 x 2 of texels: empty texels and texels no nearer the sun than the point (plus the bias) are
    * lit, filtered bilinearly in textureGather's order ((0, 1), (1, 1), (1, 0), (0, 0)).
    */
    void testTheSunsTestFiltersFourTexels() {
        const std::array<float, 4> nearer{0.9f, 0.9f, 0.9f, 0.9f};
        const std::array<float, 4> full{1.0f, 1.0f, 1.0f, 1.0f};
        const std::array<float, 4> empty{0.0f, 0.0f, 0.0f, 0.0f};
        // Stored 0.9 decodes to depth 0.8; the point at 0 lies behind it.
        assert(impostorGatherLit(nearer, full, Vec2{0.3f, 0.6f}, 0.0f, 0.05f) == 0.0f);
        assert(impostorGatherLit(nearer, empty, Vec2{0.3f, 0.6f}, 0.0f, 0.05f) == 1.0f);
        // Within the bias it is the surface itself.
        assert(impostorGatherLit(nearer, full, Vec2{0.3f, 0.6f}, 0.77f, 0.05f) == 1.0f);
        // Only the (0, 0) texel blocks: its weight (1 - 0.25) (1 - 0.5) is taken off.
        const std::array<float, 4> oneBlocks{0.5f, 0.5f, 0.5f, 0.9f};
        assert(near(impostorGatherLit(oneBlocks, full, Vec2{0.25f, 0.5f}, 0.0f, 0.05f), 1.0f - 0.75f * 0.5f, 1e-6f));
        assert(near(impostorShadowBias(1.5f, 32.0f), 1.5f * 2.0f / 32.0f, 1e-7f));
    }

    /*
    * The normal offset: a leaf facing the sun is not moved, one it grazes moves the whole bias off it towards the sun's
    * side (whichever side of the leaf that is), one at 60 degrees by the bias times sin 60.
    */
    void testTheSunsTestStartsOffTheLeaf() {
        const Vec3 normal{0.0f, 0.0f, 1.0f};
        const Vec3 point{0.1f, 0.2f, 0.3f};
        assert(nearVec(impostorShadowPoint(point, normal, normal, 0.1f), point, 1e-7f));
        assert(nearVec(impostorShadowPoint(point, normal, Vec3{0.0f, 1.0f, 0.0f}, 0.1f), Vec3{0.1f, 0.2f, 0.4f}, 1e-6f));
        const Vec3 fromBehind = Vec3{0.0f, std::sqrt(3.0f) * 0.5f, -0.5f};
        assert(nearVec(impostorShadowPoint(point, normal, fromBehind, 0.1f), Vec3{0.1f, 0.2f, 0.3f - 0.1f * std::sqrt(3.0f) * 0.5f}, 1e-6f));
    }

    // A quad facing +z at height z, x from -half to half, y from bottom to bottom + 2 half.
    IndexedMesh facingQuad(float half, float bottom, float z) {
        IndexedMesh mesh;

        for (const Vec2& corner : {Vec2{-1.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 2.0f}, Vec2{-1.0f, 2.0f}}) {
            MeshVertex vertex{};
            vertex.position = Vec3{corner.x * half, bottom + corner.y * half, z};
            vertex.normal = Vec3{0.0f, 0.0f, 1.0f};
            mesh.vertices.push_back(vertex);
        }

        mesh.indices = {0, 1, 2, 0, 2, 3};
        return mesh;
    }

    /*
    * A plant shading itself: a small quad 1 m in front of a large one. With the sun in front (+z, a frame's direction, or
    * tilted between frames), the large quad is dark behind the small one and lit beside it, the small one lit; with the
    * sun behind, the other way round. The front frame's depth behind the middle is the small quad's.
    */
    void testCardsShadowThemselves() {
        // The small quad 1.4 m wide: the frames round a tilted sun lie up to about 25 degrees from it, and each must still
        // see the middle behind it (the grid's spacing softens the plant's own shadow by that much).
        const IndexedMesh front = facingQuad(0.7f, 0.3f, 0.5f);
        const IndexedMesh back = facingQuad(1.0f, 0.0f, -0.5f);
        const ImpostorBake bake = bakeImpostor({ImpostorPart{.mesh = &front}, ImpostorPart{.mesh = &back}}, 8, 32, testJobs());
        assert(bake.normal.storage == TextureStorage::Exact);
        assert(nearVec(bake.centre, Vec3{0.0f, 1.0f, 0.0f}, 1e-5f) && near(bake.radius, 1.5f, 1e-5f));
        const auto inCard = [&](const Vec3& world) { return (world - bake.centre) / bake.radius; };

        const Vec3 behindFront = inCard(Vec3{0.0f, 1.0f, -0.5f});
        const Vec3 besideFront = inCard(Vec3{0.9f, 0.1f, -0.5f});
        const Vec3 onFront = inCard(Vec3{0.0f, 1.0f, 0.5f});
        const Vec3 ahead = impostorFrameDirection(Vec2{7.0f, 0.0f}, 8);
        assert(nearVec(ahead, Vec3{0.0f, 0.0f, 1.0f}, 1e-6f));

        const Vec3 facing{0.0f, 0.0f, 1.0f};

        for (const Vec3& sun : {ahead, Vec3{0.2f, 0.3f, 0.93f}.normalized()}) {
            assert(impostorSunLight(bake, 8, behindFront, facing, sun, 1.5f) < 0.05f);
            assert(impostorSunLight(bake, 8, besideFront, facing, sun, 1.5f) > 0.95f);
            assert(impostorSunLight(bake, 8, onFront, facing, sun, 1.5f) > 0.95f);
        }

        // From behind: the large quad's back is lit, the small quad in its shadow.
        const Vec3 behind = Vec3{0.0f, 0.3f, -0.95f}.normalized();
        assert(impostorSunLight(bake, 8, behindFront, facing, behind, 1.5f) > 0.95f);
        assert(impostorSunLight(bake, 8, onFront, facing, behind, 1.5f) < 0.05f);

        // The front frame (7, 0), its middle texel: the small quad, 0.5 m ahead of the middle.
        const std::size_t texel = (static_cast<std::size_t>(16) * bake.normal.width + 7 * 32 + 16) * 4 + 3;
        assert(near(impostorDepthDecode(static_cast<float>(bake.normal.pixels[texel]) / 255.0f), 0.5f / 1.5f, 1.0f / 255.0f));
    }

    /*
    * The card: four corners round the tree's middle, a radius out along x and y (so its bounds hold the sphere the shader
    * turns it in), its texture coordinates the corners (-1..1), its normal's x the radius, which the vertex shader reads.
    */
    void testTheCardCarriesItsMiddleAndRadius() {
        const IndexedMesh card = impostorCardMesh(Vec3{1.0f, 5.0f, -2.0f}, 3.0f);
        assert(card.vertices.size() == 4 && card.indices.size() == 6);

        for (const MeshVertex& vertex : card.vertices) {
            assert(std::abs(vertex.uv.x) == 1.0f && std::abs(vertex.uv.y) == 1.0f && vertex.normal.x == 3.0f);
            assert(near(vertex.position.x, 1.0f + 3.0f * vertex.uv.x, 1e-6f) && near(vertex.position.y, 5.0f + 3.0f * vertex.uv.y, 1e-6f));
            assert(vertex.position.z == -2.0f);
        }
    }

    /*
    * An impostor's material: a cut-out whose colour and normal textures are the atlases, its frames a side; the GPU
    * record flags it and carries the frames, the instance flags it for the vertex shader, and rays never meet it.
    */
    void testAnImpostorMaterialReachesTheGpu() {
        Material material = impostorMaterial(TextureHandle{3}, TextureHandle{4}, 8);
        assert(material.texture == TextureHandle{3} && material.normalTexture == TextureHandle{4} && isCutOut(material) && material.impostorFrames == 8);
        const GpuMaterial packed = packMaterial(material);
        assert((packed.extra[1] & MATERIAL_IMPOSTOR) != 0u && packed.attenuation[3] == 8.0f);

        DrawItem draw;
        draw.material = material;
        assert(packInstance(draw, 0).material[3] == 1u && rayMaskFor(draw) == 0u);
        draw.material = Material{};
        assert(packInstance(draw, 0).material[3] == 0u);
    }
}

void testImpostors() {
    testTheHemisphereFoldsOntoASquare();
    testEachFrameHasADirectionAndAPlane();
    testAViewBlendsTheFourNearestFrames();
    testAPointIsReadOnEachFramesPlane();
    testTheBakeDrawsEachFrame();
    testDepthAndTheSurfaceBehindACardPoint();
    testTheSunsTestFiltersFourTexels();
    testTheSunsTestStartsOffTheLeaf();
    testCardsShadowThemselves();
    testTheCardCarriesItsMiddleAndRadius();
    testAnImpostorMaterialReachesTheGpu();
}
