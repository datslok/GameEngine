#include "cloth/cloth_self_collision.h"

#include "cloth/cloth.h"
#include "math/aabb.h"
#include "math/closest_points.h"
#include "physics/overlap.h"
#include "physics/uniform_grid.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace {
    // Shorter than this, the line between two nearest points has no direction (cloth/cloth_shared.h).
    constexpr float shortestEdge = static_cast<float>(CLOTH_SHORTEST_EDGE);

    const Vec3 straightUp{0.0f, 1.0f, 0.0f};

    // The box round some points, grown by margin on every side.
    template<std::size_t Count>
    Aabb boxAround(const std::array<Vec3, Count>& corners, float margin) {
        Aabb box{corners[0], corners[0]};
        for (const Vec3& corner : corners) {
            box = enclosing(box, Aabb{corner, corner});
        }
        const Vec3 grown{margin, margin, margin};
        return Aabb{box.minimum - grown, box.maximum + grown};
    }

    std::array<Vec3, 3> corners(const std::vector<Vec3>& positions, const std::array<std::uint32_t, 3>& triangle) {
        return {positions[triangle[0]], positions[triangle[1]], positions[triangle[2]]};
    }

    // The unit direction from second to first, or fallback where they meet.
    Vec3 directionBetween(const Vec3& first, const Vec3& second, const Vec3& fallback, float& distance) {
        const Vec3 between = first - second;
        distance = between.length();
        return distance > shortestEdge ? between * (1.0f / distance) : fallback;
    }

    // A unit vector at right angles to both, or up when they are parallel: the way apart for features that meet.
    Vec3 acrossBoth(const Vec3& first, const Vec3& second) {
        const Vec3 across = first.cross(second);
        const float length = across.length();
        return length > shortestEdge ? across * (1.0f / length) : straightUp;
    }

    // A pair's heights above the second side's nearest point, from its points as they are.
    void measureHeights(ClothSelfPair& pair, const std::vector<Vec3>& positions, const Vec3& secondNearest) {
        for (std::size_t index = 0; index < pair.points.size(); ++index) {
            pair.heights[index] = (positions[pair.points[index]] - secondNearest).dot(pair.normal);
        }
    }

    // Whether any of one set of points is one with, or (with neighboursToo) one edge from, any of the other.
    template<std::size_t First, std::size_t Second>
    bool anyNear(const ClothSelfTopology& topology, const std::array<std::uint32_t, First>& first, const std::array<std::uint32_t, Second>& second,
                 bool neighboursToo) {
        for (const std::uint32_t one : first) {
            for (const std::uint32_t other : second) {
                const bool same = topology.joined[one] == topology.joined[other];
                if (same || (neighboursToo && nearInCloth(topology, one, other))) {
                    return true;
                }
            }
        }
        return false;
    }

    /*
    * A pair's nearest points as they are now and each point's coefficient along the line between them: +weight on the
    * first side, -weight on the second, so the gap's motion is the sum of coefficient x motion.
    */
    struct PairNow {
        Vec3 firstNearest;
        Vec3 secondNearest;
        std::array<float, 4> coefficients{};
    };

    /*
    * A spatial hash grid built by a counting sort, as the GPU builds one: each item's box is cut into cubic cells, each
    * cell hashed into a table of buckets (Teschner et al. 2003's primes, as UniformGrid's), and the items laid out bucket
    * by bucket in one array: a pass counting each bucket, a running total, a pass filling them. Nothing is allocated
    * per cell, and once built it is only read, so many threads may query it at once. Cells that share a bucket are
    * mixed, so a query may hand back an item from another cell; callers measure every candidate anyway. An item whose box
    * would cover more than largeCellsPerAxis cells along an axis (a cloth torn wide open) goes on a list every query
    * gets, rather than filling the table.
    */
    class SpatialHash {
    public:
        static constexpr int largeCellsPerAxis = 16;

        SpatialHash(float cellSize, const std::vector<Aabb>& boxes): cellSize(cellSize), itemCount(boxes.size()) {
            std::size_t cellCount = 0;
            std::vector<std::uint8_t> large(boxes.size(), 0);
            for (std::size_t index = 0; index < boxes.size(); ++index) {
                const GridCell low = cellOf(boxes[index].minimum, cellSize);
                const GridCell high = cellOf(boxes[index].maximum, cellSize);
                if (high.x - low.x >= largeCellsPerAxis || high.y - low.y >= largeCellsPerAxis || high.z - low.z >= largeCellsPerAxis) {
                    large[index] = 1;
                    largeItems.push_back(static_cast<std::uint32_t>(index));
                    continue;
                }
                cellCount += static_cast<std::size_t>(high.x - low.x + 1) * static_cast<std::size_t>(high.y - low.y + 1) *
                             static_cast<std::size_t>(high.z - low.z + 1);
            }

            // About two buckets a cell entry keeps few cells sharing one.
            const std::size_t buckets = std::bit_ceil(std::max<std::size_t>(2 * cellCount, 64));
            mask = buckets - 1;
            starts.assign(buckets + 1, 0);

            const auto eachCell = [&](const Aabb& box, auto&& visit) {
                const GridCell low = cellOf(box.minimum, cellSize);
                const GridCell high = cellOf(box.maximum, cellSize);
                for (int x = low.x; x <= high.x; ++x) {
                    for (int y = low.y; y <= high.y; ++y) {
                        for (int z = low.z; z <= high.z; ++z) {
                            visit(bucketOf(GridCell{x, y, z}));
                        }
                    }
                }
            };

            for (std::size_t index = 0; index < boxes.size(); ++index) {
                if (large[index] == 0) {
                    eachCell(boxes[index], [&](std::size_t bucket) {
                        ++starts[bucket + 1];
                    });
                }
            }
            for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
                starts[bucket + 1] += starts[bucket];
            }

            items.resize(starts[buckets]);
            std::vector<std::uint32_t> next(starts.begin(), starts.end() - 1);
            for (std::size_t index = 0; index < boxes.size(); ++index) {
                if (large[index] == 0) {
                    eachCell(boxes[index], [&](std::size_t bucket) {
                        items[next[bucket]++] = static_cast<std::uint32_t>(index);
                    });
                }
            }
        }

        /*
        * Every item whose box may meet region, each once, into found (replacing what it held): the large items, then
        * the cells in a fixed order, each bucket in its order, so the same grid and region always give the same list.
        * An item met twice (in two of the region's cells) is kept the first time, by marking it with the query's number
        * in a list kept per thread ("mailboxing"), rather than sorting each list, which cost more than the search.
        */
        void gather(const Aabb& region, std::vector<std::uint32_t>& found) const {
            static thread_local std::vector<std::uint32_t> seenBy;
            static thread_local std::uint32_t query = 0;
            if (seenBy.size() < itemCount) {
                seenBy.resize(itemCount, 0);
            }
            if (++query == 0) {
                std::fill(seenBy.begin(), seenBy.end(), 0);
                query = 1;
            }
            const auto meet = [&](std::uint32_t item) {
                if (seenBy[item] != query) {
                    seenBy[item] = query;
                    found.push_back(item);
                }
            };

            found.clear();
            for (const std::uint32_t item : largeItems) {
                meet(item);
            }
            const GridCell low = cellOf(region.minimum, cellSize);
            const GridCell high = cellOf(region.maximum, cellSize);
            for (int x = low.x; x <= high.x; ++x) {
                for (int y = low.y; y <= high.y; ++y) {
                    for (int z = low.z; z <= high.z; ++z) {
                        const std::size_t bucket = bucketOf(GridCell{x, y, z});
                        for (std::uint32_t entry = starts[bucket]; entry < starts[bucket + 1]; ++entry) {
                            meet(items[entry]);
                        }
                    }
                }
            }
        }

    private:
        std::size_t bucketOf(const GridCell& cell) const {
            const std::uint64_t hashed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.x)) * 73856093u) ^
                                         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.y)) * 19349663u) ^
                                         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cell.z)) * 83492791u);
            return static_cast<std::size_t>(hashed & mask);
        }

        float cellSize;
        std::size_t itemCount;
        std::uint64_t mask = 0;
        std::vector<std::uint32_t> starts;
        std::vector<std::uint32_t> items;
        std::vector<std::uint32_t> largeItems;
    };

    // The mean length of a cloth's edges as the positions have them.
    float meanEdgeLength(const std::vector<Vec3>& positions, const std::vector<std::array<std::uint32_t, 2>>& edges) {
        if (edges.empty()) {
            return 0.0f;
        }
        double total = 0.0;
        for (const std::array<std::uint32_t, 2>& edge : edges) {
            total += (positions[edge[1]] - positions[edge[0]]).length();
        }
        return static_cast<float>(total / static_cast<double>(edges.size()));
    }

    PairNow pairNow(const ClothSelfPair& pair, const std::vector<Vec3>& positions) {
        const std::array<Vec3, 4> at{positions[pair.points[0]], positions[pair.points[1]], positions[pair.points[2]], positions[pair.points[3]]};
        if (pair.kind == ClothPairKind::PointTriangle) {
            const TriangleClosest closest = closestTriangleFeature(at[0], at[1], at[2], at[3]);
            return {at[0], closest.point, {1.0f, -closest.weights.x, -closest.weights.y, -closest.weights.z}};
        }
        const ClosestPoints closest = closestPointsBetweenSegments(at[0], at[1], at[2], at[3]);
        return {closest.onFirst, closest.onSecond,
                {1.0f - closest.firstShare, closest.firstShare, closest.secondShare - 1.0f, -closest.secondShare}};
    }
}

