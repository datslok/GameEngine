#pragma once

#include "math/aabb.h"
#include "math/vec3.h"

#include <array>
#include <cmath>
#include <vector>

/*
* The light lab's level as plain boxes, so tests can check its layout without loading anything. Coordinates: +x east,
* +y up, -z north, metres; the player starts at the origin looking north. Everything is a stretched cube, coloured by
* what it is made of.
*/
enum class LabSurface {
    Stone,    // towers
    Wood,     // the fence
    Bark,     // tree trunks
    Leaves,   // tree crowns
    Plaster,  // the building's walls and roof
    Floorboards, // the building's floor
    RedPaint, // the panel lining the sunlit room's east wall
    Asphalt,  // the street's road
    Paving,   // its pavements
    Brick,    // houses
    Render,   // houses with smooth rendered fronts
    Metal,    // lamp posts, arms and shades
    Glass,    // windows (dark until lit)
    Polished, // the reflection corner's slab: dark polished stone
    Chrome,   // the mirror ball
    Mirror,   // the sunlit room's mirror: silvered glass
    BrushedSteel // the panel beside it: a rough mirror
};

struct LabBlock {
    Vec3 centre;
    Vec3 halfSize;
    LabSurface surface = LabSurface::Stone;

    Aabb bounds() const;
};

// The ground: a flat square this far from the origin each way, its top at y = 0.
inline constexpr float labGroundHalfSize = 400.0f;

/*
* Towers at doubling distances from the origin, each as wide and tall in proportion to its distance, so all look the
* same size from the start, fanned out so none hides another. Cascaded shadow maps also double in size from one to the
* next, so each tower's shadow should look about as sharp on screen as the one before.
*/
inline constexpr std::array<float, 7> labTowerDistances{5.0f, 10.0f, 20.0f, 40.0f, 80.0f, 160.0f, 320.0f};
inline constexpr float labTowerWidthPerMetre = 0.1f;   // its square side, per metre of distance
inline constexpr float labTowerHeightPerMetre = 0.4f;
inline constexpr float labFirstTowerBearingDegrees = -27.0f; // from north, positive to the east
inline constexpr float labTowerBearingStepDegrees = 9.0f;

/*
* A fence running north from near the start to the far distance, west of the towers: thin posts and one thin rail, the
* hardest shadows to keep, since a caster narrower than a shadow texel flickers or vanishes.
*/
inline constexpr float labFenceX = -6.0f;
inline constexpr float labFirstPostZ = -4.0f;
inline constexpr float labLastPostZ = -300.0f;
inline constexpr float labPostSpacing = 4.0f;
inline constexpr float labPostHalfWidth = 0.05f;
inline constexpr float labPostHeight = 2.0f;
inline constexpr float labRailHeight = 1.0f;     // its centre
inline constexpr float labRailHalfWidth = 0.03f;

/*
* A forest of box trees (a trunk and a crown) scattered round the start at between these distances, placed by hashing
* each tree's number, so the level is the same on every run. Trees keep out of the towers' and fence's view to the
* north, and clear of the building.
*/
inline constexpr int labTreeCount = 400;
inline constexpr float labForestInnerRadius = 40.0f;
inline constexpr float labForestOuterRadius = 380.0f;
inline constexpr float labTrunkHalfWidth = 0.15f;
inline constexpr float labTrunkHeight = 3.0f;
inline constexpr float labCrownHalfSize = 1.25f;
inline constexpr float labViewKeepClearHalfAngleDegrees = 34.0f; // either side of north, for the towers and the fence
inline constexpr float labViewKeepClearMargin = 15.0f;           // and this far either side of that wedge
inline constexpr float labBuildingKeepClear = 15.0f;             // around the building's walls

/*
* A two-room building south-west of the start with a flat roof. The sunlit room (south) has one window in its west wall,
* where the afternoon sun comes from, and a red panel on its east wall; a doorway in the wall between them leads to the
* back room (north), whose only other opening is the front door in its east wall. Inside, the rooms are lit only by what
* comes through those openings, which today is just the sunbeam: bounce light (DDGI) is what fills the rest.
* The boxes below are the rooms' insides (floor to ceiling, the sunlit room's inside the red panel), the window and
* doorways their openings through the walls.
*/
inline constexpr float labWallThickness = 0.3f;
inline constexpr float labFloorTop = 0.02f; // a floorboard slab under the whole building, over the grass
inline constexpr float labRoomHeight = 3.0f;
inline constexpr float labRedPanelThickness = 0.05f;
inline const Aabb labSunlitRoom{Vec3{-22.7f, labFloorTop, 10.15f}, Vec3{-17.3f - labRedPanelThickness, labRoomHeight, 14.7f}};
inline const Aabb labBackRoom{Vec3{-22.7f, labFloorTop, 5.3f}, Vec3{-17.3f, labRoomHeight, 9.85f}};
inline const Aabb labWindow{Vec3{-23.0f, 1.0f, 11.75f}, Vec3{-22.7f, 2.2f, 13.25f}};
inline const Aabb labInnerDoorway{Vec3{-20.5f, labFloorTop, 9.85f}, Vec3{-19.5f, 2.1f, 10.15f}};
inline const Aabb labFrontDoor{Vec3{-17.3f, labFloorTop, 7.0f}, Vec3{-17.0f, 2.1f, 8.2f}};

// The back room's ceiling lamp: its bulb under the middle of the ceiling, and the glowing panel above it, touching the
// ceiling (half-sizes).
inline const Vec3 labLampPosition{-20.0f, labRoomHeight - 0.15f, 7.575f};
inline const Vec3 labLampFixtureHalfSize{0.15f, 0.025f, 0.15f};

