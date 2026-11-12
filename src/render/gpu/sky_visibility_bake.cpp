#include "render/gpu/sky_visibility_bake.h"

#include "core/profiler.h"
#include "render/gpu/ddgi.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <utility>
#include <vector>

namespace {

    // A triangle's outward normal, from its winding (counterclockwise seen from the front), not of unit length.
    Vec3 triangleFacing(const StaticLevel& level, std::uint32_t triangle) {
        const auto corners = level.getTriangle(triangle);
        return (corners[1] - corners[0]).cross(corners[2] - corners[0]);
    }

    // A ray's hit on the front of a triangle, kept to be lit once the point's own rays are all cast.
    struct FrontHit {
        std::size_t ray = 0;
        Vec3 point;
        Vec3 normal;
        std::uint32_t triangle = 0;
    };

    // The cube's texels a side for looking up a point's nearest ray by direction (each about 5.6 degrees, finer than
    // 256 rays' spacing of about 13).
    constexpr int rayTableSide = 16;

    // A direction's texel on the cube: its largest axis picks the face (+x, -x, +y, -y, +z, -z), the other two its place.
    int cubeTexel(const Vec3& direction) {
        const float ax = std::abs(direction.x);
        const float ay = std::abs(direction.y);
        const float az = std::abs(direction.z);
        int face = 0;
        float u = 0.0f;
        float v = 0.0f;

        if (ax >= ay && ax >= az) {
            face = direction.x >= 0.0f ? 0 : 1;
            u = direction.y / ax;
            v = direction.z / ax;
        } else if (ay >= az) {
            face = direction.y >= 0.0f ? 2 : 3;
            u = direction.x / ay;
            v = direction.z / ay;
        } else {
            face = direction.z >= 0.0f ? 4 : 5;
            u = direction.x / az;
            v = direction.y / az;
        }

        const auto place = [](float along) {
            return std::clamp(static_cast<int>((along + 1.0f) * 0.5f * static_cast<float>(rayTableSide)), 0, rayTableSide - 1);
        };
        return (face * rayTableSide + place(u)) * rayTableSide + place(v);
    }

    // A texel's middle direction (not of unit length), the inverse of cubeTexel.
    Vec3 cubeTexelDirection(int texel) {
        const int face = texel / (rayTableSide * rayTableSide);
        const float u = (static_cast<float>(texel / rayTableSide % rayTableSide) + 0.5f) / static_cast<float>(rayTableSide) * 2.0f - 1.0f;
        const float v = (static_cast<float>(texel % rayTableSide) + 0.5f) / static_cast<float>(rayTableSide) * 2.0f - 1.0f;
        const float sign = face % 2 == 0 ? 1.0f : -1.0f;

        switch (face / 2) {
        case 0:
            return Vec3{sign, u, v};
        case 1:
            return Vec3{u, sign, v};
        default:
            return Vec3{u, v, sign};
        }
    }

    /*
    * For each cube texel, the nearest of rays rays (skyVisibilityRay), worked out once per ray count and kept for the
    * program's life (a few counts at most); each thread keeps the last one it used, so the lock is taken rarely.
    */
    const std::vector<int>& nearestRayTable(int rays) {
        thread_local int cachedRays = -1;
        thread_local const std::vector<int>* cached = nullptr;

        if (rays == cachedRays) {
            return *cached;
        }

        static std::mutex lock;
        static std::map<int, std::vector<int>> tables;
        const std::lock_guard guard(lock);
        std::vector<int>& table = tables[rays];

        if (table.empty()) {
            table.resize(static_cast<std::size_t>(6 * rayTableSide * rayTableSide));

            for (std::size_t texel = 0; texel < table.size(); ++texel) {
                const Vec3 direction = cubeTexelDirection(static_cast<int>(texel)).normalized();
                int nearest = 0;
                float best = -2.0f;

                for (int ray = 0; ray < rays; ++ray) {
                    const float closeness = skyVisibilityRay(ray, rays).dot(direction);

                    if (closeness > best) {
                        best = closeness;
                        nearest = ray;
                    }
                }

                table[texel] = nearest;
            }
        }

        cachedRays = rays;
        cached = &table;
        return table;
    }