/*
* Stitches first, so a stitched pair's representative is known before the edges are read; the neighbour lists are then
* gathered per representative, sorted, and laid end to end.
*/
ClothSelfTopology clothSelfTopology(const std::vector<Vec3>& restPositions, const std::vector<std::array<std::uint32_t, 3>>& triangles,
                                    const std::vector<ClothStitch>& stitches) {
    const std::size_t points = restPositions.size();
    ClothSelfTopology topology;
    topology.restPositions = restPositions;
    topology.joined.resize(points);
    for (std::size_t index = 0; index < points; ++index) {
        topology.joined[index] = static_cast<std::uint32_t>(index);
    }
    for (const ClothStitch& stitch : stitches) {
        const std::uint32_t representative = std::min(stitch.first, stitch.second);
        topology.joined.at(stitch.first) = representative;
        topology.joined.at(stitch.second) = representative;
    }

    for (const std::array<std::uint32_t, 3>& triangle : triangles) {
        for (int corner = 0; corner < 3; ++corner) {
            const std::uint32_t from = triangle[corner];
            const std::uint32_t to = triangle[(corner + 1) % 3];
            topology.edges.push_back({std::min(from, to), std::max(from, to)});
        }
    }
    std::sort(topology.edges.begin(), topology.edges.end());
    topology.edges.erase(std::unique(topology.edges.begin(), topology.edges.end()), topology.edges.end());

    std::vector<std::vector<std::uint32_t>> lists(points);
    for (const std::array<std::uint32_t, 2>& edge : topology.edges) {
        const std::uint32_t first = topology.joined[edge[0]];
        const std::uint32_t second = topology.joined[edge[1]];
        if (first != second) {
            lists[first].push_back(second);
            lists[second].push_back(first);
        }
    }

    topology.neighbourStarts.reserve(points + 1);
    for (std::vector<std::uint32_t>& list : lists) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
        topology.neighbourStarts.push_back(static_cast<std::uint32_t>(topology.neighbours.size()));
        topology.neighbours.insert(topology.neighbours.end(), list.begin(), list.end());
    }
    topology.neighbourStarts.push_back(static_cast<std::uint32_t>(topology.neighbours.size()));
    return topology;
}

