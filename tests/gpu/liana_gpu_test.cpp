#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "scene/liana_shape.h"

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("liana_test");

    // Lianas as the forest hangs them: loops, a nearly taut one, a steep rope, one rising, one hanging straight.
    std::vector<LianaShape> lianas() {
        const LianaTree tree{Vec3{3.0f, 0.0f, -2.0f}, 30.0f, 1.1f};
        return {
            makeLianaShape(Vec3{2.0f, 20.0f, 1.0f}, Vec3{26.0f, 21.0f, -6.0f}, 35.0f, 0.03f, 2.0f, tree, &tree, 0.2f),
            makeLianaShape(Vec3{0.0f, 25.0f, 0.0f}, Vec3{18.0f, 24.0f, 4.0f}, 18.6f, 0.02f, 2.0f, tree, &tree, 0.2f),
            makeLianaShape(Vec3{5.0f, 22.0f, 5.0f}, Vec3{7.0f, -0.2f, 7.5f}, 23.0f, 0.05f, 2.0f, tree, nullptr, 0.2f),
            makeLianaShape(Vec3{-3.0f, 15.0f, 2.0f}, Vec3{12.0f, 27.0f, 9.0f}, 26.0f, 0.01f, 2.0f, tree, &tree, 0.2f),
            makeLianaShape(Vec3{1.0f, 18.0f, 1.0f}, Vec3{1.0f, 0.0f, 1.0f}, 18.5f, 0.03f, 2.0f, tree, nullptr, 0.2f),
        };
    }

    // A case's record: its eight rows.
    void appendRecord(std::vector<GpuVec4>& inputs, const LianaShape& shape) {
        for (const float* row : {shape.from, shape.to, shape.curve, shape.firstTree, shape.secondTree, shape.trees, shape.leaves, shape.leafLook}) {
            inputs.push_back(gpuVec4(row[0], row[1], row[2], row[3]));
        }
    }

    // A case: the record, then share, u round, cos and sin round, then an unused eye.
    void appendCase(std::vector<GpuVec4>& inputs, const LianaShape& shape, float share, float around) {
        appendRecord(inputs, shape);
        const float angle = around * 6.2831853f;
        inputs.push_back(gpuVec4(share, around, std::cos(angle), std::sin(angle)));
        inputs.push_back(gpuVec4(0.0f, 0.0f, 0.0f, 0.0f));
    }

    /*
    * Every liana at shares along it and turns round it, centre and tube vertex. asinh, sqrt and division round
    * differently on the GPU: positions to a tenth of a millimetre over tens of metres, directions to 1e-5.
    */
    void checkLianas(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;
        std::vector<std::pair<std::size_t, std::pair<float, float>>> cases;
        const std::vector<LianaShape> shapes = lianas();

        for (std::size_t liana = 0; liana < shapes.size(); ++liana) {
            for (int step = 0; step <= 16; ++step) {
                const float share = static_cast<float>(step) / 16.0f;
                const float around = static_cast<float>(step % 5) / 5.0f;
                appendCase(inputs, shapes[liana], share, around);
                cases.push_back({liana, {share, around}});
            }
        }

        const FloatTolerance place{.absolute = 1e-4f, .relative = 1e-5f};
        const FloatTolerance direction{.absolute = 1e-5f, .relative = 1e-5f};
        const std::vector<GpuVec4> centres = runner.run(shader, GPU_TEST_LIANA_CENTRE, inputs, 10, 4);
        const std::vector<GpuVec4> vertices = runner.run(shader, GPU_TEST_LIANA_VERTEX, inputs, 10, 4);

        for (std::size_t index = 0; index < cases.size(); ++index) {
            const LianaShape& shape = shapes[cases[index].first];
            const float share = cases[index].second.first;
            const float around = cases[index].second.second;
            const float angle = around * 6.2831853f;
            const LianaPoint centre = lianaCentre(shape, share);
            const Vec3 run = lianaRun(shape);
            const LianaVertex vertex = lianaVertex(shape, share, around, std::cos(angle), std::sin(angle));
            const GpuVec4* gpuCentre = &centres[index * 4];
            const GpuVec4* gpuVertex = &vertices[index * 4];
            const float centreOffset[3] = {centre.offset.x, centre.offset.y, centre.offset.z};
            const float centreAlong[3] = {centre.along.x, centre.along.y, centre.along.z};
            const float runAxis[3] = {run.x, run.y, run.z};
            const float offset[3] = {vertex.offset.x, vertex.offset.y, vertex.offset.z};
            const float normal[3] = {vertex.normal.x, vertex.normal.y, vertex.normal.z};
            const float tangent[3] = {vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};

            for (int axis = 0; axis < 3; ++axis) {
                report.check("lianaCentre offset", place, centreOffset[axis], gpuCentre[0][axis]);
                report.check("lianaCentre along", direction, centreAlong[axis], gpuCentre[1][axis]);
                report.check("lianaRun", direction, runAxis[axis], gpuCentre[2][axis]);
                report.check("lianaVertex offset", place, offset[axis], gpuVertex[0][axis]);
                report.check("lianaVertex normal", direction, normal[axis], gpuVertex[1][axis]);
                report.check("lianaVertex tangent", direction, tangent[axis], gpuVertex[2][axis]);
            }

            report.check("lianaCentre travelled", place, centre.travelled, gpuCentre[0][3]);
            report.check("lianaVertex u", exactTolerance, vertex.uv.x, gpuVertex[3][0]);
            report.check("lianaVertex v", place, vertex.uv.y, gpuVertex[3][1]);
            report.check("lianaVertex along", exactTolerance, vertex.along, gpuVertex[3][2]);
            report.check("lianaVertex arm", place, vertex.arm, gpuVertex[3][3]);
        }
    }
}

