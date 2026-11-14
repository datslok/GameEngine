#pragma once

#include "core/settings_visitor.h"
#include "games/forest/valley_shapes.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

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

// How the branches grow, in metres: each step, how far a point pulls from and how near uses it up, and a cap on rounds.
struct TreeGrowth {
    float step = 0.5f;
    float influence = 4.0f;
    float kill = 1.0f;
    int maxRounds = 300;
};

// A node of a tree's skeleton: where it is and its parent's index (-1 for the root).
struct TreeNode {
    Vec3 position;
    int parent = -1;
};

/*
* A tree's skeleton grown into a crown: a straight trunk from base up to the envelope's bottom in steps of at most the
* growth's step, then space colonisation towards the attractors until they are used up, none pulls any more, or the
* rounds run out. A tip never grows a node onto another (closer than a tenth of a step), which would repeat forever
* when two pulls balance. Parents always come before their children.
*/
std::vector<TreeNode> growTree(const Vec3& base, const CrownEnvelope& crown, const std::vector<Vec3>& attractors, const TreeGrowth& growth);

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

/*
* Leaf cards at the tips, appended to mesh: leavesPerTip cards round each tip, each size metres a side showing one leaf
* of a 3 x 3 atlas (LeafSet024), its stem near the tip and the leaf reaching out and down from it. Each card's front
* faces out of the crown (its normal a random turn from crownOutward, never past square to it), and its vertex normals
* lean further towards crownOutward (crownNormalBend), so the canopy shades as one rounded mass, lit on the sun's side,
* rather than as thousands of flat cards facing every way. Tangent across the leaf (+u), bitangent stem to tip (+v).
*/
inline constexpr int treeLeafColumns = 3;
inline constexpr int treeLeafRows = 3;
inline constexpr float crownNormalBend = 0.6f;
void appendTreeLeaves(IndexedMesh& mesh, const std::vector<TreeNode>& nodes, const CrownEnvelope& crown, int leavesPerTip, float size, std::uint32_t seed);

// The trees (F5 Forest, Trees; read at start-up: they are grown once).
struct ForestTreeSettings {
    // How many emergents stand between the Ring and the clearing, and how many different shapes they are grown in.
    int count = 12;
    int variants = 4;

    // The crown's centre above the ground, its radius across and half its depth (metres).
    float crownHeight = 32.0f;
    float crownRadius = 14.0f;
    float crownHalfHeight = 5.0f;

    // How many attractors fill a crown: more grow more, finer branches.
    int attractors = 2500;

    // The tips' radius (metres), from which Leonardo's rule makes the trunk.
    float twigRadius = 0.02f;

    // How far the buttresses stand out at the ground, as a share of the trunk's radius (0: none).
    float buttress = 1.5f;

    // A leaf card's size and how many sit round each tip.
    float leafSize = 0.7f;
    int leavesPerTip = 3;
};

void visit(ForestTreeSettings& settings, SettingsVisitor& visitor);

// The fixed parts of a tree's shape.
inline constexpr float leonardoExponent = 2.0f;
inline constexpr float treeSinkMetres = 1.0f;     // the trunk starts this far under the ground
inline constexpr int buttressCount = 5;
inline constexpr float buttressHeightMetres = 1.5f; // the buttresses fall away by e every this many metres up
inline constexpr float buttressSharpness = 24.0f;   // cos^p: higher is thinner fins
inline constexpr int lobedTrunkSides = 64;

// A grown tree in its own frame (its foot at the origin on the ground, the trunk up y): bark and leaves.
struct TreeMeshes {
    IndexedMesh bark;
    IndexedMesh leaves;
    int tipCount = 0;
    float trunkRadius = 0.0f;
};

// The crown a tree of these settings grows into, in its own frame.
CrownEnvelope treeCrown(const ForestTreeSettings& settings);

/*
* One variant grown and built: its skeleton, radii, the bark tubes (the trunk's lobed into buttresses, every tube whole
* bark tiles of barkTileMetres round) and its leaves. The same seed grows the same tree.
*/
TreeMeshes growTreeMeshes(const ForestTreeSettings& settings, std::uint32_t seed, float barkTileMetres);

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

// Whether a sphere meets any of the lines.
bool blocksSight(const Vec3& centre, float radius, const std::vector<SightLine>& lines);

/*
* Where the emergents stand: by dart throwing (as the pillars), on the valley's floor between the Ring and the
* clearing, within treeSpread of the valley's axis (so they show through the Ring from the path), their trunks
* treePathGap from the path's centre and their crowns clear of each other's middles and of every pillar, sized
* 85-115% and turned at random; never where the crown would block a sight line. Fewer than asked if the tries run out.
* The same every run.
*/
inline constexpr float treeNorthmost = -210.0f;
inline constexpr float treeSouthmost = -40.0f;
inline constexpr float treeSpread = 130.0f;
inline constexpr float treePathGap = 6.0f;
std::vector<TreePlacement> placeTrees(const ForestTreeSettings& settings, const std::vector<PillarPlacement>& pillars,
                                      const std::vector<SightLine>& sightLines);

// The crown's bounding sphere of a placed tree in the world (what the sight lines are tested against).
Vec3 placedCrownCentre(const TreePlacement& tree, const ForestTreeSettings& settings);
float placedCrownRadius(const TreePlacement& tree, const ForestTreeSettings& settings);
