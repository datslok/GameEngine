#include "app/game_registry.h"
#include "games/clothlab/cloth_lab_game.h"
#include "games/clothlab/cloth_lab_layout.h"
#include "physics/overlap.h"
#include "physics/static_level.h"
#include "scene/atmosphere.h"
#include "scene/light.h"
#include "scene/model_renderer.h"
#include "scene/probe_volume.h"

#include <cassert>
#include <cmath>
#include <string>
#include <vector>

namespace {
    // A fresh cloth lab, initialised headless.
    struct ClothLab {
        World world;
        AssetManager assets;
        ClothLabGame game{ControlMode::FreeCamera, true};

        ClothLab() {
            game.onInit(world, assets);
        }
    };

    bool near(float actual, float expected) {
        return std::abs(actual - expected) < 1e-4f;
    }

    // Where a ray straight down from high above the point first meets the level, or NaN when it meets nothing.
    float levelHeightBelow(const StaticLevel& level, float x, float z) {
        const std::optional<LevelHit> hit = raycast(level, Ray{Vec3{x, 50.0f, z}, Vec3{0.0f, -1.0f, 0.0f}});
        return hit ? hit->hit.point.y : std::nanf("");
    }

    // It is a registered game, started with --game clothlab, and its title names its settings section.
    void testItIsRegistered() {
        const GameEntry* entry = findGame("clothlab");
        assert(entry != nullptr);
        assert(std::string{entry->make()->title()} == "Cloth lab");
        assert(parseLaunchOptions({"--game", "clothlab"}).game == entry);
    }

    // The ball sits on its pedestal, under its frame's bar with room for a sheet to fall onto it.
    void testTheBallRestsUnderItsFrame() {
        const ClothLabLayout layout = buildClothLabLayout();
        const float pedestalTop = layout.pedestal.centre.y + layout.pedestal.halfSize.y;
        assert(near(layout.pedestal.centre.y - layout.pedestal.halfSize.y, 0.0f));
        assert(near(layout.ball.centre.y - layout.ball.radius, pedestalTop));

        const float ballTop = layout.ball.centre.y + layout.ball.radius;
        assert(clothLabBarUnderside(layout.drapeFrame) > ballTop + 0.5f);

        // The bar runs past the ball on both sides, so a sheet pinned at its ends hangs over the whole ball.
        const float barStart = layout.drapeFrame.barCentre.x - layout.drapeFrame.barHalfLength;
        const float barEnd = layout.drapeFrame.barCentre.x + layout.drapeFrame.barHalfLength;
        assert(barStart < layout.ball.centre.x - layout.ball.radius);
        assert(barEnd > layout.ball.centre.x + layout.ball.radius);
    }

    // A frame is two posts standing on the floor and a bar resting on their tops.
    void testFramesStandOnTheFloor() {
        const ClothLabLayout layout = buildClothLabLayout();
        for (const ClothLabFrame& frame : {layout.drapeFrame, layout.fabricFrame}) {
            const std::vector<ClothLabBlock> blocks = clothLabFrameBlocks(frame);
            assert(blocks.size() == 3);
            const ClothLabBlock& bar = blocks[2];
            for (int post = 0; post < 2; ++post) {
                assert(near(blocks[post].centre.y - blocks[post].halfSize.y, 0.0f));
                assert(near(blocks[post].centre.y + blocks[post].halfSize.y, bar.centre.y - bar.halfSize.y));
            }
            assert(near(bar.centre.y - bar.halfSize.y, clothLabBarUnderside(frame)));
        }
    }

    // The stations stand apart, and none covers where the player starts.
    void testStationsStandApart() {
        const std::vector<Aabb> stations = buildClothLabLayout().stations();
        assert(stations.size() == 3);
        for (std::size_t first = 0; first < stations.size(); ++first) {
            for (std::size_t second = first + 1; second < stations.size(); ++second) {
                assert(!overlaps(stations[first], stations[second]));
            }
            assert(!overlaps(stations[first], Sphere{clothLabFirstPersonFeet + Vec3{0.0f, 1.0f, 0.0f}, 1.0f}));
        }
    }

    // The floor, the mat, the pedestal and the frames are the static level: walked on and moved off by the light probes.
    void testTheLevelIsTheLayout() {
        ClothLab lab;
        const ClothLabLayout layout = buildClothLabLayout();
        const StaticLevel& level = lab.game.getLevel();

        assert(near(levelHeightBelow(level, 10.0f, 10.0f), 0.0f));
        assert(near(levelHeightBelow(level, layout.mat.centre.x, layout.mat.centre.z), layout.mat.centre.y + layout.mat.halfSize.y));
        assert(near(levelHeightBelow(level, layout.ball.centre.x, layout.ball.centre.z), layout.ball.centre.y + layout.ball.radius));
        assert(near(levelHeightBelow(level, layout.fabricFrame.barCentre.x, layout.fabricFrame.barCentre.z),
                    clothLabBarUnderside(layout.fabricFrame) + 2.0f * clothLabBarHalfThickness));

        int staticModels = 0;
        int others = 0;
        lab.world.each<ModelRenderer>([&](Entity, ModelRenderer& renderer) { renderer.isStatic ? ++staticModels : ++others; });
        assert(staticModels >= 1);
        assert(others == 0);
    }

    // A sunlit afternoon under the real sky, with light probes round the camera.
    void testItIsLit() {
        ClothLab lab;
        int suns = 0;
        lab.world.each<DirectionalLight>([&](Entity, DirectionalLight& light) {
            assert(light.castsShadows);
            assert(light.direction.y < 0.0f);
            ++suns;
        });
        assert(suns == 1);

        int skies = 0;
        lab.world.each<Atmosphere>([&](Entity, Atmosphere&) { ++skies; });
        assert(skies == 1);

        int probes = 0;
        lab.world.each<ProbeCascades>([&](Entity, ProbeCascades&) { ++probes; });
        assert(probes == 1);
    }

    // It has golden views, looking at its stations.
    void testItHasGoldenViews() {
        ClothLab lab;
        assert(!lab.game.goldenViews().empty());
    }
}

void testClothLab() {
    testItIsRegistered();
    testTheBallRestsUnderItsFrame();
    testFramesStandOnTheFloor();
    testStationsStandApart();
    testTheLevelIsTheLayout();
    testItIsLit();
    testItHasGoldenViews();
}
