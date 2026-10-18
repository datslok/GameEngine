#pragma once

#include "math/mat4.h"
#include "physics/collision_settings.h"
#include "physics/query_stats.h"
#include "physics/raycast.h"
#include "physics/triangle_contact.h"
#include "physics/triangle_mesh.h"
#include "physics/uniform_grid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/*
* The parts of a level that never move (floors, walls, stairs), as one world-space triangle mesh with a uniform grid over
* its triangles, built as pieces are added and never rebuilt. Moving things live in a CollisionScene instead, which is
* rebuilt every tick; keeping the static triangles out of it means they are never re-inserted.
*
* The grid's cell size comes from the settings given at construction. To change the level, build a new one.
*/
class StaticLevel {
public:
    explicit StaticLevel(const CollisionSettings& settings = CollisionSettings{});

    // Adds a piece, moved into the world by model. Throws std::invalid_argument (leaving the level unchanged) if one of
    // its triangles refers to a corner it does not have.
    void add(const TriangleMesh& piece, const Mat4& model = Mat4::identity());

    std::size_t getTriangleCount() const;
    std::array<Vec3, 3> getTriangle(std::uint32_t triangle) const;
    const TriangleMesh& getMesh() const;
    const UniformGrid& getGrid() const;

private:
    TriangleMesh mesh;
    UniformGrid grid;
};

// Where a ray first meets the level, and which triangle it hit.
struct LevelHit {
    RayHit hit;
    std::uint32_t triangle = 0;
};

// The first triangle the ray meets within maxDistance, through the grid. stats, when given, receives what it cost.
std::optional<LevelHit> raycast(const StaticLevel& level, const Ray& ray, float maxDistance = unlimitedRayDistance,
                                CollisionQueryStats* stats = nullptr);

// Every triangle the capsule touches, one contact each, in no particular order.
std::vector<Contact> contacts(const StaticLevel& level, const Capsule& capsule);
