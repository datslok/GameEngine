#include "app/game_registry.h"
#include "engine/engine_settings.h"
#include "engine/golden.h"
#include "engine/state_hash.h"
#include "render/gpu/shadow_map.h"
#include "scene/frame_description.h"

#include "test_jobs.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
    /*
    * A golden view as the capture runs it: a fresh game in a fresh world, set up and simulated to the view's time, on the
    * run's job system as the engine gives a game its own (on one thread the forest's eight builds here took most of the
    * whole suite's time).
    */
    struct PreparedView {
        World world;
        AssetManager assets;
        std::unique_ptr<Game> game;

        PreparedView(const GameEntry& entry, const GoldenView& view): game(entry.make()) {
            game->useJobs(testJobs());
            game->onInit(world, assets);
            advanceToGoldenView(*game, world, view, EngineSettings{}.ticksPerSecond);
        }
    };

    std::vector<GoldenView> viewsOf(const GameEntry& entry) {
        return entry.make()->goldenViews();
    }

    std::vector<GoldenView> demoViews() {
        return viewsOf(*findGame("demo"));
    }

    const GoldenView& viewNamed(const std::vector<GoldenView>& views, const std::string& name) {
        for (const GoldenView& view : views) {
            if (view.name == name) {
                return view;
            }
        }

        assert(false && "no golden view with that name");
        return views.front();
    }

    // Names become file names, so they are short, unique within their game and plain: lower-case letters, digits and
    // dashes. Every game has some.
    void testViewsHaveUsableNames() {
        assert(demoViews().size() >= 5);

        for (const GameEntry& entry : registeredGames()) {
            const std::vector<GoldenView> views = viewsOf(entry);
            std::set<std::string> names;
            assert(!views.empty());

            for (const GoldenView& view : views) {
                assert(isGoldenViewName(view.name));
                assert(names.insert(view.name).second);
                assert(view.simulationSeconds >= 0.0);
            }
        }

        assert(isGoldenViewName("lamp-field"));
        assert(!isGoldenViewName(""));
        assert(!isGoldenViewName("Lamp field"));
        assert(!isGoldenViewName("../escape"));
    }

    // HDR output is captured too, with the same fixed display numbers on every machine.
    void testSomeViewIsHdr() {
        bool hdr = false;

        for (const GoldenView& view : demoViews()) {
            hdr = hdr || view.hdr;
        }

        assert(hdr);

        const DisplayOutput output = goldenDisplayOutput(true);
        assert(output.hdr);
        assert(output.peakBrightness > 1.0f);
        assert(!goldenDisplayOutput(false).hdr);
    }

    /*
    * The view of each game built a second time to compare: the one where the most goes on (the forest's clock moved to
    * dusk, the light lab's flashlight switched on, the demo's lamp field after the longest simulation), else a game's
    * longest. Building every view twice cost half this test's time (the forest's full valley eight times); the
    * simulation's own determinism is checked by testDeterminism.
    */
    const GoldenView& viewBuiltTwice(const GameEntry& entry, const std::vector<GoldenView>& views) {
        const std::string game = entry.name;
        const char* chosen = game == "forest" ? "rim-dusk" : game == "lightlab" ? "flashlight" : game == "demo" ? "lamp-field" : nullptr;

        if (chosen != nullptr) {
            return viewNamed(views, chosen);
        }

        return *std::max_element(views.begin(), views.end(),
                                 [](const GoldenView& a, const GoldenView& b) { return a.simulationSeconds < b.simulationSeconds; });
    }

    /*
    * The point of a golden image: the same view gives the same picture every time. Every view's pose holds still through
    * further frames without input, and two fresh runs of each game's chosen view (viewBuiltTwice) reach the same
    * simulation state and the same camera pose.
    */
    void testViewsAreDeterministic() {
        for (const GameEntry& entry : registeredGames()) {
            const std::vector<GoldenView> views = viewsOf(entry);
            const GoldenView& twice = viewBuiltTwice(entry, views);

            for (const GoldenView& view : views) {
                PreparedView first{entry, view};
                const Vec3 position = first.game->getCamera().getPosition();
                const Vec3 forward = first.game->getCamera().getForward();

                if (&view == &twice) {
                    PreparedView second{entry, view};
                    assert(hashSimulationState(first.world) == hashSimulationState(second.world));
                    assert(position.x == second.game->getCamera().getPosition().x);
                    assert(position.y == second.game->getCamera().getPosition().y);
                    assert(position.z == second.game->getCamera().getPosition().z);
                }

                first.game->onUpdate(first.world, Input{}, 1.0f / 120.0f, 1.0f);
                assert(first.game->getCamera().getPosition().x == position.x);
                assert(first.game->getCamera().getForward().z == forward.z);
            }
        }
    }

    // The lamp field is there to show light priority at work, so it needs more lamps than there are point light shadows.
    void testLampFieldHasMoreLightsThanShadows() {
        const std::vector<GoldenView> views = demoViews();
        PreparedView lamps{*findGame("demo"), viewNamed(views, "lamp-field")};
        const FrameDescription frame = buildFrame(lamps.world, lamps.game->getCamera(), 1.0f);

        assert(frame.lighting.pointLights.size() > static_cast<std::size_t>(maxShadowedPointLights));
    }

    /*
    * Fast lights' bounce light (the light probes' fast atlas) is in a golden image: the light lab's back room lit by the
    * flashlight, which is on there (and off in every other view, which keep the sunlit picture).
    */
    void testAViewShowsAFastLightsBounce() {
        const GameEntry& lab = *findGame("lightlab");
        const std::vector<GoldenView> views = viewsOf(lab);

        for (const GoldenView& view : views) {
            PreparedView prepared{lab, view};
            const FrameDescription frame = buildFrame(prepared.world, prepared.game->getCamera(), 1.0f);
            const bool fastLight = std::any_of(frame.lighting.spotLights.begin(), frame.lighting.spotLights.end(),
                                               [](const PlacedSpotLight& placed) { return placed.light.fast; });
            assert(fastLight == (view.name == "flashlight"));
            assert(frame.lighting.probeCascades.has_value());
        }
    }
}

void testGoldenViews() {
    testViewsHaveUsableNames();
    testSomeViewIsHdr();
    testViewsAreDeterministic();
    testLampFieldHasMoreLightsThanShadows();
    testAViewShowsAFastLightsBounce();
}
