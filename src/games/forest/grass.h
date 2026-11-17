#pragma once

#include "assets/image_loader.h"
#include "core/job_system.h"
#include "core/pixel.h"
#include "core/settings_visitor.h"
#include "games/forest/ground_cells.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <vector>

/*
* Grass in the open (docs/research/GROUNDCOVER.md, stage 2): blades as curves, clumped as Ghost of Tsushima's are, on a few
* shared square patches the vertex shaders lift onto the land (ModelRenderer::followsGround). Each cell of the land
* within reach of the path draws one patch, as dense as the ground's grass paint there, so the blades end where the
* grass texture does: the meadow, the clearing, the canopy's gaps. A real jungle floor under a closed canopy has little
* grass (0.2-6.5% of the sun reaches it); the forest floor's own cover is the next stage.
*/

// The blades' picture's tone bands, side by side across it: each clump's blades read one (u within its quarter).
inline constexpr int grassToneBands = 4;

// A clump's height, a share of its blades' own: between these.
inline constexpr float grassLeastClumpHeight = 0.7f;
inline constexpr float grassMostClumpHeight = 1.2f;

// The grass (F5 Forest, Grass; start-up: the patches are grown once).
struct ForestGrassSettings {
    // A cell's side (metres): one patch each.
    float cellMetres = 8.0f;

    // Blades a square metre where the grass paint is full, their heights (each a random share between, times its
    // clump's) and their width at the root, metres. Games draw blades wider than real ones (5 mm) and fewer: the ground's
    // grass texture under them fills between. 60 a square metre 1.2 cm wide was too sparse (the user's first look, Nov 16
    // 2026): about 3.75 times the cover.
    float bladesPerSquareMetre = 150.0f;
    float shortest = 0.25f;
    float tallest = 0.6f;
    float bladeWidth = 0.018f;

    // The clumps' spacing (metres): each clump's blades share a height, a lean and a tone.
    float clumpMetres = 0.8f;

    // How many different patches are grown, and how many densities of each (a cell takes the nearest to its paint).
    int variants = 4;
    int densitySteps = 4;

    // Cells are grown within this of the path (metres, its centre line from the start to the clearing), and only where
    // their mean grass paint is at least leastPaint.
    float reachMetres = 200.0f;
    float leastPaint = 0.05f;

    // From where a patch draws half its blades, a quarter, and none (metres, at a 1080-pixel, 70 degree view and the
    // levels' default pixel error; they scale with both).
    float halfDistance = 15.0f;
    float quarterDistance = 30.0f;
    float farthest = 60.0f;
};

void visit(ForestGrassSettings& settings, SettingsVisitor& visitor);

// One blade: its root (on the ground, y 0), its height, the level way it leans and how far (the tip's sideways share of
// its height), which way its face turns (radians about up), its width at the root, its tone band and its clump.
struct GrassBlade {
    Vec3 root;
    float height = 0.4f;
    Vec3 lean{1.0f, 0.0f, 0.0f};
    float leanShare = 0.2f;
    float facingRadians = 0.0f;
    float width = 0.012f;
    int band = 0;
    int clump = 0;
};

/*
* A blade appended to mesh: a quadratic curve from its root, rising, bending over its lean, segments long, the
* segments closer towards the tip where it bends most (t = 1 - (1 - s)^1.5), two vertices a row across its width
* (narrowing as 1 - t^2) and one at its tip. Its normals are its face's tilted up and rounded across it (out towards
* each edge), so the field is lit from above whichever way it faces, with the material keeping them on its back
* (Material::backKeepsNormal). u across it within its band, v from the root (0) to the tip (1).
*/
void appendGrassBlade(IndexedMesh& mesh, const GrassBlade& blade, int segments);

/*
* A patch: its blades (density times the full count of its cell, in a fixed random order, so a sparser patch of the same
* seed is the first of the same blades) and its meshes, centred on the cell's middle: all its blades in 4 segments; the
* first half widened by sqrt 2 in 3; the first quarter widened by 2 in 2.
*/
struct GrassPatch {
    std::vector<GrassBlade> blades;
    IndexedMesh mesh;
    IndexedMesh level;
    IndexedMesh farLevel;
};

GrassPatch growGrassPatch(const ForestGrassSettings& settings, float density, std::uint32_t seed);

// A cell's density step for its mean grass paint: 0 (no patch) below leastPaint, else the nearest of 1 to densitySteps.
int grassDensityStep(const ForestGrassSettings& settings, float paint);

/*
* The blades' picture, texels square, rows from the bottom (v 0, the root) as loaded images are: grassToneBands bands
* across it, each the colour given in its own tone (darker, deeper greens to lighter, yellower), each darker at the
* root (the grass's own shade) and lighter and drier towards the tip, faintly streaked along. Opaque: a blade's
* outline is its geometry. The same colour draws the same picture.
*/
ImageData grassBladeImage(int texels, Pixel colour);

// Where the forest's grass grows: the cells (forestGroundCells) of the grass paint, with the settings' cell, reach,
// least paint, steps and variants.
using GrassCell = GroundCell;
std::vector<GrassCell> forestGrassCells(const ForestGrassSettings& settings, JobSystem& jobs);
