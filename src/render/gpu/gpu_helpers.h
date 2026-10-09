#pragma once

#include "math/mat4.h"

#include <SDL3/SDL.h>
#include <stdexcept>

// An exception whose message is ours followed by SDL's description of what went wrong.
std::runtime_error gpuError(const char* message);

// Load a compiled SPIR-V shader file. The caller releases the shader (pipelines keep what they need from it).
SDL_GPUShader* loadShader(SDL_GPUDevice* device, const char* filename, SDL_GPUShaderStage stage, Uint32 uniformBufferCount, Uint32 samplerCount,
                          Uint32 storageBufferCount = 0);

// Shaders expect column-major matrices; Mat4 is row-major. Writes 16 floats.
void writeColumnMajor(const Mat4& matrix, float* target);
