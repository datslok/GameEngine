#include "math/angles.h"

#include <cmath>

/*
* remainder() subtracts the nearest whole number of turns, so the result lands in -pi..pi, unlike fmod, whose result
* keeps the sign of its input and can be almost a full turn away.
*/
float wrapAngle(float radians) {
    return std::remainder(radians, fullTurnRadians);
}
