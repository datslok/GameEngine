#include "app/cloth_yardstick_run.h"

#include "cloth/cloth_yardstick.h"
#include "core/build_info.h"
#include "core/job_system.h"
#include "engine/engine_settings.h"

#include <chrono>
#include <cstdio>

namespace {
    // The default substeps and four times as many.
    constexpr int convergedSubsteps = 4;
}

// Making the sheet (its colouring) is left out of the time: only the ticks are what a game pays every frame.
void runClothYardstick(std::ostream& out) {
    JobSystem jobs;
    const float tickSeconds = static_cast<float>(1.0 / EngineSettings{}.ticksPerSecond);
    const ClothSettings fabric;

    out << "Cloth yardstick: a " << yardstickSheetSize << " m sheet hung from its top corners, settled "
        << yardstickSettleSeconds << " s, on " << jobs.getThreadCount() << " threads"
        << (isDebugBuild ? " (debug build: times are not comparable)" : "") << '\n';
    out << "squares   points  substeps  path stretch  worst edge   sag (m)  last second (mm)  ms per tick\n";

    for (const int squares : yardstickRungs) {
        for (const int substeps : {fabric.substeps, convergedSubsteps * fabric.substeps}) {
            ClothSettings settings = fabric;
            settings.substeps = substeps;
            Cloth cloth = yardstickSheet(squares, settings);

            const auto start = std::chrono::steady_clock::now();
            const float motion = settleYardstick(cloth, yardstickSettleSeconds, tickSeconds, jobs);
            const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
            const double ticks = static_cast<double>(yardstickSettleSeconds / tickSeconds);

            const SheetMeasure measure = measureSheet(cloth, squares);
            char row[160];
            std::snprintf(row, sizeof(row), "%7d %8zu %9d %12.3f%% %10.3f%% %9.4f %17.2f %12.2f\n", squares, cloth.particles.size(), substeps,
                          100.0 * measure.pathStretch, 100.0 * measure.worstEdgeStrain, measure.sag, 1000.0 * motion,
                          elapsed.count() / ticks);
            out << row << std::flush;
        }
    }
}
