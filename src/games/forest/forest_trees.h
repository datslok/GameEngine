#pragma once

#include "core/settings_visitor.h"
#include "games/forest/island_hangings.h"
#include "games/forest/valley_shapes.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <array>
#include <cstdint>
#include <vector>

/*
* The forest's trees (ROADMAP phase 18, the forest's first stage), grown by the engine rather than scanned (Poly Haven's
* scanned trees are millions of triangles each): jungle emergents, a straight trunk with buttress roots under an
* umbrella crown, built once at start-up in a few variants and placed many times turned and scaled.
*
* How a tree grows (space colonisation, Runions et al. 2007): points scattered through the crown's envelope stand for
* the light and room a branch could reach. Each round, every point pulls the branch tip nearest it (within the
* influence distance); every pulled tip grows one step towards the mean of its pulls; points a tip has reached (within
* the kill distance) are used up. The branches compete for room as real ones do, so they fill the crown without
* crossing. The skeleton is a flat list of nodes, each naming its parent, parents always before their children, so
* anything flowing from the tips to the root (the radii) is one pass backwards over the list.
*/

// The crown's envelope: an oblate ellipsoid round centre, radius across and halfHeight up and down.
struct CrownEnvelope {
    Vec3 centre;
    float radius = 14.0f;
    float halfHeight = 5.0f;
};

// Whether a point lies inside the envelope (or on it).
bool insideCrown(const CrownEnvelope& crown, const Vec3& point);

// The way out of the crown at a point: the envelope's gradient there (its surface's normal, for points on it), made
// unit; straight up at the centre.
Vec3 crownOutward(const CrownEnvelope& crown, const Vec3& point);

// count points spread evenly through the envelope's volume (random, by rejection from its box), the same for a seed.
std::vector<Vec3> crownAttractors(const CrownEnvelope& crown, int count, std::uint32_t seed);

/*
* How the branches grow, in metres: each step, how far a point pulls from and how near uses it up, and a cap on rounds.
* gravity is tropism (Palubicki et al. 2009): each step's direction is the pulls' unit direction plus gravity times down,
* made unit again, so branches droop and crook against their pull towards the light.
*/
struct TreeGrowth {
    float step = 0.5f;
    float influence = 4.0f;
    float kill = 1.0f;
    int maxRounds = 300;
    float gravity = 0.0f;
};

// A node of a tree's skeleton: where it is and its parent's index (-1 for the root).
struct TreeNode {
    Vec3 position;
    int parent = -1;
};

/*
* Space colonisation from a skeleton already started (parents before children): rounds of growth towards the
* attractors until they are used up, none pulls any more, or the rounds run out. A tip never grows a node onto another
* (closer than a tenth of a step), which would repeat forever when two pulls balance. bounds is where the attractors lie
* (the nearest-node grid covers it). Parents always come before their children.
*/
std::vector<TreeNode> coloniseTree(std::vector<TreeNode> nodes, const CrownEnvelope& bounds, const std::vector<Vec3>& attractors,
                                   const TreeGrowth& growth);

/*
* A trunk appended to nodes from base, rising rise metres, in equal shares of the way up with every step at most step:
* it bends by lean (a level offset at its top) as base + up rise s + lean s^2 at a share s of the way up, straight at
* the foot, as a trunk grown towards light leans more the higher it gets. Returns the top's index.
*/
int appendTrunk(std::vector<TreeNode>& nodes, int parent, const Vec3& base, float rise, const Vec3& lean, float step);

/*
* A tree's skeleton grown into one crown: a trunk from base up to the envelope's bottom (appendTrunk), then space
* colonisation (coloniseTree) towards the attractors.
*/
std::vector<TreeNode> growTree(const Vec3& base, const CrownEnvelope& crown, const std::vector<Vec3>& attractors, const TreeGrowth& growth,
                               const Vec3& lean = Vec3{0.0f, 0.0f, 0.0f});

/*
* A clump of foliage: a sphere the crown's attractors fill, and the scaffold limb that grows into it. A crown of
* separate clumps (rather than one envelope filled evenly) leaves gaps and depth between them, as real crowns have.
*/
struct TreeClump {
    Vec3 centre;
    float radius = 0.0f;
    int limb = 0;
};

// How far apart clumps' centres stay, as a share of their radii's sum: a little overlap, gaps between most.
inline constexpr float clumpSpacing = 0.9f;

