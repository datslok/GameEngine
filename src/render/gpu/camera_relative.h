#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "scene/camera.h"

/*
* Camera-relative rendering. A float keeps about seven significant digits, so 12 km from the world's origin its steps
* are about a millimetre. Drawing with world positions puts the camera's position into the view matrix and the object's
* into the model matrix, and multiplying them subtracts two big numbers that were each rounded to that millimetre: the
* rounding is what remains, and vertices jitter on screen as the camera moves. So everything the main pass's shaders see
* is measured from the camera instead: the object's offset from the camera is one subtraction of the two positions, done
* before any matrix, and its result is as precise as a float near zero allows. The world's axes stay as they are (only
* the origin moves to the camera), so normals and light directions are unchanged.
*
* The positions themselves are still floats, so very far from the origin they are already rounded before the
* subtraction; the solar system (ROADMAP phase 13) keeps them in doubles for that.
*/

// The camera's view and projection (in the GPU's depth range) for points measured from the camera: it turns them but
// does not move them, since the camera sits at the origin.
Mat4 cameraRelativeViewProjection(const Camera& camera);

// A model matrix that places its object relative to origin rather than to the world's origin: only its translation changes.
Mat4 relativeTo(const Mat4& model, const Vec3& origin);
