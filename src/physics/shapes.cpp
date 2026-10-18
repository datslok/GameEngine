#include "physics/shapes.h"

#include <algorithm>
#include <type_traits>

/*
* A sphere reaches its radius past its centre on every axis; a capsule is the box around both end balls; a box is itself.
*/
Aabb boundsOf(const Shape& shape) {
    return std::visit([](const auto& held) -> Aabb {
        using Held = std::decay_t<decltype(held)>;
        if constexpr (std::is_same_v<Held, Sphere>) {
            const Vec3 reach{held.radius, held.radius, held.radius};
            return Aabb{held.centre - reach, held.centre + reach};
        } else if constexpr (std::is_same_v<Held, Capsule>) {
            const Vec3 reach{held.radius, held.radius, held.radius};
            const Vec3 low{std::min(held.a.x, held.b.x), std::min(held.a.y, held.b.y), std::min(held.a.z, held.b.z)};
            const Vec3 high{std::max(held.a.x, held.b.x), std::max(held.a.y, held.b.y), std::max(held.a.z, held.b.z)};
            return Aabb{low - reach, high + reach};
        } else {
            return held;
        }
    }, shape);
}