/*
* count clumps in the crown by dart throwing, each 75-125% of radius, their centres in the outer part of the envelope
* shrunk by their own radius (55-100% of the way out from its centre), clumpSpacing apart; fewer if the tries run out.
* Then shared among the limbs by their direction round the trunk, in turn, so every limb has one when there are as many
* clumps as limbs (each clump a limb of its own when there are fewer).
*/
std::vector<TreeClump> treeClumps(const CrownEnvelope& crown, int count, float radius, int limbs, std::uint32_t seed);

// count attractors spread evenly through the clumps (each clump's share by its volume, the rest to the first ones).
std::vector<Vec3> clumpAttractors(const std::vector<TreeClump>& clumps, int count, std::uint32_t seed);

/*
* The scaffold: a trunk from base to fork (appendTrunk, leaning to the fork's level offset), then one limb a target, each
* leaving the fork's node and walking towards its target in steps of step, kinked sideways by smooth noise of its own,
* until it is within stopMetres of it (space colonisation grows on from there).
*/
inline constexpr float limbWander = 0.45f;          // the sideways kink, as a share of the step's pull
inline constexpr float limbWanderMetres = 3.0f;     // over how many metres the kink changes
std::vector<TreeNode> growScaffold(const Vec3& base, const Vec3& fork, const std::vector<Vec3>& targets, float stopMetres, float step,
                                   std::uint32_t seed);

/*
* Each node's radius by Leonardo's rule (the pipe model): a parent's cross-section carries its children's,
* r_parent^n = sum r_child^n, from tipRadius at every tip. With n = 2 the area is conserved, as da Vinci drew it; real
* trees measure between 1.8 and 2.5. One pass from the last node to the first.
*/
std::vector<float> leonardoRadii(const std::vector<TreeNode>& nodes, float tipRadius, float exponent);

// Whether a node is a tip (no node names it as parent), for every node.
std::vector<bool> treeTips(const std::vector<TreeNode>& nodes);

/*
* The skeleton cut into chains, each drawn as one tube: from the root, each chain follows its thickest child onward,
* and every other child starts a chain of its own, which begins at the parent's node (so its tube starts inside the
* parent's) and runs on to a tip. The first chain is the trunk's. One chain a tip.
*/
std::vector<std::vector<int>> branchChains(const std::vector<TreeNode>& nodes, const std::vector<float>& radii);

// How many sides a tube of a radius gets: about one every 15 cm round it, 3 for a twig, 16 at most.
int tubeSides(float radius);

// The twigs' nodes, which carry foliage: every one but the root at most leafyRadius thick.
std::vector<bool> leafyNodes(const std::vector<float>& radii, float leafyRadius);

/*
* One species of tree (F5 Forest, Canopy trees and Emergent trees; read at start-up: they are grown once). The defaults
* are the canopy trees'; emergentTreeSettings gives the emergents'. Sizes from docs/JUNGLE.md: the canopy 20-40 m, the
* emergents 45-60 m above it.
*/
struct ForestTreeSettings {
    // How many trees stand between the Ring and the clearing, and how many different shapes they are grown in. Four
    // times the first patch's crown cover (the user, Nov 14 2026: "the tall coverage isnt there").
    int count = 80;
    int variants = 4;

    // How close two trunks of the species may stand, as a share of the larger crown's radius: below 1 their crowns
    // interlock, as a closed canopy's do.
    float spacingShare = 0.55f;

    // The tree's top and where its trunk forks into limbs, above the ground, and the crown's radius across (metres):
    // a canopy tree at the canopy's top, its crown from about half its height (docs/JUNGLE.md, The mid layer's shape:
    // forking at 12 m left no room under it for the mid layer). The crown fills the space between the fork and the top.
    float height = 38.0f;
    float forkHeight = 18.0f;
    float crownRadius = 16.0f;

    // How many scaffold limbs leave the fork (each variant one fewer to one more), and how many clumps of foliage of
    // about what radius they grow into.
    int limbs = 4;
    int clumps = 14;
    float clumpRadius = 5.5f;

    // How many attractors fill the clumps: more grow more, finer branches.
    int attractors = 5000;

    // The tips' radius (metres), from which Leonardo's rule makes the limbs and trunk.
    float twigRadius = 0.035f;

    // How far the buttresses stand out at the ground, as a share of the trunk's radius (0: none).
    float buttress = 1.5f;

    // How far the most leaning variant's fork stands off its foot (metres); each leans 30-100% of it its own way.
    float lean = 2.5f;

