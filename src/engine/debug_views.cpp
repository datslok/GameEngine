#include "engine/debug_views.h"
#include "math/transform.h"
#include "physics/collider.h"
#include "scene/interpolation.h"
#include "scene/debug_text.h"

#include <cstdio>
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
* Six lines of text, in the same colour as the bounding boxes: the camera's view, all shadow views together (each
* shadow tile counts each mesh once; CALLS counts the instanced draws they took, one per mesh per view), the point
* lights (all of them, those that reach into view, and those that got a seat in the shader and a shadow), the shadow
* tiles drawn and point light cube faces skipped as unseen with how far the sun's cascades reach, the light probes'
* states, the video memory, and the eye's exposure with the upscaler.
*/
void drawRenderStats(const RenderStats& stats, DebugDraw& debug) {

    const std::string camera = "DRAWN " + std::to_string(stats.drawn) + "  CULLED " + std::to_string(stats.culled) +
                               "  CALLS " + std::to_string(stats.drawCalls);
    const std::string shadows = "SHADOW DRAWN " + std::to_string(stats.shadowDrawn) + "  CULLED " + std::to_string(stats.shadowCulled) +
                                "  CALLS " + std::to_string(stats.shadowDrawCalls);
    std::string tiles = "TILES DRAWN " + std::to_string(stats.shadowTilesDrawn) + "  CACHED " + std::to_string(stats.shadowTilesCached) +
                        "  SKIPPED " + std::to_string(stats.shadowTilesSkipped);

    if (stats.sunShadowDistance > 0.0f) {
        char distance[32];
        std::snprintf(distance, sizeof(distance), "  SUN TO %.1f M", stats.sunShadowDistance);
        tiles += distance;
    }

    debugText(debug, debugReadoutLine(0), debugReadoutTextHeight, camera, boundingBoxColour);
    const std::string lights = "LIGHTS " + std::to_string(stats.pointLights) + "  IN VIEW " + std::to_string(stats.pointLightsInView) +
                               "  LIT " + std::to_string(stats.pointLightsLit) + "  SHADOWED " + std::to_string(stats.pointLightsShadowed);

    debugText(debug, debugReadoutLine(1), debugReadoutTextHeight, shadows, boundingBoxColour);
    debugText(debug, debugReadoutLine(2), debugReadoutTextHeight, lights, boundingBoxColour);
    debugText(debug, debugReadoutLine(3), debugReadoutTextHeight, tiles, boundingBoxColour);
    debugText(debug, debugReadoutLine(probeReadoutLine), debugReadoutTextHeight, probeStatsText(stats.probes), boundingBoxColour);
    debugText(debug, debugReadoutLine(videoMemoryReadoutLine), debugReadoutTextHeight, videoMemoryText(stats.videoMemory), boundingBoxColour);
    debugText(debug, debugReadoutLine(exposureReadoutLine), debugReadoutTextHeight,
              exposureText(stats.eyeAdaptation) + "  " + upscalerText(stats.upscaler, stats.renderScale), boundingBoxColour);
}

// Hundredths of the scale: the slider's own steps.
std::string upscalerText(Upscaler upscaler, float renderScale) {
    char text[48];
    std::snprintf(text, sizeof(text), "%s AT %.2f", upscalerNames[static_cast<std::size_t>(upscaler)], renderScale);
    return text;
}
// Signed and to a tenth of a stop: finer would only flicker as the exposure eases.
std::string exposureText(const std::optional<GpuEyeAdaptation>& adaptation) {
    if (!adaptation) {
        return "EXPOSURE FIXED";
    }

    char text[64];
    std::snprintf(text, sizeof(text), "EXPOSURE %+.1f EV  SCENE %+.1f EV", adaptation->exposureEv, adaptation->meanEv);
    return text;
}

// Tenths of a metre and hundredths of the direction: enough to set up the same view, without flickering as it moves.
std::string cameraText(const Vec3& position, const Vec3& forward) {
    char text[96];
    std::snprintf(text, sizeof(text), "CAMERA %.1f %.1f %.1f  LOOK %.2f %.2f %.2f", position.x, position.y, position.z, forward.x, forward.y,
                  forward.z);
    return text;
}

void drawCameraReadout(const Camera& camera, DebugDraw& debug) {
    debugText(debug, debugReadoutLine(cameraReadoutLine), debugReadoutTextHeight, cameraText(camera.getPosition(), camera.getForward()),
              boundingBoxColour);
}

std::string probeStatsText(const ProbeStateCounts& counts) {
    if (counts.probes == 0) {
        return "PROBES -";
    }

    return "PROBES " + std::to_string(counts.probes) + "  TRACED " + std::to_string(counts.traced) + "  AWAKE " + std::to_string(counts.awake) +
           "  ASLEEP " + std::to_string(counts.sleeping) + "  OFF " + std::to_string(counts.off) + "  MOVED " + std::to_string(counts.moved);
}

/*
* Megabytes rounded to the nearest: memory is reserved in blocks of megabytes, so finer would only flicker.
*/
std::string videoMemoryText(const VideoMemory& memory) {
    if (!memory.valid) {
        return "VRAM -";
    }

    const auto megabytes = [](std::uint64_t bytes) {
        constexpr std::uint64_t megabyte = 1024ull * 1024ull;
        return std::to_string((bytes + megabyte / 2) / megabyte);
    };

    const std::string engine = "ENGINE " + megabytes(memory.engineBytes) + " MB";

    if (!memory.driverReported) {
        return "VRAM " + engine + "  NO DRIVER BUDGET";
    }

    return "VRAM " + megabytes(memory.usedBytes) + " / " + megabytes(memory.budgetBytes) + " MB  " + engine;
}

/*
* One line so the numbers can be compared at a glance.
 Milliseconds rather than seconds, because frames and systems
* cost thousandths of a second; the stroke font has no micro sign, so small costs get more decimals instead.
*/
std::string profileText(const Profiler& profiler, int decimals, bool withWorst) {
    const auto milliseconds = [decimals](double seconds) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", decimals, seconds * 1000.0);
        return std::string(text);
    };

    std::string line;
    for (int section = 0; section < profiler.getSectionCount(); ++section) {
        const SectionTiming& timing = profiler.getTiming(section);
        line += profiler.getName(section) + " " + milliseconds(timing.averageSeconds);
        if (withWorst) {
            line += "/" + milliseconds(timing.worstSeconds);
        }
        line += "  ";
    }
    return line + "MS";
}

/*
* The GPU's numbers go through the same profiler as the CPU's, so they are averaged over the same half second and
* read the same way. Section numbers follow GpuPass, one past the total, so recording needs no lookup.
*/
Profiler makeGpuProfiler() {
    Profiler profiler;
    profiler.addSection("GPU");

    for (std::size_t pass = 0; pass < gpuPassCount; ++pass) {
        profiler.addSection(gpuPassName(static_cast<GpuPass>(pass)));
    }

    return profiler;
}

void recordGpuTimings(Profiler& profiler, const GpuTimings& timings) {
    if (!timings.valid) {
        return;
    }

    profiler.record(0, timings.totalSeconds);

    for (std::size_t pass = 0; pass < gpuPassCount; ++pass) {
        profiler.record(static_cast<int>(pass) + 1, timings.passSeconds[pass]);
    }
}

void drawProfile(const Profiler& profiler, int line, int decimals, bool withWorst, const Vec3& colour, DebugDraw& debug) {
    debugText(debug, debugReadoutLine(line), debugReadoutTextHeight, profileText(profiler, decimals, withWorst), colour);
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
