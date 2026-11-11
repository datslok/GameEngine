#pragma once

#include "core/job_system.h"
#include "physics/static_level.h"
#include "render/gpu/sky_visibility.h"

#include <cstdint>
#include <span>
#include <vector>

/*
* Baking the sky visibility grid (render/gpu/sky_visibility.h) on the CPU: rays from each point against the static
* level's triangles, through its uniform grid. GPUs without ray tracing cannot cast rays, and a tracer of its own in a
* compute shader is phase 14's software tracer; StaticLevel's raycast already exists and is tested.
*/

/*
* One point: rays rays of rayLength (skyVisibilityRay), each open if it meets nothing; a ray meeting the back of a
* triangle (the side its winding faces away from) counts as blocked and as a vote that the point is inside a wall, which
* it is when more than skyVisibilityBuriedShare of them do. Then its reach along each axis direction: the nearest hit
* within a spacing, as a share of it (1 for none). marks is this thread's own mailbox (many threads bake at once).
*/
SkyCell bakeSkyCell(const StaticLevel& level, const Vec3& point, int rays, float rayLength, float spacing, GridMarks& marks);

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
    */
    int bake(const StaticLevel& level, JobSystem& jobs, int maxCells, bool quick = false, const Vec3& forward = Vec3{});

    // Bakes up to maxCells quickly baked points again at the full rays, in the order they were baked. Returns how many.
    int refine(const StaticLevel& level, JobSystem& jobs, int maxCells);

    int getWaiting() const;

    // How many quickly baked points wait to be baked again at the full rays.
    int getRefining() const;
    std::span<const ProbeCascade> getCascades() const;

    // Every cascade's points by number (each cascade's from its firstProbe, in slot order).
    std::span<const SkyCell> getCells() const;

    // Changes whenever a point does: what the GPU's copy is compared with.
    std::uint64_t getVersion() const;

private:
    void startAfresh();
    void wait(int cell);

    // Bakes these points at rays rays, each into its own entry.
    void bakeCells(const StaticLevel& level, JobSystem& jobs, std::span<const int> batch, int rays);

    // Moves up to count of cascade's waiting points into batch, the first by bake's order.
    void takeWaiting(std::size_t cascade, std::size_t count, const Vec3& forward, std::vector<int>& batch);

    SkyVisibilitySettings shape;
    std::uint64_t levelVersion = 0;
    bool started = false;
    std::vector<ProbeCascade> cascades;
    std::vector<SkyCell> cells;

    // Where the cascades were placed round last, and the last move (zero for none).
    Vec3 centre;
    Vec3 motion;

    // Each cascade's waiting points, and which points wait.
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
