#pragma once

#include "math/vec3.h"
#include "scene/asset_handles.h"
#include "scene/indexed_mesh.h"

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

struct TwigSet {
    std::vector<MeshHandle> prototypes;
    std::vector<TwigInstance> twigs; // brick by brick
    std::vector<TwigBrick> bricks;
};

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
