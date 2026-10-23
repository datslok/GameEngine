#include "render/gpu/alpha_mask.h"

#include <cassert>

namespace {
    // glTF's rule: a texel is kept, fully opaque, when its alpha is at or above the cutoff, else thrown away.
    void testCutoff() {
        assert(!isCutAway(0.5f, 0.5f));
        assert(isCutAway(0.4999f, 0.5f));
        assert(!isCutAway(1.0f, 0.5f));
        assert(isCutAway(0.0f, 0.25f));
        assert(!isCutAway(0.0f, 0.0f));
    }

    // A double-sided surface seen from behind is shaded as its mirror image: its normal and tangent frame turned round.
    // One-sided surfaces and front faces keep theirs.
    void testFacingSign() {
        assert(facingSign(true, true) == 1.0f);
        assert(facingSign(false, true) == -1.0f);
        assert(facingSign(true, false) == 1.0f);
        assert(facingSign(false, false) == 1.0f);
    }
}

void testAlphaMask() {
    testCutoff();
    testFacingSign();
}
