#include "cloth/cloth_pattern.h"

#include "math/delaunay.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace {
    // How far apart an edge's end and the next edge's start may be and still be one corner, in metres.
    constexpr float cornerTolerance = 1e-5f;

    // Lattice points nearer the outline than this share of the spacing are left out, so none crowds an edge's points:
    // at half a spacing the border's triangles came down to 23 degrees on a rectangle, at 0.6-0.8 to 37.
    constexpr float latticeClearance = 0.6f;

    // A length a hair over a whole number of spacings still takes that many segments, not one more.
    constexpr float segmentRounding = 1e-4f;

    // The gap between pieces laid side by side at rest, in metres.
    constexpr float layoutGap = 0.1f;

    // The fewest segments that keep an edge's points no further apart than spacing.
    int segmentsFor(float length, float spacing) {
        return std::max(1, static_cast<int>(std::ceil(length / spacing - segmentRounding)));
    }

    // Twice the signed area of a closed polygon: positive counterclockwise.
    double doubledArea(const std::vector<Vec2>& outline) {
        double doubled = 0.0;
        for (std::size_t index = 0; index < outline.size(); ++index) {
            const Vec2& a = outline[index];
            const Vec2& b = outline[(index + 1) % outline.size()];
            doubled += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
        }
        return doubled;
    }

    // Whether a point is inside a closed polygon: an odd number of its sides cross the line to the point's right.
    bool insideOutline(const std::vector<Vec2>& outline, const Vec2& point) {
        bool inside = false;
        for (std::size_t index = 0, last = outline.size() - 1; index < outline.size(); last = index++) {
            const Vec2& a = outline[index];
            const Vec2& b = outline[last];
            if ((a.y > point.y) != (b.y > point.y) && point.x < a.x + (b.x - a.x) * (point.y - a.y) / (b.y - a.y)) {
                inside = !inside;
            }
        }
        return inside;
    }

    float distanceToSegment(const Vec2& point, const Vec2& a, const Vec2& b) {
        const Vec2 side = b - a;
        const float share = std::clamp((point - a).dot(side) / side.lengthSquared(), 0.0f, 1.0f);
        return (point - (a + side * share)).length();
    }

    float distanceToOutline(const std::vector<Vec2>& outline, const Vec2& point) {
        float nearest = std::numeric_limits<float>::max();
        for (std::size_t index = 0; index < outline.size(); ++index) {
            nearest = std::min(nearest, distanceToSegment(point, outline[index], outline[(index + 1) % outline.size()]));
        }
        return nearest;
    }

    std::uint64_t edgeKey(std::uint32_t from, std::uint32_t to) {
        return (static_cast<std::uint64_t>(from) << 32) | to;
    }

    // One piece cut into triangles, its points numbered from 0: the outline's first, edge by edge, then the lattice's.
    struct PieceCut {
        std::vector<Vec2> points;
        std::vector<std::array<std::uint32_t, 3>> triangles;
        std::vector<std::vector<std::uint32_t>> edgePoints;
    };

    void checkCorners(const PatternPiece& piece) {
        for (std::size_t index = 0; index < piece.edges.size(); ++index) {
            const PatternEdge& edge = piece.edges[index];
            if (edge.points.size() < 2) {
                throw std::invalid_argument("A pattern edge needs at least two points");
            }
            const Vec2& next = piece.edges[(index + 1) % piece.edges.size()].points.front();
            if ((edge.points.back() - next).length() > cornerTolerance) {
                throw std::invalid_argument("A pattern piece's outline must close: each edge ends where the next begins");
            }
            for (const Vec2& point : edge.points) {
                if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                    throw std::invalid_argument("A pattern piece's outline must be finite");
                }
            }
        }
    }

    /*
    * The outline's points edge by edge, each edge's last left to the next edge's first; the lattice's rows run along
    * x, half a spacing staggered every other row; then Delaunay, each outline segment checked, and the triangles whose
    * middles lie outside dropped.
    */
    PieceCut cutPiece(const PatternPiece& piece, const std::vector<int>& segments, float spacing) {
        PieceCut cut;
        for (std::size_t index = 0; index < piece.edges.size(); ++index) {
            const std::vector<Vec2> along = resampleEdge(piece.edges[index], segments[index]);
            std::vector<std::uint32_t> indices;
            for (std::size_t point = 0; point + 1 < along.size(); ++point) {
                indices.push_back(static_cast<std::uint32_t>(cut.points.size()));
                cut.points.push_back(along[point]);
            }
            cut.edgePoints.push_back(std::move(indices));
        }
        for (std::size_t index = 0; index < cut.edgePoints.size(); ++index) {
            cut.edgePoints[index].push_back(cut.edgePoints[(index + 1) % cut.edgePoints.size()].front());
        }
        const std::vector<Vec2> outline = cut.points;
        if (outline.size() < 3 || doubledArea(outline) <= 0.0) {
            throw std::invalid_argument("A pattern piece's outline must run counterclockwise round some area");
        }

        Vec2 low = outline.front();
        Vec2 high = outline.front();
        for (const Vec2& point : outline) {
            low = Vec2{std::min(low.x, point.x), std::min(low.y, point.y)};
            high = Vec2{std::max(high.x, point.x), std::max(high.y, point.y)};
        }
        const float rowHeight = spacing * 0.5f * std::sqrt(3.0f);
        const int firstRow = static_cast<int>(std::floor(low.y / rowHeight));
        const int lastRow = static_cast<int>(std::ceil(high.y / rowHeight));
        for (int row = firstRow; row <= lastRow; ++row) {
            const float stagger = (row % 2 + 2) % 2 == 1 ? 0.5f * spacing : 0.0f;
            const int firstColumn = static_cast<int>(std::floor((low.x - stagger) / spacing));
            const int lastColumn = static_cast<int>(std::ceil((high.x - stagger) / spacing));
            for (int column = firstColumn; column <= lastColumn; ++column) {
                const Vec2 point{stagger + spacing * static_cast<float>(column), rowHeight * static_cast<float>(row)};
                if (insideOutline(outline, point) && distanceToOutline(outline, point) >= latticeClearance * spacing) {
                    cut.points.push_back(point);
                }
            }
        }

        const std::vector<std::array<std::uint32_t, 3>> all = delaunayTriangulation(cut.points);
        std::unordered_set<std::uint64_t> sides;
        for (const std::array<std::uint32_t, 3>& triangle : all) {
            for (int side = 0; side < 3; ++side) {
                sides.insert(edgeKey(triangle[side], triangle[(side + 1) % 3]));
            }
        }
        for (std::uint32_t index = 0; index < outline.size(); ++index) {
            if (sides.count(edgeKey(index, (index + 1) % static_cast<std::uint32_t>(outline.size()))) == 0) {
                throw std::logic_error("A pattern piece's outline is too sharp for its spacing: an edge was lost");
            }
        }
        for (const std::array<std::uint32_t, 3>& triangle : all) {
            const Vec2 middle = (cut.points[triangle[0]] + cut.points[triangle[1]] + cut.points[triangle[2]]) / 3.0f;
            if (insideOutline(outline, middle)) {
                cut.triangles.push_back(triangle);
            }
        }
        return cut;
    }
}

