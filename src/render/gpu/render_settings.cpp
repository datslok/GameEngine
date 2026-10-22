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
    visitor.group("Display", [&] {
        visit(settings.display, visitor);
    });
    visitor.group("Global illumination", [&] {
        visit(settings.globalIllumination, visitor);
    });
    visitor.group("Anti-aliasing", [&] {
        visit(settings.taa, visitor);
    });
    visitor.group("Upscaling", [&] {
        visit(settings.upscaling, visitor);
    });
    visitor.group("Passes", [&] {
        visitor.field("Depth prepass", settings.depthPrepass);
        visitor.field("Clustered shading", settings.clusteredShading);
        visitor.field("Cluster heatmap", settings.clusterHeatmap);
        visitor.choice("Ray debug view", settings.rayDebugView, rayDebugViewNames);
    });
}
