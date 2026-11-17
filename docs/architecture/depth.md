# Depth and camera-relative rendering

Covers: `src/render/gpu/gpu_depth_range`, `src/render/gpu/depth_shared.h`, `src/render/gpu/camera_relative`

How the renderer keeps depth and position precision. The rules: `src/render/CLAUDE.md`; where the passes use them: `renderer.md`.

## Reversed-Z

Rule summary in `src/render/CLAUDE.md`; mechanics:

- `toGpuDepthRange(projection)` (`render/gpu/gpu_depth_range`) replaces the clip depth row: depth = 0.5 w - 0.5 z, so OpenGL's -w..w becomes w..0.
- `gpuNearDepth` 1, `gpuFarDepth` 0 (from `render/gpu/depth_shared.h`, which `fullscreen_triangle.vert` reads too).
- Every GPU projection goes through it: camera, point and spot faces, sun boxes, debug lines. So: tests `GREATER`; clears and tile resets to `gpuFarDepth`; shadow compare `GREATER_OR_EQUAL`; the blocker search counts stored depths greater than the receiver's; `linearShadowDepth` = f n / (n + depth (f - n)).
- `Frustum::fromClipMatrix(m, ClipDepth::OneToZero)` for GPU matrices.
- Why: float precision is densest near 0, where perspective depth packs the far distances, so the two nearly cancel (`gpu_depth_range_test`: 1 cm apart at 1 km stays distinguishable).
- The far plane stays finite (culling and shadow fitting use it as the draw distance).

## Camera-relative rendering

- `cameraRelativeViewProjection(camera)`: the view-projection with the camera at the origin (rotation only).
- `triangle.vert` subtracts `cameraOrigin` from the instance's translation before any product (exactly `relativeTo(model, origin)`, checked in `gpu_scene_test`); normal matrix unchanged; outputs `relativePosition`.
- `packLighting` subtracts the camera from light positions; each `ShadowTileData::matrix` takes camera-relative points (point and spot faces rebuilt from `position - camera`; a cascade box is its world matrix times `translation(camera)`, since it is snapped to texels in world space).
- World space stays for the shadow pass itself (the tile cache keys on world matrices), culling, light priority and debug lines (`viewProjection`).
- Why: far from the origin, view × model subtracts two large rounded numbers (`camera_relative_test`: the relative path is many times closer to the exact screen position at 12 km). `World` positions are still floats.

