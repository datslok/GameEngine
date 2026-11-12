#pragma once

#include "core/job_system.h"
#include "math/aabb.h"
#include "physics/static_level.h"
#include "render/gpu/sky_visibility.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

/*
* Baking the sky visibility grid (render/gpu/sky_visibility.h) on the CPU: rays from each point against the static
* level's triangles, through its uniform grid. GPUs without ray tracing cannot cast rays, and a tracer of its own in a
* compute shader is phase 14's software tracer; StaticLevel's raycast already exists and is tested.
*/

// What the grid says round a ray's hit: the point, its unit normal and the unit way back to the ray's start.
using SkyHitSampler = std::function<SkySample(const Vec3& point, const Vec3& normal, const Vec3& toViewer)>;

// Below this share of the grid known round a hit, the bake guesses the light on it instead.
inline constexpr float skyHitKnownShare = 0.5f;

// A point more than this share of whose front hits were guessed because the grid round them was not baked yet
// (SkyCell::unsureShare) is baked again once, as a quick one is.
inline constexpr float skyUnsureRefineShare = 0.1f;

/*
* The light falling on a hit, per unit of the open sky's light and per colour channel: the sky it sees facing its
* normal plus the bounce there (so light keeps bouncing as the grid fills), from the grid round it; where the grid
* knows less than skyHitKnownShare of it, fallback grey instead.
*/
Vec3 skyHitLight(const SkySample& sample, const Vec3& normal, float fallback);

/*
* The sun's bounced light falling on a hit, per unit of its irradiance and per colour channel: the sun's bounce the grid
* round it holds facing its normal (the second and later bounces, so the sun's light keeps bouncing as the grid
* refreshes; the first comes from the sunlit spots, SunEmitters); none where the grid knows less than skyHitKnownShare.
*/
Vec3 skySunHitLight(const SkySample& sample, const Vec3& normal);

// How far a ray to or from a sunlit spot stays off its surface, along the surface's normal (metres), so it does not
// meet the surface itself.
inline constexpr float skySunRayLift = 0.01f;

/*
* The sun's first bounce, gathered by aiming at the sunlit surfaces rather than meeting them by chance (next event
* estimation). A small sunlit patch seen through a door is a small, bright source: random rays from a point meet it a
* handful of times even at 2,048, and neighbouring points disagreed (warm blotches). So the sunlit surfaces round the
* grid are found first, as spots: rays down the sun's beam spacing apart, each first hit facing the sun a spot (sunlit
* by construction), standing for spacing^2 / (n . L) of surface, its radiance albedo (n . L) E / pi. Seen from a point d
* away at cos_out to its normal it fills spacing^2 cos_out / (n . L) / d^2 of solid angle, so it brings albedo
* spacing^2 cos_out / d^2 per unit of E / pi: n . L cancels (it is in how many spots a surface gets).
*/
struct SunEmitter {
    Vec3 position;
    Vec3 normal;
    Vec3 albedo;
};

