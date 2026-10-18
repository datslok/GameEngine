#pragma once

#include "core/profiler.h"
#include "ecs/world.h"
#include "render/render_stats.h"
#include "scene/debug_draw.h"
#include "scene/frame_description.h"
#include "scene/model.h"

#include <string>
#include <vector>

/*
* Debug views the engine can draw for any game. A game switches them on (Game::wantsDebugView); the engine knows
* the meshes and the renderer's counts, so it does the drawing.
*/

// A box around every draw in the frame, using its mesh's bounds (indexed like MeshHandle) and the draw's model matrix.
// Draws whose mesh has no known bounds are skipped.
void drawBoundingBoxes(const std::vector<DrawItem>& draws, const std::vector<Aabb>& meshBounds, DebugDraw& debug);

// The debug readout is lines of text down the top-left corner; this is where line `line` (0 first) starts, so a game
// can put its own lines below the engine's (drawRenderStats uses lines 0 to 3).
constexpr float debugReadoutTextHeight = 14.0f;
Vec2 debugReadoutLine(int line);

// The culling readout in the top-left corner: what the camera and the shadow views drew and skipped last frame.
void drawRenderStats(const RenderStats& stats, DebugDraw& debug);

// The readout line drawRenderStats leaves for the engine's timings (drawProfile).
constexpr int engineProfileReadoutLine = 4;

// The readout line for the GPU's timings per pass (a profiler from makeGpuProfiler).
constexpr int gpuProfileReadoutLine = 5;

// The engine's readout uses lines 0 to 5; a game's own lines start here.
constexpr int firstGameReadoutLine = 6;

// A profiler for the GPU's timings: "GPU" (the whole frame) as section 0, then one section per pass in GpuPass order.
Profiler makeGpuProfiler();

// One frame's GPU timings into a profiler from makeGpuProfiler. Timings that are not valid are left out, so a frame
// not measured does not count as one that took no time.
void recordGpuTimings(Profiler& profiler, const GpuTimings& timings);

// A profiler's numbers as one line of text: each section's name and its average per call, then "/" and the worst call
// if withWorst, in milliseconds with that many decimals, then "MS".
std::string profileText(const Profiler& profiler, int decimals, bool withWorst);

// profileText drawn on one readout line.
void drawProfile(const Profiler& profiler, int line, int decimals, bool withWorst, const Vec3& colour, DebugDraw& debug);

// Timings are drawn in this colour (linear pale green), apart from the rest of the readout.
const Vec3 profileDebugColour{0.45f, 1.0f, 0.55f};


// Collider wireframes are this colour (linear orange), apart from anything a game highlights itself.
const Vec3 colliderDebugColour{1.0f, 0.45f, 0.05f};

// Every collider in the world, drawn as the shape a query tests, at its entity's pose interpolated like the meshes (alpha).
void drawColliders(const World& world, float alpha, DebugDraw& debug);
