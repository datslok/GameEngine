#include "gameplay/spinner.h"
#include "math/transform.h"

#include <cmath>
#include <numbers>

namespace {
    /*
    * Compute the angle from the total time instead of adding a step each tick, so rounding errors cannot build up.
    */
    float animatedAngle(float initialAngle, float speed, double seconds) {
        const double angle =
            static_cast<double>(initialAngle) +
            static_cast<double>(speed) * seconds;

        const double fullTurn = 2.0 * std::numbers::pi_v<double>;

        return static_cast<float>(std::fmod(angle, fullTurn));
    }
}

void updateSpinners(World& world, double simulationSeconds) {
    world.each<Spinner, Transform>([simulationSeconds](Entity, Spinner& spinner, Transform& transform) {
        // Leave axes with zero speed alone, so other code can still control them.
        if (spinner.speed.x != 0.0f) {
            transform.rotation.x = animatedAngle(spinner.initialRotation.x, spinner.speed.x, simulationSeconds);
        }

        if (spinner.speed.y != 0.0f) {
            transform.rotation.y = animatedAngle(spinner.initialRotation.y, spinner.speed.y, simulationSeconds);
        }

        if (spinner.speed.z != 0.0f) {
            transform.rotation.z = animatedAngle(spinner.initialRotation.z, spinner.speed.z, simulationSeconds);
        }
    });
}