    // Foliage on leaf-cluster cards (docs/TREES.md, Part 2): a card's size (metres a side), a leaf's length on it, how
    // thick a branch may be and still carry foliage, how many cards each such node in a clump's shell carries, how far
    // out of its clump a node must be to have one (a share of the clump's radius), how many bigger cards fill each
    // clump's inside so it cannot be seen through, and the clusters' texture's texels a cell (four cells). Real-sized
    // leaves (15-25 cm: oversized ones made the trees read small, the user, Nov 14 2026) on cards half as big as before,
    // so each twig keeps its leaves, and more cards for the cover.
    float clusterSize = 1.0f;
    float leafLength = 0.22f;
    float leafyRadius = 0.06f;
    int cardsPerNode = 6;
    float shellShare = 0.3f;
    int fillCards = 32;
    int clusterTexels = 1024;

    // The shadow leaves (drawn into shadows only, the leaves seen casting none): shadowCardsPerNode cards at each shell
    // node and shadowFillCards in each clump, each shadowCardScale times as big: about a sixth of the cards for about
    // two thirds of the cover. Every leaf card was redrawn into the sun's cascades each frame the camera turned (23 ms of
    // shadows on the RTX 4070 Laptop, Nov 14 2026).
    int shadowCardsPerNode = 1;
    int shadowFillCards = 8;
    float shadowCardScale = 2.0f;

    // The shadow bark: the bark simplified to this share of its triangles (scene/simplify_mesh), drawn into shadows only
    // while the bark seen casts none. Bark was nine tenths of the triangles the sun's cascades redrew as the camera
    // turned (1,300 trees: 19 ms of shadows on the RTX 4070 Laptop, Nov 15 2026).
    float shadowBarkShare = 0.1f;

    // The levels of detail between the seen meshes and the shadows' stand-ins, which are the coarsest (docs/LOD.md):
    // levelCardShare of the leaf cards, each grown to keep their area (SpeedTree's fewer, bigger cards), and the bark
    // simplified to levelBarkShare of its triangles.
    float levelCardShare = 0.5f;
    float levelBarkShare = 0.3f;

    // The impostor past them (scene/impostor.h, docs/LOD.md): the error, in metres, from which the tree is drawn as its
    // card (at least the shadow stand-ins'; 1.5 m switches about 580 m off at 1080 pixels and a 70 degree view, about
    // 90 m in the reflection probe's faces), and its atlas: frames a side over the hemisphere, texels a frame (8 x 128:
    // 1024 square, Fortnite's trees 12 x 12 in 2048).
    float impostorErrorMetres = 1.5f;
    int impostorFrames = 8;
    int impostorTexels = 128;
};

void visit(ForestTreeSettings& settings, SettingsVisitor& visitor);

/*
* The emergents' settings (docs/JUNGLE.md): ten trees 55 m tall, standing apart (their whole crown's radius), standing over the canopy, a clear trunk to 36 m and a
* wide crown spread from its top, about half their height across, as real emergents' are; bigger buttresses, little
* lean. Their leaves are the canopy trees', so both share one clusters' texture.
*/
ForestTreeSettings emergentTreeSettings();

/*
* The mid layer's settings (docs/JUNGLE.md): 1200 slender trees 15 m tall, their crowns interlocking, in the canopy's shade, a trunk 20-40 cm across,
* a shallow crown from 10 m about 5 m in radius with few clumps (shade trees have flatter crowns and fewer leaf layers),
* placed under and between the canopy crowns. Their leaves are the canopy trees'.
*/
ForestTreeSettings midTreeSettings();

// The clump a point belongs to: the one it is deepest in (the least distance over radius); -1 when there are none.
int clumpOf(const Vec3& point, const std::vector<TreeClump>& clumps);

// A direction with its downward part halved, made unit again: the clump's underside lit as a cone, not a floor
// (docs/TREES.md, Part 3).
Vec3 coneFlattened(const Vec3& direction);

/*
* The foliage's normals: a card's own facing, its clump's way out and the crown's, blended (30%, 50%, 20%) and the
* last two cone-flattened. Each clump gets its own lit top and shaded underside, which gives the crown its depth; the
* card's own share keeps the clusters catching light one by one.
*/
inline constexpr float cardNormalShare = 0.3f;
inline constexpr float clumpNormalShare = 0.5f;
inline constexpr float crownNormalShare = 0.2f;
Vec3 foliageNormal(const Vec3& facing, const Vec3& clumpOut, const Vec3& crownOut);

