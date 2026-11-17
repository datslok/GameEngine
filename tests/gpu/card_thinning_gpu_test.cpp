#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "scene/card_thinning.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("card_thinning_test");

    // Thinnings from small cards growing slowly to large ones growing fast, the least share from a tenth to whole.
    CardThinning randomThinning(CaseRandom& random) {
        return CardThinning{.cardSize = random.between(0.1f, 4.0f), .growth = random.between(0.1f, 1.0f), .leastShare = random.between(0.1f, 1.0f)};
    }

    // The share kept at an error, and that share's error, from none allowed to far more than the least share's.
    void checkShare(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{41};
        std::vector<CardThinning> thinnings;
        std::vector<float> errors;
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            thinnings.push_back(randomThinning(random));
            errors.push_back(i % 10 == 0 ? 0.0f : random.between(0.0f, 4.0f));
            inputs.push_back(gpuVec4(thinnings.back().cardSize, thinnings.back().growth, thinnings.back().leastShare, errors.back()));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_THINNED_SHARE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            const float share = thinnedShare(thinnings[i], errors[i]);
            report.check("thinnedShare", transcendentalTolerance, share, outputs[i][0]);
            report.check("thinningError", transcendentalTolerance, thinningError(thinnings[i], share), outputs[i][1]);
        }
    }

    // A card's corner and its presence at shares round its rank, kept cards (rank 0) among them.
    void checkCard(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{43};
        std::vector<GpuVec4> inputs;
        std::vector<CardThinning> thinnings;
        std::vector<float> shares;
        std::vector<Vec3> positions;
        std::vector<ThinningCard> cards;

        for (int i = 0; i < 500; ++i) {
            thinnings.push_back(randomThinning(random));
            cards.push_back(ThinningCard{.anchor = Vec3{random.between(-20.0f, 20.0f), random.between(0.0f, 40.0f), random.between(-20.0f, 20.0f)},
                                         .rank = i % 4 == 0 ? 0.0f : random.between(0.1f, 1.0f)});
            shares.push_back(i % 4 == 1 ? cards.back().rank - 0.5f * cardThinningBand : random.between(thinnings.back().leastShare, 1.0f));
            positions.push_back(cards.back().anchor + Vec3{random.between(-2.0f, 2.0f), random.between(-2.0f, 2.0f), random.between(-2.0f, 2.0f)});
            inputs.push_back(gpuVec4(thinnings.back().cardSize, thinnings.back().growth, thinnings.back().leastShare, shares.back()));
            inputs.push_back(gpuVec4(positions.back(), cards.back().rank));
            inputs.push_back(gpuVec4(cards.back().anchor, 0.0f));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_THINNED_CARD, inputs, 3, 1);

        for (std::size_t i = 0; i < cards.size(); ++i) {
            const Vec3 expected = thinnedPosition(positions[i], cards[i], thinnedCardScale(thinnings[i], cards[i].rank, shares[i]));
            const FloatTolerance corner{.absolute = 4e-5f, .relative = 1e-4f};
            report.check("thinnedPosition", corner, expected.x, outputs[i][0]);
            report.check("thinnedPosition", corner, expected.y, outputs[i][1]);
            report.check("thinnedPosition", corner, expected.z, outputs[i][2]);
            report.check("cardPresence", arithmeticTolerance, cardPresence(cards[i].rank, shares[i]), outputs[i][3]);
        }
    }
}

void gpuTestCardThinning(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("card_thinning.glsl");
    checkShare(runner, report);
    checkCard(runner, report);
}
