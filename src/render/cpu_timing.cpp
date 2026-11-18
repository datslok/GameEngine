#include "render/cpu_timing.h"

#include <utility>

const char* cpuStageName(CpuStage stage) {
    switch (stage) {
    case CpuStage::SkyBake:
        return "SKYBAKE";
    case CpuStage::Wait:
        return "WAIT";
    case CpuStage::Setup:
        return "SETUP";
    case CpuStage::Frustum:
        return "FRUSTUM";
    case CpuStage::Lights:
        return "LIGHTS";
    case CpuStage::Shadows:
        return "SHADOWS";
    case CpuStage::Captures:
        return "CAPTURES";
    case CpuStage::Lines:
        return "LINES";
    case CpuStage::Pack:
        return "PACK";
    case CpuStage::Record:
        return "RECORD";
    case CpuStage::Submit:
        return "SUBMIT";
    }

    return "STAGE";
}

void CpuStageClock::start(double nowSeconds) {
    timings = CpuStageTimings{};
    lastSeconds = nowSeconds;
    running = true;
}

void CpuStageClock::lap(CpuStage stage, double nowSeconds) {
    if (!running) {
        return;
    }

    const double seconds = nowSeconds - lastSeconds;
    timings.seconds[static_cast<std::size_t>(stage)] += seconds;
    timings.totalSeconds += seconds;
    lastSeconds = nowSeconds;
}

CpuStageTimings CpuStageClock::finish() {
    CpuStageTimings finished = std::exchange(timings, CpuStageTimings{});
    finished.valid = running;
    running = false;
    return finished;
}
