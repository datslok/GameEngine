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

// The banner on the fabric bar's left half, a stand-in for cloth until the solver comes (its waves are the game's): a
// sheet of columns x rows squares, width by height, its top edge along the bar's underside.
inline constexpr int clothLabBannerColumns = 28;
inline constexpr int clothLabBannerRows = 30;
inline constexpr float clothLabBannerWidth = 1.4f;
inline constexpr float clothLabBannerHeight = 1.5f;

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

    // Where the banner's middle hangs, still: under the middle of the fabric bar's left half.
    Vec3 bannerCentre;

    // Every box in the level: the pedestal, the mat, then each frame's posts and bar.
    std::vector<ClothLabBlock> blocks() const;

    // The space each station takes, in the order above.
    std::vector<Aabb> stations() const;
};

ClothLabLayout buildClothLabLayout();
