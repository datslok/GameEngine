#pragma once

#include "scene/image_data.h"
#include "core/job_system.h"
#include "core/pixel.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/asset_handles.h"
#include "scene/indexed_mesh.h"
#include "scene/material.h"

#include <array>
#include <cstdint>
#include <vector>

/*
* Impostors (docs/research/LOD.md, Impostors; Brucks 2018, Epic's Impostor Baker): a far tree drawn as one card facing the
* camera, showing pictures of the tree baked from frames x frames directions over the upper hemisphere. The four frames
* nearest the view are blended, each read where the card's point falls on that frame's own plane, and lit with the
* normals baked beside the colour, so the sun still moves over them. The maths here is mirrored in impostor.glsl.
*/

// The upper hemisphere folded onto the unit square (straight up the middle, the horizon its edge); below the horizon
// counts as on it.
Vec2 hemiOctahedralEncode(const Vec3& direction);
Vec3 hemiOctahedralDecode(const Vec2& square);

// A frame's direction (towards the viewer, in the tree's frame): its grid place (column, row), frames a side, corner
// to corner over the square.
Vec3 impostorFrameDirection(const Vec2& frame, int frames);

// A frame's plane: right level with the ground (along x seen from straight above), up square to it and the direction.
struct ImpostorBasis {
    Vec3 right;
    Vec3 up;
};
ImpostorBasis impostorFrameBasis(const Vec3& direction);

// The four frames round a view direction on the grid (column, row) and their bilinear weights.
struct ImpostorBlend {
    std::array<Vec2, 4> frames;
    std::array<float, 4> weights;
};
ImpostorBlend impostorFrameBlend(const Vec3& viewDirection, int frames);

// Where a card point (its offset from the tree's middle over the radius, in the tree's frame) falls in a frame, 0..1.
Vec2 impostorFrameUv(const Vec3& cardPoint, const Vec3& frameDirection);

// A frame's texture coordinates in the atlas: frame (column, row) of frames a side.
Vec2 impostorAtlasUv(const Vec2& frameUv, const Vec2& frame, int frames);

// One part of what is baked: its mesh (in the tree's frame), its colour texture (none: tint alone), its colour (sRGB
// bytes) and, for a cut-out, the alpha below which its texture is a hole (0: solid).
struct ImpostorPart {
    const IndexedMesh* mesh = nullptr;
    const ImageData* colour = nullptr;
    Pixel tint{255, 255, 255};
    float alphaCutoff = 0.0f;
};

/*
* Depth in a frame (docs/research/LOD.md, Self-shadowed impostors): how far the surface seen lies along the frame's direction
* from the tree's middle, over the radius (-1..1), stored in the normal atlas's alpha as depth * 0.5 + 0.5.
*/
std::uint8_t impostorDepthEncode(float depth);
float impostorDepthDecode(float stored);

// The surface a frame sees behind a card point: the point moved along the frame's direction to the depth there (both
// over the radius, in the tree's frame).
Vec3 impostorSurfacePoint(const Vec3& cardPoint, const Vec3& frameDirection, float depth);

/*
* How much of the sun reaches a point at pointDepth along a sun frame's direction, from the four texels round it
* (textureGather's order: (0, 1), (1, 1), (1, 0), (0, 0) of the 2 x 2), each lit where it is empty (covered below 0.5) or
* its stored depth lies no nearer the sun than the point plus the bias; filtered bilinearly by fraction.
*/
float impostorGatherLit(const std::array<float, 4>& storedDepths, const std::array<float, 4>& covered, const Vec2& fraction, float pointDepth,
                        float bias);

// The bias in depth (over the radius) of biasTexels of a frame frameTexels a side, which spans two radii.
float impostorShadowBias(float biasTexels, float frameTexels);

/*
* The point the sun's test is made from (normal offset, Holbein 2011): moved off its leaf (normal, unit, either side) to
* the side facing the sun by the bias times the sine of the angle between them, so a leaf the sun grazes is not
* shadowed by its own texels, whose depths spread most there.
*/
Vec3 impostorShadowPoint(const Vec3& point, const Vec3& normal, const Vec3& sunDirection, float bias);

/*
* One of the IMPOSTOR_PLANT_SHADOW_SAMPLES points (impostor_shared.h) a card's scene sun shadow is read at and averaged
* (LevelOfDetailSettings::impostorShadowSpread), over the radius in the tree's frame: 0 the middle, then +x, -x, +y, -y,
* +z, -z, each spread from it. A card is one plane standing in for a cloud of leaves; read at the leaves it shows, a
* whole card could lie in a sun fleck its mesh's leaves mostly miss. Spread over the plant, the card takes about their
* average, one value a card (coarse, as engines shadow far cards).
*/
Vec3 impostorPlantShadowSample(int index, float spread);

// The baked atlases (frames x frames frames of frameTexels a side): colour (sRGB, alpha 255 where something was) and
// the normals in the tree's frame (linear bytes, n * 0.5 + 0.5) with the depth in alpha (impostorDepthEncode; kept
// uncompressed, TextureStorage::Exact), and the sphere they fill.
struct ImpostorBake {
    ImageData colour;
    ImageData normal;
    Vec3 centre;
    float radius = 0.0f;
};

/*
* Each frame the parts drawn orthographically along its direction over the sphere round them, nearest surface kept,
* a cut-out's holes skipped, a double-sided leaf's normal turned to the viewer; then the colour of empty texels spread
* from their neighbours (alpha left 0), so the mips do not darken the edges. A row of frames a job.
*/
ImpostorBake bakeImpostor(const std::vector<ImpostorPart>& parts, int frames, int frameTexels, JobSystem& jobs);

/*
* The plant's own shadow at a point on a leaf (over the radius, in the tree's frame; its normal, unit, zero for none) from
* the sun (towards it, unit, tree's frame), 1 lit: from impostorShadowPoint, the four frames round the sun's direction
* read as shadow maps (impostorGatherLit), blended bilinearly as a view blends them. A point off a frame's square is lit
* in it. impostorSunLight in impostor_sampling.glsl reads the GPU's atlases the same way; this reads the bake's, for tests.
*/
float impostorSunLight(const ImpostorBake& bake, int frames, const Vec3& point, const Vec3& normal, const Vec3& sunDirection, float biasTexels);

// The card: four corners a radius round the middle along x and y, texture coordinates the corners (-1..1), the normal's
// x the radius (the vertex shader turns it to face the viewer); two triangles.
IndexedMesh impostorCardMesh(const Vec3& centre, float radius);

// The colour atlas's cutoff: its alpha is 1 where the tree was and 0 elsewhere, and its mips keep that coverage.
inline constexpr float impostorCoverageCutoff = 0.5f;

// An impostor's material: a double-sided cut-out of the colour atlas, the normal atlas in the normal map's place.
Material impostorMaterial(TextureHandle colour, TextureHandle normal, int frames);