bool nearInCloth(const ClothSelfTopology& topology, std::uint32_t first, std::uint32_t second) {
    const std::uint32_t one = topology.joined[first];
    const std::uint32_t other = topology.joined[second];
    if (one == other) {
        return true;
    }
    const auto begin = topology.neighbours.begin() + topology.neighbourStarts[one];
    const auto end = topology.neighbours.begin() + topology.neighbourStarts[one + 1];
    return std::binary_search(begin, end, other);
}

int firstSidePoints(ClothPairKind kind) {
    return kind == ClothPairKind::PointTriangle ? 1 : 2;
}

// Where the point meets the triangle, it is pushed off along the triangle's normal.
ClothSelfPair pointTrianglePair(const std::vector<Vec3>& positions, std::uint32_t point, const std::array<std::uint32_t, 3>& triangle) {
    const std::array<Vec3, 3> at = corners(positions, triangle);
    const TriangleClosest closest = closestTriangleFeature(positions[point], at[0], at[1], at[2]);

    ClothSelfPair pair;
    pair.kind = ClothPairKind::PointTriangle;
    pair.points = {point, triangle[0], triangle[1], triangle[2]};
    pair.normal = directionBetween(positions[point], closest.point, acrossBoth(at[1] - at[0], at[2] - at[0]), pair.gap);
    measureHeights(pair, positions, closest.point);
    return pair;
}

