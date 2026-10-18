#include "engine/debug_views.h"
#include "math/transform.h"
#include "physics/collider.h"
#include "scene/interpolation.h"
#include "scene/debug_text.h"

#include <string>

namespace {
    const Vec3 boundingBoxColour{1.0f, 0.75f, 0.0f};
}

/*
* Each part is drawn as its own mesh, so each gets its own box: a model made of several meshes shows several boxes.
*/
void drawBoundingBoxes(const std::vector<DrawItem>& draws, const std::vector<Aabb>& meshBounds, DebugDraw& debug) {
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

    const std::string camera = "DRAWN " + std::to_string(stats.drawn) + "  CULLED " + std::to_string(stats.culled);
    const std::string shadows = "SHADOW DRAWN " + std::to_string(stats.shadowDrawn) + "  CULLED " + std::to_string(stats.shadowCulled);
    const std::string tiles = "TILES DRAWN " + std::to_string(stats.shadowTilesDrawn) + "  CACHED " + std::to_string(stats.shadowTilesCached) +
                              "  SKIPPED " + std::to_string(stats.shadowTilesSkipped);

    debugText(debug, debugReadoutLine(0), debugReadoutTextHeight, camera, boundingBoxColour);
    const std::string lights = "LIGHTS " + std::to_string(stats.pointLights) + "  IN VIEW " + std::to_string(stats.pointLightsInView) +
                               "  LIT " + std::to_string(stats.pointLightsLit) + "  SHADOWED " + std::to_string(stats.pointLightsShadowed);

    debugText(debug, debugReadoutLine(1), debugReadoutTextHeight, shadows, boundingBoxColour);
    debugText(debug, debugReadoutLine(2), debugReadoutTextHeight, lights, boundingBoxColour);
    debugText(debug, debugReadoutLine(3), debugReadoutTextHeight, tiles, boundingBoxColour);
}
/*
* A margin from the window's corner, then one line every lineSpacing pixels: a little more than the text height, so
* lines do not touch.
*/
Vec2 debugReadoutLine(int line) {
    constexpr float margin = 16.0f;
    constexpr float lineSpacing = 22.0f;
    return Vec2{margin, margin + static_cast<float>(line) * lineSpacing};
}

/*
* Colliders are drawn at the interpolated pose, like the meshes, so the wireframes sit on what is drawn instead of trailing
* it by up to a tick. Queries use the current pose; the difference is less than one tick of motion.
*/
void drawColliders(const World& world, float alpha, DebugDraw& debug) {
    world.each<Collider, Transform>([&](Entity entity, const Collider& collider, const Transform&) {
        const Mat4 model = getRenderTransform(world, entity, alpha).getMatrix();
        debug.shape(worldShape(collider.shape, model), colliderDebugColour);
    });
}