    /*
    * The sun's first bounce at a point from the sunlit spots, added into its bands (per channel, a = the sum over 4 pi, b
    * = 3 times the sum along the directions over 4 pi, as skyVisibilityFromRays makes them from rays). Each cell's
    * chance (skySunCellChance) mixes its light over its distance squared (at least a cell's size), facing the point, with
    * the same cut to skySunOccludedImportance where the point's nearest ray met a surface well before it; the samples walk
    * the chances in even steps and pick a spot in the cell by the golden ratio's sequence, so neighbouring points choose alike.
    */
    void addSunSpots(const StaticLevel& level, const Vec3& point, const SunEmitters& spots, int samples, std::span<const float> rayDistances, GridMarks& marks,
                     SkyBounce& bands) {
        if (spots.cells.empty() || samples <= 0 || rayDistances.empty()) {
            return;
        }

        const std::vector<int>& nearest = nearestRayTable(static_cast<int>(rayDistances.size()));
        thread_local std::vector<float> plain;
        thread_local std::vector<float> guessed;
        thread_local std::vector<float> running;
        plain.clear();
        guessed.clear();
        running.clear();
        float plainTotal = 0.0f;
        float guessedTotal = 0.0f;
        const float smallest = skySunCellSize * skySunCellSize;
        const float slack = skySunCellSize;

        for (const SunEmitterCell& cell : spots.cells) {
            const Vec3 toCell = cell.centroid - point;
            const float distanceSquared = toCell.lengthSquared();
            const float distance = std::sqrt(distanceSquared);
            const Vec3 direction = distance > 1e-6f ? toCell * (1.0f / distance) : Vec3{0.0f, 1.0f, 0.0f};
            const float facing = 0.05f + std::max(-cell.normal.dot(direction), 0.0f);
            const float met = rayDistances[static_cast<std::size_t>(nearest[static_cast<std::size_t>(cubeTexel(direction))])];
            const float seen = met + slack < distance ? skySunOccludedImportance : 1.0f;
            const float importance = cell.flux * facing / std::max(distanceSquared, smallest);
            plain.push_back(importance);
            guessed.push_back(importance * seen);
            plainTotal += importance;
            guessedTotal += importance * seen;
        }

        if (!(plainTotal > 0.0f)) {
            return;
        }

        // The mixture's chances, running (each cell's in [running before it, its own)).
        float total = 0.0f;

        for (std::size_t index = 0; index < plain.size(); ++index) {
            total += skySunCellChance(plain[index], plainTotal, guessed[index], guessedTotal);
            running.push_back(total);
        }

        constexpr float golden = 0.61803398875f;
        Vec3 open{0.0f, 0.0f, 0.0f};
        std::array<Vec3, 3> lean{};
        const float area = spots.spacing * spots.spacing;

        for (int sample = 0; sample < samples; ++sample) {
            const float target = (static_cast<float>(sample) + 0.5f) / static_cast<float>(samples) * total;
            const std::size_t index = std::min(static_cast<std::size_t>(std::upper_bound(running.begin(), running.end(), target) - running.begin()),
                                               running.size() - 1);
            const SunEmitterCell& cell = spots.cells[index];
            const float chance = (running[index] - (index > 0 ? running[index - 1] : 0.0f)) / total / static_cast<float>(cell.count);
            const float pick = static_cast<float>(sample) * golden;
            const std::uint32_t within = std::min(cell.count - 1, static_cast<std::uint32_t>((pick - std::floor(pick)) * static_cast<float>(cell.count)));
            const SunEmitter& spot = spots.emitters[cell.first + within];
            const Vec3 toSpot = spot.position + spot.normal * skySunRayLift - point;
            const float distanceSquared = toSpot.lengthSquared();
            const float distance = std::sqrt(distanceSquared);

            if (!(distance > 1e-6f) || !(chance > 0.0f)) {
                continue;
            }

            const Vec3 direction = toSpot * (1.0f / distance);
            const float cosineOut = -spot.normal.dot(direction);

            if (cosineOut <= 0.0f || raycast(level, Ray{point, direction}, distance, marks)) {
                continue;
            }

            const Vec3 light = spot.albedo * (area * cosineOut / std::max(distanceSquared, area) / chance / static_cast<float>(samples));
            open = open + light;
            lean[0] = lean[0] + direction * light.x;
            lean[1] = lean[1] + direction * light.y;
            lean[2] = lean[2] + direction * light.z;
        }

        const float overSphere = 1.0f / (4.0f * std::numbers::pi_v<float>);
        const float channels[3] = {open.x, open.y, open.z};

        for (std::size_t channel = 0; channel < bands.size(); ++channel) {
            bands[channel].open += channels[channel] * overSphere;
            bands[channel].lean = bands[channel].lean + lean[channel] * (3.0f * overSphere);
        }
    }
}

float skySunCellChance(float plain, float plainTotal, float guessed, float guessedTotal) {
    const float plainShare = plainTotal > 0.0f ? plain / plainTotal : 0.0f;
    const float guessedShare = guessedTotal > 0.0f ? guessed / guessedTotal : plainShare;
    return 0.5f * plainShare + 0.5f * guessedShare;
}

namespace {}

Vec3 skyHitLight(const SkySample& sample, const Vec3& normal, float fallback) {
    if (sample.known < skyHitKnownShare) {
        return Vec3{fallback, fallback, fallback};
    }

    return skyLightFacing(sample.visibility, sample.bounce, normal, 0.0f);
}

Vec3 skySunHitLight(const SkySample& sample, const Vec3& normal) {
    return sample.known >= skyHitKnownShare ? skyBounceFacing(sample.sunBounce, normal) : Vec3{0.0f, 0.0f, 0.0f};
}

