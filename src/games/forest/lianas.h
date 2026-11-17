#pragma once

#include "core/job_system.h"
#include "core/settings_visitor.h"
#include "games/forest/island_hangings.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

/*
* Lianas, the woody vines hanging in the jungle patch (docs/research/VINES.md, stage 1): loops sagging from one crown into a
* neighbour's, and ropes hanging from a crown to the ground, as old lianas are left when their hosts grow and fall. Each
* hangs as a catenary between its two holds, its thickness drawn from Barro Colorado's census, swept into a bark tube,
* with ivy only where it runs through a crown. Built once at start-up in world space, a chunk of ground a mesh.
*/

// The most metres between a liana's points: each ring of its tube.
inline constexpr float lianaStepMetres = 0.5f;

// How far a rope's foot is sunk below the ground, so its tube ends inside it.
inline constexpr float lianaGroundSink = 0.2f;

// The sun's cascades lianas cast into (ModelRenderer::shadowCascades): thin lines the coarser texels cannot show.
inline constexpr int lianaShadowCascades = 2;

// Lianas thicker than this across get tubes of more sides.
inline constexpr float lianaThickDiameter = 0.05f;

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

// A tree's crown lianas hang from: its bounding sphere in the world.
struct LianaCrown {
    Vec3 centre;
    float radius = 0.0f;
};

/*
* One liana: from a hold in the lower part of crown, to a hold in otherCrown's (a loop) or to the ground (a rope,
* otherCrown -1), length metres along, radius metres round.
*/
struct Liana {
    Vec3 from;
    Vec3 to;
    float length = 0.0f;
    float radius = 0.0f;
    int crown = 0;
    int otherCrown = -1;
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
* The lianas of a patch's crowns: each crown carries some with the settings' share (pathShare within pathReach of the
* path, by pathDistance at its centre), 1 to mostPerTree of them, each a rope (ropeShare, or no neighbour) or a loop to
* a random neighbour; holds at random in the lower part of the crowns, a rope's foot 1-6 m off across the ground
* (groundHeight) and sunk lianaGroundSink into it; thickness by lianaDiameter times the settings' thickness. The same
* seed gives the same lianas.
*/
std::vector<Liana> chooseLianas(const std::vector<LianaCrown>& crowns, const ForestLianaSettings& settings, std::uint64_t seed,
                                const std::function<float(float, float)>& groundHeight, const std::function<float(float, float)>& pathDistance);

// One chunk's lianas: their bark tubes and their ivy, in world space.
struct LianaMeshes {
    IndexedMesh bark;
    IndexedMesh leaves;
};

/*
* The lianas' meshes, grouped by the chunkMetres square their first hold stands in, in a fixed order, a job a chunk:
* each a catenary swept into a tube of 4 sides (6 from lianaThickDiameter across), its bark in tiles of barkTileMetres
* along it, ivy cards (appendLeafCards) leafSpacing apart on the runs of its points inside a crown it hangs from and
* sparseLeafSpacing apart along the rest. With anchors (each liana's two: its hold's and its far end's, scene/wind.h),
* each hangs between them in the wind (hangBetween), its ivy with it.
*/
std::vector<LianaMeshes> lianaMeshes(const std::vector<Liana>& lianas, const std::vector<LianaCrown>& crowns, const ForestLianaSettings& settings,
                                     float barkTileMetres, float chunkMetres, JobSystem& jobs,
                                     const std::vector<std::array<std::uint16_t, 2>>& anchors = {});
