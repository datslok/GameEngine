#pragma once

#include "core/settings_visitor.h"
#include "games/forest/island_hangings.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <functional>
#include <vector>

/*
* Lianas, the woody vines hanging in the jungle patch (docs/research/VINES.md, stage 1): loops sagging from one crown into a
* neighbour's, and ropes hanging from a crown to the ground, as old lianas are left when their hosts grow and fall. Each
* hangs as a catenary between its two holds, its thickness drawn from Barro Colorado's census, swept into a bark tube,
* (its ivy, dense where it runs through a crown, returns in ROADMAP d2c). Drawn by the GPU from a record each
* (scene/liana_shape.h), in the near cells round the player (near_trees.h).
*/

// The most metres between a liana's points: each ring of its tube.
inline constexpr float lianaStepMetres = 0.5f;

// How far a rope's foot is sunk below the ground, so its tube ends inside it.
inline constexpr float lianaGroundSink = 0.2f;

// The sun's cascades lianas cast into (ModelRenderer::shadowCascades): thin lines the coarser texels cannot show.
inline constexpr int lianaShadowCascades = 2;

/*
* A liana's stem across in metres for a share (0..1) of all lianas, rising with it, by Barro Colorado's 50 ha census
* (Schnitzer et al. 2012): half 1-2 cm, 42% 2-5 cm, 7% 5-10 cm, 1% 10-20 cm, even within each band.
*/
float lianaDiameter(float share);

/*
* A catenary of length metres from from to to (a heavy flexible line hanging between two holds: y = a cosh(x / a) in
* the upright plane through them), its points evenly spaced along it no more than step apart, starting on from and
* ending on to, each of radius radius. A length no longer than the straight line, or holds one above the other, give
* the straight line.
*/
HangingStrand catenaryStrand(const Vec3& from, const Vec3& to, float length, float radius, float step = lianaStepMetres);

/*
* A tree's crown lianas hang from: its envelope in the world (an ellipsoid round centre, radius across and halfHeight up
* and down: a canopy crown is about three times wider than tall), the tree's identity (the same tree has the same one
* whichever box planted it: near_trees.h), and whether it hangs lianas of its own here (a neighbour only takes loops).
*/
struct LianaCrown {
    Vec3 centre;
    float radius = 0.0f;
    float halfHeight = 0.0f;
    std::uint64_t identity = 0;
    bool carries = true;
};

// Whether a point lies inside a crown's envelope.
bool insideLianaCrown(const LianaCrown& crown, const Vec3& point);

/*
* A hold in a crown's lower part for three random shares: round its centre at a turn, 25-65% of its half-height down,
* and 30-70% of the way out to the envelope's edge at that depth, so always inside its leaves and below its centre.
*/
Vec3 lianaCrownHold(const LianaCrown& crown, float turn, float out, float down);

/*
* One liana: from a hold in the lower part of crown, to a hold in otherCrown's (a loop) or to the ground (a rope,
* otherCrown -1), length metres along, radius metres round, and its own seed (its leaves).
*/
struct Liana {
    Vec3 from;
    Vec3 to;
    float length = 0.0f;
    float radius = 0.0f;
    int crown = 0;
    int otherCrown = -1;
    std::uint32_t seed = 0;
};

// How many lianas and how they hang (F5 Forest, Lianas; start-up: the meshes are built once).
struct ForestLianaSettings {
    // The share of crowns carrying lianas (Barro Colorado: about half to three quarters of trees), and of those within
    // pathReach metres of the path, since forest edges carry more. Above the census's for a lush look (the user's).
    float treeShare = 0.8f;
    float pathShare = 0.95f;
    float pathReach = 15.0f;

    // Each carrying crown has 1 to this many.
    int mostPerTree = 8;

    // The census's thicknesses times this: at 1, half are 1-2 cm across, under a pixel 20 m off, so the patch hardly
    // read as vine-laden (the user chose them a little thicker).
    float thickness = 2.0f;

    // The share hanging to the ground; the rest loop to a crown whose centre is within neighbourSpan metres across
    // (ropes too when there is none).
    float ropeShare = 0.35f;
    float neighbourSpan = 30.0f;

    // A loop's sag below its chord, a random share of its span between these; a rope's most length beyond straight.
    float leastSag = 0.1f;
    float mostSag = 0.4f;
    float ropeSlack = 0.05f;

    // The metres between ivy leaves where a liana runs through a crown and along the rest of it, and a leaf's length.
    float leafSpacing = 0.35f;
    float sparseLeafSpacing = 1.2f;
    float leafSize = 0.3f;
};

void visit(ForestLianaSettings& settings, SettingsVisitor& visitor);

/*
* The lianas of the carrying crowns: each carries some with the settings' share (pathShare within pathReach of the
* path, by pathDistance at its centre), 1 to mostPerTree of them, each a rope (ropeShare, or no neighbour) or a loop to
* a random neighbour (any crown within neighbourSpan across, in the list's order); holds at random in the lower part
* of the crowns, a rope's foot 1-6 m off across the ground (groundHeight) and sunk lianaGroundSink into it; thickness by
* lianaDiameter times the settings' thickness. Each crown draws from its own stream (seed and its identity), so its
* lianas are the same among any crowns holding its neighbours in the same order: a cell of land hangs its own alone.
*/
std::vector<Liana> chooseLianas(const std::vector<LianaCrown>& crowns, const ForestLianaSettings& settings, std::uint64_t seed,
                                const std::function<float(float, float)>& groundHeight, const std::function<float(float, float)>& pathDistance);
