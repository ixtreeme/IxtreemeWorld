// IXVulkanEditorAdapter: the only place (with the frame owner) allowed to touch
// ImGui_ImplVulkan_* and native views/samplers for UI display. Editor UI code
// goes through IEditorTextureProvider and never includes this file.

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "IXVulkanEditorAdapter.h"

#include "Debug.h"
#include "IXRHIDevice.h"
#include "IXVulkanBridge.h"
#include "IXVulkanCommandList.h"
#include "VulkanDevice.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#if defined(_WIN32)
#include <imgui_impl_win32.h>
#include <windows.h>
#endif

#include <cstdint>
#include <limits>

#if defined(_WIN32)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);
#endif

namespace ixvulkan
{
namespace
{

constexpr std::uint32_t kAdapterMinImageCount = 2;

void CheckBackendResult(VkResult result)
{
    if (result == VK_SUCCESS)
        return;
    TraceError("[EDITOR-IMGUI] Vulkan backend call failed: %d", static_cast<int>(result));
}

#if defined(_WIN32)
int CreateAdapterVkSurface(ImGuiViewport* viewport,
                           ImU64 vkInstance,
                           const void* vkAllocator,
                           ImU64* outVkSurface)
{
    VkWin32SurfaceCreateInfoKHR create{};
    create.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create.hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
    create.hinstance = GetModuleHandle(nullptr);
    return static_cast<int>(vkCreateWin32SurfaceKHR(reinterpret_cast<VkInstance>(vkInstance),
        &create,
        static_cast<const VkAllocationCallbacks*>(vkAllocator),
        reinterpret_cast<VkSurfaceKHR*>(outVkSurface)));
}
#endif

std::uint32_t CountAdapterDrawCommands(const ImDrawData* drawData)
{
    if (!drawData)
        return 0;
    std::uint32_t count = 0;
    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
        count += static_cast<std::uint32_t>(drawData->CmdLists[listIndex]->CmdBuffer.Size);
    return count;
}

} // namespace

IXVulkanEditorAdapter::IXVulkanEditorAdapter(VulkanDevice& loop, ixrhi::IXRHIDevice& rhi)
    : m_loop(&loop)
    , m_rhi(&rhi)
{
}

IXVulkanEditorAdapter::~IXVulkanEditorAdapter()
{
    ShutdownBackend(); // idempotent; pool dies after all UI registrations
}

std::unique_ptr<IXVulkanEditorAdapter> IXVulkanEditorAdapter::Create(VulkanDevice& loop,
                                                                     ixrhi::IXRHIDevice& rhi)
{
    return std::unique_ptr<IXVulkanEditorAdapter>(new IXVulkanEditorAdapter(loop, rhi));
}

bool IXVulkanEditorAdapter::CreateBackend(void* windowHandle)
{
    if (m_backendReady)
        return true;

    // ImGui context is adapter-owned (created here, destroyed in ShutdownBackend
    // after the backend shutdowns that need it).
    if (ImGui::GetCurrentContext() == nullptr)
        ImGui::CreateContext();

    if (!CreateDescriptorPool())
        return false;

#if defined(_WIN32)
    if (windowHandle != nullptr)
    {
        if (!ImGui_ImplWin32_Init(windowHandle))
        {
            TraceError("[EDITOR-IMGUI] Win32 backend initialization failed");
            return false;
        }
    }
#else
    (void)windowHandle;
#endif

#if defined(_WIN32)
    ImGui::GetPlatformIO().Platform_CreateVkSurface = CreateAdapterVkSurface;
#endif

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_2;
    init.Instance = m_loop->GetInstance();
    init.PhysicalDevice = m_loop->GetPhysicalDevice();
    init.Device = m_loop->GetDevice();
    init.QueueFamily = m_loop->GetGraphicsQueueFamily();
    init.Queue = m_loop->GetGraphicsQueue();
    init.DescriptorPool = m_descriptorPool;
    init.MinImageCount = kAdapterMinImageCount;
    init.ImageCount = m_loop->GetSwapchainImageCount();
    // Backend main pass (owned by the swapchain object, not the legacy loop).
    init.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
    if (const ixrhi::IXRHIRenderPass* mainPass = m_rhi->GetMainPass())
        init.PipelineInfoMain.RenderPass = NativePassOf(*mainPass);
    init.PipelineInfoMain.Subpass = 0;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.CheckVkResultFn = CheckBackendResult;

    if (!ImGui_ImplVulkan_Init(&init))
    {
        TraceError("[EDITOR-IMGUI] Vulkan backend initialization failed");
        return false;
    }

    m_backendReady = true;
    Tracenf("[EDITOR-IMGUI] Initialized with imgui version %s, vulkan backend ready", IMGUI_VERSION);
    return true;
}

void IXVulkanEditorAdapter::ShutdownBackend()
{
    if (!m_backendReady && m_descriptorPool == VK_NULL_HANDLE &&
        ImGui::GetCurrentContext() == nullptr)
        return;

    // UI registrations die before the pool (parity with the old Destroy order).
    ClearViewSlot(m_sceneView);
    ClearViewSlot(m_gameView);
    ReleaseAllPreviewTextures();

    if (m_backendReady)
    {
        ImGui_ImplVulkan_Shutdown();
        m_backendReady = false;
    }
#if defined(_WIN32)
    ImGui_ImplWin32_Shutdown();
#endif
    if (ImGui::GetCurrentContext() != nullptr)
        ImGui::DestroyContext();

    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_loop->GetDevice(), m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_frameActive = false;
}

void IXVulkanEditorAdapter::OnRenderPassChanged()
{
    if (!m_backendReady)
        return;

    if (m_frameActive)
    {
        ImGui::EndFrame();
        ImGuiIO& io = ImGui::GetIO();
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        m_frameActive = false;
    }

    m_loop->WaitIdle();

    // Re-query the backend main pass (rebuilt with the swapchain); the legacy
    // mirror would also work, but the adapter resolves backend authority.
    VkRenderPass mainPass = VK_NULL_HANDLE;
    if (const ixrhi::IXRHIRenderPass* pass = m_rhi->GetMainPass())
        mainPass = NativePassOf(*pass);
    ImGui_ImplVulkan_PipelineInfo pipeline{};
    pipeline.RenderPass = mainPass;
    pipeline.Subpass = 0;
    pipeline.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_CreateMainPipeline(&pipeline);

    Tracen("[EDITOR-IMGUI] Vulkan main pipeline recreated for resized render pass");
}

#if defined(_WIN32)
bool IXVulkanEditorAdapter::HandleWin32Message(HWND hwnd,
                                               UINT message,
                                               WPARAM wParam,
                                               LPARAM lParam,
                                               LRESULT& result)
{
    if (!m_backendReady)
        return false;
    result = ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam);
    return result != 0;
}
#endif

