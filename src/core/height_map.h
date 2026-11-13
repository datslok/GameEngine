#pragma once

#include "core/coverage_mips.h"
#include "math/vec3.h"

/*
* A normal map worked out from a height map, so a surface's lighting agrees with its parallax (render/gpu/parallax.h).
* The height map is grey (its red channel read), 0 the deepest and 255 the top, depth deep in texture units (the
* texture's width being 1). Each texel's slope comes from its neighbours on either side (central differences), wrapping
* round the edges since textures repeat; the normal (-dz/du, -dz/dv, 1) is made unit length and stored as 0..255, alpha
* 255. Rows run along v (row 0 is v = 0), as the engine uploads images.
*/
ImageLevel normalMapFromHeight(const ImageLevel& height, float depth);
