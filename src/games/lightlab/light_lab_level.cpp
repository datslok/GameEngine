#include "games/lightlab/light_lab_level.h"

#include "core/hash.h"
#include "math/angles.h"
#include "physics/overlap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
            const bool nearStreet = overlaps(crown.bounds(), grown(labStreetBounds, labStreetKeepClear));
            if (inViewKeepClear(x, z) || nearBuilding || nearStreet) {
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

    /*
    * The hashed numbers' uses, apart from the trees' (0 to 2), so a house's width never shares a number with anything
    * else's. Houses are numbered along each row, west to east, the north row's first; windows in the order they are made.
    */
    enum StreetHashUse : std::uint32_t {
        houseWidthUse = 10,
        houseStoreysUse,
        houseSurfaceUse,
        windowLitUse = 20,
        windowTintUse
    };

    /*
    * One row of houses from west to east along a front, the north row's (side -1) backs towards the start, cut by the
    * cross street. Each stretch is filled with hashed widths; a house that would leave less than the narrowest house
    * before the stretch's end takes the rest instead. Then each house's front gets a window per bay and storey.
    */
    void buildHouseRow(float side, std::uint32_t& houseNumber, LightLabLayout& layout) {
        const float front = labStreetZ + side * labHouseFrontOffset;
        const float back = front + side * labHouseDepth;
        const float stretches[2][2] = {{labStreetWestX, -labCrossStreetHalfWidth}, {labCrossStreetHalfWidth, labStreetEastX}};

        for (const auto& stretch : stretches) {
            float west = stretch[0];

            while (stretch[1] - west > 1e-3f) {
                const std::uint32_t house = houseNumber++;
                float width = labHouseMinWidth + (labHouseMaxWidth - labHouseMinWidth) * hashedFraction(house, houseWidthUse);
                if (stretch[1] - west - width < labHouseMinWidth) {
                    width = stretch[1] - west;
                }

                const int storeys = labHouseMinStoreys + static_cast<int>(hashedFraction(house, houseStoreysUse) *
                                                                         static_cast<float>(labHouseMaxStoreys - labHouseMinStoreys + 1));
                const float height = labStoreyHeight * static_cast<float>(storeys);
                const LabSurface surface = hashedFraction(house, houseSurfaceUse) < 0.5f ? LabSurface::Brick : LabSurface::Render;
                layout.street.push_back(spanning(Vec3{west, 0.0f, std::min(front, back)}, Vec3{west + width, height, std::max(front, back)}, surface));

                const int bays = std::max(1, static_cast<int>(width / labWindowBayWidth));
                for (int storey = 0; storey < storeys; ++storey) {
                    for (int bay = 0; bay < bays; ++bay) {
                        const std::uint32_t number = static_cast<std::uint32_t>(layout.windows.size());
                        const Vec3 centre{west + width * (static_cast<float>(bay) + 0.5f) / static_cast<float>(bays),
                                          labStoreyHeight * static_cast<float>(storey) + labWindowCentreAboveFloor, front};
                        layout.windows.push_back(LabWindow{LabBlock{centre, Vec3{labWindowHalfWidth, labWindowHalfHeight, labWindowHalfDepth}, LabSurface::Glass},
                                                           hashedFraction(number, windowLitUse) < labLitWindowShare, hashedFraction(number, windowTintUse)});
                    }
                }

                west += width;
            }
        }
    }

    /*
    * The lamps along one kerb (side -1 north, +1 south), the south side's half a spacing further east. Each is a post on
    * the pavement set back from the bulb towards the houses, an arm from the post's top over the bulb, and a shade over
    * the bulb, just clear of it, with the arm lying along the shade's top.
    */
    void buildLampRow(float side, LightLabLayout& layout) {
        const float bulbZ = labStreetZ + side * (labRoadHalfWidth - labStreetLampKerbInset);
        const float postZ = bulbZ + side * labStreetLampPostSetBack;
        const float shadeBottom = labStreetLampHeight + labStreetBulbHalfSize + 0.01f;
        const float shadeTop = shadeBottom + 2.0f * labStreetShadeHalfHeight;
        const float armHalf = labStreetLampPostHalfWidth;
        const float firstX = labStreetWestX + labStreetLampSpacing * (side < 0.0f ? 0.25f : 0.75f);

        for (int index = 0; index < labStreetLampsPerSide; ++index) {
            const float x = firstX + labStreetLampSpacing * static_cast<float>(index);
            layout.streetLamps.push_back(Vec3{x, labStreetLampHeight, bulbZ});
            layout.street.push_back(spanning(Vec3{x - armHalf, labKerbHeight, postZ - armHalf}, Vec3{x + armHalf, shadeTop + 2.0f * armHalf, postZ + armHalf},
                                             LabSurface::Metal));
            layout.street.push_back(spanning(Vec3{x - armHalf, shadeTop, std::min(bulbZ, postZ - side * armHalf)},
                                             Vec3{x + armHalf, shadeTop + 2.0f * armHalf, std::max(bulbZ, postZ - side * armHalf)}, LabSurface::Metal));
            layout.street.push_back(spanning(Vec3{x - labStreetShadeHalfWidth, shadeBottom, bulbZ - labStreetShadeHalfWidth},
                                             Vec3{x + labStreetShadeHalfWidth, shadeTop, bulbZ + labStreetShadeHalfWidth}, LabSurface::Metal));
        }
    }

    // The road and pavements, both rows of houses with their windows, and both rows of lamps.
    void buildStreet(LightLabLayout& layout) {
        layout.street.push_back(spanning(Vec3{labStreetWestX, 0.0f, labStreetZ - labRoadHalfWidth}, Vec3{labStreetEastX, labRoadTop, labStreetZ + labRoadHalfWidth},
                                         LabSurface::Asphalt));
        for (const float side : {-1.0f, 1.0f}) {
            const float kerb = labStreetZ + side * labRoadHalfWidth;
            const float front = labStreetZ + side * labHouseFrontOffset;
            layout.street.push_back(spanning(Vec3{labStreetWestX, 0.0f, std::min(kerb, front)}, Vec3{labStreetEastX, labKerbHeight, std::max(kerb, front)},
                                             LabSurface::Paving));
        }

        std::uint32_t houseNumber = 0;
        buildHouseRow(-1.0f, houseNumber, layout);
        buildHouseRow(1.0f, houseNumber, layout);
        buildLampRow(-1.0f, layout);
        buildLampRow(1.0f, layout);
    }
}

