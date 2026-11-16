#pragma once

#include "assets/image_loader.h"
#include "core/coverage_mips.h"
#include "core/pixel.h"
#include "core/settings_visitor.h"
#include "games/forest/forest_trees.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <functional>
#include <vector>

/*
* Understorey palms (ROADMAP phase 18, docs/JUNGLE.md, Palms): 2-6 m palms with feather-shaped fronds, grown here since
* no CC0 library has palm scans. Each frond's rachis (its midrib) leaves the crown rising and bends down as a tapering
* cantilever does under its own weight; its leaflets, in pairs along it, are geometry cut to their outline (narrow
* strips folded into a V along their own midrib), so nothing is drawn where there is no leaf. A few variants are grown
* at start-up and placed many times along the path, with coarser levels and the impostor cards the plants use.
*/

// The palms (F5 Forest, Palms; read at start-up).
struct ForestPalmSettings {
    // The grid the palms are thrown on (one at most a cell, jittered within it), metres.
    float spacing = 5.0f;

    // Every cell is planted within nearPathMetres of the path's centre, farShare of them past farPathMetres, a straight
    // blend between, as the understorey's.
    float nearPathMetres = 25.0f;
    float farPathMetres = 60.0f;
    float farShare = 0.3f;

    // How far palms keep from the path's centre and from every trunk's reach (metres).
    float pathClearance = 2.0f;
    float trunkClearance = 1.0f;

    // The shortest and tallest variant (metres): understorey palms are 2-6 m (Astrocaryum alatum, Geonoma, Chamaedorea).
    float shortest = 2.0f;
    float tallest = 6.0f;

    // How many variants are grown, each its own height between them and its own fronds.
    int variants = 8;

    // Each variant's fronds, between these (more on taller palms, as leaf size and count grow with the plant).
    int leastFronds = 7;
    int mostFronds = 14;

    // Leaflet pairs along each frond, and a leaflet's widest across as a share of its length.
    int leafletPairs = 30;
    float leafletWidthShare = 0.12f;

    // How far an average frond's rachis turns down from its base to its tip (degrees): the youngest fronds, near the
    // top, less, the oldest, round the outside, more.
    float droopDegrees = 70.0f;

    // How many of the sun's cascades its shadows are drawn into, nearest first (ModelRenderer::shadowCascades).
    int shadowCascades = 2;

    // The card past the levels (scene/impostor.h, docs/LOD.md), as the understorey's: the error from which a palm is
    // drawn as its card, its frames a side and each frame's texels.
    float impostorErrorMetres = 0.15f;
    int impostorFrames = 8;
    int impostorTexels = 128;
};

void visit(ForestPalmSettings& settings, SettingsVisitor& visitor);

// The fixed parts of a palm's shape (docs/JUNGLE.md, Palms).
inline constexpr float palmFrondLengthShare = 0.45f; // a frond's length, a share of the palm's height
inline constexpr float palmPetioleShare = 0.25f;     // the bare stalk, a share of the frond, before the leaflets
inline constexpr float palmLeafletShare = 0.3f;      // the longest leaflet, a share of the frond
inline constexpr float palmRachisTaper = 0.7f;       // the rachis's radius falls by this share to the tip
inline constexpr float palmScaleSpread = 0.15f;      // each palm within this share of its variant's size

/*
* The share of a frond's whole turn its rachis has turned by at along (0 at the crown, 1 at the tip). A cantilever under
* an even load bends with curvature M / EI: the moment M grows as (1 - s)^2 towards the base, and a rachis tapering to
* (1 - palmRachisTaper) of its radius has a stiffness EI going as its radius^4, so the curvature goes as
* (1 - s)^2 / (1 - palmRachisTaper s)^4, an arch bending most in its middle; its integral from 0 to along, over its
* integral to 1. 0 at 0, 1 at 1, never falling.
*/
float frondBendShare(float along);

// A leaflet's length along the leafy part of a frond (0 nearest the crown, 1 at the tip), a share of the longest: a
// spindle, longest in the middle and a third of that at either end.
float palmLeafletLength(float along);

// A leaflet's half-width at along its length (0 at the rachis, 1 at its tip), a share of its widest: narrow at its base,
// widest a little before its middle, coming to a point.
float palmLeafletWidth(float along);

// One variant's shape: its height (metres, to the top of its highest frond), stem, fronds, droop and leaflets.
struct PalmShape {
    float height = 4.0f;
    float stemHeight = 2.0f;
    float stemRadius = 0.03f;
    int fronds = 10;
    float frondLength = 1.8f;
    float droopRadians = 1.2f;
    int leafletPairs = 30;
    float leafletWidthShare = 0.12f;
    std::uint32_t seed = 0;
};

// A variant's height, evenly spread from the shortest to the tallest: variant + 0.5 of variants along the way.
float palmVariantHeight(const ForestPalmSettings& settings, int variant);

// A variant's shape from the settings and its seed: its height, its fronds more the taller it is, its stem the height
// left under the crown.
PalmShape palmVariantShape(const ForestPalmSettings& settings, int variant, std::uint32_t seed);

