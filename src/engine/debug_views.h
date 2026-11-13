#pragma once

#include "core/profiler.h"
#include "ecs/world.h"
#include "render/render_stats.h"
#include "render/gpu/video_memory.h"
#include "scene/camera.h"
#include "scene/debug_draw.h"
#include "scene/frame_description.h"
#include "scene/model.h"

#include <array>
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

// The debug readout is lines of text down the top-left corner; this is where line `line` (0 first) starts.
constexpr float debugReadoutTextHeight = 14.0f;
Vec2 debugReadoutLine(int line);

// The unit ending a timing readout ("... MAIN 0.91  MS"), repeated at the end of each line it wraps onto.
inline constexpr const char* readoutUnit = "MS";

/*
* A readout's text broken into lines of at most maxCharacters (0: no limit), only between its items (two spaces apart;
* one space is inside an item), never inside one: an item longer than the limit has a line to itself. A text ending in
* readoutUnit ends each of its lines with it.
*/
std::vector<std::string> wrapReadout(const std::string& text, std::size_t maxCharacters);

// How many readout characters fit across a screen that wide between the margins (at least 1), or 0 (no limit) for no width.
std::size_t readoutCharactersFitting(float screenWidth);

/*
* Text on the readout's next free lines (DebugDraw::takeReadoutLine), wrapped to the DebugDraw's screen width, so a
* long line never runs off the screen and what follows goes below it. The engine writes its lines first each frame,
* then the game's onDebugDraw writes its own after them.
*/
void writeReadout(DebugDraw& debug, const std::string& text, const Vec3& colour);

// The culling readout in the top-left corner: what the camera and the shadow views drew and skipped last frame, the
// lights, shadow tiles, probes, video memory and exposure. renderStatsLines is its text, a line each (also the stats
// command's), and drawRenderStats writes them.
std::vector<std::string> renderStatsLines(const RenderStats& stats);
void drawRenderStats(const RenderStats& stats, DebugDraw& debug);

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

// The first passes on the GPU readout's second and third lines: the screen's lighting from the prepass's depth
// (ambient occlusion's parts, contact shadows, mirrored lights, the main pass), then the picture's passes after it.
inline constexpr GpuPass firstLightingPass = GpuPass::ColourPyramid;
inline constexpr GpuPass firstPicturePass = GpuPass::ProbeBalls;

/*
* A profiler from makeGpuProfiler as the readout's three groups (each wrapped to the screen when drawn), in milliseconds to two
* decimals with the passes that took no time left out: the total and the scene's setup up to the depth prepass, the
* screen's lighting up to the main pass, then the picture's after it.
*/
std::array<std::string, 3> gpuProfileLines(const Profiler& profiler);

// gpuProfileLines written to the readout, each wrapped to the screen.
void drawGpuProfile(const Profiler& profiler, DebugDraw& debug);

// A profiler for the cloth's GPU time: "CLOTH" (all of it) as section 0, then one section per kind in ClothPass order.
Profiler makeClothProfiler();

// One frame's cloth timings into a profiler from makeClothProfiler; timings that are not valid are left out.
void recordClothTimings(Profiler& profiler, const ClothPassTimings& timings);

/*
* A profiler's numbers as one line of text: each section's name and its average per call, then "/" and the worst call
* if withWorst, in milliseconds with that many decimals, then "MS". With hideIdle, sections that took no time (would
* show as zero at those decimals) are left out (the cloth's kinds of pass another solver uses, the GPU's empty passes),
* except section 0, the total, so a long list fits the line. Only
* the sections from firstSection to before endSection (-1: all of them to the last).
*/
std::string profileText(const Profiler& profiler, int decimals, bool withWorst, bool hideIdle = false, int firstSection = 0, int endSection = -1);

// profileText written to the readout, wrapped to the screen.
void drawProfile(const Profiler& profiler, int decimals, bool withWorst, const Vec3& colour, DebugDraw& debug, bool hideIdle = false);

// The cloth line while the latest frame had more cloth passes than its stamps (ClothPassTimings::overflowed).
inline constexpr const char* clothPassesNotMeasuredText = "CLOTH PASSES NOT MEASURED: TOO MANY IN A FRAME";

// The readout's cloth line: clothPassesNotMeasuredText while unmeasured, else the cloth profiler's kinds that took time.
std::string clothReadoutText(const Profiler& profiler, bool unmeasured);

// clothReadoutText written to the readout.
void drawClothReadout(const Profiler& profiler, bool unmeasured, DebugDraw& debug);

// Timings are drawn in this colour (linear pale green), apart from the rest of the readout.
const Vec3 profileDebugColour{0.45f, 1.0f, 0.55f};


// Collider wireframes are this colour (linear orange), apart from anything a game highlights itself.
const Vec3 colliderDebugColour{1.0f, 0.45f, 0.05f};

// Every collider in the world, drawn as the shape a query tests, at its entity's pose interpolated like the meshes (alpha).
void drawColliders(const World& world, float alpha, DebugDraw& debug);
