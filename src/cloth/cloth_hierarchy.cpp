#include "cloth/cloth_hierarchy.h"

#include <algorithm>
#include <cstddef>

namespace {
    /*
    * Keeps a parent's weight finite when the child lies on it. The paper gives no value; this one is small against
    * d / the farthest d for any two distinct points of a mesh, so it barely changes their weights.
    */
    constexpr float parentWeightEpsilon = 1e-3f;

    // Children moved by one job: each is a few parents' weighted sum, so a job takes many.
    constexpr std::size_t followPerJob = 256;

    bool containsSorted(const std::vector<std::uint32_t>& list, std::uint32_t value) {
        return std::binary_search(list.begin(), list.end(), value);
    }

    void insertSorted(std::vector<std::uint32_t>& list, std::uint32_t value) {
        list.insert(std::lower_bound(list.begin(), list.end(), value), value);
    }

    void eraseSorted(std::vector<std::uint32_t>& list, std::uint32_t value) {
        const auto found = std::lower_bound(list.begin(), list.end(), value);
        if (found != list.end() && *found == value) {
            list.erase(found);
        }
    }

    /*
    * Collapse a fine point into its parent nearest the parents' mean: the edge between them goes, and each of the
    * point's other edges moves to that parent, unless the parent already has it. The parents are still the point's
    * neighbours here, since only a collapse removes an edge and parents never collapse.
    */
    void collapse(std::vector<std::vector<std::uint32_t>>& neighbours, std::uint32_t point, const std::vector<std::uint32_t>& parents,
                  const std::vector<Vec3>& restPositions) {
        Vec3 mean{0.0f, 0.0f, 0.0f};
        for (const std::uint32_t parent : parents) {
            mean = mean + restPositions[parent];
        }
        mean = mean * (1.0f / static_cast<float>(parents.size()));

        std::uint32_t into = parents.front();
        for (const std::uint32_t parent : parents) {
            if ((restPositions[parent] - mean).lengthSquared() < (restPositions[into] - mean).lengthSquared()) {
                into = parent;
            }
        }

        for (const std::uint32_t other : neighbours[point]) {
            eraseSorted(neighbours[other], point);
            if (other != into && !containsSorted(neighbours[into], other)) {
                insertSorted(neighbours[into], other);
                insertSorted(neighbours[other], into);
            }
        }
        neighbours[point].clear();
    }
}

std::vector<bool> chooseCoarsePoints(const std::vector<std::vector<std::uint32_t>>& neighbours, const std::vector<bool>& keptCoarse) {
    std::vector<bool> coarse(neighbours.size(), true);
    std::vector<int> coarseNeighbours(neighbours.size());
    for (std::size_t point = 0; point < neighbours.size(); ++point) {
        coarseNeighbours[point] = static_cast<int>(neighbours[point].size());
    }

    for (std::size_t point = 0; point < neighbours.size(); ++point) {
        if (keptCoarse[point] || coarseNeighbours[point] < clothParentsWanted) {
            continue;
        }
        const bool othersKeepEnough = std::all_of(neighbours[point].begin(), neighbours[point].end(), [&](std::uint32_t other) {
            return coarse[other] || coarseNeighbours[other] > clothParentsWanted;
        });
        if (!othersKeepEnough) {
            continue;
        }
        coarse[point] = false;
        for (const std::uint32_t other : neighbours[point]) {
            --coarseNeighbours[other];
        }
    }
    return coarse;
}

std::vector<float> parentWeights(const Vec3& child, const std::vector<Vec3>& parents) {
    std::vector<float> distances;
    float farthest = 0.0f;
    for (const Vec3& parent : parents) {
        distances.push_back((parent - child).length());
        farthest = std::max(farthest, distances.back());
    }

    std::vector<float> weights(parents.size(), 1.0f);
    if (farthest > 0.0f) {
        for (std::size_t index = 0; index < parents.size(); ++index) {
            weights[index] = 1.0f / (distances[index] / farthest + parentWeightEpsilon);
        }
    }
    float total = 0.0f;
    for (const float weight : weights) {
        total += weight;
    }
    for (float& weight : weights) {
        weight /= total;
    }
    return weights;
}