/*
* A street running east to west south of the start, lined with houses and lit at night by 60 lamps and the houses' lit
* windows: the test ground for many lights. A 10 m road between 3 m pavements, raised a kerb's height, then rows of
* houses (6 to 12 m wide, 3 or 4 storeys, 10 m deep, each a solid block), cut by a cross street where it meets the line
* north from the start, so the start can walk in. The lamps stand at each pavement's kerb every 10 m, the two sides
* staggered by half that, so a point on the road is lit by two to four: a pole set back towards the houses, an arm over
* the bulb, and a shade above it that keeps the light off the sky and the upper storeys, as a real lamp's housing does.
* Each house front has a window per bay and storey, a share of them lit, each a little warmer or cooler.
*/
inline constexpr float labStreetZ = 45.0f;              // the road's centre line
inline constexpr float labStreetWestX = -150.0f;
inline constexpr float labStreetEastX = 150.0f;
inline constexpr float labRoadHalfWidth = 5.0f;
inline constexpr float labPavementWidth = 3.0f;
inline constexpr float labKerbHeight = 0.15f;
inline constexpr float labRoadTop = 0.02f;              // over the grass, as the building's floor
inline constexpr float labCrossStreetHalfWidth = 5.0f;  // round x = 0
inline constexpr float labHouseDepth = 10.0f;
inline constexpr float labHouseMinWidth = 6.0f;
inline constexpr float labHouseMaxWidth = 12.0f;
inline constexpr float labStoreyHeight = 3.0f;
inline constexpr int labHouseMinStoreys = 3;
inline constexpr int labHouseMaxStoreys = 4;
inline constexpr float labWindowBayWidth = 3.0f;        // at most one window per this much front
inline constexpr float labWindowHalfWidth = 0.6f;
inline constexpr float labWindowHalfHeight = 0.75f;
inline constexpr float labWindowCentreAboveFloor = 1.65f;
inline constexpr float labWindowHalfDepth = 0.03f;      // a pane centred on the front, standing this far out of it
inline constexpr float labLitWindowShare = 0.4f;
inline constexpr int labStreetLampsPerSide = 30;
inline constexpr float labStreetLampSpacing = 10.0f;
inline constexpr float labStreetLampKerbInset = 0.5f;   // the bulb over the road, this far in from the kerb
inline constexpr float labStreetLampHeight = 6.0f;      // the bulb's centre
inline constexpr float labStreetLampPostSetBack = 0.6f; // the post behind the bulb, towards the houses
inline constexpr float labStreetLampPostHalfWidth = 0.07f;
inline constexpr float labStreetBulbHalfSize = 0.12f;
inline constexpr float labStreetShadeHalfWidth = 0.4f;
inline constexpr float labStreetShadeHalfHeight = 0.04f;
inline constexpr float labStreetKeepClear = 10.0f;      // trees keep this far from the street's houses

// The house fronts' distance from the road's centre line, either side.
inline constexpr float labHouseFrontOffset = labRoadHalfWidth + labPavementWidth;

// The street from the far side of one row of houses to the other's, from the ground to the tallest roof.
inline const Aabb labStreetBounds{Vec3{labStreetWestX, 0.0f, labStreetZ - labHouseFrontOffset - labHouseDepth},
                                  Vec3{labStreetEastX, labStoreyHeight * static_cast<float>(labHouseMaxStoreys), labStreetZ + labHouseFrontOffset + labHouseDepth}};

/*
* A window pane on a house front, whether it is lit at night, and its light's tint, from 0 (the warmest) to 1 (the
* coolest), picked by hashing the window's number.
*/
struct LabWindow {
    LabBlock pane;
    bool lit = false;
    float tint = 0.0f;
};

/*
* The reflection corner north-east of the start, the test ground for reflections (render/gpu/screen_reflections.h): a
* slab of dark polished stone on the grass, smooth enough to mirror the towers and the sky, and a chrome ball resting on
* it, which mirrors everything round it.
*/
inline const Aabb labPolishedSlab{Vec3{2.0f, 0.0f, -11.0f}, Vec3{8.0f, 0.04f, -5.0f}};
inline constexpr float labMirrorBallRadius = 0.6f;
inline const Vec3 labMirrorBallCentre{5.0f, 0.04f + labMirrorBallRadius, -8.5f};

// A tall mirror on the sunlit room's south wall, facing the doorway from the back room across the room, so the
// doorway and the room are seen in it (2 cm thick, standing out from the wall).
inline constexpr float labMirrorThickness = 0.02f;
inline const Aabb labMirror{Vec3{-20.7f, 0.6f, 14.7f - labMirrorThickness}, Vec3{-19.3f, 2.4f, 14.7f}};

// A brushed steel panel on the same wall, east of the mirror and as tall: a rough mirror beside a sharp one, so a
// flashlight swept across both shows the reflected spot's edges softening (render/gpu/mirrored_light.h, footprints).
inline const Aabb labSteelPanel{Vec3{-19.0f, 0.6f, 14.7f - labMirrorThickness}, Vec3{-17.75f, 2.4f, 14.7f}};

// The level's parts, each group drawn as one entity and all of it solid.
struct LightLabLayout {
    std::vector<LabBlock> towers;
    std::vector<LabBlock> fence;
    std::vector<LabBlock> trees; // a trunk then its crown, for each tree
    std::vector<LabBlock> building;
    std::vector<LabBlock> street; // the road, pavements, houses and lamps' posts, arms and shades
    std::vector<LabWindow> windows;
    std::vector<Vec3> streetLamps; // each lamp's bulb: the north side's west to east, then the south side's
    std::vector<LabBlock> reflectors; // the polished slab, the mirror, then the steel panel (the mirror ball is a sphere, placed by the game)
};

LightLabLayout buildLightLabLayout();
