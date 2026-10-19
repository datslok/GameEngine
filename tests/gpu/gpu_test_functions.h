#ifndef TESTS_GPU_GPU_TEST_FUNCTIONS_H
#define TESTS_GPU_GPU_TEST_FUNCTIONS_H

// Which function a GPU test shader runs, shared by the test shaders (tests/gpu/shaders) and the C++ tests that
// dispatch them. Numbered per shader. Macro-only, since GLSL reads it too.

// Invocations per workgroup in every test shader (local_size_x): 64 fills both NVIDIA's 32-wide and AMD's 64-wide units.
#define GPU_TEST_GROUP_SIZE 64

// brdf_test.comp
#define GPU_TEST_GGX_DISTRIBUTION 0
#define GPU_TEST_SMITH_VISIBILITY 1
#define GPU_TEST_SCHLICK_FRESNEL 2
#define GPU_TEST_PREPARE_SHADING 3
#define GPU_TEST_ADD_LIGHT 4
#define GPU_TEST_FACES_LIGHT 5

// normal_mapping_test.comp
#define GPU_TEST_DECODE_NORMAL_TEXEL 0
#define GPU_TEST_PERTURB_NORMAL 1

// lighting_test.comp
#define GPU_TEST_DISTANCE_FALLOFF 0
#define GPU_TEST_SPOT_CONE 1
#define GPU_TEST_POINT_SHADOW_FACE 2

// soft_shadow_test.comp
#define GPU_TEST_LINEAR_SHADOW_DEPTH 0
#define GPU_TEST_BLOCKER_SEARCH_TEXELS 1
#define GPU_TEST_PENUMBRA_TEXELS 2

// tone_mapping_test.comp
#define GPU_TEST_TONE_MAP 0

// bloom_test.comp
#define GPU_TEST_KARIS_WEIGHT 0
#define GPU_TEST_MIX_BLOOM 1

// dither_test.comp
#define GPU_TEST_LINEAR_TO_SRGB 0
#define GPU_TEST_SRGB_TO_LINEAR 1
#define GPU_TEST_INTERLEAVED_GRADIENT_NOISE 2
#define GPU_TEST_DITHER_FOR_EIGHT_BITS 3

// overlay_test.comp
#define GPU_TEST_OVERLAY_OUTPUT 0

// clusters_test.comp
#define GPU_TEST_TO_CLUSTER_VIEW 0
#define GPU_TEST_CLUSTER_SLICE 1
#define GPU_TEST_CLUSTER_OF_PIXEL 2
#define GPU_TEST_CLUSTER_BOUNDS 3
#define GPU_TEST_SPHERE_TOUCHES_BOX 4
#define GPU_TEST_CONE_TOUCHES_SPHERE 5
#define GPU_TEST_CLUSTER_HEAT_COLOUR 6

#endif
