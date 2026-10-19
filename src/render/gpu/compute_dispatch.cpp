#include "render/gpu/compute_dispatch.h"

#include <stdexcept>

// Rounded up without adding groupSize - 1 first, which would wrap around for counts near the top of the range.
std::uint32_t workgroupCount(std::uint32_t count, std::uint32_t groupSize) {
    if (groupSize == 0) {
        throw std::invalid_argument("A workgroup must have at least one invocation");
    }

    return count / groupSize + (count % groupSize != 0 ? 1u : 0u);
}
