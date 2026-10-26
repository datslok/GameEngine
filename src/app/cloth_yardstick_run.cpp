#include "app/cloth_yardstick_run.h"

#include "cloth/cloth_yardstick.h"
#include "core/build_info.h"
#include "core/job_system.h"
#include "engine/engine_settings.h"
#include "render/gpu/cloth_gpu.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_cloth.h"
#include "render/vulkan/vulkan_device.h"

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

    // What a run settles: its sheets, substeps and settling time, and whether it adds the GPU's rows.
    struct YardstickPlan {
        std::vector<int> rungs;
        std::vector<int> substeps;
        float settleSeconds = 0.0f;
        bool gpu = false;
    };

    YardstickPlan planFor(YardstickRun run, const ClothSettings& fabric) {
        if (run == YardstickRun::Quick) {
            return YardstickPlan{{quickRungs.begin(), quickRungs.end()}, {fabric.substeps}, quickSettleSeconds, false};
        }
        return YardstickPlan{{yardstickRungs.begin(), yardstickRungs.end()}, {fabric.substeps, convergedSubsteps * fabric.substeps},
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

    double millisecondsSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // Making the sheet (its colouring and coarse levels) is left out of the time: only the ticks are what a game pays every frame.
    YardstickRow settleOnCpu(const char* solver, int squares, const ClothSettings& settings, float settleSeconds, float tickSeconds, JobSystem& jobs) {
        Cloth cloth = yardstickSheet(squares, settings);
        const auto start = std::chrono::steady_clock::now();
        const float motion = settleYardstick(cloth, settleSeconds, tickSeconds, jobs);
        const double elapsed = millisecondsSince(start);
        const int ticks = static_cast<int>(std::lround(settleSeconds / tickSeconds));
        const int levels = std::min(settings.hierarchyLevels, static_cast<int>(cloth.hierarchy.levels.size()));
        return YardstickRow{solver, squares, cloth.particles.size(), settings.substeps, levels, 0, measureSheet(cloth, squares), motion, elapsed / ticks};
    }

    /*
    * The same settling on the GPU (render/vulkan/vulkan_cloth.h), a second of ticks a submission, waited for each time;
    * the time counts recording, submitting and waiting, as a game's frame would. The points are read back a second
    * before the end and at the end, for the last second's motion and the measures.
    */
    YardstickRow settleOnGpu(const char* solver, const VulkanDevice& device, const VulkanClothPipelines& pipelines, ImmediateCommands& commands,
                             int squares, const ClothSettings& settings, float settleSeconds, float tickSeconds) {
        Cloth cloth = yardstickSheet(squares, settings);
        VulkanCloth gpuCloth{device, pipelines, commands, cloth};
        const std::vector<ClothTickCommands> batch(gpuTicksPerSubmission, clothTickCommands(cloth, tickSeconds));
        const int ticks = static_cast<int>(std::lround(settleSeconds / tickSeconds));
        const int lastSecondTicks = std::min(ticks, static_cast<int>(std::lround(1.0f / tickSeconds)));

        std::vector<Vec3> before = cloth.particles.positions;
        const auto start = std::chrono::steady_clock::now();
        for (int done = 0; done < ticks;) {
            const int count = std::min(gpuTicksPerSubmission, ticks - done);
            const bool lastSecondStarts = done + count == ticks - lastSecondTicks;
            const bool finished = done + count == ticks;
            commands.run([&](VkCommandBuffer buffer) {
                gpuCloth.recordTicks(buffer, std::span<const ClothTickCommands>{batch.data(), static_cast<std::size_t>(count)}, 0);
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
        return YardstickRow{solver, squares, cloth.particles.size(), settings.substeps, static_cast<int>(clothLevelsUsed(cloth, settings)),
                            clothDispatchesPerTick(cloth, settings),
                            measureSheet(cloth, squares), motion, elapsed / ticks};
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

    out << "Cloth yardstick" << (run == YardstickRun::Quick ? " (quick)" : "") << ": a " << yardstickSheetSize
        << " m sheet hung from its top corners, settled " << plan.settleSeconds << " s, the CPU on " << jobs.getThreadCount() << " threads"
        << (device ? ", the GPU on " + device->getName() : std::string{}) << (isDebugBuild ? " (debug build: times are not comparable)" : "")
        << '\n';
    out << "solver     squares   points  substeps levels dispatches  path stretch  worst edge   sag (m)  last second (mm)  ms per tick\n";

    std::unique_ptr<VulkanClothPipelines> pipelines;
    std::unique_ptr<ImmediateCommands> commands;
    if (device) {
        pipelines = std::make_unique<VulkanClothPipelines>(*device);
        commands = std::make_unique<ImmediateCommands>(*device);
    }

    for (const int squares : plan.rungs) {
        for (const int substeps : plan.substeps) {
            ClothSettings settings = fabric;
            settings.substeps = substeps;
            // Vertex block descent only at the fabric's substeps: at four times as many it would take most of the run.
            for (const auto& [solver, solverSettings] : cpuSolvers(settings)) {
                if (substeps == fabric.substeps || solverSettings.method == ClothMethod::Xpbd) {
                    printRow(out, settleOnCpu(solver, squares, solverSettings, plan.settleSeconds, tickSeconds, jobs));
                }
            }
            if (device) {
                for (const auto& [solver, solverSettings] : gpuSolvers(settings)) {
                    if (substeps == fabric.substeps || solverSettings.method == ClothMethod::Xpbd) {
                        printRow(out, settleOnGpu(solver, *device, *pipelines, *commands, squares, solverSettings, plan.settleSeconds, tickSeconds));
                    }
                }
            }
        }
    }
}
