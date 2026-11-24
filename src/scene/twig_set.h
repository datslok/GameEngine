#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/asset_handles.h"
#include "scene/indexed_mesh.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/*
* A plant's twigs drawn as instances of a few prototype meshes (ROADMAP phase 21 stage 3b-iii, docs/research/LEAFGEOMETRY.md;
* Nanite's assemblies): what a game hands the renderer. The prototypes are drawn-only meshes with cluster levels of their
* own (AssetManager::setGivenClusterLevels); each twig places one in the plant's frame. The twigs are kept in bricks of
* twigBrickSize, near ones together, each brick a sphere holding its twigs, so the GPU culls a brick before its twigs:
* its cost grows with the bricks in view, not the twigs on the plant.
*/

inline constexpr std::uint32_t twigBrickSize = 64;

/*
* One twig: its foot in the plant's frame and its frame there (across, towardsTip, facing: where the prototype's x, y
* and z go; right-handed, unit), its scale, its prototype, and the lean its leaves' normals bend by (the clump's and
* crown's share of the foliage blend, games/forest/forest_trees.h); and the bone it rides on in its part's skeleton
* (RenderPart::skeleton), a bone below 0 for none.
*/
struct TwigInstance {
    Vec3 foot;
    Vec3 across;
    Vec3 towardsTip;
    Vec3 facing;
    float scale = 1.0f;
    std::uint32_t prototype = 0;
    Vec3 lean;
    SwayBinding binding{.bone = -1, .weight = 1.0f};
};

// A run of twigs and the sphere holding every one of them (each twig its foot and its prototype's reach times its scale).
struct TwigBrick {
    Vec3 centre;
    float radius = 0.0f;
    std::uint32_t firstTwig = 0;
    std::uint32_t twigCount = 0;
};

/*
* Far levels above the twigs (ROADMAP phase 21 stage 3e, docs/research/LEAFGEOMETRY.md, Far levels above the twigs;
* Nanite's assemblies, a part merged once it is one cluster). From twigBrickLevel on a twig carries a few one-row leaves
* in a workgroup with room for far more, so a brick takes over its twigs there: its leaves packed as records in full
* clusters, coarser levels keeping half of them each, survivors grown, bricks merged in pairs once one cluster is left.
*/

// The twig level whose leaves a brick's finest level holds, exactly (scene/leaf_pruning.h's level; the coarsest a
// prototype has when it has fewer).
inline constexpr int twigBrickLevel = 2;

/*
* One leaf of a prototype's twigBrickLevel, in its frame (games/forest/twigs.h): its foot, the way to its tip and its
* facing (unit, square to each other), its cell's side in metres as drawn at that level, its shape's area (its cell's
* units squared, the rows it is drawn with there), the farthest any point of it lies from its foot (a share of its
* side), its atlas cell and its number in the twig (its tone's).
*/
struct TwigLeafPlace {
    Vec3 foot;
    Vec3 towardsTip;
    Vec3 facing;
    float side = 1.0f;
    float area = 0.0f;
    float reach = 0.0f;
    std::uint32_t cell = 0;
    std::uint32_t number = 0;
};

// A prototype's leaves at twigBrickLevel, how much that level grew them over the finest (their sides there), and its
// error (metres, prototype's frame).
struct TwigFarLeaves {
    std::vector<TwigLeafPlace> leaves;
    float growth = 1.0f;
    float error = 0.0f;
};

/*
* A far leaf's record, in the plant's frame: a TwigLeafPlace carried by its twig (foot, frame, side times the twig's
* scale and the level's growth), its cell and number, and its twig's number in the set (whose binding, lean and tone
* it shares).
*/
struct FarLeaf {
    Vec3 foot;
    Vec3 towardsTip;
    Vec3 facing;
    float side = 0.0f;
    std::uint32_t cell = 0;
    std::uint32_t number = 0;
    std::uint32_t twig = 0;
};

