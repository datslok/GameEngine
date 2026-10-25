#include "cloth/cloth_hierarchy.h"

#include "cloth/cloth_collision.h"
#include "cloth/cloth_shared.h"

#include <algorithm>
#include <cstddef>

namespace {
    /*
    * Keeps a parent's weight finite when the child lies on it. The paper gives no value; this one is small against
    * d / the farthest d for any two distinct points of a mesh, so it barely changes their weights.
    */
    constexpr float parentWeightEpsilon = 1e-3f;

    // Below this length an edge has no direction (cloth/cloth_shared.h).
    constexpr float shortestEdge = static_cast<float>(CLOTH_SHORTEST_EDGE);

    // Points a job takes at a time: each is a few weighted sums.
    constexpr std::size_t pointsPerJob = 1024;

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

    std::size_t indexOnLevel(const ClothLevel& level, std::uint32_t point) {
        return static_cast<std::size_t>(std::lower_bound(level.points.begin(), level.points.end(), point) - level.points.begin());
    }

    /*
    * Each level's weights straight from the cloth's points, and its coarse points' masses. Every point's row of P (its
    * weight for each point of the level, found by following its parents up: a few nonzero weights each) is made top
    * down, a child's from its parents', which are on a coarser level and done already. The rows are what spreading
    * reads, their transpose (each level point's column) what gathering reads, and the masses are the diagonal of
    * P^T M P: the sum over the free points of weight^2 times mass.
    */
    void addLevelWeights(ClothHierarchy& hierarchy, const std::vector<float>& inverseMasses) {
        const std::size_t pointCount = inverseMasses.size();
        for (std::size_t levelIndex = 0; levelIndex < hierarchy.levels.size(); ++levelIndex) {
            ClothLevel& level = hierarchy.levels[levelIndex];
            std::vector<std::vector<ClothLink>> rows(pointCount);
            for (const std::uint32_t point : level.points) {
                rows[point] = {ClothLink{point, 1.0f}};
            }
            for (std::size_t below = levelIndex + 1; below-- > 0;) {
                for (const std::uint32_t child : hierarchy.levels[below].children) {
                    std::vector<ClothLink> row;
                    for (std::uint32_t link = hierarchy.parentStarts[child]; link < hierarchy.parentStarts[child + 1]; ++link) {
                        const ClothLink& parent = hierarchy.parents[link];
                        for (const ClothLink& entry : rows[parent.point]) {
                            row.push_back(ClothLink{entry.point, entry.weight * parent.weight});
                        }
                    }
                    std::sort(row.begin(), row.end(), [](const ClothLink& a, const ClothLink& b) {
                        return a.point < b.point;
                    });
                    std::vector<ClothLink> merged;
                    for (const ClothLink& entry : row) {
                        if (!merged.empty() && merged.back().point == entry.point) {
                            merged.back().weight += entry.weight;
                        } else {
                            merged.push_back(entry);
                        }
                    }
                    rows[child] = std::move(merged);
                }
            }

            std::vector<std::vector<ClothLink>> columns(level.points.size());
            std::vector<double> masses(level.points.size(), 0.0);
            level.spreadStarts.reserve(pointCount + 1);
            for (std::uint32_t point = 0; point < pointCount; ++point) {
                level.spreadStarts.push_back(static_cast<std::uint32_t>(level.spread.size()));
                level.spread.insert(level.spread.end(), rows[point].begin(), rows[point].end());
                for (const ClothLink& entry : rows[point]) {
                    const std::size_t at = indexOnLevel(level, entry.point);
                    columns[at].push_back(ClothLink{point, entry.weight});
                    if (inverseMasses[point] > 0.0f) {
                        masses[at] += double(entry.weight) * entry.weight / inverseMasses[point];
                    }
                }
            }
            level.spreadStarts.push_back(static_cast<std::uint32_t>(level.spread.size()));
            for (const std::vector<ClothLink>& column : columns) {
                level.gatherStarts.push_back(static_cast<std::uint32_t>(level.gather.size()));
                level.gather.insert(level.gather.end(), column.begin(), column.end());
            }
            level.gatherStarts.push_back(static_cast<std::uint32_t>(level.gather.size()));
            level.masses.assign(masses.begin(), masses.end());
        }
    }

