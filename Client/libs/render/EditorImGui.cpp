#include "EditorImGui.h"

#include "Debug.h"
#include "VulkanDevice.h"

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
#include <cstring>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
constexpr uint32_t kMinImageCount = 2;
constexpr const char* kLayoutFile = "editor_layout.ini";

void CheckVkResult(VkResult result)
{
    if (result == VK_SUCCESS)
        return;
    TraceError("[EDITOR-IMGUI] Vulkan backend call failed: %d", static_cast<int>(result));
}

int CreateWin32VkSurface(ImGuiViewport* viewport, ImU64 vkInstance, const void* vkAllocator, ImU64* outVkSurface)
{
    VkWin32SurfaceCreateInfoKHR create{};
    create.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    create.hwnd = static_cast<HWND>(viewport->PlatformHandleRaw);
    create.hinstance = GetModuleHandle(nullptr);
    return static_cast<int>(vkCreateWin32SurfaceKHR(
        reinterpret_cast<VkInstance>(vkInstance),
        &create,
        static_cast<const VkAllocationCallbacks*>(vkAllocator),
        reinterpret_cast<VkSurfaceKHR*>(outVkSurface)));
}

uint32_t CountDrawCommands(const ImDrawData* drawData)
{
    if (!drawData)
        return 0;

    uint32_t count = 0;
    for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex)
        count += static_cast<uint32_t>(drawData->CmdLists[listIndex]->CmdBuffer.Size);
    return count;
}
}

EditorImGui::~EditorImGui()
{
    Destroy();
}

bool EditorImGui::Create(VulkanDevice& device, HWND hwnd)
{
    if (m_initialized)
        return true;

    m_device = device.GetDevice();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = kLayoutFile;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 4.0f;
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;

    if (!CreateDescriptorPool(device))
    {
        ImGui::DestroyContext();
        return false;
    }

    if (!ImGui_ImplWin32_Init(hwnd))
    {
        TraceError("[EDITOR-IMGUI] Win32 backend initialization failed");
        Destroy();
        return false;
    }

    ImGui::GetPlatformIO().Platform_CreateVkSurface = CreateWin32VkSurface;

    if (!InitVulkanBackend(device))
    {
        Destroy();
        return false;
    }

    m_initialized = true;
    Tracenf("[EDITOR-IMGUI] Initialized with imgui version %s, vulkan backend ready", IMGUI_VERSION);
    Tracen("[EDITOR-IMGUI] Docking enabled, multi-viewport enabled");
    Tracenf("[EDITOR-IMGUI] Layout file: %s", kLayoutFile);
    return true;
}

bool EditorImGui::CreateDescriptorPool(VulkanDevice& device)
{
    const VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
    };

    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = 3000;
    pool.poolSizeCount = static_cast<uint32_t>(sizeof(poolSizes) / sizeof(poolSizes[0]));
    pool.pPoolSizes = poolSizes;

    const VkResult result = vkCreateDescriptorPool(device.GetDevice(), &pool, nullptr, &m_descriptorPool);
    if (result != VK_SUCCESS)
    {
        TraceError("[EDITOR-IMGUI] Failed to create descriptor pool: %d", static_cast<int>(result));
        m_descriptorPool = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool EditorImGui::InitVulkanBackend(VulkanDevice& device)
{
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_2;
    init.Instance = device.GetInstance();
    init.PhysicalDevice = device.GetPhysicalDevice();
    init.Device = device.GetDevice();
    init.QueueFamily = device.GetGraphicsQueueFamily();
    init.Queue = device.GetGraphicsQueue();
    init.DescriptorPool = m_descriptorPool;
    init.MinImageCount = kMinImageCount;
    init.ImageCount = device.GetSwapchainImageCount();
    init.PipelineInfoMain.RenderPass = device.GetRenderPass();
    init.PipelineInfoMain.Subpass = 0;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.CheckVkResultFn = CheckVkResult;

    if (!ImGui_ImplVulkan_Init(&init))
    {
        TraceError("[EDITOR-IMGUI] Vulkan backend initialization failed");
        m_vulkanBackendReady = false;
        return false;
    }

    m_vulkanBackendReady = true;
    return true;
}

bool EditorImGui::HandleWin32Message(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    if (!m_initialized)
        return false;

    result = ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam);
    return result != 0;
}

