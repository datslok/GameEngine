#pragma once

#include "core/profiler.h"
#include "ecs/world.h"
#include "render/render_stats.h"
#include "render/gpu/video_memory.h"
#include "scene/camera.h"
#include "scene/debug_draw.h"
#include "scene/frame_description.h"
#include "scene/model.h"

#include <optional>
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
// can put its own lines below the engine's (drawRenderStats uses lines 0 to 6).
constexpr float debugReadoutTextHeight = 14.0f;
Vec2 debugReadoutLine(int line);

// The culling readout in the top-left corner: what the camera and the shadow views drew and skipped last frame.
void drawRenderStats(const RenderStats& stats, DebugDraw& debug);

// The readout lines drawRenderStats writes the light probes' states (probeStatsText), the video memory
// (videoMemoryText) and the eye's exposure (exposureText) on.
constexpr int probeReadoutLine = 4;
constexpr int videoMemoryReadoutLine = 5;
constexpr int exposureReadoutLine = 6;

// The readout line drawCameraReadout writes the camera's place and direction on.
constexpr int cameraReadoutLine = 7;

// The readout line drawRenderStats leaves for the engine's timings (drawProfile).
constexpr int engineProfileReadoutLine = 8;

// The readout line for the GPU's timings per pass (a profiler from makeGpuProfiler).
constexpr int gpuProfileReadoutLine = 9;

// The engine's readout uses lines 0 to 9; a game's own lines start here.
constexpr int firstGameReadoutLine = 10;

// The light probes' line: how many, traced (awake ones among them), asleep, off and moved by relocation, or "PROBES -"
// without probes.
std::string probeStatsText(const ProbeStateCounts& counts);

// The video memory line: "VRAM used / budget MB  ENGINE n MB" (megabytes of 2^20 bytes), or the engine's share alone
// when the driver gives no budget, or "VRAM -" before anything was measured.
std::string videoMemoryText(const VideoMemory& memory);

// The eye adaptation's exposure and the scene's measured brightness in stops, "EXPOSURE +2.3 EV  SCENE -5.1 EV", or
// "EXPOSURE FIXED" while it does not adapt.
std::string exposureText(const std::optional<GpuEyeAdaptation>& adaptation);

// The upscaler drawing the frame and the render scale it draws at, after the exposure on its line.
std::string upscalerText(Upscaler upscaler, float renderScale);

// The camera's line: its position in metres and the unit direction it looks, "CAMERA -10.0 1.7 9.0  LOOK -0.98 -0.05 0.20",
// for setting up the same view elsewhere (a golden image's).
std::string cameraText(const Vec3& position, const Vec3& forward);
void drawCameraReadout(const Camera& camera, DebugDraw& debug);

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
