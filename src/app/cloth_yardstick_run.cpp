#include "app/cloth_yardstick_run.h"

#include "assets/obj_loader.h"
#include "cloth/cloth_yardstick.h"
#include "core/build_info.h"
#include "core/job_system.h"
#include "engine/engine_settings.h"
#include "games/clothlab/cloth_lab_dress.h"
#include "games/clothlab/cloth_lab_layout.h"
#include "render/gpu/cloth_gpu.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_cloth.h"
#include "render/vulkan/vulkan_device.h"
#include "scene/model.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {
    // The default substeps and four times as many.
    constexpr int convergedSubsteps = 4;

    // GPU ticks a submission: a second's worth, so the wait for each costs little against the work.
    constexpr int gpuTicksPerSubmission = 120;

    // The quick run's sheets and settling: enough to judge a change in about a minute, not the numbers recorded.
    constexpr std::array<int, 3> quickRungs{25, 50, 100};
    constexpr float quickSettleSeconds = 4.0f;

    /*
    * The dress-sized test's rungs: the dress cut with its points this far apart, about 3,000, 6,000, 12,000 (the cloth
    * lab's dressSpacing) and 25,000 points, since a solver's error shows in how it grows with the resolution. The quick
    * run leaves out the finest.
    */
    constexpr std::array<float, 4> dressRungs{0.02f, 0.014f, 0.01f, 0.007f}; // m
    constexpr std::array<float, 3> quickDressRungs{0.02f, 0.014f, 0.01f};    // m

    // The mannequin stands on the floor at the origin.
    const Vec3 dressFeet{0.0f, 0.0f, 0.0f};

    // What a run settles: its sheets, substeps and settling time, its dresses, and whether it adds the GPU's rows.
    struct YardstickPlan {
        std::vector<int> rungs;
        std::vector<int> substeps;
        std::vector<float> dressRungs;
        float settleSeconds = 0.0f;
        bool gpu = false;
    };

    YardstickPlan planFor(YardstickRun run, const ClothSettings& fabric) {
        if (run == YardstickRun::Quick) {
            return YardstickPlan{{quickRungs.begin(), quickRungs.end()}, {fabric.substeps}, {quickDressRungs.begin(), quickDressRungs.end()},
                                 quickSettleSeconds, false};
        }
        const std::vector<float> dresses{dressRungs.begin(), dressRungs.end()};
        if (run == YardstickRun::Dress) {
            return YardstickPlan{{}, {fabric.substeps}, dresses, yardstickSettleSeconds, true};
        }
        return YardstickPlan{{yardstickRungs.begin(), yardstickRungs.end()}, {fabric.substeps, convergedSubsteps * fabric.substeps}, dresses,
                             yardstickSettleSeconds, true};
    }

    ClothSettings withBlockDescent(const ClothSettings& settings) {
        ClothSettings blockDescent = settings;
        blockDescent.method = ClothMethod::VertexBlockDescent;
        return blockDescent;
    }

    // Vertex block descent with every coarse level the sheet has.
    ClothSettings withLevels(const ClothSettings& settings) {
        ClothSettings layered = withBlockDescent(settings);
        layered.hierarchyLevels = maxClothLevels;
        return layered;
    }

    ClothSettings withTethers(const ClothSettings& settings) {
        ClothSettings tethered = settings;
        tethered.tethers = true;
        return tethered;
    }

    // The CPU solvers every sheet is settled with, each by the name its rows carry: XPBD, XPBD with tethers, vertex
    // block descent, and vertex block descent with its levels.
    std::vector<std::pair<const char*, ClothSettings>> cpuSolvers(const ClothSettings& settings) {
        return {{"XPBD", settings}, {"XPBD+T", withTethers(settings)}, {"VBD", withBlockDescent(settings)}, {"VBD+L", withLevels(settings)}};
    }

    // The GPU's, the same four.
    std::vector<std::pair<const char*, ClothSettings>> gpuSolvers(const ClothSettings& settings) {
        return {{"GPU", settings}, {"GPU+T", withTethers(settings)}, {"GPU VBD", withBlockDescent(settings)}, {"GPU VBD+L", withLevels(settings)}};
    }

    // A cloth settled, how far it still moved in its last second, and what each tick cost.
    struct Settled {
        Cloth cloth;
        float motion = 0.0f;
        double millisecondsPerTick = 0.0;
    };

    struct YardstickRow {
        const char* solver;
        int squares = 0;
        std::size_t points = 0;
        int substeps = 0;
        int levels = 0;
        std::uint32_t dispatches = 0;
        SheetMeasure measure;
        float motion = 0.0f;
        double millisecondsPerTick = 0.0;
    };

    void printRow(std::ostream& out, const YardstickRow& row) {
        char text[200];
        std::snprintf(text, sizeof(text), "%-10s %7d %8zu %9d %6d %10u %12.3f%% %10.3f%% %9.4f %17.2f %12.2f\n", row.solver, row.squares,
                      row.points, row.substeps, row.levels, row.dispatches, 100.0 * row.measure.pathStretch, 100.0 * row.measure.worstEdgeStrain, row.measure.sag,
                      1000.0 * row.motion, row.millisecondsPerTick);
        out << text << std::flush;
    }

    struct DressRow {
        const char* solver;
        float spacing = 0.0f;
        std::size_t points = 0;
        std::uint32_t dispatches = 0;
        GarmentMeasure measure;
        float motion = 0.0f;
        double millisecondsPerTick = 0.0;
    };

    void printDressRow(std::ostream& out, const DressRow& row) {
        char text[220];
        std::snprintf(text, sizeof(text), "%-12s %8.1f %7zu %10u %11.3f%% %10.3f%% %10.3f%% %13.2f %10.3f %14.2f %17.2f %12.2f\n", row.solver,
                      100.0 * row.spacing, row.points, row.dispatches, 100.0 * row.measure.meanStretch, 100.0 * row.measure.percentileStrain,
                      100.0 * row.measure.worstEdgeStrain, 1000.0 * row.measure.worstSeamGap, row.measure.hemHeight, 1000.0 * row.measure.closestToBody,
                      1000.0 * row.motion, row.millisecondsPerTick);
        out << text << std::flush;
    }

    double millisecondsSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // Making the cloth (its colouring, coarse levels and tethers) is left out of the time: only the ticks are what a game pays every frame.
    Settled settleOnCpu(Cloth cloth, float settleSeconds, float tickSeconds, JobSystem& jobs, const ClothFields& fields = {},
                        const ClothTickPreparation& prepare = {}) {
        const auto start = std::chrono::steady_clock::now();
        const float motion = settleYardstick(cloth, settleSeconds, tickSeconds, jobs, fields, prepare);
        const double elapsed = millisecondsSince(start);
        const int ticks = static_cast<int>(std::lround(settleSeconds / tickSeconds));
        return Settled{std::move(cloth), motion, elapsed / ticks};
    }

    // What the GPU rows share: the device, the cloth passes, a command buffer to run them in, and the field bodies' samples.
    struct GpuBench {
        const VulkanDevice& device;
        const VulkanClothPipelines& pipelines;
        ImmediateCommands& commands;
        VulkanClothFields& fields;
    };

    /*
    * The same settling on the GPU (render/vulkan/vulkan_cloth.h), a second of ticks a submission, waited for each time;
    * the time counts readying the ticks, recording, submitting and waiting, as a game's frame would. Each tick's
    * commands are taken from the cloth after prepare has readied it, as stepCloths takes them. The points are read back
    * a second before the end and at the end, for the last second's motion and the measures.
    */
    Settled settleOnGpu(const GpuBench& bench, Cloth cloth, float settleSeconds, float tickSeconds, const ClothFields& fields = {},
                        const ClothTickPreparation& prepare = {}) {
        VulkanCloth gpuCloth{bench.device, bench.pipelines, bench.commands, cloth};
        bench.fields.hold(bench.commands, fields);
        const VulkanClothFields* heldFields = fields.empty() ? nullptr : &bench.fields;
        const int ticks = static_cast<int>(std::lround(settleSeconds / tickSeconds));
        const int lastSecondTicks = std::min(ticks, static_cast<int>(std::lround(1.0f / tickSeconds)));

        std::vector<Vec3> before = cloth.particles.positions;
        std::vector<ClothTickCommands> batch;
        const auto start = std::chrono::steady_clock::now();
        for (int done = 0; done < ticks;) {
            const int count = std::min(gpuTicksPerSubmission, ticks - done);
            batch.clear();
            for (int tick = done; tick < done + count; ++tick) {
                if (prepare) {
                    prepare(cloth, tick);
                }
                batch.push_back(clothTickCommands(cloth, tickSeconds, {}, fields));
            }
            const bool lastSecondStarts = done + count == ticks - lastSecondTicks;
            const bool finished = done + count == ticks;
            bench.commands.run([&](VkCommandBuffer buffer) {
                gpuCloth.recordTicks(buffer, std::span<const ClothTickCommands>{batch}, 0, nullptr, heldFields);
                if (lastSecondStarts || finished) {
                    gpuCloth.recordReadback(buffer, 0);
                }
            });
            done += count;
            if (lastSecondStarts) {
                before = gpuCloth.readPositions(0);
            }
        }
        const double elapsed = millisecondsSince(start);
        cloth.particles.positions = gpuCloth.readPositions(0);

        float motion = 0.0f;
        for (std::size_t index = 0; index < before.size(); ++index) {
            motion = std::max(motion, (cloth.particles.positions[index] - before[index]).length());
        }
        return Settled{std::move(cloth), motion, elapsed / ticks};
    }

    YardstickRow cpuSheetRow(const char* solver, int squares, const ClothSettings& settings, float settleSeconds, float tickSeconds, JobSystem& jobs) {
        const Settled settled = settleOnCpu(yardstickSheet(squares, settings), settleSeconds, tickSeconds, jobs);
        const int levels = std::min(settings.hierarchyLevels, static_cast<int>(settled.cloth.hierarchy.levels.size()));
        return YardstickRow{solver, squares, settled.cloth.particles.size(), settings.substeps, levels, 0, measureSheet(settled.cloth, squares),
                            settled.motion, settled.millisecondsPerTick};
    }

    YardstickRow gpuSheetRow(const char* solver, const GpuBench& bench, int squares, const ClothSettings& settings, float settleSeconds, float tickSeconds) {
        const Settled settled = settleOnGpu(bench, yardstickSheet(squares, settings), settleSeconds, tickSeconds);
        return YardstickRow{solver, squares, settled.cloth.particles.size(), settings.substeps,
                            static_cast<int>(clothLevelsUsed(settled.cloth, settings)), clothDispatchesPerTick(settled.cloth, settings),
                            measureSheet(settled.cloth, squares), settled.motion, settled.millisecondsPerTick};
    }

    void runSheets(std::ostream& out, const YardstickPlan& plan, const ClothSettings& fabric, float tickSeconds, JobSystem& jobs, const GpuBench* bench) {
        out << "solver     squares   points  substeps levels dispatches  path stretch  worst edge   sag (m)  last second (mm)  ms per tick\n";
        for (const int squares : plan.rungs) {
            for (const int substeps : plan.substeps) {
                ClothSettings settings = fabric;
                settings.substeps = substeps;
                // Vertex block descent only at the fabric's substeps: at four times as many it would take most of the run.
                for (const auto& [solver, solverSettings] : cpuSolvers(settings)) {
                    if (substeps == fabric.substeps || solverSettings.method == ClothMethod::Xpbd) {
                        printRow(out, cpuSheetRow(solver, squares, solverSettings, plan.settleSeconds, tickSeconds, jobs));
                    }
                }
                if (bench) {
                    for (const auto& [solver, solverSettings] : gpuSolvers(settings)) {
                        if (substeps == fabric.substeps || solverSettings.method == ClothMethod::Xpbd) {
                            printRow(out, gpuSheetRow(solver, *bench, squares, solverSettings, plan.settleSeconds, tickSeconds));
                        }
                    }
                }
            }
        }
    }

    /*
    * The dress-sized test (docs/CLOTHGPU.md, Part 5): the cloth lab's dress on its mannequin, at each rung held by
    * contact alone and pinned at its shoulders and tethered, with XPBD at the fabric's substeps, each made, sewn and
    * settled as the lab does it (dressOnMannequin, prepareDressTick), on the CPU and then the GPU.
    */
    void runDresses(std::ostream& out, const YardstickPlan& plan, const ClothSettings& fabric, float tickSeconds, JobSystem& jobs, const GpuBench* bench) {
        const Mesh model = loadObj(clothLabMannequinModel);
        const ClothFields fields{std::make_shared<const SignedDistanceField>(
            mannequinField(model, clothLabMannequinPlacement(meshBounds(model), dressFeet), jobs))};

        out << "Dress-sized test: the two-panel dress on the " << clothLabMannequinHeight << " m mannequin, sewn " << dressSewingSeconds
            << " s and settled to " << plan.settleSeconds << " s, XPBD at " << fabric.substeps << " substeps\n";
        out << "solver       spacing (cm) points dispatches mean stretch  99th strain  worst edge  seam gap (mm) hem (m)  to body (mm)  last second (mm)  ms per tick\n";

        for (const float spacing : plan.dressRungs) {
            for (const bool pinned : {false, true}) {
                const DressOnMannequin dress = dressOnMannequin(*fields.front(), dressFeet, spacing, fabric, pinned, jobs);
                const ClothTickPreparation prepare = [&](Cloth& cloth, int tick) {
                    prepareDressTick(cloth, dress.strapPaths, fabric, static_cast<double>(tick) * tickSeconds, tickSeconds);
                };
                const auto row = [&](const char* solver, const Settled& settled, std::uint32_t dispatches) {
                    printDressRow(out, DressRow{solver, spacing, settled.cloth.particles.size(), dispatches,
                                                measureGarment(settled.cloth, dress.hem, dressFeet.y, fields), settled.motion, settled.millisecondsPerTick});
                };

                row(pinned ? "XPBD pinned" : "XPBD contact", settleOnCpu(dress.cloth, plan.settleSeconds, tickSeconds, jobs, fields, prepare), 0);
                if (bench) {
                    row(pinned ? "GPU pinned" : "GPU contact", settleOnGpu(*bench, dress.cloth, plan.settleSeconds, tickSeconds, fields, prepare),
                        clothDispatchesPerTick(dress.cloth, fabric));
                }
            }
        }
    }
}