// The spots in a cell of skySunCellSize: where their light is (flux-weighted), which way they face on the whole, their
// light (the brightest albedo channel times spacing^2, summed) and their run in SunEmitters::emitters.
struct SunEmitterCell {
    Vec3 centroid;
    Vec3 normal;
    float flux = 0.0f;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

struct SunEmitters {
    Vec3 toSun;
    float spacing = 0.0f;
    Aabb region;
    std::vector<SunEmitter> emitters; // in their cells' order
    std::vector<SunEmitterCell> cells;
};

// The cells' side (metres), how far past the grid's coarsest cascade spots are found, and how far the grid moves
// before they are found again round it.
inline constexpr float skySunCellSize = 2.0f;
inline constexpr float skySunEmitterMargin = 8.0f;
inline constexpr float skySunEmitterRebuild = 8.0f;

// How far beyond the region a ray down the beam starts, so blockers above it still shade it (metres).
inline constexpr float skySunOccluderReach = 30.0f;

// A cell the point's own rays show a surface in front of is still sampled, this much less often (never 0, so the
// estimate stays unbiased where the rays were wrong).
inline constexpr float skySunOccludedImportance = 0.05f;

/*
* A cell's chance of being sampled: half its share of the plain importance (its light over its distance squared,
* facing the point), half its share of that cut by the rays' guess at what is hidden (a defensive mixture). The point's
* 256 rays cannot resolve a door 5 m off, so cells seen through one were guessed hidden, sampled twenty times less
* often, and each sample that got through counted twenty times: green spikes from the grass outside. With half the
* chance plain, no cell is sampled less than half as often as without the guess.
*/
float skySunCellChance(float plain, float plainTotal, float guessed, float guessedTotal);

/*
* The spots in region for the sun towards toSun (unit), spacing apart across its beam, each lit triangle's albedo (none
* without albedo); a row of rays a job, kept in order. Then grouped into cells, in the order of their cells' places.
*/
SunEmitters buildSunEmitters(const StaticLevel& level, std::span<const Vec3> albedo, const Vec3& toSun, const Aabb& region, float spacing, JobSystem& jobs);

/*
* One point: rays rays of rayLength (skyVisibilityRay), each open if it meets nothing; a ray meeting the back of a
* triangle (the side its winding faces away from) counts as blocked and as a vote that the point is inside a wall, which
* it is when more than skyVisibilityBuriedShare of them do. Then its reach along each axis direction: the nearest hit
* within a spacing, as a share of it (1 for none). marks is this thread's own mailbox (many threads bake at once).
*
* With each triangle's albedo (in the level's triangle order) and the grid round the hits (hitSampler), each ray that
* meets the front of a triangle also brings back that triangle's albedo times the light on its hit (skyHitLight, its
* fallback the point's own guess, twice its open share within 0..1: under an open sky a point sees half the sphere and
* its ground all of it), each colour channel's values made into bands as the sky's are: the cell's bounce. Without
* them, no bounce.
*
* With the sunlit spots too (sunEmitters), the cell's sun bounce, per unit of the sun's irradiance: sunSamples shadow
* rays at spots, each cell chosen in proportion to its light over its distance squared, facing the point, and far less
* often where the point's own rays met a surface before it; a spot in it at random; each sample its spot's light over
* its chance, made into bands as a ray's is. Plus the second bounce from the rays' hits (skySunHitLight). Without
* spots, none.
*/
SkyCell bakeSkyCell(const StaticLevel& level, const Vec3& point, int rays, float rayLength, float spacing, GridMarks& marks,
                    std::span<const Vec3> albedo = {}, const SkyHitSampler& hitSampler = {}, const SunEmitters* sunEmitters = nullptr,
                    int sunSamples = 64);

// A job takes this many points at once, so handing them out costs little beside their rays.
inline constexpr std::size_t skyBakePointsPerJob = 8;

// How long one background batch bakes for, about (seconds): long enough that handing it over costs little, short enough
// that results come in steadily and a move's new points wait little behind it.
inline constexpr double skyBackgroundBatchSeconds = 0.03;

// The smallest batch worth baking on threads workers: a round of jobs for each (so the time a point takes is measured
// as when the batch is spread over every core).
int skyMinimumBatch(unsigned threads);

// The workers the background bake's job system gets for a share of hardwareThreads: at least one.
unsigned skyBackgroundWorkers(float share, unsigned hardwareThreads);

/*
* How many points fit a budget, from the time one took last: at least minimumBatch (and one), at most what is waiting.
* A batch smaller than a round of jobs for every core runs on fewer cores and measures a point dearer than it is, which
* would shrink the next budget in turn until the bake crawled at a point a frame.
*/
int skyCellsForBudget(double budgetSeconds, double secondsPerCell, int waiting, int minimumBatch = 1);

// What one frame bakes: waiting points (quickly, at the quick rays, or at the full rays), then points to refine.
struct SkyBakePlan {
    int waiting = 0;
    bool quick = false;
    int refine = 0;
};

/*
* everything (a capture, whose picture must not depend on what came before): every waiting point and every one to
* refine, at full rays. afresh (the first fill, a new level or shape, which has nothing at all to read): every waiting
* point at once, quickly when quickFewer (the quick rays are fewer than the rays). Otherwise never more than a frame's
* budget, however far a move left it behind, so moving never freezes the frame: while budgetCells (at full rays)
* covers what waits, the waiting points and refinement with what is left; past it, quickBudgetCells at the quick rays
* when quickFewer (budgetCells at full rays when not). Points behind read the coarser cascade, or the open sky.
*/
SkyBakePlan planSkyBake(int waiting, int refining, bool afresh, bool everything, int budgetCells, int quickBudgetCells, bool quickFewer);

/*
* Whether a point baked at the full rays gets the deep rays as well (SkyVisibilitySettings::deepRays): it was baked, saw
* the sky through at least one ray but fewer than deepOpenRays, and the deep rays are more than the rays.
*/
bool skyWantsDeepRays(const SkyCell& cell, const SkyVisibilitySettings& settings);

// A batch's measured time a point (secondsPerCell, at rays rays) as it would be at fullRays: in proportion (0 for no rays).
double skySecondsAtFullRays(double secondsPerCell, int rays, int fullRays);

// What a baked point still needs: nothing, baking again at the full rays (baked at the quick rays, or before the grid
// round its hits was), or the deep rays (skyWantsDeepRays).
enum class SkyBakeStage : std::uint8_t { Finished, Refine, Deepen };

// A point the store kept: as the GPU reads it (packed and unpacked again, so it packs to the very same words), what
// it still needs, and which of the grid's suns it was baked with (SkyVisibilityGrid::followSun).
struct StoredSkyCell {
    SkyCell cell;
    SkyBakeStage next = SkyBakeStage::Finished;
    std::uint32_t sunEpoch = 0;
};

// About what one kept point costs: its key, its packed point and flag, and the list and map round them.
inline constexpr std::size_t skyStoreBytesPerPoint = 144;

// The points the settings' store holds at most (storeMegabytes over skyStoreBytesPerPoint).
std::size_t skyStoreCapacity(const SkyVisibilitySettings& settings);

/*
* Baked points kept by their place in the world, a cascade and its lattice coordinate, so coming back to a place finds
* them instead of baking them again: a point depends only on the level and its place, so a kept one is exact. Past
* its capacity the point used longest ago goes first (a list in order of use, a map to its entries; each step O(1)).
* Places past +-2^19 lattice steps on any axis (about 260 km at 0.5 m) are not kept.
*/
class SkyCellStore {
public:
    void setCapacity(std::size_t points);
    void clear();

