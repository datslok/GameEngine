#include "cloth/cloth_tethers.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
    // Points a job: a tether is a few multiplies, so a job needs many points to be worth handing out.
    constexpr std::size_t tetheredPointsPerJob = 1024;
}

float clothRestDistance(const std::vector<Vec3>& restPositions, std::uint32_t from, std::uint32_t to) {
    return (restPositions.at(to) - restPositions.at(from)).length();
}

/*
* Every free point measures its distance to every anchor and keeps the nearest few: points times anchors distances,
* done once when the cloth is made or pinned, never a tick.
*/
std::vector<ClothTether> clothTethers(const std::vector<Vec3>& restPositions, const std::vector<float>& inverseMasses) {
    if (restPositions.size() != inverseMasses.size()) {
        throw std::invalid_argument("A cloth's tethers need a rest position and an inverse mass for every point");
    }

    std::vector<std::uint32_t> anchors;
    for (std::size_t index = 0; index < inverseMasses.size(); ++index) {
        if (inverseMasses[index] == 0.0f) {
            anchors.push_back(static_cast<std::uint32_t>(index));
        }
    }
    if (anchors.empty()) {
        return {};
    }

    std::vector<ClothTether> tethers(restPositions.size() * tethersPerPoint);
    std::vector<std::pair<float, std::uint32_t>> byDistance(anchors.size());
    const std::size_t kept = std::min(tethersPerPoint, anchors.size());
    for (std::size_t point = 0; point < restPositions.size(); ++point) {
        if (inverseMasses[point] == 0.0f) {
            continue;
        }

        for (std::size_t index = 0; index < anchors.size(); ++index) {
            byDistance[index] = {clothRestDistance(restPositions, static_cast<std::uint32_t>(point), anchors[index]), anchors[index]};
        }
        std::partial_sort(byDistance.begin(), byDistance.begin() + static_cast<std::ptrdiff_t>(kept), byDistance.end());
        for (std::size_t rank = 0; rank < kept; ++rank) {
            tethers[point * tethersPerPoint + rank] = ClothTether{byDistance[rank].second, byDistance[rank].first};
        }
    }
    return tethers;
}

// The spare slots come last, so a point's tethers end at the first one with no anchor.
void solveTethers(ClothParticles& particles, const std::vector<ClothTether>& tethers, float slack, JobSystem& jobs) {
    if (tethers.empty()) {
        return;
    }
    if (tethers.size() != particles.size() * tethersPerPoint) {
        throw std::invalid_argument("A cloth's tethers were built for a different number of points");
    }

    const float stretchAllowed = 1.0f + slack;
    jobs.parallelFor(particles.size(), tetheredPointsPerJob, [&](std::size_t point) {
        if (particles.inverseMasses[point] == 0.0f) {
            return;
        }

        Vec3& position = particles.positions[point];
        for (std::size_t rank = 0; rank < tethersPerPoint; ++rank) {
            const ClothTether& tether = tethers[point * tethersPerPoint + rank];
            if (tether.anchor == noTetherAnchor) {
                break;
            }

            solveTether(position, particles.positions[tether.anchor], tether.restLength, stretchAllowed);
        }
    });
}