/*
* The rays' directions, openings and front hits go into this thread's own scratch lists, so a point allocates nothing
* once the first has grown them, and the bands come from skyVisibilityFromRays, as the tests' do. The hits are lit after
* every ray is cast, since their fallback is the point's own open share.
*/
SkyCell bakeSkyCell(const StaticLevel& level, const Vec3& point, int rays, float rayLength, float spacing, GridMarks& marks, std::span<const Vec3> albedo,
                    const SkyHitSampler& hitSampler, const SunEmitters* sunEmitters, int sunSamples) {
    thread_local std::vector<Vec3> directions;
    thread_local std::vector<float> openings;
    thread_local std::vector<float> distances;
    distances.clear();
    thread_local std::vector<FrontHit> frontHits;
    thread_local std::vector<float> bounced;
    directions.clear();
    openings.clear();
    frontHits.clear();
    int backFaces = 0;
    int escaped = 0;

    for (int ray = 0; ray < rays; ++ray) {
        const Vec3 direction = skyVisibilityRay(ray, rays);
        const std::optional<LevelHit> hit = raycast(level, Ray{point, direction}, rayLength, marks);
        directions.push_back(direction);
        openings.push_back(hit ? 0.0f : 1.0f);
        distances.push_back(hit ? hit->hit.distance : std::numeric_limits<float>::infinity());

        if (!hit) {
            ++escaped;
            continue;
        }

        const Vec3 facing = triangleFacing(level, hit->triangle);

        if (facing.dot(direction) > 0.0f) {
            ++backFaces;
        } else if (facing.lengthSquared() > 0.0f) {
            frontHits.push_back(FrontHit{static_cast<std::size_t>(ray), point + direction * hit->hit.distance, facing.normalized(), hit->triangle});
        }
    }

    SkyCell cell;
    cell.visibility = skyVisibilityFromRays(directions, openings);
    cell.state = static_cast<float>(backFaces) > skyVisibilityBuriedShare * static_cast<float>(rays) ? SkyCellState::Buried : SkyCellState::Baked;
    cell.escapedRays = escaped;

    // The light on each front hit, the sky's and (with sunlit spots) the sun's bounced light, from the grid round it.
    if (!albedo.empty() && hitSampler) {
        thread_local std::vector<Vec3> hitLight;
        thread_local std::vector<Vec3> sunLight;
        hitLight.clear();
        sunLight.clear();
        const float fallback = std::clamp(2.0f * cell.visibility.open, 0.0f, 1.0f);
        int unsure = 0;

        for (const FrontHit& hit : frontHits) {
            const SkySample sample = hitSampler(hit.point, hit.normal, directions[hit.ray] * -1.0f);
            hitLight.push_back(skyHitLight(sample, hit.normal, fallback));

            if (sunEmitters) {
                sunLight.push_back(skySunHitLight(sample, hit.normal));
            }

            unsure += sample.known < skyHitKnownShare && sample.missing >= skyHitKnownShare ? 1 : 0;
        }

        cell.unsureShare = frontHits.empty() ? 0.0f : static_cast<float>(unsure) / static_cast<float>(frontHits.size());

        // Each channel's values in turn: zero for every ray, then each front hit's albedo times the light on it.
        const auto bandsOf = [&](const std::vector<Vec3>& light) {
            SkyBounce bands = noSkyBounce;

            for (std::size_t channel = 0; channel < bands.size(); ++channel) {
                bounced.assign(directions.size(), 0.0f);

                for (std::size_t index = 0; index < frontHits.size(); ++index) {
                    const FrontHit& hit = frontHits[index];
                    const Vec3 rho = hit.triangle < albedo.size() ? albedo[hit.triangle] : Vec3{0.0f, 0.0f, 0.0f};
                    const float channels[3] = {rho.x * light[index].x, rho.y * light[index].y, rho.z * light[index].z};
                    bounced[hit.ray] = channels[channel];
                }

                bands[channel] = skyVisibilityFromRays(directions, bounced);
            }

            return bands;
        };

        cell.bounce = bandsOf(hitLight);

        // The sun's: its later bounces from the hits, its first aimed at the sunlit spots.
        if (sunEmitters) {
            cell.sunBounce = bandsOf(sunLight);
            addSunSpots(level, point, *sunEmitters, sunSamples, distances, marks, cell.sunBounce);
        }
    }

    for (int direction = 0; direction < skyReachDirections; ++direction) {
        const std::optional<LevelHit> hit = raycast(level, Ray{point, skyReachDirection(direction)}, spacing, marks);
        cell.reach[static_cast<std::size_t>(direction)] = hit ? std::min(hit->hit.distance / spacing, 1.0f) : 1.0f;
    }

    return cell;
}

/*
* The beam's rays start on a plane across it past the region (its half diagonal, then skySunOccluderReach more, so
* blockers above still shade it) and run through it; the plane's two axes from the sun's way, its extent the region's
* corners seen along the beam. A hit counts when it faces the sun, lies in the region and its triangle has albedo. The
* cells are sorted by their places, so the result does not depend on the jobs.
*/
SunEmitters buildSunEmitters(const StaticLevel& level, std::span<const Vec3> albedo, const Vec3& toSun, const Aabb& region, float spacing, JobSystem& jobs) {
    SunEmitters spots;
    spots.toSun = toSun.normalized();
    spots.spacing = spacing;
    spots.region = region;

    if (albedo.empty() || !(spacing > 0.0f)) {
        return spots;
    }

    const Vec3 along = spots.toSun;
    const Vec3 helper = std::abs(along.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 across = along.cross(helper).normalized();
    const Vec3 up = across.cross(along);
    const Vec3 centre = region.centre();
    const float halfDiagonal = region.size().length() * 0.5f;
    float lowU = 0.0f;
    float highU = 0.0f;
    float lowV = 0.0f;
    float highV = 0.0f;

    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 place{(corner & 1) ? region.maximum.x : region.minimum.x, (corner & 2) ? region.maximum.y : region.minimum.y,
                         (corner & 4) ? region.maximum.z : region.minimum.z};
        const Vec3 offset = place - centre;
        lowU = std::min(lowU, offset.dot(across));
        highU = std::max(highU, offset.dot(across));
        lowV = std::min(lowV, offset.dot(up));
        highV = std::max(highV, offset.dot(up));
    }

    const int columns = std::max(1, static_cast<int>(std::ceil((highU - lowU) / spacing)));
    const int rows = std::max(1, static_cast<int>(std::ceil((highV - lowV) / spacing)));
    const float start = halfDiagonal + skySunOccluderReach;
    std::vector<std::vector<SunEmitter>> found(static_cast<std::size_t>(rows));

    jobs.parallelFor(static_cast<std::size_t>(rows), 1, [&](std::size_t row) {
        thread_local GridMarks marks;
        const float v = lowV + (static_cast<float>(row) + 0.5f) * spacing;

        for (int column = 0; column < columns; ++column) {
            const float u = lowU + (static_cast<float>(column) + 0.5f) * spacing;
            const Vec3 origin = centre + across * u + up * v + along * start;
            const std::optional<LevelHit> hit = raycast(level, Ray{origin, along * -1.0f}, start + halfDiagonal, marks);

            if (!hit || hit->triangle >= albedo.size()) {
                continue;
            }

            const Vec3 facing = triangleFacing(level, hit->triangle);
            const Vec3 point = origin - along * hit->hit.distance;
            const Vec3 colour = albedo[hit->triangle];

            if (facing.dot(along) <= 0.0f || !region.contains(point) || std::max({colour.x, colour.y, colour.z}) <= 0.0f) {
                continue;
            }

            found[row].push_back(SunEmitter{point, facing.normalized(), colour});
        }
    });

    struct Keyed {
        std::int64_t key;
        SunEmitter spot;
    };

    std::vector<Keyed> keyed;

    for (const std::vector<SunEmitter>& row : found) {
        for (const SunEmitter& spot : row) {
            const auto cellOf = [](float coordinate) {
                return static_cast<std::int64_t>(std::floor(coordinate / skySunCellSize)) + (std::int64_t{1} << 20);
            };
            keyed.push_back(Keyed{(cellOf(spot.position.x) << 42) | (cellOf(spot.position.y) << 21) | cellOf(spot.position.z), spot});
        }
    }

    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
    spots.emitters.reserve(keyed.size());
    const float area = spacing * spacing;

    for (std::size_t index = 0; index < keyed.size(); ++index) {
        if (index == 0 || keyed[index].key != keyed[index - 1].key) {
            spots.cells.push_back(SunEmitterCell{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, static_cast<std::uint32_t>(index), 0});
        }

        const SunEmitter& spot = keyed[index].spot;
        SunEmitterCell& cell = spots.cells.back();
        const float flux = std::max({spot.albedo.x, spot.albedo.y, spot.albedo.z}) * area;
        cell.centroid = cell.centroid + spot.position * flux;
        cell.normal = cell.normal + spot.normal * flux;
        cell.flux += flux;
        ++cell.count;
        spots.emitters.push_back(spot);
    }

    for (SunEmitterCell& cell : spots.cells) {
        cell.centroid = cell.centroid * (1.0f / cell.flux);
        cell.normal = cell.normal * (1.0f / cell.flux);
    }

    return spots;
}