    // Keeps (or replaces) the point at a place, as used now.
    void keep(int cascade, const ProbeGridCoord& place, const SkyCell& cell, SkyBakeStage next, std::uint32_t sunEpoch = 0);

    // The point kept at a place, marked used now; none when not kept.
    std::optional<StoredSkyCell> find(int cascade, const ProbeGridCoord& place);

    // Marks the point at a place, if kept, used now.
    void touch(int cascade, const ProbeGridCoord& place);

    std::size_t size() const;

private:
    struct Entry {
        std::uint64_t key = 0;
        GpuSkyCell packed{};
        SkyBakeStage next = SkyBakeStage::Finished;
        std::uint32_t sunEpoch = 0;
    };

    // The most recently used at the front.
    std::list<Entry> used;
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> places;
    std::size_t capacity = 0;
};

/*
* Points taken from the grid to bake apart from it (SkyVisibilityGrid::takeBatch, takeRefineBatch), on another thread
* while the grid follows the camera on, and applied when done (applyBatch). Each entry keeps its place in the world, so
* a result whose slot a move gave to another place goes to the store instead. The grid as it stood when taken lights
* the rays' hits, so a batch reads nothing the grid changes meanwhile.
*/
struct SkyBakeBatch {
    struct Entry {
        int cell = 0;
        int cascade = 0;
        ProbeGridCoord place;
        Vec3 position;
        float spacing = 0.0f;
    };

