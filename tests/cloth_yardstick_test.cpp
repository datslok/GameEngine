#include "cloth/cloth_yardstick.h"
#include "cloth_helpers.h"
#include "physics/signed_distance_field.h"
#include "scene/collision_mesh.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

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
        Cloth few = yardstickSheet(10, untetheredSettings());
        const float motion = settleYardstick(few, 4.0f, tick);
        assert(motion >= 0.0f && motion < 0.01f);
        assert(near(few.particles.positions[110].x, -0.5f, 1e-6f) && near(few.particles.positions[120].y, 0.0f, 1e-6f));

        const SheetMeasure settled = measureSheet(few, 10);
        assert(settled.pathStretch > 0.0f && settled.pathStretch < 0.05f);
        assert(settled.worstEdgeStrain >= settled.pathStretch);
        assert(settled.sag > 0.9f && settled.sag < 1.1f);

        ClothSettings more = untetheredSettings();
        more.substeps = 60;
        Cloth many = yardstickSheet(10, more);
        settleYardstick(many, 4.0f, tick);
        assert(measureSheet(many, 10).pathStretch < settled.pathStretch);
    }

    /*
    * The problem the yardstick is for, as today's solver has it: the same fabric on a mesh twice as fine stretches
    * more, since each pass carries a correction only about one edge (the tethers off: they hide it). A solver with a
    * low-frequency layer should turn this test round (docs/CLOTHGPU.md).
    */
    void testFinerSheetStretchesMore() {
        Cloth coarse = yardstickSheet(10, untetheredSettings());
        Cloth fine = yardstickSheet(20, untetheredSettings());
        settleYardstick(coarse, 4.0f, tick);
        settleYardstick(fine, 4.0f, tick);
        assert(measureSheet(fine, 20).pathStretch > measureSheet(coarse, 10).pathStretch);
    }

    // The hem of a yardstick sheet: its bottom row, points 0 to squares.
    std::vector<std::uint32_t> sheetHem(int squares) {
        std::vector<std::uint32_t> hem;
        for (int column = 0; column <= squares; ++column) {
            hem.push_back(static_cast<std::uint32_t>(column));
        }
        return hem;
    }

    /*
    * The tick's preparation runs once before each tick, in order, and what it does is stepped: a weightless sheet
    * stays still unless its pin is moved.
    */
    void testSettlingPreparesEachTick() {
        ClothSettings weightless;
        weightless.gravity = 0.0f;
        Cloth cloth = yardstickSheet(4, weightless);
        std::vector<int> prepared;
        settleYardstick(cloth, 0.5f, tick, serialJobs(), {}, [&](Cloth&, int ticks) { prepared.push_back(ticks); });
        assert(prepared.size() == 60);
        for (std::size_t index = 0; index < prepared.size(); ++index) {
            assert(prepared[index] == static_cast<int>(index));
        }
        assert(near(cloth.particles.positions[20].x, -0.5f, 1e-6f));

        const float motion = settleYardstick(cloth, 0.5f, tick, serialJobs(), {}, [](Cloth& moving, int) { moving.pins[0].position.x -= 0.001f; });
        assert(near(cloth.particles.positions[20].x, -0.56f, 1e-5f));
        assert(motion > 0.05f);
    }

    // A garment at rest: nothing stretched, the seams shut, its hem where it hangs, no body near.
    void testGarmentAtRest() {
        const Cloth cloth = yardstickSheet(20, ClothSettings{});
        const GarmentMeasure rest = measureGarment(cloth, sheetHem(20), -2.0f);
        assert(rest.meanStretch == 0.0f && near(rest.percentileStrain, 0.0f, 1e-6f) && near(rest.worstEdgeStrain, 0.0f, 1e-6f));
        assert(rest.worstSeamGap == 0.0f);
        assert(near(rest.hemHeight, 1.0f, 1e-6f));
        assert(rest.closestToBody == std::numeric_limits<float>::infinity());
    }

    // Grown by 10% about a pin: every edge 10% long, so the mean, the percentile and the worst all read 10%.
    void testGarmentGrown() {
        Cloth cloth = yardstickSheet(20, ClothSettings{});
        const Vec3 pin = cloth.particles.positions[cloth.pins[0].particle];
        for (Vec3& position : cloth.particles.positions) {
            position = pin + (position - pin) * 1.1f;
        }
        const GarmentMeasure grown = measureGarment(cloth, sheetHem(20), -2.0f);
        assert(near(grown.meanStretch, 0.1f, 1e-5f) && near(grown.percentileStrain, 0.1f, 1e-5f) && near(grown.worstEdgeStrain, 0.1f, 1e-5f));
        assert(near(grown.hemHeight, 0.9f, 1e-5f));
    }

    /*
    * One point pulled down half a square: the edge above it twice its length, the worst; the few edges round it are
    * fewer than 1% of the sheet's 1,240, so the percentile does not see them; the edges below are squashed, which is not
    * stretch, so the mean counts only the stretched ones, spread over every edge.
    */
    void testGarmentOneLongEdge() {
        Cloth cloth = yardstickSheet(20, ClothSettings{});
        assert(cloth.stretch.size() == 1240);
        cloth.particles.positions[10 * 21 + 10].y -= 0.025f;
        const GarmentMeasure pulled = measureGarment(cloth, sheetHem(20), -2.0f);
        assert(near(pulled.worstEdgeStrain, 0.5f, 1e-4f));
        assert(near(pulled.percentileStrain, 0.0f, 1e-6f));
        assert(pulled.meanStretch > 0.0f && pulled.meanStretch < 0.01f * pulled.worstEdgeStrain);
    }

    // A seam's gap is the widest any of its stitches stands open.
    void testGarmentSeamGap() {
        Cloth cloth = yardstickSheet(4, ClothSettings{});
        cloth.stitches = {ClothStitch{0, 1}, ClothStitch{5, 6}};
        cloth.particles.positions[6].x += 0.05f;
        assert(near(measureGarment(cloth, sheetHem(4), -2.0f).worstSeamGap, 0.3f, 1e-5f));
    }

    // The point nearest a body, by its distance field: negative inside it.
    void testGarmentNearABody() {
        const Vec3 ball{5.0f, 0.0f, 0.0f};
        const std::shared_ptr<const SignedDistanceField> field = std::make_shared<const SignedDistanceField>(buildSignedDistanceField(
            collisionMesh(Mesh::sphere(64, 32), Mat4::translation(ball.x, ball.y, ball.z) * Mat4::scaling(0.4f, 0.4f, 0.4f)), 0.01f, 0.03f));
        Cloth cloth = yardstickSheet(4, ClothSettings{});
        assert(measureGarment(cloth, sheetHem(4), -2.0f, {field}).closestToBody == field->band);
        cloth.particles.positions[3] = ball + Vec3{0.0f, 1.0f, 0.0f} * 0.39f;
        assert(near(measureGarment(cloth, sheetHem(4), -2.0f, {field}).closestToBody, -0.01f, 0.002f));
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
    testSettlingPreparesEachTick();
    testGarmentAtRest();
    testGarmentGrown();
    testGarmentOneLongEdge();
    testGarmentSeamGap();
    testGarmentNearABody();
}
