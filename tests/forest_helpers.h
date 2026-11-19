#pragma once

#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "games/forest/forest_game.h"
#include "games/forest/forest_planting.h"
#include "games/forest/forest_terrain.h"
#include "games/forest/forest_trees.h"
#include "games/forest/palms.h"
#include "games/forest/understorey.h"
#include "games/forest/valley_maps.h"
#include "core/settings_visitor.h"
#include "gameplay/control_mode.h"
#include "scene/indexed_mesh.h"
#include "scene/planting_maps.h"
#include "scene/terrain_heights.h"

#include "test_jobs.h"

#include <functional>

/*
* A valley for the tests (forest_test, island_hangings_test, hanging_moss_test, drifting_rocks_test, forest_trees_test),
* built on the run's one job system (test_jobs.h). A fresh valley, initialised headless with its stand-in ground (an
* asset folder that does not exist), its coarsest ground and fewer shapes by default, so the tests neither need the
* downloaded assets nor spend long building it. Its
* trees are two sparse variants (a few hundred attractors each, where the game grows four of 4000): placed exactly as
* the game's, since the crowns' sizes are the same, but grown in a fraction of the time (forest_trees_test grows full
* ones), and their leaf clusters' texture small.
*/
// Planting maps round the trees' patch only (the game's cover the whole valley), so building them is quick.
inline ForestPlantingSettings testPlantingSettings() {
    ForestPlantingSettings settings;
    settings.northmost = -450.0f;
    settings.southmost = 100.0f;
    return settings;
}

// Every species' reach with its default settings: the canopy caps the planting maps are built for.
inline std::vector<float> defaultPlantReaches() {
    return {treeReach(emergentTreeSettings()), treeReach(ForestTreeSettings{}), treeReach(midTreeSettings()),
            palmReach(ForestPalmSettings{}), understoreyReach(ForestUnderstoreySettings{})};
}

/*
* The planting maps over the patch (testPlantingSettings) with the default valley's pillars, the towers and the sight
* lines, caps for every species' default reach: built once for the run.
*/
inline const ValleyMaps& patchValleyMaps() {
    static const ValleyMaps maps = [] {
        std::vector<BastionShape> towers = farTowers();
        towers.push_back(monolithBastion(forestHeight(forestClearingCentre.x, forestClearingCentre.y), ForestBastionSettings{}));
        return buildValleyMaps(testPlantingSettings(), forestObstacles(placePillars(ValleyLayout{}), towers), forestSightLines(),
                               defaultPlantReaches(), testJobs());
    }();
    return maps;
}

// The land round the patch the placement tests stand plants on: forestHeight on a 2 m grid, as the game's finest ring.
inline const TerrainHeights& patchTerrainHeights() {
    static const TerrainHeights heights = [] {
        TerrainHeights made;
        made.grids.push_back(makeTerrainHeightGrid(Vec2{0.0f, -125.0f}, 400.0f, 2.0f, forestHeight, testJobs()));
        return made;
    }();
    return heights;
}

// Every species with its default settings.
inline const ForestSpecies& defaultSpecies() {
    static const ForestTreeSettings emergent = emergentTreeSettings();
    static const ForestTreeSettings canopy;
    static const ForestTreeSettings mid = midTreeSettings();
    static const ForestUnderstoreySettings understorey;
    static const ForestPalmSettings palms;
    static const ForestSpecies species{emergent, canopy, mid, understorey, palms};
    return species;
}

// The patch planted with every default layer on patchValleyMaps and patchTerrainHeights, once for the run.
inline const ForestPlanting& defaultPatchPlanting() {
    static const ForestPlanting planted = plantThePatch(defaultSpecies(), patchValleyMaps(), patchTerrainHeights(), testJobs());
    return planted;
}

// One layer planted alone (nothing before it to keep clear of) over the patch's box, the plants in the patch kept.
inline std::vector<PlantedPlant> plantAlone(const PlantingLayerRecord& layer, const ValleyMaps& maps) {
    std::vector<PlantedPlant> kept;

    for (const PlantedPlant& plant : plantLayers({layer}, maps.planting, patchTerrainHeights(), regionBox(forestPatch), testJobs()).front()) {
        if (insideRegion(forestPatch, plant.foot)) {
            kept.push_back(plant);
        }
    }

    return kept;
}

struct TestForest {
    World world;
    AssetManager assets;
    ForestGame game{ControlMode::FirstPerson, true, "no-such-folder/forest"};

    explicit TestForest(ForestValleySettings valley = ForestValleySettings{.groundCoarseness = 2, .pillarCount = 40, .islandCount = 8}) {
        game.getValleySettings() = valley;
        game.getTreeSettings().attractors = 300;
        game.getTreeSettings().variants = 2;
        game.getTreeSettings().clusterTexels = 64;
        game.getEmergentSettings().attractors = 300;
        game.getEmergentSettings().variants = 2;
        game.getEmergentSettings().clusterTexels = 64;
        game.getMidSettings().attractors = 150;
        game.getMidSettings().variants = 2;
        game.getMidSettings().clusterTexels = 64;
        game.getGrassSettings().bladesPerSquareMetre = 4.0f;
        game.getGrassSettings().variants = 1;
        game.getGrassSettings().densitySteps = 2;
        game.getGrassSettings().reachMetres = 40.0f;
        game.getFloorSettings().leavesPerSquareMetre = 2.0f;
        game.getFloorSettings().variants = 1;
        game.getFloorSettings().densitySteps = 2;
        game.getFloorSettings().reachMetres = 40.0f;
        game.getPlantingSettings() = testPlantingSettings();
        game.useJobs(testJobs());
        game.onInit(world, assets);
    }
};

// Counts the fields a settings visit names, and how many apply every frame (the rest apply at start-up).
class FieldCounter : public SettingsVisitor {
public:
    int fields = 0;
    int live = 0;

protected:
    void visitFloat(const char*, float&, const FloatLimits& limits) override {
        count(limits.applies);
    }

    void visitInt(const char*, int&, const IntLimits& limits) override {
        count(limits.applies);
    }

    void visitBool(const char*, bool&, FieldApplies applies) override {
        count(applies);
    }

    void visitChoice(const char*, int&, const char* const*, int, FieldApplies applies) override {
        count(applies);
    }

    void visitGroup(const char*, const std::function<void()>& contents) override {
        contents();
    }

private:
    void count(FieldApplies applies) {
        ++fields;
        live += applies == FieldApplies::EveryFrame ? 1 : 0;
    }
};

// Every triangle of a mesh faces the way its corners' normals say (wound counterclockwise seen from outside).
inline bool trianglesFaceTheirNormals(const IndexedMesh& mesh) {
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const MeshVertex& a = mesh.vertices[mesh.indices[i]];
        const MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 facing = (b.position - a.position).cross(c.position - a.position);

        if (facing.lengthSquared() < 1e-12f) {
            continue;
        }

        if (facing.dot(a.normal + b.normal + c.normal) <= 0.0f) {
            return false;
        }
    }

    return true;
}