LightLabLayout buildLightLabLayout() {
    LightLabLayout layout{buildTowers(), buildFence(), buildTrees(), buildBuilding(), {}, {}, {}, {}};
    buildStreet(layout);

    layout.reflectors.push_back(spanning(labPolishedSlab.minimum, labPolishedSlab.maximum, LabSurface::Polished));
    layout.reflectors.push_back(spanning(labMirror.minimum, labMirror.maximum, LabSurface::Mirror));
    layout.reflectors.push_back(spanning(labSteelPanel.minimum, labSteelPanel.maximum, LabSurface::BrushedSteel));
    return layout;
}

/*
* Each wire family is the lines u + v = k / n (and u - v = k / n) for whole k, n diamonds a tile; a point's distance from
* the nearest is |n (u + v) - k| / (n sqrt 2). Whole tiles move n (u + v) by whole numbers, so the pattern repeats.
*/
float chainLinkAlpha(float u, float v) {
    const float diamonds = static_cast<float>(labChainLinkDiamonds);
    const auto distance = [diamonds](float across) {
        const float lines = across * diamonds;
        return std::abs(lines - std::round(lines)) / (diamonds * std::sqrt(2.0f));
    };

    const float nearest = std::min(distance(u + v), distance(u - v));
    const float texel = 1.0f / static_cast<float>(labChainLinkTexels);
    return std::clamp((labChainLinkWireHalfWidth - nearest) / texel + 0.5f, 0.0f, 1.0f);
}

ImageLevel chainLinkImage() {
    ImageLevel image{labChainLinkTexels, labChainLinkTexels, {}};
    image.pixels.reserve(std::size_t{labChainLinkTexels} * labChainLinkTexels * 4);

    for (std::uint32_t y = 0; y < labChainLinkTexels; ++y) {
        for (std::uint32_t x = 0; x < labChainLinkTexels; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(labChainLinkTexels);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(labChainLinkTexels);
            const auto alpha = static_cast<std::uint8_t>(std::lround(chainLinkAlpha(u, v) * 255.0f));
            image.pixels.insert(image.pixels.end(), {180, 182, 185, alpha});
        }
    }

    return image;
}

/*
* Seen from the east (looking west, up is +y), north (-z) is to the right, so the corners run anticlockwise from the
* bottom south corner: the front faces east. It is double-sided, so the west sees it too.
*/
Mesh chainLinkPanel() {
    Mesh panel;
    panel.vertices = {
        Vec4{labFenceX, 0.0f, labFirstPostZ, 1.0f},
        Vec4{labFenceX, 0.0f, labLastPostZ, 1.0f},
        Vec4{labFenceX, labPostHeight, labLastPostZ, 1.0f},
        Vec4{labFenceX, labPostHeight, labFirstPostZ, 1.0f}
    };

    const auto uvOf = [](const Vec4& corner) {
        return Vec2{(labFirstPostZ - corner.z) / labChainLinkTileMetres, corner.y / labChainLinkTileMetres};
    };

    for (const std::array<std::size_t, 3>& corners : {std::array<std::size_t, 3>{0, 1, 2}, std::array<std::size_t, 3>{0, 2, 3}}) {
        Triangle triangle{};
        triangle.first = corners[0];
        triangle.second = corners[1];
        triangle.third = corners[2];

        for (std::size_t corner = 0; corner < 3; ++corner) {
            triangle.normals[corner] = Vec3{1.0f, 0.0f, 0.0f};
            triangle.uvs[corner] = uvOf(panel.vertices[corners[corner]]);
        }

        panel.triangles.push_back(triangle);
    }

    return panel;
}
