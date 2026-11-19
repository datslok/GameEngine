#pragma once

#include "math/vec3.h"
#include "scene/image_data.h"
#include "scene/mesh.h"

#include <cstdint>
#include <vector>

class SettingsVisitor;

/*
* The Bastion (ROADMAP phase 18, the structures; docs/research/STRUCTURES.md, "Planned: the Bastion"): a military brutalist
* megatower standing where the Monolith was, its front face's middle on forestMonolithPlace facing the clearing. An armoured
* glacis battered in from its foot, three tiers of concrete stepping in, each face ribbed by fins with slots between, and a
* command head overhanging them on 45-degree corbels. The far towers come from the same kit, and the old strut to the
* Monolith's pillar is an armoured bridge into its side.
*
* Its shape (the start-up part: height, the shaft's width, the glacis, the fins, the head's overhang, all in metres) and
* its concrete (live: albedo, roughness, how deep the formwork's seams read, the formwork tile's size). At night (stage 4,
* "Lights at night"): the share of the slit windows lit (a scale on each row's, start-up), the windows' brightness and
* their cells' pitch, the light strips' brightness, and the floodlights' intensity and range (live).
*/
struct ForestBastionSettings {
    float height = 610.0f;
    float shaftWidth = 200.0f;
    float glacisHeight = 80.0f;
    float glacisShare = 1.45f;
    float finPitch = 7.0f;
    float finDepth = 2.5f;
    float majorFinDepth = 8.0f;
    float headOverhang = 30.0f;
    float concreteAlbedo = 0.25f;
    float concreteRoughness = 0.9f;
    float formworkDepth = 1.0f;
    float formworkTile = 16.5f;
    float windowsLit = 1.0f;
    float windowBrightness = 6.0f;
    float windowPitch = 6.0f;
    float stripBrightness = 12.0f;
    float floodIntensity = 1500.0f;
    float floodRange = 300.0f;

    bool operator==(const ForestBastionSettings&) const = default;
};

// The windows' colours (linear, at brightness 1): high-pressure sodium, about 2000 K, and the teal of the strips.
inline const Vec3 bastionWarmLight{1.0f, 0.52f, 0.18f};
inline const Vec3 bastionCoolLight{0.25f, 0.85f, 1.0f};
// The floodlights' colour (linear): warm white, about 3000 K.
inline const Vec3 bastionFloodLight{1.0f, 0.72f, 0.45f};

void visit(ForestBastionSettings& settings, SettingsVisitor& visitor);

// The fins on a face: their pitch, width and depth, and every majorEvery-th from the middle a major fin, wider and deeper.
struct BastionFins {
    float pitch = 0.0f;
    float width = 0.0f;
    float depth = 0.0f;
    int majorEvery = 0;
    float majorWidth = 0.0f;
    float majorDepth = 0.0f;
};

// A tier: its bottom and top above the foot and its width (x); its depth (z) is the shape's depthShare of it.
struct BastionTier {
    float bottom = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
};

/*
* A tower of the kit, in its own frame with the origin at its foot (the middle of the shaft's footprint at the ground's
* level): sunk below it; the glacis from -sink to glacisTop, glacisWidth wide at the bottom and the first tier's width at
* the top; the tiers bottom to top, touching; the head from headBottom to headTop, overhanging the top tier by
* headOverhang on every side. Every section a rectangle with its corners chamfered by chamferShare of its width.
*/
struct BastionShape {
    Vec3 foot;
    float sink = 0.0f;
    float depthShare = 0.75f;
    float chamferShare = 0.06f;
    float glacisTop = 0.0f;
    float glacisWidth = 0.0f;
    std::vector<BastionTier> tiers;
    float headBottom = 0.0f;
    float headTop = 0.0f;
    float headOverhang = 0.0f;
    BastionFins fins;
    std::uint32_t seed = 0;
};

// The Bastion at the Monolith's place, its foot on the clearing's level (the concept sheet's numbers scaled by the settings).
BastionShape monolithBastion(float groundLevel, const ForestBastionSettings& settings);

// The far towers: one at each of the old spires' places in the haze, 300-800 m tall, 4-8 times as tall as wide, fins only.
std::vector<BastionShape> farTowers();

/*
* Where the fins stand across a face's flat span (centred on 0): an odd number, so one stands in the middle and they are
* symmetric, as near the pitch as fits a whole number (span / pitch rounded to odd), each in the middle of its share of
* the span, so none is cut in half at a corner. Major every majorEvery-th counted from the middle.
*/
struct FinPlace {
    float centre = 0.0f;
    bool major = false;
};

std::vector<FinPlace> finPlaces(float span, const BastionFins& fins);

// The tower's surface in its own frame: the glacis, the tiers, their fins, the head and its corbels; a facet number pair a face.
Mesh bastionMesh(const BastionShape& shape);

// The width of the tier at a height above the foot (0 above the head or below the first tier).
float bastionWidthAt(const BastionShape& shape, float height);

// An armoured bridge: its deck's middle line from one point to another (the deck's top), its width and its depth below it.
struct BridgeShape {
    Vec3 from;
    Vec3 to;
    float width = 0.0f;
    float depth = 0.0f;
    std::uint32_t seed = 0;
};

// The bridge from inside the Monolith's pillar (monolithPillar) into the Bastion's west side, its deck 331 m up.
BridgeShape monolithBridge(float groundLevel, const BastionShape& bastion);

// The bridge's surface in world coordinates: a box with its lower edges chamfered, and a parapet along each side of the deck.
Mesh bridgeMesh(const BridgeShape& bridge);

/*
* A row of slit windows round a tower (render/gpu/window_mask.h): its bottom and height above the foot (metres), the
* share of its windows lit, the share in the cool colour, and the slit's share of its cell across.
*/
struct WindowRow {
    float bottom = 0.0f;
    float height = 0.0f;
    float lit = 0.0f;
    float cool = 0.0f;
    float fill = 0.0f;
};

/*
* A tower's rows, bottom to top: in each tier, pairs of rows 3 m apart every windowPairSpacing from just above its
* bottom (the first pair over a setback the command level, lit more), and a visor band of three taller rows round the
* head, mostly lit. Openings stay a few percent of the wall: a fortress, not flats.
*/
inline constexpr float windowPairSpacing = 40.0f;

std::vector<WindowRow> bastionWindowRows(const BastionShape& shape);

/*
* The rows as the material's band strip (Material::windowBands): one texel tall, windowStripTexelsPerMetre texels a metre
* from the sunk foot to the head's top, linear and kept exact. Red: how much of the texel's height rows cover; green,
* blue, alpha: the nearest row's share lit (times litScale), share cool and fill, carried into the blank wall between rows
* too, so the strip's smaller mips average the rows' cover without diluting their shares.
*/
inline constexpr float windowStripTexelsPerMetre = 8.0f;

ImageData windowBandImage(const BastionShape& shape, const std::vector<WindowRow>& rows, float litScale);

// The light strips: a bar 0.2 m proud of the front of every major fin, inset 8 m from its tier's ends, in the tower's frame.
Mesh bastionStripsMesh(const BastionShape& shape);

// A floodlight in the tower's frame: where it stands and the way it shines.
struct FloodPlace {
    Vec3 position;
    Vec3 direction;
};

/*
* The floodlights washing the walls: at the glacis's foot on the ground and on each setback's ledge, two before the
* front and back of the wall above, one before each side, a few metres out, aimed up it and leaning a little towards it.
*/
std::vector<FloodPlace> bastionFloods(const BastionShape& shape);
