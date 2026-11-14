#include "games/forest/drifting_rocks.h"

#include "games/forest/forest_terrain.h"
#include "math/angles.h"
#include "math/quat.h"

#include <algorithm>
#include <cmath>

namespace {
    // Darts thrown for each rock wanted before giving up on it.
    constexpr int triesPerRock = 30;

    // The rings: out from an island's centre and below its rim (shares of its radius); out from a pillar's foot and up it.
    constexpr float islandRingInner = 1.35f;
    constexpr float islandRingWidth = 0.6f;
    constexpr float islandRingDepth = 0.6f;
    constexpr float pillarRingInner = 1.7f;
    constexpr float pillarRingWidth = 0.6f;
    constexpr float pillarLowest = 0.45f;
    constexpr float pillarHighestExtra = 0.45f;
    constexpr int mostPillarRocks = 3;

    // Clear of other shapes: of a pillar, below a little over its top; of an island, from under its tip to over its rim.
    constexpr float pillarClear = 1.6f;
    constexpr float islandClear = 1.3f;
    constexpr float islandTipDepth = 1.5f;
    constexpr float islandDomeClear = 0.2f;

    // Turning: up to this many radians a second either way, at least a fifth of it.
    constexpr float fastestTurn = 0.04f;

    const Vec3 up{0.0f, 1.0f, 0.0f};

    // A rock at a place, its size and motion from its own random numbers.
    DriftingRock makeRock(const Vec3& rest, float size, const ForestDriftSettings& settings, std::uint32_t seed) {
        const float spin = fastestTurn * (0.2f + 0.8f * forestUnitRandom(seed, 3));
        return DriftingRock{.rest = rest,
                            .size = size,
                            .bobMetres = settings.bobMetres,
                            .bobSeconds = settings.bobSeconds * (0.5f + forestUnitRandom(seed, 0)),
                            .phase = fullTurnRadians * forestUnitRandom(seed, 1),
                            .turnRadians = fullTurnRadians * forestUnitRandom(seed, 2),
                            .turnSpeed = forestUnitRandom(seed, 4) < 0.5f ? -spin : spin,
                            .seed = seed};
    }

    bool clearOf(const DriftingRock& rock, const std::vector<DriftingRock>& kept) {
        return std::all_of(kept.begin(), kept.end(), [&](const DriftingRock& other) {
            return (rock.rest - other.rest).length() >= rock.size + other.size + rock.bobMetres + other.bobMetres;
        });
    }

    float levelDistance(const Vec3& a, const Vec3& b) {
        return Vec2{a.x - b.x, a.z - b.z}.length();
    }

    /*
    * Rocks round a centre by dart throwing: place(dart) gives a candidate's place and size from its random numbers (a
    * dart's own seed); kept only clear of those kept.
    */
    template <typename Place>
    std::vector<DriftingRock> throwRocks(int count, std::uint32_t seed, const ForestDriftSettings& settings, Place place) {
        std::vector<DriftingRock> rocks;

        for (int dart = 0; dart < count * triesPerRock && static_cast<int>(rocks.size()) < count; ++dart) {
            const std::uint32_t dartSeed = forestShapeSeed(seed, static_cast<std::uint32_t>(dart) + 1);
            const float size = settings.largestRock * (0.1f + 0.9f * std::pow(forestUnitRandom(dartSeed, 10), 2.0f));
            const DriftingRock rock = makeRock(place(dartSeed, size), size, settings, dartSeed);

            if (clearOf(rock, rocks)) {
                rocks.push_back(rock);
            }
        }

        return rocks;
    }
}

Transform driftingRockPose(const DriftingRock& rock, double seconds) {
    // In doubles and wrapped, so a long run's time keeps its precision.
    const double bobs = seconds / static_cast<double>(rock.bobSeconds);
    const float bobAngle = static_cast<float>(static_cast<double>(fullTurnRadians) * (bobs - std::floor(bobs))) + rock.phase;
    const double turned = static_cast<double>(rock.turnRadians) + static_cast<double>(rock.turnSpeed) * seconds;
    const float turn = wrapAngle(static_cast<float>(std::fmod(turned, static_cast<double>(fullTurnRadians))));
    return Transform{.position = rock.rest + up * (rock.bobMetres * std::sin(bobAngle)), .rotation = Quat::fromAxisAngle(up, turn),
                     .scale = Vec3{1.0f, 1.0f, 1.0f}};
}

void updateDriftingRocks(World& world, double simulationSeconds, bool moving) {
    const double seconds = moving ? simulationSeconds : 0.0;
    world.each<DriftingRock, Transform>([seconds](Entity, const DriftingRock& rock, Transform& transform) {
        transform = driftingRockPose(rock, seconds);
    });
}

