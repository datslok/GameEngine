#pragma once

#include "math/frustum.h"
#include "math/mat4.h"
#include "scene/frame_description.h"
#include "scene/model.h"

#include <cstddef>
#include <vector>

// The sphere around a mesh's bounding box, placed and scaled by its model matrix.
BoundingSphere boundingSphere(const ModelBounds& localBounds, const Mat4& model);

// Give each draw the sphere around its mesh, from bounds indexed like MeshHandle. Draws whose mesh has no known bounds keep
// a negative radius, which means "unknown, never cull".
void attachBoundingSpheres(std::vector<DrawItem>& draws, const std::vector<ModelBounds>& boundsByMesh);

// The draws the camera can see (by index, in order).
std::vector<std::size_t> visibleDraws(const std::vector<DrawItem>& draws, const Frustum& cameraFrustum);

// The draws that can block a light: shadow casters inside the light's own view. Never use the camera's frustum here:
// something behind the camera can still throw a shadow into view.
std::vector<std::size_t> shadowCasters(const std::vector<DrawItem>& draws, const Frustum& lightFrustum);