    std::vector<Entry> entries;
    int rays = 0;
    float rayLength = 0.0f;
    float lookupBias = 0.0f;

    // Quick: baked at the quick rays, to be baked again. Refining: taken from the refinement queue. Deepening: from the
    // deep queue, at the deep rays.
    bool quick = false;
    bool refining = false;
    bool deepening = false;

    // Which fresh start of the grid it was taken in: one taken before a new level or shape is thrown away.
    std::uint64_t generation = 0;

    // The way to the sun its points' sun bounce is baked for (none: no sun bounce), and which of the grid's suns that is:
    // a point baked for an older one is placed and baked again.
    std::optional<Vec3> toSun;
    std::uint32_t sunEpoch = 0;

    // The grid's sunlit spots (none: no sun bounce) and the shadow rays a point aims at them.
    std::shared_ptr<const SunEmitters> sunEmitters;
    int sunSamples = 0;

    // The grid as it stood, for the hits.
    std::vector<ProbeCascade> cascades;
    std::shared_ptr<const std::vector<SkyCell>> grid;

    // Filled by bakeSkyBatch: each entry's point, and the time a point took at the full rays (for the next batch's size).
    std::vector<SkyCell> results;
    double secondsPerCell = 0.0;
};

/*
* Bakes a batch's points against level (a job per few points on jobs), their bounce from albedo (none when empty), the
* hits lit from the grid as it stood when taken. Safe on any thread: it touches only the batch.
*/
void bakeSkyBatch(const StaticLevel& level, JobSystem& jobs, std::span<const Vec3> albedo, SkyBakeBatch& batch);

/*
* The grid round the camera and its baking. follow moves the cascades with the camera; points a move brings in, or
* every point when the level or the grid's shape changed, wait (left out when read) until bake reaches them: the
* coarsest cascade first, and within one the points ahead first, nearest first. Render-only: nothing here touches the
* simulation.
*/
class SkyVisibilityGrid {
public:
    /*
    * Places the cascades round centre for these settings and this level (levelVersion changes whenever it does). Returns
    * true when it started afresh: every point waiting (the first call, a new level or a new shape).
    */
    bool follow(const Vec3& centre, const SkyVisibilitySettings& settings, std::uint64_t levelVersion);

    /*
    * The way to the light the sun bounce follows (the brightest directional light; none without one), after follow:
    * none while the settings' sun bounce is off. When it appears, goes, or moves past the settings' sunRefreshDegrees
    * from the one the grid bakes with, the grid takes it and every baked point is queued to be baked again at the full
    * rays (the refinement queue), and the store's points come back queued. Returns whether it did.
    */
    bool followSun(const std::optional<Vec3>& toSun);

    /*
    * The sunlit spots for the grid's sun (buildSunEmitters), round the coarsest cascade and skySunEmitterMargin past it,
    * at the settings' spacing: found again after a sun refresh, a change of spacing, or once the grid has moved
    * skySunEmitterRebuild from where they were found (points baked meanwhile keep what they had); none without a sun.
    * The batches taken share them.
    */
    void prepareSunEmitters(const StaticLevel& level, std::span<const Vec3> albedo, JobSystem& jobs);
    std::shared_ptr<const SunEmitters> getSunEmitters() const;

    /*
    * Bakes up to maxCells waiting points against level, a job per point on jobs. Returns how many it baked. quick bakes
    * them at the settings' quick rays and queues them for refine, when those are fewer than the rays.
    *
    * Which first, when not all fit: the coarsest cascade's (a coarse point covers eight times the space a point a level
    * denser does, so falling behind costs least there, and a missing dense point reads the coarser one); within a
    * cascade, the points ahead (in front of the camera, along forward, or ahead of the last move, either one; all of
    * them when neither is known), then the rest, nearest the camera first in each.
    *
    * albedo (each of the level's triangles', in its order) gives the points their bounce, its hits lit from the grid as
    * it stood before this bake (bakeSkyCell); none without it.
    */
    int bake(const StaticLevel& level, JobSystem& jobs, int maxCells, bool quick = false, const Vec3& forward = Vec3{}, std::span<const Vec3> albedo = {});

