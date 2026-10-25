#ifndef RENDER_GPU_CLOTH_GPU_SHARED_H
#define RENDER_GPU_CLOTH_GPU_SHARED_H

// Cloth on the GPU (render/gpu/cloth_gpu.h, cloth.glsl and the cloth_*.comp passes). Only #defines of plain numbers,
// since GLSL includes this file too; C++ reads them as typed constants in render/gpu/cloth_gpu.h.

// Invocations per workgroup in every cloth pass: one point or one constraint each. 64 fills both NVIDIA's 32-wide and
// AMD's 64-wide units.
#define CLOTH_GROUP_SIZE 64

// A collider's kind (GpuCollider::kind), in the order of the Shape variant's types.
#define CLOTH_COLLIDER_SPHERE 0
#define CLOTH_COLLIDER_CAPSULE 1
#define CLOTH_COLLIDER_BOX 2

// The bending model (GpuClothTick::bendingModel), BendingModel's values.
#define CLOTH_BEND_ISOMETRIC 0
#define CLOTH_BEND_HINGE 1

// A point no pin moves (pinSlots), and a tick with no hand (GpuClothTick::grabParticle).
#define CLOTH_NO_PIN 4294967295u
#define CLOTH_NO_GRAB 4294967295u

#endif
