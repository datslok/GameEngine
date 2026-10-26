#pragma once

#include "core/job_system.h"
#include "math/aabb.h"
#include "math/vec3.h"
#include "physics/triangle_mesh.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

/*
* A body's shape as a grid of signed distances: at each sample, how far the nearest point of its surface is, positive
* outside and negative inside. Between samples the eight round a point are blended (trilinear interpolation), and the
* blend's slope points out of the body, so any shape answers what a sphere's formula does: how far, and which way out.
* Like a potential's level sets: the surface is the zero contour, and the slope is the gradient.
*
* Only a band round the surface holds real distances (what cloth touching a body needs); past it a sample holds the
* band, with its sign. Built once, from a closed triangle mesh already placed in the world; a still pose (a moving body
* would rebuild it, or carry one per bone).
*/
struct SignedDistanceField {
    Vec3 origin;                  // where sample (0, 0, 0) is
    float spacing = 0.0f;         // between samples, m
    std::array<int, 3> counts{};  // samples along x, y and z
    float band = 0.0f;            // the most distance a sample holds, m
    std::vector<float> distances; // x fastest, then y, then z

    float at(int x, int y, int z) const {
        return distances[(static_cast<std::size_t>(z) * static_cast<std::size_t>(counts[1]) + static_cast<std::size_t>(y)) *
                             static_cast<std::size_t>(counts[0]) +
                         static_cast<std::size_t>(x)];
    }

    // From the first sample to the last.
    Aabb bounds() const;
};

/*
* The field of a closed mesh (its triangles counterclockwise seen from outside, each edge shared by two of them through
* their corners' indices), its samples spacing apart over the mesh's box grown by band and one spacing, so the box's
* edge is outside and far. Each sample within band of the surface holds its exact distance, its sign from the nearest
* feature's normal (Baerentzen & Aanaes 2005: a face's own, an edge's the sum of its two faces', a corner's its faces'
* weighted by their angles there, which for a closed mesh always tells inside from outside); each further one holds
* band, outside if it is reached from the box's edge without crossing the band, inside otherwise.
*
* Each triangle writes its distance into the samples within band of it, keeping the nearest, rather than each sample
* searching the triangles: a triangle reaches only a few thousand samples. The grid is cut into slabs along y, a job
* each, every slab taking its triangles in order, so any number of threads gives the same field to the last bit.
* Throws std::invalid_argument for a spacing that is not positive, a band less than the spacing, or no triangles.
*/
SignedDistanceField buildSignedDistanceField(const TriangleMesh& mesh, float spacing, float band, JobSystem& jobs = serialJobs());

// The field at a point: its distance and the slope of the blend (not of unit length). Outside the field's box, band away with no slope.
struct FieldPoint {
    float distance = 0.0f;
    Vec3 gradient;
};

FieldPoint sampleField(const SignedDistanceField& field, const Vec3& point);

/*
* Component: a body cloth collides with, given by its field already placed in the world (the entity's Transform is not
* used). Shared, since a field is large (the mannequin's: about 6 million samples) and never changes. Not a Collider:
* the player, picking and the physics' own queries do not see it.
*/
struct DistanceFieldCollider {
    std::shared_ptr<const SignedDistanceField> field;
};
