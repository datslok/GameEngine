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
* Four lines of text, in the same colour as the bounding boxes: the camera's view, all shadow views together (each
* shadow tile counts each mesh once), the point lights (all of them, those that reach into view, and those that got
* a seat in the shader and a shadow), and the shadow tiles drawn and point light cube faces skipped as unseen.
*/
void drawRenderStats(const RenderStats& stats, DebugDraw& debug) {
    constexpr float margin = 16.0f;
    constexpr float textHeight = 14.0f;
    constexpr float lineSpacing = 22.0f;

    const std::string camera = "DRAWN " + std::to_string(stats.drawn) + "  CULLED " + std::to_string(stats.culled);
    const std::string shadows = "SHADOW DRAWN " + std::to_string(stats.shadowDrawn) + "  CULLED " + std::to_string(stats.shadowCulled);
    const std::string tiles = "SHADOW TILES " + std::to_string(stats.shadowTilesDrawn) + "  SKIPPED " + std::to_string(stats.shadowTilesSkipped);

    debugText(debug, Vec2{margin, margin}, textHeight, camera, boundingBoxColour);
    const std::string lights = "LIGHTS " + std::to_string(stats.pointLights) + "  IN VIEW " + std::to_string(stats.pointLightsInView) +
                               "  LIT " + std::to_string(stats.pointLightsLit) + "  SHADOWED " + std::to_string(stats.pointLightsShadowed);

    debugText(debug, Vec2{margin, margin + lineSpacing}, textHeight, shadows, boundingBoxColour);
    debugText(debug, Vec2{margin, margin + 2.0f * lineSpacing}, textHeight, lights, boundingBoxColour);
    debugText(debug, Vec2{margin, margin + 3.0f * lineSpacing}, textHeight, tiles, boundingBoxColour);
}