    // Bakes up to maxCells quickly baked points again at the full rays, in the order they were baked, with their bounce
    // from albedo as bake gives it. Returns how many.
    int refine(const StaticLevel& level, JobSystem& jobs, int maxCells, std::span<const Vec3> albedo = {});

    // Bakes up to maxCells points queued for the deep rays (skyWantsDeepRays) at them, in the order they were queued,
    // after which they are finished. Returns how many.
    int deepen(const StaticLevel& level, JobSystem& jobs, int maxCells, std::span<const Vec3> albedo = {});

    /*
    * Takes up to maxCells waiting points, in bake's order, into a batch to bake apart (bakeSkyBatch) and apply later:
    * they wait no more meanwhile, and read as not baked. quick as bake's.
    */
    SkyBakeBatch takeBatch(int maxCells, bool quick, const Vec3& forward = Vec3{});

    // Takes up to maxCells quick points to bake again at the full rays, in the order refine takes them.
    SkyBakeBatch takeRefineBatch(int maxCells);

    // Takes up to maxCells points queued for the deep rays, in the order deepen takes them.
    SkyBakeBatch takeDeepBatch(int maxCells);

    /*
    * Puts a baked batch in place: each point into its slot while the slot still holds its place (a slot waiting there
    * again stops waiting), else into the store by its place; quick ones, or ones whose hits were mostly guessed, are
    * queued to be baked again; others baked at the full rays that want the deep rays are queued for them (a deep bake
    * is finished). Nothing when the grid started afresh since it was taken.
    */
    void applyBatch(const SkyBakeBatch& batch);

    int getWaiting() const;

    // How many quickly baked points wait to be baked again at the full rays.
    int getRefining() const;

    // How many points wait for the deep rays.
    int getDeepening() const;
    std::span<const ProbeCascade> getCascades() const;

    // Every cascade's points by number (each cascade's from its firstProbe, in slot order).
    std::span<const SkyCell> getCells() const;

    // Changes whenever a point does: what the GPU's copy is compared with.
    std::uint64_t getVersion() const;

    /*
    * The points changed since the last call, for the GPU's copy to repack only those: true when every point may have
    * (a fresh start), changed then left empty; otherwise false with changed holding the points (each at least once).
    */
    bool takeChanges(std::vector<int>& changed);

    /*
    * The points round a place in the world, as text for the sky-points command: how many wait and how many quick bakes
    * wait to be refined, then each cascade's eight points round it with their place, state (unknown, baked, buried),
    * whether they wait or are quick bakes still to refine, their open share, the share their hits were unsure of, and
    * their bounce's mean (red, green, blue).
    */
    std::string describePoints(const Vec3& place) const;

private:
    void startAfresh();
    void wait(int cell);

    // A batch of these points (the grid as it stands copied for its hits), to bake at rays rays.
    SkyBakeBatch makeBatch(std::span<const int> points, int rays, bool quick, bool refining, bool deepening) const;

    // Each fresh start counts one, so a batch taken before it is known to be stale.
    std::uint64_t generation = 0;

    // The way to the sun the grid bakes with, and each change of it counts one.
    std::optional<Vec3> sun;
    std::uint32_t sunEpoch = 0;

    // The sunlit spots, the sun they were found for, and where the coarsest cascade was then.
    std::shared_ptr<const SunEmitters> sunEmitters;
    std::uint32_t sunEmittersEpoch = 0;
    Vec3 sunEmittersCentre;

    // Moves up to count of cascade's waiting points into batch, the first by bake's order.
    void takeWaiting(std::size_t cascade, std::size_t count, const Vec3& forward, std::vector<int>& batch);

