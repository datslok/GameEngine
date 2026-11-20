#pragma once

#include "assets/image_loader.h"
#include "core/coverage_mips.h"
#include "core/pixel.h"
#include "core/settings_visitor.h"
#include "games/forest/island_hangings.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <vector>

/*
* Climbers on the big trees' trunks (docs/research/VINES.md, stage 2): root climbers such as philodendrons and monsteras, which
* take a trunk of any size. Each climbs from the foot, between the buttresses, up the bark, winding a little; low down
* its leaves are small and pressed flat to the bark like tiles ("shingling"), higher up big heart-shaped leaves on
* stalks stand off the trunk and droop; feeder roots hang straight down from it to the ground, some ending in mid air.
* Grown into each tree variant in its own frame, so they repeat with it, as the tree does.
*/

// The sun's cascades climbers cast into (ModelRenderer::shadowCascades; 0 all): all, so their shadows on the trunks do
// not pop in as one walks up (ForestUnderstoreySettings::shadowCascades).
inline constexpr int climberShadowCascades = 0;

// How many climbers and how they grow (F5 Forest, Climbers; start-up: the meshes are built once).
struct ForestClimberSettings {
    // The share of big trees' variants carrying climbers, and the most on one. Fuller than first grown (0.7 and 2, a
    // thin strip up one side of a trunk), the user's choice.
    float treeShare = 0.8f;
    int mostPerTree = 4;

    // How high one climbs: a random share between these of the trunk's height to its fork.
    float leastHeightShare = 0.5f;
    float mostHeightShare = 1.0f;

    // How far one winds round the trunk from where the buttresses end to its top, in turns (either way).
    float twist = 0.3f;

    // Its stem across at the foot (metres), tapering to 60% at the top.
    float stemDiameter = 0.03f;

    // Below this height (metres) the leaves are shingles, small and pressed to the bark, their length and the metres
    // between them along the stem; above it, big stalked leaves.
    float shingleHeight = 4.0f;
    float shingleLeafLength = 0.2f;
    float shingleSpacing = 0.12f;
    float bigLeafLength = 0.8f;
    float bigLeafSpacing = 0.4f;

    // The metres between feeder roots up the stem, and the share ending in mid air.
    float rootSpacing = 1.5f;
    float midAirShare = 0.3f;
};

void visit(ForestClimberSettings& settings, SettingsVisitor& visitor);

// A place on a host's surface, and the way out of it (square to the trunk).
struct HostPoint {
    Vec3 position;
    Vec3 outward;
};

/*
* The host's surface at a height and an angle round it (its tube's frames, tubeFrames of its strand): its centre line
* and radius there, between its points, at its lobed radius (lobedRadius) when it has lobes, as its tube is drawn.
*/
HostPoint hostSurface(const ClimbHost& host, const std::vector<TubeFrame>& frames, float height, float angle);

// How far a point is outside the host's surface, square to the trunk at its height (negative inside).
float hostClearance(const ClimbHost& host, const std::vector<TubeFrame>& frames, const Vec3& point);

// One tree's climbers: stems, leaf stalks and roots in one mesh (bark), leaves in another, in the tree's own frame.
struct ClimberMeshes {
    IndexedMesh stems;
    IndexedMesh leaves;
    int shingleLeaves = 0;
    int bigLeaves = 0;
    int roots = 0;
    std::vector<HangingStrand> rootStrands;
    float top = 0.0f; // the highest a climber reached
    Vec3 facing;      // the way out of the trunk at the first climber, a third of its way up (level; zero without one)
};

/*
* One climber up the host from its seed: from just above the ground, starting between the buttresses (where the lobes
* stand out least), up to a random share of its height, the stem a little out from the surface; its leaves every so
* often along it, shingles below shingleHeight (flat on the bark, pointing up and a little aside, alternately), big
* stalked drooping ones above; its feeder roots from 2.5 m up, each pushed out until it hangs clear of the trunk and
* its buttresses all the way down, to the ground or, for midAirShare of them, part way.
*/
ClimberMeshes growClimber(const ClimbHost& host, const ForestClimberSettings& settings, std::uint32_t seed);

// How many climbers a tree with this seed carries: none for 1 - treeShare of them, else 1 to mostPerTree.
int climbersOnTree(const ForestClimberSettings& settings, std::uint32_t seed);

// A tree's climbers, as many as climbersOnTree says, grown together into one pair of meshes.
ClimberMeshes growClimbers(const ClimbHost& host, const ForestClimberSettings& settings, std::uint32_t seed);

/*
* A climber's leaf picture, texels square, u across the leaf and v from its base to its tip: its colour (sRGB, alpha
* 255: the leaf's outline is its geometry) is the colour given, glossy and dark, with a paler midrib down u 0.5 and
* lateral veins curving out towards the tip; its height (grey) has the midrib raised and the veins sunk, for its
* normal map. The same seed draws the same pictures.
*/
struct AroidLeafImages {
    ImageData colour;
    ImageLevel height;
};

AroidLeafImages aroidLeafImages(int texels, Pixel colour, std::uint32_t seed);
