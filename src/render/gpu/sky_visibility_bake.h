#pragma once

#include "core/job_system.h"
#include "physics/static_level.h"
#include "render/gpu/sky_visibility.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <span>
#include <unordered_map>
#include <string>
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
*/
SkyCell bakeSkyCell(const StaticLevel& level, const Vec3& point, int rays, float rayLength, float spacing, GridMarks& marks,
                    std::span<const Vec3> albedo = {}, const SkyHitSampler& hitSampler = {});

// How many points fit a budget, from the time one took last: at least one, at most what is waiting.
int skyCellsForBudget(double budgetSeconds, double secondsPerCell, int waiting);

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

// A point the store kept: as the GPU reads it (packed and unpacked again, so it packs to the very same words), and
// whether it is still to be baked again (baked at the quick rays, or before the grid round its hits was).
struct StoredSkyCell {
    SkyCell cell;
    bool unfinished = false;
};

// About what one kept point costs: its key, its packed point and flag, and the list and map round them.
inline constexpr std::size_t skyStoreBytesPerPoint = 128;

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
    void keep(int cascade, const ProbeGridCoord& place, const SkyCell& cell, bool unfinished);

    // The point kept at a place, marked used now; none when not kept.
    std::optional<StoredSkyCell> find(int cascade, const ProbeGridCoord& place);

    // Marks the point at a place, if kept, used now.
    void touch(int cascade, const ProbeGridCoord& place);

    std::size_t size() const;

private:
    struct Entry {
        std::uint64_t key = 0;
        GpuSkyCell packed{};
        bool unfinished = false;
    };

    // The most recently used at the front.
    std::list<Entry> used;
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> places;
    std::size_t capacity = 0;
};

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

    int getWaiting() const;

    // How many quickly baked points wait to be baked again at the full rays.
    int getRefining() const;
    std::span<const ProbeCascade> getCascades() const;

    // Every cascade's points by number (each cascade's from its firstProbe, in slot order).
    std::span<const SkyCell> getCells() const;

    // Changes whenever a point does: what the GPU's copy is compared with.
    std::uint64_t getVersion() const;

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

    // Bakes these points at rays rays, each into its own entry, their hits lit from the grid as it stood before.
    void bakeCells(const StaticLevel& level, JobSystem& jobs, std::span<const int> batch, int rays, std::span<const Vec3> albedo);

    // The batch's results, written while the rest of the grid is read, then put in place.
    std::vector<SkyCell> baked;

    // Moves up to count of cascade's waiting points into batch, the first by bake's order.
    void takeWaiting(std::size_t cascade, std::size_t count, const Vec3& forward, std::vector<int>& batch);

    // A point a move brought into its slot: the store's, if it kept that place (back to refinement if unfinished), else waiting.
    void arrive(std::size_t cascade, int cell, const ProbeCascade& now);

    // Queues a point for refinement.
    void queueRefine(int cell);

    // Keeps these just-baked points in the store, at their places in the cascades as they stand.
    void keepBaked(int cell, bool unfinished);

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

    // The quickly baked points, in the order they were baked (those before nextRefining are done), and which still are:
    // a point a move clears leaves the queue by its flag alone.
    std::vector<int> refining;
    std::vector<std::uint8_t> isRefining;
    std::size_t nextRefining = 0;
    int refiningCount = 0;

    std::uint64_t version = 0;
};
