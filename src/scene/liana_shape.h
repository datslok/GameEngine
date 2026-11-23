#pragma once

#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"
#include "scene/liana_shared.h"
#include "scene/sway_bones.h"
#include "math/transform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

/*
* A liana drawn by the GPU (ROADMAP phase 18, d2b; docs/research/VINES.md, At valley scale): one record a liana, from
* which the vertex shaders bend one shared template tube onto its catenary, its sway worked out there too. The curve's
* maths mirrored by liana.glsl, exact to rounding (tests/liana_shape_test.cpp, tests/gpu/liana_gpu_test.cpp).
*/

/*
* One liana as the shaders read it (liana.glsl's LianaShape, std430, 144 bytes). The catenary in the upright plane
* through its holds, by its a and u0, the slope (sinh) at the first hold; a = 0 a straight line. The trees it hangs from,
* each moving as that tree's crown does there (WIND_CATEGORY_TREES); a second scale of 0 leaves the far end fixed (a
* rope's foot in the ground). Its ivy (ROADMAP d2c): dense where it runs through either crown, sparse between. Its
* anchors on its trees' bones (ROADMAP phase 21 stage 3c-iii-c-iii): each hold's binding and its tree's planting layer,
* by which the GPU finds the tree among its planted plants (by its foot) and moves the hold as the bark there.
*/
struct LianaShape {
    float from[4]{};       // xyz the first hold, w the radius
    float to[4]{};         // xyz the far end, w the length along it
    float curve[4]{};      // x a (0 straight), y u0, z its deepest sag below its chord (its swing's length), w the bark's tile in metres
    float firstTree[4]{};  // xyz the first tree's foot, w its height in its mesh's units
    float secondTree[4]{}; // xyz the second tree's foot, w its height
    float trees[4]{};      // x the first tree's scale, y the second's (0: fixed), z the trees' bend
    float leaves[4]{};     // x the metres from the first hold inside its crown, y from the far end inside the second's, z the ivy's spacing there, w along the rest
    float leafLook[4]{};   // x a leaf's length (0: no ivy), y the distance its cards start thinning, z the distance they are gone by, w its seed (below 2^24, exact)
    std::uint32_t anchors[4]{}; // x the first hold's binding (packSwayBinding), y the far end's (SWAY_UNBOUND none), z w their trees' layers (LIANA_NO_TREE none)
};

static_assert(sizeof(LianaShape) == 144, "LianaShape must match liana.glsl's LianaShape");

/*
* A tree a liana hangs from: its foot, its mesh's height, scale and turn (as its WindSway and placement), its planting
* layer (LIANA_NO_TREE: none, the hold not moved by bones) and the binding of the hold to its bones.
*/
struct LianaTree {
    Vec3 foot;
    float height = 1.0f;
    float scale = 1.0f;
    float turnRadians = 0.0f;
    std::uint32_t layer = LIANA_NO_TREE;
    SwayBinding binding{};
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
* A liana's ivy (setLianaLeaves): the metres from each end that run through its crown, the spacing of the leaves there
* and along the rest, a leaf's length, the distances from the eye its cards thin from and are gone by (0: never), and
* its seed.
*/
struct LianaLeaves {
    float firstCrownMetres = 0.0f;
    float lastCrownMetres = 0.0f;
    float spacing = 0.0f;
    float sparseSpacing = 0.0f;
    float size = 0.0f;
    float thinFrom = 0.0f;
    float goneBy = 0.0f;
    std::uint32_t seed = 0;
};

// A record's ivy: the crowns' runs clamped to its length (the far one to what the first leaves), the seed's low 24 bits.
void setLianaLeaves(LianaShape& shape, const LianaLeaves& leaves);

/*
* The leaves its ivy holds (lianaLeafCount in liana.glsl): N = the crowns' metres / spacing + the rest / sparse spacing,
* the integral of the leaves' density along it; 0 without ivy. Its cards are the template's first ceil(N).
*/
float lianaLeafCount(const LianaShape& shape);

/*
* The metres along it of card (0 to ceil(N) - 1), jitter (0..1) its random stray: the count C(s) of leaves up to s
* metres along is a line in three pieces (steep in the crowns, shallow between), so the card the count reaches at
* t = (card + 1/2 + stray) N / ceil(N) stands at C^-1(t): every card used, dense in the crowns.
*/
float lianaLeafPlace(const LianaShape& shape, float card, float jitter);

/*
* How much of its length a card shows with its stem distance metres from the eye: 1 near; past the thinning distance
* every other card shrinks away over LIANA_LEAF_FADE_BAND of it while the rest grow by sqrt 2 (the same leaf area); all
* shrink away over the band before the last distance; 0 for cards past the ivy's ceil(N).
*/
float lianaLeafGrowth(const LianaShape& shape, float card, float distance);

// A corner of an ivy card: its offset from the first hold, its stem's (where it sways), its normal, its tangent across
// (+u, w +1), its texture place in the atlas, its share along and arm below the chord at its stem.
struct LianaLeafVertex {
    Vec3 offset;
    Vec3 stem;
    Vec3 normal;
    Vec3 tangent;
    Vec2 uv;
    float along = 0.0f;
    float arm = 0.0f;
};

/*
* A corner of an ivy card placed (lianaLeafVertex in liana.glsl): across (0..1, from its left edge) and up (0..1, stem
* to tip) the card, as appendLeafCard places it. Its stem on the tube's surface at its place, pointing out at a random
* turn round it and drooping (LIANA_LEAF_DROOP), its leaf one of the atlas's six at random; grown by lianaLeafGrowth at
* its stem's distance from eye (a world point), so a card grown to nothing collapses onto its stem and draws nothing.
*/
LianaLeafVertex lianaLeafVertex(const LianaShape& shape, float card, float across, float up, const Vec3& eye);

/*
* The ivy's template: cards cards of 4 corners and 2 triangles each; a corner's uv is (across + 2 card, up), so the
* card and its corner reach the shadow passes too, and its position is spread over the template's box ([0, 1] along,
* [-1, 1] round) for the template's bounds.
*/
IndexedMesh lianaLeafTemplateMesh(int cards);

// The ivy templates' sizes, and the smallest holding count cards (the largest when none does).
inline constexpr std::array<int, 5> lianaLeafTemplateCards{16, 32, 64, 128, 256};
std::size_t lianaLeafTemplate(float count);

// Where a liana's ivy template stands: lianaPose's box grown by the farthest a grown card reaches out from the tube.
Transform lianaLeafPose(const LianaShape& shape);

// A draw's liana name (ModelRenderer::lianaShape): its record's index, and whether it draws the ivy (LIANA_IVY_BIT).
std::uint32_t lianaRecordOf(std::uint32_t named);
bool lianaDrawsIvy(std::uint32_t named);

/*
* Component: the lianas the GPU draws (FrameDescription::lianaShapes; the first placed counts). Slots of perSlot
* records each, replaced whole when their cell changes, so the renderer copies only the slots whose pointer changed. A
* ModelRenderer's lianaShape names one: slot * perSlot + its place, plus 1, with LIANA_IVY_BIT for its ivy.
*/
struct LianaShapes {
    std::vector<std::shared_ptr<const std::vector<LianaShape>>> slots;
    std::uint32_t perSlot = 0;
};
