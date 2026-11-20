#pragma once

#include "assets/image_loader.h"
#include "core/coverage_mips.h"
#include "core/pixel.h"
#include "core/settings_visitor.h"
#include "games/forest/forest_trees.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <vector>

/*
* Epiphytes on the trees (docs/research/VINES.md, stage 3): plants growing on the branches, not the ground. Big rosettes
* (bird's-nest ferns and tank bromeliads) sit in the crotches and on top of thick, nearly level limbs at the crown's
* base, where humus gathers (Johansson's zone III), and moss covers the upper sides of the limbs. A Panama plot had
* about 34 on each tree carrying any, and trunk thickness predicts them best: here 20-60 on a big tree, a few
* bromeliads on a small one. Grown into each tree variant from its kept skeleton (TreeMeshes::skeleton), so they repeat
* with it, as the tree and its climbers do.
*/

// The sun's cascades epiphytes cast into (ModelRenderer::shadowCascades; 0 all): all, so their shadows do not pop in as
// one walks up (ForestUnderstoreySettings::shadowCascades).
inline constexpr int epiphyteShadowCascades = 0;

// How many epiphytes and how they grow (F5 Forest, Epiphytes; start-up: the meshes are built once).
struct ForestEpiphyteSettings {
    // How many rosettes a tree carries: a big tree (the canopy and emergent trees) between the least and the most, a
    // small one (the mid layer) up to its most; fewer when the tree has no room for them.
    int leastPerBigTree = 20;
    int mostPerBigTree = 60;
    int mostPerSmallTree = 5;

    // A limb carries rosettes when at least this share of the trunk's radius at the fork thick (and at least the least
    // radius, metres), and no steeper than this; rosettes stand at least spacing metres apart.
    float limbShare = 0.25f;
    float leastLimbRadius = 0.03f;
    float steepestLimbDegrees = 35.0f;
    float spacing = 1.2f;

    // The share of a big tree's rosettes that are bird's-nest ferns (the rest bromeliads; a small tree's all
    // bromeliads), and each kind's length (each rosette 50-150% of it) and leaf count. A bird's-nest fern's fronds are
    // 0.5-1.5 m long, a tank bromeliad's leaves 20-60 cm.
    float fernShare = 0.5f;
    float fernLength = 1.0f;
    int fernFronds = 12;
    float bromeliadLength = 0.4f;
    int bromeliadLeaves = 16;

    // Moss over the top of the limbs: whether it grows, on limbs at least this share of the trunk's radius thick, no
    // steeper than this, covering this much of the way round them.
    bool moss = true;
    float mossLimbShare = 0.15f;
    float mossSteepestDegrees = 50.0f;
    float mossArcDegrees = 140.0f;
};

void visit(ForestEpiphyteSettings& settings, SettingsVisitor& visitor);

// Where a rosette may sit: in a crotch (a node where the tree forks) or on top of a limb.
enum class EpiphyteSiteKind {
    Crotch,
    Limb
};

// A place for a rosette: its base, the way out of the limb's top there (straight up in a crotch), the limb's radius.
struct EpiphyteSite {
    Vec3 base;
    Vec3 limbUp;
    EpiphyteSiteKind kind = EpiphyteSiteKind::Limb;
    float limbRadius = 0.0f;
};

/*
* Every place on the skeleton a rosette may sit, crotches first (in node order), then limbs (chain by chain): each node
* with two children or more and thick enough (limbShare of the trunk's radius, at least leastLimbRadius), the rosette
* sunk a quarter of its radius above it; and the middle of each segment of a chain that thick and no steeper than
* steepestLimbDegrees, on its top (its radius out, square to it, upwards).
*/
std::vector<EpiphyteSite> epiphyteSites(const TreeSkeleton& skeleton, const ForestEpiphyteSettings& settings);