/*
* Leaf-cluster cards, appended to mesh: cardsPerNode cards at every leafy node in its clump's shell (at least
* shellShare of the clump's radius out of its centre), each clusterSize a side, its stem near the node, the twig reaching
* out of the clump and a little up, its front facing out of the clump (a random turn, never past square), its picture
* one of the clusters' four cells; then fillCards cards half as big again inside each clump, near its middle, facing
* every way, shading with the crown's way out. Normals by foliageNormal. Returns the cards made.
*/
// Each leaf-cluster cell's outline (clusterOutline), in cell order; a card is drawn as its cell's outline.
// nodeCardStride (0: cardsPerNode) numbers each node's cards as if it had that many, so the same seed with fewer cards
// makes the first of the same cards (a level of detail's, its survivors only grown).
using ClusterOutlines = std::array<std::vector<Vec2>, 4>;

int appendClusterCards(IndexedMesh& mesh, const std::vector<TreeNode>& nodes, const std::vector<float>& radii, const CrownEnvelope& crown,
                       const std::vector<TreeClump>& clumps, const ForestTreeSettings& settings, std::uint32_t seed,
                       const ClusterOutlines* outlines = nullptr, int nodeCardStride = 0);

// The fixed parts of a tree's shape. Leonardo's n of 2.5 keeps the limbs thick against the twigs (measured trees: 2-3).
inline constexpr float leonardoExponent = 2.5f;
inline constexpr float treeSinkMetres = 1.0f;     // the trunk starts this far under the ground
inline constexpr int buttressCount = 5;
inline constexpr float buttressHeightMetres = 1.5f; // the buttresses fall away by e every this many metres up
inline constexpr float buttressSharpness = 24.0f;   // cos^p: higher is thinner fins
inline constexpr int lobedTrunkSides = 64;
inline constexpr float trunkTaper = 0.5f;           // the trunk at its foot is this much thicker than at the fork
inline constexpr float leastLeanShare = 0.3f;       // each variant leans 30-100% of the setting
inline constexpr float treeGravity = 0.2f;          // the trees' tropism (TreeGrowth::gravity)

/*
* A grown tree's skeleton, kept for what grows on it (games/forest/epiphytes.h): its nodes, each one's radius as its
* bark was built (the trunk's tapered), its chains (branchChains) and Leonardo's radius at the fork.
*/
struct TreeSkeleton {
    std::vector<TreeNode> nodes;
    std::vector<float> radii;
    std::vector<std::vector<int>> chains;
    float trunkRadius = 0.0f;
};

// A grown tree in its own frame (its foot at the origin on the ground, the trunk up y): bark and leaves.
struct TreeMeshes {
    IndexedMesh bark;
    IndexedMesh leaves;
    IndexedMesh shadowLeaves; // the shadow passes' stand-in for the leaves (ForestTreeSettings::shadowCardsPerNode)
    IndexedMesh shadowBark;   // and for the bark (ForestTreeSettings::shadowBarkShare)
    IndexedMesh leafLevel;    // the leaves' first coarser level (ForestTreeSettings::levelCardShare); the shadow leaves the second
    IndexedMesh barkLevel;    // the bark's (ForestTreeSettings::levelBarkShare); the shadow bark the second
    int tipCount = 0;
    int leafyCount = 0;
    int cardCount = 0;
    int shadowCardCount = 0;
    int leafLevelCardCount = 0;

    // Each level's error (metres, scene/level_of_detail.h): for the cards how much longer their sides grew, for the bark
    // how far the simplifier moved its surface.
    float leafLevelError = 0.0f;
    float shadowLeafError = 0.0f;
    float barkLevelError = 0.0f;
    float shadowBarkError = 0.0f;
    float trunkRadius = 0.0f; // Leonardo's, at the fork; the foot is trunkTaper thicker

    // The trunk's strand and lobes as its tube was built, up to its fork, for climbers (games/forest/climbers.h).
    ClimbHost trunk;

    // The whole skeleton, for epiphytes (games/forest/epiphytes.h).
    TreeSkeleton skeleton;
};

// The crown a tree of these settings grows into, in its own frame: from the fork to the top, crownRadius across.
CrownEnvelope treeCrown(const ForestTreeSettings& settings);

