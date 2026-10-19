#include "render/gpu/depth_prepass.h"

DepthState prepassDepthState() {
    return DepthState{DepthCompare::Greater, true};
}

DepthState mainPassDepthState(bool afterPrepass) {
    if (afterPrepass) {
        return DepthState{DepthCompare::Equal, false};
    }

    return DepthState{DepthCompare::Greater, true};
}