/*
* One level of a far node: its leaves' run in the records (each level its own run, so its clusters are runs of
* farLeavesPerCluster), its density (it keeps one leaf in 2^density of the set's, by the set's rank), the growth of
* its leaves over the twig level's, and its error (metres, plant's frame): how far a grown leaf strays past its finest
* size, never less than the level before's, nor its children's, nor its twigs'.
*/
struct TwigFarLevel {
    std::uint32_t firstLeaf = 0;
    std::uint32_t leafCount = 0;
    std::uint32_t density = 0;
    float growth = 1.0f;
    float error = 0.0f;
};

inline constexpr std::uint32_t twigFarNoNode = 0xffffffffu;

/*
* A node of the far tree: a brick (nodes 0 to bricks - 1, a brick each) or two nodes merged (its children). Its sphere
* holds every leaf of each of its levels and its children's spheres; its bricks' run, and its levels' run.
*/
struct TwigFarNode {
    Vec3 centre;
    float radius = 0.0f;
    std::uint32_t firstBrick = 0;
    std::uint32_t brickCount = 0;
    std::uint32_t firstLevel = 0;
    std::uint32_t levelCount = 0;
    std::uint32_t children[2] = {twigFarNoNode, twigFarNoNode};
    std::uint32_t parent = twigFarNoNode;
};

/*
* An atlas cell's leaf as far leaves draw it, one row (games/forest/leaf_clusters.h's LeafShape traced with one row, in
* its cell's units): its base and tip on the midrib, its row's height and half widths left and right of it, and the
* cell's texture rectangle (its leaf's number not yet added, scene/foliage_shading.h).
*/
struct FarLeafShape {
    float base = 0.0f;
    float tip = 1.0f;
    float up = 0.5f;
    float left = 0.0f;
    float right = 0.0f;
    Vec2 uvMin;
    Vec2 uvMax;
};

// Every atlas cell's shape (by cell), and how far every leaf is folded about its midrib and curled (ModelledLeafPlace's).
struct FarLeafShapes {
    std::vector<FarLeafShape> cells;
    float foldRadians = 0.0f;
    float curl = 0.0f;
};

// The far levels of a set: the bricks' nodes first, the merged ones after their children, the root last; the shapes
// their records' cells name.
struct TwigFarLevels {
    std::vector<FarLeaf> leaves;
    std::vector<TwigFarLevel> levels;
    std::vector<TwigFarNode> nodes;
    FarLeafShapes shapes;
};

struct TwigSet {
    std::vector<MeshHandle> prototypes;
    std::vector<TwigInstance> twigs; // brick by brick
    std::vector<TwigBrick> bricks;
    TwigFarLevels far; // empty until made (scene/twig_far_levels.h)
};

/*
* How many of a run of bricks the median split puts in its first half (makeTwigSet): half, rounded up. The far tree
* merges them by the same split, so neighbours pair.
*/
std::size_t twigBricksFirstHalf(std::size_t bricks);

// How far a mesh reaches from its origin: its farthest vertex's distance (0 for none).
float meshReach(const IndexedMesh& mesh);

// A twig's point in the plant's frame from a point in its prototype's frame.
Vec3 twigPoint(const TwigInstance& twig, const Vec3& local);

/*
* The twigs cut into bricks of twigBrickSize by splitting their feet's box along its longest side at a whole number of
* bricks, again and again (a median split, as a bounding volume tree's leaves; a space-filling curve's runs jump across
* the crown now and then, making a few huge bricks), each brick's sphere holding its twigs' spheres (each twig's foot and
* its prototype's reach, from reaches, times its scale). Every brick but the last is full. Throws std::invalid_argument
* for a twig whose prototype has no reach given. Deterministic; cost grows with the twigs times their log.
*/
TwigSet makeTwigSet(std::vector<MeshHandle> prototypes, const std::vector<float>& reaches, std::vector<TwigInstance> twigs);