float edgeLength(const PatternEdge& edge) {
    float length = 0.0f;
    for (std::size_t index = 0; index + 1 < edge.points.size(); ++index) {
        length += (edge.points[index + 1] - edge.points[index]).length();
    }
    return length;
}

std::vector<Vec2> resampleEdge(const PatternEdge& edge, int segments) {
    if (segments < 1 || edge.points.size() < 2) {
        throw std::invalid_argument("Resampling needs an edge with two points and at least one segment");
    }

    const float length = edgeLength(edge);
    std::vector<Vec2> points{edge.points.front()};
    std::size_t piece = 0;
    float pieceStart = 0.0f;
    for (int index = 1; index < segments; ++index) {
        const float wanted = length * static_cast<float>(index) / static_cast<float>(segments);
        float pieceLength = (edge.points[piece + 1] - edge.points[piece]).length();
        while (pieceStart + pieceLength < wanted && piece + 2 < edge.points.size()) {
            pieceStart += pieceLength;
            ++piece;
            pieceLength = (edge.points[piece + 1] - edge.points[piece]).length();
        }
        const float share = pieceLength > 0.0f ? std::clamp((wanted - pieceStart) / pieceLength, 0.0f, 1.0f) : 0.0f;
        points.push_back(edge.points[piece] + (edge.points[piece + 1] - edge.points[piece]) * share);
    }
    points.push_back(edge.points.back());
    return points;
}

