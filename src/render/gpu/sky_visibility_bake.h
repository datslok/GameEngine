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

/*
* The grid round the camera and its baking. follow moves the cascades with the camera; points a move brings in, or
* every point when the level or the grid's shape changed, wait (left out when read) until bake reaches them, densest
* cascade first, in the order they came. Render-only: nothing here touches the simulation.
*/
class SkyVisibilityGrid {
public:
    /*
    * Places the cascades round centre for these settings and this level (levelVersion changes whenever it does). Returns
    * true when it started afresh: every point waiting (the first call, a new level or a new shape).
    */
    bool follow(const Vec3& centre, const SkyVisibilitySettings& settings, std::uint64_t levelVersion);

    // Bakes up to maxCells waiting points against level, a job per point on jobs. Returns how many it baked.
    int bake(const StaticLevel& level, JobSystem& jobs, int maxCells);

    int getWaiting() const;
    std::span<const ProbeCascade> getCascades() const;

    // Every cascade's points by number (each cascade's from its firstProbe, in slot order).
    std::span<const SkyCell> getCells() const;

    // Changes whenever a point does: what the GPU's copy is compared with.
    std::uint64_t getVersion() const;

private:
    void startAfresh();
    void wait(int cell);

    SkyVisibilitySettings shape;
    std::uint64_t levelVersion = 0;
    bool started = false;
    std::vector<ProbeCascade> cascades;
    std::vector<SkyCell> cells;

    // The points waiting, in the order they came (those before nextWaiting are done), and which are.
    std::vector<int> waiting;
    std::vector<std::uint8_t> isWaiting;
    std::size_t nextWaiting = 0;
    std::uint64_t version = 0;
};
