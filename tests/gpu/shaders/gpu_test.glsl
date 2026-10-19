// What every GPU test shader shares, included with #include "gpu_test.glsl": the case buffers, the push constants that
// say which function to run on how many cases, and reading and writing one case. Each invocation runs one case.

#ifndef GPU_TEST_GLSL
#define GPU_TEST_GLSL

#include "gpu_test_functions.h"

layout(local_size_x = GPU_TEST_GROUP_SIZE) in;

// Cases one after another, inputsPerCase vec4s each in, outputsPerCase vec4s each out. Only vec4s, which take 16 bytes
// in std430 and in C++ (GpuVec4) alike, so there is no padding to get wrong.
layout(std430, set = 0, binding = 0) readonly buffer Inputs {
    vec4 inputs[];
};

layout(std430, set = 0, binding = 1) writeonly buffer Outputs {
    vec4 outputs[];
};

// Must match TestPushConstants in tests/gpu/gpu_function_runner.cpp.
layout(push_constant) uniform TestCase {
    uint count;
    uint function;
    uint inputsPerCase;
    uint outputsPerCase;
};

// The last workgroup has spare invocations past the last case; they must do nothing.
bool outsideTheCases() {
    return gl_GlobalInvocationID.x >= count;
}

vec4 testInput(uint index) {
    return inputs[gl_GlobalInvocationID.x * inputsPerCase + index];
}

void testOutput(uint index, vec4 value) {
    outputs[gl_GlobalInvocationID.x * outputsPerCase + index] = value;
}

#endif
