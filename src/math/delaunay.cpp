#include "math/delaunay.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace {
    constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

    // Coordinates in doubles, scaled into a unit square round the origin, so the tests below keep their precision.
    struct Point {
        double x = 0.0;
        double y = 0.0;
    };

    // Twice the signed area of a, b, c: positive when they turn counterclockwise.
    double orientation(const Point& a, const Point& b, const Point& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    // Positive when d lies inside the circle through the counterclockwise a, b, c (each measured from d).
    double inCircle(const Point& a, const Point& b, const Point& c, const Point& d) {
        const double adx = a.x - d.x;
        const double ady = a.y - d.y;
        const double bdx = b.x - d.x;
        const double bdy = b.y - d.y;
        const double cdx = c.x - d.x;
        const double cdy = c.y - d.y;
        return (adx * adx + ady * ady) * (bdx * cdy - cdx * bdy) + (bdx * bdx + bdy * bdy) * (cdx * ady - adx * cdy) +
               (cdx * cdx + cdy * cdy) * (adx * bdy - bdx * ady);
    }

    // Where a cell of a 2^16 x 2^16 grid comes along a Hilbert curve, which visits neighbouring cells one after another.
    std::uint64_t hilbertIndex(std::uint32_t x, std::uint32_t y) {
        std::uint64_t index = 0;
        for (std::uint32_t half = 1u << 15; half > 0; half >>= 1) {
            const std::uint32_t right = (x & half) != 0 ? 1u : 0u;
            const std::uint32_t up = (y & half) != 0 ? 1u : 0u;
            index += static_cast<std::uint64_t>(half) * half * ((3u * right) ^ up);
            if (up == 0) {
                if (right == 1) {
                    x = half - 1 - (x & (half - 1));
                    y = half - 1 - (y & (half - 1));
                }
                std::swap(x, y);
            }
        }
        return index;
    }

    /*
    * The triangulation while it is built: each face's corners counterclockwise and, for each corner, the face across
    * the side facing it (none on the outside). The first three points are a triangle far round all the others, so
    * every new point falls inside some face; the faces touching it are dropped at the end.
    */
    class Triangulation {
    public:
        explicit Triangulation(std::vector<Point> points):
            points(std::move(points)),
            marks()
        {
            faces.push_back(Face{{0, 1, 2}, {none, none, none}});
            marks.push_back(0);
        }

        void insert(std::uint32_t point) {
            const std::uint32_t start = locate(point);
            ++stamp;
            std::vector<std::uint32_t> cavity = growCavity(start, point);
            trimCavity(cavity, start, point);
            fill(cavity, point);
        }

        std::vector<std::array<std::uint32_t, 3>> realTriangles(std::uint32_t superCorners) const {
            std::vector<std::array<std::uint32_t, 3>> triangles;
            for (const Face& face : faces) {
                if (face.corners[0] == none) {
                    continue;
                }
                if (face.corners[0] < superCorners || face.corners[1] < superCorners || face.corners[2] < superCorners) {
                    continue;
                }
                triangles.push_back({face.corners[0] - superCorners, face.corners[1] - superCorners, face.corners[2] - superCorners});
            }
            return triangles;
        }

    private:
        struct Face {
            std::array<std::uint32_t, 3> corners;
            std::array<std::uint32_t, 3> across;
        };

        // The two corners of a face's side facing corner, in the face's counterclockwise order.
        std::pair<std::uint32_t, std::uint32_t> side(const Face& face, int corner) const {
            return {face.corners[(corner + 1) % 3], face.corners[(corner + 2) % 3]};
        }

        double sideOrientation(const Face& face, int corner, std::uint32_t point) const {
            const auto [from, to] = side(face, corner);
            return orientation(points[from], points[to], points[point]);
        }

        bool circleHolds(const Face& face, std::uint32_t point) const {
            return inCircle(points[face.corners[0]], points[face.corners[1]], points[face.corners[2]], points[point]) > 0.0;
        }

        /*
        * The face holding the point, by walking from the last face made towards it, across whichever side the point is
        * beyond. Walks over Delaunay triangles always arrive; if rounding ever made one circle, every face is tried.
        */
        std::uint32_t locate(std::uint32_t point) const {
            std::uint32_t face = lastFace;
            for (std::size_t steps = 0; steps <= faces.size(); ++steps) {
                bool moved = false;
                for (int corner = 0; corner < 3; ++corner) {
                    if (sideOrientation(faces[face], corner, point) < 0.0 && faces[face].across[corner] != none) {
                        face = faces[face].across[corner];
                        moved = true;
                        break;
                    }
                }
                if (!moved) {
                    return face;
                }
            }
            for (std::uint32_t candidate = 0; candidate < faces.size(); ++candidate) {
                if (faces[candidate].corners[0] != none && sideOrientation(faces[candidate], 0, point) >= 0.0 &&
                    sideOrientation(faces[candidate], 1, point) >= 0.0 && sideOrientation(faces[candidate], 2, point) >= 0.0) {
                    return candidate;
                }
            }
            throw std::logic_error("A point fell outside the triangulation");
        }

        /*
        * The faces whose circumcircles hold the point, found outwards from its face: they form one connected region.
        * A face the point lies on the side of joins too, whatever its circle says, or that side would get a flat triangle.
        */
        std::vector<std::uint32_t> growCavity(std::uint32_t start, std::uint32_t point) {
            std::vector<std::uint32_t> cavity{start};
            marks[start] = stamp;
            for (int corner = 0; corner < 3; ++corner) {
                const std::uint32_t neighbour = faces[start].across[corner];
                if (neighbour != none && sideOrientation(faces[start], corner, point) == 0.0) {
                    marks[neighbour] = stamp;
                    cavity.push_back(neighbour);
                }
            }
            for (std::size_t next = 0; next < cavity.size(); ++next) {
                for (const std::uint32_t neighbour : faces[cavity[next]].across) {
                    if (neighbour != none && marks[neighbour] != stamp && circleHolds(faces[neighbour], point)) {
                        marks[neighbour] = stamp;
                        cavity.push_back(neighbour);
                    }
                }
            }
            return cavity;
        }

        bool inCavity(std::uint32_t face) const {
            return face != none && marks[face] == stamp;
        }

        /*
        * The hole must be seen whole from the point: each side of it with the point on its line or beyond would make a
        * flat or backwards triangle. Rounding in near-ties can leave such sides; the face behind each is put back, and
        * whatever that cuts off from the point's face too, until none is left.
        */
        void trimCavity(std::vector<std::uint32_t>& cavity, std::uint32_t start, std::uint32_t point) {
            bool trimmed = true;
            while (trimmed) {
                trimmed = false;
                for (const std::uint32_t face : cavity) {
                    if (face == start || !inCavity(face)) {
                        continue;
                    }
                    for (int corner = 0; corner < 3; ++corner) {
                        if (!inCavity(faces[face].across[corner]) && sideOrientation(faces[face], corner, point) <= 0.0) {
                            marks[face] = 0;
                            trimmed = true;
                            break;
                        }
                    }
                }
                if (!trimmed) {
                    break;
                }

                // Keep only what still joins the point's face.
                const std::uint32_t kept = stamp;
                ++stamp;
                std::vector<std::uint32_t> joined{start};
                marks[start] = stamp;
                for (std::size_t next = 0; next < joined.size(); ++next) {
                    for (const std::uint32_t neighbour : faces[joined[next]].across) {
                        if (neighbour != none && marks[neighbour] == kept) {
                            marks[neighbour] = stamp;
                            joined.push_back(neighbour);
                        }
                    }
                }
                cavity = std::move(joined);
            }
        }

        std::uint32_t newFace(const Face& face) {
            if (!freeFaces.empty()) {
                const std::uint32_t index = freeFaces.back();
                freeFaces.pop_back();
                faces[index] = face;
                return index;
            }
            faces.push_back(face);
            marks.push_back(0);
            return static_cast<std::uint32_t>(faces.size() - 1);
        }

        /*
        * The hole's sides, each with the face outside it, become triangles with the point; side to side they share the
        * point's edges, found by where each starts and ends.
        */
        void fill(const std::vector<std::uint32_t>& cavity, std::uint32_t point) {
            struct Rim {
                std::uint32_t from;
                std::uint32_t to;
                std::uint32_t outside;
            };
            std::vector<Rim> rim;
            for (const std::uint32_t face : cavity) {
                for (int corner = 0; corner < 3; ++corner) {
                    const std::uint32_t neighbour = faces[face].across[corner];
                    if (!inCavity(neighbour)) {
                        const auto [from, to] = side(faces[face], corner);
                        rim.push_back(Rim{from, to, neighbour});
                    }
                }
            }
            for (const std::uint32_t face : cavity) {
                faces[face].corners = {none, none, none};
                freeFaces.push_back(face);
            }

            std::vector<std::uint32_t> made;
            made.reserve(rim.size());
            for (const Rim& edge : rim) {
                const std::uint32_t face = newFace(Face{{edge.from, edge.to, point}, {none, none, edge.outside}});
                made.push_back(face);
                if (edge.outside != none) {
                    Face& outside = faces[edge.outside];
                    for (int corner = 0; corner < 3; ++corner) {
                        const auto [from, to] = side(outside, corner);
                        if (from == edge.to && to == edge.from) {
                            outside.across[corner] = face;
                        }
                    }
                }
            }

            // Face (a, b, p) meets (b, c, p) along b-p: the first's side facing a, the second's facing c.
            for (std::size_t index = 0; index < rim.size(); ++index) {
                for (std::size_t other = 0; other < rim.size(); ++other) {
                    if (rim[other].from == rim[index].to) {
                        faces[made[index]].across[0] = made[other];
                        faces[made[other]].across[1] = made[index];
                    }
                }
            }
            lastFace = made.front();
        }

        std::vector<Point> points;
        std::vector<Face> faces;
        std::vector<std::uint32_t> freeFaces;

        // Which faces are in the hole being made: those marked with this insertion's stamp.
        std::vector<std::uint32_t> marks;
        std::uint32_t stamp = 0;
        std::uint32_t lastFace = 0;
    };
}

