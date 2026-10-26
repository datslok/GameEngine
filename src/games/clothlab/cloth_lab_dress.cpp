#include "games/clothlab/cloth_lab_dress.h"

#include "math/angles.h"

#include <iterator>

namespace {
    // The right half of the outline, in metres; the left half is its mirror image.
    const Vec2 hemCorner{0.40f, 0.0f};
    const Vec2 armpit{0.25f, 0.61f};
    const Vec2 armholeBend{0.19f, 0.64f};
    const Vec2 strapOuter{0.19f, 0.92f};
    const Vec2 strapInner{0.12f, 0.95f};

    // How low each neckline dips at the middle: the front's 15 cm below the straps, the back's 5.
    constexpr float frontNeckHeight = 0.80f;
    constexpr float backNeckHeight = 0.90f;

    // Points along each curve; the mesher spaces the edge's own points evenly along this polyline.
    constexpr int curveSegments = 24;

    // A quadratic Bezier curve from start to end, leaving start towards bend and arriving at end from it.
    std::vector<Vec2> curve(const Vec2& start, const Vec2& bend, const Vec2& end) {
        std::vector<Vec2> points;
        for (int index = 0; index <= curveSegments; ++index) {
            const float t = static_cast<float>(index) / static_cast<float>(curveSegments);
            points.push_back(start * ((1.0f - t) * (1.0f - t)) + bend * (2.0f * (1.0f - t) * t) + end * (t * t));
        }
        return points;
    }

    // The same points across the middle line and in the other order: the matching edge on the outline's left half.
    std::vector<Vec2> mirrorImage(const std::vector<Vec2>& points) {
        std::vector<Vec2> mirrored;
        for (auto point = points.rbegin(); point != points.rend(); ++point) {
            mirrored.push_back(Vec2{-point->x, point->y});
        }
        return mirrored;
    }

    // One panel: the shared outline with its own neckline, edges in DressEdge's order.
    PatternPiece panel(float neckHeight) {
        const std::vector<Vec2> rightSide{hemCorner, armpit};
        const std::vector<Vec2> rightArmhole = curve(armpit, armholeBend, strapOuter);
        const std::vector<Vec2> rightShoulder{strapOuter, strapInner};
        std::vector<Vec2> neckline = curve(strapInner, Vec2{strapInner.x, neckHeight}, Vec2{0.0f, neckHeight});
        const std::vector<Vec2> leftNeck = mirrorImage(neckline);
        neckline.insert(neckline.end(), std::next(leftNeck.begin()), leftNeck.end());

        PatternPiece piece;
        piece.edges = {PatternEdge{{Vec2{-hemCorner.x, hemCorner.y}, hemCorner}},
                       PatternEdge{rightSide},
                       PatternEdge{rightArmhole},
                       PatternEdge{rightShoulder},
                       PatternEdge{neckline},
                       PatternEdge{mirrorImage(rightShoulder)},
                       PatternEdge{mirrorImage(rightArmhole)},
                       PatternEdge{mirrorImage(rightSide)}};
        return piece;
    }

    std::uint32_t index(DressPiece piece) {
        return static_cast<std::uint32_t>(piece);
    }

    std::uint32_t index(DressEdge edge) {
        return static_cast<std::uint32_t>(edge);
    }

    PatternSeam seam(DressEdge front, DressEdge back) {
        return PatternSeam{index(DressPiece::Front), index(front), index(DressPiece::Back), index(back)};
    }
}

/*
* Front and back seen from outside put each side seam on opposite halves of the two outlines: the front's right side
* meets the back's left, at the wearer's left, and so on round.
*/
Pattern twoPanelDress() {
    Pattern pattern;
    pattern.pieces = {panel(frontNeckHeight), panel(backNeckHeight)};
    pattern.seams = {seam(DressEdge::RightSide, DressEdge::LeftSide), seam(DressEdge::LeftSide, DressEdge::RightSide),
                     seam(DressEdge::RightShoulder, DressEdge::LeftShoulder), seam(DressEdge::LeftShoulder, DressEdge::RightShoulder)};
    return pattern;
}

std::vector<Vec3> dressAroundMannequin(const PatternMesh& meshed, const Vec3& feet) {
    std::vector<Vec3> start;
    start.reserve(meshed.flatPositions.size());
    for (std::size_t point = 0; point < meshed.flatPositions.size(); ++point) {
        const float facing = meshed.pieces[point] == index(DressPiece::Front) ? 0.0f : 0.5f * fullTurnRadians;
        start.push_back(wrapOnCylinder(meshed.flatPositions[point], feet + dressAxisOffset, dressWrapRadius, facing));
    }
    return start;
}
