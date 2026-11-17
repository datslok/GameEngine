#pragma once

#include "core/settings_visitor.h"
#include "games/forest/forest_trees.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <vector>

/*
* The jungle's understorey (ROADMAP phase 18, docs/research/JUNGLE.md, The understorey): plants at knee to chest height, thick
* along the path so the view closes at eye level, thinning away from it. Scanned plants (Poly Haven's fern, calathea and
* anthurium, each file several plants side by side, each a variant) stood on their feet, simplified (scene/simplify_mesh)
* once for drawing and further for their shadow's stand-in, and placed many times, turned and scaled up from house-plant
* size. Without the scans (a fresh clone, the tests), a plain stand-in plant.
*/

enum class UnderstoreyKind {
    Fern,
    Calathea,
    Anthurium
};

inline constexpr int understoreyKindCount = 3;

// Each kind's scan, its folder under the forest's assets and its file's name stem (<name>/<name>_1k.gltf).
const char* understoreyScanName(UnderstoreyKind kind);

// The understorey (F5 Forest, Understorey; read at start-up).
struct ForestUnderstoreySettings {
    // The grid the plants are thrown on (one at most a cell, jittered within it), metres.
    float spacing = 1.5f;

    // Every cell is planted within nearPathMetres of the path's centre, farShare of them past farPathMetres, a straight
    // blend between: thick along the walk, thinning into the trees.
    float nearPathMetres = 20.0f;
    float farPathMetres = 60.0f;
    float farShare = 0.35f;

    // How far plants keep from the path's centre and from every trunk's reach (metres).
    float pathClearance = 1.5f;
    float trunkClearance = 1.0f;

    // Each kind's share of the plants (the anthuriums the rest), and its tallest plant's height (metres; each plant
    // within understoreyScaleSpread of its own): the scans are house plants 0.2-0.5 m tall, a jungle's are 1-2 m, with
    // leaves to match (a giant calathea's are most of a metre long).
    float fernShare = 0.45f;
    float calatheaShare = 0.3f;
    float fernHeight = 1.3f;
    float calatheaHeight = 1.5f;
    float anthuriumHeight = 1.7f;

    // The share of each scan's triangles drawn (its border kept), the share its first level of detail keeps (docs/research/LOD.md;
    // the shadow's stand-in is the second), and the share its shadow's stand-in keeps.
    float keepShare = 0.3f;
    float levelKeepShare = 0.12f;
    float shadowKeepShare = 0.05f;

    // How many of the sun's cascades its shadows are drawn into, nearest first (ModelRenderer::shadowCascades): a plant's
    // shadow is a few metres long; the 10,000 plants in every cascade cost most of 22 ms of shadows while turning.
    int shadowCascades = 2;

    // The card past the levels (scene/impostor.h, docs/research/LOD.md), as the trees': the error, in metres, from which a plant is
    // drawn as its card (a plant a twentieth of a tree's height, so about that share of the trees' 1.5 m: a 1.5 m plant
    // turns into its card some 20 m away at 2 pixels on a 1080-line screen), its frames a side and each frame's texels
    // (fewer than the trees': a plant's card is never big on screen).
    float impostorErrorMetres = 0.06f;
    int impostorFrames = 8;
    int impostorTexels = 64;
};

void visit(ForestUnderstoreySettings& settings, SettingsVisitor& visitor);

// The fixed parts: how far each plant varies from its kind's size, how far its foot sinks into the ground, and the most
// its simplifying may move the surface (a share of the plant's size).
inline constexpr float understoreyScaleSpread = 0.25f;
inline constexpr float understoreySinkMetres = 0.05f;
inline constexpr float understoreySimplifyError = 0.01f;

// A kind's tallest plant's height (metres).
float understoreyHeight(const ForestUnderstoreySettings& settings, UnderstoreyKind kind);

// The scale that brings a kind's tallest plant (its variants, footed) to height; 1 for none.
float understoreyKindScale(const std::vector<IndexedMesh>& plants, float height);

// Where a plant stands: its foot (sunk a little into the ground), its size against its kind's (within
// understoreyScaleSpread of 1), turn about up, kind, and a number in 0..1 that picks one of its kind's variants (however
// many there are).
struct UnderstoreyPlacement {
    Vec3 foot;
    float scale = 1.0f;
    float turnRadians = 0.0f;
    UnderstoreyKind kind = UnderstoreyKind::Fern;
    float variantPick = 0.0f;
};

/*
* Where a kind of plant may stand on the floor (the understorey's, the palms'): a grid spacing apart, every cell kept
* within nearPathMetres of the path's centre, farShare of them past farPathMetres, a straight blend between; none within
* pathClearance of the path's centre or trunkClearance of a trunk's reach.
*/
struct PlantingGrid {
    float spacing = 1.5f;
    float nearPathMetres = 20.0f;
    float farPathMetres = 60.0f;
    float farShare = 0.35f;
    float pathClearance = 1.5f;
    float trunkClearance = 1.0f;
};

// A cell planted: where on the floor (x, z), and the first of its own random numbers left for its plant (each cell has
// drawsPerCell of them, the first three spent placing it).
struct PlantedCell {
    Vec2 place;
    std::uint64_t firstDraw = 0;
};

inline constexpr std::uint64_t plantingDrawsUsed = 3;

// The understorey's random numbers: its seed, and each cell's numbers (three placing it, four for its plant).
inline constexpr std::uint64_t understoreySeed = 0x554e4445ull;
inline constexpr std::uint64_t understoreyDrawsPerCell = 7;

/*
* The cells planted: a jittered grid over the trees' stretch of the floor (treeNorthmost to treeSouthmost, within
* treeSpread of the valley's axis), one place a cell, kept by its distance from the path and clear of the path and the
* trunks (given as StandingCrowns, their radius the reach), in rows from the north. Random numbers forestUnitRandom(seed,
* cell * drawsPerCell + k); the keeping of a cell does not depend on the shares, so more or fewer plants keep the same
* places. The same every run.
*/
std::vector<PlantedCell> plantedCells(const PlantingGrid& grid, const std::vector<StandingCrown>& trunks, std::uint64_t seed,
                                      std::uint64_t drawsPerCell);

/*
* The plants' places: a jittered grid spacing apart over the trees' stretch of the floor (treeNorthmost to
* treeSouthmost, within treeSpread of the valley's axis), each cell planted with a share by its distance from the path
* (nearPathMetres, farPathMetres, farShare), none within pathClearance of the path's centre or trunkClearance of a
* trunk's reach (the trunks given as StandingCrowns, their radius the reach), none whose nominal size meets a sight line.
* The same every run; the keeping of a cell does not depend on the shares, so more or fewer plants keep the same places.
*/
std::vector<UnderstoreyPlacement> placeUnderstorey(const ForestUnderstoreySettings& settings, const std::vector<StandingCrown>& trunks,
                                                   const std::vector<SightLine>& sightLines);

// A plant's nominal sphere in the world, its kind's height across (for the sight lines).
Vec3 understoreyCentre(const ForestUnderstoreySettings& settings, const UnderstoreyPlacement& plant);
float understoreyRadius(const ForestUnderstoreySettings& settings, const UnderstoreyPlacement& plant);

// A plant moved to stand on its foot: its lowest point at y 0, its box centred over the origin.
IndexedMesh footedPlant(const IndexedMesh& plant);

// A plain stand-in plant for when the scans are missing: three crossed upright leaf blades 1 m tall, footed.
IndexedMesh understoreyStandIn();