/*
* How finely a palm is drawn: the share of its leaflet pairs kept (each made as much wider, so the frond covers about as
* much), each leaflet's segments along it, and the rachis's and stem's sides round.
*/
struct PalmDetail {
    float pairShare = 1.0f;
    int leafletSegments = 4;
    int rachisSides = 4;
    int stemSides = 6;
};

/*
* A strap leaf's shape beyond its size: folded along its midrib into a V opening down, each half foldRadians below
* level, and drooping under its own weight, its midrib's tip falling droop of its length (a parabola, so a leaf rising
* steeply arches over when droop is large).
*/
struct StrapFold {
    float foldRadians = 0.0f;
    float droop = 0.0f;
};

/*
* One strap leaf appended to mesh (a palm's leaflet, an epiphyte's frond): from base along direction, length long, its
* widest width across (palmLeafletWidth's outline), its midrib's up lift (made square to it), segments along it, folded
* and drooping as fold says. u runs across it (0 and 1 at the edges, 0.5 on the midrib), v from its base to its tip;
* each triangle faces the way its corners' normals do.
*/
void appendStrapLeaf(IndexedMesh& mesh, const Vec3& base, const Vec3& direction, const Vec3& lift, float length, float width, int segments,
                     const StrapFold& fold);

/*
* A frond's rachis: count + 1 points from base, length long, leaving at azimuth (radians round up from +x towards -z, as
* turns about +y go) and elevation (radians above level), turning down by droop * frondBendShare(s) as it goes, in the
* upright plane of its azimuth.
*/
std::vector<Vec3> frondSpine(const Vec3& base, float azimuth, float elevation, float length, float droop, int count);

/*
* A feather-shaped frond (a palm's, a fern's): its rachis from base at azimuth and elevation, length long, turning down
* by droop (frondSpine), a tapering tube of rachisSides; its leaflets in pairs along its leafy part (past
* palmPetioleShare), each a folded strap leaf (appendStrapLeaf) of leafletSegments, palmLeafletShare of the frond at
* their longest (palmLeafletLength along it), leafletWidthShare of their length across, angled forward and lifted out
* of the frond's plane, each pair's angle and length jittered by random's draws (two a leaflet, in order). The tube
* reads the texture's midrib column (u 0.5).
*/
struct PinnateFrond {
    Vec3 base;
    float azimuth = 0.0f;
    float elevation = 0.0f;
    float length = 1.0f;
    float droop = 0.0f;
    int pairs = 30;
    float leafletWidthShare = 0.12f;
    int rachisSides = 4;
    int leafletSegments = 4;
};

void appendPinnateFrond(IndexedMesh& mesh, const PinnateFrond& frond, const std::function<float()>& random);

/*
* One palm in its own frame, footed at the origin and up +y, at the shape's size before it is fitted to its height: its
* stem (a tube from just below the ground to the crown), and its fronds round the crown by the golden angle, the
* youngest rising steepest; each frond's rachis a tapering tube and its leaflets pairs of folded strips. Leaflets' u runs
* across them (0 and 1 at the edges, 0.5 on the midrib) and v from the rachis to the tip; the tubes read the texture's
* midrib column (u 0.5). The same shape and detail grow the same palm.
*/
IndexedMesh growPalm(const PalmShape& shape, const PalmDetail& detail);

// A palm's seen mesh, its coarser level and its shadow's stand-in (a quarter of the pairs), each with its error
// (metres), all fitted to the shape's height together.
struct PalmMeshes {
    IndexedMesh palm;
    IndexedMesh level;
    IndexedMesh shadow;
    float levelError = 0.0f;
    float shadowError = 0.0f;
};

PalmMeshes growPalmMeshes(const PalmShape& shape);

// Where a palm stands: its foot (sunk a little into the ground), its size against its variant's, its turn about up and
// its variant.
struct PalmPlacement {
    Vec3 foot;
    float scale = 1.0f;
    float turnRadians = 0.0f;
    int variant = 0;
};

/*
* The palms' places, as the understorey's (plantedCells): a jittered grid spacing apart over the trees' stretch, thick
* by the path and thinning away from it, clear of the path and every trunk, none whose nominal size meets a sight line.
* The same every run.
*/
std::vector<PalmPlacement> placePalms(const ForestPalmSettings& settings, const std::vector<StandingCrown>& trunks,
                                      const std::vector<SightLine>& sightLines);

// A palm's nominal sphere in the world, its height across (for the sight lines).
Vec3 palmCentre(const ForestPalmSettings& settings, const PalmPlacement& palm);
float palmRadius(const ForestPalmSettings& settings, const PalmPlacement& palm);

/*
* A palm leaflet's pictures, texels square, u across the leaflet and v along it (rows from the bottom, v 0 at its base,
* as loaded images are): its colour (sRGB, alpha 255 everywhere: the leaflet's outline is its geometry) is the colour
* given, with a paler, yellower midrib down u 0.5, parallel veins either side of it, faint streaks along them and a dry,
* browner tip; its height (grey, the red channel read: 0 the deepest) has the midrib and veins as ridges, for its normal
* map. The same seed draws the same pictures.
*/
struct PalmLeafletImages {
    ImageData colour;
    ImageLevel height;
};

PalmLeafletImages palmLeafletImages(int texels, Pixel colour, std::uint32_t seed);
