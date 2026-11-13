#include "render/gpu/render_settings.h"

void visit(RenderSettings& settings, SettingsVisitor& visitor) {
    visitor.group("Lights", [&] {
        visit(settings.lights, visitor);
    });
    visitor.group("Shadows", [&] {
        visit(settings.shadows, visitor);
    });
    visitor.group("Bloom", [&] {
        visit(settings.bloom, visitor);
    });
    visitor.group("Tone mapping", [&] {
        visit(settings.toneMapping, visitor);
    });
    visitor.group("Camera", [&] {
        visit(settings.camera, visitor);
    });
    visitor.group("Display", [&] {
        visit(settings.display, visitor);
    });
    visitor.group("Global illumination", [&] {
        // The light probes trace their rays: all of them need hardware ray tracing.
        visitor.needingRayQueries([&] {
            visit(settings.globalIllumination, visitor);
        });
    });
    visitor.group("Upscaling", [&] {
        visit(settings.upscaling, visitor);
    });
    visitor.group("TAA", [&] {
        visit(settings.taa, visitor);
    });
    visitor.group("Ambient occlusion", [&] {
        visit(settings.ambientOcclusion, visitor);
    });
    visitor.group("Contact shadows", [&] {
        visit(settings.contactShadows, visitor);
    });
    visitor.group("Sky visibility", [&] {
        visit(settings.skyVisibility, visitor);
    });
    visitor.group("Reflections", [&] {
        visit(settings.reflections, visitor);
        visit(settings.mirroredLights, visitor);
    });
    visitor.group("Transparency", [&] {
        visit(settings.transparency, visitor);
    });
    visitor.group("Fog", [&] {
        visit(settings.fog, visitor);
    });
    visitor.group("Materials", [&] {
        visit(settings.materials, visitor);
    });
    visitor.group("Passes", [&] {
        visitor.field("Depth prepass", settings.depthPrepass);
        visitor.field("Clustered shading", settings.clusteredShading);
        visitor.field("Cluster heatmap", settings.clusterHeatmap);
        visitor.field("Normal map filtering", settings.normalMapFiltering);
        visitor.field("Specular anti-aliasing", settings.specularAntiAliasing);
        visitor.field("Specular anti-aliasing variance", settings.specularAntiAliasingVariance, {.min = 0.0f, .max = 4.0f, .sliderMin = 0.0f, .sliderMax = 1.0f});
        visitor.needingRayQueries([&] {
            visitor.field("Ray tracing", settings.rayTracing);
            visitor.choice("Ray debug view", settings.rayDebugView, rayDebugViewNames);
        });
    });
}

bool tracesRays(const RenderSettings& settings, bool rayQueries) {
    return settings.rayTracing && rayQueries;
}
