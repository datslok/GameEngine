#include "gpu_check.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>

std::uint32_t CaseRandom::next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// The top 24 bits, which a float holds exactly, as a fraction of 2^24.
float CaseRandom::between(float lowest, float highest) {
    const float unit = static_cast<float>(next() >> 8) / 16777216.0f;
    return lowest + (highest - lowest) * unit;
}

/*
* Uniform over the sphere: the height z uniform in -1..1 and the angle around uniform (Archimedes' hat-box theorem:
* a sphere's area between two heights is proportional to their distance apart).
*/
Vec3 CaseRandom::direction() {
    const float z = between(-1.0f, 1.0f);
    const float angle = between(0.0f, 2.0f * std::numbers::pi_v<float>);
    const float around = std::sqrt(std::max(1.0f - z * z, 0.0f));
    return Vec3{around * std::cos(angle), around * std::sin(angle), z};
}

GpuCheckReport::Group& GpuCheckReport::find(const std::string& name, const FloatTolerance& tolerance) {
    for (Group& group : groups) {
        if (group.section == currentSection && group.name == name) {
            return group;
        }
    }

    groups.push_back(Group{currentSection, name, ToleranceTally{tolerance}});
    return groups.back();
}

void GpuCheckReport::check(const std::string& name, const FloatTolerance& tolerance, float expected, float actual) {
    find(name, tolerance).tally.add(expected, actual, tolerance);
}

void GpuCheckReport::check(const std::string& name, const FloatTolerance& tolerance, const Vec3& expected, const Vec3& actual) {
    Group& group = find(name, tolerance);
    group.tally.add(expected.x, actual.x, tolerance);
    group.tally.add(expected.y, actual.y, tolerance);
    group.tally.add(expected.z, actual.z, tolerance);
}

void GpuCheckReport::section(const std::string& title) {
    currentSection = title;
}

/*
* One line per group: how many values, and the worst one's share of the tolerance with the two values behind it. The
* share is the useful number: it says how much room is left before rounding alone would fail the test.
*/
void GpuCheckReport::print() const {
    std::string section;

    for (const Group& group : groups) {
        if (group.section != section) {
            section = group.section;
            std::cout << section << ":\n";
        }

        const ToleranceTally& tally = group.tally;
        std::cout << "  " << std::left << std::setw(36) << group.name << std::right
                  << (tally.passes() ? "pass" : "FAIL") << "  " << std::setw(5) << tally.checked << " values, worst used "
                  << std::fixed << std::setprecision(1) << std::setw(6) << tally.worstUse * 100.0f << "% of tolerance"
                  << std::defaultfloat << std::setprecision(9) << " (C++ " << tally.worstExpected << ", GPU " << tally.worstActual << ")";

        if (!tally.passes()) {
            std::cout << ", " << tally.failures << " beyond it";
        }

        std::cout << '\n';
    }
}

std::size_t GpuCheckReport::failedGroups() const {
    std::size_t failed = 0;

    for (const Group& group : groups) {
        if (!group.tally.passes()) {
            ++failed;
        }
    }

    return failed;
}
