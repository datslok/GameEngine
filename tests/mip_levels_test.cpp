#include "render/gpu/mip_levels.h"

#include <cassert>
#include <stdexcept>

/*
* A full mip chain halves the texture until the long side reaches one pixel.
*/
void testMipLevels() {
    assert(mipLevelCount(1, 1) == 1);
    assert(mipLevelCount(2, 2) == 2);
    assert(mipLevelCount(256, 256) == 9);

    // The long side decides: a 256x64 texture still needs levels down to 1x1.
    assert(mipLevelCount(256, 64) == 9);
    assert(mipLevelCount(1, 512) == 10);

    // Sizes that are not powers of two round down at each level: 300 -> 150 -> 75 -> 37 -> 18 -> 9 -> 4 -> 2 -> 1.
    assert(mipLevelCount(300, 200) == 9);

    bool threw = false;

    try {
        mipLevelCount(0, 16);
    }
    catch (const std::invalid_argument&) {
        threw = true;
    }

    assert(threw);
}
