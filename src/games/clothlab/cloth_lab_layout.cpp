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
    const Vec3 reach{clothLabMannequinStationHalfWidth, 0.0f, clothLabMannequinStationHalfWidth};
    const Aabb mannequin{mannequinFeet - reach, mannequinFeet + reach + Vec3{0.0f, clothLabMannequinStationHeight, 0.0f}};
    return {drape, mat.bounds(), frameBounds(fabricFrame), mannequin};
}

/*
* Sized for sheets about 1.5 m square: the drape bar 2 m up, the ball's top 0.6 m below it and its centre just in front
* of the bar, so a sheet pinned under the bar cannot hang straight down but comes to rest draped over the ball; the
* fabric bar long enough for two such sheets with a gap between them.
*/
ClothLabLayout buildClothLabLayout() {
    ClothLabLayout layout;

    constexpr float ballRadius = 0.4f;
    constexpr float pedestalHalfHeight = 0.3f;
    constexpr float barDepth = -1.35f;
    constexpr float ballInFrontOfBar = 0.2f;
    layout.pedestal = ClothLabBlock{Vec3{-4.0f, pedestalHalfHeight, barDepth + ballInFrontOfBar}, Vec3{0.15f, pedestalHalfHeight, 0.15f}};
    layout.ball = Sphere{Vec3{-4.0f, 2.0f * pedestalHalfHeight + ballRadius, barDepth + ballInFrontOfBar}, ballRadius};
    layout.drapeFrame = ClothLabFrame{Vec3{-4.0f, 2.0f + clothLabBarHalfThickness, barDepth}, 1.0f};
    layout.drapeSheetCentre = Vec3{layout.drapeFrame.barCentre.x, clothLabBarUnderside(layout.drapeFrame),
                                   layout.drapeFrame.barCentre.z + 0.5f * clothLabDrapeSheetSize};

    layout.mat = ClothLabBlock{Vec3{0.0f, 0.005f, -1.0f}, Vec3{1.5f, 0.005f, 1.5f}};
    layout.bigSheetCentre = Vec3{layout.mat.centre.x, layout.mat.centre.y + layout.mat.halfSize.y + clothLabBigSheetHeight, layout.mat.centre.z};

    layout.fabricFrame = ClothLabFrame{Vec3{4.5f, 2.0f + clothLabBarHalfThickness, -1.35f}, 1.8f};
    const ClothLabFrame& bar = layout.fabricFrame;
    layout.sheetCentre = Vec3{bar.barCentre.x - 0.5f * bar.barHalfLength, clothLabBarUnderside(bar), bar.barCentre.z + 0.5f * clothLabSheetDepth};

    layout.mannequinFeet = Vec3{-7.5f, 0.0f, -1.0f};
    return layout;
}

Mat4 clothLabMannequinPlacement(const Aabb& modelBounds, const Vec3& feet) {
    const float scale = clothLabMannequinHeight / (modelBounds.maximum.y - modelBounds.minimum.y);
    const Vec3 middle = modelBounds.centre();
    return Mat4::translation(feet.x - scale * middle.x, feet.y - scale * modelBounds.minimum.y, feet.z - scale * middle.z) *
           Mat4::scaling(scale, scale, scale);
}
