#pragma once

#include "render/render_stats.h"
#include "scene/debug_draw.h"
#include "scene/frame_description.h"
#include "scene/model.h"

#include <vector>

/*
* Debug views the engine can draw for any game. A game switches them on (Game::wantsDebugView); the engine knows
* the meshes and the renderer's counts, so it does the drawing.
*/

// A box around every draw in the frame, using its mesh's bounds (indexed like MeshHandle) and the draw's model matrix.
// Draws whose mesh has no known bounds are skipped.
void drawBoundingBoxes(const std::vector<DrawItem>& draws, const std::vector<ModelBounds>& meshBounds, DebugDraw& debug);

// The culling readout in the top-left corner: what the camera and the shadow views drew and skipped last frame.
void drawRenderStats(const RenderStats& stats, DebugDraw& debug);
