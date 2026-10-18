#pragma once

#include <cstddef>

/*
* The most bytes of one uniform block a shader can read. The renderer binds every uniform block through a descriptor
* with a 4 KB range (FrameUniforms; the size SDL_GPU used, kept from the port), so a shader reading past it reads
* outside the range, with no error anywhere.
* Every uniform struct checks itself against this; anything bigger belongs in a storage buffer.
*/
inline constexpr std::size_t maxUniformBlockBytes = 4096;