bool IXVulkanEditorAdapter::IsReady() const
{
    return m_backendReady;
}

void IXVulkanEditorAdapter::BeginBackendFrame()
{
    if (!m_backendReady)
        return;
    ImGui_ImplVulkan_NewFrame();
#if defined(_WIN32)
    ImGui_ImplWin32_NewFrame();
#endif
    m_frameActive = true;
}

void IXVulkanEditorAdapter::DrawFrame(ixrhi::IXRHICommandList& cmd, std::uint64_t frameNumber)
{
    if (!m_backendReady || !m_frameActive)
        return;
    m_frameActive = false;

    auto* native = dynamic_cast<IXVulkanCommandList*>(&cmd);
    if (native == nullptr)
        return;

    ImDrawData* drawData = ImGui::GetDrawData();
    if (frameNumber < 3 || (frameNumber % 60u) == 0u)
    {
        TraceDiagf("[FRAME] imgui_render called = yes, draw_lists=%d draw_cmds=%u",
            drawData ? drawData->CmdListsCount : 0,
            CountAdapterDrawCommands(drawData));
    }
    ImGui_ImplVulkan_RenderDrawData(drawData, native->Native());

    static std::uint32_t lastLoggedDrawCommands = std::numeric_limits<std::uint32_t>::max();
    const std::uint32_t drawCommands = CountAdapterDrawCommands(drawData);
    const bool drawCommandsChanged = lastLoggedDrawCommands != drawCommands;
    if (frameNumber != m_lastLoggedFrame && frameNumber % 300 == 0 && drawCommandsChanged)
    {
        m_lastLoggedFrame = frameNumber;
        TraceDiagf("[EDITOR-IMGUI] Frame %llu rendered with %u draw calls",
            static_cast<unsigned long long>(frameNumber),
            drawCommands);
        lastLoggedDrawCommands = drawCommands;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void IXVulkanEditorAdapter::SetSceneViewTexture(std::shared_ptr<ixrhi::IXRHITexture> texture,
                                                std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                                std::uint32_t width,
                                                std::uint32_t height)
{
    SetViewSlot(m_sceneView, std::move(texture), std::move(sampler), width, height, "scene");
}

void IXVulkanEditorAdapter::SetGameViewTexture(std::shared_ptr<ixrhi::IXRHITexture> texture,
                                               std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                               std::uint32_t width,
                                               std::uint32_t height)
{
    SetViewSlot(m_gameView, std::move(texture), std::move(sampler), width, height, "game");
}

void* IXVulkanEditorAdapter::GetSceneViewTexture()
{
    return m_sceneView.uiId;
}

void IXVulkanEditorAdapter::GetSceneViewSize(std::uint32_t& width, std::uint32_t& height)
{
    width = m_sceneView.width;
    height = m_sceneView.height;
}

void* IXVulkanEditorAdapter::GetGameViewTexture()
{
    return m_gameView.uiId;
}

void IXVulkanEditorAdapter::GetGameViewSize(std::uint32_t& width, std::uint32_t& height)
{
    width = m_gameView.width;
    height = m_gameView.height;
}

ixeditor::graphics::EditorTextureHandle IXVulkanEditorAdapter::UploadPreviewTexture(
    const void* rgba8,
    std::uint32_t width,
    std::uint32_t height,
    const char* tag)
{
    using ixeditor::graphics::EditorTextureHandle;
    if (!m_backendReady || rgba8 == nullptr || width == 0 || height == 0)
        return EditorTextureHandle{};

    ixrhi::IXRHITextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = ixrhi::IXRHIFormat::R8G8B8A8Unorm;
    desc.usage = ixrhi::IXRHITextureUsage::Sampled | ixrhi::IXRHITextureUsage::TransferDst;
    desc.debugName = tag ? std::string("Preview:") + tag : "Preview";
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4u;
    auto texture = m_rhi->CreateTexture(desc, rgba8, bytes);
    if (!texture)
        return EditorTextureHandle{};

    ixrhi::IXRHISamplerDesc samplerDesc;
    samplerDesc.minFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.magFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.mipmapFilter = ixrhi::IXRHISamplerFilter::Linear;
    samplerDesc.addressU = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressV = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.addressW = ixrhi::IXRHISamplerAddress::ClampToEdge;
    samplerDesc.maxLod = 1.0f;
    samplerDesc.debugName = desc.debugName + ":Sampler";
    auto sampler = m_rhi->CreateSampler(samplerDesc);
    if (!sampler)
        return EditorTextureHandle{};

    EditorTextureHandle handle;
    void* uiId = RegisterUiTexture(*texture, *sampler, tag ? tag : "preview", handle);
    if (uiId == nullptr)
        return EditorTextureHandle{};
    PreviewEntry entry;
    entry.handle = handle;
    entry.texture = std::move(texture);
    entry.sampler = std::move(sampler);
    entry.uiId = uiId;
    m_previews.emplace(handle.value, std::move(entry));
    return handle;
}

void IXVulkanEditorAdapter::ReleasePreviewTexture(ixeditor::graphics::EditorTextureHandle handle)
{
    const auto it = m_previews.find(handle.value);
    if (it == m_previews.end())
        return;
    if (it->second.uiId != nullptr && m_backendReady)
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(it->second.uiId));
    m_bridge.Unregister(it->second.handle);
    m_previews.erase(it);
}

void IXVulkanEditorAdapter::ReleaseAllPreviewTextures()
{
    if (!m_previews.empty())
        m_loop->WaitIdle(); // parity: previews were destroyed under idle before
    for (auto& [_, entry] : m_previews)
    {
        if (entry.uiId != nullptr && m_backendReady)
            ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(entry.uiId));
        m_bridge.Unregister(entry.handle);
    }
    m_previews.clear();
}

