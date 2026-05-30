#pragma once

#include "InputEvent.h"

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <functional>

class NativeWindow
{
public:
    virtual ~NativeWindow() = default;

    virtual bool PumpMessages() = 0;
    virtual bool ConsumeResize(uint32_t& width, uint32_t& height) = 0;
    virtual void RequestClose() = 0;

    using InputCallback = std::function<void(const InputEvent&)>;
    virtual void SetInputCallback(InputCallback cb) = 0;

    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
    virtual VkResult CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) = 0;
    virtual const char* GetVulkanSurfaceExtensionName() const = 0;
};
