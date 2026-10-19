#pragma once

#include "math/aabb.h"
#include "math/vec3.h"
#include "scene/probe_volume.h"

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
    RedPaint  // the panel lining the sunlit room's east wall
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

/*
* Light probes for global illumination round the building, 1 m apart, on half-metre marks so that every wall, the floor
* and the roof fall between two layers of probes (none is buried in a wall, until probes can move themselves out), and
* each room holds full cells of 8. Three layers reach out beyond the walls and two above the roof, so the outside of
* the building is lit too: 12 x 5 x 16 probes.
*/
inline const ProbeVolume labProbeVolume{Vec3{-25.5f, 0.5f, 2.5f}, Vec3{1.0f, 1.0f, 1.0f}, ProbeCounts{12, 5, 16}};

/*
* A test of finer bounce light (switched on in the settings, LightLabProbeSettings): probes 20 cm apart inside the
* building only, from the back room's floor and west wall to the sunlit room's far corner, centred so the outermost
* probes sit about half a spacing in from the walls (where the volume's full weight still reaches). 27 x 15 x 47 =
* 19,035 probes: the whole field at this spacing would need 89,000, past the 65,535 one volume may hold. With the walls
* 30 cm thick, some layers fall inside walls, the floor or the ceiling, until probes can move themselves out; their back
* faces keep them out of the light they cannot see.
*/
inline constexpr float labDenseProbeSpacing = 0.2f;
inline const ProbeVolume labDenseProbeVolume = [] {
    const Vec3 low{labBackRoom.minimum.x, labBackRoom.minimum.y, labBackRoom.minimum.z};
    const Vec3 high{labBackRoom.maximum.x, labBackRoom.maximum.y, labSunlitRoom.maximum.z};
    const auto along = [](float from, float to, int& count) {
        count = static_cast<int>(std::lround((to - from) / labDenseProbeSpacing));
        const float span = labDenseProbeSpacing * static_cast<float>(count - 1);
        return from + 0.5f * ((to - from) - span);
    };

    ProbeVolume volume;
    volume.spacing = Vec3{labDenseProbeSpacing, labDenseProbeSpacing, labDenseProbeSpacing};
    volume.origin = Vec3{along(low.x, high.x, volume.counts.x), along(low.y, high.y, volume.counts.y), along(low.z, high.z, volume.counts.z)};
    return volume;
}();

// The level's parts, each group drawn as one entity and all of it solid.
struct LightLabLayout {
    std::vector<LabBlock> towers;
    std::vector<LabBlock> fence;
    std::vector<LabBlock> trees; // a trunk then its crown, for each tree
    std::vector<LabBlock> building;
};

LightLabLayout buildLightLabLayout();
