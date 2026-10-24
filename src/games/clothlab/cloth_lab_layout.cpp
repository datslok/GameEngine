#include "games/clothlab/cloth_lab_layout.h"

namespace {
    Aabb frameBounds(const ClothLabFrame& frame) {
        const std::vector<ClothLabBlock> blocks = clothLabFrameBlocks(frame);
        Aabb bounds = blocks[0].bounds();
        for (const ClothLabBlock& block : blocks) {
            bounds = enclosing(bounds, block.bounds());
        }
        return bounds;
    }
}

Aabb ClothLabBlock::bounds() const {
    return Aabb{centre - halfSize, centre + halfSize};
}

float clothLabBarUnderside(const ClothLabFrame& frame) {
    return frame.barCentre.y - clothLabBarHalfThickness;
}

/*
* The bar overhangs neither post: it rests on their tops, its ends flush with their outer faces, so the posts stand
* just inside the bar's length and the cloth pinned at its ends hangs clear of them.
*/
std::vector<ClothLabBlock> clothLabFrameBlocks(const ClothLabFrame& frame) {
    const float postHalfHeight = clothLabBarUnderside(frame) * 0.5f;
    const float postOffset = frame.barHalfLength - clothLabPostHalfWidth;
    const Vec3 postHalfSize{clothLabPostHalfWidth, postHalfHeight, clothLabPostHalfWidth};

    return {
        ClothLabBlock{Vec3{frame.barCentre.x - postOffset, postHalfHeight, frame.barCentre.z}, postHalfSize},
        ClothLabBlock{Vec3{frame.barCentre.x + postOffset, postHalfHeight, frame.barCentre.z}, postHalfSize},
        ClothLabBlock{frame.barCentre, Vec3{frame.barHalfLength, clothLabBarHalfThickness, clothLabPostHalfWidth}}};
}

std::vector<ClothLabBlock> ClothLabLayout::blocks() const {
    std::vector<ClothLabBlock> all{pedestal, mat};
    for (const ClothLabFrame& frame : {drapeFrame, fabricFrame}) {
        const std::vector<ClothLabBlock> frameBlocks = clothLabFrameBlocks(frame);
        all.insert(all.end(), frameBlocks.begin(), frameBlocks.end());
    }
    return all;
}

std::vector<Aabb> ClothLabLayout::stations() const {
    const Aabb drape = enclosing(enclosing(boundsOf(Shape{ball}), pedestal.bounds()), frameBounds(drapeFrame));
    return {drape, mat.bounds(), frameBounds(fabricFrame)};
}

/*
* Sized for sheets about 1.5 m square: the drape bar 2 m up, the ball's top a metre below it and half a sheet out in
* front, so a sheet pinned at the bar's ends swings down onto the ball; the fabric bar long enough for two such
* sheets with a gap between them.
*/
ClothLabLayout buildClothLabLayout() {
    ClothLabLayout layout;

    constexpr float ballRadius = 0.4f;
    constexpr float pedestalHalfHeight = 0.3f;
    layout.pedestal = ClothLabBlock{Vec3{-4.0f, pedestalHalfHeight, -0.6f}, Vec3{0.15f, pedestalHalfHeight, 0.15f}};
    layout.ball = Sphere{Vec3{-4.0f, 2.0f * pedestalHalfHeight + ballRadius, -0.6f}, ballRadius};
    layout.drapeFrame = ClothLabFrame{Vec3{-4.0f, 2.0f + clothLabBarHalfThickness, -1.35f}, 1.0f};

    layout.mat = ClothLabBlock{Vec3{0.0f, 0.005f, -1.0f}, Vec3{1.5f, 0.005f, 1.5f}};

    layout.fabricFrame = ClothLabFrame{Vec3{4.5f, 2.0f + clothLabBarHalfThickness, -1.35f}, 1.8f};
    return layout;
}