// Where two edges meet, they are pushed apart at right angles to both.
ClothSelfPair edgeEdgePair(const std::vector<Vec3>& positions, const std::array<std::uint32_t, 2>& first, const std::array<std::uint32_t, 2>& second) {
    const Vec3& a = positions[first[0]];
    const Vec3& b = positions[first[1]];
    const Vec3& c = positions[second[0]];
    const Vec3& d = positions[second[1]];
    const ClosestPoints closest = closestPointsBetweenSegments(a, b, c, d);

    ClothSelfPair pair;
    pair.kind = ClothPairKind::EdgeEdge;
    pair.points = {first[0], first[1], second[0], second[1]};
    pair.normal = directionBetween(closest.onFirst, closest.onSecond, acrossBoth(b - a, d - c), pair.gap);
    measureHeights(pair, positions, closest.onSecond);
    return pair;
}

namespace {
    // How many points or edges one job looks at: enough that handing it out costs little next to its work.
    constexpr std::size_t pairSearchBlock = 256;

    /*
    * Each of count items' pairs, found by find(index, candidates, out) a block of items a job, joined in the items'
    * order, so the result is the same on any number of threads.
    */
    template<typename Find>
    std::vector<ClothSelfPair> findInBlocks(std::size_t count, JobSystem& jobs, Find&& find) {
        const std::size_t blocks = (count + pairSearchBlock - 1) / pairSearchBlock;
        std::vector<std::vector<ClothSelfPair>> found(blocks);
        jobs.parallelFor(blocks, 1, [&](std::size_t block) {
            std::vector<std::uint32_t> candidates;
            const std::size_t end = std::min(count, (block + 1) * pairSearchBlock);
            for (std::size_t index = block * pairSearchBlock; index < end; ++index) {
                find(static_cast<std::uint32_t>(index), candidates, found[block]);
            }
        });

        std::vector<ClothSelfPair> pairs;
        for (std::vector<ClothSelfPair>& part : found) {
            pairs.insert(pairs.end(), part.begin(), part.end());
        }
        return pairs;
    }
}

/*
* Two spatial hashes, cells an edge and two reaches wide (so a triangle grown by the reach covers a few cells): the
* triangles' boxes grown by the reach, met by each point; then the edges' boxes grown by the reach, met by each edge's
* box, each two edges once (the later met by the earlier). Only neighbours have their rest gap measured: anything
* further apart in the mesh is at least about two triangles' heights apart at rest.
*/
std::vector<ClothSelfPair> findClothPairs(const std::vector<Vec3>& positions, const std::vector<std::array<std::uint32_t, 3>>& triangles,
                                          const ClothSelfTopology& topology, float reach, float thickness, JobSystem& jobs) {
    const std::vector<std::array<std::uint32_t, 2>>& edges = topology.edges;
    const float cellSize = std::max(meanEdgeLength(positions, edges) + 2.0f * reach, 1e-4f);

    std::vector<Aabb> boxes(triangles.size());
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        boxes[index] = boxAround(corners(positions, triangles[index]), reach);
    }
    const SpatialHash triangleHash{cellSize, boxes};

    std::vector<ClothSelfPair> pairs = findInBlocks(positions.size(), jobs, [&](std::uint32_t point, std::vector<std::uint32_t>& candidates,
                                                                                std::vector<ClothSelfPair>& out) {
        triangleHash.gather(Aabb{positions[point], positions[point]}, candidates);
        const std::array<std::uint32_t, 1> alone{point};
        for (const std::uint32_t triangle : candidates) {
            if (!boxes[triangle].contains(positions[point]) || anyNear(topology, alone, triangles[triangle], false)) {
                continue;
            }
            const std::array<Vec3, 3> at = corners(positions, triangles[triangle]);
            if ((closestTriangleFeature(positions[point], at[0], at[1], at[2]).point - positions[point]).lengthSquared() >= reach * reach ||
                (anyNear(topology, alone, triangles[triangle], true) &&
                 pointTrianglePair(topology.restPositions, point, triangles[triangle]).gap < thickness)) {
                continue;
            }
            out.push_back(pointTrianglePair(positions, point, triangles[triangle]));
        }
    });

    boxes.resize(edges.size());
    for (std::size_t index = 0; index < edges.size(); ++index) {
        boxes[index] = boxAround(std::array<Vec3, 2>{positions[edges[index][0]], positions[edges[index][1]]}, reach);
    }
    const SpatialHash edgeHash{cellSize, boxes};

    const std::vector<ClothSelfPair> edgePairs = findInBlocks(edges.size(), jobs, [&](std::uint32_t edge, std::vector<std::uint32_t>& candidates,
                                                                                      std::vector<ClothSelfPair>& out) {
        const Aabb own = boxAround(std::array<Vec3, 2>{positions[edges[edge][0]], positions[edges[edge][1]]}, 0.0f);
        edgeHash.gather(own, candidates);
        for (const std::uint32_t other : candidates) {
            if (other <= edge || !overlaps(own, boxes[other]) || anyNear(topology, edges[edge], edges[other], false)) {
                continue;
            }
            const ClosestPoints closest = closestPointsBetweenSegments(positions[edges[edge][0]], positions[edges[edge][1]], positions[edges[other][0]],
                                                                      positions[edges[other][1]]);
            if (closest.distanceSquared() >= reach * reach || (anyNear(topology, edges[edge], edges[other], true) &&
                                                               edgeEdgePair(topology.restPositions, edges[edge], edges[other]).gap < thickness)) {
                continue;
            }
            out.push_back(edgeEdgePair(positions, edges[edge], edges[other]));
        }
    });

    pairs.insert(pairs.end(), edgePairs.begin(), edgePairs.end());
    return pairs;
}

