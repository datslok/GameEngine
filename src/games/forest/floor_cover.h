#pragma once

#include "assets/image_loader.h"
#include "core/job_system.h"
#include "core/pixel.h"
#include "core/settings_visitor.h"
#include "games/forest/ground_cells.h"
#include "math/vec3.h"
#include "scene/indexed_mesh.h"

#include <cstdint>
#include <vector>

/*
* The forest floor's cover (docs/research/GROUNDCOVER.md, stage 3): under a closed canopy a jungle floor has little grass but
* leaf litter nearly everywhere, tree seedlings (its commonest small plants) and small ferns. Drawn as the grass is:
* a few shared patches the vertex shaders lift onto the land, a cell of land each, as dense as the ground's leaf
* litter paint there. Fallen leaves give the litter scan relief; one picture of four bands serves every piece.
*/

// The cover's picture's bands, side by side: three tones of fallen leaf (dark, ochre, yellowed) and a green one, the
// seedlings', the ferns' and the few fresh fallen leaves'.
inline constexpr int floorCoverBands = 4;
inline constexpr int floorGreenBand = 3;

// The forest floor's cover (F5 Forest, Forest floor; start-up: the patches are grown once).
struct ForestFloorSettings {
    // A cell's side (metres): one patch each.
    float cellMetres = 8.0f;

    // Where the leaf litter paint is full: fallen leaves a square metre and their length (each 60-160% of it), seedlings
    // a square metre and their heights, small ferns a square metre and their fronds' length (each 80-100% of it).
    // The first look (Nov 16 2026) saw none: the leaves were buried under the drawn ground's triangles, and the plants
    // were too few and small (0.6 seedlings and 0.15 ferns of 0.4 m a square metre).
    float leavesPerSquareMetre = 40.0f;
    float leafLength = 0.15f;
    float seedlingsPerSquareMetre = 1.5f;
    float shortestSeedling = 0.1f;
    float tallestSeedling = 0.4f;
    float fernsPerSquareMetre = 0.4f;
    float fernLength = 0.5f;

    // How many different patches are grown, and how many densities of each.
    int variants = 4;
    int densitySteps = 4;

    // Cells within this of the path (metres), where their mean leaf litter paint is at least leastPaint.
    float reachMetres = 200.0f;
    float leastPaint = 0.05f;

    // From where a patch draws half its leaves, a quarter (and half its plants), and nothing (metres, as the grass's).
    float halfDistance = 12.0f;
    float quarterDistance = 25.0f;
    float farthest = 50.0f;
};

void visit(ForestFloorSettings& settings, SettingsVisitor& visitor);

// A fallen leaf: its middle on the ground, its length, its turn about up, how far its edges curl up (metres), its band.
struct FallenLeaf {
    Vec3 centre;
    float length = 0.12f;
    float turnRadians = 0.0f;
    float curl = 0.01f;
    int band = 0;
};

/*
* A fallen leaf appended to mesh: an ovate outline 55% as wide as long, its midrib a few millimetres above the ground,
* its edges curled up by curl and its tip half that, as drying leaves cup; six triangles round its middle, facing up,
* normals up. u across it within its band, v from its stalk to its tip.
*/
void appendFallenLeaf(IndexedMesh& mesh, const FallenLeaf& leaf);

// A tree seedling: its root, its height, its leaves, its turn and its seed.
struct Seedling {
    Vec3 root;
    float height = 0.25f;
    int leaves = 4;
    float turnRadians = 0.0f;
    std::uint32_t seed = 0;
};

/*
* A seedling appended to mesh: a thin stem from just under the ground to its height, its leaves round it by the golden
* angle over its top half, each a strap leaf (appendStrapLeaf) a quarter to 40% of its height long, broad, rising a little
* and drooping; all in the green band.
*/
void appendSeedling(IndexedMesh& mesh, const Seedling& seedling);

// A small fern: its root, its fronds' length and how many, its seed.
struct FloorFern {
    Vec3 root;
    float length = 0.4f;
    int fronds = 5;
    std::uint32_t seed = 0;
};

// A small fern appended to mesh: its fronds round its root by the golden angle (appendPinnateFrond, the palms'),
// rising 50-70 degrees and arching over, 80-100% of its length; all in the green band.
void appendFloorFern(IndexedMesh& mesh, const FloorFern& fern);

/*
* A patch centred on its cell's middle: its pieces (density times each kind's full count, in fixed random orders, so a
* sparser patch of a seed is the first of the same pieces) and its meshes: everything; half the leaves and every plant;
* a quarter of the leaves and half the plants.
*/
struct FloorPatch {
    std::vector<FallenLeaf> leaves;
    std::vector<Seedling> seedlings;
    std::vector<FloorFern> ferns;
    IndexedMesh mesh;
    IndexedMesh level;
    IndexedMesh farLevel;
};

FloorPatch growFloorPatch(const ForestFloorSettings& settings, float density, std::uint32_t seed);

/*
* The cover's picture: floorCoverBands bands of texels square side by side, rows from the bottom (v 0, a leaf's stalk):
* each a leaf filling its band (its outline is geometry), a midrib down its middle and veins slanting out, mottled, its
* edges darker; the first three the litter's colour darker, as it is and yellowed, the last the green given. Opaque.
*/
ImageData floorCoverImage(int texels, Pixel litter, Pixel green);

// Where the floor's cover grows: the cells (forestGroundCells) of the leaf litter paint, with the settings' cell, reach,
// least paint, steps and variants.
std::vector<GroundCell> forestFloorCells(const ForestFloorSettings& settings, JobSystem& jobs);
