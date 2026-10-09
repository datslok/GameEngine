#include "engine/debug_views.h"
#include "scene/debug_text.h"

#include <string>

namespace {
    const Vec3 boundingBoxColour{1.0f, 0.75f, 0.0f};
}

/*
* Each part is drawn as its own mesh, so each gets its own box: a model made of several meshes shows several boxes.
*/
void drawBoundingBoxes(const std::vector<DrawItem>& draws, const std::vector<ModelBounds>& meshBounds, DebugDraw& debug) {
    for (const DrawItem& draw : draws) {
        if (!draw.mesh.isValid() || draw.mesh.index >= meshBounds.size()) {
            continue;
        }

        debug.box(meshBounds[draw.mesh.index], draw.model, boundingBoxColour);
    }
}

/*
* Two lines of text, one for the camera's view and one for all shadow views together (each shadow tile counts each
* mesh once), in the same colour as the bounding boxes.
*/
void drawRenderStats(const RenderStats& stats, DebugDraw& debug) {
    constexpr float margin = 16.0f;
    constexpr float textHeight = 14.0f;
    constexpr float lineSpacing = 22.0f;

    const std::string camera = "DRAWN " + std::to_string(stats.drawn) + "  CULLED " + std::to_string(stats.culled);
    const std::string shadows = "SHADOW DRAWN " + std::to_string(stats.shadowDrawn) + "  CULLED " + std::to_string(stats.shadowCulled);

    debugText(debug, Vec2{margin, margin}, textHeight, camera, boundingBoxColour);
    debugText(debug, Vec2{margin, margin + lineSpacing}, textHeight, shadows, boundingBoxColour);
}