#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "scene/planting.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/*
* planting.glsl against scene/planting.cpp: every result exact, since the GPU must plant what the CPU does (the valley's
* stage 2, docs/research/VALLEY.md). The cases sit on the decisions' edges as often as off them (a radius equal to the
* distance, a threshold equal to the share), where a fused multiply and add or a looser rounding would show.
*/
namespace {
    const std::string shader = gpuTestShader("planting_test");

    // A 64-bit number as four 16-bit pieces, each a whole float (planting_test.comp's fromPieces).
    GpuVec4 pieces(std::uint64_t value) {
        return gpuVec4(static_cast<float>(value & 0xffff), static_cast<float>((value >> 16) & 0xffff), static_cast<float>((value >> 32) & 0xffff),
                       static_cast<float>((value >> 48) & 0xffff));
    }

    std::uint64_t random64(CaseRandom& random) {
        std::uint64_t value = 0;

        for (int piece = 0; piece < 4; ++piece) {
            value = (value << 16) | static_cast<std::uint64_t>(random.between(0.0f, 65535.0f));
        }

        return value;
    }

    float whole(CaseRandom& random, float lowest, float highest) {
        return std::floor(random.between(lowest, highest));
    }

    float flag(bool value) {
        return value ? 1.0f : 0.0f;
    }

    void checkRandom(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{41};
        std::vector<GpuVec4> inputs;
        std::vector<std::uint64_t> seeds;
        std::vector<std::uint64_t> indices;
        std::vector<std::pair<int, int>> cells;

        for (int i = 0; i < 2000; ++i) {
            seeds.push_back(i < 100 ? static_cast<std::uint64_t>(i) : random64(random));
            indices.push_back(i % 3 == 0 ? static_cast<std::uint64_t>(i) : random64(random));
            cells.push_back({static_cast<int>(whole(random, -40000.0f, 40000.0f)), static_cast<int>(whole(random, -40000.0f, 40000.0f))});
            inputs.push_back(pieces(seeds.back()));
            inputs.push_back(pieces(indices.back()));
            inputs.push_back(gpuVec4(static_cast<float>(cells.back().first), static_cast<float>(cells.back().second)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_RANDOM, inputs, 3, 2);

        for (std::size_t i = 0; i < seeds.size(); ++i) {
            report.check("plantingRandom", exactTolerance, plantingRandom(seeds[i], indices[i]), outputs[2 * i][0]);
            const GpuVec4 key = pieces(plantingCellKey(cells[i].first, cells[i].second));

            for (std::size_t piece = 0; piece < 4; ++piece) {
                report.check("plantingCellKey", exactTolerance, key[piece], outputs[2 * i + 1][piece]);
            }
        }
    }

    void checkThreshold(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{43};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            inputs.push_back(gpuVec4(whole(random, -1e6f, 1e6f), whole(random, -1e6f, 1e6f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_THRESHOLD, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("plantingThreshold", exactTolerance, plantingThreshold(static_cast<int>(inputs[i][0]), static_cast<int>(inputs[i][1])), outputs[i][0]);
        }
    }

    // Shares from bytes and densities as the maps give them, a threshold half the time equal to the share's band's edge.
    void checkShare(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{47};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            const std::array<float, 3> weights{random.between(0.0f, 255.0f), random.between(0.0f, 255.0f), random.between(0.0f, 255.0f)};
            const std::array<float, 3> perWeight{random.between(0.0f, 0.01f), random.between(0.0f, 0.01f), random.between(0.0f, 0.01f)};
            const float gap = random.between(0.0f, 255.0f);
            const float gapStep = random.between(-0.004f, 0.004f);
            const float pathShare = random.between(0.2f, 1.0f);
            const float near = random.between(0.0f, 30.0f);
            const float far = near + random.between(0.0f, 60.0f);
            const float distance = i % 5 == 0 ? near : random.between(0.0f, 100.0f);
            const float below = i % 2 == 0 ? 0.0f : random.between(0.0f, 0.5f);
            const float share = plantingShare(weights, perWeight, gap, gapStep, pathShare);
            const float threshold = i % 4 == 0 ? below + std::min(share, 1.0f - below) : random.between(0.0f, 1.0f);
            inputs.push_back(gpuVec4(weights[0], weights[1], weights[2], gap));
            inputs.push_back(gpuVec4(perWeight[0], perWeight[1], perWeight[2], gapStep));
            inputs.push_back(gpuVec4(pathShare, distance, near, far));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), far > near ? 1.0f / (far - near) : 0.0f, threshold, below));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_SHARE, inputs, 4, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& a = inputs[4 * i];
            const GpuVec4& b = inputs[4 * i + 1];
            const GpuVec4& c = inputs[4 * i + 2];
            const GpuVec4& d = inputs[4 * i + 3];
            const float share = plantingShare({a[0], a[1], a[2]}, {b[0], b[1], b[2]}, a[3], b[3], c[0]);
            report.check("plantingShare", exactTolerance, share, outputs[i][0]);
            report.check("plantingPathShare", exactTolerance, plantingPathShare(c[1], c[2], c[3], d[0], d[1]), outputs[i][1]);
            report.check("plantingTaken", exactTolerance, flag(plantingTaken(d[2], d[3], share)), outputs[i][2]);
        }
    }

