#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"

#include <cstdint>
#include <memory>
#include <vector>

/*
* Component: the wind over the land, which the renderer's wind force field (render/gpu/wind.h) turns into the sway of
* everything growing. A game places one, as it places a Fog; the first counts. It blows towards towardsDegrees (from +x
* towards +z, about up) at speed, its speed rising and falling by up to gustStrength of it in gusts about gustSize across
* that travel with it, and pushing sideways by up to turbulence of it in eddies about turbulenceSize across. Under a
* closed rainforest canopy the wind is under 1 m/s; above it a few m/s (docs/research/WIND.md).
*/
struct Wind {
    float towardsDegrees = 0.0f;
    float speed = 3.0f;          // m/s
    float gustStrength = 0.6f;   // a share of the speed
    float gustSize = 40.0f;      // m
    float turbulence = 0.3f;     // a share of the speed, sideways
    float turbulenceSize = 10.0f; // m
};

void visit(Wind& wind, SettingsVisitor& visitor);

/*
* Which of the wind force field's springs a drawn thing follows (render/gpu/wind.h), in the field's own order after
* Still, which does not sway. Trees and plants (and hanging things) read their spring once, at their foot (the model's
* origin), so everything placed with the same pose (a tree's bark, leaves, climbers and epiphytes) bends as one; grass
* reads it at each vertex, so gusts roll across a meadow, each vertex bending from the ground below it.
*/
enum class SwayKind {
    Still,
    Tree,
    Plant,
    Grass,
    Hanging,
};

/*
* How a drawn thing sways (ModelRenderer::sway, docs/research/WIND.md). Its mesh stands with its foot at its frame's origin and up
* along y; height (in the mesh's own units, so it scales with the object) is what its bend is measured against: a
* vertex at height h leans by bend times its spring's lean (radians) times h^2 / height, keeping its distance from the
* foot (a cantilever's shape), and its branches bob more the further from the trunk. A tree's climbers and epiphytes
* take the tree's height, so they stay on it. flutter is how many metres its leaves shiver per radian of lean, more
* towards the top. swing is how far what hangs from it (its mesh's hanging vertices, below) swings per radian of the
* hanging spring's lean and metre of its arm.
*/
struct WindSway {
    SwayKind kind = SwayKind::Still;
    float height = 1.0f;
    float bend = 1.0f;
    float flutter = 0.0f;
    float swing = 0.0f;
};

// A hanging vertex's anchor that never moves (rock, the ground), and one that moves as its own object sways there (a
// root hanging inside its tree's frame).
inline constexpr std::uint16_t windAnchorFixed = 0xFFFF;
inline constexpr std::uint16_t windAnchorOwn = 0xFFFE;

/*
* A vertex of something hanging (IndexedMesh::hanging, docs/research/WIND.md stage 3): the two anchors it hangs between (indices
* into the frame's WindAnchors, or the codes above; the same twice for something hanging from one), its share of the
* way from the first to the second (their motions blended by it), and its arm: how far it hangs below the line between
* them, or below its one anchor (metres), which swings it on the hanging spring as a chain's links swing; and its swing's
* length, the pendulum its strand swings as (metres): a strand hung from one anchor its whole length, one between two
* its deepest sag (a sagging rope swinging about its chord is within about 8% of a chain its sag long), which sets how
* slowly it swings.
*/
struct HangingVertex {
    std::uint16_t first = windAnchorOwn;
    std::uint16_t second = windAnchorOwn;
    float along = 0.0f;
    float arm = 0.0f;
    float swingLength = 0.0f;

    bool operator==(const HangingVertex&) const = default;
};

/*
* A point things hang from on a swaying tree (a liana's hold in a crown): the tree's foot and its height in its mesh's
* units, its scale and bend (as its WindSway), and the point in the world. The shaders move it exactly as the tree's
* own vertex there moves.
*/
struct WindAnchor {
    Vec3 foot{};
    float height = 1.0f;
    float scale = 1.0f;
    float bend = 1.0f;
    Vec3 point{};
};

// Component: the anchors a game's hanging things hang from (FrameDescription::windAnchors; the first counts), shared so
// each frame takes them without copying.
struct WindAnchors {
    std::shared_ptr<const std::vector<WindAnchor>> anchors;
};
