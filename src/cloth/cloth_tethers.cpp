#include "cloth/cloth_tethers.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
    // Points a job: a tether is a few multiplies, so a job needs many points to be worth handing out.
    constexpr std::size_t tetheredPointsPerJob = 1024;

    // Points a job while measuring tethers along pieces: each point's sight lines are a few thousand segment tests.
    constexpr std::size_t measuredPointsPerJob = 64;

    constexpr float infinitelyFar = std::numeric_limits<float>::infinity();

    // How near the outline a point may be and still count as on it, in metres.
    constexpr float onOutline = 1e-5f;

    // Twice the signed area of a, b, c, in doubles: positive when they turn counterclockwise, exactly 0 when two are the same point.
    double turn(const Vec2& a, const Vec2& b, const Vec2& c) {
        return (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) - (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x);
    }

    // Whether segments ab and cd cross at a point inside both: merely touching (at an end, or through a corner) is not crossing.
    bool crossProperly(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
        const double aSide = turn(c, d, a);
        const double bSide = turn(c, d, b);
        const double cSide = turn(a, b, c);
        const double dSide = turn(a, b, d);
        return ((aSide > 0.0 && bSide < 0.0) || (aSide < 0.0 && bSide > 0.0)) && ((cSide > 0.0 && dSide < 0.0) || (cSide < 0.0 && dSide > 0.0));
    }

    float distanceToSegment(const Vec2& point, const Vec2& a, const Vec2& b) {
        const Vec2 side = b - a;
        const float lengthSquared = side.lengthSquared();
        const float share = lengthSquared > 0.0f ? std::clamp((point - a).dot(side) / lengthSquared, 0.0f, 1.0f) : 0.0f;
        return (point - (a + side * share)).length();
    }

    // Inside the closed outline (an odd number of its sides cross the line to the point's right), or on it.
    bool insideOrOn(const std::vector<Vec2>& outline, const Vec2& point) {
        bool inside = false;
        for (std::size_t index = 0, last = outline.size() - 1; index < outline.size(); last = index++) {
            const Vec2& a = outline[index];
            const Vec2& b = outline[last];
            if (distanceToSegment(point, a, b) <= onOutline) {
                return true;
            }
            if ((a.y > point.y) != (b.y > point.y) && point.x < a.x + (b.x - a.x) * (point.y - a.y) / (b.y - a.y)) {
                inside = !inside;
            }
        }
        return inside;
    }

    Vec2 flat(const Vec3& position) {
        return Vec2{position.x, position.y};
    }
}

/*
* Each piece's reflex corners are where its outline turns clockwise; each anchor's distances to them come from
* Dijkstra's algorithm over the corners' sight lines, a dense graph of a few hundred corners at most, so the plain
* O(n^2) form with no heap.
*/
ClothRestPaths::ClothRestPaths(const std::vector<Vec3>& restPositions, const ClothPieces& pieces, const std::vector<std::uint32_t>& anchors):
    restPositions(restPositions),
    pieces(pieces),
    anchors(anchors)
{
    if (pieces.outlines.empty()) {
        return;
    }
    if (pieces.pieceOf.size() != restPositions.size()) {
        throw std::invalid_argument("A cloth's pieces need a piece for every point");
    }

    for (const std::vector<std::uint32_t>& outline : pieces.outlines) {
        Piece piece;
        for (const std::uint32_t point : outline) {
            piece.outline.push_back(flat(restPositions.at(point)));
        }
        for (std::size_t index = 0; index < piece.outline.size() && piece.outline.size() >= 3; ++index) {
            const Vec2& before = piece.outline[(index + piece.outline.size() - 1) % piece.outline.size()];
            const Vec2& after = piece.outline[(index + 1) % piece.outline.size()];
            if (turn(before, piece.outline[index], after) < 0.0) {
                piece.corners.push_back(piece.outline[index]);
            }
        }
        flatPieces.push_back(std::move(piece));
    }
    for (std::uint32_t index = 0; index < anchors.size(); ++index) {
        flatPieces.at(pieces.pieceOf.at(anchors[index])).anchors.push_back(index);
    }

    for (Piece& piece : flatPieces) {
        const std::size_t corners = piece.corners.size();
        std::vector<float> between(corners * corners, infinitelyFar);
        for (std::size_t from = 0; from < corners; ++from) {
            for (std::size_t to = from + 1; to < corners; ++to) {
                if (sees(piece, piece.corners[from], piece.corners[to])) {
                    const float length = (piece.corners[to] - piece.corners[from]).length();
                    between[from * corners + to] = length;
                    between[to * corners + from] = length;
                }
            }
        }

        for (const std::uint32_t anchor : piece.anchors) {
            const Vec2 start = flat(restPositions[anchors[anchor]]);
            std::vector<float> distance(corners, infinitelyFar);
            for (std::size_t corner = 0; corner < corners; ++corner) {
                if (sees(piece, start, piece.corners[corner])) {
                    distance[corner] = (piece.corners[corner] - start).length();
                }
            }
            std::vector<bool> settled(corners, false);
            for (std::size_t round = 0; round < corners; ++round) {
                std::size_t nearest = corners;
                for (std::size_t corner = 0; corner < corners; ++corner) {
                    if (!settled[corner] && distance[corner] < infinitelyFar && (nearest == corners || distance[corner] < distance[nearest])) {
                        nearest = corner;
                    }
                }
                if (nearest == corners) {
                    break;
                }
                settled[nearest] = true;
                for (std::size_t corner = 0; corner < corners; ++corner) {
                    distance[corner] = std::min(distance[corner], distance[nearest] + between[nearest * corners + corner]);
                }
            }
            piece.cornerDistances.push_back(std::move(distance));
        }
    }
}

