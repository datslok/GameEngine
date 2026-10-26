#include "math/delaunay.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
    using Triangles = std::vector<std::array<std::uint32_t, 3>>;

    // Twice the signed area of a, b, c: positive when they turn counterclockwise.
    double doubledArea(const Vec2& a, const Vec2& b, const Vec2& c) {
        return (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) - (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x);
    }

    // The convex hull's area (Andrew's monotone chain, shoelace).
    double hullArea(std::vector<Vec2> points) {
        std::sort(points.begin(), points.end(), [](const Vec2& a, const Vec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
        std::vector<Vec2> hull;
        for (int pass = 0; pass < 2; ++pass) {
            const std::size_t start = hull.size();
            for (const Vec2& point : points) {
                while (hull.size() >= start + 2 && doubledArea(hull[hull.size() - 2], hull.back(), point) <= 0.0) {
                    hull.pop_back();
                }
                hull.push_back(point);
            }
            hull.pop_back();
            std::reverse(points.begin(), points.end());
        }
        double doubled = 0.0;
        for (std::size_t index = 0; index < hull.size(); ++index) {
            const Vec2& a = hull[index];
            const Vec2& b = hull[(index + 1) % hull.size()];
            doubled += static_cast<double>(a.x) * b.y - static_cast<double>(b.x) * a.y;
        }
        return 0.5 * doubled;
    }

    /*
    * A valid triangulation of the points' hull: every triangle counterclockwise with area, each directed edge in one
    * triangle at most (no overlaps, consistent turning), and the areas adding up to the hull's (no holes).
    */
    void assertTriangulatesHull(const std::vector<Vec2>& points, const Triangles& triangles) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> directedEdges;
        double area = 0.0;
        for (const auto& triangle : triangles) {
            for (const std::uint32_t corner : triangle) {
                assert(corner < points.size());
            }
            const double doubled = doubledArea(points[triangle[0]], points[triangle[1]], points[triangle[2]]);
            assert(doubled > 0.0);
            area += 0.5 * doubled;
            for (int side = 0; side < 3; ++side) {
                const int uses = ++directedEdges[std::make_pair(triangle[side], triangle[(side + 1) % 3])];
                assert(uses == 1);
            }
        }
        const double hull = hullArea(points);
        assert(std::abs(area - hull) <= 1e-5 * hull);
    }

    // Twice the largest amount by which any other point lies inside a triangle's circumcircle, as a share of its radius.
    double worstCircleIntrusion(const std::vector<Vec2>& points, const Triangles& triangles) {
        double worst = 0.0;
        for (const auto& triangle : triangles) {
            const double ax = points[triangle[0]].x, ay = points[triangle[0]].y;
            const double bx = points[triangle[1]].x, by = points[triangle[1]].y;
            const double cx = points[triangle[2]].x, cy = points[triangle[2]].y;
            const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
            const double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d;
            const double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
            const double radius = std::hypot(ax - ux, ay - uy);
            for (std::uint32_t index = 0; index < points.size(); ++index) {
                if (index == triangle[0] || index == triangle[1] || index == triangle[2]) {
                    continue;
                }
                const double inside = radius - std::hypot(points[index].x - ux, points[index].y - uy);
                worst = std::max(worst, inside / radius);
            }
        }
        return worst;
    }

    // The smallest corner angle of any triangle, in degrees.
    double smallestAngle(const std::vector<Vec2>& points, const Triangles& triangles) {
        double smallest = 180.0;
        for (const auto& triangle : triangles) {
            for (int corner = 0; corner < 3; ++corner) {
                const Vec2 at = points[triangle[corner]];
                const Vec2 toNext = points[triangle[(corner + 1) % 3]] - at;
                const Vec2 toLast = points[triangle[(corner + 2) % 3]] - at;
                const double cosine = toNext.dot(toLast) / (static_cast<double>(toNext.length()) * toLast.length());
                smallest = std::min(smallest, std::acos(std::clamp(cosine, -1.0, 1.0)) * 180.0 / 3.14159265358979323846);
            }
        }
        return smallest;
    }

    // A small repeatable generator, so the same points are tested every run.
    class Numbers {
    public:
        float next() {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<float>(state >> 40) / static_cast<float>(1ull << 24);
        }

    private:
        std::uint64_t state = 2024;
    };

    /*
    * A square's four corners lie on one circle, so either diagonal is Delaunay: the exact tie must still give two
    * triangles covering the square, never overlapping ones.
    */
    void testSquareOfFourPoints() {
        const std::vector<Vec2> points{Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{0.0f, 1.0f}};
        const Triangles triangles = delaunayTriangulation(points);
        assert(triangles.size() == 2);
        assertTriangulatesHull(points, triangles);
    }

    // A grid of whole numbers is all exact ties: each square split in two, every square's four sides kept.
    void testGridOfTies() {
        std::vector<Vec2> points;
        for (int row = 0; row <= 6; ++row) {
            for (int column = 0; column <= 8; ++column) {
                points.push_back(Vec2{static_cast<float>(column), static_cast<float>(row)});
            }
        }
        const Triangles triangles = delaunayTriangulation(points);
        assert(triangles.size() == 2 * 6 * 8);
        assertTriangulatesHull(points, triangles);
        assert(worstCircleIntrusion(points, triangles) <= 1e-9);
    }

    /*
    * Scattered points: no point inside any triangle's circumcircle (the Delaunay property), and Euler's count for a
    * triangulated hull, 2n - 2 - h triangles with h points on the hull.
    */
    void testScatteredPointsHaveEmptyCircles() {
        Numbers numbers;
        std::vector<Vec2> points;
        for (int index = 0; index < 400; ++index) {
            points.push_back(Vec2{numbers.next() * 3.0f - 1.0f, numbers.next() * 2.0f + 5.0f});
        }
        const Triangles triangles = delaunayTriangulation(points);
        assertTriangulatesHull(points, triangles);
        assert(worstCircleIntrusion(points, triangles) <= 1e-6);

        std::map<std::pair<std::uint32_t, std::uint32_t>, int> edgeUses;
        for (const auto& triangle : triangles) {
            for (int side = 0; side < 3; ++side) {
                ++edgeUses[std::make_pair(std::min(triangle[side], triangle[(side + 1) % 3]), std::max(triangle[side], triangle[(side + 1) % 3]))];
            }
        }
        std::size_t hullEdges = 0;
        for (const auto& [edge, uses] : edgeUses) {
            hullEdges += uses == 1 ? 1 : 0;
        }
        assert(triangles.size() == 2 * points.size() - 2 - hullEdges);
    }

    /*
    * The cloth's lattice: equilateral triangles, every six neighbours of a point on one circle with the point's
    * neighbours, so nearly every test is a tie lost in rounding. It must still come out whole, its triangles the
    * lattice's own (60 degrees), only the hull's zig-zag sides' halves of them down to 30.
    */
    void testEquilateralLattice() {
        const float spacing = 0.01f;
        const float rowHeight = spacing * 0.8660254f;
        std::vector<Vec2> points;
        for (int row = 0; row < 40; ++row) {
            const float offset = row % 2 == 0 ? 0.0f : 0.5f * spacing;
            for (int column = 0; column < 50; ++column) {
                points.push_back(Vec2{0.3f + offset + spacing * static_cast<float>(column), -0.2f + rowHeight * static_cast<float>(row)});
            }
        }
        const Triangles triangles = delaunayTriangulation(points);
        assertTriangulatesHull(points, triangles);
        assert(smallestAngle(points, triangles) > 29.9);
        assert(worstCircleIntrusion(points, triangles) <= 1e-4);
    }

    // Too few points, or all on one line, span no triangle; the same point twice is an error.
    void testDegenerateInputs() {
        assert(delaunayTriangulation({}).empty());
        assert(delaunayTriangulation({Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}}).empty());
        assert(delaunayTriangulation({Vec2{0.0f, 0.0f}, Vec2{1.0f, 1.0f}, Vec2{2.0f, 2.0f}, Vec2{3.0f, 3.0f}}).empty());

        bool threw = false;
        try {
            delaunayTriangulation({Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}, Vec2{1.0f, 0.0f}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // The triangles are a function of the points alone: the same points give the same triangles in the same order.
    void testRepeatable() {
        Numbers numbers;
        std::vector<Vec2> points;
        for (int index = 0; index < 200; ++index) {
            points.push_back(Vec2{numbers.next(), numbers.next()});
        }
        assert(delaunayTriangulation(points) == delaunayTriangulation(points));
    }
}

void testDelaunay() {
    testSquareOfFourPoints();
    testGridOfTies();
    testScatteredPointsHaveEmptyCircles();
    testEquilateralLattice();
    testDegenerateInputs();
    testRepeatable();
}
