#include "cloth/cloth_particles.h"

#include <cmath>
#include <stdexcept>

std::size_t ClothParticles::size() const {
    return positions.size();
}

ClothParticles makeClothParticles(const Mesh& mesh, float areaDensity) {
    if (!std::isfinite(areaDensity) || areaDensity <= 0.0f) {
        throw std::invalid_argument("A fabric's mass per area must be finite and positive");
    }

    ClothParticles particles;
    for (const Vec4& vertex : mesh.vertices) {
        particles.positions.push_back(Vec3{vertex.x, vertex.y, vertex.z});
    }
    particles.previousPositions = particles.positions;
    particles.velocities.assign(particles.size(), Vec3{0.0f, 0.0f, 0.0f});

    std::vector<float> masses(particles.size(), 0.0f);
    for (const Triangle& triangle : mesh.triangles) {
        const std::size_t corners[3]{triangle.first, triangle.second, triangle.third};
        for (std::size_t corner : corners) {
            if (corner >= particles.size()) {
                throw std::invalid_argument("A cloth triangle names a vertex the mesh does not have");
            }
        }

        const Vec3 edge1 = particles.positions[corners[1]] - particles.positions[corners[0]];
        const Vec3 edge2 = particles.positions[corners[2]] - particles.positions[corners[0]];
        const float thirdOfMass = areaDensity * 0.5f * edge1.cross(edge2).length() / 3.0f;
        for (std::size_t corner : corners) {
            masses[corner] += thirdOfMass;
        }
    }

    particles.inverseMasses.reserve(particles.size());
    for (float mass : masses) {
        if (!(mass > 0.0f)) {
            throw std::invalid_argument("Every cloth point must belong to a triangle with area, or it has no mass");
        }
        particles.inverseMasses.push_back(1.0f / mass);
    }
    return particles;
}

void pinParticle(ClothParticles& particles, std::size_t index) {
    particles.inverseMasses.at(index) = 0.0f;
    particles.velocities.at(index) = Vec3{0.0f, 0.0f, 0.0f};
}

void predictCloth(ClothParticles& particles, const Vec3& acceleration, float seconds) {
    const Vec3 gain = acceleration * seconds;
    for (std::size_t index = 0; index < particles.size(); ++index) {
        particles.previousPositions[index] = particles.positions[index];
        if (particles.inverseMasses[index] > 0.0f) {
            particles.velocities[index] = particles.velocities[index] + gain;
            particles.positions[index] = particles.positions[index] + particles.velocities[index] * seconds;
        }
    }
}

void finishClothStep(ClothParticles& particles, float seconds) {
    const float perSecond = 1.0f / seconds;
    for (std::size_t index = 0; index < particles.size(); ++index) {
        particles.velocities[index] = (particles.positions[index] - particles.previousPositions[index]) * perSecond;
    }
}