    // A level point's column of P times the values: what it gathers from the points it moves.
    Vec3 gatherAt(const ClothLevel& level, std::size_t at, const std::vector<Vec3>& values) {
        Vec3 sum{0.0f, 0.0f, 0.0f};
        for (std::uint32_t link = level.gatherStarts[at]; link < level.gatherStarts[at + 1]; ++link) {
            sum = sum + values[level.gather[link].point] * level.gather[link].weight;
        }
        return sum;
    }

    // A cloth point's row of P times the level points' values: its share of their moves.
    Vec3 spreadAt(const ClothLevel& level, std::size_t point, const std::vector<Vec3>& values) {
        Vec3 sum{0.0f, 0.0f, 0.0f};
        for (std::uint32_t link = level.spreadStarts[point]; link < level.spreadStarts[point + 1]; ++link) {
            sum = sum + values[level.spread[link].point] * level.spread[link].weight;
        }
        return sum;
    }

    /*
    * A coarse point's step: minus its gathered gradient, divided by its 3 x 3 stiffness: its region's mass / h^2 times
    * the identity plus its coarse edges' n n^T / compliance (an approximation of the fabric's share). Zero for a
    * singular one (a region with no free mass).
    */
    Vec3 coarseStep(const ClothLevel& level, std::size_t at, const std::vector<Vec3>& positions, const Vec3& gathered, float perSecondSquared,
                    float stiffness) {
        ClothStiffnessBlock block;
        block.addDiagonal(level.masses[at] * perSecondSquared);
        const Vec3& point = positions[level.points[at]];
        for (std::uint32_t next = level.neighbourStarts[at]; next < level.neighbourStarts[at + 1]; ++next) {
            const Vec3 between = positions[level.neighbours[next]] - point;
            const float lengthSquared = between.lengthSquared();
            if (lengthSquared >= shortestEdge * shortestEdge) {
                block.addOuter(between, stiffness / lengthSquared);
            }
        }
        return block.solve(gathered * -1.0f);
    }

    // Held points: the pinned, and those within thickness of a body, which the contacts hold.
    void markHeldPoints(const ClothParticles& particles, const std::vector<Shape>& bodies, float thickness, ClothHierarchyScratch& scratch,
                        JobSystem& jobs) {
        scratch.held.resize(particles.size());
        jobs.parallelFor(particles.size(), pointsPerJob, [&](std::size_t point) {
            bool held = particles.inverseMasses[point] == 0.0f;
            for (const Shape& body : bodies) {
                held = held || surfacePoint(body, particles.positions[point]).distance < thickness;
            }
            scratch.held[point] = held ? 1 : 0;
        });
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
* level; collapsing then makes the coarser level's mesh, every point's in index order, so the result never depends on
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
    std::vector<std::vector<ClothLink>> parentsOf(pointCount);
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
                parentsOf[child].push_back(ClothLink{childParents[index][parent], weights[parent]});
            }
        }
        for (std::size_t index = 0; index < level.children.size(); ++index) {
            collapse(neighbours, level.children[index], childParents[index], restPositions);
        }

        for (const std::uint32_t point : level.points) {
            hierarchy.pointLevels[point] = number;
            level.neighbourStarts.push_back(static_cast<std::uint32_t>(level.neighbours.size()));
            level.neighbours.insert(level.neighbours.end(), neighbours[point].begin(), neighbours[point].end());
        }
        level.neighbourStarts.push_back(static_cast<std::uint32_t>(level.neighbours.size()));
        levelPoints = level.points;
        hierarchy.levels.push_back(std::move(level));
    }

    hierarchy.parentStarts.reserve(pointCount + 1);
    for (const std::vector<ClothLink>& parents : parentsOf) {
        hierarchy.parentStarts.push_back(static_cast<std::uint32_t>(hierarchy.parents.size()));
        hierarchy.parents.insert(hierarchy.parents.end(), parents.begin(), parents.end());
    }
    hierarchy.parentStarts.push_back(static_cast<std::uint32_t>(hierarchy.parents.size()));
    addLevelWeights(hierarchy, inverseMasses);
    return hierarchy;
}