// One variant's own shape from its seed: its crown (the settings'), its fork's lean (30-100% of the setting, any way
// round) and its limbs (the setting's, one fewer or one more, two at least).
struct TreeVariantShape {
    CrownEnvelope crown;
    Vec3 lean;
    int limbs = 0;
};
TreeVariantShape treeVariantShape(const ForestTreeSettings& settings, std::uint32_t seed);

/*
* One variant grown and built: its clumps, its scaffold (the trunk to the fork, a limb towards each limb's clumps),
* space colonisation from it into the clumps' attractors with the trees' gravity, the radii, the bark tubes (the trunk's
* lobed into buttresses and tapering from the foot to the fork, every tube whole bark tiles of barkTileMetres round),
* its leaves and its shadow leaves. The same seed grows the same tree.
*/
TreeMeshes growTreeMeshes(const ForestTreeSettings& settings, std::uint32_t seed, float barkTileMetres, const ClusterOutlines* outlines = nullptr);

// Where a tree stands: its foot (on the lowest ground round its trunk), its size against the variant's, its turn about
// up, and which variant it is.
struct TreePlacement {
    Vec3 foot;
    float scale = 1.0f;
    float turnRadians = 0.0f;
    int variant = 0;
};

// A line of sight from somewhere walked to a landmark.
struct SightLine {
    Vec3 from;
    Vec3 to;
};

/*
* The lines of sight the forest must leave open (the user's requirement): from eye height on the path every
* sightLineSpacing metres, round the meadow and round the clearing, to a point high on the Monolith's face and to the
* Ring's top. A tree whose crown meets one is not planted, so some of both stays in view all along the walk.
*/
inline constexpr float sightLineSpacing = 10.0f;
inline constexpr float sightEyeHeight = 1.7f;
std::vector<SightLine> forestSightLines();

/*
* Where to start among the trees (ForestStart::Trees): treesStartDistance metres south of the tree with the most others
* within treesStartNeighbourhood (the first on a tie), so the player stands under the grove facing it (north); the
* path's point 100 m past the Ring when there are no trees.
*/
inline constexpr float treesStartDistance = 12.0f;
inline constexpr float treesStartNeighbourhood = 60.0f;
Vec2 forestTreesStart(const std::vector<TreePlacement>& trees);

// Whether a sphere meets any of the lines.
bool blocksSight(const Vec3& centre, float radius, const std::vector<SightLine>& lines);

/*
* A tree already standing, which later species keep clear of: its trunk's place on the ground and its crown's radius.
*/
struct StandingCrown {
    Vec2 foot;
    float radius = 0.0f;
};

/*
* Where a species' trees stand: by dart throwing (as the pillars), from its own seed, on the valley's floor between the
* Ring and the clearing, within treeSpread of the valley's axis (so they show through the Ring from the path), their
* trunks treePathGap from the path's centre and outside the crowns of each other and of the trees already standing,
* their crowns clear of every pillar, sized 85-115% and turned at random; never where the crown would block a sight
* line. Fewer than asked if the tries run out. The same every run.
*/
inline constexpr float treeNorthmost = -210.0f;
inline constexpr float treeSouthmost = -40.0f;
inline constexpr float treeSpread = 130.0f;
inline constexpr float treePathGap = 6.0f;
inline constexpr std::uint64_t emergentPlaceSeed = 0x454d4552ull;
inline constexpr std::uint64_t canopyPlaceSeed = 0x54524545ull;
inline constexpr std::uint64_t midPlaceSeed = 0x4d494444ull;
std::vector<TreePlacement> placeTrees(const ForestTreeSettings& settings, const std::vector<PillarPlacement>& pillars,
                                      const std::vector<SightLine>& sightLines, const std::vector<StandingCrown>& standing,
                                      std::uint64_t seed);

// The placed trees as crowns for the next species to keep clear of.
std::vector<StandingCrown> standingCrowns(const std::vector<TreePlacement>& trees, const ForestTreeSettings& settings);

// The placed trees' trunks for the mid layer to keep clear of, each clearance metres round (its crown may stand under
// theirs): past the buttresses, with room to walk between.
inline constexpr float midTrunkClearance = 4.0f;
std::vector<StandingCrown> standingTrunks(const std::vector<TreePlacement>& trees, float clearance);

// The crown's bounding sphere of a placed tree in the world (what the sight lines are tested against).
Vec3 placedCrownCentre(const TreePlacement& tree, const ForestTreeSettings& settings);
float placedCrownRadius(const TreePlacement& tree, const ForestTreeSettings& settings);
