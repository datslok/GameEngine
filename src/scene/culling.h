#pragma once

#include "math/frustum.h"
#include "math/mat4.h"
#include "scene/frame_description.h"
#include "scene/model.h"

#include <cstddef>
#include <vector>

// The sphere around a mesh's bounding box, placed and scaled by its model matrix.
BoundingSphere boundingSphere(const Aabb& localBounds, const Mat4& model);

// Give each draw the sphere around its mesh, from bounds indexed like MeshHandle. Draws whose mesh has no known bounds keep
// a negative radius, which means "unknown, never cull".
void attachBoundingSpheres(std::vector<DrawItem>& draws, const std::vector<Aabb>& boundsByMesh);

// Give each draw its mesh's flat faces, from lists indexed like MeshHandle, which must outlive the frame.
void attachFlatFaces(std::vector<DrawItem>& draws, const std::vector<std::vector<FlatFace>>& facesByMesh);

// The draws the camera can see (by index, in order).
std::vector<std::size_t> visibleDraws(const std::vector<DrawItem>& draws, const Frustum& cameraFrustum);

// The draws that can block a light: shadow casters inside the light's own view. Never use the camera's frustum here:
// something behind the camera can still throw a shadow into view.
std::vector<std::size_t> shadowCasters(const std::vector<DrawItem>& draws, const Frustum& lightFrustum);

// The shadow casters a light can reach: those whose sphere touches its reach (its range around it; a negative radius
// reaches everything, as for a directional light). One cheap test per caster before a point light's six face frustums.
std::vector<std::size_t> castersInReach(const std::vector<DrawItem>& draws, const BoundingSphere& reach);

// The glass (transparent draws) a light can reach, by the same test: what its coloured shadow map draws
// (render/gpu/transparency.h). Every pane tints, whatever its castsShadows says, as the glass rays tint through every
// pane; solid casters never include glass, nor glass solid casters.
std::vector<std::size_t> glassInReach(const std::vector<DrawItem>& draws, const BoundingSphere& reach);

// The candidates (draw indices, such as castersInReach gives) inside a light view's frustum, in the candidates' order.
std::vector<std::size_t> shadowCasters(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& candidates,
                                       const Frustum& lightFrustum);