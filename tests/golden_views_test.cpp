#include "app/game_registry.h"
#include "engine/engine_settings.h"
#include "engine/golden.h"
#include "engine/state_hash.h"
#include "render/gpu/shadow_map.h"
#include "scene/frame_description.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
    // A golden view as the capture runs it: a fresh game in a fresh world, set up and simulated to the view's time.
    struct PreparedView {
        World world;
        AssetManager assets;
        std::unique_ptr<Game> game;

        PreparedView(const GameEntry& entry, const GoldenView& view): game(entry.make()) {
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
    * The point of a golden image: the same view gives the same picture every time. Two fresh runs of each view reach the
    * same simulation state and the same camera pose, and the pose holds still through further frames without input.
    */
    void testViewsAreDeterministic() {
        for (const GameEntry& entry : registeredGames()) {
            for (const GoldenView& view : viewsOf(entry)) {
                PreparedView first{entry, view};
                PreparedView second{entry, view};

                assert(hashSimulationState(first.world) == hashSimulationState(second.world));

                const Vec3 position = first.game->getCamera().getPosition();
                const Vec3 forward = first.game->getCamera().getForward();
                assert(position.x == second.game->getCamera().getPosition().x);
                assert(position.y == second.game->getCamera().getPosition().y);
                assert(position.z == second.game->getCamera().getPosition().z);

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
            assert(frame.lighting.probeVolumes.size() == 1);
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
