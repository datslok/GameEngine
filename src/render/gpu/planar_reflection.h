#pragma once

#include "math/frustum.h"
#include "math/mat4.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/planar_reflection_shared.h"
#include "render/gpu/ray_tracing.h"
#include "render/gpu/upscaling.h"
#include "scene/camera.h"
#include "scene/frame_description.h"

#include <cstddef>
#include <optional>
#include <vector>

/*
* Planar reflections: a flat mirror's picture drawn for real. The reflection probe sees the world from one point, so a
* flat mirror, which shows it from a point behind the glass, asks for things the probe never saw (past a door frame) at
* full size, where every error shows. But a mirror image is only the world seen from a mirrored viewpoint, so the scene
* is drawn a second time through the reflection matrix.
*
* Reflecting across the plane n.x = d sends x to x - 2 (n.x - d) n. Drawn through viewProjection * reflection, what stands
* in front of the mirror lands on the very pixel where the eye sees it in the mirror, so the reflection pass simply reads
* the image at the mirror's own pixel, with no search. Three things follow:
* - a reflection turns the scene inside out, so front faces become back faces. The reflection probe's capture has the
*   same problem, and shares its pipelines (front faces culled);
* - what is behind the mirror must not show in it. The usual trick turns the projection's near plane onto the mirror
*   (an oblique near plane, Lengyel 2005), but every pipeline clamps depth rather than clip at the near plane, which
*   flattened the wall behind the mirror onto it, in front of everything. So the vertex shader clips at the plane itself
*   (gl_ClipDistance, planarClipPlane), and the projection stays the camera's own reflected, with ordinary depths, which
*   the screen's depth measurement (render/gpu/depth_range.h) reads as it reads the main pass's;
* - the sun's cascades cover what the camera sees, but the mirror shows what is behind it, which they never hold, so
*   everything in the mirror stood in full sun. The mirror gets cascades of its own (shadow_map.h, MirrorShadowView):
*   the camera seen in the mirror (mirroredCamera) from where the glass can start along its view (mirrorCascadesStart)
*   to the farthest surface its picture measured.
*
* Nothing is marked by hand: each frame the flat, smooth, metal face filling the most of the view is chosen
* (choosePlanarMirror), keeping last frame's unless another is clearly better, or mirrors would pop as the camera moves.
* Everything on that plane is the mirror, so two panels side by side on a wall share one picture.
*
* Rough mirrors read blurrier levels of the picture. That blur is the same everywhere, where true blur grows with the
* distance from the mirror, so it stops at planarMaxRoughness. In the capture the mirror's objects are shaded as the
* reflection probe's are, with the view from the camera: their matte light is right and their highlights are a little off,
* and a mirror in the mirror shows only its stand-in.
*
* This is the API-free part. isOnPlanarMirror and planarClipDistance are mirrored by assets/shaders/planar_reflection.glsl.
*/

// The plane n.x = offset, with the unit normal towards the side it reflects.
struct MirrorPlane {
    Vec3 normal;
    float offset = 0.0f;
};

// The same plane measured from a point (so from the camera, with a camera-relative matrix).
MirrorPlane planeRelativeTo(const MirrorPlane& plane, const Vec3& origin);

// The point on the far side of the plane, as far from it.
Vec3 reflectAcrossPlane(const Vec3& point, const MirrorPlane& plane);

// That reflection as a matrix.
Mat4 planeReflection(const MirrorPlane& plane);

// The view through the mirror: viewProjection (reversed-Z) reflected across the plane. What is behind the mirror is cut
// by planarClipPlane, not by this.
Mat4 planarViewProjection(const Mat4& viewProjection, const MirrorPlane& plane);

// What the mirror's picture can show, for culling its draws: the view (world space, reversed-Z) with the mirror's plane
// (in the world, lifted as planarClipPlane is) for its near side, so nothing behind the glass is drawn.
Frustum planarFrustum(const Mat4& worldViewProjection, const MirrorPlane& plane);

// The camera seen in the mirror: as far behind the glass as the camera is in front, looking the mirrored way, with the
// same lens. Its view of the world is what the mirror's picture shows (left and right swapped).
Camera mirroredCamera(const Camera& camera, const MirrorPlane& plane);

// How near along the mirrored camera's view anything past the glass can be, rounded down to the cascades' ladder
// (cascades.h), and never nearer than the near plane: where the mirror's cascades start.
float mirrorCascadesStart(const Camera& camera, const MirrorPlane& plane);

/*
* The mirror for this frame, from the visible draws: a metal surface no rougher than maxRoughness, not glass, whose flat
* face turns towards the camera (a world position), scored by the angle it fills. previous, last frame's choice, keeps its
* place unless another beats it by half as much again. None if nothing qualifies.
*/
std::optional<MirrorPlane> choosePlanarMirror(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& visible, const Vec3& cameraPosition,
                                              float maxRoughness, const std::optional<MirrorPlane>& previous);

/*
* The plane the mirror's capture clips at, from a plane measured from the camera: xyz its normal, w minus its offset
* and PLANAR_CLIP_LIFT, so a point's signed distance is planarClipDistance, negative behind the mirror. The oblique near
* plane is the same plane, but every pipeline clamps depth (render/vulkan/vulkan_pipeline.cpp) rather than clip at the
* near plane, so the vertex shader cuts at it too, through gl_ClipDistance.
*/
Vec4 planarClipPlane(const MirrorPlane& relativePlane);

// The plane every view but a mirror's clips at: nothing is behind it.
Vec4 noClipPlane();

// A point's signed distance from a clip plane (xyz its normal, w its offset negated), as gl_ClipDistance takes it.
float planarClipDistance(const Vec4& plane, const Vec3& position);

// 1 if a point (from the camera) with this normal is on the mirror's plane, as measured from the camera, else 0.
float isOnPlanarMirror(const Vec3& position, const Vec3& normal, const Vec3& planeNormal, float planeOffset);

// The camera's rays (packRayDebugView) seen in a mirror (in the world), for drawing the sky in it.
GpuRayDebugView mirroredRays(const GpuRayDebugView& view, const MirrorPlane& plane);

// The picture's size: the render size scaled, at least a texel.
RenderSize planarRenderSize(RenderSize render, float scale);
