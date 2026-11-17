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

// The baked atlases (frames x frames frames of frameTexels a side): colour (sRGB, alpha 255 where something was) and
// the normals in the tree's frame (linear bytes, n * 0.5 + 0.5), and the sphere they fill.
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

// The card: four corners a radius round the middle along x and y, texture coordinates the corners (-1..1), the normal's
// x the radius (the vertex shader turns it to face the viewer); two triangles.
IndexedMesh impostorCardMesh(const Vec3& centre, float radius);

// The colour atlas's cutoff: its alpha is 1 where the tree was and 0 elsewhere, and its mips keep that coverage.
inline constexpr float impostorCoverageCutoff = 0.5f;

// An impostor's material: a double-sided cut-out of the colour atlas, the normal atlas in the normal map's place.
Material impostorMaterial(TextureHandle colour, TextureHandle normal, int frames);
