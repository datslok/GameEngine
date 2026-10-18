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
        transform.rotation = Quat::fromEulerAngles(Vec3{
            animatedAngle(spinner.initialAngles.x, spinner.speed.x, simulationSeconds),
            animatedAngle(spinner.initialAngles.y, spinner.speed.y, simulationSeconds),
            animatedAngle(spinner.initialAngles.z, spinner.speed.z, simulationSeconds)
        });
    });
}