bool skyWantsDeepRays(const SkyCell& cell, const SkyVisibilitySettings& settings) {
    return settings.deepRays > settings.rays && cell.state == SkyCellState::Baked && cell.escapedRays > 0 && cell.escapedRays < settings.deepOpenRays;
}

double skySecondsAtFullRays(double secondsPerCell, int rays, int fullRays) {
    return rays > 0 ? secondsPerCell * static_cast<double>(fullRays) / static_cast<double>(rays) : 0.0;
}

unsigned skyBackgroundWorkers(float share, unsigned hardwareThreads) {
    return std::max(1u, static_cast<unsigned>(std::lround(std::clamp(share, 0.0f, 1.0f) * static_cast<float>(hardwareThreads))));
}

int skyMinimumBatch(unsigned threads) {
    return static_cast<int>(std::max(threads, 1u) * skyBakePointsPerJob);
}

int skyCellsForBudget(double budgetSeconds, double secondsPerCell, int waiting, int minimumBatch) {
    if (waiting <= 0 || !(budgetSeconds > 0.0)) {
        return 0;
    }

    if (!(secondsPerCell > 0.0)) {
        return waiting;
    }

    const double fit = std::round(budgetSeconds / secondsPerCell);
    return fit >= static_cast<double>(waiting) ? waiting : std::min(waiting, std::max({1, minimumBatch, static_cast<int>(fit)}));
}

SkyBakePlan planSkyBake(int waiting, int refining, bool afresh, bool everything, int budgetCells, int quickBudgetCells, bool quickFewer) {
    if (everything) {
        return SkyBakePlan{waiting, false, refining};
    }

    if (waiting > 0 && afresh) {
        return SkyBakePlan{waiting, quickFewer, 0};
    }

    if (waiting > budgetCells) {
        return quickFewer ? SkyBakePlan{std::min(waiting, quickBudgetCells), true, 0} : SkyBakePlan{budgetCells, false, 0};
    }

    return SkyBakePlan{waiting, false, std::min(refining, std::max(budgetCells - waiting, 0))};
}

namespace {
    // Lattice coordinates the store's key holds: 20 bits an axis, offset by half their range.
    constexpr int storeAxisBits = 20;
    constexpr int storeAxisHalf = 1 << (storeAxisBits - 1);

    // A place's key: the cascade in the top bits, then x, y and z offset to be positive; none past the range.
    std::optional<std::uint64_t> storeKey(int cascade, const ProbeGridCoord& place) {
        for (const int axis : {place.x, place.y, place.z}) {
            if (axis < -storeAxisHalf || axis >= storeAxisHalf) {
                return std::nullopt;
            }
        }

        const auto field = [](int axis) { return static_cast<std::uint64_t>(axis + storeAxisHalf); };
        return (static_cast<std::uint64_t>(cascade) << (3 * storeAxisBits)) | (field(place.x) << (2 * storeAxisBits)) | (field(place.y) << storeAxisBits) |
               field(place.z);
    }
}

std::size_t skyStoreCapacity(const SkyVisibilitySettings& settings) {
    return static_cast<std::size_t>(static_cast<double>(settings.storeMegabytes) * 1024.0 * 1024.0 / static_cast<double>(skyStoreBytesPerPoint));
}

void SkyCellStore::setCapacity(std::size_t points) {
    capacity = points;

    while (places.size() > capacity) {
        places.erase(used.back().key);
        used.pop_back();
    }
}

void SkyCellStore::clear() {
    used.clear();
    places.clear();
}

void SkyCellStore::keep(int cascade, const ProbeGridCoord& place, const SkyCell& cell, SkyBakeStage next, std::uint32_t sunEpoch) {
    const std::optional<std::uint64_t> key = storeKey(cascade, place);

    if (capacity == 0 || !key) {
        return;
    }

    const auto found = places.find(*key);

    if (found != places.end()) {
        found->second->packed = packSkyCell(cell);
        found->second->next = next;
        found->second->sunEpoch = sunEpoch;
        used.splice(used.begin(), used, found->second);
        return;
    }

    used.push_front(Entry{*key, packSkyCell(cell), next, sunEpoch});
    places.emplace(*key, used.begin());
    setCapacity(capacity);
}

