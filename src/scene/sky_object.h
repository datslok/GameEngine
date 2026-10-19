#pragma once

#include "math/transform.h"
#include "math/vec3.h"

/*
* Things in the sky so far away that they show no parallax, like the sun: walk a kilometre and it is still in the same
* direction and the same size. Drawn as an object kept at a fixed offset from the camera every frame (the trick
* skyboxes use), inside the far plane, so its direction and size never change wherever the camera goes.
*/

// The brightest a sky object may glow. The real sun is about 46,000 times brighter than a white surface it lights,
// which overflows the half floats the scene is drawn into (65,504 at most) and would spread infinities through bloom.
// Kept well under half of that, since bloom adds blurred levels together.
inline constexpr float maxSkyRadiance = 30000.0f;

// Where a sky object goes this frame: distance from the camera along the way towards it (any length), a unit sphere
// scaled so its edge is angularRadius (radians) off that line as the camera sees it.
Transform skyObjectPose(const Vec3& cameraPosition, const Vec3& towardsObject, float distance, float angularRadius);

// The patch of sky (steradians) a disc of that angular radius covers: 4 pi sin^2(theta / 2), about pi theta^2 when small.
float discSolidAngle(float angularRadius);

/*
* How brightly a disc of that angular radius must glow to give a light's value. The engine's light values include
* Lambert's 1/pi (a white surface facing a light of value v shows v), so the illuminance is pi v, and spread over the
* disc's solid angle that is a radiance of pi v / solid angle. A bigger disc glows dimmer by as much, so the light in
* total stays the same. Capped at maxSkyRadiance by scaling the whole colour, so its hue stays.
*/
Vec3 discRadiance(const Vec3& light, float angularRadius);
