#pragma once

#include "math/aabb.h"
#include "math/vec3.h"
#include "physics/shapes.h"

#include <vector>

/*
* The cloth lab's layout, in metres: a test station for each of phase 7's milestones on a plain floor, laid out along x
* in front of where the player starts (ROADMAP.md, phase 7):
*   - draping: a ball on a pedestal, under a bar to pin a sheet's corners to;
*   - piling: a mat on the floor, where a sheet dropped onto itself folds into a pile;
*   - fabrics: a wide bar to hang two sheets side by side (silk beside denim).
* Kept apart from the game so the tests can check it without building one.
*/

// The floor: a flat square this far from the origin each way, its top at y = 0.
inline constexpr float clothLabFloorHalfSize = 30.0f;

// Where the first-person body starts, looking at the stations along -z.
inline const Vec3 clothLabFirstPersonFeet{0.0f, 0.0f, 4.0f};

// A frame's posts and bar: thin square sections, like a clothes rail's.
inline constexpr float clothLabPostHalfWidth = 0.04f;
inline constexpr float clothLabBarHalfThickness = 0.025f;

// The hanging sheet on the fabric bar's left half: columns x rows squares, width by depth, laid flat level with the bar's
// underside and stretching forward from it, pinned at the two corners of its back edge under the bar.
inline constexpr int clothLabSheetColumns = 28;
inline constexpr int clothLabSheetRows = 30;
inline constexpr float clothLabSheetWidth = 1.4f;
inline constexpr float clothLabSheetDepth = 1.5f;

// The drape sheet at the drape station: a square of squares x squares, laid flat level with the drape bar's underside
// and stretching forward from it over the ball, pinned at the two corners of its back edge.
inline constexpr int clothLabDrapeSheetSquares = 24;
inline constexpr float clothLabDrapeSheetSize = 1.2f;

// The floor's collision box reaches this far below the ground, so nothing can pass through it in a substep.
inline constexpr float clothLabFloorDepth = 1.0f;

// A box: its centre and half its size along each axis.
struct ClothLabBlock {
    Vec3 centre;
    Vec3 halfSize;

    Aabb bounds() const;
};

// A frame to pin cloth to: a bar along x resting on two posts at its ends, standing on the floor.
struct ClothLabFrame {
    Vec3 barCentre;
    float barHalfLength = 1.0f;
};

// The bar's lowest face, where pinned cloth hangs from.
float clothLabBarUnderside(const ClothLabFrame& frame);

// The frame's two posts (the -x one first) and then its bar.
std::vector<ClothLabBlock> clothLabFrameBlocks(const ClothLabFrame& frame);

struct ClothLabLayout {
    // Draping.
    Sphere ball;
    ClothLabBlock pedestal;
    ClothLabFrame drapeFrame;

    // Piling.
    ClothLabBlock mat;

    // Fabrics.
    ClothLabFrame fabricFrame;

    // Where the flat drape sheet's middle starts: level with the drape bar's underside, half its size in front of it.
    Vec3 drapeSheetCentre;

    // Where the flat sheet's middle starts: level with the bar's underside, under the middle of its left half, half
    // the sheet's depth in front of it.
    Vec3 sheetCentre;

    // Every box in the level: the pedestal, the mat, then each frame's posts and bar.
    std::vector<ClothLabBlock> blocks() const;

    // The space each station takes, in the order above.
    std::vector<Aabb> stations() const;
};

ClothLabLayout buildClothLabLayout();