    void checkPlace(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{53};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            const float footprint = random.between(0.5f, 20.0f);
            inputs.push_back(gpuVec4(whole(random, -5000.0f, 5000.0f), whole(random, -5000.0f, 5000.0f), footprint, footprint * random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.5f, 1.0f), random.between(0.0f, 0.6f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_PLACE, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& a = inputs[3 * i];
            const GpuVec4& b = inputs[3 * i + 1];
            const GpuVec4& c = inputs[3 * i + 2];
            const Vec2 place = plantingJitteredPlace(static_cast<int>(a[0]), static_cast<int>(a[1]), a[2], a[3], b[0], b[1]);
            report.check("plantingJitteredPlace", exactTolerance, place.x, outputs[i][0]);
            report.check("plantingJitteredPlace", exactTolerance, place.y, outputs[i][1]);
            report.check("plantingScale", exactTolerance, plantingScale(b[2], b[3], c[0]), outputs[i][2]);
            report.check("plantingTurn", exactTolerance, plantingTurn(c[1]), outputs[i][3]);
        }
    }

    // Draws on a kind's edges (k / count, a cumulative share) as well as between.
    void checkKind(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{59};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            const float count = whole(random, 1.0f, 33.0f);
            const float shared = whole(random, 1.0f, 6.0f);
            const float first = random.between(0.0f, 0.5f);
            const GpuVec4 cumulative = gpuVec4(first, first + random.between(0.0f, 0.3f), 0.85f, 0.95f);
            const float draw = i % 3 == 0 ? whole(random, 0.0f, count) / count : (i % 3 == 1 ? cumulative[i % 4] : random.between(0.0f, 1.0f));
            const float cover = static_cast<float>(plantingCoverValue(i % plantingMostKinds, (i / 7) % 4));
            inputs.push_back(gpuVec4(draw, count, shared, cover));
            inputs.push_back(cumulative);
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_KIND, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& a = inputs[2 * i];
            const GpuVec4& b = inputs[2 * i + 1];
            report.check("plantingUniformKind", exactTolerance, static_cast<float>(plantingUniformKind(a[0], static_cast<int>(a[1]))), outputs[i][0]);
            const std::array<float, plantingMostSharedKinds - 1> cumulative{b[0], b[1], b[2], b[3]};
            report.check("plantingSharedKind", exactTolerance, static_cast<float>(plantingSharedKind(a[0], cumulative, static_cast<int>(a[2]))), outputs[i][1]);
            report.check("plantingCoverKind", exactTolerance, static_cast<float>(plantingCoverKind(static_cast<int>(a[3]))), outputs[i][2]);
            report.check("plantingCoverTurn", exactTolerance, plantingCoverTurn(static_cast<int>(a[3])), outputs[i][3]);
        }
    }

