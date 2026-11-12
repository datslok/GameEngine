#include "render/gpu/sky_visibility_bake.h"

#include "core/profiler.h"
#include "render/gpu/ddgi.h"

#include <algorithm>
#include <cmath>
#include <format>
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
}

Vec3 skyHitLight(const SkySample& sample, const Vec3& normal, float fallback) {
    if (sample.known < skyHitKnownShare) {
        return Vec3{fallback, fallback, fallback};
    }

    return skyLightFacing(sample.visibility, sample.bounce, normal, 0.0f);
}

/*
* The rays' directions, openings and front hits go into this thread's own scratch lists, so a point allocates nothing
* once the first has grown them, and the bands come from skyVisibilityFromRays, as the tests' do. The hits are lit after
* every ray is cast, since their fallback is the point's own open share.
*/
SkyCell bakeSkyCell(const StaticLevel& level, const Vec3& point, int rays, float rayLength, float spacing, GridMarks& marks, std::span<const Vec3> albedo,
                    const SkyHitSampler& hitSampler) {
    thread_local std::vector<Vec3> directions;
    thread_local std::vector<float> openings;
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

    // Each channel's values in turn: zero for every ray, then each front hit's albedo times the light on it.
    if (!albedo.empty() && hitSampler) {
        thread_local std::vector<Vec3> hitLight;
        hitLight.clear();
        const float fallback = std::clamp(2.0f * cell.visibility.open, 0.0f, 1.0f);
        int unsure = 0;

        for (const FrontHit& hit : frontHits) {
            const SkySample sample = hitSampler(hit.point, hit.normal, directions[hit.ray] * -1.0f);
            hitLight.push_back(skyHitLight(sample, hit.normal, fallback));
            unsure += sample.known < skyHitKnownShare && sample.missing >= skyHitKnownShare ? 1 : 0;
        }

        cell.unsureShare = frontHits.empty() ? 0.0f : static_cast<float>(unsure) / static_cast<float>(frontHits.size());

        for (std::size_t channel = 0; channel < cell.bounce.size(); ++channel) {
            bounced.assign(directions.size(), 0.0f);

            for (std::size_t index = 0; index < frontHits.size(); ++index) {
                const FrontHit& hit = frontHits[index];
                const Vec3 rho = hit.triangle < albedo.size() ? albedo[hit.triangle] : Vec3{0.0f, 0.0f, 0.0f};
                const float channels[3] = {rho.x * hitLight[index].x, rho.y * hitLight[index].y, rho.z * hitLight[index].z};
                bounced[hit.ray] = channels[channel];
            }

            cell.bounce[channel] = skyVisibilityFromRays(directions, bounced);
        }
    }

    for (int direction = 0; direction < skyReachDirections; ++direction) {
        const std::optional<LevelHit> hit = raycast(level, Ray{point, skyReachDirection(direction)}, spacing, marks);
        cell.reach[static_cast<std::size_t>(direction)] = hit ? std::min(hit->hit.distance / spacing, 1.0f) : 1.0f;
    }

    return cell;
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

void SkyCellStore::keep(int cascade, const ProbeGridCoord& place, const SkyCell& cell, SkyBakeStage next) {
    const std::optional<std::uint64_t> key = storeKey(cascade, place);

    if (capacity == 0 || !key) {
        return;
    }

    const auto found = places.find(*key);

    if (found != places.end()) {
        found->second->packed = packSkyCell(cell);
        found->second->next = next;
        used.splice(used.begin(), used, found->second);
        return;
    }

    used.push_front(Entry{*key, packSkyCell(cell), next});
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
    return StoredSkyCell{unpackSkyCell(found->second->packed), found->second->next};
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

    queueNext(cell, found->next);
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
        batch.results[index] = bakeSkyCell(level, entry.position, batch.rays, batch.rayLength, entry.spacing, marks, albedo, gridAtHits);
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

        store.keep(entry.cascade, entry.place, result, next);

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
            text += std::format("\n  ({:.2f} {:.2f} {:.2f}) {}{}{}{}  open {:.4f}  escaped {}  unsure {:.2f}  bounce {:.4f} {:.4f} {:.4f}", position.x, position.y,
                                position.z, states[static_cast<std::size_t>(sky.state)], isWaiting[index] == Waiting ? ", waiting" : "",
                                refining.contains(slotIndex) ? ", quick" : "", deepening.contains(slotIndex) ? ", deep" : "", sky.visibility.open,
                                sky.escapedRays, sky.unsureShare, sky.bounce[0].open, sky.bounce[1].open, sky.bounce[2].open);
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