float dividingHeight(const ClothSelfPair& pair, const std::array<Vec3, 4>& motions) {
    const int firstPoints = firstSidePoints(pair.kind);
    float firstApproach = 0.0f;
    float secondApproach = 0.0f;
    for (int index = 0; index < 4; ++index) {
        const float along = motions[index].dot(pair.normal);
        if (index < firstPoints) {
            firstApproach = std::max(firstApproach, -along);
        } else {
            secondApproach = std::max(secondApproach, along);
        }
    }

    const float approach = firstApproach + secondApproach;
    const float firstShare = approach > 0.0f ? firstApproach / approach : 0.5f;
    return pair.gap * (1.0f - firstShare);
}

/*
* The paper lets a point with t_i < 0 move freely; here a point's distance is measured from where it was when the pairs
* were found, on its own side, so it is below zero only by rounding at a gap squeezed to nothing, and letting it go on
* would let it through. It stops instead.
*/
float cutShare(float distance, float approach) {
    if (approach <= 0.0f) {
        return 1.0f;
    }
    if (distance <= 0.0f) {
        return 0.0f;
    }
    const float reached = distance / approach;
    return reached >= 1.0f / clothCutRelaxation ? 1.0f : clothCutRelaxation * reached;
}

/*
* Each pair writes its cut to its free points, the least winning (the GPU's atomic minimum); then each free point's
* motion is cut once. A point whose motion is kept whole is left exactly where it is, not rebuilt from start and its
* motion, which could round it a little.
*/
void cutClothMotion(std::vector<Vec3>& positions, const std::vector<Vec3>& start, const std::vector<float>& inverseMasses,
                    const std::vector<ClothSelfPair>& pairs, float reach) {
    std::vector<float> shares(positions.size(), 1.0f);

    for (const ClothSelfPair& pair : pairs) {
        std::array<Vec3, 4> motions;
        for (std::size_t index = 0; index < 4; ++index) {
            motions[index] = positions[pair.points[index]] - start[pair.points[index]];
        }
        const float plane = dividingHeight(pair, motions);
        const int firstPoints = firstSidePoints(pair.kind);

        for (int index = 0; index < 4; ++index) {
            const std::uint32_t point = pair.points[index];
            if (inverseMasses[point] == 0.0f) {
                continue;
            }
            const float along = motions[index].dot(pair.normal);
            const float share = index < firstPoints ? cutShare(pair.heights[index] - plane, -along) : cutShare(plane - pair.heights[index], along);
            shares[point] = std::min(shares[point], share);
        }
    }

    const float longest = 0.5f * clothCutRelaxation * reach;
    for (std::size_t point = 0; point < positions.size(); ++point) {
        if (inverseMasses[point] == 0.0f) {
            continue;
        }
        const Vec3 motion = positions[point] - start[point];
        const float length = motion.length();
        float share = shares[point];
        if (length * share > longest) {
            share = longest / length;
        }
        if (share < 1.0f) {
            positions[point] = start[point] + motion * share;
        }
    }
}

