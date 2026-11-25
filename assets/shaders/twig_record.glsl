// A twig's record and matrix, a far leaf's record and vertices, and a twig's share while its crown fades into its card
// (render/gpu/twig_records.h), included by twig_drawing.glsl and, for the mesh shaders' twig instances, drawn_objects.glsl.
#ifndef TWIG_RECORD_GLSL
#define TWIG_RECORD_GLSL

#include "card_thinning.glsl"
#include "hash.glsl"
#include "level_fade.glsl"
#include "quat.glsl"
#include "render/gpu/twig_drawing_shared.h"
#include "scene/twig_far_levels_shared.h"

// A twig: its matrix's three rows (prototype frame to plant frame), its lean and prototype. Must match GpuTwig.
struct Twig {
    vec4 rows[3];
    vec3 lean;
    uint prototype;
};

// The twig's matrix (its rows, then 0 0 0 1), column by column as GLSL holds it.
mat4 twigMatrix(Twig twig) {
    return transpose(mat4(twig.rows[0], twig.rows[1], twig.rows[2], vec4(0.0, 0.0, 0.0, 1.0)));
}

// A crown fading into its card (ROADMAP phase 21 stage 3g): the least rank leaves every twig gone by the fade's last step.
const float TWIG_FADE_LEAST_RANK = float(CARD_THINNING_BAND) + 1.0 / float(DRAW_LIST_FADE_STEPS);

// A twig's rank, fixed by its plant (its pick's bits) and its number: the hash's top 24 bits, TWIG_FADE_LEAST_RANK to 1.
float twigFadeRank(uint plant, uint twig) {
    uint hashed = pcg3d(uvec3(plant, twig, TWIG_FADE_HASH_USE)).x >> 8u;
    return TWIG_FADE_LEAST_RANK + (1.0 - TWIG_FADE_LEAST_RANK) * float(hashed) * (1.0 / 16777216.0);
}

// How whole a twig of this rank is while its part's entry fades: 1 when not fading or for the coarser level; else its
// presence at the kept share, 1 less the fade's share. Opaque twigs shrink away instead of taking the dither's discard.
float twigFadePresence(uint fadeField, float rank) {
    if (fadeField == 0u || (fadeField & DRAW_LIST_FADE_COARSE) != 0u) {
        return 1.0;
    }

    return cardPresence(rank, 1.0 - levelFadeThreshold(fadeField));
}

// A far leaf (ROADMAP phase 21 stage 3e-ii): its foot, its frame's quaternion (four snorm16), its side (half), cell and
// number (a byte each), its twig. Must match GpuFarLeaf (28 B).
struct FarLeaf {
    float foot[3];
    uint turn[2];
    uint sideCellNumber;
    uint twig;
};

// An atlas cell's one-row leaf: base, tip, row height, left half width; right half width, the fold's raise, curl;
// its texture rectangle. Must match GpuFarLeafShape (48 B).
struct FarLeafShape {
    vec4 shape;
    float right;
    float raise;
    float curl;
    float unused;
    vec4 uvRect;
};

// A far leaf's vertex in the plant's frame (FarLeafVertex).
struct FarLeafVertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
    vec4 tangent;
};

// A one-row leaf's corners in its cell's units (across the midrib, up it): base, left edge, midrib, right edge, tip.
vec2 farLeafCorner(FarLeafShape shape, uint vertex) {
    if (vertex == 0u) {
        return vec2(0.0, shape.shape.x);
    }

    if (vertex == 1u) {
        return vec2(-shape.shape.w, shape.shape.z);
    }

    if (vertex == 2u) {
        return vec2(0.0, shape.shape.z);
    }

    if (vertex == 3u) {
        return vec2(shape.right, shape.shape.z);
    }

    return vec2(0.0, shape.shape.y);
}

// A far leaf's triangle as its three vertices, wound as appendModelledLeaf winds a one-row leaf (farLeafTriangle).
uvec3 farLeafTriangle(uint triangle) {
    if (triangle == 0u) {
        return uvec3(0u, 2u, 1u);
    }

    if (triangle == 1u) {
        return uvec3(0u, 3u, 2u);
    }

    if (triangle == 2u) {
        return uvec3(1u, 2u, 4u);
    }

    return uvec3(2u, 3u, 4u);
}

/*
* A far leaf's vertex (farLeafVertex): the frame from the record's quaternion, every corner placed (raised by the fold,
* dropped by the curl) so the vertex's normal can sum the facings of the triangles it touches, its texture coordinate
* numbered by its leaf, its tangent across it.
*/
FarLeafVertex farLeafVertex(FarLeaf leaf, FarLeafShape shape, uint vertex) {
    vec4 turn = normalize(vec4(unpackSnorm2x16(leaf.turn[0]), unpackSnorm2x16(leaf.turn[1])));
    vec3 across = quatRotate(turn, vec3(1.0, 0.0, 0.0));
    vec3 towardsTip = quatRotate(turn, vec3(0.0, 1.0, 0.0));
    vec3 facing = quatRotate(turn, vec3(0.0, 0.0, 1.0));
    vec3 foot = vec3(leaf.foot[0], leaf.foot[1], leaf.foot[2]);
    float side = unpackHalf2x16(leaf.sideCellNumber).x;
    float number = float(leaf.sideCellNumber >> 24u);
    float leafLength = max(shape.shape.y - shape.shape.x, 1e-6);
    uint chosen = min(vertex, uint(FAR_LEAF_VERTICES) - 1u);
    vec3 points[FAR_LEAF_VERTICES];

    for (uint corner = 0u; corner < uint(FAR_LEAF_VERTICES); ++corner) {
        vec2 at = farLeafCorner(shape, corner);
        float t = clamp((at.y - shape.shape.x) / leafLength, 0.0, 1.0);
        float z = abs(at.x) * shape.raise - shape.curl * t * t;
        points[corner] = foot + (across * at.x + towardsTip * at.y + facing * z) * side;
    }

    vec3 normal = vec3(0.0);

    for (uint triangle = 0u; triangle < uint(FAR_LEAF_TRIANGLES); ++triangle) {
        uvec3 corners = farLeafTriangle(triangle);

        if (corners.x == chosen || corners.y == chosen || corners.z == chosen) {
            normal += cross(points[corners.y] - points[corners.x], points[corners.z] - points[corners.x]);
        }
    }

    vec2 at = farLeafCorner(shape, chosen);
    float uMin = shape.uvRect.x + number;
    float uMax = shape.uvRect.z + number;
    FarLeafVertex made;
    made.position = points[chosen];
    made.normal = dot(normal, normal) > 1e-20 ? normalize(normal) : facing;
    made.uv = vec2(uMin + (uMax - uMin) * (0.5 + at.x), shape.uvRect.y + (shape.uvRect.w - shape.uvRect.y) * at.y);
    made.tangent = vec4(across, 1.0);
    return made;
}

#endif