std::optional<StoredSkyCell> SkyCellStore::find(int cascade, const ProbeGridCoord& place) {
    const std::optional<std::uint64_t> key = storeKey(cascade, place);

    if (!key) {
        return std::nullopt;
    }

    const auto found = places.find(*key);

    if (found == places.end()) {
        return std::nullopt;
    }

    used.splice(used.begin(), used, found->second);
    return StoredSkyCell{unpackSkyCell(found->second->packed), found->second->next, found->second->sunEpoch};
}

void SkyCellStore::touch(int cascade, const ProbeGridCoord& place) {
    const std::optional<std::uint64_t> key = storeKey(cascade, place);

    if (!key) {
        return;
    }

    const auto found = places.find(*key);

    if (found != places.end()) {
        used.splice(used.begin(), used, found->second);
    }
}

std::size_t SkyCellStore::size() const {
    return places.size();
}

/*
* Afresh when nothing was placed yet, the level changed, or the settings shape another grid; otherwise each cascade's
* window moves, and each slot whose lattice point changed (scrolledProbes) marks the place it leaves as used now in the
* store, so the places last in the grid are the last to go, and takes its new place's point from the store or waits.
*/
bool SkyVisibilityGrid::follow(const Vec3& place, const SkyVisibilitySettings& settings, std::uint64_t level) {
    const bool afresh = !started || level != levelVersion || !sameSkyVisibilityGrid(shape, settings);
    std::vector<ProbeCascade> next = skyVisibilityCascades(place, settings);
    shape = settings;
    motion = afresh ? Vec3{} : place - centre;
    centre = place;
    store.setCapacity(skyStoreCapacity(settings));

    if (afresh) {
        started = true;
        levelVersion = level;
        cascades = std::move(next);
        startAfresh();
        return true;
    }

    bool changed = false;

    for (std::size_t c = 0; c < cascades.size(); ++c) {
        for (const int cell : scrolledProbes(cascades[c], next[c])) {
            store.touch(static_cast<int>(c), probeLatticeCoord(cascades[c], cell - cascades[c].firstProbe));
            arrive(c, cell, next[c]);
            changed = true;
        }
    }

    cascades = std::move(next);

    if (changed) {
        ++version;
    }

    return false;
}

// A new level or shape: nothing kept means anything any more.
void SkyVisibilityGrid::startAfresh() {
    const int count = skyVisibilityCellCount(shape);
    store.clear();
    cells.assign(static_cast<std::size_t>(count), SkyCell{});
    isWaiting.assign(static_cast<std::size_t>(count), NotListed);
    waiting.assign(cascades.size(), {});
    waitingCount = 0;
    refining.reset(static_cast<std::size_t>(count));
    deepening.reset(static_cast<std::size_t>(count));

    for (int cell = 0; cell < count; ++cell) {
        wait(cell);
    }

    allChanged = true;
    changedCells.clear();
    ++generation;
    ++version;
}

// A point waiting again (cleared by a move) is baked afresh at full rays, so it leaves both queues. One found in the
// store meanwhile is still listed, so it only waits again.
void SkyVisibilityGrid::wait(int cell) {
    std::uint8_t& flag = isWaiting[static_cast<std::size_t>(cell)];

    if (flag == NotListed) {
        waiting[static_cast<std::size_t>(probeCascadeHolding(cascades, cell))].push_back(cell);
    }

    if (flag != Waiting) {
        flag = Waiting;
        ++waitingCount;
    }

    refining.remove(cell);
    deepening.remove(cell);
}

void SkyVisibilityGrid::arrive(std::size_t cascade, int cell, const ProbeCascade& now) {
    if (!allChanged) {
        changedCells.push_back(cell);
    }

    const std::optional<StoredSkyCell> found = store.find(static_cast<int>(cascade), probeLatticeCoord(now, cell - now.firstProbe));

    if (!found) {
        cells[static_cast<std::size_t>(cell)] = SkyCell{};
        wait(cell);
        return;
    }

    cells[static_cast<std::size_t>(cell)] = found->cell;
    std::uint8_t& flag = isWaiting[static_cast<std::size_t>(cell)];

    if (flag == Waiting) {
        flag = Found;
        --waitingCount;
    }

    // Baked for an older sun: at least baked again at the full rays.
    queueNext(cell, found->sunEpoch != sunEpoch && found->next == SkyBakeStage::Finished ? SkyBakeStage::Refine : found->next);
}

/*
* The angle between two unit directions, past the refresh's, by their dot product against its cosine. Every baked
* point (buried ones too: cheap, and the same rule everywhere) goes to the refinement queue, which follows the waiting
* points and comes before the deep rays; the version stays, since no point changed yet.
*/
bool SkyVisibilityGrid::followSun(const std::optional<Vec3>& toSun) {
    const std::optional<Vec3> wanted = shape.sunBounce ? toSun : std::nullopt;
    const float refreshCosine = std::cos(shape.sunRefreshDegrees * std::numbers::pi_v<float> / 180.0f);
    const bool moved = wanted.has_value() != sun.has_value() || (wanted && sun && wanted->dot(*sun) < refreshCosine);

    if (!moved) {
        return false;
    }

    sun = wanted;
    ++sunEpoch;

    for (std::size_t cell = 0; cell < cells.size(); ++cell) {
        if (cells[cell].state != SkyCellState::Unknown && isWaiting[cell] != Waiting) {
            queueNext(static_cast<int>(cell), SkyBakeStage::Refine);
        }
    }

    return true;
}

void SkyVisibilityGrid::queueNext(int cell, SkyBakeStage next) {
    if (next == SkyBakeStage::Refine) {
        refining.push(cell);
    } else {
        refining.remove(cell);
    }

    if (next == SkyBakeStage::Deepen) {
        deepening.push(cell);
    } else {
        deepening.remove(cell);
    }
}

void SkyVisibilityGrid::BakeQueue::reset(std::size_t cells) {
    order.clear();
    queued.assign(cells, 0);
    next = 0;
    count = 0;
}

