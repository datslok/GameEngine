#include "games/lightlab/light_lab_level.h"

#include "core/hash.h"
#include "math/angles.h"
#include "physics/overlap.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>

Aabb LabBlock::bounds() const {
    return Aabb{centre - halfSize, centre + halfSize};
}

namespace {
    // A block filling the box from minimum to maximum.
    LabBlock spanning(const Vec3& minimum, const Vec3& maximum, LabSurface surface) {
        return LabBlock{(minimum + maximum) * 0.5f, (maximum - minimum) * 0.5f, surface};
    }

    Aabb grown(const Aabb& box, float margin) {
        return Aabb{box.minimum - Vec3{margin, margin, margin}, box.maximum + Vec3{margin, margin, margin}};
    }

    std::vector<LabBlock> buildTowers() {
        std::vector<LabBlock> towers;

        for (std::size_t index = 0; index < labTowerDistances.size(); ++index) {
            const float distance = labTowerDistances[index];
            const float bearing = radiansFromDegrees(labFirstTowerBearingDegrees + labTowerBearingStepDegrees * static_cast<float>(index));
            const float halfWidth = distance * labTowerWidthPerMetre * 0.5f;
            const float halfHeight = distance * labTowerHeightPerMetre * 0.5f;
            towers.push_back(LabBlock{Vec3{distance * std::sin(bearing), halfHeight, -distance * std::cos(bearing)},
                                      Vec3{halfWidth, halfHeight, halfWidth}, LabSurface::Stone});
        }

        return towers;
    }

    // The posts from the first to the last, then the rail along them.
    std::vector<LabBlock> buildFence() {
        std::vector<LabBlock> fence;
        const int posts = static_cast<int>(std::lround((labFirstPostZ - labLastPostZ) / labPostSpacing)) + 1;

        for (int index = 0; index < posts; ++index) {
            const float z = labFirstPostZ - labPostSpacing * static_cast<float>(index);
            fence.push_back(LabBlock{Vec3{labFenceX, labPostHeight * 0.5f, z}, Vec3{labPostHalfWidth, labPostHeight * 0.5f, labPostHalfWidth},
                                     LabSurface::Wood});
        }

        fence.push_back(LabBlock{Vec3{labFenceX, labRailHeight, (labFirstPostZ + labLastPostZ) * 0.5f},
                                 Vec3{labRailHalfWidth, labRailHalfWidth, (labFirstPostZ - labLastPostZ) * 0.5f}, LabSurface::Wood});
        return fence;
    }

    // A number from 0 up to (not including) 1, the same for the same attempt and use every time.
    float hashedFraction(std::uint32_t attempt, std::uint32_t use) {
        Fnv1aHash hash;
        hash.add(attempt);
        hash.add(use);
        return static_cast<float>(mixBits(hash.value) >> 40) / static_cast<float>(1ull << 24);
    }

    bool inViewKeepClear(float x, float z) {
        const float slope = std::tan(radiansFromDegrees(labViewKeepClearHalfAngleDegrees));
        return z < 0.0f && std::abs(x) < slope * -z + labViewKeepClearMargin;
    }

    /*
    * Trees at random places spread evenly over the ring's area (the radius as the square root of an even fraction of the
    * ring's squared radii, as a uniform disc needs), each a little taller or shorter, skipping places in the keep-clear
    * areas, until there are enough. Each candidate's numbers come from hashing its attempt number, never from a
    * random generator's running state, so the forest cannot change with the order anything else draws numbers.
    */
    std::vector<LabBlock> buildTrees() {
        std::vector<LabBlock> trees;
        const float inner = labForestInnerRadius * labForestInnerRadius;
        const float outer = labForestOuterRadius * labForestOuterRadius;
        const std::uint32_t attemptLimit = static_cast<std::uint32_t>(labTreeCount) * 100;

        for (std::uint32_t attempt = 0; static_cast<int>(trees.size()) < labTreeCount * 2; ++attempt) {
            if (attempt == attemptLimit) {
                throw std::logic_error("The light lab's forest does not fit outside its keep-clear areas");
            }

            const float radius = std::sqrt(inner + hashedFraction(attempt, 0) * (outer - inner));
            const float angle = fullTurnRadians * hashedFraction(attempt, 1);
            const float size = 0.8f + 0.5f * hashedFraction(attempt, 2);
            const float x = radius * std::sin(angle);
            const float z = -radius * std::cos(angle);

            const float trunkHeight = labTrunkHeight * size;
            const float crownHalf = labCrownHalfSize * size;
            const LabBlock trunk{Vec3{x, trunkHeight * 0.5f, z}, Vec3{labTrunkHalfWidth * size, trunkHeight * 0.5f, labTrunkHalfWidth * size},
                                 LabSurface::Bark};
            const LabBlock crown{Vec3{x, trunkHeight + crownHalf * 0.6f, z}, Vec3{crownHalf, crownHalf, crownHalf}, LabSurface::Leaves};

            const bool nearBuilding = overlaps(crown.bounds(), grown(labSunlitRoom, labBuildingKeepClear)) ||
                                      overlaps(crown.bounds(), grown(labBackRoom, labBuildingKeepClear));
            if (inViewKeepClear(x, z) || nearBuilding) {
                continue;
            }

            trees.push_back(trunk);
            trees.push_back(crown);
        }

        return trees;
    }

