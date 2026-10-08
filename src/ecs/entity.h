#pragma once

#include <cstdint>

/*
* An entity is only a name for a thing in the world. Its data lives in components stored by the World.
* The index picks a slot, and the generation counts how often that slot has been reused, so a handle to a destroyed entity never matches the new entity that later takes its slot.
* Generations start at 1, so a default Entity{} never refers to a live entity and works as "no entity".
*/
struct Entity {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    bool operator==(const Entity& other) const = default;
};
