#pragma once

#include "math/vec3.h"
#include "scene/mesh.h"

#include <cstddef>
#include <vector>

/*
* A cloth's points, one per vertex of its mesh, as plain arrays rather than one struct per point (structure of arrays):
* a loop that needs only positions then reads only positions, every byte of each cache line it fetches useful, and the
* compiler can work on several floats at once. Point i is the mesh's vertex i.
*
* Inverse masses rather than masses: the constraints (XPBD, next) move each point in proportion to its inverse mass, so
* a pinned point, inverse mass 0, is simply infinitely heavy and nothing moves it, with no special case anywhere.
*/
struct ClothParticles {
    std::vector<Vec3> positions;
    std::vector<Vec3> previousPositions; // where each point was when the step began
    std::vector<Vec3> velocities;
    std::vector<float> inverseMasses;    // 1 / kg; 0 for a pinned point

    std::size_t size() const;
};

/*
* The mesh's vertices as points at rest, with lumped masses: each point gets a third of the mass of every triangle it
* belongs to, the triangle's area times areaDensity (kg per square metre), so the masses add up to the fabric's. Throws
* std::invalid_argument for a density that is not finite and positive, or a point in no triangle with area (no mass).
*/
ClothParticles makeClothParticles(const Mesh& mesh, float areaDensity);

// Pin a point where it is: infinitely heavy and still. Throws std::out_of_range for a point the cloth does not have.
void pinParticle(ClothParticles& particles, std::size_t index);

/*
* The step's prediction, symplectic Euler: every free point's velocity gains acceleration x seconds, then it moves by
* its new velocity; each point's start is kept. Pinned points neither speed up nor move.
*/
void predictCloth(ClothParticles& particles, const Vec3& acceleration, float seconds);

/*
* The step's end: each point's velocity is its motion over the step divided by its length, so whatever moved it after
* the prediction (the constraints) shapes its velocity. With nothing in between it gives back exactly the predicted
* velocity, which is why the prediction moves by the new velocity: moving by v t + a t^2 / 2 (phase 5's fly) would
* give back v + a t / 2, and a sheet would fall at half of gravity.
*/
void finishClothStep(ClothParticles& particles, float seconds);
