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
    // How the plants are planted from the valley's maps (valley_maps): every 1.5 m cell on the floor within 20 m of the
    // path's centre, 35% of them past 60 m (thick along the walk, thinning into the trees), more in the gaps' light;
    // 1.5 m clear of the path's centre.
    PlantingLayer planting{.floorPerHectare = 4500.0f, .wallPerHectare = 2500.0f, .rimPerHectare = 3000.0f, .footprintMetres = 1.5f,
                           .jitterShare = 1.0f, .gapShare = 1.5f, .pathClearance = 1.5f, .nearPathMetres = 20.0f, .farPathMetres = 60.0f,
                           .farShare = 0.35f};

    // How far plants keep from every trunk's reach (metres).
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

    // How many of the sun's cascades its shadows are drawn into, nearest first (ModelRenderer::shadowCascades; 0 all).
    // All: the nearest two ended the plants' shadows about 20 m away, where they popped in plant by plant as one walked
    // up; with the shadows culled and cached on the GPU, all four cost about 0.2 ms more of the shadow pass walking or turning.
    int shadowCascades = 0;

    // How many of the nearest cascades the leaves seen cast into themselves, the stand-in only past them
    // (ModelRenderer::shadowFromCascade): the stand-in's coarse cards poked out round the seen leaves and shadowed them
    // in polygons close up. 2, about 9 m, as Horizon Zero Dawn's (docs/research/FOLIAGESHADOWS.md); 0 the stand-in alone.
    int seenShadowCascades = 2;

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

// The understorey's random numbers' seed.
inline constexpr std::uint64_t understoreySeed = 0x554e4445ull;

/*
* The understorey's layer of the planting (games/forest/forest_planting.h): its density from its settings, its kinds by
* their shares (fernShare, calatheaShare, the anthuriums the rest), each its kind's height's sphere (understoreyRadius),
* sized within understoreyScaleSpread of 1 and turned at random, sunk understoreySinkMetres, with a pick of its kind's
* variants. Kept clear of the trunks by the forest's layers.
*/
PlantingLayerRecord understoreyPlantingLayer(const ForestUnderstoreySettings& settings);

// The farthest any plant's nominal sphere reaches (the tallest kind at its largest): the canopy cap it reads.
float understoreyReach(const ForestUnderstoreySettings& settings);

// A plant's nominal sphere in the world, its kind's height across (for the sight lines).
Vec3 understoreyCentre(const ForestUnderstoreySettings& settings, const UnderstoreyPlacement& plant);
float understoreyRadius(const ForestUnderstoreySettings& settings, const UnderstoreyPlacement& plant);

// A plant moved to stand on its foot: its lowest point at y 0, its box centred over the origin.
IndexedMesh footedPlant(const IndexedMesh& plant);

// A plain stand-in plant for when the scans are missing: three crossed upright leaf blades 1 m tall, footed.
IndexedMesh understoreyStandIn();