void visit(ForestDriftSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Rocks round islands per 10 m", settings.islandRocksPerTenMetres, {.min = 0.0f, .max = 10.0f, .sliderMax = 4.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Pillars with rocks", settings.pillarShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Largest rock", settings.largestRock, {.min = 1.0f, .max = 100.0f, .sliderMax = 40.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Bob", settings.bobMetres, {.min = 0.0f, .max = 50.0f, .sliderMax = 10.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Bob time", settings.bobSeconds, {.min = 1.0f, .max = 600.0f, .sliderMax = 120.0f, .unit = "s", .applies = FieldApplies::AtStartup});
    visitor.field("Rocks drift", settings.moving);
}

std::vector<DriftingRock> islandRocks(const IslandPlacement& island, const ForestDriftSettings& settings) {
    const int count = std::max(0, static_cast<int>(std::lround(settings.islandRocksPerTenMetres * island.radius / 10.0f)));
    return throwRocks(count, forestShapeSeed(island.seed, 40), settings, [&](std::uint32_t seed, float size) {
        const float angle = fullTurnRadians * forestUnitRandom(seed, 11);
        const float out = island.radius * (islandRingInner + islandRingWidth * forestUnitRandom(seed, 12)) + size;
        const float below = islandRingDepth * island.radius * forestUnitRandom(seed, 13);
        return island.rim + Vec3{out * std::sin(angle), -below, out * std::cos(angle)};
    });
}

std::vector<DriftingRock> pillarRocks(const PillarPlacement& pillar, const ForestDriftSettings& settings) {
    const std::uint32_t seed = forestShapeSeed(pillar.seed, 41);

    if (forestUnitRandom(seed, 0) >= settings.pillarShare) {
        return {};
    }

    const int count = 1 + std::min(static_cast<int>(forestUnitRandom(seed, 1) * mostPillarRocks), mostPillarRocks - 1);
    return throwRocks(count, seed, settings, [&](std::uint32_t dartSeed, float size) {
        const float angle = fullTurnRadians * forestUnitRandom(dartSeed, 11);
        const float out = pillar.radius * (pillarRingInner + pillarRingWidth * forestUnitRandom(dartSeed, 12)) + size;
        const float rise = pillar.height * (pillarLowest + pillarHighestExtra * forestUnitRandom(dartSeed, 13));
        return pillar.foot + Vec3{out * std::sin(angle), rise, out * std::cos(angle)};
    });
}

std::vector<DriftingRock> valleyRocks(const std::vector<PillarPlacement>& pillars, const std::vector<IslandPlacement>& islands,
                                      const ForestDriftSettings& settings) {
    std::vector<DriftingRock> candidates;

    for (const IslandPlacement& island : islands) {
        const std::vector<DriftingRock> rocks = islandRocks(island, settings);
        candidates.insert(candidates.end(), rocks.begin(), rocks.end());
    }

    for (const PillarPlacement& pillar : pillars) {
        const std::vector<DriftingRock> rocks = pillarRocks(pillar, settings);
        candidates.insert(candidates.end(), rocks.begin(), rocks.end());
    }

    std::vector<DriftingRock> kept;

    for (const DriftingRock& rock : candidates) {
        const float reach = rock.size + rock.bobMetres;
        const bool touchesPillar = std::any_of(pillars.begin(), pillars.end(), [&](const PillarPlacement& pillar) {
            return levelDistance(rock.rest, pillar.foot) < pillarClear * pillar.radius + rock.size && rock.rest.y - reach < pillar.foot.y + pillar.height;
        });
        const bool touchesIsland = std::any_of(islands.begin(), islands.end(), [&](const IslandPlacement& island) {
            return levelDistance(rock.rest, island.rim) < islandClear * island.radius + rock.size &&
                   rock.rest.y + reach > island.rim.y - islandTipDepth * island.radius && rock.rest.y - reach < island.rim.y + islandDomeClear * island.radius;
        });

        if (!touchesPillar && !touchesIsland && clearOf(rock, kept)) {
            kept.push_back(rock);
        }
    }

    return kept;
}

SurfaceGrid driftingRockSurface(const DriftingRock& rock, std::uint32_t seed) {
    const RockCarving uncarved{.bands = {StrataBand{.bottom = -1.0e5f, .top = 1.0e5f}}, .warp = {}, .warpTileMetres = 1500.0f, .warpMetres = 0.0f,
                               .jointMetres = 0.0f};
    return islandSurface(IslandPlacement{.rim = Vec3{0.0f, 0.0f, 0.0f}, .radius = rock.size, .turnRadians = 0.0f, .seed = seed}, uncarved);
}
