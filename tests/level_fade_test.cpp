#include "render/gpu/level_fade.h"

#include <cassert>
#include <cstdint>

namespace {
    /*
    * A field is never 0 (0 is no fade): its step runs 1 to 63 across the band, the coarser level's with its bit set;
    * shares outside 0..1 hold to the ends. A draw list entry keeps its instance and thinned bit beside it.
    */
    void testTheFieldStepsAcrossTheBand() {
        assert(levelFadeField(0.0f, false) == 1u && levelFadeField(0.0f, true) == (drawListFadeCoarse | 1u));
        assert(levelFadeField(1.0f, false) == drawListFadeSteps && levelFadeField(2.0f, true) == (drawListFadeCoarse | drawListFadeSteps));
        assert(levelFadeField(-1.0f, false) == 1u);
        assert(levelFadeField(0.5f, false) == 32u);

        const std::uint32_t entry = 123456u | DRAW_LIST_THINNED_BIT | (levelFadeField(0.25f, true) << drawListFadeShift);
        assert(drawListFade(entry) == levelFadeField(0.25f, true));
        assert((entry & drawListInstanceMask) == 123456u && (entry & DRAW_LIST_THINNED_BIT) != 0u);
        assert(drawListFade(123456u | DRAW_LIST_THINNED_BIT) == 0u);
        assert(drawListInstances == 16777216u);
    }

    /*
    * The two levels of one draw split every pixel: whatever the noise, exactly one keeps it, so no hole opens and nothing
    * is drawn twice; a level not fading keeps them all. The coarser's share of the pixels follows the band's share.
    */
    void testTheTwoLevelsSplitEveryPixel() {
        for (int step = 0; step <= 100; ++step) {
            const float share = static_cast<float>(step) / 100.0f;
            const std::uint32_t finer = levelFadeField(share, false);
            const std::uint32_t coarser = levelFadeField(share, true);
            int keptByCoarser = 0;
            const int pixels = 128;

            for (int y = 0; y < pixels; ++y) {
                for (int x = 0; x < pixels; ++x) {
                    const float noise = levelFadeNoise(static_cast<float>(x), static_cast<float>(y), static_cast<std::uint32_t>(step));
                    assert(noise >= 0.0f && noise < 1.0f);
                    assert(levelFadeKeeps(finer, noise) != levelFadeKeeps(coarser, noise));
                    assert(levelFadeKeeps(0u, noise));
                    keptByCoarser += levelFadeKeeps(coarser, noise) ? 1 : 0;
                }
            }

            // Within a step and a little noise of the share: neither level ever keeps all or none.
            const float kept = static_cast<float>(keptByCoarser) / static_cast<float>(pixels * pixels);
            assert(kept > share - 0.03f && kept < share + 0.03f);
            assert(keptByCoarser > 0 && keptByCoarser < pixels * pixels);
        }
    }

    // The noise turns from frame to frame, so temporal anti-aliasing sees each pixel kept by both levels over time.
    void testTheNoiseTurnsEachFrame() {
        int changed = 0;

        for (int x = 0; x < 64; ++x) {
            const float now = levelFadeNoise(static_cast<float>(x), 7.0f, 10u);
            const float next = levelFadeNoise(static_cast<float>(x), 7.0f, 11u);
            changed += now != next ? 1 : 0;
        }

        assert(changed > 60);
    }
}

/*
* Levels cross-faded by a dither (render/gpu/level_fade.h): the draw list's fade field and the keep test the depth
* prepass and the velocity pass make.
*/
void testLevelFade() {
    testTheFieldStepsAcrossTheBand();
    testTheTwoLevelsSplitEveryPixel();
    testTheNoiseTurnsEachFrame();
}
