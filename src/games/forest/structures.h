#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/mesh.h"

#include <cstdint>
#include <vector>

/*
* The valley's structures' shape kit (ROADMAP phase 18, docs/research/STRUCTURES.md) and the Ring. Every shape is lofted
* from convex sections and shaded flat; each triangle carries two random numbers in its texture coordinate, read by
* materials with facet variation (Material::facetTone, facetRoughness) as its own tone and roughness. The Ring keeps the
* stealth-alien look, a number pair a triangle; the Bastion (games/forest/bastion) a pair a flat face, so its walls stay
* one tone each.
*/

// Which triangles share a pair of facet numbers: each its own, or every triangle of one flat face (a side quad, a cap).
enum class FacetNumbers {
    PerTriangle,
    PerFace
};

/*
* Sections lofted into a closed surface: each a convex polygon of the same number of points, which go round from -u
* through +v to +u and -v where u x v points back along the loft (as round a box seen from above, the loft rising: the
* polygon's winding normal, (b - a) x (c - b), points along the loft), so the sides between neighbouring sections face
* out; both ends capped by fans. The vertices are shared, the triangles flat. facet counts the number pairs taken from
* seed, so several lofts into one mesh keep drawing new ones.
*/
void appendLoft(Mesh& mesh, const std::vector<std::vector<Vec3>>& sections, std::uint32_t seed, std::uint64_t& facet, FacetNumbers numbers);

// A convex polygon pushed along offset into a closed prism, the polygon turned round first if its winding faces back.
void appendExtrusion(Mesh& mesh, const std::vector<Vec3>& polygon, const Vec3& offset, std::uint32_t seed, std::uint64_t& facet, FacetNumbers numbers);

// A section at a point of a loft: offsets (along -u .. +v .. +u .. -v) round a centre.
std::vector<Vec3> sectionAt(Vec3 centre, Vec3 u, Vec3 v, const std::vector<Vec2>& offsets);

// The Ring's segments round its polygon, and the triangles each is made of (eight sides, two caps of six).
inline constexpr int ringSegments = 20;
inline constexpr int ringSegmentTriangles = 28;

/*
* The Ring in its own frame: ringSegments straight segments round the origin in the xy plane (the ring faces along z),
* their middles radius from it, each a prism of chamfered square section halfThickness either side, its ends cut on the
* radial planes a gap short of the corners. The segments within 30 degrees of the top have drifted outwards a few
* metres and turned a degree.
*/
Mesh ringSegmentsMesh(float radius, float halfThickness, std::uint32_t seed);
