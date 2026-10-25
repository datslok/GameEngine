#include "cloth/cloth_yardstick.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace {
    constexpr float tick = 1.0f / 120.0f;

    // Pinned at the top corners, upright and at rest: no stretch, the hem's middle a sheet's height below the pins.
    void testSheetAtRest() {
        const Cloth cloth = yardstickSheet(4, ClothSettings{});
        assert(cloth.particles.size() == 25);
        assert(cloth.pins.size() == 2);
        assert(cloth.particles.inverseMasses[20] == 0.0f && cloth.particles.inverseMasses[24] == 0.0f);
        assert(near(cloth.particles.positions[20].x, -0.5f, 1e-6f) && near(cloth.particles.positions[20].y, 0.0f, 1e-6f));
        assert(near(cloth.particles.positions[24].x, 0.5f, 1e-6f) && near(cloth.particles.positions[2].y, -1.0f, 1e-6f));

        const SheetMeasure rest = measureSheet(cloth, 4);
        assert(near(rest.pathStretch, 0.0f, 1e-6f));
        assert(near(rest.worstEdgeStrain, 0.0f, 1e-6f));
        assert(near(rest.sag, 1.0f, 1e-6f));
    }

    // A sheet grown by 10% about a pin measures 10% stretch everywhere, and a hem 1.1 m down.
    void testMeasureGrownSheet() {
        Cloth cloth = yardstickSheet(4, ClothSettings{});
        const Vec3 pin = cloth.particles.positions[20];
        for (Vec3& position : cloth.particles.positions) {
            position = pin + (position - pin) * 1.1f;
        }
        const SheetMeasure grown = measureSheet(cloth, 4);
        assert(near(grown.pathStretch, 0.1f, 1e-5f));
        assert(near(grown.worstEdgeStrain, 0.1f, 1e-5f));
        assert(near(grown.sag, 1.1f, 1e-5f));
    }

    // One edge pulled long: the longest column takes its share, the worst edge all of it, the sag the pull.
    void testMeasureOneLongEdge() {
        Cloth cloth = yardstickSheet(4, ClothSettings{});
        cloth.particles.positions[2].y -= 0.1f;
        const SheetMeasure pulled = measureSheet(cloth, 4);
        assert(near(pulled.pathStretch, 0.1f, 1e-5f));
        assert(near(pulled.worstEdgeStrain, 0.4f, 1e-5f));
        assert(near(pulled.sag, 1.1f, 1e-5f));
        assert(pulled.worstEdgeStrain >= pulled.pathStretch);
    }

    // An odd sheet has no middle point on its hem: its sag is the mean of the two beside the middle.
    void testOddSheetSag() {
        Cloth cloth = yardstickSheet(5, ClothSettings{});
        assert(near(measureSheet(cloth, 5).sag, 1.0f, 1e-6f));
        cloth.particles.positions[2].y -= 0.1f;
        assert(near(measureSheet(cloth, 5).sag, 1.05f, 1e-5f));
    }

    void testBadSheets() {
        for (const int squares : {0, -2}) {
            bool threw = false;
            try {
                yardstickSheet(squares, ClothSettings{});
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            assert(threw);
        }
    }

    // A sheet with nothing pulling it does not move; the settling's motion is measured over the last second only.
    void testStillSheetDoesNotMove() {
        ClothSettings weightless;
        weightless.gravity = 0.0f;
        Cloth cloth = yardstickSheet(4, weightless);
        assert(settleYardstick(cloth, 2.0f, tick) == 0.0f);
    }

    /*
    * A settled sheet stays on its pins, hangs about a sheet's height, stretches a little and has nearly stopped. More
    * substeps a tick carry the stiffness further across it, so it stretches less.
    */
    void testSettledSheet() {
        Cloth few = yardstickSheet(10, ClothSettings{});
        const float motion = settleYardstick(few, 4.0f, tick);
        assert(motion >= 0.0f && motion < 0.01f);
        assert(near(few.particles.positions[110].x, -0.5f, 1e-6f) && near(few.particles.positions[120].y, 0.0f, 1e-6f));

        const SheetMeasure settled = measureSheet(few, 10);
        assert(settled.pathStretch > 0.0f && settled.pathStretch < 0.05f);
        assert(settled.worstEdgeStrain >= settled.pathStretch);
        assert(settled.sag > 0.9f && settled.sag < 1.1f);

        ClothSettings more;
        more.substeps = 60;
        Cloth many = yardstickSheet(10, more);
        settleYardstick(many, 4.0f, tick);
        assert(measureSheet(many, 10).pathStretch < settled.pathStretch);
    }

    /*
    * The problem the yardstick is for, as today's solver has it: the same fabric on a mesh twice as fine stretches
    * more, since each pass carries a correction only about one edge. A solver with a low-frequency layer should turn
    * this test round (docs/CLOTHGPU.md).
    */
    void testFinerSheetStretchesMore() {
        Cloth coarse = yardstickSheet(10, ClothSettings{});
        Cloth fine = yardstickSheet(20, ClothSettings{});
        settleYardstick(coarse, 4.0f, tick);
        settleYardstick(fine, 4.0f, tick);
        assert(measureSheet(fine, 20).pathStretch > measureSheet(coarse, 10).pathStretch);
    }
}

void testClothYardstick() {
    testSheetAtRest();
    testMeasureGrownSheet();
    testMeasureOneLongEdge();
    testOddSheetSag();
    testBadSheets();
    testStillSheetDoesNotMove();
    testSettledSheet();
    testFinerSheetStretchesMore();
}