// Once: a point already queued stays where it is in the queue.
void SkyVisibilityGrid::BakeQueue::push(int cell) {
    std::uint8_t& flag = queued[static_cast<std::size_t>(cell)];

    if (flag != 0) {
        return;
    }

    flag = 1;
    order.push_back(cell);
    ++count;
}

void SkyVisibilityGrid::BakeQueue::remove(int cell) {
    std::uint8_t& flag = queued[static_cast<std::size_t>(cell)];

    if (flag != 0) {
        flag = 0;
        --count;
    }
}

bool SkyVisibilityGrid::BakeQueue::contains(int cell) const {
    return queued[static_cast<std::size_t>(cell)] != 0;
}

/*
* Walked from its front, skipping points a move took out of it, until the batch is full; the walked part is dropped
* once it is more than half the list.
*/
void SkyVisibilityGrid::BakeQueue::take(int maxCells, std::vector<int>& points) {
    while (next < order.size() && static_cast<int>(points.size()) < maxCells) {
        const int cell = order[next++];

        if (contains(cell)) {
            remove(cell);
            points.push_back(cell);
        }
    }

    if (next * 2 > order.size()) {
        order.erase(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(next));
        next = 0;
    }
}


/*
* Each job bakes its points into their own entries of the results while every thread reads the grid round its hits as
* it stood when the batch was taken, so the result is the same on any number of threads (a point's neighbour in the
* same batch is read as it was). Each thread walks the level's grid with marks of its own.
*/
void bakeSkyBatch(const StaticLevel& level, JobSystem& jobs, std::span<const Vec3> albedo, SkyBakeBatch& batch) {
    const double start = profilerSeconds();
    batch.results.resize(batch.entries.size());

    if (batch.entries.empty()) {
        return;
    }

    static const std::vector<SkyCell> none;
    const std::vector<SkyCell>& grid = batch.grid ? *batch.grid : none;
    const SkyHitSampler gridAtHits = [&batch, &grid](const Vec3& point, const Vec3& normal, const Vec3& toViewer) {
        return sampleSkyVisibility(batch.cascades, grid, batch.lookupBias, point, normal, toViewer);
    };

    jobs.parallelFor(batch.entries.size(), skyBakePointsPerJob, [&](std::size_t index) {
        thread_local GridMarks marks;
        const SkyBakeBatch::Entry& entry = batch.entries[index];
        batch.results[index] =
            bakeSkyCell(level, entry.position, batch.rays, batch.rayLength, entry.spacing, marks, albedo, gridAtHits, batch.sunEmitters.get(), batch.sunSamples);
    });

    batch.secondsPerCell = (profilerSeconds() - start) / static_cast<double>(batch.entries.size());
}

// The places and positions are worked out now, while the cascades are those the points belong to.
SkyBakeBatch SkyVisibilityGrid::makeBatch(std::span<const int> points, int rays, bool quick, bool refining, bool deepening) const {
    SkyBakeBatch batch;
    batch.rays = rays;
    batch.rayLength = shape.rayLength;
    batch.lookupBias = shape.lookupBias;
    batch.quick = quick;
    batch.refining = refining;
    batch.deepening = deepening;
    batch.generation = generation;
    batch.toSun = sun;
    batch.sunEpoch = sunEpoch;
    // Only spots found for this very sun (prepareSunEmitters runs right after followSun, so a frame's batches have them).
    batch.sunEmitters = sun && sunEmittersEpoch == sunEpoch ? sunEmitters : nullptr;
    batch.sunSamples = shape.sunSamples;
    batch.entries.reserve(points.size());

    for (const int cell : points) {
        const int c = probeCascadeHolding(cascades, cell);
        const ProbeCascade& cascade = cascades[static_cast<std::size_t>(c)];
        batch.entries.push_back(SkyBakeBatch::Entry{cell, c, probeLatticeCoord(cascade, cell - cascade.firstProbe),
                                                    probePosition(cascade.volume, cell - cascade.firstProbe), cascade.volume.spacing.x});
    }

    if (!batch.entries.empty()) {
        batch.cascades = cascades;
        batch.grid = std::make_shared<const std::vector<SkyCell>>(cells);
    }

    return batch;
}

/*
* The points found in the store meanwhile leave the list first. Then all of it in its order when it fits; otherwise
* each point's rank, worked out once (behind counts as farther than anything ahead), and the best count chosen by
* nth_element, linear in the list, with the point's number breaking ties so the choice never depends on the list's order.
*/
void SkyVisibilityGrid::takeWaiting(std::size_t cascade, std::size_t count, const Vec3& forward, std::vector<int>& batch) {
    std::vector<int>& list = waiting[cascade];
    std::erase_if(list, [this](int cell) {
        std::uint8_t& flag = isWaiting[static_cast<std::size_t>(cell)];

        if (flag == Found) {
            flag = NotListed;
            return true;
        }

        return false;
    });

    if (list.size() <= count) {
        batch.insert(batch.end(), list.begin(), list.end());
        list.clear();
        return;
    }

    const ProbeCascade& holding = cascades[cascade];
    const bool hasForward = forward.lengthSquared() > 0.0f;
    const bool hasMotion = motion.lengthSquared() > 0.0f;
    constexpr float behindRank = 1e9f;
    thread_local std::vector<std::pair<float, int>> ranked;
    ranked.clear();

    for (const int cell : list) {
        const Vec3 offset = probePosition(holding.volume, cell - holding.firstProbe) - centre;
        const bool ahead = (!hasForward && !hasMotion) || (hasForward && offset.dot(forward) >= 0.0f) || (hasMotion && offset.dot(motion) >= 0.0f);
        ranked.emplace_back(offset.length() + (ahead ? 0.0f : behindRank), cell);
    }

    std::nth_element(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(count), ranked.end());
    list.clear();

    for (std::size_t i = 0; i < ranked.size(); ++i) {
        (i < count ? batch : list).push_back(ranked[i].second);
    }
}

