#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/planar_reflection_shared.h"
#include "render/gpu/ray_tracing.h"
#include "render/gpu/upscaling.h"
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
* the image at the mirror's own pixel, with no search. Two things follow:
* - a reflection turns the scene inside out, so front faces become back faces. The reflection probe's capture has the
*   same problem, and shares its pipelines (front faces culled);
* - what is behind the mirror must not show in it. Rather than a clip test in every shader, the projection's near plane
*   is turned to lie on the mirror (an oblique near plane, Lengyel 2005). In reversed-Z the near test is z <= w, so the
*   depth row becomes z = w - k pi(x), with pi the plane's signed distance: z > w exactly behind the mirror. k is chosen
*   so that the view's far corners stay at z >= 0.
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
* This is the API-free part. isOnPlanarMirror is mirrored by assets/shaders/planar_reflection.glsl.
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

// The view through the mirror: viewProjection (reversed-Z) reflected across the plane, its near plane on the mirror.
Mat4 planarViewProjection(const Mat4& viewProjection, const MirrorPlane& plane);

/*
* The mirror for this frame, from the visible draws: a metal surface no rougher than maxRoughness, not glass, whose flat
* face turns towards the camera (a world position), scored by the angle it fills. previous, last frame's choice, keeps its
* place unless another beats it by half as much again. None if nothing qualifies.
*/
std::optional<MirrorPlane> choosePlanarMirror(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& visible, const Vec3& cameraPosition,
                                              float maxRoughness, const std::optional<MirrorPlane>& previous);

// 1 if a point (from the camera) with this normal is on the mirror's plane, as measured from the camera, else 0.
float isOnPlanarMirror(const Vec3& position, const Vec3& normal, const Vec3& planeNormal, float planeOffset);

// The camera's rays (packRayDebugView) seen in a mirror (in the world), for drawing the sky in it.
GpuRayDebugView mirroredRays(const GpuRayDebugView& view, const MirrorPlane& plane);

// The picture's size: the render size scaled, at least a texel.
RenderSize planarRenderSize(RenderSize render, float scale);