/*
* The constraint C = distance - thickness >= 0 along n, with no give: its gradient on each point is its coefficient
* times n, so the step lambda = (thickness - distance) / sum(coefficient^2 / mass) moves each point by lambda x
* coefficient / its mass along n, which opens the gap by exactly what was missing and moves no centre of mass. Friction
* then works on the slide between the nearest points during the substep, spread the same way.
*/
void solveSelfContacts(ClothParticles& particles, const std::vector<ClothSelfPair>& pairs, const ClothSettings& settings) {
    std::vector<Vec3>& positions = particles.positions;

    for (const ClothSelfPair& pair : pairs) {
        const PairNow now = pairNow(pair, positions);
        float distance = 0.0f;
        const Vec3 normal = directionBetween(now.firstNearest, now.secondNearest, pair.normal, distance);
        const float push = settings.thickness - distance;
        if (push <= 0.0f) {
            continue;
        }

        float weights = 0.0f;
        for (std::size_t index = 0; index < 4; ++index) {
            weights += now.coefficients[index] * now.coefficients[index] * particles.inverseMasses[pair.points[index]];
        }
        if (weights <= 0.0f) {
            continue;
        }

        const float step = push / weights;
        Vec3 slide;
        for (std::size_t index = 0; index < 4; ++index) {
            const std::uint32_t point = pair.points[index];
            positions[point] = positions[point] + normal * (step * now.coefficients[index] * particles.inverseMasses[point]);
            slide = slide + (positions[point] - particles.previousPositions[point]) * now.coefficients[index];
        }

        const Vec3 across = slide - normal * normal.dot(slide);
        const float acrossLength = across.length();
        const float held = acrossLength < settings.staticFriction * push ? 1.0f
                           : acrossLength > 0.0f                         ? std::min(settings.kineticFriction * push / acrossLength, 1.0f)
                                                                         : 0.0f;
        if (held <= 0.0f) {
            continue;
        }
        const Vec3 undone = across * (held / weights);
        for (std::size_t index = 0; index < 4; ++index) {
            const std::uint32_t point = pair.points[index];
            positions[point] = positions[point] - undone * (now.coefficients[index] * particles.inverseMasses[point]);
        }
    }
}

namespace {
    // Whether the segment passes through the triangle's inside: its ends strictly on either side of the plane and the
    // crossing within all three edges.
    bool passesThrough(const Vec3& from, const Vec3& to, const Vec3& a, const Vec3& b, const Vec3& c) {
        const Vec3 normal = (b - a).cross(c - a);
        const float fromSide = normal.dot(from - a);
        const float toSide = normal.dot(to - a);
        if (!((fromSide > 0.0f && toSide < 0.0f) || (fromSide < 0.0f && toSide > 0.0f))) {
            return false;
        }
        const Vec3 crossing = from + (to - from) * (fromSide / (fromSide - toSide));
        return normal.dot((b - a).cross(crossing - a)) >= 0.0f && normal.dot((c - b).cross(crossing - b)) >= 0.0f &&
               normal.dot((a - c).cross(crossing - c)) >= 0.0f;
    }
}

std::size_t countClothCrossings(const std::vector<Vec3>& positions, const std::vector<std::array<std::uint32_t, 3>>& triangles,
                                const ClothSelfTopology& topology) {
    if (topology.edges.empty()) {
        return 0;
    }

    std::vector<Aabb> boxes(triangles.size());
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        boxes[index] = boxAround(corners(positions, triangles[index]), 0.0f);
    }
    const SpatialHash grid{std::max(meanEdgeLength(positions, topology.edges), 1e-4f), boxes};

    std::size_t crossings = 0;
    std::vector<std::uint32_t> found;
    for (const std::array<std::uint32_t, 2>& edge : topology.edges) {
        grid.gather(boxAround(std::array<Vec3, 2>{positions[edge[0]], positions[edge[1]]}, 0.0f), found);
        for (const std::uint32_t index : found) {
            const std::array<std::uint32_t, 3>& triangle = triangles[index];
            bool shared = false;
            for (const std::uint32_t corner : triangle) {
                const std::uint32_t joined = topology.joined[corner];
                shared = shared || joined == topology.joined[edge[0]] || joined == topology.joined[edge[1]];
            }
            if (!shared && passesThrough(positions[edge[0]], positions[edge[1]], positions[triangle[0]], positions[triangle[1]], positions[triangle[2]])) {
                ++crossings;
            }
        }
    }
    return crossings;
}
