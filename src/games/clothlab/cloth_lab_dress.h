#pragma once

#include "cloth/cloth_pattern.h"
#include "math/vec3.h"
#include "physics/signed_distance_field.h"

#include <array>
#include <cstdint>
#include <vector>

/*
* Phase 7's dress (ROADMAP.md): a sleeveless A-line dress to the knee, sewn from two panels, a front and a back, at
* its two side seams and its two shoulders, for the cloth lab's mannequin at 1.75 m.
*
* Sized from the mannequin itself: its chest about 94 cm round (34 cm across, 25 cm deep), its waist 79 cm, its hips
* 94 cm, its armpits at 1.20-1.25 m, the tops of its shoulders at 1.44 m and their ends 21 cm from its middle. Each
* panel is a quarter of the chest round plus a few centimetres to move in, so 50 cm across at the armpit, flaring to
* 80 cm at the hem; the armholes are cut deep (the dress's armpit 10 cm under the body's), since the arms stand only
* 5-8 cm off the body there in its A-pose.
*
* Pattern coordinates in metres: x across the panel from its middle line, y up from the hem. Both panels are drawn
* seen from outside, so a panel's +x (its outline's right side) is the wearer's left on the front and the wearer's
* right on the back; the front's right side seam is sewn to the back's left one.
*/
enum class DressPiece : std::uint32_t {
    Front,
    Back,
};

// Each panel's edges, in its outline's counterclockwise order from the hem.
enum class DressEdge : std::uint32_t {
    Hem,
    RightSide,
    RightArmhole,
    RightShoulder,
    Neckline,
    LeftShoulder,
    LeftArmhole,
    LeftSide,
};

inline constexpr std::uint32_t dressEdgeCount = 8;

// How far apart the dress's points are (the mesher's spacing).
inline constexpr float dressSpacing = 0.01f;    // m

/*
* Where the panels start, laid round the mannequin before sewing: wrapped round an upright cylinder of this radius,
* the front facing +z and the back -z, a little behind the middle of the body's box (the chest and hips reach further
* back than forward), its hem this high, so the shoulder straps start about 10 cm over the shoulders and their seams
* close in the air above them: started only a few centimetres over them, a seam's straight pull ran through the rounded
* shoulder, which pushed one strap off sideways into the armpit. The cylinder is wide enough for the hem's flare to
* reach round to just short of the sides.
*/
inline constexpr float dressWrapRadius = 0.29f;                       // m
inline const Vec3 dressAxisOffset{0.0f, 0.61f, -0.02f};               // from the feet, m: the hem's height and the axis's
inline constexpr float dressShoulderHeight = 1.44f;                   // m above the feet: the tops of the mannequin's shoulders

// How long the dress is weightless while its seams pull shut round the body, before gravity lets it settle.
inline constexpr float dressSewingSeconds = 1.0f;

// The two panels' outlines and the four seams joining them: the two side seams, then the two shoulders.
Pattern twoPanelDress();

// Which of the pattern's seams are the shoulders, where the straps are pinned.
inline constexpr std::array<std::uint32_t, 2> dressShoulderSeams{2, 3};

// Each point's start: its panel wrapped round the mannequin standing on feet (each piece's flat positions on the cylinder).
std::vector<Vec3> dressAroundMannequin(const PatternMesh& meshed, const Vec3& feet);

/*
* Pinned at the shoulders (the lab's default), as a dressmaker pins a dress on a dress form: both points of each
* shoulder stitch are pins the lab carries onto the shoulders while the dress sews, and every other point is tethered
* to its nearest strap pins on its own panel (cloth_tethers.h), so the dress hangs from its straps as a pinned sheet
* hangs from its hooks, about 0.2% stretched, rather than as far as XPBD alone lets it.
*
* A pin goes exactly where it is put, through the body if its way does, so the way has two legs: across, from its
* start to where it meets its partner (the middle of their starts, above the shoulder), by dressStrapsMeetSeconds;
* then down onto the shoulder by the end of the sewing, each leg eased in and out (smoothStep) so the straps start and
* stop gently. Pins from the start, moved by the lab, because GPU cloth takes its pins when it is first uploaded.
*/
inline constexpr float dressStrapsMeetSeconds = 0.5f;

struct StrapPinPath {
    Vec3 start;
    Vec3 meeting;
    Vec3 onShoulder;
};

// Where a strap pin is seconds after the dress was made.
Vec3 strapPinPosition(const StrapPinPath& path, float seconds);

/*
* The strap pins, both points of every shoulder stitch in the stitches' order, and their ways: a pair's meeting point
* the middle of their starts, its place on the shoulder straight below that where the body's surface is thickness away
* (stepping down through the body's field a millimetre at a time; the meeting point itself if nothing is below).
*/
struct DressStrapPins {
    std::vector<std::uint32_t> points;
    std::vector<StrapPinPath> paths;
};

DressStrapPins dressStrapPins(const PatternMesh& meshed, const std::vector<Vec3>& start, const SignedDistanceField& body, float thickness);
