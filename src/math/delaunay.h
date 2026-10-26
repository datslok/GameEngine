#pragma once

#include "math/vec2.h"

#include <array>
#include <cstdint>
#include <vector>

/*
* The Delaunay triangulation of points in a plane: of every way to join them into triangles covering their convex hull,
* the one where no point lies inside any triangle's circumcircle, which also makes the smallest angle as large as it can
* be (Lawson 1977), so it avoids thin slivers whenever the points allow.
*
* Built by Bowyer-Watson (Bowyer 1981, Watson 1981): the points go in one at a time, nearby ones one after another
* (along a Hilbert curve) so finding each one's triangle walks only a few steps; the triangles whose circumcircles hold the new point
* are removed, leaving a hole the point sees all of, and the hole is filled with a fan of triangles from the point.
*
* Points four to a circle (a square's corners, a regular lattice) make the circle test an exact or rounded tie. Ties
* count as outside, and the hole is trimmed until the new point sees each of its sides from inside, so rounding can
* only pick between equally good triangles, never overlap them.
*
* The triangles come out counterclockwise, as indices into points, in an order that depends on the points alone.
* Fewer than three points, or all on one line, give none. Throws std::invalid_argument for a point that is not finite
* or the same point twice.
*/
std::vector<std::array<std::uint32_t, 3>> delaunayTriangulation(const std::vector<Vec2>& points);
