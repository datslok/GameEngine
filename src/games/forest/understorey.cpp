#include "games/forest/understorey.h"

#include "games/forest/forest_terrain.h"
#include "math/angles.h"
#include "scene/mesh.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
    // The random numbers each cell draws for its plant, after the ones placing it.
    enum CellDraw : std::uint64_t {
        Kind,
        Scale,
        Turn,
        Variant
    };
}

const char* understoreyScanName(UnderstoreyKind kind) {
    switch (kind) {
    case UnderstoreyKind::Fern:
        return "fern_02";
    case UnderstoreyKind::Calathea:
        return "calathea_orbifolia_01";
    case UnderstoreyKind::Anthurium:
        return "anthurium_botany_01";
    }

    return "fern_02";
}

void visit(ForestUnderstoreySettings& settings, SettingsVisitor& visitor) {
    visitor.group("Planting", [&] {
        visit(settings.planting, visitor);
    });
    visitor.field("Trunk clearance", settings.trunkClearance, {.min = 0.0f, .max = 10.0f, .sliderMax = 4.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Fern share", settings.fernShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Calathea share", settings.calatheaShare, {.min = 0.0f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Fern height", settings.fernHeight, {.min = 0.1f, .max = 10.0f, .sliderMax = 4.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Calathea height", settings.calatheaHeight, {.min = 0.1f, .max = 10.0f, .sliderMax = 4.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Anthurium height", settings.anthuriumHeight, {.min = 0.1f, .max = 10.0f, .sliderMax = 4.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Keep share", settings.keepShare, {.min = 0.01f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Level keep share", settings.levelKeepShare, {.min = 0.005f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Shadow cascades", settings.shadowCascades, {.min = 0, .max = 4, .applies = FieldApplies::AtStartup});
    visitor.field("Shadow keep share", settings.shadowKeepShare, {.min = 0.005f, .max = 1.0f, .applies = FieldApplies::AtStartup});
    visitor.field("Impostor error", settings.impostorErrorMetres, {.min = 0.001f, .max = 10.0f, .sliderMax = 1.0f, .unit = "m", .applies = FieldApplies::AtStartup});
    visitor.field("Impostor frames", settings.impostorFrames, {.min = 2, .max = 32, .sliderMax = 16, .applies = FieldApplies::AtStartup});
    visitor.field("Impostor texels", settings.impostorTexels, {.min = 8, .max = 512, .sliderMax = 256, .applies = FieldApplies::AtStartup});
}

float understoreyHeight(const ForestUnderstoreySettings& settings, UnderstoreyKind kind) {
    switch (kind) {
    case UnderstoreyKind::Fern:
        return settings.fernHeight;
    case UnderstoreyKind::Calathea:
        return settings.calatheaHeight;
    case UnderstoreyKind::Anthurium:
        return settings.anthuriumHeight;
    }

    return 1.0f;
}

float understoreyKindScale(const std::vector<IndexedMesh>& plants, float height) {
    float tallest = 0.0f;

    for (const IndexedMesh& plant : plants) {
        tallest = std::max(tallest, indexedMeshBounds(plant).maximum.y);
    }

    return tallest > 0.0f ? height / tallest : 1.0f;
}

std::vector<UnderstoreyPlacement> placeUnderstorey(const ForestUnderstoreySettings& settings, const ValleyMaps& maps,
                                                   const std::vector<StandingCrown>& trunks) {
    std::vector<UnderstoreyPlacement> plants;

    for (const DitheredCell& cell : ditheredCells(settings.planting, maps, forestPatch, understoreySeed)) {
        const bool byTrunk = std::any_of(trunks.begin(), trunks.end(), [&](const StandingCrown& trunk) {
            return (cell.place - trunk.foot).length() < trunk.radius + settings.trunkClearance;
        });

        if (byTrunk) {
            continue;
        }

        const auto draw = [&](CellDraw which) {
            return forestUnitRandom(understoreySeed, cell.firstDraw + which);
        };

        const float kindDraw = draw(Kind);
        const UnderstoreyKind kind = kindDraw < settings.fernShare ? UnderstoreyKind::Fern
                                     : kindDraw < settings.fernShare + settings.calatheaShare ? UnderstoreyKind::Calathea
                                                                                              : UnderstoreyKind::Anthurium;
        UnderstoreyPlacement plant{
            .foot = Vec3{cell.place.x, forestHeight(cell.place.x, cell.place.y) - understoreySinkMetres, cell.place.y},
            .scale = 1.0f + understoreyScaleSpread * (2.0f * draw(Scale) - 1.0f),
            .turnRadians = fullTurnRadians * draw(Turn),
            .kind = kind,
            .variantPick = std::min(draw(Variant), 0.99999f)
        };

        const float radius = understoreyRadius(settings, plant);

        if (fitsTheMaps(maps, cell.place, radius, understoreyCentre(settings, plant).y + radius)) {
            plants.push_back(plant);
        }
    }

    return plants;
}

float understoreyReach(const ForestUnderstoreySettings& settings) {
    const float tallest = std::max({settings.fernHeight, settings.calatheaHeight, settings.anthuriumHeight});
    return 0.5f * tallest * (1.0f + understoreyScaleSpread);
}

float understoreyRadius(const ForestUnderstoreySettings& settings, const UnderstoreyPlacement& plant) {
    return 0.5f * understoreyHeight(settings, plant.kind) * plant.scale;
}

Vec3 understoreyCentre(const ForestUnderstoreySettings& settings, const UnderstoreyPlacement& plant) {
    return plant.foot + Vec3{0.0f, understoreyRadius(settings, plant), 0.0f};
}

IndexedMesh footedPlant(const IndexedMesh& plant) {
    const Aabb bounds = indexedMeshBounds(plant);
    const Vec3 shift{-0.5f * (bounds.minimum.x + bounds.maximum.x), -bounds.minimum.y, -0.5f * (bounds.minimum.z + bounds.maximum.z)};
    IndexedMesh footed = plant;

    for (MeshVertex& vertex : footed.vertices) {
        vertex.position = vertex.position + shift;
    }

    return footed;
}

IndexedMesh understoreyStandIn() {
    IndexedMesh plant;

    for (int blade = 0; blade < 3; ++blade) {
        const float angle = fullTurnRadians * static_cast<float>(blade) / 6.0f;
        const Vec3 across{0.3f * std::cos(angle), 0.0f, 0.3f * std::sin(angle)};
        const Vec3 facing{-std::sin(angle), 0.0f, std::cos(angle)};
        const std::uint32_t first = static_cast<std::uint32_t>(plant.vertices.size());
        const Vec3 corners[4] = {across * -1.0f, across, across + Vec3{0.0f, 1.0f, 0.0f}, across * -1.0f + Vec3{0.0f, 1.0f, 0.0f}};
        const Vec2 uvs[4] = {Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};

        for (int corner = 0; corner < 4; ++corner) {
            plant.vertices.push_back(MeshVertex{corners[corner], facing, uvs[corner], Vec4{across.normalized().x, 0.0f, across.normalized().z, 1.0f}});
        }

        plant.indices.insert(plant.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
    }

    return footedPlant(plant);
}
