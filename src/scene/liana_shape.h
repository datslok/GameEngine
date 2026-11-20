#pragma once

#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"
#include "math/transform.h"

#include <cstdint>
#include <memory>
#include <vector>

/*
* A liana drawn by the GPU (ROADMAP phase 18, d2b; docs/research/VINES.md, At valley scale): one record a liana, from
* which the vertex shaders bend one shared template tube onto its catenary, its sway worked out there too. The curve's
* maths mirrored by liana.glsl, exact to rounding (tests/liana_shape_test.cpp, tests/gpu/liana_gpu_test.cpp).
*/

/*
* One liana as the shaders read it (liana.glsl's LianaShape, std430, 96 bytes). The catenary in the upright plane
* through its holds, by its a and u0, the slope (sinh) at the first hold; a = 0 a straight line. The trees it hangs from,
* each moving as that tree's crown does there (WIND_CATEGORY_TREES); a second scale of 0 leaves the far end fixed (a
* rope's foot in the ground).
*/
struct LianaShape {
    float from[4]{};       // xyz the first hold, w the radius
    float to[4]{};         // xyz the far end, w the length along it
    float curve[4]{};      // x a (0 straight), y u0, z its deepest sag below its chord (its swing's length), w the bark's tile in metres
    float firstTree[4]{};  // xyz the first tree's foot, w its height in its mesh's units
    float secondTree[4]{}; // xyz the second tree's foot, w its height
    float trees[4]{};      // x the first tree's scale, y the second's (0: fixed), z the trees' bend
};

static_assert(sizeof(LianaShape) == 96, "LianaShape must match liana.glsl's LianaShape");

// A tree a liana hangs from: its foot, its mesh's height and its scale (as its WindSway and placement).
struct LianaTree {
    Vec3 foot;
    float height = 1.0f;
    float scale = 1.0f;
};

/*
* The catenary of length total hanging between two holds d apart across and h apart up (h above 0: the far one higher),
* in the upright plane through them: 2 a sinh(d / 2a) = sqrt(L^2 - h^2) fixes a (the left side falls from infinity to
* d as a grows, so halving finds it, in doubles since sinh of d / 2a overflows floats for tight curves), and its lowest
* point lies x0 = d / 2 - a atanh(h / L) across from the first hold. Only for a length above the straight line and holds
* apart across.
*/
struct CatenaryFit {
    double a = 0.0;
    double x0 = 0.0;
};

CatenaryFit fitCatenary(double d, double h, double total);

/*
* A liana's record: the catenary of length metres from from to to, solved as catenaryStrand solves it (in doubles); a
* length no longer than the straight line, or holds one above the other, a straight line. Its first tree, and its
* second (none: its far end fixed), bending by bend; its bark tiled every barkTileMetres along it.
*/
LianaShape makeLianaShape(const Vec3& from, const Vec3& to, float length, float radius, float barkTileMetres, const LianaTree& first,
                          const LianaTree* second, float bend);

// A point of a liana's centre line: its offset from the first hold, its unit direction along, the metres along to it.
struct LianaPoint {
    Vec3 offset;
    Vec3 along;
    float travelled = 0.0f;
};

/*
* The centre line at share (0..1) of the length (lianaCentre in liana.glsl). With u the slope there (u0 + s / a), the
* rise y = s (u + u0) / (c + c0) and the run x = a asinh(s / a (c0 - u0 (u + u0) / (c + c0))), c = sqrt(1 + u^2): the
* catenary's a cosh and asinh rewritten without differences of large numbers, so a nearly taut liana (a huge) keeps
* its precision in floats.
*/
LianaPoint lianaCentre(const LianaShape& shape, float share);

// A vertex of a liana's tube: its offset from the first hold, its outward normal, its tangent round it (+u, w +1),
// its texture place, its share along and how far it hangs below the chord (its arm).
struct LianaVertex {
    Vec3 offset;
    Vec3 normal;
    Vec3 tangent;
    Vec2 uv;
    float along = 0.0f;
    float arm = 0.0f;
};

/*
* A template vertex placed (lianaVertex in liana.glsl): share along it, the ring's cos and sin round it, u round it
* (0..1, the seam's column twice). The ring square to the line, its axes the level side (across the upright plane)
* and the side cross the line, so it never twists; v along it in bark tiles; as appendTube's.
*/
LianaVertex lianaVertex(const LianaShape& shape, float share, float around, float acrossCos, float acrossSin);

// The level direction across the upright plane from the first hold (x when the holds are one above the other).
Vec3 lianaRun(const LianaShape& shape);

/*
* The template tube: rings + 1 rings of sides + 1 vertices (the seam's twice), each vertex's position (share, cos, sin)
* and its u, triangles facing out of the tube in its own frame (x along, y and z round): the tube lianaVertex bends.
*/
IndexedMesh lianaTemplateMesh(int rings, int sides);

/*
* Where a liana's template stands, as an entity's Transform: the template's box ([0, 1] along, [-1, 1] round) mapped
* onto a box holding the whole liana and its tube, turned to the run, so its bounds, culling and level are its own.
*/
Transform lianaPose(const LianaShape& shape);

/*
* Component: the lianas the GPU draws (FrameDescription::lianaShapes; the first placed counts). Slots of perSlot
* records each, replaced whole when their cell changes, so the renderer copies only the slots whose pointer changed. A
* ModelRenderer's lianaShape names one: slot * perSlot + its place, plus 1.
*/
struct LianaShapes {
    std::vector<std::shared_ptr<const std::vector<LianaShape>>> slots;
    std::uint32_t perSlot = 0;
};
