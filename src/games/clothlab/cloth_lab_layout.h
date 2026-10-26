#pragma once

#include "math/aabb.h"
#include "math/mat4.h"
#include "math/vec3.h"
#include "physics/shapes.h"

#include <vector>

/*
* The cloth lab's layout, in metres: a test station for each of phase 7's milestones on a plain floor, laid out along x
* in front of where the player starts (ROADMAP.md, phase 7):
*   - draping: a ball on a pedestal, under a bar to pin a sheet's corners to;
*   - piling: a mat on the floor, where a sheet dropped onto itself folds into a pile;
*   - fabrics: a wide bar to hang two sheets side by side (silk beside denim);
*   - the mannequin: a human figure to drape cloth on, and later the dress sewn from two panels.
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

// The big sheet over the mat, the cloth that gives the solver's threads work: squares x squares, size metres square,
// laid flat this far above the mat with its corners on hooks drawn in to cornerSpan apart, so it sags like a hammock.
inline constexpr int clothLabBigSheetSquares = 100;
inline constexpr float clothLabBigSheetSize = 2.0f;
inline constexpr float clothLabBigSheetCornerSpan = 1.6f;
inline constexpr float clothLabBigSheetHeight = 1.2f;

/*
* The mannequin: this model (watertight, its arms out in an A-pose, facing +z, towards where the player starts), scaled
* to this height, and the distance field the cloth collides with round it (physics/signed_distance_field.h): samples
* 5 mm apart, real distances within 3 cm of the skin. The band must stay wider than any cloth's thickness.
*/
inline constexpr const char* clothLabMannequinModel = "assets/models/human.obj";
inline constexpr float clothLabMannequinHeight = 1.75f;           // m
inline constexpr float clothLabMannequinFieldSpacing = 0.005f;    // m
inline constexpr float clothLabMannequinFieldBand = 0.03f;        // m

// The sheet dropped onto it: squares x squares, size metres square, laid flat this far above the mannequin's head and
// centred over its feet, pinned nowhere.
inline constexpr int clothLabDropSheetSquares = 60;
inline constexpr float clothLabDropSheetSize = 1.6f;
inline constexpr float clothLabDropSheetClearance = 0.2f;

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

    // The mannequin: the middle of its feet on the floor.
    Vec3 mannequinFeet;

    // Where the dropped sheet's middle starts: over the mannequin's head.
    Vec3 dropSheetCentre;

    // Where the flat drape sheet's middle starts: level with the drape bar's underside, half its size in front of it.
    Vec3 drapeSheetCentre;

    // Where the big sheet's middle starts: above the mat's middle.
    Vec3 bigSheetCentre;

    // Where the flat sheet's middle starts: level with the bar's underside, under the middle of its left half, half
    // the sheet's depth in front of it.
    Vec3 sheetCentre;

    // Every box in the level: the pedestal, the mat, then each frame's posts and bar.
    std::vector<ClothLabBlock> blocks() const;

    // The space each station takes, in the order above.
    std::vector<Aabb> stations() const;
};

ClothLabLayout buildClothLabLayout();

/*
* Where the mannequin's model goes, from its box in the model's own units: scaled to clothLabMannequinHeight, the middle
* of its box's bottom (centred across and front to back) on feet. The model already faces +z.
*/
Mat4 clothLabMannequinPlacement(const Aabb& modelBounds, const Vec3& feet);
