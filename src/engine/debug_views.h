#pragma once

#include "ecs/world.h"
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
void drawBoundingBoxes(const std::vector<DrawItem>& draws, const std::vector<Aabb>& meshBounds, DebugDraw& debug);

// The culling readout in the top-left corner: what the camera and the shadow views drew and skipped last frame.
void drawRenderStats(const RenderStats& stats, DebugDraw& debug);


// Collider wireframes are this colour (linear orange), apart from anything a game highlights itself.
const Vec3 colliderDebugColour{1.0f, 0.45f, 0.05f};

// Every collider in the world, drawn as the shape a query tests, at its entity's pose interpolated like the meshes (alpha).
void drawColliders(const World& world, float alpha, DebugDraw& debug);