namespace {
    /*
    * Every liana with ivy (5 m and 3 m in its crowns, the forest's spacing), each card's corners near and in the
    * thinning band: the count, place, growth and every corner alike. The card and its leaf come from the same hash bits
    * both sides; positions as the tube's, the count and growth to rounding.
    */
    void checkLeaves(GpuFunctionRunner& runner, GpuCheckReport& report) {
        struct LeafCase {
            LianaShape shape;
            float card = 0.0f;
            float across = 0.0f;
            float up = 0.0f;
            Vec3 eye;
        };

        std::vector<GpuVec4> inputs;
        std::vector<LeafCase> cases;

        for (LianaShape shape : lianas()) {
            setLianaLeaves(shape, LianaLeaves{.firstCrownMetres = 5.0f, .lastCrownMetres = 3.0f, .spacing = 0.35f, .sparseSpacing = 1.2f, .size = 0.3f,
                                              .thinFrom = 80.0f, .goneBy = 150.0f, .seed = 0x2468aceu});
            const int cards = static_cast<int>(std::ceil(lianaLeafCount(shape)));

            for (int card = 0; card <= cards; card += 5) {
                for (const Vec3& eye : {Vec3{0.0f, 20.0f, 0.0f}, Vec3{0.0f, 20.0f, -95.0f}}) {
                    const float across = static_cast<float>(card % 3) * 0.5f;
                    const float up = static_cast<float>(card % 2);
                    appendRecord(inputs, shape);
                    inputs.push_back(gpuVec4(static_cast<float>(card), across, up, 0.0f));
                    inputs.push_back(gpuVec4(eye.x, eye.y, eye.z, (eye - Vec3{shape.from[0], shape.from[1], shape.from[2]}).length()));
                    cases.push_back(LeafCase{shape, static_cast<float>(card), across, up, eye});
                }
            }
        }

        const FloatTolerance place{.absolute = 1e-4f, .relative = 1e-5f};
        const FloatTolerance direction{.absolute = 1e-5f, .relative = 1e-5f};
        const std::vector<GpuVec4> leaves = runner.run(shader, GPU_TEST_LIANA_LEAF, inputs, 10, 5);

        for (std::size_t index = 0; index < cases.size(); ++index) {
            const LeafCase& leafCase = cases[index];
            const LianaLeafVertex leaf = lianaLeafVertex(leafCase.shape, leafCase.card, leafCase.across, leafCase.up, leafCase.eye);
            const float distance = (leafCase.eye - Vec3{leafCase.shape.from[0], leafCase.shape.from[1], leafCase.shape.from[2]}).length();
            const GpuVec4* gpu = &leaves[index * 5];
            const float offset[3] = {leaf.offset.x, leaf.offset.y, leaf.offset.z};
            const float stem[3] = {leaf.stem.x, leaf.stem.y, leaf.stem.z};
            const float normal[3] = {leaf.normal.x, leaf.normal.y, leaf.normal.z};
            const float tangent[3] = {leaf.tangent.x, leaf.tangent.y, leaf.tangent.z};

            for (int axis = 0; axis < 3; ++axis) {
                report.check("lianaLeafVertex offset", place, offset[axis], gpu[0][axis]);
                report.check("lianaLeafVertex stem", place, stem[axis], gpu[1][axis]);
                report.check("lianaLeafVertex normal", direction, normal[axis], gpu[2][axis]);
                report.check("lianaLeafVertex tangent", direction, tangent[axis], gpu[3][axis]);
            }

            report.check("lianaLeafCount", place, lianaLeafCount(leafCase.shape), gpu[0][3]);
            report.check("lianaLeafGrowth", place, lianaLeafGrowth(leafCase.shape, leafCase.card, distance), gpu[1][3]);
            report.check("lianaLeafVertex along", place, leaf.along, gpu[2][3]);
            report.check("lianaLeafVertex arm", place, leaf.arm, gpu[3][3]);
            report.check("lianaLeafVertex u", exactTolerance, leaf.uv.x, gpu[4][0]);
            report.check("lianaLeafVertex v", exactTolerance, leaf.uv.y, gpu[4][1]);
            report.check("lianaLeafPlace", place, lianaLeafPlace(leafCase.shape, leafCase.card, 0.25f), gpu[4][2]);
        }
    }
}

void gpuTestLiana(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("liana.glsl");
    checkLianas(runner, report);
    checkLeaves(runner, report);
}
