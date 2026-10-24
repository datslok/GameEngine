#include "engine/game.h"
#include "scene/deforming_mesh.h"
#include "scene/interpolation.h"

/*
* Saving previous transforms and shapes belongs to the engine, because interpolation is part of rendering. Doing it here means no game can forget it.
*/
void runFixedUpdate(Game& game, World& world, float tickSeconds, double simulationSeconds) {
    savePreviousTransforms(world);
    savePreviousShapes(world);
    game.onFixedUpdate(world, tickSeconds, simulationSeconds);
}