// The synchronous bake: a batch taken, baked on the caller (and jobs) and applied at once.
int SkyVisibilityGrid::bake(const StaticLevel& level, JobSystem& jobs, int maxCells, bool quick, const Vec3& forward, std::span<const Vec3> albedo) {
    SkyBakeBatch batch = takeBatch(maxCells, quick, forward);
    bakeSkyBatch(level, jobs, albedo, batch);
    applyBatch(batch);
    return static_cast<int>(batch.entries.size());
}

int SkyVisibilityGrid::refine(const StaticLevel& level, JobSystem& jobs, int maxCells, std::span<const Vec3> albedo) {
    SkyBakeBatch batch = takeRefineBatch(maxCells);
    bakeSkyBatch(level, jobs, albedo, batch);
    applyBatch(batch);
    return static_cast<int>(batch.entries.size());
}

int SkyVisibilityGrid::deepen(const StaticLevel& level, JobSystem& jobs, int maxCells, std::span<const Vec3> albedo) {
    SkyBakeBatch batch = takeDeepBatch(maxCells);
    bakeSkyBatch(level, jobs, albedo, batch);
    applyBatch(batch);
    return static_cast<int>(batch.entries.size());
}

SkyBakeBatch SkyVisibilityGrid::takeBatch(int maxCells, bool quick, const Vec3& forward) {
    const int count = std::min(maxCells, getWaiting());
    thread_local std::vector<int> points;
    points.clear();

    for (std::size_t c = waiting.size(); count > 0 && c-- > 0 && static_cast<int>(points.size()) < count;) {
        takeWaiting(c, static_cast<std::size_t>(count) - points.size(), forward, points);
    }

    for (const int cell : points) {
        isWaiting[static_cast<std::size_t>(cell)] = NotListed;
    }

    waitingCount -= static_cast<int>(points.size());
    const bool refines = quick && shape.quickRays < shape.rays;
    return makeBatch(points, refines ? shape.quickRays : shape.rays, refines, false, false);
}

// Those taken replace their earlier bake in the store when applied.
SkyBakeBatch SkyVisibilityGrid::takeRefineBatch(int maxCells) {
    thread_local std::vector<int> points;
    points.clear();
    refining.take(maxCells, points);
    return makeBatch(points, shape.rays, false, true, false);
}

// The deep rays turned off (or no more than the rays) meanwhile: the queue is let go.
SkyBakeBatch SkyVisibilityGrid::takeDeepBatch(int maxCells) {
    thread_local std::vector<int> points;
    points.clear();

    if (shape.deepRays <= shape.rays) {
        deepening.reset(cells.size());
    }

    deepening.take(maxCells, points);
    return makeBatch(points, shape.deepRays, false, false, true);
}

/*
* A quick point, or one baked (not refined) before the grid round its hits was, is to be baked again; one baked or
* refined at the full rays that wants the deep rays is to get them; a deep bake is finished. The store keeps each so. A
* point placed while queued (it came back from the store meanwhile) takes this result's next step instead.
*/
void SkyVisibilityGrid::applyBatch(const SkyBakeBatch& batch) {
    if (batch.generation != generation || batch.entries.empty()) {
        return;
    }

    for (std::size_t index = 0; index < batch.entries.size(); ++index) {
        const SkyBakeBatch::Entry& entry = batch.entries[index];
        const SkyCell& result = batch.results[index];
        const ProbeCascade& holding = cascades[static_cast<std::size_t>(entry.cascade)];
        const ProbeGridCoord place = probeLatticeCoord(holding, entry.cell - holding.firstProbe);
        const bool samePlace = place.x == entry.place.x && place.y == entry.place.y && place.z == entry.place.z;

        // Not baked after all (no level to bake against): waits again where it still belongs.
        if (result.state == SkyCellState::Unknown) {
            if (samePlace && isWaiting[static_cast<std::size_t>(entry.cell)] != Waiting && cells[static_cast<std::size_t>(entry.cell)].state == SkyCellState::Unknown) {
                wait(entry.cell);
            }

            continue;
        }

        SkyBakeStage next = SkyBakeStage::Finished;

        if (!batch.deepening && !batch.refining && (batch.quick || result.unsureShare > skyUnsureRefineShare)) {
            next = SkyBakeStage::Refine;
        } else if (!batch.deepening && skyWantsDeepRays(result, shape)) {
            next = SkyBakeStage::Deepen;
        }

        // Baked for a sun since moved on: placed (better than nothing meanwhile) and baked again.
        if (batch.sunEpoch != sunEpoch && next == SkyBakeStage::Finished) {
            next = SkyBakeStage::Refine;
        }

        store.keep(entry.cascade, entry.place, result, next, batch.sunEpoch);

        if (!samePlace) {
            continue;
        }

        const std::size_t cell = static_cast<std::size_t>(entry.cell);
        cells[cell] = result;

        if (isWaiting[cell] == Waiting) {
            isWaiting[cell] = Found;
            --waitingCount;
        }

        queueNext(entry.cell, next);

        if (!allChanged) {
            changedCells.push_back(entry.cell);
        }
    }

    ++version;
}