/*
* One pass: each level point sums its column, its own value and those of the points below it that follow it; it never
* reads another level point, so the level's points can be written in place, all at once.
*/
void restrictToLevel(const ClothHierarchy& hierarchy, std::vector<Vec3>& values, int level, JobSystem& jobs) {
    const ClothLevel& current = hierarchy.levels[static_cast<std::size_t>(level - 1)];
    jobs.parallelFor(current.points.size(), pointsPerJob, [&](std::size_t at) {
        values[current.points[at]] = gatherAt(current, at, values);
    });
}

// One pass: each point below the level sums its row, which holds only level points, never written here.
void prolongFromLevel(const ClothHierarchy& hierarchy, std::vector<Vec3>& values, int level, JobSystem& jobs) {
    const ClothLevel& current = hierarchy.levels[static_cast<std::size_t>(level - 1)];
    jobs.parallelFor(values.size(), pointsPerJob, [&](std::size_t point) {
        if (hierarchy.pointLevels[point] < level) {
            values[point] = spreadAt(current, point, values);
        }
    });
}

/*
* Each level's step uses the gradient at the points as the coarser levels left them (multiplicative, as a V-cycle's way
* down), so a level never undoes a coarser one's work.
*/
void solveClothHierarchy(ClothParticles& particles, const ClothHierarchy& hierarchy, int levels, const ClothEnergyTerms& terms,
                         const std::vector<Shape>& bodies, float thickness, float seconds, ClothHierarchyScratch& scratch, JobSystem& jobs) {
    const int used = std::min(levels, static_cast<int>(hierarchy.levels.size()));
    if (used < 1) {
        return;
    }

    const std::vector<Vec3>& positions = particles.positions;
    const std::size_t pointCount = particles.size();
    scratch.gradient.resize(pointCount);
    scratch.direction.resize(pointCount);
    scratch.coarseSteps.resize(pointCount);
    markHeldPoints(particles, bodies, thickness, scratch, jobs);

    const float perSecondSquared = 1.0f / (seconds * seconds);
    const float stiffness = energyStiffness(terms.stretchCompliance);
    for (int number = used; number >= 1; --number) {
        const ClothLevel& level = hierarchy.levels[static_cast<std::size_t>(number - 1)];
        clothEnergyGradient(particles, scratch.predicted, terms, seconds, scratch.held, scratch.gradient, jobs);

        // Gather and step in one pass: each level point's step from its column of the gradient.
        jobs.parallelFor(level.points.size(), pointsPerJob, [&](std::size_t at) {
            const std::uint32_t point = level.points[at];
            const Vec3 step = coarseStep(level, at, positions, gatherAt(level, at, scratch.gradient), perSecondSquared, stiffness);
            scratch.coarseSteps[point] = step;
            scratch.direction[point] = scratch.held[point] != 0 ? Vec3{0.0f, 0.0f, 0.0f} : step;
        });
        // Spread down, held points staying where they are, in one pass.
        jobs.parallelFor(pointCount, pointsPerJob, [&](std::size_t point) {
            if (scratch.held[point] != 0) {
                scratch.direction[point] = Vec3{0.0f, 0.0f, 0.0f};
                return;
            }
            if (hierarchy.pointLevels[point] < number) {
                scratch.direction[point] = spreadAt(level, point, scratch.coarseSteps);
            }
        });

        clothLineSearch(particles, scratch.gradient, scratch.direction, terms, seconds, jobs);
    }
}
