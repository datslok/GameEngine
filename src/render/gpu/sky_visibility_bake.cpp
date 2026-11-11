#include "render/gpu/sky_visibility_bake.h"

#include "render/gpu/ddgi.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {
    // A job takes this many points at once, so handing them out costs little beside their rays.
    constexpr std::size_t pointsPerJob = 8;

    // Whether a ray along direction meets this triangle from behind: its winding (counterclockwise seen from the front)
    // gives its outward normal, and the ray runs along it.
    bool hitsBackFace(const StaticLevel& level, std::uint32_t triangle, const Vec3& direction) {
        const auto corners = level.getTriangle(triangle);
        const Vec3 normal = (corners[1] - corners[0]).cross(corners[2] - corners[0]);
        return normal.dot(direction) > 0.0f;
    }
}

/*
* The rays' directions and openings go into this thread's own scratch lists, so a point allocates nothing once the
* first has grown them, and the bands come from skyVisibilityFromRays, as the tests' do.
*/
SkyCell bakeSkyCell(const StaticLevel& level, const Vec3& point, int rays, float rayLength, float spacing, GridMarks& marks) {
    thread_local std::vector<Vec3> directions;
    thread_local std::vector<float> openings;
    directions.clear();
    openings.clear();
    int backFaces = 0;

    for (int ray = 0; ray < rays; ++ray) {
        const Vec3 direction = skyVisibilityRay(ray, rays);
        const std::optional<LevelHit> hit = raycast(level, Ray{point, direction}, rayLength, marks);
        directions.push_back(direction);
        openings.push_back(hit ? 0.0f : 1.0f);

        if (hit && hitsBackFace(level, hit->triangle, direction)) {
            ++backFaces;
        }
    }

    SkyCell cell;
    cell.visibility = skyVisibilityFromRays(directions, openings);
    cell.state = static_cast<float>(backFaces) > skyVisibilityBuriedShare * static_cast<float>(rays) ? SkyCellState::Buried : SkyCellState::Baked;

    for (int direction = 0; direction < skyReachDirections; ++direction) {
        const std::optional<LevelHit> hit = raycast(level, Ray{point, skyReachDirection(direction)}, spacing, marks);
        cell.reach[static_cast<std::size_t>(direction)] = hit ? std::min(hit->hit.distance / spacing, 1.0f) : 1.0f;
    }

    return cell;
}

int skyCellsForBudget(double budgetSeconds, double secondsPerCell, int waiting) {
    if (waiting <= 0 || !(budgetSeconds > 0.0)) {
        return 0;
    }

    if (!(secondsPerCell > 0.0)) {
        return waiting;
    }

    const double fit = std::round(budgetSeconds / secondsPerCell);
    return fit >= static_cast<double>(waiting) ? waiting : std::max(1, static_cast<int>(fit));
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

void SkyCellStore::keep(int cascade, const ProbeGridCoord& place, const SkyCell& cell, bool quick) {
    const std::optional<std::uint64_t> key = storeKey(cascade, place);

    if (capacity == 0 || !key) {
        return;
    }

    const auto found = places.find(*key);

    if (found != places.end()) {
        found->second->packed = packSkyCell(cell);
        found->second->quick = quick;
        used.splice(used.begin(), used, found->second);
        return;
    }

    used.push_front(Entry{*key, packSkyCell(cell), quick});
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
    return StoredSkyCell{unpackSkyCell(found->second->packed), found->second->quick};
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
    isRefining.assign(static_cast<std::size_t>(count), 0);
    refining.clear();
    nextRefining = 0;
    refiningCount = 0;

    for (int cell = 0; cell < count; ++cell) {
        wait(cell);
    }

    ++version;
}

// A point waiting again (cleared by a move) is baked afresh at full rays, so it leaves refinement. One found in the
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

    std::uint8_t& refined = isRefining[static_cast<std::size_t>(cell)];

    if (refined != 0) {
        refined = 0;
        --refiningCount;
    }
}

void SkyVisibilityGrid::arrive(std::size_t cascade, int cell, const ProbeCascade& now) {
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

    std::uint8_t& refined = isRefining[static_cast<std::size_t>(cell)];

    if (refined != 0) {
        refined = 0;
        --refiningCount;
    }

    if (found->quick) {
        queueRefine(cell);
    }
}