/*
* Found round the coarsest cascade's window (its first and last points), skySunEmitterMargin past it on every side.
* Rebuilt when the sun or the spacing changed, or the window's middle moved skySunEmitterRebuild from the last build's.
*/
void SkyVisibilityGrid::prepareSunEmitters(const StaticLevel& level, std::span<const Vec3> albedo, JobSystem& jobs) {
    if (!sun || cascades.empty()) {
        sunEmitters.reset();
        return;
    }

    const ProbeVolume& coarsest = cascades.back().volume;
    const Vec3 last = coarsest.origin + coarsest.spacing * Vec3{static_cast<float>(coarsest.counts.x - 1), static_cast<float>(coarsest.counts.y - 1),
                                                                static_cast<float>(coarsest.counts.z - 1)};
    const Vec3 margin{skySunEmitterMargin, skySunEmitterMargin, skySunEmitterMargin};
    const Aabb region{coarsest.origin - margin, last + margin};
    const bool current = sunEmitters && sunEmittersEpoch == sunEpoch && sunEmitters->spacing == shape.sunEmitterSpacing &&
                         (region.centre() - sunEmittersCentre).length() < skySunEmitterRebuild;

    if (current) {
        return;
    }

    sunEmitters = std::make_shared<const SunEmitters>(buildSunEmitters(level, albedo, *sun, region, shape.sunEmitterSpacing, jobs));
    sunEmittersEpoch = sunEpoch;
    sunEmittersCentre = region.centre();
}

std::shared_ptr<const SunEmitters> SkyVisibilityGrid::getSunEmitters() const {
    return sunEmitters;
}

int SkyVisibilityGrid::getWaiting() const {
    return waitingCount;
}

int SkyVisibilityGrid::getRefining() const {
    return refining.count;
}

int SkyVisibilityGrid::getDeepening() const {
    return deepening.count;
}

std::span<const ProbeCascade> SkyVisibilityGrid::getCascades() const {
    return cascades;
}

std::span<const SkyCell> SkyVisibilityGrid::getCells() const {
    return cells;
}

std::string SkyVisibilityGrid::describePoints(const Vec3& place) const {
    std::string text = std::format("waiting {}  refining {}  deepening {}", waitingCount, refining.count, deepening.count);
    const char* const states[] = {"unknown", "baked", "buried"};

    for (std::size_t c = 0; c < cascades.size(); ++c) {
        const ProbeCascade& cascade = cascades[c];
        const ProbeVolume& volume = cascade.volume;
        text += std::format("\ncascade {} (spacing {:.2f} m)", c, volume.spacing.x);
        const ProbeCell cell = probeCell(volume, place);

        for (int corner = 0; corner < 8; ++corner) {
            const int slot = probeSlot(volume, probeCellCorner(cell, volume.counts, corner));
            const std::size_t index = static_cast<std::size_t>(cascade.firstProbe + slot);
            const SkyCell& sky = cells[index];
            const Vec3 position = probePosition(volume, slot);
            const int slotIndex = static_cast<int>(index);
            text += std::format("\n  ({:.2f} {:.2f} {:.2f}) {}{}{}{}  open {:.4f}  escaped {}  unsure {:.2f}  bounce {:.4f} {:.4f} {:.4f}  sun {:.4f} {:.4f} {:.4f}", position.x, position.y,
                                position.z, states[static_cast<std::size_t>(sky.state)], isWaiting[index] == Waiting ? ", waiting" : "",
                                refining.contains(slotIndex) ? ", quick" : "", deepening.contains(slotIndex) ? ", deep" : "", sky.visibility.open,
                                sky.escapedRays, sky.unsureShare, sky.bounce[0].open, sky.bounce[1].open, sky.bounce[2].open,
                                sky.sunBounce[0].open, sky.sunBounce[1].open, sky.sunBounce[2].open);
        }
    }

    return text;
}

bool SkyVisibilityGrid::takeChanges(std::vector<int>& changed) {
    changed.swap(changedCells);
    changedCells.clear();

    if (allChanged) {
        allChanged = false;
        changed.clear();
        return true;
    }

    return false;
}

std::uint64_t SkyVisibilityGrid::getVersion() const {
    return version;
}

SkyBackgroundBaker::SkyBackgroundBaker(JobSystem& jobs, std::function<void()> threadStart):
    jobs(jobs)
{
    thread = std::thread([this, start = std::move(threadStart)] {
        if (start) {
            start();
        }

        run();
    });
}

SkyBackgroundBaker::~SkyBackgroundBaker() {
    {
        const std::lock_guard<std::mutex> guard{lock};
        stopping = true;
    }

    changed.notify_all();
    thread.join();
}

bool SkyBackgroundBaker::busy() const {
    const std::lock_guard<std::mutex> guard{lock};
    return started;
}

void SkyBackgroundBaker::start(SkyBakeBatch next, std::shared_ptr<const StaticLevel> nextLevel, std::shared_ptr<const std::vector<Vec3>> nextAlbedo) {
    {
        const std::lock_guard<std::mutex> guard{lock};

        if (started) {
            return;
        }

        batch = std::move(next);
        level = std::move(nextLevel);
        albedo = std::move(nextAlbedo);
        started = true;
        finished = false;
    }

    changed.notify_all();
}

std::optional<SkyBakeBatch> SkyBackgroundBaker::takeFinished() {
    const std::lock_guard<std::mutex> guard{lock};

    if (!started || !finished) {
        return std::nullopt;
    }

    started = false;
    finished = false;
    level.reset();
    albedo.reset();
    return std::move(batch);
}

void SkyBackgroundBaker::waitFinished() {
    std::unique_lock<std::mutex> guard{lock};
    changed.wait(guard, [this] { return !started || finished; });
}

// Waits for a batch, bakes it unlocked (the batch is the thread's alone until finished is set), then says so.
void SkyBackgroundBaker::run() {
    while (true) {
        {
            std::unique_lock<std::mutex> guard{lock};
            changed.wait(guard, [this] { return stopping || (started && !finished); });

            if (stopping) {
                return;
            }
        }

        if (level) {
            const std::span<const Vec3> colours = albedo ? std::span<const Vec3>{*albedo} : std::span<const Vec3>{};
            bakeSkyBatch(*level, jobs, colours, batch);
        } else {
            batch.results.assign(batch.entries.size(), SkyCell{});
        }

        {
            const std::lock_guard<std::mutex> guard{lock};
            finished = true;
        }

        changed.notify_all();
    }
}