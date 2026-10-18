#pragma once

#include "volk/volk.h"

#include <utility>

/*
* One Vulkan object made from a device (a pipeline, a sampler, a semaphore...), destroyed with it when this goes. The
* destroy function is stored as a value: volk fills its function pointers in at run time, so they cannot be template
* arguments. Move-only, so every object has exactly one owner.
*/
template<typename Handle>
class OwnedHandle {
public:
    using Destroy = void (*)(VkDevice, Handle, const VkAllocationCallbacks*);

    OwnedHandle() = default;

    OwnedHandle(VkDevice device, Handle handle, Destroy destroy):
        device(device),
        handle(handle),
        destroy(destroy)
    {
    }

    ~OwnedHandle() {
        reset();
    }

    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;

    OwnedHandle(OwnedHandle&& other) noexcept:
        device(other.device),
        handle(std::exchange(other.handle, VK_NULL_HANDLE)),
        destroy(other.destroy)
    {
    }

    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            device = other.device;
            handle = std::exchange(other.handle, VK_NULL_HANDLE);
            destroy = other.destroy;
        }

        return *this;
    }

    void reset() noexcept {
        if (handle != VK_NULL_HANDLE) {
            destroy(device, handle, nullptr);
            handle = VK_NULL_HANDLE;
        }
    }

    Handle get() const {
        return handle;
    }

    explicit operator bool() const {
        return handle != VK_NULL_HANDLE;
    }

private:
    VkDevice device = VK_NULL_HANDLE;
    Handle handle = VK_NULL_HANDLE;
    Destroy destroy = nullptr;
};