void EditorImGui::BeginFrame(bool editorModeActive)
{
    if (!m_initialized || !m_vulkanBackendReady || m_frameActive)
        return;

    m_editorModeActive = editorModeActive;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    m_frameActive = true;
}

void EditorImGui::RenderDemoPanels()
{
    if (!m_editorModeActive)
        return;

    if (ImGui::Begin("Editor Test Panel"))
    {
        ImGui::Text("ImGui-Vulkan-binding active");
        ImGui::Text("ImGui version: %s", IMGUI_VERSION);
        ImGui::Separator();

        if (ImGui::Button("Click Me"))
            Tracen("[EDITOR-IMGUI] Test button clicked");

        ImGui::SameLine();
        ImGui::Text("Press the button to verify event-handling");
        ImGui::Separator();
        ImGui::Text("Docking: drag the panel header to test docking");
        ImGui::Text("Multi-viewport: drag the panel out of the main window");
    }
    ImGui::End();

    if (m_showDemoWindow)
        ImGui::ShowDemoWindow(&m_showDemoWindow);
}

void EditorImGui::Render(VulkanDevice& device)
{
    if (!m_initialized || !m_vulkanBackendReady || !m_frameActive)
        return;

    RenderDemoPanels();
    ImGui::Render();

    ImDrawData* drawData = ImGui::GetDrawData();
    ImGui_ImplVulkan_RenderDrawData(drawData, device.GetCommandBuffer());

    if (device.GetFrameNumber() != m_lastLoggedFrame && device.GetFrameNumber() % 300 == 0)
    {
        m_lastLoggedFrame = device.GetFrameNumber();
        Tracenf("[EDITOR-IMGUI] Frame %llu rendered with %u draw calls",
            static_cast<unsigned long long>(device.GetFrameNumber()),
            CountDrawCommands(drawData));
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    m_frameActive = false;
}

void EditorImGui::OnRenderPassChanged(VulkanDevice& device)
{
    if (!m_initialized || !m_vulkanBackendReady)
        return;

    vkDeviceWaitIdle(device.GetDevice());
    ImGui_ImplVulkan_Shutdown();
    m_vulkanBackendReady = false;
    InitVulkanBackend(device);
}

bool EditorImGui::WantsInputCapture(const InputEvent& event) const
{
    if (!m_initialized)
        return false;

    const ImGuiIO& io = ImGui::GetIO();
    switch (event.type)
    {
    case InputEvent::MouseMove:
    case InputEvent::MouseDown:
    case InputEvent::MouseUp:
    case InputEvent::MouseWheel:
        return io.WantCaptureMouse;
    case InputEvent::KeyDown:
    case InputEvent::KeyUp:
    case InputEvent::Char:
        return io.WantCaptureKeyboard;
    default:
        return false;
    }
}

void EditorImGui::Destroy()
{
    if (m_vulkanBackendReady)
    {
        ImGui_ImplVulkan_Shutdown();
        m_vulkanBackendReady = false;
    }

    if (m_initialized)
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_initialized = false;
    }

    if (m_descriptorPool && m_device)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    m_device = VK_NULL_HANDLE;
    m_frameActive = false;
}
#else
EditorImGui::~EditorImGui() = default;

bool EditorImGui::Create(VulkanDevice&, void*)
{
    return true;
}

void EditorImGui::BeginFrame(bool)
{
}

void EditorImGui::Render(VulkanDevice&)
{
}

void EditorImGui::OnRenderPassChanged(VulkanDevice&)
{
}

bool EditorImGui::WantsInputCapture(const InputEvent&) const
{
    return false;
}

void EditorImGui::Destroy()
{
}
#endif