/*
* A sight line is blocked when it crosses one of the outline's sides; one that crosses none lies wholly inside or
* wholly outside (it can only leave through a side or a corner), so its middle tells which: a chord across a
* neckline's curve, between two points on it, crosses nothing and its middle is outside.
*/
bool ClothRestPaths::sees(const Piece& piece, const Vec2& from, const Vec2& to) const {
    if (piece.outline.size() < 3) {
        return true;
    }
    for (std::size_t index = 0; index < piece.outline.size(); ++index) {
        if (crossProperly(from, to, piece.outline[index], piece.outline[(index + 1) % piece.outline.size()])) {
            return false;
        }
    }
    return insideOrOn(piece.outline, (from + to) * 0.5f);
}

void ClothRestPaths::distances(std::uint32_t point, std::vector<float>& distances) const {
    distances.assign(anchors.size(), infinitelyFar);
    if (flatPieces.empty()) {
        for (std::size_t index = 0; index < anchors.size(); ++index) {
            distances[index] = (restPositions.at(anchors[index]) - restPositions.at(point)).length();
        }
        return;
    }

    const Piece& piece = flatPieces.at(pieces.pieceOf.at(point));
    const Vec2 at = flat(restPositions[point]);
    std::vector<float> toCorner(piece.corners.size(), infinitelyFar);
    for (std::size_t corner = 0; corner < piece.corners.size(); ++corner) {
        if (sees(piece, at, piece.corners[corner])) {
            toCorner[corner] = (piece.corners[corner] - at).length();
        }
    }
    for (std::size_t local = 0; local < piece.anchors.size(); ++local) {
        const Vec2 anchor = flat(restPositions[anchors[piece.anchors[local]]]);
        float shortest = sees(piece, at, anchor) ? (anchor - at).length() : infinitelyFar;
        for (std::size_t corner = 0; corner < piece.corners.size(); ++corner) {
            shortest = std::min(shortest, toCorner[corner] + piece.cornerDistances[local][corner]);
        }
        distances[piece.anchors[local]] = shortest;
    }
}

/*
* Every free point measures its distance to every anchor along the cloth and keeps the nearest few: done once when the
* cloth is made or pinned, never a tick. Anchors on other pieces, infinitely far, are never kept.
*/
std::vector<ClothTether> clothTethers(const std::vector<Vec3>& restPositions, const std::vector<float>& inverseMasses, const ClothPieces& pieces,
                                      JobSystem& jobs) {
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

    const ClothRestPaths paths(restPositions, pieces, anchors);
    std::vector<ClothTether> tethers(restPositions.size() * tethersPerPoint);
    jobs.parallelFor(restPositions.size(), measuredPointsPerJob, [&](std::size_t point) {
        if (inverseMasses[point] == 0.0f) {
            return;
        }

        std::vector<float> distances;
        paths.distances(static_cast<std::uint32_t>(point), distances);
        std::vector<std::pair<float, std::uint32_t>> byDistance;
        for (std::size_t index = 0; index < anchors.size(); ++index) {
            if (distances[index] < infinitelyFar) {
                byDistance.push_back({distances[index], anchors[index]});
            }
        }
        const std::size_t kept = std::min(tethersPerPoint, byDistance.size());
        std::partial_sort(byDistance.begin(), byDistance.begin() + static_cast<std::ptrdiff_t>(kept), byDistance.end());
        for (std::size_t rank = 0; rank < kept; ++rank) {
            tethers[point * tethersPerPoint + rank] = ClothTether{byDistance[rank].second, byDistance[rank].first};
        }
    });
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
