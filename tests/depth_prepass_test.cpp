#include "engine/settings_file.h"
#include "render/gpu/depth_prepass.h"
#include "render/gpu/render_settings.h"

#include <cassert>
#include <string>
#include <vector>

namespace {
    SettingsWalk walkOf(RenderSettings& settings) {
        return [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
    }

    // On by default, and switchable while the game runs (the debug UI and settings.cfg reach it through visit()).
    void testPrepassIsOnByDefaultAndSwitchesLive() {
        const RenderSettings defaults;
        assert(defaults.depthPrepass);

        RenderSettings settings;
        const std::vector<std::string> warnings = applySettings({{"passes.depthPrepass", "false"}}, walkOf(settings), StartupFields::Keep);
        assert(warnings.empty());
        assert(!settings.depthPrepass);
    }

    // The prepass finds the nearest surface the way the main pass always has: reversed-Z, nearer is greater, written.
    void testPrepassKeepsTheNearestDepth() {
        const DepthState prepass = prepassDepthState();
        assert(prepass.compare == DepthCompare::Greater);
        assert(prepass.write);
    }

    /*
    * Without a prepass the main pass finds the nearest surface itself. After one, the depth image already holds it, so
    * the main pass only shades the fragment whose depth equals it, and writes nothing: each pixel is shaded once.
    */
    void testMainPassDepthFollowsThePrepass() {
        const DepthState alone = mainPassDepthState(false);
        assert(alone.compare == DepthCompare::Greater);
        assert(alone.write);

        const DepthState afterPrepass = mainPassDepthState(true);
        assert(afterPrepass.compare == DepthCompare::Equal);
        assert(!afterPrepass.write);
    }
}

void testDepthPrepass() {
    testPrepassIsOnByDefaultAndSwitchesLive();
    testPrepassKeepsTheNearestDepth();
    testMainPassDepthFollowsThePrepass();
}
