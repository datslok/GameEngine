#include "games/clothlab/cloth_lab_dress.h"

#include "games/clothlab/cloth_lab_layout.h"
#include "math/angles.h"
#include "math/smooth_step.h"
#include "scene/collision_mesh.h"

#include <iterator>

namespace {
    // The right half of the outline, in metres; the left half is its mirror image.
    const Vec2 hemCorner{0.42f, 0.0f};
    const Vec2 armpit{0.27f, 0.61f};
    const Vec2 armholeBend{0.21f, 0.64f};
    const Vec2 strapOuter{0.19f, 0.92f};
    const Vec2 strapInner{0.12f, 0.95f};

    // How low each neckline dips at the middle: the front's 15 cm below the straps, the back's 5.
    constexpr float frontNeckHeight = 0.80f;
    constexpr float backNeckHeight = 0.90f;

    // Points along each curve; the mesher spaces the edge's own points evenly along this polyline.
    constexpr int curveSegments = 24;

    // How the shoulder is found below a strap's meeting point: steps of a millimetre, down to 30 cm.
    constexpr float shoulderSearchStep = 0.001f;
    constexpr int shoulderSearchSteps = 300;

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

Vec3 strapPinPosition(const StrapPinPath& path, float seconds) {
    if (seconds < dressStrapsMeetSeconds) {
        return path.start + (path.meeting - path.start) * smoothStep(0.0f, dressStrapsMeetSeconds, seconds);
    }
    return path.meeting + (path.onShoulder - path.meeting) * smoothStep(dressStrapsMeetSeconds, dressSewingSeconds, seconds);
}

DressStrapPins dressStrapPins(const PatternMesh& meshed, const std::vector<Vec3>& start, const SignedDistanceField& body, float thickness) {
    DressStrapPins pins;
    for (const std::uint32_t seam : dressShoulderSeams) {
        for (const ClothStitch& stitch : meshed.seams.at(seam)) {
            const Vec3 meeting = (start.at(stitch.first) + start.at(stitch.second)) * 0.5f;
            Vec3 onShoulder = meeting;
            for (int step = 1; step <= shoulderSearchSteps; ++step) {
                const Vec3 below = meeting - Vec3{0.0f, shoulderSearchStep * static_cast<float>(step), 0.0f};
                if (sampleField(body, below).distance <= thickness) {
                    onShoulder = below;
                    break;
                }
            }
            for (const std::uint32_t point : {stitch.first, stitch.second}) {
                pins.points.push_back(point);
                pins.paths.push_back(StrapPinPath{start[point], meeting, onShoulder});
            }
        }
    }
    return pins;
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

std::vector<std::uint32_t> dressHemPoints(const PatternMesh& meshed) {
    std::vector<std::uint32_t> hem;
    for (const DressPiece piece : {DressPiece::Front, DressPiece::Back}) {
        const std::vector<std::uint32_t>& points = meshed.edgePoints.at(index(piece)).at(index(DressEdge::Hem));
        hem.insert(hem.end(), points.begin(), points.end());
    }
    return hem;
}

SignedDistanceField mannequinField(const Mesh& model, const Mat4& placement, JobSystem& jobs) {
    return buildSignedDistanceField(collisionMesh(model, placement), clothLabMannequinFieldSpacing, clothLabMannequinFieldBand, jobs);
}

DressOnMannequin dressOnMannequin(const SignedDistanceField& body, const Vec3& feet, float spacing, const ClothSettings& settings, bool pinned,
                                  JobSystem& jobs) {
    const PatternMesh pattern = meshPattern(twoPanelDress(), spacing);
    const std::vector<Vec3> start = dressAroundMannequin(pattern, feet);
    DressOnMannequin dress{pattern.mesh, makeCloth(pattern.mesh, start, settings, clothSewing(pattern)), {}, dressHemPoints(pattern)};
    if (pinned) {
        const DressStrapPins straps = dressStrapPins(pattern, start, body, settings.thickness);
        addClothPins(dress.cloth, straps.points, jobs);
        dress.strapPaths = straps.paths;
    }
    return dress;
}

void prepareDressTick(Cloth& dress, const std::vector<StrapPinPath>& strapPaths, const ClothSettings& fabric, double simulationSeconds, float tickSeconds) {
    dress.settings = fabric;
    if (simulationSeconds < dressSewingSeconds) {
        dress.settings.gravity = 0.0f;
    }

    const float tickEnd = static_cast<float>(simulationSeconds) + tickSeconds;
    for (std::size_t pin = 0; pin < strapPaths.size(); ++pin) {
        dress.pins[pin].position = strapPinPosition(strapPaths[pin], tickEnd);
    }
}