/*
* Up to count of the sites, spacing apart: the crotches first in a random order, then the limbs' in a random order, each
* taken when it is far enough from every one taken before. The same for a seed.
*/
std::vector<EpiphyteSite> chooseEpiphyteSites(const std::vector<EpiphyteSite>& sites, int count, float spacing, std::uint32_t seed);

// How many rosettes a tree with this seed would carry, room allowing: least to most on a big tree, 0 to the most on a
// small one.
int epiphytesOnTree(const ForestEpiphyteSettings& settings, bool bigTree, std::uint32_t seed);

enum class EpiphyteKind {
    Fern,
    Bromeliad
};

// One rosette's shape: its kind, its leaves' length (metres) and count, its seed.
struct RosetteShape {
    EpiphyteKind kind = EpiphyteKind::Fern;
    float length = 1.0f;
    int leaves = 12;
    std::uint32_t seed = 0;
};

// A rosette of a kind from the settings: its length 50-150% of the kind's, its leaves the kind's count.
RosetteShape rosetteShape(const ForestEpiphyteSettings& settings, EpiphyteKind kind, std::uint32_t seed);

/*
* A rosette appended to mesh round base: its strap leaves (appendStrapLeaf, the palms' leaflets) round up by the golden
* angle, the youngest rising steepest from the middle, the oldest round the outside lower and drooping more, each
* 85-100% of the shape's length. A bird's-nest fern's fronds open as a funnel and arch over, nearly flat; a bromeliad's
* leaves are channelled (folded more) and stiffer. Each reads its kind's half of the leaves' atlas (epiphyteLeafImages).
*/
void appendRosette(IndexedMesh& mesh, const Vec3& base, const RosetteShape& shape);

/*
* Moss over the limbs (none when settings.moss is off): over each run of a chain's segments thick and level enough, a
* shell mossArcDegrees round the top of the limb, a centimetre out from its bark, narrowing to nothing at the run's
* ends; u round it and v along it in tiles of tileMetres, normals out.
*/
IndexedMesh limbMoss(const TreeSkeleton& skeleton, const ForestEpiphyteSettings& settings, float tileMetres);

/*
* The rosettes' leaves' atlas, texels tall and twice as wide, rows from the bottom (v 0 at a leaf's base) as loaded
* images are; each kind reads its own half (appendRosette): the fern's frond on the left, the bromeliad's leaf on the
* right. Its colour is sRGB, alpha 255 (a leaf's outline is its geometry); its height (grey) feeds its normal map.
*
* A bird's-nest fern's frond is a bright, glossy apple green with a dark, nearly black midrib and fine veins running
* slanted from it to the edge, close together; its height has the midrib raised and the veins sunk. The bromeliad's half
* is the palms' leaflet picture (palmLeafletImages) in a duller, redder green. The same seed draws the same pictures.
*/
struct EpiphyteLeafImages {
    ImageData colour;
    ImageLevel height;
};

// Each kind's colour from the leaves' own (sRGB): the fern's brighter and yellower, the bromeliad's duller and redder.
Pixel fernColour(Pixel leaf);
Pixel bromeliadColour(Pixel leaf);

EpiphyteLeafImages epiphyteLeafImages(int texels, Pixel leaf, std::uint32_t seed);

// One tree's epiphytes, in its own frame: the rosettes' leaves in one mesh, the moss in another, and where they sit.
struct EpiphyteMeshes {
    IndexedMesh leaves;
    IndexedMesh moss;
    int ferns = 0;
    int bromeliads = 0;
    std::vector<EpiphyteSite> sites;
};

/*
* A tree's epiphytes from its skeleton and seed: epiphytesOnTree rosettes on chosen sites (a big tree's fernShare of
* them ferns, the rest bromeliads; a small tree's all bromeliads), and its limbs' moss in tiles of
* forestLimbMossLayer's.
*/
EpiphyteMeshes growEpiphytes(const TreeSkeleton& skeleton, const ForestEpiphyteSettings& settings, bool bigTree, std::uint32_t seed);
