#include "games/forest/forest_sway.h"

namespace {
    void visitGroup(const char* name, ForestSwayGroup& group, SettingsVisitor& visitor) {
        visitor.group(name, [&] {
            visitor.field("Bend", group.bend, {.min = 0.0f, .max = 10.0f, .sliderMax = 2.0f});
            visitor.field("Flutter", group.flutter, {.min = 0.0f, .max = 10.0f, .sliderMax = 1.0f, .unit = "m"});
        });
    }

    const ForestSwayGroup& groupOf(const ForestSwaySettings& settings, ForestSwayPart part) {
        switch (part) {
        case ForestSwayPart::Trees:
            return settings.trees;
        case ForestSwayPart::Plants:
            return settings.plants;
        case ForestSwayPart::Grass:
            return settings.grass;
        case ForestSwayPart::Floor:
        case ForestSwayPart::Hanging:
            break;
        }

        return settings.floor;
    }

    SwayKind kindOf(ForestSwayPart part) {
        switch (part) {
        case ForestSwayPart::Trees:
            return SwayKind::Tree;
        case ForestSwayPart::Plants:
            return SwayKind::Plant;
        case ForestSwayPart::Hanging:
            return SwayKind::Hanging;
        case ForestSwayPart::Grass:
        case ForestSwayPart::Floor:
            break;
        }

        return SwayKind::Grass;
    }
}

void visit(ForestSwaySettings& settings, SettingsVisitor& visitor) {
    visitGroup("Trees", settings.trees, visitor);
    visitGroup("Plants", settings.plants, visitor);
    visitGroup("Grass", settings.grass, visitor);
    visitGroup("Forest floor", settings.floor, visitor);
    visitor.group("Hanging", [&] {
        visitor.field("Swing", settings.hangingSwing, {.min = 0.0f, .max = 10.0f, .sliderMax = 2.0f});
        visitor.field("Flutter", settings.hangingFlutter, {.min = 0.0f, .max = 10.0f, .sliderMax = 1.0f, .unit = "m"});
    });
}

WindSway forestSway(const ForestSwaySettings& settings, const ForestSwayer& swayer) {
    if (swayer.part == ForestSwayPart::Hanging) {
        return WindSway{.kind = SwayKind::Hanging, .height = swayer.height, .bend = 0.0f, .flutter = swayer.leafy ? settings.hangingFlutter : 0.0f,
                        .swing = settings.hangingSwing};
    }

    const ForestSwayGroup& group = groupOf(settings, swayer.part);
    return WindSway{.kind = kindOf(swayer.part), .height = swayer.height, .bend = group.bend, .flutter = swayer.leafy ? group.flutter : 0.0f,
                    .swing = settings.hangingSwing};
}