    // A point a move brought into its slot: the store's, if it kept that place (back to the queue of what it still
    // needs), else waiting.
    void arrive(std::size_t cascade, int cell, const ProbeCascade& now);

    // A point into the queue of what it still needs, out of the other (nothing for a finished one).
    void queueNext(int cell, SkyBakeStage next);

    /*
    * Points to bake again, in the order they were queued (those before next are done), and which still are: a point a
    * move clears leaves by its flag alone. Each point is in it once at most.
    */
    struct BakeQueue {
        std::vector<int> order;
        std::vector<std::uint8_t> queued;
        std::size_t next = 0;
        int count = 0;

        void reset(std::size_t cells);
        void push(int cell);
        void remove(int cell);
        bool contains(int cell) const;

        // Up to maxCells points from the front, which leave it.
        void take(int maxCells, std::vector<int>& points);
    };


    SkyCellStore store;

    SkyVisibilitySettings shape;
    std::uint64_t levelVersion = 0;
    bool started = false;
    std::vector<ProbeCascade> cascades;
    std::vector<SkyCell> cells;

    // Where the cascades were placed round last, and the last move (zero for none).
    Vec3 centre;
    Vec3 motion;

    // Each cascade's waiting points, and each point's state in them: not listed, waiting, or listed but found in the
    // store meanwhile (dropped from the list when it is next walked).
    enum WaitState : std::uint8_t { NotListed = 0, Waiting = 1, Found = 2 };
    std::vector<std::vector<int>> waiting;
    std::vector<std::uint8_t> isWaiting;
    int waitingCount = 0;

    // The points to bake again at the full rays (quick ones, and ones baked before the grid round their hits was), and
    // those to bake at the deep rays.
    BakeQueue refining;
    BakeQueue deepening;

    std::uint64_t version = 0;

    // What changed since takeChanges last ran: every point, or these.
    bool allChanged = true;
    std::vector<int> changedCells;
};

/*
* Bakes batches on a thread of its own, its work spread over a job system's workers, so the grid fills as fast as the
* cores allow without the frame waiting: the frame starts a batch when none is baking and applies it when done
* (SkyVisibilityGrid::applyBatch). The level and the albedo are kept alive until the batch is done. The thread is
* joined when the baker goes, after its batch finishes.
*
* Give it a job system of its own, not the frame's: a thread outside a job system shares one pile with every other such
* thread, so the frame, waiting for its own jobs, would run the bake's meanwhile (a millisecond or more each), and its
* frames grew from 25 to 37 ms while baking. threadStart runs first on its thread (to lower its priority, as its job
* system's workers').
*/
class SkyBackgroundBaker {
public:
    explicit SkyBackgroundBaker(JobSystem& jobs, std::function<void()> threadStart = {});
    ~SkyBackgroundBaker();

    SkyBackgroundBaker(const SkyBackgroundBaker&) = delete;
    SkyBackgroundBaker& operator=(const SkyBackgroundBaker&) = delete;

    // A batch started and not taken back yet (baking, or done).
    bool busy() const;

    // Starts baking a batch (when not busy; an empty one is done at once).
    void start(SkyBakeBatch batch, std::shared_ptr<const StaticLevel> level, std::shared_ptr<const std::vector<Vec3>> albedo);

    // The batch, once done; none while it bakes or none was started.
    std::optional<SkyBakeBatch> takeFinished();

    // Waits until the batch started is done (at once when none is baking).
    void waitFinished();

private:
    JobSystem& jobs;
    mutable std::mutex lock;
    std::condition_variable changed;
    bool stopping = false;
    bool started = false;
    bool finished = false;
    SkyBakeBatch batch;
    std::shared_ptr<const StaticLevel> level;
    std::shared_ptr<const std::vector<Vec3>> albedo;

    // Last, so everything it reads is set up before it starts.
    std::thread thread;

    void run();
};
