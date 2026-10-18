#pragma once

#include "ecs/world.h"

#include <cstdint>

/*
* One number summing up the simulation's state, for checking determinism: the same build given the same commands must
* reach the same state bit for bit, so two runs (or a client and a server, later) agree exactly when their hashes do.
*
* It covers every component the fixed tick changes, field by field and in each storage's packed order (which a
* deterministic run reproduces too), with each entity's handle: Transform, PreviousTransform, CharacterController,
* CharacterMovement, FirstPersonBody, Particle and Grenade. Settings, looks and colliders are not hashed: ticks do not
* change them. A new component that ticks change must be added here.
*/
std::uint64_t hashSimulationState(const World& world);
