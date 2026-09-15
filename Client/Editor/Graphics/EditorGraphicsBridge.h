#pragma once

// EditorGraphicsBridge — generic editor texture registry (Phase 3B, §10).
//
// Vk-free, ImGui-free, dependency-free (IXRHI types only). Maps stable
// EditorTextureHandles to IXRHI texture+sampler pairs. The backend-specific
// adapter (IXVulkanEditorAdapter) owns backend UI registration (ImGui
// AddTexture/RemoveTexture) against these entries; editor UI resolves opaque
// UI ids through the provider interface below.
//
// Ownership (§29): offscreen renderers own IXRHI resources; the bridge holds
// WEAK references (no cycles, no lifetime extension); the backend adapter owns
// UI-side registrations. Main-thread only (matches ImGui usage).

#include "IXRHITexture.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ixeditor::graphics
{

struct EditorTextureHandle
{
    std::uint64_t value = 0;

    bool IsValid() const noexcept { return value != 0; }
    bool operator==(const EditorTextureHandle& other) const noexcept
    {
        return value == other.value;
    }
};

// Generic UI texture provider consumed by editor code (EditorImGui + panels).
// No Vulkan, no ImGui in these signatures: UI ids are opaque void* resolved
// to ImTextureID by panels (which already reinterpret_cast today).
class IEditorTextureProvider
{
public:
    virtual ~IEditorTextureProvider() = default;

    virtual bool IsReady() const = 0;
    virtual void BeginBackendFrame() = 0;
    // Backend frame still open (OnRenderPassChanged may close it mid-frame;
    // editor UI must not record panels afterwards — parity guard).
    virtual bool IsBackendFrameActive() const = 0;

    // Scene/game view slots. Registration is explicit (creation/resize), never
    // per-frame; null/empty inputs unbind the slot. Sizes via *Size methods.
    virtual void SetSceneViewTexture(std::shared_ptr<ixrhi::IXRHITexture> texture,
                                     std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                     std::uint32_t width,
                                     std::uint32_t height) = 0;
    virtual void SetGameViewTexture(std::shared_ptr<ixrhi::IXRHITexture> texture,
                                    std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                    std::uint32_t width,
                                    std::uint32_t height) = 0;

    // Scene/game view slots (null id when unbound). Sizes via *Size methods.
    virtual void* GetSceneViewTexture() = 0;
    virtual void GetSceneViewSize(std::uint32_t& width, std::uint32_t& height) = 0;
    virtual void* GetGameViewTexture() = 0;
    virtual void GetGameViewSize(std::uint32_t& width, std::uint32_t& height) = 0;

    // Asset/material preview pool (keyed by the bridge, N entries, §15).
    virtual EditorTextureHandle UploadPreviewTexture(const void* rgba8,
                                                      std::uint32_t width,
                                                      std::uint32_t height,
                                                      const char* tag) = 0;
    virtual void ReleasePreviewTexture(EditorTextureHandle handle) = 0;
    virtual void ReleaseAllPreviewTextures() = 0; // waits idle internally
    virtual void* GetPreviewTexture(EditorTextureHandle handle) = 0;
};

class EditorGraphicsBridge
{
public:
    EditorGraphicsBridge() = default;
    ~EditorGraphicsBridge() = default;

    EditorGraphicsBridge(const EditorGraphicsBridge&) = delete;
    EditorGraphicsBridge& operator=(const EditorGraphicsBridge&) = delete;

    EditorTextureHandle Register(std::shared_ptr<ixrhi::IXRHITexture> texture,
                                 std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                 const std::string& tag);
    void Unregister(EditorTextureHandle handle);
    void Clear();

    bool Lookup(EditorTextureHandle handle,
                std::shared_ptr<ixrhi::IXRHITexture>& outTexture,
                std::shared_ptr<ixrhi::IXRHISampler>& outSampler) const;

    std::size_t EntryCount() const;

private:
    struct Entry
    {
        std::weak_ptr<ixrhi::IXRHITexture> texture;
        std::weak_ptr<ixrhi::IXRHISampler> sampler;
        std::string tag;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::uint64_t, Entry> m_entries;
    std::uint64_t m_nextHandle = 1;
};

} // namespace ixeditor::graphics