    // Radii and caps equal to what they are compared with half the time.
    void checkTests(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{61};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            const Vec2 place{random.between(-3000.0f, 3000.0f), random.between(-5000.0f, 800.0f)};
            const Vec2 other{place.x + random.between(-30.0f, 30.0f), place.y + random.between(-30.0f, 30.0f)};
            const float apart = std::sqrt((place.x - other.x) * (place.x - other.x) + (place.y - other.y) * (place.y - other.y));
            const float radius = i % 2 == 0 ? apart : random.between(0.0f, 40.0f);
            const float reach = random.between(0.5f, 20.0f);
            const float clearance = i % 3 == 0 ? reach : random.between(0.0f, 60.0f);
            const float footHeight = random.between(-50.0f, 400.0f);
            const float centre = random.between(0.0f, 1.0f) * 30.0f;
            const float scale = random.between(0.8f, 1.2f);
            const float top = plantingTop(footHeight, centre, scale, reach);
            const float corridor = random.between(0.0f, 5.0f);
            const float cap = i % 3 == 1 ? top + corridor : random.between(-50.0f, 500.0f);
            inputs.push_back(gpuVec4(place.x, place.y, other.x, other.y));
            inputs.push_back(gpuVec4(radius, clearance, reach, top));
            inputs.push_back(gpuVec4(corridor, cap, footHeight, centre));
            inputs.push_back(gpuVec4(scale));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_TESTS, inputs, 4, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& a = inputs[4 * i];
            const GpuVec4& b = inputs[4 * i + 1];
            const GpuVec4& c = inputs[4 * i + 2];
            const GpuVec4& d = inputs[4 * i + 3];
            report.check("plantingWithin", exactTolerance, flag(plantingWithin(Vec2{a[0], a[1]}, Vec2{a[2], a[3]}, b[0])), outputs[i][0]);
            report.check("plantingFits", exactTolerance, flag(plantingFits(b[1], b[2], b[3], c[0], c[1])), outputs[i][1]);
            report.check("plantingTop", exactTolerance, plantingTop(c[2], c[3], d[0], b[2]), outputs[i][2]);
        }
    }

    void checkStored(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{67};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            inputs.push_back(gpuVec4(whole(random, 0.0f, 65535.0f), i % 2 == 0 ? plantingDecimetre : plantingCentimetre, random.between(0.0f, 1000.0f),
                                     random.between(-32767.0f, 32767.0f)));
            inputs.push_back(gpuVec4(random.between(-5000.0f, 5000.0f), random.between(-5000.0f, 5000.0f), 1.0f / random.between(0.5f, 16.0f), random.between(0.0f, 1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_STORED, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& a = inputs[2 * i];
            const GpuVec4& b = inputs[2 * i + 1];
            report.check("plantingStored", exactTolerance, plantingStored(a[0], a[1], a[2]), outputs[i][0]);
            report.check("plantingPathDistance", exactTolerance, plantingPathDistance(a[3]), outputs[i][1]);
            report.check("plantingCoordinate", exactTolerance, plantingCoordinate(b[0], b[1], b[2]), outputs[i][2]);
            report.check("plantingBetween", exactTolerance, plantingBetween(a[2], b[0], b[3]), outputs[i][3]);
        }
    }

    void checkRing(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{71};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            inputs.push_back(gpuVec4(random.between(-3000.0f, 3000.0f), random.between(-5000.0f, 800.0f), random.between(0.0f, 3.0f), static_cast<float>(i % 8)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_RING, inputs, 1, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const Vec2 point = plantingRingPoint(Vec2{inputs[i][0], inputs[i][1]}, inputs[i][2], static_cast<int>(inputs[i][3]));
            report.check("plantingRingPoint", exactTolerance, point.x, outputs[i][0]);
            report.check("plantingRingPoint", exactTolerance, point.y, outputs[i][1]);
        }
    }

    // Neighbours a spacing apart and thresholds tied a third of the time.
    void checkGivesWay(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{73};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            const Vec2 place{random.between(-3000.0f, 3000.0f), random.between(-5000.0f, 800.0f)};
            const float reach = random.between(1.0f, 20.0f);
            const float otherReach = random.between(1.0f, 20.0f);
            const float spacingShare = random.between(0.3f, 1.2f);
            const float spacing = spacingShare * std::max(reach, otherReach);
            const float angle = random.between(0.0f, 6.0f);
            const float apart = i % 2 == 0 ? spacing : random.between(0.0f, 2.0f * spacing);
            const Vec2 other{place.x + apart * std::cos(angle), place.y + apart * std::sin(angle)};
            const float threshold = random.between(0.0f, 1.0f);
            const float otherThreshold = i % 3 == 0 ? threshold : random.between(0.0f, 1.0f);
            inputs.push_back(gpuVec4(place.x, place.y, reach, threshold));
            inputs.push_back(gpuVec4(other.x, other.y, otherReach, otherThreshold));
            inputs.push_back(gpuVec4(flag(i % 4 < 2), spacingShare));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_GIVES_WAY, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& a = inputs[3 * i];
            const GpuVec4& b = inputs[3 * i + 1];
            const GpuVec4& c = inputs[3 * i + 2];
            const bool givesWay = plantingGivesWay(Vec2{a[0], a[1]}, a[2], a[3], Vec2{b[0], b[1]}, b[2], b[3], c[0] != 0.0f, c[1]);
            report.check("plantingGivesWay", exactTolerance, flag(givesWay), outputs[i][0]);
        }
    }

    // Places a radius from a piece of path half the time (along it, or past an end).
    void checkNearSegment(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{79};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 2000; ++i) {
            const Vec2 start{random.between(-500.0f, 500.0f), random.between(-500.0f, 500.0f)};
            const Vec2 along{random.between(-150.0f, 150.0f), random.between(-150.0f, 150.0f)};
            const float lengthSquared = along.x * along.x + along.y * along.y;
            const Vec2 place{start.x + along.x * random.between(-0.2f, 1.2f) + random.between(-6.0f, 6.0f),
                             start.y + along.y * random.between(-0.2f, 1.2f) + random.between(-6.0f, 6.0f)};
            const float share = std::clamp(((place.x - start.x) * along.x + (place.y - start.y) * along.y) / lengthSquared, 0.0f, 1.0f);
            const Vec2 nearest{start.x + along.x * share, start.y + along.y * share};
            const float apart = std::sqrt((place.x - nearest.x) * (place.x - nearest.x) + (place.y - nearest.y) * (place.y - nearest.y));
            inputs.push_back(gpuVec4(place.x, place.y, start.x, start.y));
            inputs.push_back(gpuVec4(along.x, along.y, 1.0f / lengthSquared, i % 2 == 0 ? apart : random.between(0.0f, 6.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PLANTING_NEAR_SEGMENT, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4& a = inputs[2 * i];
            const GpuVec4& b = inputs[2 * i + 1];
            const bool nearIt = plantingNearSegment(Vec2{a[0], a[1]}, Vec2{a[2], a[3]}, Vec2{b[0], b[1]}, b[2], b[3]);
            report.check("plantingNearSegment", exactTolerance, flag(nearIt), outputs[i][0]);
        }
    }
}

void gpuTestPlanting(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("planting.glsl");
    checkRandom(runner, report);
    checkThreshold(runner, report);
    checkShare(runner, report);
    checkPlace(runner, report);
    checkKind(runner, report);
    checkTests(runner, report);
    checkStored(runner, report);
    checkRing(runner, report);
    checkGivesWay(runner, report);
    checkNearSegment(runner, report);
}