void SkyVisibilityGrid::queueRefine(int cell) {
    isRefining[static_cast<std::size_t>(cell)] = 1;
    refining.push_back(cell);
    ++refiningCount;
}

void SkyVisibilityGrid::keepBaked(std::span<const int> batch, bool quick) {
    for (const int cell : batch) {
        const int c = probeCascadeHolding(cascades, cell);
        const ProbeCascade& cascade = cascades[static_cast<std::size_t>(c)];
        store.keep(c, probeLatticeCoord(cascade, cell - cascade.firstProbe), cells[static_cast<std::size_t>(cell)], quick);
    }
}

/*
* Each job bakes its points into their own entries, so the result is the same on any number of threads; each thread
* walks the level's grid with marks of its own.
*/
void SkyVisibilityGrid::bakeCells(const StaticLevel& level, JobSystem& jobs, std::span<const int> batch, int rays) {
    jobs.parallelFor(batch.size(), pointsPerJob, [&](std::size_t index) {
        thread_local GridMarks marks;
        const int cell = batch[index];
        const ProbeCascade& cascade = cascades[static_cast<std::size_t>(probeCascadeHolding(cascades, cell))];
        const Vec3 position = probePosition(cascade.volume, cell - cascade.firstProbe);
        cells[static_cast<std::size_t>(cell)] = bakeSkyCell(level, position, rays, shape.rayLength, cascade.volume.spacing.x, marks);
    });
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

int SkyVisibilityGrid::bake(const StaticLevel& level, JobSystem& jobs, int maxCells, bool quick, const Vec3& forward) {
    const int count = std::min(maxCells, getWaiting());

    if (count <= 0) {
        return 0;
    }

    thread_local std::vector<int> batch;
    batch.clear();

    for (std::size_t c = waiting.size(); c-- > 0 && static_cast<int>(batch.size()) < count;) {
        takeWaiting(c, static_cast<std::size_t>(count) - batch.size(), forward, batch);
    }

    const bool refines = quick && shape.quickRays < shape.rays;
    bakeCells(level, jobs, batch, refines ? shape.quickRays : shape.rays);

    for (const int cell : batch) {
        isWaiting[static_cast<std::size_t>(cell)] = NotListed;

        if (refines) {
            queueRefine(cell);
        }
    }

    keepBaked(batch, refines);
    waitingCount -= static_cast<int>(batch.size());
    ++version;
    return static_cast<int>(batch.size());
}

/*
* The queue is walked from its front, skipping points a move took out of it, until the batch is full; those taken are
* baked at full rays, leave it, and replace their quick bake in the store.
*/
int SkyVisibilityGrid::refine(const StaticLevel& level, JobSystem& jobs, int maxCells) {
    thread_local std::vector<int> batch;
    batch.clear();

    while (nextRefining < refining.size() && static_cast<int>(batch.size()) < maxCells) {
        const int cell = refining[nextRefining++];

        if (isRefining[static_cast<std::size_t>(cell)] != 0) {
            batch.push_back(cell);
        }
    }

    if (nextRefining * 2 > refining.size()) {
        refining.erase(refining.begin(), refining.begin() + static_cast<std::ptrdiff_t>(nextRefining));
        nextRefining = 0;
    }

    if (batch.empty()) {
        return 0;
    }

    bakeCells(level, jobs, batch, shape.rays);

    for (const int cell : batch) {
        isRefining[static_cast<std::size_t>(cell)] = 0;
    }

    keepBaked(batch, false);
    refiningCount -= static_cast<int>(batch.size());
    ++version;
    return static_cast<int>(batch.size());
}

int SkyVisibilityGrid::getWaiting() const {
    return waitingCount;
}

int SkyVisibilityGrid::getRefining() const {
    return refiningCount;
}

std::span<const ProbeCascade> SkyVisibilityGrid::getCascades() const {
    return cascades;
}

std::span<const SkyCell> SkyVisibilityGrid::getCells() const {
    return cells;
}

std::uint64_t SkyVisibilityGrid::getVersion() const {
    return version;
}
