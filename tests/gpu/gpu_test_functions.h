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
#define GPU_TEST_TOKSVIG_ROUGHNESS 2

// lighting_test.comp
#define GPU_TEST_DISTANCE_FALLOFF 0
#define GPU_TEST_SPOT_CONE 1
#define GPU_TEST_POINT_SHADOW_FACE 2

// soft_shadow_test.comp
#define GPU_TEST_LINEAR_SHADOW_DEPTH 0
#define GPU_TEST_BLOCKER_SEARCH_TEXELS 1
#define GPU_TEST_PENUMBRA_TEXELS 2
#define GPU_TEST_BOX_SHADOW_DISTANCE 3
#define GPU_TEST_SUN_BLOCKER_SEARCH_TEXELS 4
#define GPU_TEST_SUN_PENUMBRA_TEXELS 5

// tone_mapping_test.comp
#define GPU_TEST_TONE_MAP 0
#define GPU_TEST_NIGHT_VISION 1

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

// cascades_test.comp
#define GPU_TEST_CASCADE_EDGE_DISTANCE 0
#define GPU_TEST_CASCADE_BLEND 1
#define GPU_TEST_SUN_SHADOW_BIAS 2

// ray_tracing_test.comp
#define GPU_TEST_CAMERA_RAY 0

// ddgi_test.comp
#define GPU_TEST_OCTAHEDRAL 0
#define GPU_TEST_PROBE_TEXELS 1
#define GPU_TEST_PROBE_RAYS 2
#define GPU_TEST_PROBE_WEIGHTS 3
#define GPU_TEST_PROBE_BLENDING 4
#define GPU_TEST_PROBE_GRID 5
#define GPU_TEST_PROBE_REACH 6
#define GPU_TEST_PROBE_ADAPTIVE 7
#define GPU_TEST_PROBE_SHARING 8
#define GPU_TEST_PROBE_RESTART 9

// ddgi_sampling_test.comp
#define GPU_TEST_PROBE_BIAS 0
#define GPU_TEST_PROBE_CELL 1
#define GPU_TEST_PROBE_CORNER_WEIGHTS 2
#define GPU_TEST_PROBE_CASCADE_WEIGHT 3
#define GPU_TEST_PROBE_BLEND 4
#define GPU_TEST_HIT_LIGHTS 5
#define GPU_TEST_BRIGHTEST_LIGHTS 6

// probe_relocation_test.comp
#define GPU_TEST_ESCAPE_SCALE 0
#define GPU_TEST_RELOCATION_STEP 1

// atmosphere_test.comp
#define GPU_TEST_ATMOSPHERE_MEDIUM 0
#define GPU_TEST_ATMOSPHERE_TRANSMITTANCE 1
#define GPU_TEST_ATMOSPHERE_TABLES 2
#define GPU_TEST_ATMOSPHERE_MARCH 3
#define GPU_TEST_ATMOSPHERE_MULTIPLE_SCATTERING 4
#define GPU_TEST_ATMOSPHERE_LIGHTS 5

// stars_test.comp
#define GPU_TEST_STARS 0

// eye_adaptation_test.comp
#define GPU_TEST_EYE_STEPS 0
#define GPU_TEST_EYE_MEAN 1

// taa_test.comp
#define GPU_TEST_TAA_PIXELS 0
#define GPU_TEST_TAA_COLOURS 1
#define GPU_TEST_TAA_WEIGHTS 2
#define GPU_TEST_TAA_REPROJECTION 3
#define GPU_TEST_TAA_UPSCALING 4

// screen_reflections_test.comp
#define GPU_TEST_SSR_PYRAMID 0
#define GPU_TEST_SSR_POSITION 1
#define GPU_TEST_SSR_SCREEN_RAY 2
#define GPU_TEST_SSR_MARCH 3
#define GPU_TEST_SSR_SHADING 4

#endif