void runClothYardstick(std::ostream& out, YardstickRun run) {
    JobSystem jobs;
    const float tickSeconds = static_cast<float>(1.0 / EngineSettings{}.ticksPerSecond);
    const ClothSettings fabric;
    const YardstickPlan plan = planFor(run, fabric);

    // No validation layers: they check every call and would slow the GPU rows down many times.
    std::unique_ptr<VulkanDevice> device;
    if (plan.gpu) {
        try {
            device = std::make_unique<VulkanDevice>(nullptr, false);
        } catch (const std::exception& error) {
            out << "No GPU rows: " << error.what() << '\n';
        }
    }

    out << "Cloth yardstick" << (run == YardstickRun::Quick ? " (quick)" : run == YardstickRun::Dress ? " (dress)" : "") << ": settled "
        << plan.settleSeconds << " s, the CPU on " << jobs.getThreadCount() << " threads" << (device ? ", the GPU on " + device->getName() : std::string{})
        << (isDebugBuild ? " (debug build: times are not comparable)" : "") << '\n';

    std::unique_ptr<VulkanClothPipelines> pipelines;
    std::unique_ptr<ImmediateCommands> commands;
    std::unique_ptr<VulkanClothFields> fields;
    std::unique_ptr<GpuBench> bench;
    if (device) {
        pipelines = std::make_unique<VulkanClothPipelines>(*device);
        commands = std::make_unique<ImmediateCommands>(*device);
        fields = std::make_unique<VulkanClothFields>(*device);
        bench = std::make_unique<GpuBench>(GpuBench{*device, *pipelines, *commands, *fields});
    }

    if (!plan.rungs.empty()) {
        out << "A " << yardstickSheetSize << " m sheet hung from its top corners\n";
        runSheets(out, plan, fabric, tickSeconds, jobs, bench.get());
    }
    if (!plan.dressRungs.empty()) {
        runDresses(out, plan, fabric, tickSeconds, jobs, bench.get());
    }
}