    /*
    * The walls round the two rooms and between them, cut around the window and doorways, the red panel and the roof,
    * all measured from the rooms' insides and the openings, so the boxes and the empty spaces cannot disagree.
    */
    std::vector<LabBlock> buildBuilding() {
        std::vector<LabBlock> building;
        const float wall = labWallThickness;
        const float top = labRoomHeight;
        const float westInside = labBackRoom.minimum.x;
        const float eastInside = labBackRoom.maximum.x;
        const float west = westInside - wall;
        const float east = eastInside + wall;
        const float north = labBackRoom.minimum.z - wall;
        const float south = labSunlitRoom.maximum.z + wall;
        const auto add = [&building](const Vec3& minimum, const Vec3& maximum, LabSurface surface = LabSurface::Plaster) {
            building.push_back(spanning(minimum, maximum, surface));
        };

        // West wall, round the window.
        add(Vec3{west, 0.0f, north}, Vec3{westInside, top, labWindow.minimum.z});
        add(Vec3{west, 0.0f, labWindow.maximum.z}, Vec3{westInside, top, south});
        add(Vec3{west, 0.0f, labWindow.minimum.z}, Vec3{westInside, labWindow.minimum.y, labWindow.maximum.z});
        add(Vec3{west, labWindow.maximum.y, labWindow.minimum.z}, Vec3{westInside, top, labWindow.maximum.z});

        // East wall, round the front door.
        add(Vec3{eastInside, 0.0f, north}, Vec3{east, top, labFrontDoor.minimum.z});
        add(Vec3{eastInside, 0.0f, labFrontDoor.maximum.z}, Vec3{east, top, south});
        add(Vec3{eastInside, labFrontDoor.maximum.y, labFrontDoor.minimum.z}, Vec3{east, top, labFrontDoor.maximum.z});

        // North and south walls between them.
        add(Vec3{westInside, 0.0f, north}, Vec3{eastInside, top, labBackRoom.minimum.z});
        add(Vec3{westInside, 0.0f, labSunlitRoom.maximum.z}, Vec3{eastInside, top, south});

        // The wall between the rooms, round its doorway.
        const float partitionNorth = labBackRoom.maximum.z;
        const float partitionSouth = labSunlitRoom.minimum.z;
        add(Vec3{westInside, 0.0f, partitionNorth}, Vec3{labInnerDoorway.minimum.x, top, partitionSouth});
        add(Vec3{labInnerDoorway.maximum.x, 0.0f, partitionNorth}, Vec3{eastInside, top, partitionSouth});
        add(Vec3{labInnerDoorway.minimum.x, labInnerDoorway.maximum.y, partitionNorth}, Vec3{labInnerDoorway.maximum.x, top, partitionSouth});

        // The red panel on the sunlit room's east wall, the floor under everything (its top above the grass, so the
        // two never fight over the same depth), and the roof over everything.
        add(Vec3{labSunlitRoom.maximum.x, labFloorTop, labSunlitRoom.minimum.z}, Vec3{eastInside, top, labSunlitRoom.maximum.z}, LabSurface::RedPaint);
        add(Vec3{west, 0.0f, north}, Vec3{east, labFloorTop, south}, LabSurface::Floorboards);
        add(Vec3{west, top, north}, Vec3{east, top + wall, south});
        return building;
    }
}

LightLabLayout buildLightLabLayout() {
    return LightLabLayout{buildTowers(), buildFence(), buildTrees(), buildBuilding()};
}
