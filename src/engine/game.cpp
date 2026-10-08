#include "engine/game.h"
#include "scene/interpolation.h"

/*
* Saving previous transforms belongs to the engine, because interpolation is part of rendering. Doing it here means no game can forget it.
*/
void runFixedUpdate(Game& game, World& world, float tickSeconds, double simulationSeconds) {
    savePreviousTransforms(world);
    game.onFixedUpdate(world, tickSeconds, simulationSeconds);
}
