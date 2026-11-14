#pragma once

#include "assets/image_loader.h"
#include "core/pixel.h"
#include "core/settings_visitor.h"
#include "games/forest/valley_shapes.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <vector>

/*
* Hanging moss (ROADMAP phase 18): clumps of strands on crossed cut-out cards, hanging from the tips of the islands'
* roots and vines, from the islands' undersides and from under the pillars' ledges. Neither CC0 library has any, so the
* strands' texture is drawn here, coloured from the moss scan. Built once at start-up in each shape's own frame.
*/

// A clump's cards are this share of its length wide.
inline constexpr float mossClumpWidthShare = 1.0f / 3.0f;

// A ledge's lip stands out at least this far past the rock below it (metres) to carry moss.
inline constexpr float mossLipMetres = 1.0f;

/*
* The strands' texture, width x height texels, rows from the bottom (v 0) as loaded images are: strands hanging from the
* top edge, most short and a few the whole height, each a wavy line a few texels wide thinning to its tip, the colour
* given made lighter or darker per strand and lighter towards the tips; alpha 255 on a strand, 0 between (the colour
* kept there, so mips do not darken the edges). The same seed draws the same image.
*/
ImageData mossStrandImage(int width, int height, Pixel colour, std::uint32_t seed);

/*
* A clump hanging from top, appended to mesh: two vertical cards crossed at right angles, the first turned turnRadians
* about the vertical, each length long and mossClumpWidthShare of it wide, centred under top; the texture's top edge
* (v 1) at the top. Tangent across (+u), bitangent up (+v).
*/
void appendMossClump(IndexedMesh& mesh, const Vec3& top, float length, float turnRadians);

// A place moss can hang from: a point of a shape's surface and the level way out of the shape there.
struct MossPlace {
    Vec3 point;
    Vec3 outward;
};

/*
* A surface's lips: each point standing out at least least metres past the point of the same column in the row below
* it, measured level and out from its row's middle (so a ledge's lower edge, where moss hangs clear of the rock below).
*/
std::vector<MossPlace> overhangLips(const SurfaceGrid& surface, float least);

// How much moss hangs (F5 Forest, Moss; start-up).
struct ForestMossSettings {
    // The longest clump (metres); each is a random 30-100% of it, those on roots' tips half that.
    float longestMoss = 6.0f;

    // The share of roots' and vines' tips with a clump.
    float tipShare = 0.7f;

    // Clumps scattered over an island's underside for each 10 m of its radius.
    float undersidePerTenMetres = 6.0f;

    // The share of the pillars' ledges' lips (a point of the surface each, about every 2.5 m round) with a clump.
    float ledgeShare = 0.35f;
};

void visit(ForestMossSettings& settings, SettingsVisitor& visitor);

// A pillar's moss, in its own frame: a clump under ledgeShare of its lips (overhangLips at mossLipMetres).
IndexedMesh pillarMoss(const PillarPlacement& pillar, const SurfaceGrid& surface, const ForestMossSettings& settings);

/*
* An island's moss, in its own frame: a clump on tipShare of its roots' and vines' tips, and undersidePerTenMetres for
* each 10 m of radius over its underside (chosen as the roots' anchors are, more often near the rim).
*/
IndexedMesh islandMoss(const IslandPlacement& island, const SurfaceGrid& surface, const std::vector<Vec3>& tips, const ForestMossSettings& settings);