/*
* The points are scaled into a unit square round the origin, the far triangle's corners a thousand times further out
* (so its circles near the points are almost straight lines and it changes no triangle between them), and put in along
* a Hilbert curve over their box, ties by index.
*/
std::vector<std::array<std::uint32_t, 3>> delaunayTriangulation(const std::vector<Vec2>& points) {
    for (const Vec2& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            throw std::invalid_argument("Delaunay triangulation needs finite points");
        }
    }

    std::vector<std::uint32_t> order(points.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return points[a].x < points[b].x || (points[a].x == points[b].x && (points[a].y < points[b].y || (points[a].y == points[b].y && a < b)));
    });
    for (std::size_t index = 1; index < order.size(); ++index) {
        if (points[order[index]].x == points[order[index - 1]].x && points[order[index]].y == points[order[index - 1]].y) {
            throw std::invalid_argument("Delaunay triangulation was given the same point twice");
        }
    }
    if (points.size() < 3) {
        return {};
    }

    double lowX = points.front().x;
    double highX = lowX;
    double lowY = points.front().y;
    double highY = lowY;
    for (const Vec2& point : points) {
        lowX = std::min(lowX, static_cast<double>(point.x));
        highX = std::max(highX, static_cast<double>(point.x));
        lowY = std::min(lowY, static_cast<double>(point.y));
        highY = std::max(highY, static_cast<double>(point.y));
    }
    const double size = std::max(highX - lowX, highY - lowY);
    const double middleX = 0.5 * (lowX + highX);
    const double middleY = 0.5 * (lowY + highY);

    constexpr std::uint32_t superCorners = 3;
    constexpr double far = 1000.0;
    std::vector<Point> scaled{Point{0.0, 2.0 * far}, Point{-std::sqrt(3.0) * far, -far}, Point{std::sqrt(3.0) * far, -far}};
    for (const Vec2& point : points) {
        scaled.push_back(Point{(point.x - middleX) / size, (point.y - middleY) / size});
    }

    std::vector<std::uint64_t> curve(points.size());
    for (std::size_t index = 0; index < points.size(); ++index) {
        const double cells = 65535.0;
        const auto cell = [&](double coordinate) { return static_cast<std::uint32_t>(std::clamp((coordinate + 0.5) * cells, 0.0, cells)); };
        curve[index] = hilbertIndex(cell(scaled[index + superCorners].x), cell(scaled[index + superCorners].y));
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return curve[a] < curve[b] || (curve[a] == curve[b] && a < b); });

    Triangulation triangulation(std::move(scaled));
    for (const std::uint32_t index : order) {
        triangulation.insert(index + superCorners);
    }
    return triangulation.realTriangles(superCorners);
}
