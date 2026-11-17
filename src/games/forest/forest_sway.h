#pragma once

#include "core/settings_visitor.h"
#include "ecs/entity.h"
#include "scene/wind.h"

/*
* How the forest sways in the wind (docs/research/WIND.md, stage 2): each kind of growth's answer to its spring of the wind
* force field (render/gpu/wind.h), F5 Forest, Sway. Under a rainforest's canopy the wind is weak and the crowns above
* move most (docs/research/WIND.md, Real wind and plants), so the plants' bend stands in for the shelter as much as for their
* stiffness.
*/

// One kind's bend (its top's lean per radian of its spring) and flutter (metres its leaves shiver per radian).
struct ForestSwayGroup {
    float bend = 1.0f;
    float flutter = 0.0f;

    bool operator==(const ForestSwayGroup&) const = default;
};

/*
* The trees (bark, leaves, their shadows' stand-ins, climbers and epiphytes, all bending as one); the plants (the
* understorey and the palms); the grass; and the forest floor's cover (seedlings and ferns; the fallen leaves lie flat
* and never move, being at no height).
*/
struct ForestSwaySettings {
    ForestSwayGroup trees{.bend = 0.1f, .flutter = 0.3f};
    ForestSwayGroup plants{.bend = 0.3f, .flutter = 0.04f};
    ForestSwayGroup grass{.bend = 0.5f, .flutter = 0.02f};
    ForestSwayGroup floor{.bend = 0.3f, .flutter = 0.01f};

    // Hanging things (lianas, the islands' roots, vines and moss, the climbers' roots in mid air): how far they swing
    // per radian of the hanging spring's lean and metre they hang, and their leaves' flutter.
    float hangingSwing = 0.3f;
    float hangingFlutter = 0.05f;

    bool operator==(const ForestSwaySettings&) const = default;
};

void visit(ForestSwaySettings& settings, SettingsVisitor& visitor);

// Which of the settings' groups a swaying thing follows.
enum class ForestSwayPart {
    Trees,
    Plants,
    Grass,
    Floor,
    Hanging,
};

// A swaying entity: its group, the height its bend is measured against (its mesh's units; a tree's for everything on
// it), and whether it has leaves to flutter (bark, stems and moss do not).
struct ForestSwayer {
    Entity entity;
    ForestSwayPart part = ForestSwayPart::Trees;
    float height = 1.0f;
    bool leafy = false;
};

// Its sway for the settings: the trees' kind for trees, plants' for plants, grass's for the grass and the floor's cover
// (bending from the ground below each vertex, since a patch is many plants), hanging things' for what hangs; every
// one's swing the hanging swing, for whatever of it hangs (a tree's climbers' roots).
WindSway forestSway(const ForestSwaySettings& settings, const ForestSwayer& swayer);
