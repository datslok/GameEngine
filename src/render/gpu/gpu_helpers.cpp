#include "render/gpu/gpu_helpers.h"

#include <string>

/*
* SDL reports failures through a null result plus a message in SDL_GetError, so every GPU call site needs both pieces.
*/
std::runtime_error gpuError(const char* message) {
    return std::runtime_error(
        std::string{message} + ": " + SDL_GetError()
    );
}

/*
* The shader's resource counts must be declared up front: SDL_GPU binds by slot, and the counts must match the shader.
*/
SDL_GPUShader* loadShader(
    SDL_GPUDevice* device,
    const char* filename,
    SDL_GPUShaderStage stage,
    Uint32 uniformBufferCount,
    Uint32 samplerCount
) {
    std::size_t codeSize = 0;
    void* code = SDL_LoadFile(filename, &codeSize);

    if (code == nullptr) {
        throw gpuError(filename);
    }

    SDL_GPUShaderCreateInfo info{};
    info.code = static_cast<const Uint8*>(code);
    info.code_size = codeSize;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_uniform_buffers = uniformBufferCount;
    info.num_samplers = samplerCount;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);

    if (shader == nullptr) {
        const std::runtime_error error =
            gpuError("Shader creation failed");

        SDL_free(code);
        throw error;
    }

    SDL_free(code);
    return shader;
}

/*
* GLSL stores matrices column by column, while Mat4 stores values[row][column], so pushing one is a transpose.
*/
void writeColumnMajor(const Mat4& matrix, float* target) {
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            target[column * 4 + row] = matrix.values[row][column];
        }
    }
}