void* IXVulkanEditorAdapter::GetPreviewTexture(ixeditor::graphics::EditorTextureHandle handle)
{
    const auto it = m_previews.find(handle.value);
    return it != m_previews.end() ? it->second.uiId : nullptr;
}

std::size_t IXVulkanEditorAdapter::RegisteredTextureCount() const
{
    return m_bridge.EntryCount();
}

bool IXVulkanEditorAdapter::CreateDescriptorPool()
{
    const VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
    };

    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = 3000;
    pool.poolSizeCount = static_cast<std::uint32_t>(sizeof(poolSizes) / sizeof(poolSizes[0]));
    pool.pPoolSizes = poolSizes;

    const VkResult result =
        vkCreateDescriptorPool(m_loop->GetDevice(), &pool, nullptr, &m_descriptorPool);
    if (result != VK_SUCCESS)
    {
        TraceError("[EDITOR-IMGUI] Failed to create descriptor pool: %d", static_cast<int>(result));
        m_descriptorPool = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void IXVulkanEditorAdapter::SetViewSlot(ViewSlot& slot,
                                        std::shared_ptr<ixrhi::IXRHITexture> texture,
                                        std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        const char* logTag)
{
    // Dedupe: same resources + same size keep the existing UI registration
    // (parity with the old descriptorMatches early-out; no per-frame churn).
    if (slot.uiId != nullptr && slot.texture == texture && slot.sampler == sampler &&
        slot.width == width && slot.height == height)
        return;

    ClearViewSlot(slot);
    slot.width = width;
    slot.height = height;
    if (!m_backendReady || !texture || !sampler || width == 0 || height == 0)
        return;

    slot.texture = std::move(texture);
    slot.sampler = std::move(sampler);
    ixeditor::graphics::EditorTextureHandle handle;
    slot.uiId = RegisterUiTexture(*slot.texture, *slot.sampler, logTag, handle);
    if (slot.uiId != nullptr)
    {
        slot.handle = handle;
        Tracenf("[EDITOR-%s-VIEW] bound offscreen texture extent=%ux%u",
            logTag == nullptr ? "?" : logTag,
            width,
            height);
    }
    else
    {
        slot.texture.reset();
        slot.sampler.reset();
    }
}

void IXVulkanEditorAdapter::ClearViewSlot(ViewSlot& slot)
{
    UnregisterUiTexture(slot.uiId, slot.handle);
    slot.texture.reset();
    slot.sampler.reset();
    slot.width = 0;
    slot.height = 0;
}

void* IXVulkanEditorAdapter::RegisterUiTexture(ixrhi::IXRHITexture& texture,
                                               ixrhi::IXRHISampler& sampler,
                                               const char* tag,
                                               ixeditor::graphics::EditorTextureHandle& outHandle)
{
    // IXRHI resources are shared_ptr-owned by contract; bad_weak_ptr guards
    // against misuse with stack-owned objects.
    try
    {
        outHandle = m_bridge.Register(texture.shared_from_this(),
            sampler.shared_from_this(),
            tag ? tag : "");
    }
    catch (const std::bad_weak_ptr&)
    {
        outHandle = ixeditor::graphics::EditorTextureHandle{};
        return nullptr;
    }
    if (!outHandle.IsValid())
        return nullptr;
    const VkImageView view = NativeViewOf(texture);
    const VkSampler nativeSampler = NativeSamplerOf(sampler);
    if (view == VK_NULL_HANDLE || nativeSampler == VK_NULL_HANDLE)
    {
        m_bridge.Unregister(outHandle);
        outHandle = ixeditor::graphics::EditorTextureHandle{};
        return nullptr;
    }
    VkDescriptorSet id = ImGui_ImplVulkan_AddTexture(nativeSampler,
        view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (id == VK_NULL_HANDLE)
    {
        m_bridge.Unregister(outHandle);
        outHandle = ixeditor::graphics::EditorTextureHandle{};
        return nullptr;
    }
    return reinterpret_cast<void*>(id);
}

void IXVulkanEditorAdapter::UnregisterUiTexture(void*& uiId,
                                                ixeditor::graphics::EditorTextureHandle& handle)
{
    if (uiId != nullptr && m_backendReady)
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(uiId));
    uiId = nullptr;
    if (handle.IsValid())
    {
        m_bridge.Unregister(handle);
        handle = ixeditor::graphics::EditorTextureHandle{};
    }
}

} // namespace ixvulkan
