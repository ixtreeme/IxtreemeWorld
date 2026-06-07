#pragma once

#include "InputEvent.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

class VulkanDevice;

namespace client::asset
{
class IAssetReader;
}

class RmlUiLayer
{
public:
    RmlUiLayer();
    ~RmlUiLayer();

    bool Create(VulkanDevice& device, client::asset::IAssetReader& assets, uint32_t width, uint32_t height);
    void Update();
    void Render(VulkanDevice& device);
    void Resize(uint32_t width, uint32_t height);
    void OnRenderPassChanged(VulkanDevice& device);
    bool OnInput(const InputEvent& event);
    void Destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
