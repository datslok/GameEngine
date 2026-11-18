#pragma once

#include "math/vec3.h"
#include "scene/mesh.h"

#include <cstdint>
#include <vector>

/*
* The valley's structures in their stealth-alien look (ROADMAP phase 18, docs/research/STRUCTURES.md): the Monolith as
* five faceted slabs with a strut to its pillar, the Ring as segments round a polygon, the far spires. Every shape is
* lofted from sections and shaded flat; each triangle carries two random numbers in its texture coordinate, read by
* materials with facet variation (Material::facetTone, facetRoughness) as its own tone and roughness.
*/

// How far a slab's shoulder rises while it steps in (metres): short, so the step's facets face the sky.
inline constexpr float slabShoulderRise = 3.0f;

// A level of a slab: its height above the foot (metres; the first is below, sunk), its width and depth as shares of
// the slab's, how far its middle stands to the side (metres along x), and how far its front ridge stands off the
// slab's (a share of the width), so the ridge zigzags and the faces break into facets turned their own ways.
struct SlabLevel {
    float height = 0.0f;
    float widthShare = 1.0f;
    float depthShare = 1.0f;
    float offset = 0.0f;
    float ridgeShift = 0.0f;
};

/*
* A slab: its foot (the middle of its footprint, at the ground's level), its width (x) and depth (z) there, its height,
* where its front ridge stands across the width (-1..1; the back is the front turned half round), its levels bottom
* to top and the seed of its facets' numbers. Each level's cross-section is a hexagon: the left edge, the front ridge, a
* knee between the ridge and the right edge, the right edge, and the same turned half round, so each side has three
* facets a band.
*/
struct SlabShape {
    Vec3 foot;
    float width = 0.0f;
    float depth = 0.0f;
    float height = 0.0f;
    float ridgeShare = 0.0f;
    std::vector<SlabLevel> levels;
    std::uint32_t seed = 0;
};

/*
* A slab of the Monolith's kind: sunk below its foot, nearly upright up to shoulderShare of its height, there stepping in
* a quarter over slabShoulderRise, then rising to a chisel top cut towards +x (cut 1) or -x (cut -1), its front ridge
* zigzagging between levels. Its depth is slabDepthShare of its width.
*/
SlabShape makeSlab(Vec3 foot, float width, float height, float cut, float shoulderShare, float ridgeShare, std::uint32_t seed);

// The slab's faceted surface in its own frame (the origin at its foot): its levels lofted, capped below and on top.
Mesh slabMesh(const SlabShape& slab);

// The Monolith: five slabs across the clearing's north end, the tallest 600 m with its front ridge on forestMonolithPlace.
std::vector<SlabShape> monolithSlabs(float groundLevel);

// The far spires: needles 300-800 m tall standing 2.5-7 km away in the haze, each on the ground under it.
std::vector<SlabShape> farSpires();

// A straight strut of rhombus section, thickness across, from one point to another, with its facets' seed.
struct StrutShape {
    Vec3 from;
    Vec3 to;
    float thickness = 0.0f;
    std::uint32_t seed = 0;
};

// The strut from the Monolith's pillar (monolithPillar) into the side of a slab, where the bridge was.
StrutShape monolithStrut(float groundLevel);

// The strut's faceted surface in world coordinates.
Mesh strutMesh(const StrutShape& strut);

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
