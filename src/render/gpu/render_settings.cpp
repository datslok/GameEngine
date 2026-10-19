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
    visitor.group("Passes", [&] {
        visitor.field("Depth prepass", settings.depthPrepass);
        visitor.field("Clustered shading", settings.clusteredShading);
    });
}