/*
* Each level is built from the one below's edges, held as each point's sorted neighbours (empty for points not on the
* level). A fine point's parents are its coarse neighbours before any collapse, as the paper defines them on the finer
* level; collapsing then makes the coarser level's edges, every point's in index order, so the result never depends on
* anything but the mesh and its pins. A level that would hold fewer than smallestClothLevel points is not made.
*/
ClothHierarchy buildClothHierarchy(const std::vector<Vec3>& restPositions, const std::vector<StretchConstraint>& edges,
                                   const std::vector<float>& inverseMasses) {
    const std::size_t pointCount = restPositions.size();
    std::vector<std::vector<std::uint32_t>> neighbours(pointCount);
    for (const StretchConstraint& edge : edges) {
        if (!containsSorted(neighbours[edge.first], edge.second)) {
            insertSorted(neighbours[edge.first], edge.second);
            insertSorted(neighbours[edge.second], edge.first);
        }
    }
    std::vector<bool> pinned(pointCount);
    for (std::size_t point = 0; point < pointCount; ++point) {
        pinned[point] = inverseMasses[point] == 0.0f;
    }

    ClothHierarchy hierarchy;
    hierarchy.pointLevels.assign(pointCount, 0);
    std::vector<std::vector<ClothParent>> parentsOf(pointCount);
    std::vector<std::uint32_t> levelPoints(pointCount);
    for (std::size_t point = 0; point < pointCount; ++point) {
        levelPoints[point] = static_cast<std::uint32_t>(point);
    }

    for (int number = 1; number <= maxClothLevels; ++number) {
        const std::vector<bool> coarse = chooseCoarsePoints(neighbours, pinned);
        ClothLevel level;
        for (const std::uint32_t point : levelPoints) {
            (coarse[point] ? level.points : level.children).push_back(point);
        }
        if (level.children.empty() || level.points.size() < smallestClothLevel) {
            break;
        }

        std::vector<std::vector<std::uint32_t>> childParents(level.children.size());
        for (std::size_t index = 0; index < level.children.size(); ++index) {
            const std::uint32_t child = level.children[index];
            std::vector<Vec3> parentPositions;
            for (const std::uint32_t other : neighbours[child]) {
                if (coarse[other]) {
                    childParents[index].push_back(other);
                    parentPositions.push_back(restPositions[other]);
                }
            }
            const std::vector<float> weights = parentWeights(restPositions[child], parentPositions);
            for (std::size_t parent = 0; parent < weights.size(); ++parent) {
                parentsOf[child].push_back(ClothParent{childParents[index][parent], weights[parent]});
            }
        }
        for (std::size_t index = 0; index < level.children.size(); ++index) {
            collapse(neighbours, level.children[index], childParents[index], restPositions);
        }

        for (const std::uint32_t point : level.points) {
            hierarchy.pointLevels[point] = number;
            for (const std::uint32_t other : neighbours[point]) {
                if (other > point) {
                    level.limits.push_back(StretchConstraint{point, other, (restPositions[point] - restPositions[other]).length()});
                }
            }
        }
        level.limitColours = colourStretch(level.limits, pointCount);
        levelPoints = level.points;
        hierarchy.levels.push_back(std::move(level));
    }

    hierarchy.parentStarts.reserve(pointCount + 1);
    for (const std::vector<ClothParent>& parents : parentsOf) {
        hierarchy.parentStarts.push_back(static_cast<std::uint32_t>(hierarchy.parents.size()));
        hierarchy.parents.insert(hierarchy.parents.end(), parents.begin(), parents.end());
    }
    hierarchy.parentStarts.push_back(static_cast<std::uint32_t>(hierarchy.parents.size()));
    return hierarchy;
}

std::uint32_t pointOnLevel(const ClothHierarchy& hierarchy, std::uint32_t point, int level) {
    while (hierarchy.pointLevels[point] < level && hierarchy.parentStarts[point] < hierarchy.parentStarts[point + 1]) {
        const ClothParent* heaviest = &hierarchy.parents[hierarchy.parentStarts[point]];
        for (std::uint32_t index = hierarchy.parentStarts[point]; index < hierarchy.parentStarts[point + 1]; ++index) {
            if (hierarchy.parents[index].weight > heaviest->weight) {
                heaviest = &hierarchy.parents[index];
            }
        }
        point = heaviest->point;
    }
    return point;
}

/*
* A child reads only its parents, which are on a coarser level and never children in the same list, and writes only
* itself, so the children are shared among the jobs with the same result on any number of threads.
*/
void solveClothHierarchy(ClothParticles& particles, const ClothHierarchy& hierarchy, int levels, float limitCompliance,
                         const std::optional<ClothGrab>& grab, float grabCompliance, float seconds, std::vector<Vec3>& starts, JobSystem& jobs) {
    const int used = std::min(levels, static_cast<int>(hierarchy.levels.size()));
    if (used < 1) {
        return;
    }

    std::vector<Vec3>& positions = particles.positions;
    for (const std::uint32_t point : hierarchy.levels.front().points) {
        starts[point] = positions[point];
    }

    const auto follow = [&](const std::vector<std::uint32_t>& children) {
        jobs.parallelFor(children.size(), followPerJob, [&](std::size_t index) {
            const std::uint32_t child = children[index];
            if (particles.inverseMasses[child] == 0.0f) {
                return;
            }
            Vec3 motion{0.0f, 0.0f, 0.0f};
            for (std::uint32_t parent = hierarchy.parentStarts[child]; parent < hierarchy.parentStarts[child + 1]; ++parent) {
                const ClothParent& link = hierarchy.parents[parent];
                motion = motion + (positions[link.point] - starts[link.point]) * link.weight;
            }
            positions[child] = positions[child] + motion;
        });
    };

    for (int number = used; number >= 1; --number) {
        if (number < used) {
            follow(hierarchy.levels[static_cast<std::size_t>(number)].children);
        }
        const ClothLevel& level = hierarchy.levels[static_cast<std::size_t>(number - 1)];
        solveStretchLimits(particles, level.limits, level.limitColours, limitCompliance, seconds, jobs);
        if (grab) {
            const std::uint32_t standIn = pointOnLevel(hierarchy, grab->particle, number);
            const Vec3 target = positions[standIn] + (grab->target - positions[grab->particle]);
            solveGrab(particles, ClothGrab{standIn, target}, grabCompliance, seconds);
        }
    }
    follow(hierarchy.levels.front().children);
}
