#include "app/game_registry.h"
#include "games/clothlab/cloth_lab_game.h"
#include "games/clothlab/cloth_lab_layout.h"
#include "physics/overlap.h"
#include "physics/static_level.h"
#include "cloth/cloth.h"
#include "cloth/cloth_collision.h"
#include "cloth/cloth_grab.h"
#include "demo_helpers.h"
#include "scene/deforming_mesh.h"
#include "engine/game.h"
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

        // Everything but the two sheets, which move.
        int staticModels = 0;
        int others = 0;
        lab.world.each<ModelRenderer>([&](Entity, ModelRenderer& renderer) { renderer.isStatic ? ++staticModels : ++others; });
        assert(staticModels >= 1);
        assert(others == 2);
        assert(!lab.world.get<ModelRenderer>(lab.game.getSheet()).isStatic);
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

    /*
    * The hanging sheet: a cloth laid flat level with the fabric bar, its back corners pinned under the bar, falls, swings
    * down and hangs from them in front of the bar, its points handed to its deforming mesh, the same on every run.
    */
    void testTheSheetHangs() {
        ClothLab lab;
        const Entity sheet = lab.game.getSheet();
        const ClothLabLayout layout = buildClothLabLayout();
        const float barUnderside = clothLabBarUnderside(layout.fabricFrame);
        const DeformingMesh& mesh = lab.world.get<DeformingMesh>(sheet);
        assert(lab.assets.isDeforming(mesh.mesh));
        assert(lab.world.get<ModelRenderer>(sheet).parts.at(0).mesh.index == mesh.mesh.index);
        assert(lab.world.get<ModelRenderer>(sheet).parts.at(0).material.doubleSided);
        assert(!lab.world.get<ModelRenderer>(sheet).isStatic);

        // It starts flat, level with the bar's underside.
        for (const Vec3& point : lab.world.get<Cloth>(sheet).particles.positions) {
            assert(near(point.y, barUnderside));
        }

        const auto hang = [](ClothLab& lab) {
            for (int tick = 0; tick < 600; ++tick) {
                runFixedUpdate(lab.game, lab.world, 1.0f / 120.0f, tick / 120.0);
            }
        };
        hang(lab);

        const ClothParticles& particles = lab.world.get<Cloth>(sheet).particles;
        const std::size_t backEdge = static_cast<std::size_t>(clothLabSheetRows) * (clothLabSheetColumns + 1);
        for (std::size_t pin : {backEdge, backEdge + clothLabSheetColumns}) {
            assert(near(particles.positions[pin].y, barUnderside));
            assert(near(particles.positions[pin].z, layout.fabricFrame.barCentre.z));
        }

        const Aabb fabricStation = layout.stations()[2];
        for (const Vec3& point : particles.positions) {
            assert(point.x >= fabricStation.minimum.x && point.x <= fabricStation.maximum.x);
            assert(point.y <= barUnderside + 1e-3f && point.y > 0.0f);
        }
        // The front edge has swung down to hang below the bar.
        assert(particles.positions[clothLabSheetColumns / 2].y < barUnderside - 1.0f);

        const std::vector<Vec3>& shown = lab.world.get<DeformingMesh>(sheet).positions;
        assert(shown.size() == particles.size());
        for (std::size_t index = 0; index < shown.size(); ++index) {
            assert(shown[index].x == particles.positions[index].x && shown[index].y == particles.positions[index].y);
        }

        ClothLab again;
        hang(again);
        const std::vector<Vec3>& repeated = again.world.get<Cloth>(again.game.getSheet()).particles.positions;
        for (std::size_t index = 0; index < repeated.size(); ++index) {
            assert(repeated[index].x == particles.positions[index].x && repeated[index].y == particles.positions[index].y &&
                   repeated[index].z == particles.positions[index].z);
        }
    }

    /*
    * The sheet hangs from two hooks on the bar, a sheet's width apart at the start. The Hook gap setting slides them
    * along the bar at a steady speed, about the sheet's middle, never further apart than the sheet is wide, and the
    * sheet bunches up between them.
    */
    void testHooksSlide() {
        ClothLab lab;
        const ClothLabLayout layout = buildClothLabLayout();
        const Cloth& cloth = lab.world.get<Cloth>(lab.game.getSheet());
        assert(cloth.pins.size() == 2);
        assert(near(lab.game.getHookSettings().hookGap, clothLabSheetWidth));
        const auto gap = [&] {
            return cloth.particles.positions[cloth.pins[1].particle].x - cloth.particles.positions[cloth.pins[0].particle].x;
        };
        assert(near(gap(), clothLabSheetWidth));

        lab.game.getHookSettings().hookGap = 0.4f;
        runFixedUpdate(lab.game, lab.world, 1.0f / 120.0f, 0.0);
        assert(near(gap(), clothLabSheetWidth - clothLabHookSpeed / 120.0f));

        for (int tick = 1; tick < 360; ++tick) {
            runFixedUpdate(lab.game, lab.world, 1.0f / 120.0f, tick / 120.0);
        }
        assert(near(gap(), 0.4f));
        for (const ClothPin& pin : cloth.pins) {
            const Vec3& hook = cloth.particles.positions[pin.particle];
            assert(near(hook.y, clothLabBarUnderside(layout.fabricFrame)));
            assert(near(hook.z, layout.fabricFrame.barCentre.z));
        }
        const float middle = 0.5f * (cloth.particles.positions[cloth.pins[0].particle].x + cloth.particles.positions[cloth.pins[1].particle].x);
        assert(near(middle, layout.sheetCentre.x));

        // Asked for more than the sheet is wide, the hooks stop at its width.
        lab.game.getHookSettings().hookGap = 5.0f;
        for (int tick = 0; tick < 360; ++tick) {
            runFixedUpdate(lab.game, lab.world, 1.0f / 120.0f, tick / 120.0);
        }
        assert(near(gap(), clothLabSheetWidth));
    }

    /*
    * Dragging: pressing the left button with the crosshair on the sheet grabs the point there; while it is held, the
    * hand stays at the grab's distance along the crosshair, so lifting the camera lifts the cloth; letting go drops it.
    * Out of reach, a press grabs nothing.
    */
    void testTheSheetCanBeDragged() {
        ClothLab lab;
        const Entity sheet = lab.game.getSheet();
        const Vec3 point = lab.world.get<Cloth>(sheet).particles.positions[5 * (clothLabSheetColumns + 1) + 14];
        const Vec3 above{0.0f, 2.0f, 1.5f};
        lab.game.getControls().poseFreeCamera(lab.world, point + above, point);

        Input input = firstPersonInput({});
        input.pressMouseButton(MouseButtonPress{MouseButton::Left, Vec2{800.0f, 450.0f}, 0});
        lab.game.onInput(lab.world, input);
        const std::optional<ClothGrab>& grab = lab.game.getSheetGrab();
        assert(grab.has_value());
        const ClothParticles& particles = lab.world.get<Cloth>(sheet).particles;
        assert((particles.positions[grab->particle] - point).length() < 0.06f);
        assert((grab->target - point).length() < 0.01f);

        // Held while the camera rises half a metre: the hand rises with it, and the ticks pull the point up after it.
        input.beginFrame();
        lab.game.getControls().poseFreeCamera(lab.world, point + above + Vec3{0.0f, 0.5f, 0.0f}, point + Vec3{0.0f, 0.5f, 0.0f});
        lab.game.onInput(lab.world, input);
        assert(lab.game.getSheetGrab().has_value());
        assert(near(lab.game.getSheetGrab()->target.y, point.y + 0.5f));
        for (int tick = 0; tick < 60; ++tick) {
            runFixedUpdate(lab.game, lab.world, 1.0f / 120.0f, tick / 120.0);
        }
        assert(particles.positions[lab.game.getSheetGrab()->particle].y > point.y + 0.4f);

        input.beginFrame();
        input.releaseMouseButton(MouseButton::Left);
        lab.game.onInput(lab.world, input);
        assert(!lab.game.getSheetGrab().has_value());

        // With the mouse freed (after Escape), the click that captures it again grabs nothing.
        Input freed;
        freed.beginFrame();
        freed.setWindowSize(Vec2{1600.0f, 900.0f});
        freed.setWindowState(true, false, false);
        freed.setCursor(Vec2{800.0f, 450.0f}, true);
        freed.pressMouseButton(MouseButtonPress{MouseButton::Left, Vec2{800.0f, 450.0f}, 0});
        lab.game.getControls().poseFreeCamera(lab.world, point + above, point);
        lab.game.onInput(lab.world, freed);
        assert(!lab.game.getSheetGrab().has_value());

        // From 20 m away the sheet is out of reach.
        lab.game.getControls().poseFreeCamera(lab.world, point + Vec3{0.0f, 12.0f, 16.0f}, point);
        input.beginFrame();
        input.pressMouseButton(MouseButtonPress{MouseButton::Left, Vec2{800.0f, 450.0f}, 0});
        lab.game.onInput(lab.world, input);
        assert(!lab.game.getSheetGrab().has_value());
    }

    /*
    * The bodies cloth collides with are the layout's: the ball, the pedestal, the mat, every post and bar, and the floor,
    * whose top is the ground.
    */
    void testCollidersMatchTheLayout() {
        ClothLab lab;
        const ClothLabLayout layout = buildClothLabLayout();
        const std::vector<Shape> shapes = worldColliderShapes(lab.world);
        assert(shapes.size() == 1 + layout.blocks().size() + 1);

        int balls = 0;
        bool floor = false;
        for (const Shape& shape : shapes) {
            if (const Sphere* ball = std::get_if<Sphere>(&shape)) {
                assert(near(ball->radius, layout.ball.radius));
                assert(near((ball->centre - layout.ball.centre).length(), 0.0f));
                ++balls;
            } else if (const Aabb* box = std::get_if<Aabb>(&shape)) {
                floor = floor || (near(box->maximum.y, 0.0f) && box->maximum.x >= clothLabFloorHalfSize);
            }
        }
        assert(balls == 1);
        assert(floor);
    }

    /*
    * The drape milestone: a sheet laid flat level with the drape bar, pinned at its back corners under it, falls onto
    * the ball and settles draped over it: nothing inside the ball or below the floor, part of it resting on the ball.
    */
    void testTheDrapeSheetSettlesOverTheBall() {
        ClothLab lab;
        const ClothLabLayout layout = buildClothLabLayout();
        const Entity drape = lab.game.getDrapeSheet();
        assert(lab.assets.isDeforming(lab.world.get<DeformingMesh>(drape).mesh));
        for (const Vec3& point : lab.world.get<Cloth>(drape).particles.positions) {
            assert(near(point.y, clothLabBarUnderside(layout.drapeFrame)));
        }

        for (int tick = 0; tick < 600; ++tick) {
            runFixedUpdate(lab.game, lab.world, 1.0f / 120.0f, tick / 120.0);
        }

        const Cloth& cloth = lab.world.get<Cloth>(drape);
        float closest = 1e9f;
        for (const Vec3& point : cloth.particles.positions) {
            const float fromBall = (point - layout.ball.centre).length() - layout.ball.radius;
            assert(fromBall > 0.5f * cloth.settings.thickness);
            assert(point.y >= 0.5f * cloth.settings.thickness);
            closest = std::min(closest, fromBall);
        }
        assert(closest < 2.0f * cloth.settings.thickness);
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
    testTheSheetHangs();
    testHooksSlide();
    testTheSheetCanBeDragged();
    testCollidersMatchTheLayout();
    testTheDrapeSheetSettlesOverTheBall();
    testItHasGoldenViews();
}