/*
* Sewn edges first get their segments, the longer edge's count for both; then each piece is cut on its own and
* appended, moved right of the last, and the stitches are read off the edges' points.
*/
PatternMesh meshPattern(const Pattern& pattern, float spacing) {
    if (!std::isfinite(spacing) || spacing <= 0.0f) {
        throw std::invalid_argument("A pattern's spacing must be finite and positive");
    }

    std::vector<std::vector<int>> segments;
    for (const PatternPiece& piece : pattern.pieces) {
        checkCorners(piece);
        std::vector<int> counts;
        for (const PatternEdge& edge : piece.edges) {
            counts.push_back(segmentsFor(edgeLength(edge), spacing));
        }
        segments.push_back(std::move(counts));
    }

    std::vector<std::vector<bool>> sewn;
    for (const PatternPiece& piece : pattern.pieces) {
        sewn.emplace_back(piece.edges.size(), false);
    }
    for (const PatternSeam& seam : pattern.seams) {
        if (seam.firstPiece >= pattern.pieces.size() || seam.secondPiece >= pattern.pieces.size() ||
            seam.firstEdge >= pattern.pieces[seam.firstPiece].edges.size() || seam.secondEdge >= pattern.pieces[seam.secondPiece].edges.size()) {
            throw std::invalid_argument("A seam names an edge the pattern does not have");
        }
        if (sewn[seam.firstPiece][seam.firstEdge] || sewn[seam.secondPiece][seam.secondEdge] ||
            (seam.firstPiece == seam.secondPiece && seam.firstEdge == seam.secondEdge)) {
            throw std::invalid_argument("A pattern edge can be sewn once");
        }
        sewn[seam.firstPiece][seam.firstEdge] = true;
        sewn[seam.secondPiece][seam.secondEdge] = true;
        const int count = std::max(segments[seam.firstPiece][seam.firstEdge], segments[seam.secondPiece][seam.secondEdge]);
        segments[seam.firstPiece][seam.firstEdge] = count;
        segments[seam.secondPiece][seam.secondEdge] = count;
    }

    PatternMesh meshed;
    float layoutRight = 0.0f;
    for (std::uint32_t pieceIndex = 0; pieceIndex < pattern.pieces.size(); ++pieceIndex) {
        const PieceCut cut = cutPiece(pattern.pieces[pieceIndex], segments[pieceIndex], spacing);
        const std::uint32_t first = static_cast<std::uint32_t>(meshed.flatPositions.size());

        float left = cut.points.front().x;
        float right = left;
        for (const Vec2& point : cut.points) {
            left = std::min(left, point.x);
            right = std::max(right, point.x);
        }
        const float shift = pieceIndex == 0 ? 0.0f : layoutRight + layoutGap - left;
        layoutRight = right + shift;

        for (const Vec2& point : cut.points) {
            meshed.flatPositions.push_back(point);
            meshed.pieces.push_back(pieceIndex);
            meshed.mesh.vertices.push_back(Vec4{point.x + shift, point.y, 0.0f, 1.0f});
        }
        for (const std::array<std::uint32_t, 3>& triangle : cut.triangles) {
            Triangle placed{first + triangle[0], first + triangle[1], first + triangle[2]};
            placed.uvs = {cut.points[triangle[0]], cut.points[triangle[1]], cut.points[triangle[2]]};
            meshed.mesh.triangles.push_back(placed);
        }
        std::vector<std::vector<std::uint32_t>> edges = cut.edgePoints;
        for (std::vector<std::uint32_t>& edge : edges) {
            for (std::uint32_t& point : edge) {
                point += first;
            }
        }
        meshed.edgePoints.push_back(std::move(edges));
    }

    std::vector<bool> stitched(meshed.flatPositions.size(), false);
    for (const PatternSeam& seam : pattern.seams) {
        const std::vector<std::uint32_t>& firstEdge = meshed.edgePoints[seam.firstPiece][seam.firstEdge];
        const std::vector<std::uint32_t>& secondEdge = meshed.edgePoints[seam.secondPiece][seam.secondEdge];
        std::vector<ClothStitch> stitches;
        for (std::size_t index = 0; index < firstEdge.size(); ++index) {
            const ClothStitch stitch{firstEdge[index], secondEdge[secondEdge.size() - 1 - index]};
            if (stitched[stitch.first] || stitched[stitch.second] || stitch.first == stitch.second) {
                throw std::invalid_argument("A pattern point would be in two stitches: two sewn edges meet at a corner");
            }
            stitched[stitch.first] = true;
            stitched[stitch.second] = true;
            stitches.push_back(stitch);
        }
        meshed.seams.push_back(std::move(stitches));
    }
    return meshed;
}

ClothSeams sewnSeams(const PatternMesh& meshed) {
    std::vector<Vec3> rest;
    rest.reserve(meshed.mesh.vertices.size());
    for (const Vec4& vertex : meshed.mesh.vertices) {
        rest.push_back(Vec3{vertex.x, vertex.y, vertex.z});
    }
    std::vector<std::array<std::uint32_t, 3>> triangles;
    triangles.reserve(meshed.mesh.triangles.size());
    for (const Triangle& triangle : meshed.mesh.triangles) {
        triangles.push_back({static_cast<std::uint32_t>(triangle.first), static_cast<std::uint32_t>(triangle.second), static_cast<std::uint32_t>(triangle.third)});
    }

    ClothSeams seams;
    for (const std::vector<ClothStitch>& seam : meshed.seams) {
        seams.stitches.insert(seams.stitches.end(), seam.begin(), seam.end());
        const std::vector<BendConstraint> hinges = seamHinges(rest, triangles, seam);
        seams.hinges.insert(seams.hinges.end(), hinges.begin(), hinges.end());
    }
    return seams;
}

Vec3 wrapOnCylinder(const Vec2& flat, const Vec3& axisFoot, float radius, float facing) {
    const float angle = facing + flat.x / radius;
    return Vec3{axisFoot.x + radius * std::sin(angle), axisFoot.y + flat.y, axisFoot.z + radius * std::cos(angle)};
}
