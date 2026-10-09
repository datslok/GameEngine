#pragma once

#include <cstddef>

/*
* The most bytes of one uniform block a shader can read. SDL's Vulkan backend binds each uniform buffer with a 4 KB
* window (MAX_UBO_SECTION_SIZE), so data pushed past that reads as zero in the shader, with no error anywhere.
* Every uniform struct checks itself against this; anything bigger belongs in a storage buffer.
*/
inline constexpr std::size_t maxUniformBlockBytes = 4096;
