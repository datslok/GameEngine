#pragma once

#include "math/vec2.h"
#include "scene/image_data.h"

#include <cstdint>
#include <vector>

class JobSystem;

/*
* The structures' panel lines (ROADMAP phase 18, the structures; docs/research/STRUCTURES.md): a square tile of panels,
* seamless, read once in each facet's own plane (Material::groundProjection, Facets), so its rows run level round a tower
* like floors. Two patterns: the Ring's stealth seams (seamLayout) and the Bastion's concrete formwork (formworkLayout).
* The stealth pattern: rows 2-4 m tall, each split into panels 2-6 m wide; some row edges and panel edges sawtooth at
* 45 degrees, the stealth language's broken edges; now and then a small inset hatch inside a panel. A seam is a groove
* of seamHalfWidth either side of its line, bevelled over seamBevel: dark in the albedo, tilting the normal map.
* Coordinates are metres across the tile (x along a facet, y up it), wrapping at the tile's size.
*/
inline constexpr float seamHalfWidth = 0.015f;
inline constexpr float seamBevel = 0.02f;

// A boundary between rows (its height) or between panels in a row (its place across): straight, or a sawtooth.
struct SeamLine {
    float at = 0.0f;
    bool sawtooth = false;
};

// A row: its bottom boundary (the next row's bottom is its top; the last row's top is the first's bottom, a tile up) and
// the splits between its panels, ascending, the first panel wrapping round from the last split.
struct SeamRow {
    SeamLine bottom;
    std::vector<SeamLine> splits;
};

// An inset hatch: a rectangle's corner and size, inside one panel.
struct SeamHatch {
    Vec2 corner;
    Vec2 size;
};

// The pattern: the tile's size, its rows and hatches, and the formwork's tie holes on a square grid of tiePitch (0 none),
// each at the middle of its grid square, a groove's width across.
struct SeamLayout {
    float tile = 0.0f;
    std::vector<SeamRow> rows;
    std::vector<SeamHatch> hatches;
    float tiePitch = 0.0f;
};

// The stealth pattern (the Ring) for a tile of tileMetres, the same for the same seed.
SeamLayout seamLayout(float tileMetres, std::uint32_t seed);

/*
* Concrete's formwork (the Bastion; docs/research/STRUCTURES.md): pours formworkPour tall, each of panels about
* formworkPanel wide, every other row's joints moved half a panel, and tie holes on a grid about formworkTie apart; all
* straight, no hatches. Each spacing is fitted to a whole number in the tile, so the tile repeats seamlessly.
*/
inline constexpr float formworkPour = 2.75f;
inline constexpr float formworkPanel = 2.4f;
// About 0.6 m (the research), a hair under it so the default 16.5 m tile's count (28) is no rounding tie.
inline constexpr float formworkTie = 0.59f;

SeamLayout formworkLayout(float tileMetres);

/*
* How far a place is from the nearest seam's line (metres), the tile wrapping: row boundaries, panel splits (each a segment
* from its row's bottom to its top), hatch outlines and tie holes' middles. A sawtooth runs at 45 degrees, its teeth
* sawtoothTooth tall.
*/
inline constexpr float sawtoothTooth = 0.3f;

float seamDistance(const SeamLayout& layout, Vec2 place);

// The surface's height at a distance from a seam's line: 0 in the groove, 1 on a panel, smooth across the bevel.
float seamHeight(float distance);

/*
* The tile's textures, size texels across: albedo (sRGB, white panels each a few percent lighter or darker, grooves
* darkened to grooveShade), height (linear grey, seamHeight: 0 in a groove, 1 on a panel; the moss's blend fills the
* grooves first) and normal map (linear, decodeNormalTexel's: x along the tile's x, y along its y, made from the height
* with grooves grooveDepth metres deep). Row y of an image is the tile's y, upwards. A row of texels a job.
*/
inline constexpr float grooveShade = 0.55f;
inline constexpr float grooveDepth = 0.02f;

struct SeamImages {
    ImageData albedo;
    ImageData height;
    ImageData normal;
};

SeamImages seamImages(const SeamLayout& layout, int size, JobSystem& jobs);
