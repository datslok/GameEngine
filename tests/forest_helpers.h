#pragma once

#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "games/forest/forest_game.h"
#include "core/settings_visitor.h"
#include "gameplay/control_mode.h"
#include "scene/indexed_mesh.h"

#include "test_jobs.h"

#include <functional>

/*
* A valley for the tests (forest_test, island_hangings_test, hanging_moss_test, drifting_rocks_test, forest_trees_test),
* built on the run's one job system (test_jobs.h). A fresh valley, initialised headless with its stand-in ground (an
* asset folder that does not exist), its coarsest ground and fewer shapes by default, so the tests neither need the
* downloaded assets nor spend long building it. Its
* trees are two sparse variants (a few hundred attractors each, where the game grows four of 2500): placed exactly as
* the game's, since the crowns' sizes are the same, but grown in a fraction of the time (forest_trees_test grows full
* ones).
*/
struct TestForest {
    World world;
    AssetManager assets;
    ForestGame game{ControlMode::FirstPerson, true, "no-such-folder/forest"};

    explicit TestForest(ForestValleySettings valley = ForestValleySettings{.groundCoarseness = 2, .pillarCount = 40, .islandCount = 8}) {
        game.getValleySettings() = valley;
        game.getTreeSettings().attractors = 300;
        game.getTreeSettings().variants = 2;
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

