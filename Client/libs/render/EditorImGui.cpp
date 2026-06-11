#include "EditorImGui.h"

#include "Debug.h"
#include "ProjectManager.h"
#include "SceneManager.h"
#include "VulkanDevice.h"

#if defined(IXTREEME_WITH_EDITOR) && defined(_WIN32)
#include "IconsFontAwesome6.h"
#include "UIHelpers.h"
#define VK_USE_PLATFORM_WIN32_KHR
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>
#include <stb_image.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <commdlg.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
constexpr uint32_t kMinImageCount = 2;
constexpr const char* kLayoutFile = "editor_layout.ini";
constexpr const char* kAssetPayloadType = "ASSET_ID";

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

std::string ToLowerAscii(std::string value)
{
    for (char& ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

std::string ComparablePath(const std::filesystem::path& path)
{
    if (path.empty())
        return {};

    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::absolute(path, ec);
    if (ec)
        normalized = path;

    ec.clear();
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(normalized, ec);
    if (!ec)
        normalized = canonical;

    return ToLowerAscii(normalized.lexically_normal().generic_string());
}

bool ContainsCaseInsensitive(const std::string& value, const std::string& needle)
{
    if (needle.empty())
        return true;
    return ToLowerAscii(value).find(ToLowerAscii(needle)) != std::string::npos;
}

void CopyToBuffer(char* buffer, size_t size, const std::string& value)
{
    if (!buffer || size == 0)
        return;
    const size_t count = std::min(size - 1, value.size());
    std::memcpy(buffer, value.data(), count);
    buffer[count] = '\0';
}

std::filesystem::path InitialProjectBrowserPath(const std::filesystem::path& preferred)
{
    std::error_code ec;
    if (!preferred.empty() && std::filesystem::exists(preferred, ec))
        return std::filesystem::is_directory(preferred, ec) ? preferred : preferred.parent_path();
    return std::filesystem::current_path(ec);
}

std::string WaterBodyDisplayName(const WaterBody& body)
{
    return body.name.empty() ? ("Water Body " + std::to_string(body.id)) : body.name;
}

std::string PointLightDisplayName(const PointLight& light)
{
    return light.name.empty() ? ("Point Light " + std::to_string(light.id)) : light.name;
}

std::string SpotLightDisplayName(const SpotLight& light)
{
    return light.name.empty() ? ("Spot Light " + std::to_string(light.id)) : light.name;
}

std::string ParentSubpath(const std::string& subpath)
{
    const std::string normalized = AssetLibrary::NormalizeSubpath(subpath);
    const size_t slash = normalized.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    return normalized.substr(0, slash);
}

std::string FolderDisplayName(const std::string& subpath)
{
    const std::string normalized = AssetLibrary::NormalizeSubpath(subpath);
    if (normalized.empty())
        return "Home";
    const size_t slash = normalized.find_last_of('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

bool IsDirectChildFolder(const std::string& parent, const std::string& child)
{
    const std::string normalizedParent = AssetLibrary::NormalizeSubpath(parent);
    const std::string normalizedChild = AssetLibrary::NormalizeSubpath(child);
    if (normalizedChild.empty() || normalizedChild == normalizedParent)
        return false;
    if (normalizedParent.empty())
        return normalizedChild.find('/') == std::string::npos;
    if (normalizedChild.rfind(normalizedParent + "/", 0) != 0)
        return false;
    return normalizedChild.find('/', normalizedParent.size() + 1) == std::string::npos;
}

bool CheckEditorVk(VkResult result, const char* call)
{
    if (result == VK_SUCCESS)
        return true;
    TraceError("[EDITOR-IMGUI] Vulkan thumbnail call failed: %s result=%d", call, static_cast<int>(result));
    return false;
}

void TransitionPreviewImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

ImVec4 AssetCategoryColor(AssetLibrary::Category category)
{
    switch (category)
    {
    case AssetLibrary::Category::Texture: return ImVec4(0.20f, 0.42f, 0.72f, 1.0f);
    case AssetLibrary::Category::Model: return ImVec4(0.48f, 0.38f, 0.70f, 1.0f);
    case AssetLibrary::Category::Animation: return ImVec4(0.72f, 0.50f, 0.20f, 1.0f);
    case AssetLibrary::Category::Material: return ImVec4(0.38f, 0.58f, 0.36f, 1.0f);
    case AssetLibrary::Category::WaterMaterial: return ImVec4(0.16f, 0.58f, 0.64f, 1.0f);
    case AssetLibrary::Category::Scene: return ImVec4(0.42f, 0.50f, 0.66f, 1.0f);
    default: return ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
    }
}

const char* AssetCategoryIcon(AssetLibrary::Category category)
{
    switch (category)
    {
    case AssetLibrary::Category::Texture: return ICON_FA_IMAGE;
    case AssetLibrary::Category::Model: return ICON_FA_CUBE;
    case AssetLibrary::Category::Animation: return ICON_FA_PERSON_RUNNING;
    case AssetLibrary::Category::Material: return ICON_FA_PALETTE;
    case AssetLibrary::Category::WaterMaterial: return ICON_FA_DROPLET;
    case AssetLibrary::Category::Scene: return ICON_FA_GLOBE;
    default: return ICON_FA_FILE;
    }
}

std::string ShortAssetFilename(const AssetLibrary::Entry& entry)
{
    const std::string name = !entry.filename.empty() ? entry.filename : entry.displayName;
    constexpr size_t kVisibleCharacters = 10;
    if (name.size() <= kVisibleCharacters)
        return name;
    return name.substr(0, kVisibleCharacters) + "...";
}

std::optional<std::filesystem::path> PickAssetFileForImport(AssetLibrary::Category category)
{
    const wchar_t* filter = L"All files (*.*)\0*.*\0\0";
    const wchar_t* title = L"Import Asset";
    switch (category)
    {
    case AssetLibrary::Category::Texture:
        filter = L"Image Files (*.png;*.jpg;*.jpeg;*.tga;*.dds)\0*.png;*.jpg;*.jpeg;*.tga;*.dds\0All files (*.*)\0*.*\0\0";
        title = L"Import Texture";
        break;
    case AssetLibrary::Category::Model:
        filter = L"Model Files (*.glb;*.gltf;*.obj;*.fbx)\0*.glb;*.gltf;*.obj;*.fbx\0All files (*.*)\0*.*\0\0";
        title = L"Import Model";
        break;
    case AssetLibrary::Category::Animation:
        filter = L"Animation Files (*.ozz;*.glb;*.gltf)\0*.ozz;*.glb;*.gltf\0All files (*.*)\0*.*\0\0";
        title = L"Import Animation";
        break;
    case AssetLibrary::Category::Material:
        filter = L"Material Files (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
        title = L"Import Material";
        break;
    case AssetLibrary::Category::WaterMaterial:
        filter = L"Water Materials (*.watermat;*.json)\0*.watermat;*.json\0All files (*.*)\0*.*\0\0";
        title = L"Import Water Material";
        break;
    case AssetLibrary::Category::Scene:
        filter = L"Scene Files (*.scene)\0*.scene\0All files (*.*)\0*.*\0\0";
        title = L"Import Scene";
        break;
    }

    std::array<wchar_t, 32768> file{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return std::nullopt;
    return std::filesystem::path(file.data());
}

std::optional<std::filesystem::path> FindEditorFont(const char* filename)
{
    const std::filesystem::path candidates[] = {
        std::filesystem::path("assets") / "fonts" / filename,
        std::filesystem::path("Client") / "assets" / "fonts" / filename,
        std::filesystem::path("..") / ".." / ".." / "assets" / "fonts" / filename,
    };
    for (const std::filesystem::path& path : candidates)
    {
        if (std::filesystem::exists(path))
            return path;
    }
    return std::nullopt;
}

void LoadEditorFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    if (auto path = FindEditorFont("Inter-Regular.ttf"))
    {
        regular = io.Fonts->AddFontFromFileTTF(path->generic_string().c_str(), 16.0f);
        Tracen("[EDITOR-VISUAL] Loaded font: Inter-Regular.ttf (16px)");
    }
    if (!regular)
    {
        regular = io.Fonts->AddFontDefault();
        TraceError("[EDITOR-VISUAL] Inter-Regular.ttf missing; using ImGui default font");
    }

    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    if (auto path = FindEditorFont("fa-solid-900.ttf"))
    {
        ImFontConfig iconsConfig;
        iconsConfig.MergeMode = true;
        iconsConfig.PixelSnapH = true;
        iconsConfig.GlyphMinAdvanceX = 16.0f;
        io.Fonts->AddFontFromFileTTF(path->generic_string().c_str(), 14.0f, &iconsConfig, iconRanges);
        Tracen("[EDITOR-VISUAL] Loaded font: fa-solid-900.ttf (14px, merged)");
    }
    else
    {
        TraceError("[EDITOR-VISUAL] fa-solid-900.ttf missing; editor icons will fall back to text");
    }

    if (auto path = FindEditorFont("Inter-Bold.ttf"))
    {
        bold = io.Fonts->AddFontFromFileTTF(path->generic_string().c_str(), 18.0f);
        Tracen("[EDITOR-VISUAL] Loaded font: Inter-Bold.ttf (18px)");
    }
    if (!bold)
        bold = regular;
    if (bold)
    {
        if (auto path = FindEditorFont("fa-solid-900.ttf"))
        {
            ImFontConfig iconsConfig;
            iconsConfig.MergeMode = true;
            iconsConfig.PixelSnapH = true;
            iconsConfig.GlyphMinAdvanceX = 16.0f;
            io.Fonts->AddFontFromFileTTF(path->generic_string().c_str(), 16.0f, &iconsConfig, iconRanges);
        }
    }

    io.FontDefault = regular;
    UI::SetEditorFonts({regular, bold});
}

ImVec4 ColorU8(int r, int g, int b, int a = 255)
{
    const auto toLinear = [](int value) {
        const float srgb = static_cast<float>(value) / 255.0f;
        return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    };
    return ImVec4(
        toLinear(r),
        toLinear(g),
        toLinear(b),
        static_cast<float>(a) / 255.0f);
}

void ApplyEditorStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;

    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 18.0f;
    style.CellPadding = ImVec2(5.0f, 3.0f);
    style.GrabMinSize = 11.0f;
    style.ScrollbarSize = 12.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ColorU8(27, 29, 33);
    colors[ImGuiCol_ChildBg] = ColorU8(33, 36, 41);
    colors[ImGuiCol_PopupBg] = ColorU8(30, 33, 37, 248);
    colors[ImGuiCol_MenuBarBg] = ColorU8(30, 32, 36);
    colors[ImGuiCol_TitleBg] = ColorU8(24, 26, 30);
    colors[ImGuiCol_TitleBgActive] = ColorU8(32, 35, 40);
    colors[ImGuiCol_TitleBgCollapsed] = ColorU8(22, 24, 28);
    colors[ImGuiCol_FrameBg] = ColorU8(42, 46, 52);
    colors[ImGuiCol_FrameBgHovered] = ColorU8(51, 56, 64);
    colors[ImGuiCol_FrameBgActive] = ColorU8(58, 65, 75);
    colors[ImGuiCol_Button] = ColorU8(46, 51, 58);
    colors[ImGuiCol_ButtonHovered] = ColorU8(58, 65, 75);
    colors[ImGuiCol_ButtonActive] = ColorU8(69, 77, 88);
    colors[ImGuiCol_Header] = ColorU8(38, 42, 48);
    colors[ImGuiCol_HeaderHovered] = ColorU8(49, 55, 64);
    colors[ImGuiCol_HeaderActive] = ColorU8(58, 65, 75);
    colors[ImGuiCol_Tab] = ColorU8(36, 40, 46);
    colors[ImGuiCol_TabHovered] = ColorU8(52, 58, 66);
    colors[ImGuiCol_TabActive] = ColorU8(46, 51, 58);
    colors[ImGuiCol_TabUnfocused] = ColorU8(30, 33, 38);
    colors[ImGuiCol_TabUnfocusedActive] = ColorU8(39, 43, 49);
    colors[ImGuiCol_CheckMark] = ColorU8(61, 126, 219);
    colors[ImGuiCol_SliderGrab] = ColorU8(61, 126, 219);
    colors[ImGuiCol_SliderGrabActive] = ColorU8(91, 155, 232);
    colors[ImGuiCol_Separator] = ColorU8(46, 50, 58);
    colors[ImGuiCol_SeparatorHovered] = ColorU8(61, 126, 219, 204);
    colors[ImGuiCol_SeparatorActive] = ColorU8(91, 155, 232);
    colors[ImGuiCol_Border] = ColorU8(52, 56, 63);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_Text] = ColorU8(213, 216, 221);
    colors[ImGuiCol_TextDisabled] = ColorU8(107, 112, 121);
    colors[ImGuiCol_DragDropTarget] = ColorU8(61, 126, 219, 204);
    colors[ImGuiCol_ScrollbarBg] = ColorU8(27, 29, 33);
    colors[ImGuiCol_ScrollbarGrab] = ColorU8(58, 62, 70);
    colors[ImGuiCol_ScrollbarGrabHovered] = ColorU8(73, 79, 88);
    colors[ImGuiCol_ScrollbarGrabActive] = ColorU8(91, 99, 110);
    colors[ImGuiCol_ResizeGrip] = ColorU8(61, 126, 219, 64);
    colors[ImGuiCol_ResizeGripHovered] = ColorU8(61, 126, 219, 128);
    colors[ImGuiCol_ResizeGripActive] = ColorU8(91, 155, 232, 190);
    colors[ImGuiCol_TableHeaderBg] = ColorU8(38, 42, 48);
    colors[ImGuiCol_TableBorderStrong] = ColorU8(52, 56, 63);
    colors[ImGuiCol_TableBorderLight] = ColorU8(46, 50, 58);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = ColorU8(255, 255, 255, 10);
    colors[ImGuiCol_NavHighlight] = ColorU8(61, 126, 219, 190);
    colors[ImGuiCol_DockingPreview] = ColorU8(61, 126, 219, 102);
    colors[ImGuiCol_DockingEmptyBg] = ColorU8(27, 29, 33);

    Tracen("[EDITOR-VISUAL] Dark compact editor style applied");
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
    m_physicalDevice = device.GetPhysicalDevice();
    m_graphicsQueue = device.GetGraphicsQueue();
    m_graphicsQueueFamily = device.GetGraphicsQueueFamily();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = kLayoutFile;
    m_applyDefaultDockLayout = !std::filesystem::exists(kLayoutFile);

    LoadEditorFonts();
    ApplyEditorStyle();

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
    if (!m_applyDefaultDockLayout)
        Tracen("[EDITOR-LAYOUT] Loaded layout from editor_layout.ini");
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

void EditorImGui::SetMapEditorSettings(const MapEditorSettings& settings)
{
    m_editorSettings = settings;
}

void EditorImGui::SetEditorPlayModeState(const EditorPlayModeState& state)
{
    m_playModeState = state;
    if (!CanUseEditorTools())
    {
        m_editorSettings.toolMode = MapEditorToolMode::None;
        m_editorSettings.waterSculptActive = false;
    }
}

void EditorImGui::SetLightingState(const LightingState& state)
{
    m_lightingState = state;
}

void EditorImGui::SetDynamicLightEditorState(const DynamicLightEditorState& state)
{
    m_dynamicLightState = state;
}

void EditorImGui::SetWaterBodyEditorState(const WaterBodyEditorState& state)
{
    m_waterBodyState = state;
}

void EditorImGui::SetMeshRendererEditorState(const MeshRendererEditorState& state)
{
    m_meshRendererState = state;
}

void EditorImGui::SetTerrainEditorState(const TerrainEditorState& state)
{
    m_terrainState = state;
}

void EditorImGui::SetEngineStats(const EngineStats& stats)
{
    m_engineStats = stats;
}

void EditorImGui::SetHierarchySceneState(std::uint64_t sceneRootEntity,
                                         std::string sceneRootName,
                                         std::vector<HierarchySceneEntity> entities)
{
    m_sceneRootEntity = sceneRootEntity;
    m_sceneRootName = sceneRootName.empty() ? "Untitled" : std::move(sceneRootName);
    m_hierarchyEntities = std::move(entities);
    m_selectedHierarchyEntity = 0;
    for (const HierarchySceneEntity& entity : m_hierarchyEntities)
    {
        if (entity.selected)
        {
            m_selectedHierarchyEntity = entity.entity;
            break;
        }
    }
}

void EditorImGui::SetWaterMaterials(std::vector<std::pair<std::string, WaterMaterialData>> materials)
{
    m_waterMaterials = std::move(materials);
}

void EditorImGui::SetPaletteSlots(const std::array<MapEditorPaletteSlot, 8>& slots)
{
    m_paletteSlots = slots;
    m_editorSettings.textureSlot = std::min<std::uint32_t>(m_editorSettings.textureSlot, 7u);
}

void EditorImGui::SetWaterMaterialUsageCounts(std::vector<std::pair<std::string, std::uint32_t>> usageCounts)
{
    m_waterMaterialUsageCounts.clear();
    for (const auto& usage : usageCounts)
        m_waterMaterialUsageCounts[usage.first] = usage.second;
}

std::vector<std::pair<std::string, WaterMaterialData>> EditorImGui::GetWaterMaterialsSnapshot() const
{
    std::vector<std::pair<std::string, WaterMaterialData>> materials;
    if (!m_assetLibrary)
        return m_waterMaterials;

    auto texturePathForId = [this](const std::string& textureId) -> std::string {
        if (textureId.empty() || !m_assetLibrary)
            return {};
        auto texture = m_assetLibrary->FindById(textureId);
        if (!texture || texture->category != AssetLibrary::Category::Texture)
            return {};
        return m_assetLibrary->AssetRelativePath(*texture);
    };

    for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
    {
        if (entry.category == AssetLibrary::Category::WaterMaterial && !entry.id.empty())
        {
            WaterMaterialData material = entry.waterMaterial;
            if (m_waterMaterialEditor.windowOpen &&
                m_waterMaterialEditor.materialId == entry.id &&
                m_waterMaterialEditor.dirty)
            {
                material = m_waterMaterialEditor.draft;
            }
            materials.push_back({entry.id, material});
        }
        else if (entry.category == AssetLibrary::Category::Material && !entry.id.empty())
        {
            const AssetLibrary::MaterialData& source =
                (m_pbrMaterialEditor.windowOpen && m_pbrMaterialEditor.materialId == entry.id && m_pbrMaterialEditor.dirty)
                    ? m_pbrMaterialEditor.draft
                    : entry.material;
            WaterMaterialData converted{};
            converted.diffuseMap = texturePathForId(source.diffuseTextureId);
            converted.normalMapA = texturePathForId(source.normalTextureId);
            converted.normalMapB = converted.normalMapA;
            converted.normalTiling = std::clamp((source.tilingScaleX + source.tilingScaleY) * 0.5f, 0.001f, 100.0f);
            converted.scrollSpeedA[0] = 0.03f;
            converted.scrollSpeedA[1] = 0.014f;
            converted.scrollSpeedB[0] = -0.015f;
            converted.scrollSpeedB[1] = 0.02f;
            converted.config.normalStrength = std::clamp(source.normalStrength, 0.0f, 2.0f);
            materials.push_back({entry.id, converted});
        }
    }
    return materials;
}

void EditorImGui::SyncWaterMaterialSnapshot()
{
    m_waterMaterials = GetWaterMaterialsSnapshot();
}

void EditorImGui::SetEngineRoot(const std::filesystem::path& clientRoot)
{
    m_engineRoot = clientRoot;
    if (m_projectBrowserPath.empty())
        m_projectBrowserPath = InitialProjectBrowserPath(clientRoot);
    if (m_projectParentBuffer[0] == '\0')
        CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
}

void EditorImGui::InitializeAssetLibrary(const std::filesystem::path& clientRoot)
{
    SetEngineRoot(clientRoot);
    DestroyAssetPreviewTextures();
    m_assetLibrary = std::make_unique<AssetLibrary>(clientRoot);
    if (!m_assetLibrary->Initialize())
    {
        m_assetLibrary.reset();
        m_assetStatus = "Asset library init failed";
        TraceError("[EDITOR-IMGUI-3] Asset library initialization failed");
        return;
    }

    m_assetStatus = "Asset library ready";
    SyncWaterMaterialSnapshot();
    Tracenf("[EDITOR-IMGUI-3] Asset library root=%s", m_assetLibrary->LibraryRoot().generic_string().c_str());
}

void EditorImGui::InitializeProjectAssetLibrary(const std::filesystem::path& projectRoot,
                                                const std::filesystem::path& assetRoot)
{
    DestroyAssetPreviewTextures();
    m_assetLibrary = std::make_unique<AssetLibrary>(projectRoot, assetRoot);
    if (!m_assetLibrary->Initialize())
    {
        m_assetLibrary.reset();
        m_assetStatus = "Project asset library init failed";
        TraceError("[PROJECT] asset library initialization failed: %s", assetRoot.generic_string().c_str());
        return;
    }

    m_assetFilter = AssetBrowserFilter::All;
    m_assetSubpath.clear();
    m_selectedAssetId.clear();
    m_activeAssetTags.clear();
    m_assetStatus = "Project assets ready";
    SyncWaterMaterialSnapshot();
    Tracenf("[PROJECT] asset browser root=%s", m_assetLibrary->LibraryRoot().generic_string().c_str());
}

void EditorImGui::ReleaseSceneViewTextureDescriptor()
{
    if (m_sceneViewDescriptor && m_vulkanBackendReady)
        ImGui_ImplVulkan_RemoveTexture(m_sceneViewDescriptor);
    m_sceneViewDescriptor = VK_NULL_HANDLE;
    m_sceneViewDescriptorSampler = VK_NULL_HANDLE;
    m_sceneViewDescriptorImageView = VK_NULL_HANDLE;
    m_sceneViewDescriptorImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

void EditorImGui::SetSceneViewTexture(VkSampler sampler,
                                      VkImageView imageView,
                                      VkImageLayout layout,
                                      VkExtent2D extent)
{
    m_sceneViewSampler = sampler;
    m_sceneViewImageView = imageView;
    m_sceneViewImageLayout = layout;
    m_sceneViewExtent = extent;

    const bool descriptorMatches =
        m_sceneViewDescriptor &&
        m_sceneViewDescriptorSampler == sampler &&
        m_sceneViewDescriptorImageView == imageView &&
        m_sceneViewDescriptorImageLayout == layout;
    if (descriptorMatches)
        return;

    ReleaseSceneViewTextureDescriptor();
    if (!m_vulkanBackendReady || !sampler || !imageView || layout == VK_IMAGE_LAYOUT_UNDEFINED)
        return;

    m_sceneViewDescriptor = ImGui_ImplVulkan_AddTexture(sampler, imageView, layout);
    if (m_sceneViewDescriptor)
    {
        m_sceneViewDescriptorSampler = sampler;
        m_sceneViewDescriptorImageView = imageView;
        m_sceneViewDescriptorImageLayout = layout;
        Tracenf("[EDITOR-SCENE-VIEW] bound offscreen texture extent=%ux%u",
            extent.width,
            extent.height);
    }
}

MapEditorCommands EditorImGui::ConsumeCommands()
{
    MapEditorCommands commands = m_commands;
    m_commands = {};
    if (!CanUseEditorTools())
    {
        const bool enterPlayMode = commands.enterPlayMode;
        const bool exitPlayMode = commands.exitPlayMode;
        const bool pausePlayMode = commands.pausePlayMode;
        const bool resumePlayMode = commands.resumePlayMode;
        commands = {};
        commands.enterPlayMode = enterPlayMode;
        commands.exitPlayMode = exitPlayMode;
        commands.pausePlayMode = pausePlayMode;
        commands.resumePlayMode = resumePlayMode;
    }
    return commands;
}

void EditorImGui::RefreshAssetLibrary()
{
    if (!m_assetLibrary)
        return;

    std::string error;
    if (!m_assetLibrary->Refresh(error))
    {
        m_assetStatus = "Refresh failed: " + error;
        return;
    }
    m_assetStatus = "Assets refreshed";
    SyncWaterMaterialSnapshot();
    Tracen("[EDITOR-IMGUI-3] Asset Browser refreshed");
}

bool EditorImGui::ActiveAssetCategory(AssetLibrary::Category category) const
{
    switch (m_assetFilter)
    {
    case AssetBrowserFilter::All: return true;
    case AssetBrowserFilter::Texture: return category == AssetLibrary::Category::Texture;
    case AssetBrowserFilter::Model: return category == AssetLibrary::Category::Model;
    case AssetBrowserFilter::Animation: return category == AssetLibrary::Category::Animation;
    case AssetBrowserFilter::Material: return category == AssetLibrary::Category::Material;
    case AssetBrowserFilter::WaterMaterial: return category == AssetLibrary::Category::WaterMaterial;
    case AssetBrowserFilter::Scene: return category == AssetLibrary::Category::Scene;
    default: return true;
    }
}

AssetLibrary::Category EditorImGui::FolderCategory() const
{
    switch (m_assetFilter)
    {
    case AssetBrowserFilter::Model: return AssetLibrary::Category::Model;
    case AssetBrowserFilter::Animation: return AssetLibrary::Category::Animation;
    case AssetBrowserFilter::Material: return AssetLibrary::Category::Material;
    case AssetBrowserFilter::WaterMaterial: return AssetLibrary::Category::WaterMaterial;
    case AssetBrowserFilter::Scene: return AssetLibrary::Category::Scene;
    case AssetBrowserFilter::All:
    case AssetBrowserFilter::Texture:
    default:
        return AssetLibrary::Category::Texture;
    }
}

const char* EditorImGui::AssetFilterName() const
{
    switch (m_assetFilter)
    {
    case AssetBrowserFilter::All: return "All";
    case AssetBrowserFilter::Texture: return "Textures";
    case AssetBrowserFilter::Model: return "Models";
    case AssetBrowserFilter::Animation: return "Anims";
    case AssetBrowserFilter::Material: return "Materials";
    case AssetBrowserFilter::WaterMaterial: return "Water Mats";
    case AssetBrowserFilter::Scene: return "Scenes";
    default: return "Assets";
    }
}

bool EditorImGui::AssetPassesCurrentFilters(const AssetLibrary::Entry& entry) const
{
    if (!ActiveAssetCategory(entry.category))
        return false;
    if (AssetLibrary::NormalizeSubpath(entry.subpath) != AssetLibrary::NormalizeSubpath(m_assetSubpath))
        return false;

    for (const std::string& tag : m_activeAssetTags)
    {
        if (std::find(entry.tags.begin(), entry.tags.end(), tag) == entry.tags.end())
            return false;
    }

    const std::string search = m_assetSearchBuffer;
    if (!search.empty() &&
        !ContainsCaseInsensitive(entry.displayName, search) &&
        !ContainsCaseInsensitive(entry.filename, search) &&
        !ContainsCaseInsensitive(AssetLibrary::CategoryName(entry.category), search) &&
        !ContainsCaseInsensitive(AssetLibrary::TextureRoleName(entry.textureRole), search) &&
        !ContainsCaseInsensitive(AssetLibrary::TagsToCsv(entry.tags), search))
    {
        return false;
    }
    return true;
}

std::vector<AssetLibrary::Entry> EditorImGui::QueryVisibleAssets() const
{
    std::vector<AssetLibrary::Entry> result;
    if (m_assetLibrary)
    {
        for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
        {
            if (AssetPassesCurrentFilters(entry))
                result.push_back(entry);
        }
    }
    for (const AssetLibrary::Entry& entry : QuerySceneAssets())
    {
        if (AssetPassesCurrentFilters(entry))
            result.push_back(entry);
    }

    std::sort(result.begin(), result.end(), [](const AssetLibrary::Entry& a, const AssetLibrary::Entry& b) {
        if (a.category != b.category)
            return static_cast<int>(a.category) < static_cast<int>(b.category);
        return ToLowerAscii(a.displayName) < ToLowerAscii(b.displayName);
    });
    return result;
}

std::vector<std::string> EditorImGui::QueryVisibleFolders() const
{
    std::set<std::string> folders;
    if (!m_assetLibrary)
        return {};

    const AssetLibrary::Category categories[] = {
        AssetLibrary::Category::Texture,
        AssetLibrary::Category::Model,
        AssetLibrary::Category::Animation,
        AssetLibrary::Category::Material,
        AssetLibrary::Category::WaterMaterial,
        AssetLibrary::Category::Scene,
    };
    for (AssetLibrary::Category category : categories)
    {
        if (!ActiveAssetCategory(category))
            continue;
        if (category == AssetLibrary::Category::Scene)
        {
            for (const AssetLibrary::Entry& entry : QuerySceneAssets())
            {
                if (!entry.subpath.empty())
                    folders.insert(AssetLibrary::NormalizeSubpath(entry.subpath));
            }
        }
        else
        {
            for (const std::string& folder : m_assetLibrary->FolderSubpathsFor(category))
                folders.insert(AssetLibrary::NormalizeSubpath(folder));
        }
    }
    return {folders.begin(), folders.end()};
}

std::vector<std::pair<std::string, std::uint32_t>> EditorImGui::QueryVisibleTags() const
{
    std::map<std::string, std::uint32_t> counts;
    if (!m_assetLibrary)
        return {};

    for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
    {
        if (!ActiveAssetCategory(entry.category))
            continue;
        if (AssetLibrary::NormalizeSubpath(entry.subpath) != AssetLibrary::NormalizeSubpath(m_assetSubpath))
            continue;
        for (const std::string& tag : entry.tags)
            ++counts[tag];
    }
    for (const AssetLibrary::Entry& entry : QuerySceneAssets())
    {
        if (!ActiveAssetCategory(entry.category))
            continue;
        if (AssetLibrary::NormalizeSubpath(entry.subpath) != AssetLibrary::NormalizeSubpath(m_assetSubpath))
            continue;
        for (const std::string& tag : entry.tags)
            ++counts[tag];
    }

    std::vector<std::pair<std::string, std::uint32_t>> result(counts.begin(), counts.end());
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second)
            return a.second > b.second;
        return a.first < b.first;
    });
    return result;
}

void EditorImGui::DestroyAssetPreviewTexture(AssetPreviewTexture& texture)
{
    if (texture.descriptor)
        ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
    if (texture.sampler)
        vkDestroySampler(m_device, texture.sampler, nullptr);
    if (texture.view)
        vkDestroyImageView(m_device, texture.view, nullptr);
    if (texture.image)
        vkDestroyImage(m_device, texture.image, nullptr);
    if (texture.memory)
        vkFreeMemory(m_device, texture.memory, nullptr);
    texture = {};
}

void EditorImGui::DestroyAssetPreviewTextures()
{
    if (m_device)
        vkDeviceWaitIdle(m_device);
    for (auto& preview : m_assetPreviewTextures)
        DestroyAssetPreviewTexture(preview.second);
    m_assetPreviewTextures.clear();
}

std::optional<std::filesystem::path> EditorImGui::AssetPreviewPathFor(const AssetLibrary::Entry& entry) const
{
    if (!m_assetLibrary)
        return std::nullopt;

    const auto thumbnailPath = [this](const AssetLibrary::Entry& textureEntry) -> std::optional<std::filesystem::path> {
        if (textureEntry.thumbnail.empty() ||
            textureEntry.thumbnail == "model_icon" ||
            textureEntry.thumbnail == "animation_icon" ||
            textureEntry.thumbnail == "material_icon" ||
            textureEntry.thumbnail == "water_material_icon")
        {
            return std::nullopt;
        }

        const std::filesystem::path path = m_assetLibrary->LibraryRoot() / textureEntry.thumbnail;
        if (!std::filesystem::exists(path))
            return std::nullopt;
        return path;
    };

    if (entry.category == AssetLibrary::Category::Texture)
        return thumbnailPath(entry);

    if (entry.category == AssetLibrary::Category::Material)
    {
        const std::string ids[] = {
            entry.material.diffuseTextureId,
            entry.material.normalTextureId,
            entry.material.aoTextureId,
            entry.material.roughnessTextureId,
            entry.material.metallicTextureId,
            entry.material.heightTextureId,
        };
        for (const std::string& textureId : ids)
        {
            if (textureId.empty())
                continue;
            auto textureEntry = m_assetLibrary->FindById(textureId);
            if (textureEntry)
            {
                if (auto path = thumbnailPath(*textureEntry))
                    return path;
            }
        }
    }

    return std::nullopt;
}

bool EditorImGui::LoadAssetPreviewTexture(const std::filesystem::path& path, AssetPreviewTexture& outTexture)
{
    if (!m_device || !m_physicalDevice || !m_graphicsQueue || m_graphicsQueueFamily == UINT32_MAX)
        return false;

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0)
    {
        if (decoded)
            stbi_image_free(decoded);
        TraceError("[EDITOR-IMGUI] Failed to decode asset thumbnail: %s", path.string().c_str());
        return false;
    }

    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkCommandPool uploadPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    auto findMemoryType = [this](uint32_t typeFilter, VkMemoryPropertyFlags properties) -> std::optional<uint32_t> {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        {
            if ((typeFilter & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }
        return std::nullopt;
    };

    auto cleanupUpload = [&]() {
        if (uploadPool)
            vkDestroyCommandPool(m_device, uploadPool, nullptr);
        if (stagingBuffer)
            vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        if (stagingMemory)
            vkFreeMemory(m_device, stagingMemory, nullptr);
        stbi_image_free(decoded);
    };

    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = byteSize;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!CheckEditorVk(vkCreateBuffer(m_device, &buffer, nullptr, &stagingBuffer), "vkCreateBuffer"))
    {
        cleanupUpload();
        return false;
    }

    VkMemoryRequirements stagingReq{};
    vkGetBufferMemoryRequirements(m_device, stagingBuffer, &stagingReq);
    const auto stagingMemoryType = findMemoryType(stagingReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!stagingMemoryType)
    {
        cleanupUpload();
        return false;
    }

    VkMemoryAllocateInfo stagingAlloc{};
    stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAlloc.allocationSize = stagingReq.size;
    stagingAlloc.memoryTypeIndex = *stagingMemoryType;
    if (!CheckEditorVk(vkAllocateMemory(m_device, &stagingAlloc, nullptr, &stagingMemory), "vkAllocateMemory(staging)") ||
        !CheckEditorVk(vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0), "vkBindBufferMemory"))
    {
        cleanupUpload();
        return false;
    }

    void* mapped = nullptr;
    if (!CheckEditorVk(vkMapMemory(m_device, stagingMemory, 0, byteSize, 0, &mapped), "vkMapMemory"))
    {
        cleanupUpload();
        return false;
    }
    std::memcpy(mapped, decoded, static_cast<size_t>(byteSize));
    vkUnmapMemory(m_device, stagingMemory);

    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!CheckEditorVk(vkCreateImage(m_device, &image, nullptr, &outTexture.image), "vkCreateImage"))
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    VkMemoryRequirements imageReq{};
    vkGetImageMemoryRequirements(m_device, outTexture.image, &imageReq);
    const auto imageMemoryType = findMemoryType(imageReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!imageMemoryType)
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    VkMemoryAllocateInfo imageAlloc{};
    imageAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAlloc.allocationSize = imageReq.size;
    imageAlloc.memoryTypeIndex = *imageMemoryType;
    if (!CheckEditorVk(vkAllocateMemory(m_device, &imageAlloc, nullptr, &outTexture.memory), "vkAllocateMemory(image)") ||
        !CheckEditorVk(vkBindImageMemory(m_device, outTexture.image, outTexture.memory, 0), "vkBindImageMemory"))
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
    if (!CheckEditorVk(vkCreateCommandPool(m_device, &poolInfo, nullptr, &uploadPool), "vkCreateCommandPool"))
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = uploadPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    if (!CheckEditorVk(vkAllocateCommandBuffers(m_device, &cmdAlloc, &cmd), "vkAllocateCommandBuffers"))
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!CheckEditorVk(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer"))
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    TransitionPreviewImage(cmd, outTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, outTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    TransitionPreviewImage(cmd, outTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (!CheckEditorVk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer"))
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (!CheckEditorVk(vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit") ||
        !CheckEditorVk(vkQueueWaitIdle(m_graphicsQueue), "vkQueueWaitIdle"))
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = outTexture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (!CheckEditorVk(vkCreateImageView(m_device, &view, nullptr, &outTexture.view), "vkCreateImageView"))
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.0f;
    if (!CheckEditorVk(vkCreateSampler(m_device, &sampler, nullptr, &outTexture.sampler), "vkCreateSampler"))
    {
        cleanupUpload();
        DestroyAssetPreviewTexture(outTexture);
        return false;
    }

    outTexture.descriptor = ImGui_ImplVulkan_AddTexture(outTexture.sampler, outTexture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    outTexture.width = static_cast<uint32_t>(width);
    outTexture.height = static_cast<uint32_t>(height);
    cleanupUpload();
    Tracenf("[EDITOR-ASSET-PREVIEW] Loaded thumbnail: %s %dx%d", path.string().c_str(), width, height);
    return outTexture.descriptor != VK_NULL_HANDLE;
}

EditorImGui::AssetPreviewTexture* EditorImGui::GetAssetPreviewTexture(const AssetLibrary::Entry& entry)
{
    auto path = AssetPreviewPathFor(entry);
    if (!path)
        return nullptr;

    const std::string key = path->generic_string();
    auto [it, inserted] = m_assetPreviewTextures.try_emplace(key);
    if (inserted)
    {
        if (!LoadAssetPreviewTexture(*path, it->second))
            it->second.failed = true;
    }
    return it->second.failed || !it->second.descriptor ? nullptr : &it->second;
}

void EditorImGui::CreateAssetFolder()
{
    if (!m_assetLibrary)
        return;
    std::string outSubpath;
    std::string error;
    if (!m_assetLibrary->CreateFolder(FolderCategory(), m_assetSubpath, m_newAssetFolderName, outSubpath, error))
    {
        m_assetStatus = "Folder create failed: " + error;
        return;
    }
    m_assetSubpath = outSubpath;
    m_newAssetFolderName[0] = '\0';
    m_assetStatus = "Folder created: " + FolderDisplayName(outSubpath);
    Tracenf("[EDITOR-IMGUI-3] Folder created: %s", outSubpath.c_str());
}

void EditorImGui::DeleteAssetFolder()
{
    if (!m_assetLibrary || m_assetSubpath.empty())
        return;

    std::uint32_t removedAssets = 0;
    std::string error;
    const std::string deleted = m_assetSubpath;
    if (!m_assetLibrary->DeleteFolder(FolderCategory(), m_assetSubpath, removedAssets, error))
    {
        m_assetStatus = "Folder delete failed: " + error;
        return;
    }
    m_assetSubpath = ParentSubpath(deleted);
    m_assetStatus = "Folder deleted, removed assets: " + std::to_string(removedAssets);
    Tracenf("[EDITOR-IMGUI-3] Folder deleted: %s removed=%u", deleted.c_str(), removedAssets);
}

void EditorImGui::DeleteAsset(const AssetLibrary::Entry& entry)
{
    if (!m_assetLibrary)
        return;

    const std::string deletedId = entry.id;
    const std::string deletedName = entry.displayName;
    std::string error;
    if (!m_assetLibrary->Remove(entry.id, error))
    {
        m_assetStatus = "Delete failed: " + error;
        return;
    }

    if (m_selectedAssetId == deletedId)
        m_selectedAssetId.clear();
    if (m_waterMaterialEditor.materialId == deletedId)
        m_waterMaterialEditor = {};
    if (m_pbrMaterialEditor.materialId == deletedId)
        m_pbrMaterialEditor = {};
    SyncWaterMaterialSnapshot();
    m_assetStatus = "Deleted asset: " + deletedName;
    Tracenf("[EDITOR-IMGUI-3] Asset deleted: asset_id=%s type=%s",
        deletedId.c_str(),
        AssetLibrary::CategoryName(entry.category));
}

void EditorImGui::SetToolMode(MapEditorToolMode mode)
{
    if (!CanUseEditorTools())
    {
        m_editorSettings.toolMode = MapEditorToolMode::None;
        m_editorSettings.waterSculptActive = false;
        return;
    }
    if (m_editorSettings.toolMode == mode)
        return;
    const MapEditorToolMode previous = m_editorSettings.toolMode;
    m_editorSettings.toolMode = mode;
    m_editorSettings.waterSculptActive = mode == MapEditorToolMode::WaterSculpt;
    if (mode == MapEditorToolMode::SplatPaint)
        m_editorSettings.tool = MapEditorTool::Paint;
    else if (mode == MapEditorToolMode::None)
        m_editorSettings.waterSculptActive = false;
    Tracenf("[EDITOR-IMGUI-5] Tool mode changed: %d -> %d",
        static_cast<int>(previous),
        static_cast<int>(mode));
}

bool EditorImGui::CanUseEditorTools() const
{
    return m_playModeState.mode == EditorPlayMode::Edit;
}

MapEditorPaletteSlot EditorImGui::BuildPaletteSlotFromAsset(std::uint32_t slotIndex, const AssetLibrary::Entry& entry) const
{
    MapEditorPaletteSlot slot{};
    slot.slot = std::min<std::uint32_t>(slotIndex, 7u);
    slot.assetId = entry.id;
    slot.displayName = entry.displayName;

    if (!m_assetLibrary)
        return slot;

    auto texturePathForId = [this](const std::string& textureId) -> std::string {
        if (textureId.empty() || !m_assetLibrary)
            return {};
        auto texture = m_assetLibrary->FindById(textureId);
        if (!texture || texture->category != AssetLibrary::Category::Texture)
            return {};
        return m_assetLibrary->AssetRelativePath(*texture);
    };

    if (entry.category == AssetLibrary::Category::Texture)
    {
        slot.texturePath = m_assetLibrary->AssetRelativePath(entry);
    }
    else if (entry.category == AssetLibrary::Category::Material)
    {
        slot.texturePath = texturePathForId(entry.material.diffuseTextureId);
        slot.normalTexturePath = texturePathForId(entry.material.normalTextureId);
        slot.aoTexturePath = texturePathForId(entry.material.aoTextureId);
        slot.roughnessTexturePath = texturePathForId(entry.material.roughnessTextureId);
        slot.metallicTexturePath = texturePathForId(entry.material.metallicTextureId);
        slot.heightTexturePath = texturePathForId(entry.material.heightTextureId);
        slot.tilingScaleX = entry.material.tilingScaleX;
        slot.tilingScaleY = entry.material.tilingScaleY;
        slot.colorTint[0] = entry.material.colorTint[0];
        slot.colorTint[1] = entry.material.colorTint[1];
        slot.colorTint[2] = entry.material.colorTint[2];
        slot.normalStrength = entry.material.normalStrength;
        slot.aoStrength = entry.material.aoStrength;
        slot.roughnessStrength = entry.material.roughnessStrength;
        slot.metallicStrength = entry.material.metallicStrength;
    }
    return slot;
}

void EditorImGui::ImportAssetWithDialog(AssetLibrary::Category category)
{
    if (!m_assetLibrary)
        return;

    const auto path = PickAssetFileForImport(category);
    if (!path)
        return;

    AssetLibrary::ImportOptions options{};
    options.displayName = path->stem().string();
    options.subpath = m_assetSubpath;
    AssetLibrary::Entry entry{};
    std::string error;
    if (!m_assetLibrary->Import(category, *path, options, entry, error))
    {
        m_assetStatus = "Import failed: " + error;
        return;
    }

    m_selectedAssetId = entry.id;
    m_assetFilter = category == AssetLibrary::Category::Texture ? AssetBrowserFilter::Texture :
        category == AssetLibrary::Category::Model ? AssetBrowserFilter::Model :
        category == AssetLibrary::Category::Animation ? AssetBrowserFilter::Animation :
        category == AssetLibrary::Category::Material ? AssetBrowserFilter::Material :
        AssetBrowserFilter::WaterMaterial;
    RefreshAssetLibrary();
    m_assetStatus = "Imported: " + entry.displayName;
    Tracenf("[EDITOR-IMGUI-5] Asset import: type=%s path=%s",
        AssetLibrary::CategoryName(category),
        path->generic_string().c_str());
}

void EditorImGui::CreatePbrMaterialAsset()
{
    if (!m_assetLibrary)
        return;
    AssetLibrary::ImportOptions options{};
    options.displayName = "material";
    options.subpath = m_assetSubpath;
    options.tags = {"material"};
    AssetLibrary::Entry entry{};
    std::string error;
    if (!m_assetLibrary->CreateMaterial(options, {}, entry, error))
    {
        m_assetStatus = "Material create failed: " + error;
        return;
    }
    m_assetFilter = AssetBrowserFilter::Material;
    m_selectedAssetId = entry.id;
    m_assetStatus = "Material created: " + entry.displayName;
    OpenPbrMaterialEditor(entry.id);
}

void EditorImGui::CreateWaterMaterialAsset()
{
    AssetLibrary::Entry entry{};
    if (CreateWaterMaterialAsset("Water_Material", entry))
        OpenWaterMaterialEditor(entry.id);
}

bool EditorImGui::CreateWaterMaterialAsset(const std::string& displayName, AssetLibrary::Entry& outEntry)
{
    if (!m_assetLibrary)
        return false;
    AssetLibrary::ImportOptions options{};
    options.displayName = displayName.empty() ? "Water_Material" : displayName;
    options.subpath = m_assetSubpath;
    options.tags = {"water", "material"};
    AssetLibrary::Entry entry{};
    std::string error;
    if (!m_assetLibrary->CreateWaterMaterial(options, {}, entry, error))
    {
        m_assetStatus = "Water material create failed: " + error;
        return false;
    }
    m_assetFilter = AssetBrowserFilter::WaterMaterial;
    m_selectedAssetId = entry.id;
    SyncWaterMaterialSnapshot();
    m_assetStatus = "Water material created: " + entry.displayName;
    Tracenf("[EDITOR-IMGUI-4] Material saved: id=%s name=%s",
        entry.id.c_str(),
        entry.displayName.c_str());
    outEntry = entry;
    return true;
}

bool EditorImGui::OpenWaterMaterialEditor(const std::string& materialId)
{
    if (!m_assetLibrary)
        return false;

    std::string id = materialId.empty() ? std::string("watermat_Default_Water") : materialId;
    auto entry = m_assetLibrary->FindById(id);
    if (!entry || entry->category != AssetLibrary::Category::WaterMaterial)
    {
        entry = m_assetLibrary->FindById("watermat_Default_Water");
        id = "watermat_Default_Water";
    }
    if (!entry || entry->category != AssetLibrary::Category::WaterMaterial)
    {
        m_assetStatus = "Water material not found: " + materialId;
        return false;
    }

    m_waterMaterialEditor.windowOpen = true;
    m_waterMaterialEditor.dirty = false;
    m_waterMaterialEditor.materialId = entry->id;
    m_waterMaterialEditor.draft = entry->waterMaterial;
    std::snprintf(m_waterMaterialEditor.name, sizeof(m_waterMaterialEditor.name), "%s", entry->displayName.c_str());
    m_assetStatus = "Editing water material: " + entry->displayName;
    Tracenf("[EDITOR-IMGUI-4] Water Material Editor opened: material_id=%s name=%s",
        entry->id.c_str(),
        entry->displayName.c_str());
    return true;
}

bool EditorImGui::OpenPbrMaterialEditor(const std::string& materialId)
{
    if (!m_assetLibrary)
        return false;

    auto entry = m_assetLibrary->FindById(materialId);
    if (!entry || entry->category != AssetLibrary::Category::Material)
    {
        m_assetStatus = "PBR material not found: " + materialId;
        return false;
    }

    m_pbrMaterialEditor.windowOpen = true;
    m_pbrMaterialEditor.dirty = false;
    m_pbrMaterialEditor.materialId = entry->id;
    m_pbrMaterialEditor.draft = entry->material;
    std::snprintf(m_pbrMaterialEditor.name, sizeof(m_pbrMaterialEditor.name), "%s", entry->displayName.c_str());
    m_assetStatus = "Editing material: " + entry->displayName;
    Tracenf("[EDITOR-IMGUI-4] PBR Material Editor opened: material_id=%s name=%s",
        entry->id.c_str(),
        entry->displayName.c_str());
    return true;
}

void EditorImGui::MarkWaterMaterialChanged(const char* field)
{
    if (m_waterMaterialEditor.materialId.empty())
        return;
    m_waterMaterialEditor.dirty = true;
    SceneManager::Instance().MarkDirty();
    SyncWaterMaterialSnapshot();
    Tracenf("[EDITOR-IMGUI-4] Material parameter changed: material_id=%s field=%s",
        m_waterMaterialEditor.materialId.c_str(),
        field ? field : "unknown");
}

void EditorImGui::MarkPbrMaterialChanged(const char* field)
{
    if (m_pbrMaterialEditor.materialId.empty())
        return;
    m_pbrMaterialEditor.dirty = true;
    SceneManager::Instance().MarkDirty();
    SyncWaterMaterialSnapshot();
    Tracenf("[EDITOR-IMGUI-4] Material parameter changed: material_id=%s field=%s",
        m_pbrMaterialEditor.materialId.c_str(),
        field ? field : "unknown");
}

bool EditorImGui::SaveWaterMaterialEditor()
{
    if (!m_assetLibrary || m_waterMaterialEditor.materialId.empty())
        return false;

    auto entry = m_assetLibrary->FindById(m_waterMaterialEditor.materialId);
    if (!entry || entry->category != AssetLibrary::Category::WaterMaterial)
        return false;

    AssetLibrary::Entry current = *entry;
    const std::string requestedName = m_waterMaterialEditor.name[0] != '\0'
        ? std::string(m_waterMaterialEditor.name)
        : current.displayName;
    if (requestedName != current.displayName)
    {
        AssetLibrary::Entry renamed{};
        std::string renameError;
        if (!m_assetLibrary->RenameAsset(current.id, requestedName, true, renamed, renameError))
        {
            m_assetStatus = "Water material rename failed: " + renameError;
            return false;
        }
        current = renamed;
        m_waterMaterialEditor.materialId = renamed.id;
    }

    AssetLibrary::Entry updated{};
    std::string error;
    if (!m_assetLibrary->UpdateWaterMaterial(current.id, m_waterMaterialEditor.draft, updated, error))
    {
        m_assetStatus = "Water material save failed: " + error;
        return false;
    }

    m_waterMaterialEditor.materialId = updated.id;
    m_waterMaterialEditor.draft = updated.waterMaterial;
    m_waterMaterialEditor.dirty = false;
    std::snprintf(m_waterMaterialEditor.name, sizeof(m_waterMaterialEditor.name), "%s", updated.displayName.c_str());
    m_selectedAssetId = updated.id;
    RefreshAssetLibrary();
    SyncWaterMaterialSnapshot();
    m_assetStatus = "Water material saved: " + updated.displayName;
    Tracenf("[EDITOR-IMGUI-4] Material saved: id=%s name=%s",
        updated.id.c_str(),
        updated.displayName.c_str());
    return true;
}

bool EditorImGui::SavePbrMaterialEditor()
{
    if (!m_assetLibrary || m_pbrMaterialEditor.materialId.empty())
        return false;

    auto entry = m_assetLibrary->FindById(m_pbrMaterialEditor.materialId);
    if (!entry || entry->category != AssetLibrary::Category::Material)
        return false;

    AssetLibrary::Entry current = *entry;
    const std::string requestedName = m_pbrMaterialEditor.name[0] != '\0'
        ? std::string(m_pbrMaterialEditor.name)
        : current.displayName;
    if (requestedName != current.displayName)
    {
        AssetLibrary::Entry renamed{};
        std::string renameError;
        if (!m_assetLibrary->RenameAsset(current.id, requestedName, true, renamed, renameError))
        {
            m_assetStatus = "Material rename failed: " + renameError;
            return false;
        }
        current = renamed;
        m_pbrMaterialEditor.materialId = renamed.id;
    }

    AssetLibrary::Entry updated{};
    std::string error;
    if (!m_assetLibrary->UpdateMaterial(current.id, m_pbrMaterialEditor.draft, updated, error))
    {
        m_assetStatus = "Material save failed: " + error;
        return false;
    }

    m_pbrMaterialEditor.materialId = updated.id;
    m_pbrMaterialEditor.draft = updated.material;
    m_pbrMaterialEditor.dirty = false;
    std::snprintf(m_pbrMaterialEditor.name, sizeof(m_pbrMaterialEditor.name), "%s", updated.displayName.c_str());
    m_selectedAssetId = updated.id;
    RefreshAssetLibrary();
    SyncWaterMaterialSnapshot();
    m_assetStatus = "Material saved: " + updated.displayName;
    Tracenf("[EDITOR-IMGUI-4] Material saved: id=%s name=%s",
        updated.id.c_str(),
        updated.displayName.c_str());
    return true;
}

bool EditorImGui::DeleteWaterMaterialEditor()
{
    if (!m_assetLibrary || m_waterMaterialEditor.materialId.empty())
        return false;

    const std::string deletedId = m_waterMaterialEditor.materialId;
    const auto usageIt = m_waterMaterialUsageCounts.find(deletedId);
    const std::uint32_t users = usageIt != m_waterMaterialUsageCounts.end() ? usageIt->second : 0u;
    std::string error;
    if (!m_assetLibrary->Remove(deletedId, error))
    {
        m_assetStatus = "Water material delete failed: " + error;
        return false;
    }

    if (m_selectedAssetId == deletedId)
        m_selectedAssetId.clear();
    m_waterMaterialEditor = {};
    m_commands.waterMaterialDeleted = true;
    m_commands.deletedWaterMaterialId = deletedId;
    RefreshAssetLibrary();
    SyncWaterMaterialSnapshot();
    m_assetStatus = "Water material deleted: " + deletedId;
    Tracenf("[EDITOR-IMGUI-4] Material deleted: id=%s affected_water_bodies=%u",
        deletedId.c_str(),
        users);
    return true;
}

void EditorImGui::AssignAssetToSelectedWaterBody(const std::string& assetId)
{
    if (!m_assetLibrary || !m_waterBodyState.selected)
        return;

    const auto entry = m_assetLibrary->FindById(assetId);
    if (!entry)
    {
        m_assetStatus = "Drop failed: asset not found";
        return;
    }
    if (entry->category != AssetLibrary::Category::WaterMaterial &&
        entry->category != AssetLibrary::Category::Material)
    {
        m_assetStatus = "Water bodies accept water or PBR materials";
        return;
    }

    m_waterBodyState.materialId = entry->id;
    m_waterBodyState.materialName = entry->displayName;
    MarkSelectedWaterBodyChanged();
    if (entry->category == AssetLibrary::Category::WaterMaterial)
    {
        OpenWaterMaterialEditor(entry->id);
    }
    m_assetStatus = "Water body material <- " + entry->displayName;
    Tracenf("[EDITOR-IMGUI-3] Drag dropped: asset_id=%s target_type=selected_water_body body_id=%u",
        entry->id.c_str(),
        m_waterBodyState.id);
}

void EditorImGui::AssignAssetToSelectedMeshRenderer(const std::string& assetId)
{
    if (!m_assetLibrary || !m_meshRendererState.selected)
        return;

    const auto entry = m_assetLibrary->FindById(assetId);
    if (!entry)
    {
        m_assetStatus = "Drop failed: asset not found";
        return;
    }
    if (entry->category != AssetLibrary::Category::Model)
    {
        m_assetStatus = "MeshRenderer accepts model assets";
        return;
    }

    m_meshRendererState.meshAssetId = entry->id;
    m_meshRendererState.meshAssetPath = m_assetLibrary->AssetRelativePath(*entry);
    m_meshRendererState.meshDisplayName = entry->displayName;
    MarkSelectedMeshRendererChanged();
    m_assetStatus = "MeshRenderer mesh <- " + entry->displayName;
    Tracenf("[MESH-ENTITY] Mesh assigned: entity=%u asset_id=%s path=%s",
        m_meshRendererState.id,
        entry->id.c_str(),
        m_meshRendererState.meshAssetPath.c_str());
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
    {
        static uint32_t beginSkippedLogs = 0;
        if (beginSkippedLogs < 3)
        {
            ++beginSkippedLogs;
            Tracenf("[FRAME] imgui_begin called = no, initialized=%d backend_ready=%d frame_active=%d editor_mode=%d",
                m_initialized ? 1 : 0,
                m_vulkanBackendReady ? 1 : 0,
                m_frameActive ? 1 : 0,
                editorModeActive ? 1 : 0);
        }
        return;
    }

    m_editorModeActive = editorModeActive;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    m_frameActive = true;
    static uint32_t beginLogs = 0;
    if (beginLogs < 3)
    {
        ++beginLogs;
        Tracenf("[FRAME] imgui_begin called = yes, editor_mode=%d", editorModeActive ? 1 : 0);
    }
}

void EditorImGui::RenderDemoPanels()
{
    if (!m_editorModeActive)
        return;

    if (m_showDemoWindow)
        ImGui::ShowDemoWindow(&m_showDemoWindow);
}

void EditorImGui::RenderDockSpace()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Editor DockSpace", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    if (m_applyDefaultDockLayout && !m_defaultDockLayoutBuilt)
    {
        m_defaultDockLayoutBuilt = true;
        ImGui::DockBuilderRemoveNode(dockspaceId);
        const ImGuiDockNodeFlags dockBuilderFlags = static_cast<ImGuiDockNodeFlags>(
            static_cast<int>(ImGuiDockNodeFlags_DockSpace) |
            static_cast<int>(ImGuiDockNodeFlags_PassthruCentralNode));
        ImGui::DockBuilderAddNode(dockspaceId, dockBuilderFlags);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        ImGuiID mainId = dockspaceId;
        ImGuiID leftId = 0;
        ImGuiID rightId = 0;
        ImGuiID bottomId = 0;
        ImGuiID topId = 0;
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Up, 0.06f, &topId, &mainId);
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Left, 0.20f, &leftId, &mainId);
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.25f, &rightId, &mainId);
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Down, 0.30f, &bottomId, &mainId);
        ImGui::DockBuilderDockWindow("Editor Toolbar", topId);
        ImGui::DockBuilderDockWindow(ICON_FA_LIST_TREE " Hierarchy", leftId);
        ImGui::DockBuilderDockWindow("Tools", leftId);
        ImGui::DockBuilderDockWindow("Inspector", rightId);
        ImGui::DockBuilderDockWindow("Scene Settings", rightId);
        ImGui::DockBuilderDockWindow(ICON_FA_GLOBE " World", rightId);
        ImGui::DockBuilderDockWindow("Asset Browser", bottomId);
        ImGui::DockBuilderDockWindow("Scene View", mainId);
        ImGui::DockBuilderFinish(dockspaceId);
        Tracen("[EDITOR-LAYOUT] Default Unity-style dock layout applied");
    }
    ImGui::End();
}

void EditorImGui::RenderSceneViewDropTarget()
{
    const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
    const bool assetDragActive = activePayload && std::strcmp(activePayload->DataType, kAssetPayloadType) == 0;
    m_viewportInputDiagnostics = {};
    m_viewportInputDiagnostics.assetDragActive = assetDragActive;
    if (!assetDragActive)
    {
        m_viewportDropTargetLogged = false;
        m_loggedDragAssetId.clear();
    }

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse;

    auto acceptModelDrop = [&]() {
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
            {
                if (payload->IsDelivery())
                {
                    const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                    const auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
                    if (entry && entry->category == AssetLibrary::Category::Model)
                    {
                        const ImVec2 mouse = ImGui::GetMousePos();
                        const ImGuiViewport* viewport = ImGui::GetMainViewport();
                        const ImVec2 viewportPos = viewport ? viewport->Pos : ImVec2(0.0f, 0.0f);
                        InputEvent dropEvent{};
                        dropEvent.type = InputEvent::MouseUp;
                        dropEvent.x = static_cast<int>(std::round(mouse.x - viewportPos.x));
                        dropEvent.y = static_cast<int>(std::round(mouse.y - viewportPos.y));
                        const InputEvent mappedDropEvent = MapInputToSceneView(dropEvent);
                        m_commands.addMeshEntity = true;
                        m_commands.meshAssetId = entry->id;
                        m_commands.meshDropScreenPositionValid = true;
                        m_commands.meshDropScreenPosition[0] = static_cast<float>(mappedDropEvent.x);
                        m_commands.meshDropScreenPosition[1] = static_cast<float>(mappedDropEvent.y);
                        m_assetStatus = "Mesh entity dropped: " + entry->displayName;
                        Tracenf("[DND] payload accepted: %s", entry->id.c_str());
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    };

    if (!ImGui::Begin("Scene View", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    const ImVec2 sceneMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    const ImVec2 viewportPos = mainViewport ? mainViewport->Pos : ImVec2(0.0f, 0.0f);
    ImVec2 imageMin = sceneMin;
    ImVec2 imageSize = avail;
    if (avail.x > 1.0f && avail.y > 1.0f &&
        m_sceneViewExtent.width > 0 && m_sceneViewExtent.height > 0)
    {
        const float targetAspect = static_cast<float>(m_sceneViewExtent.width) /
            static_cast<float>(m_sceneViewExtent.height);
        const float availableAspect = avail.x / avail.y;
        if (availableAspect > targetAspect)
        {
            imageSize.x = avail.y * targetAspect;
            imageMin.x += (avail.x - imageSize.x) * 0.5f;
        }
        else
        {
            imageSize.y = avail.x / targetAspect;
            imageMin.y += (avail.y - imageSize.y) * 0.5f;
        }
    }
    if (avail.x > 1.0f && avail.y > 1.0f)
    {
        m_viewportInputDiagnostics.sceneViewRectValid = true;
        m_viewportInputDiagnostics.sceneViewMin[0] = imageMin.x - viewportPos.x;
        m_viewportInputDiagnostics.sceneViewMin[1] = imageMin.y - viewportPos.y;
        m_viewportInputDiagnostics.sceneViewSize[0] = imageSize.x;
        m_viewportInputDiagnostics.sceneViewSize[1] = imageSize.y;
        m_viewportInputDiagnostics.sceneViewExtent[0] = m_sceneViewExtent.width;
        m_viewportInputDiagnostics.sceneViewExtent[1] = m_sceneViewExtent.height;
    }
    const bool canDrawSceneView = m_sceneViewDescriptor && avail.x > 1.0f && avail.y > 1.0f;
    bool sceneViewItemDrawn = false;
    if (canDrawSceneView)
    {
        ImGui::SetCursorScreenPos(imageMin);
        ImGui::Image(reinterpret_cast<ImTextureID>(m_sceneViewDescriptor), imageSize);
        sceneViewItemDrawn = true;
    }
    else if (avail.x > 1.0f && avail.y > 1.0f)
    {
        ImGui::BeginDisabled();
        ImGui::TextUnformatted("Scene render target unavailable");
        ImGui::EndDisabled();
    }
    m_viewportInputDiagnostics.sceneViewHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
        (sceneViewItemDrawn && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
    m_viewportInputDiagnostics.sceneViewFocused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (assetDragActive && avail.x > 1.0f && avail.y > 1.0f)
    {
        if (!m_viewportDropTargetLogged)
        {
            m_viewportDropTargetLogged = true;
            Tracen("[DND] viewport drop target active");
        }

        ImGuiID dropItemId = 0;
        if (sceneViewItemDrawn)
        {
            dropItemId = ImGui::GetItemID();
        }
        else
        {
            dropItemId = ImGui::GetID("##SceneViewDropTarget");
            ImGui::SetCursorScreenPos(imageMin);
            ImGui::InvisibleButton("##SceneViewDropTarget", imageSize);
        }
        m_viewportInputDiagnostics.dropTargetVisible = true;
        m_viewportInputDiagnostics.dropTargetHovered = ImGui::IsItemHovered();
        m_viewportInputDiagnostics.dropTargetActive = ImGui::IsItemActive();
        if (m_viewportInputDiagnostics.dropTargetHovered)
            m_viewportInputDiagnostics.hoveredItemId = static_cast<std::uint32_t>(dropItemId);
        m_viewportInputDiagnostics.activeItemId = m_viewportInputDiagnostics.dropTargetActive
            ? static_cast<std::uint32_t>(dropItemId)
            : 0u;
        acceptModelDrop();
    }

    ImGui::End();

    if (!assetDragActive || m_commands.addMeshEntity)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport)
        return;

    const ImVec2 workPos = viewport->WorkPos;
    const ImVec2 workSize = viewport->WorkSize;
    const ImVec2 overlayPos(workPos.x + workSize.x * 0.20f, workPos.y + workSize.y * 0.06f);
    const ImVec2 overlaySize(workSize.x * 0.55f, workSize.y * 0.64f);
    if (overlaySize.x <= 1.0f || overlaySize.y <= 1.0f)
        return;

    ImGui::SetNextWindowPos(overlayPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(overlaySize, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    const ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("##ViewportDropOverlay", nullptr, overlayFlags))
    {
        const ImGuiID overlayItemId = ImGui::GetID("##ViewportDropOverlayTarget");
        ImGui::InvisibleButton("##ViewportDropOverlayTarget", ImGui::GetContentRegionAvail());
        m_viewportInputDiagnostics.dropTargetVisible = true;
        m_viewportInputDiagnostics.overlayDropTargetHovered = ImGui::IsItemHovered();
        m_viewportInputDiagnostics.overlayDropTargetActive = ImGui::IsItemActive();
        if (m_viewportInputDiagnostics.overlayDropTargetHovered)
            m_viewportInputDiagnostics.hoveredItemId = static_cast<std::uint32_t>(overlayItemId);
        if (m_viewportInputDiagnostics.overlayDropTargetActive)
            m_viewportInputDiagnostics.activeItemId = static_cast<std::uint32_t>(overlayItemId);
        acceptModelDrop();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorImGui::OpenProjectDialog(ProjectDialogMode mode)
{
    m_projectDialogMode = mode;
    m_projectPopupNeedsOpen = true;
    m_projectCreateBrowserVisible = mode == ProjectDialogMode::Open;
    if (m_projectBrowserPath.empty())
        m_projectBrowserPath = InitialProjectBrowserPath(m_engineRoot);
    CopyToBuffer(m_projectBrowsePathBuffer, sizeof(m_projectBrowsePathBuffer), m_projectBrowserPath.string());
    if (m_projectParentBuffer[0] == '\0')
        CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
}

bool EditorImGui::NavigateProjectBrowser(const std::filesystem::path& path, bool createMissing)
{
    std::error_code ec;
    std::filesystem::path target = path.empty() ? InitialProjectBrowserPath(m_engineRoot) : path;
    target = std::filesystem::absolute(target, ec);
    if (ec)
    {
        m_projectStatus = "Browse failed: " + ec.message();
        return false;
    }

    if (std::filesystem::is_regular_file(target, ec))
        target = target.parent_path();
    bool createdFolder = false;
    if (!std::filesystem::exists(target, ec) || !std::filesystem::is_directory(target, ec))
    {
        if (!createMissing)
        {
            m_projectStatus = "Browse failed: folder not found";
            return false;
        }

        ec.clear();
        std::filesystem::create_directories(target, ec);
        if (ec)
        {
            m_projectStatus = "Browse create failed: " + ec.message();
            return false;
        }
        createdFolder = true;
        m_projectStatus = "Folder created: " + target.string();
    }

    m_projectBrowserPath = target;
    CopyToBuffer(m_projectBrowsePathBuffer, sizeof(m_projectBrowsePathBuffer), m_projectBrowserPath.string());
    if (!createdFolder)
        m_projectStatus = "Browse folder: " + target.string();
    return true;
}

void EditorImGui::ActivateCurrentProject()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return;

    InitializeProjectAssetLibrary(projects.ProjectRoot(), projects.AssetRootPath());
    SceneManager::Instance().CloseScene();
    const bool openedScene = LoadProjectStartupScene();
    const bool createdScene = openedScene ? false : CreateDefaultProjectScene();
    m_projectDialogMode = ProjectDialogMode::None;
    m_projectPopupNeedsOpen = false;
    m_projectStatus = "Project active: " + projects.CurrentProject().name;
    if (openedScene)
        m_projectStatus += " | scene loaded";
    else if (createdScene)
        m_projectStatus += " | default scene created";
    else
        m_projectStatus += " | no scene";

    m_attachedScenePaths.clear();
    const std::string activePath = SceneManager::Instance().GetCurrentScenePath();
    if (!activePath.empty())
    {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(activePath, projects.ProjectRoot(), ec);
        m_attachedScenePaths.push_back(ec ? std::filesystem::path(activePath).generic_string() : relative.generic_string());
    }
}

bool EditorImGui::LoadProjectStartupScene()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return false;

    std::vector<std::string> candidates;
    const ProjectData& project = projects.CurrentProject();
    if (!project.startupScene.empty())
        candidates.push_back(project.startupScene);
    for (const std::string& recentScene : project.recentScenes)
    {
        if (!recentScene.empty() &&
            std::find(candidates.begin(), candidates.end(), recentScene) == candidates.end())
        {
            candidates.push_back(recentScene);
        }
    }

    for (const std::string& candidate : candidates)
    {
        std::filesystem::path scenePath(candidate);
        if (!scenePath.is_absolute())
            scenePath = projects.ProjectRoot() / scenePath;
        if (!std::filesystem::exists(scenePath))
        {
            Tracenf("[PROJECT] startup scene missing: %s", scenePath.string().c_str());
            continue;
        }
        if (SceneManager::Instance().LoadScene(scenePath.string()))
        {
            Tracenf("[PROJECT] startup scene loaded: %s", scenePath.string().c_str());
            return true;
        }
        Tracenf("[PROJECT] startup scene load failed: %s", scenePath.string().c_str());
    }

    return false;
}

bool EditorImGui::CreateDefaultProjectScene()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return false;

    std::filesystem::path scenePath = projects.ScenesPath() / "Main" / "Main.scene";
    if (std::filesystem::exists(scenePath))
    {
        if (SceneManager::Instance().LoadScene(scenePath.string()))
        {
            Tracenf("[PROJECT] existing default scene loaded: %s", scenePath.string().c_str());
            return true;
        }

        for (int index = 2; index < 1000; ++index)
        {
            const std::string name = "Main_" + std::to_string(index);
            std::filesystem::path candidate = projects.ScenesPath() / name / (name + ".scene");
            if (!std::filesystem::exists(candidate))
            {
                scenePath = std::move(candidate);
                break;
            }
        }
    }

    SceneManager& scenes = SceneManager::Instance();
    scenes.NewScene();
    scenes.SetSceneName("Main");

    if (!scenes.SaveSceneAs(scenePath.string()))
    {
        TraceError("[PROJECT] default scene create failed: %s", scenePath.string().c_str());
        return false;
    }

    Tracenf("[PROJECT] default scene created: %s", scenePath.string().c_str());
    return true;
}

void EditorImGui::CreateProjectFromDialog()
{
    std::string error;
    if (!ProjectManager::Instance().CreateProject(m_projectParentBuffer, m_projectNameBuffer, error))
    {
        m_projectStatus = "Create failed: " + error;
        return;
    }

    ActivateCurrentProject();
    ImGui::CloseCurrentPopup();
}

void EditorImGui::OpenProjectFromDialog(const std::filesystem::path& manifestPath)
{
    std::string error;
    if (!ProjectManager::Instance().OpenProject(manifestPath, error))
    {
        m_projectStatus = "Open failed: " + error;
        return;
    }

    ActivateCurrentProject();
    ImGui::CloseCurrentPopup();
}

void EditorImGui::RenderProjectBrowser(bool pickProjectFile)
{
    if (m_projectBrowserPath.empty())
        NavigateProjectBrowser(InitialProjectBrowserPath(m_engineRoot));

    const std::string currentPath = m_projectBrowserPath.string();
    ImGui::TextUnformatted("Browse Path");
    ImGui::SetNextItemWidth(-56.0f);
    if (ImGui::InputText("##ProjectBrowsePath",
            m_projectBrowsePathBuffer,
            sizeof(m_projectBrowsePathBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (NavigateProjectBrowser(m_projectBrowsePathBuffer, !pickProjectFile) && !pickProjectFile)
            CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
    }
    ImGui::SameLine();
    if (ImGui::Button("Go", ImVec2(44.0f, 0.0f)))
    {
        if (NavigateProjectBrowser(m_projectBrowsePathBuffer, !pickProjectFile) && !pickProjectFile)
            CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
    }

#if defined(_WIN32)
    ImGui::TextUnformatted("Drives");
    bool firstDrive = true;
    for (char drive = 'A'; drive <= 'Z'; ++drive)
    {
        const std::string root = std::string(1, drive) + ":\\";
        std::error_code driveEc;
        if (!std::filesystem::exists(root, driveEc))
            continue;
        if (!firstDrive)
            ImGui::SameLine();
        firstDrive = false;
        if (ImGui::Button((std::string(1, drive) + ":").c_str()))
            NavigateProjectBrowser(root);
    }
#endif

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ProjectBrowseFilter", "Filter folders/projects...", m_projectBrowseFilterBuffer, sizeof(m_projectBrowseFilterBuffer));
    ImGui::TextDisabled("%s", currentPath.c_str());
    if (ImGui::Button("Up"))
    {
        const std::filesystem::path parent = m_projectBrowserPath.parent_path();
        if (!parent.empty())
            NavigateProjectBrowser(parent);
    }
    ImGui::SameLine();
    if (!pickProjectFile && ImGui::Button("Use This Folder"))
    {
        CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
        m_projectCreateBrowserVisible = false;
    }

    ImGui::Separator();
    if (ImGui::BeginChild(pickProjectFile ? "OpenProjectBrowser" : "CreateProjectBrowser", ImVec2(0.0f, 220.0f), true))
    {
        std::vector<std::filesystem::directory_entry> directories;
        std::vector<std::filesystem::directory_entry> manifests;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(m_projectBrowserPath, ec))
        {
            if (entry.is_directory(ec))
                directories.push_back(entry);
            else if (pickProjectFile && entry.path().extension() == ".ixproj")
                manifests.push_back(entry);
        }
        std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
            return a.path().filename().string() < b.path().filename().string();
        });
        std::sort(manifests.begin(), manifests.end(), [](const auto& a, const auto& b) {
            return a.path().filename().string() < b.path().filename().string();
        });

        for (const auto& entry : directories)
        {
            const std::string filename = entry.path().filename().string();
            if (!ContainsCaseInsensitive(filename, m_projectBrowseFilterBuffer))
                continue;
            const std::string label = "[Folder] " + filename;
            if (ImGui::Selectable(label.c_str()))
            NavigateProjectBrowser(entry.path());
        }
        if (pickProjectFile)
        {
            for (const auto& entry : manifests)
            {
                const std::string filename = entry.path().filename().string();
                if (!ContainsCaseInsensitive(filename, m_projectBrowseFilterBuffer))
                    continue;
                const std::string label = "[Project] " + filename;
                if (ImGui::Selectable(label.c_str()))
                    CopyToBuffer(m_projectOpenPathBuffer, sizeof(m_projectOpenPathBuffer), entry.path().string());
            }
        }
    }
    ImGui::EndChild();
}

void EditorImGui::RenderProjectModal()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (m_projectDialogMode == ProjectDialogMode::None)
        return;

    if (m_projectPopupNeedsOpen)
    {
        ImGui::OpenPopup("Project");
        m_projectPopupNeedsOpen = false;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Project", nullptr, ImGuiWindowFlags_NoCollapse))
        return;

    if (m_projectDialogMode == ProjectDialogMode::NoProject)
    {
        ImGui::TextUnformatted("No Project");
        ImGui::TextDisabled("Create or open a project to bind the Asset Browser and scene workspace.");
        ImGui::Separator();
        if (ImGui::Button("Create New Project", ImVec2(180.0f, 0.0f)))
            m_projectDialogMode = ProjectDialogMode::Create;
        ImGui::SameLine();
        if (ImGui::Button("Open Project", ImVec2(180.0f, 0.0f)))
            m_projectDialogMode = ProjectDialogMode::Open;

        ImGui::Separator();
        ImGui::TextUnformatted("Recent Projects");
        if (projects.RecentProjects().empty())
        {
            ImGui::TextDisabled("No recent projects.");
        }
        else
        {
            for (const auto& path : projects.RecentProjects())
            {
                const std::string label = path.parent_path().filename().string() + "##" + path.string();
                if (ImGui::Selectable(label.c_str()))
                    OpenProjectFromDialog(path);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", path.string().c_str());
            }
        }
    }
    else if (m_projectDialogMode == ProjectDialogMode::Create)
    {
        ImGui::TextUnformatted("Create New Project");
        ImGui::TextUnformatted("Parent Folder");
        const float browseButtonWidth = 96.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - browseButtonWidth - spacing));
        ImGui::InputText("##ProjectParentFolder", m_projectParentBuffer, sizeof(m_projectParentBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(96.0f, 0.0f)))
        {
            m_projectCreateBrowserVisible = !m_projectCreateBrowserVisible;
            if (NavigateProjectBrowser(m_projectParentBuffer, true))
                CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
        }
        ImGui::TextUnformatted("Project Name");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##ProjectName", m_projectNameBuffer, sizeof(m_projectNameBuffer));

        const std::filesystem::path targetProjectPath = std::filesystem::path(m_projectParentBuffer) / m_projectNameBuffer;
        ImGui::TextDisabled("Target: %s", targetProjectPath.string().c_str());
        if (ImGui::Button("Create Project", ImVec2(150.0f, 0.0f)))
            CreateProjectFromDialog();
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(120.0f, 0.0f)))
            m_projectDialogMode = projects.HasProject() ? ProjectDialogMode::None : ProjectDialogMode::NoProject;

        if (m_projectCreateBrowserVisible)
            RenderProjectBrowser(false);
    }
    else if (m_projectDialogMode == ProjectDialogMode::Open)
    {
        ImGui::TextUnformatted("Open Project");
        ImGui::InputText("project.ixproj", m_projectOpenPathBuffer, sizeof(m_projectOpenPathBuffer));
        RenderProjectBrowser(true);
        ImGui::Separator();
        if (ImGui::Button("Open", ImVec2(120.0f, 0.0f)))
            OpenProjectFromDialog(m_projectOpenPathBuffer);
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(120.0f, 0.0f)))
            m_projectDialogMode = projects.HasProject() ? ProjectDialogMode::None : ProjectDialogMode::NoProject;
    }

    if (!m_projectStatus.empty())
    {
        ImGui::Separator();
        ImGui::TextDisabled("%s", m_projectStatus.c_str());
    }

    ImGui::EndPopup();
}

void EditorImGui::RenderMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;

    SceneManager& scenes = SceneManager::Instance();
    ProjectManager& projects = ProjectManager::Instance();
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Project..."))
            OpenProjectDialog(ProjectDialogMode::Create);
        if (ImGui::MenuItem("Open Project..."))
            OpenProjectDialog(ProjectDialogMode::Open);
        if (ImGui::MenuItem("Save Project", nullptr, false, projects.HasProject()))
        {
            std::string error;
            if (!projects.SaveProject(error))
                m_projectStatus = "Save project failed: " + error;
            else
                m_projectStatus = "Project saved";
        }
        if (ImGui::BeginMenu("Recent Projects", !projects.RecentProjects().empty()))
        {
            for (const auto& path : projects.RecentProjects())
            {
                const std::string label = path.parent_path().filename().string();
                if (ImGui::MenuItem(label.c_str()))
                    OpenProjectFromDialog(path);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FILE "  New Scene", "Ctrl+N"))
            scenes.NewScene();
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Scene...", "Ctrl+O"))
        {
#if defined(_WIN32)
            char file[MAX_PATH]{};
            OPENFILENAMEA ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrTitle = "Open Scene";
            ofn.lpstrFilter = "Scene Files (*.scene)\0*.scene\0All files (*.*)\0*.*\0\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn))
                scenes.LoadScene(file);
#endif
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save Scene", "Ctrl+S"))
            scenes.SaveScene();
        if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save Scene As...", "Ctrl+Shift+S"))
            scenes.SaveSceneAs({});
        ImGui::Separator();
        if (ImGui::BeginMenu("Recent Scenes", !scenes.GetRecentScenes().empty()))
        {
            for (const std::string& path : scenes.GetRecentScenes())
            {
                const std::string label = std::filesystem::path(path).filename().string();
                if (ImGui::MenuItem(label.c_str()))
                    scenes.LoadScene(path);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
        ImGui::MenuItem("Undo", "Ctrl+Z", false, false);
        ImGui::MenuItem("Redo", "Ctrl+Y", false, false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        ImGui::MenuItem("Demo Window", nullptr, &m_showDemoWindow);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        ImGui::MenuItem("About IxtreemeWorld Engine", nullptr, false, false);
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void EditorImGui::HandleEditorHotkeys()
{
    if (!m_editorModeActive)
        return;

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard)
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
    {
        if (io.KeyShift)
        {
            if (m_playModeState.mode != EditorPlayMode::Edit)
                m_commands.exitPlayMode = true;
        }
        else if (m_playModeState.mode == EditorPlayMode::Edit)
        {
            if (SceneManager::Instance().HasOpenScene())
                m_commands.enterPlayMode = true;
            else
                Tracen("[EDIT-PLAY] F5 ignored: no open scene");
        }
        else
        {
            m_commands.exitPlayMode = true;
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
    {
        if (m_playModeState.mode == EditorPlayMode::Play)
            m_commands.pausePlayMode = true;
        else if (m_playModeState.mode == EditorPlayMode::PlayPaused)
            m_commands.resumePlayMode = true;
    }

    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        if (m_waterBodyState.selected)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::WaterBody, m_waterBodyState.id))
                QueueHierarchyFocus(*entity);
        }
        else if (m_dynamicLightState.type == DynamicLightType::Point)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::PointLight, m_dynamicLightState.id))
                QueueHierarchyFocus(*entity);
        }
        else if (m_dynamicLightState.type == DynamicLightType::Spot)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::SpotLight, m_dynamicLightState.id))
                QueueHierarchyFocus(*entity);
        }
    }

    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F2, false))
    {
        if (m_waterBodyState.selected)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::WaterBody, m_waterBodyState.id))
                StartHierarchyRename(*entity);
        }
        else if (m_dynamicLightState.type == DynamicLightType::Point)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::PointLight, m_dynamicLightState.id))
                StartHierarchyRename(*entity);
        }
        else if (m_dynamicLightState.type == DynamicLightType::Spot)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::SpotLight, m_dynamicLightState.id))
                StartHierarchyRename(*entity);
        }
    }

    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        if (m_waterBodyState.selected)
        {
            m_commands.hierarchyDeleteEntity = true;
            m_commands.hierarchyEntityType = HierarchyEntityType::WaterBody;
            m_commands.hierarchyEntityId = m_waterBodyState.id;
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::WaterBody, m_waterBodyState.id))
                m_commands.hierarchyEntityHandle = entity->entity;
        }
        else if (m_dynamicLightState.type == DynamicLightType::Point)
        {
            m_commands.hierarchyDeleteEntity = true;
            m_commands.hierarchyEntityType = HierarchyEntityType::PointLight;
            m_commands.hierarchyEntityId = m_dynamicLightState.id;
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::PointLight, m_dynamicLightState.id))
                m_commands.hierarchyEntityHandle = entity->entity;
        }
        else if (m_dynamicLightState.type == DynamicLightType::Spot)
        {
            m_commands.hierarchyDeleteEntity = true;
            m_commands.hierarchyEntityType = HierarchyEntityType::SpotLight;
            m_commands.hierarchyEntityId = m_dynamicLightState.id;
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::SpotLight, m_dynamicLightState.id))
                m_commands.hierarchyEntityHandle = entity->entity;
        }
    }

    if (io.KeyCtrl)
    {
        SceneManager& scenes = SceneManager::Instance();
        if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false))
            scenes.NewScene();
        if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O, false))
        {
#if defined(_WIN32)
            char file[MAX_PATH]{};
            OPENFILENAMEA ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrTitle = "Open Scene";
            ofn.lpstrFilter = "Scene Files (*.scene)\0*.scene\0All files (*.*)\0*.*\0\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn))
                scenes.LoadScene(file);
#endif
        }
        if (ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            if (io.KeyShift)
                scenes.SaveSceneAs({});
            else
                scenes.SaveScene();
        }
    }
}

void EditorImGui::RenderEditorToolbar()
{
    if (!m_editorModeActive)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(760.0f, 58.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Editor Toolbar", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar))
    {
        const bool isEdit = m_playModeState.mode == EditorPlayMode::Edit;
        const bool isPlay = m_playModeState.mode == EditorPlayMode::Play;
        const bool isPaused = m_playModeState.mode == EditorPlayMode::PlayPaused;

        if (isEdit)
        {
            const bool canPlay = SceneManager::Instance().HasOpenScene();
            if (!canPlay)
                ImGui::BeginDisabled();
            if (UI::IconButton(ICON_FA_PLAY, "Play", ImVec2(96.0f, 32.0f)))
                m_commands.enterPlayMode = true;
            if (!canPlay)
            {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open a scene to Play");
            }
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
            if (UI::IconButton(ICON_FA_STOP, "Stop", ImVec2(96.0f, 32.0f)))
                m_commands.exitPlayMode = true;
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (isPlay)
            {
                if (UI::IconButton(ICON_FA_PAUSE, "Pause", ImVec2(104.0f, 32.0f)))
                    m_commands.pausePlayMode = true;
            }
            else if (isPaused)
            {
                if (UI::IconButton(ICON_FA_PLAY, "Resume", ImVec2(112.0f, 32.0f)))
                    m_commands.resumePlayMode = true;
            }
        }

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(16.0f, 0.0f));
        ImGui::SameLine();

        const char* modeText = isEdit ? "EDIT MODE" : isPlay ? "PLAY MODE" : "PAUSED";
        const ImVec4 modeColor = isEdit
            ? ImVec4(0.72f, 0.72f, 0.72f, 1.0f)
            : isPlay ? ImVec4(0.35f, 0.90f, 0.35f, 1.0f) : ImVec4(0.95f, 0.74f, 0.30f, 1.0f);
        ImGui::TextColored(modeColor, "%s", modeText);
        if (!isEdit)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%.1fs, frame %d)", m_playModeState.elapsedSeconds, m_playModeState.frameCount);
        }

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(24.0f, 0.0f));
        ImGui::SameLine();
        RenderGizmoControls();

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(18.0f, 0.0f));
        ImGui::SameLine();
        ImGui::TextDisabled("FPS %.0f | %.2f ms | CPU %.0f%%",
            m_engineStats.fps,
            m_engineStats.averageFrameMs > 0.0 ? m_engineStats.averageFrameMs : m_engineStats.frameMs,
            m_engineStats.processCpuPercent);
    }
    ImGui::End();
}

void EditorImGui::RenderSceneSettingsPanel()
{
    if (ImGui::Begin("Scene Settings"))
    {
        SceneManager& scenes = SceneManager::Instance();
        if (!scenes.HasOpenScene())
        {
            ImGui::TextUnformatted("No scene open");
            ImGui::TextWrapped("Create or open a scene before editing scene metadata.");
        }
        else
        {
            const SceneData& scene = scenes.GetCurrentScene();

            char nameBuffer[128]{};
            std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", scene.name.c_str());
            if (ImGui::InputText("Scene Name", nameBuffer, sizeof(nameBuffer)))
                scenes.SetSceneName(nameBuffer);

            ImGui::Separator();
            ImGui::TextUnformatted("Camera");
            ImGui::Text("Position: %.1f, %.1f, %.1f",
                scene.cameraPosition[0],
                scene.cameraPosition[1],
                scene.cameraPosition[2]);
            ImGui::Text("FOV: %.1f  Near/Far: %.2f / %.1f",
                scene.cameraFov,
                scene.cameraNear,
                scene.cameraFar);

            ImGui::Separator();
            ImGui::TextUnformatted("Environment");
            ImGui::Text("Sun: %.1f / %.1f  Intensity: %.2f",
                scene.lighting.directional.elevationDegrees,
                scene.lighting.directional.azimuthDegrees,
                scene.lighting.directional.intensity);
            ImGui::Text("Ambient: %.2f", scene.lighting.ambient.intensity);
        }
    }
    ImGui::End();
}

bool EditorImGui::HierarchyPassesSearch(const std::string& name) const
{
    return ContainsCaseInsensitive(name, m_hierarchySearchBuffer);
}

const HierarchySceneEntity* EditorImGui::FindHierarchyEntity(std::uint64_t entity) const
{
    auto it = std::find_if(m_hierarchyEntities.begin(), m_hierarchyEntities.end(),
        [entity](const HierarchySceneEntity& candidate) {
            return candidate.entity == entity;
        });
    return it == m_hierarchyEntities.end() ? nullptr : &*it;
}

const HierarchySceneEntity* EditorImGui::FindHierarchyEntity(HierarchyEntityType type, std::uint32_t objectId) const
{
    auto it = std::find_if(m_hierarchyEntities.begin(), m_hierarchyEntities.end(),
        [type, objectId](const HierarchySceneEntity& candidate) {
            return candidate.type == type && candidate.objectId == objectId;
        });
    return it == m_hierarchyEntities.end() ? nullptr : &*it;
}

bool EditorImGui::HierarchySubtreePassesSearch(std::uint64_t entity) const
{
    const HierarchySceneEntity* node = FindHierarchyEntity(entity);
    if (!node)
        return false;
    if (HierarchyPassesSearch(node->name))
        return true;
    for (const HierarchySceneEntity& child : m_hierarchyEntities)
    {
        if (child.parent == entity && HierarchySubtreePassesSearch(child.entity))
            return true;
    }
    return false;
}

void EditorImGui::QueueHierarchySelection(const HierarchySceneEntity& entity)
{
    m_commands.hierarchySelectEntity = true;
    m_commands.hierarchyEntityType = entity.type;
    m_commands.hierarchyEntityId = entity.objectId;
    m_commands.hierarchyEntityHandle = entity.entity;
    m_selectedHierarchyEntity = entity.entity;
    Tracenf("[HIERARCHY] Selected entity: flecs=%llu object=%u type=%d",
        static_cast<unsigned long long>(entity.entity),
        entity.objectId,
        static_cast<int>(entity.type));
}

void EditorImGui::QueueHierarchyFocus(const HierarchySceneEntity& entity)
{
    m_commands.hierarchyFocusEntity = true;
    m_commands.hierarchyEntityType = entity.type;
    m_commands.hierarchyEntityId = entity.objectId;
    m_commands.hierarchyEntityHandle = entity.entity;
    Tracenf("[HIERARCHY] Focus requested: flecs=%llu object=%u type=%d",
        static_cast<unsigned long long>(entity.entity),
        entity.objectId,
        static_cast<int>(entity.type));
}

void EditorImGui::StartHierarchyRename(const HierarchySceneEntity& entity)
{
    m_hierarchyRenamingEntity = entity.entity;
    std::snprintf(m_hierarchyRenameBuffer, sizeof(m_hierarchyRenameBuffer), "%s", entity.name.c_str());
}

void EditorImGui::RenderHierarchyToolbar()
{
    ImGui::PushItemWidth(-1.0f);
    const bool changed = ImGui::InputTextWithHint("##hierarchy_search",
        ICON_FA_MAGNIFYING_GLASS " Search entities/scenes...",
        m_hierarchySearchBuffer,
        sizeof(m_hierarchySearchBuffer));
    ImGui::PopItemWidth();
    if (changed)
        Tracenf("[HIERARCHY] Search filter: '%s'", m_hierarchySearchBuffer);

    if (m_hierarchySearchBuffer[0] != '\0')
    {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK))
        {
            m_hierarchySearchBuffer[0] = '\0';
            Tracen("[HIERARCHY] Search filter cleared");
        }
    }
}

std::vector<EditorImGui::ProjectSceneEntry> EditorImGui::QueryProjectScenes() const
{
    std::vector<ProjectSceneEntry> scenes;
    const SceneManager& sceneManager = SceneManager::Instance();
    const std::string activePathKey = ComparablePath(sceneManager.GetCurrentScenePath());

    auto appendScene = [&](const std::filesystem::path& path, const std::string& relativePath, bool active) {
        ProjectSceneEntry entry;
        entry.path = path;
        entry.relativePath = relativePath;
        entry.name = path.empty() ? m_sceneRootName : path.stem().string();
        if (entry.name.empty())
            entry.name = "Untitled";
        entry.active = active;
        scenes.push_back(std::move(entry));
    };

    ProjectManager& projects = ProjectManager::Instance();
    if (projects.HasProject())
    {
        for (const std::string& attachedPath : m_attachedScenePaths)
        {
            if (attachedPath.empty())
                continue;
            std::filesystem::path scenePath(attachedPath);
            if (!scenePath.is_absolute())
                scenePath = projects.ProjectRoot() / scenePath;
            const bool active = !activePathKey.empty() && ComparablePath(scenePath) == activePathKey;
            appendScene(scenePath, attachedPath, active);
        }

        if (sceneManager.HasOpenScene())
        {
            const bool activeListed = std::any_of(scenes.begin(), scenes.end(), [](const ProjectSceneEntry& scene) {
                return scene.active;
            });
            if (!activeListed)
            {
                const std::filesystem::path activePath(sceneManager.GetCurrentScenePath());
                appendScene(activePath, activePath.empty() ? std::string{} : activePath.generic_string(), true);
            }
        }
    }
    else if (sceneManager.HasOpenScene())
    {
        const std::filesystem::path activePath(sceneManager.GetCurrentScenePath());
        appendScene(activePath, activePath.empty() ? std::string{} : activePath.generic_string(), true);
    }

    std::sort(scenes.begin(), scenes.end(), [](const ProjectSceneEntry& a, const ProjectSceneEntry& b) {
        return ToLowerAscii(a.relativePath.empty() ? a.name : a.relativePath) <
            ToLowerAscii(b.relativePath.empty() ? b.name : b.relativePath);
    });
    return scenes;
}

std::vector<AssetLibrary::Entry> EditorImGui::QuerySceneAssets() const
{
    std::vector<AssetLibrary::Entry> scenes;
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return scenes;

    const std::filesystem::path scenesRoot = projects.ScenesPath();
    std::error_code ec;
    if (!std::filesystem::exists(scenesRoot, ec))
        return scenes;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             scenesRoot,
             std::filesystem::directory_options::skip_permission_denied,
             ec))
    {
        if (ec)
            break;
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc) || entry.path().extension() != ".scene")
            continue;

        std::filesystem::path absolutePath = std::filesystem::absolute(entry.path(), entryEc);
        if (entryEc)
            absolutePath = entry.path();
        const std::filesystem::path relativePath = std::filesystem::relative(absolutePath, projects.ProjectRoot(), entryEc);
        const std::string relative = entryEc ? entry.path().generic_string() : relativePath.generic_string();

        AssetLibrary::Entry scene;
        scene.category = AssetLibrary::Category::Scene;
        scene.displayName = absolutePath.stem().string();
        scene.filename = absolutePath.filename().string();
        scene.originalPath = absolutePath.string();
        const std::filesystem::path sceneRelativePath = std::filesystem::relative(absolutePath, scenesRoot, entryEc);
        scene.subpath = AssetLibrary::NormalizeSubpath(
            (entryEc ? std::filesystem::path(relative) : sceneRelativePath).parent_path().generic_string());
        scene.tags = {"scene"};
        scene.id = "scene_";
        for (char ch : relative)
        {
            const unsigned char uch = static_cast<unsigned char>(ch);
            scene.id += std::isalnum(uch) ? static_cast<char>(std::tolower(uch)) : '_';
        }
        scenes.push_back(std::move(scene));
    }

    std::sort(scenes.begin(), scenes.end(), [](const AssetLibrary::Entry& a, const AssetLibrary::Entry& b) {
        return ToLowerAscii(a.originalPath) < ToLowerAscii(b.originalPath);
    });
    return scenes;
}

bool EditorImGui::AttachSceneToHierarchy(const AssetLibrary::Entry& entry)
{
    if (entry.category != AssetLibrary::Category::Scene || entry.originalPath.empty())
        return false;

    ProjectManager& projects = ProjectManager::Instance();
    std::filesystem::path scenePath(entry.originalPath);
    std::string relativePath = entry.originalPath;
    if (projects.HasProject())
    {
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(scenePath, projects.ProjectRoot(), ec);
        if (!ec)
            relativePath = relative.generic_string();
    }

    auto samePath = [&](const std::string& existing) {
        std::filesystem::path existingPath(existing);
        if (projects.HasProject() && !existingPath.is_absolute())
            existingPath = projects.ProjectRoot() / existingPath;
        return ComparablePath(existingPath) == ComparablePath(scenePath);
    };
    if (std::none_of(m_attachedScenePaths.begin(), m_attachedScenePaths.end(), samePath))
        m_attachedScenePaths.push_back(relativePath);

    if (SceneManager::Instance().LoadScene(scenePath.string()))
    {
        m_projectStatus = "Scene added to Hierarchy: " + relativePath;
        Tracenf("[HIERARCHY] Scene attached: %s", relativePath.c_str());
        return true;
    }

    m_attachedScenePaths.erase(
        std::remove_if(m_attachedScenePaths.begin(), m_attachedScenePaths.end(), samePath),
        m_attachedScenePaths.end());
    m_projectStatus = "Scene attach failed: " + relativePath;
    return false;
}

bool EditorImGui::DetachSceneFromHierarchy(const ProjectSceneEntry& scene)
{
    if (scene.relativePath.empty())
        return false;

    auto matchesScene = [&](const std::string& existing) {
        std::filesystem::path existingPath(existing);
        if (ProjectManager::Instance().HasProject() && !existingPath.is_absolute())
            existingPath = ProjectManager::Instance().ProjectRoot() / existingPath;
        return ComparablePath(existingPath) == ComparablePath(scene.path);
    };
    m_attachedScenePaths.erase(
        std::remove_if(m_attachedScenePaths.begin(), m_attachedScenePaths.end(), matchesScene),
        m_attachedScenePaths.end());

    if (scene.active)
    {
        if (SceneManager::Instance().IsDirty())
        {
            m_attachedScenePaths.push_back(scene.relativePath);
            m_projectStatus = "Save the scene before removing it from Hierarchy.";
            Tracenf("[HIERARCHY] Scene detach blocked by dirty scene: %s", scene.relativePath.c_str());
            return false;
        }
        SceneManager::Instance().CloseScene();
    }

    m_projectStatus = "Scene removed from Hierarchy: " + scene.name;
    Tracenf("[HIERARCHY] Scene detached: %s", scene.relativePath.c_str());
    return true;
}

void EditorImGui::RenderProjectSceneNode(const ProjectSceneEntry& scene)
{
    const bool activeHasMatchingEntity =
        scene.active && m_hierarchySearchBuffer[0] != '\0' && HierarchySubtreePassesSearch(m_sceneRootEntity);
    if (m_hierarchySearchBuffer[0] != '\0' && !HierarchyPassesSearch(scene.name) && !activeHasMatchingEntity)
        return;

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(scene.relativePath.empty() ? scene.name.c_str() : scene.relativePath.c_str());

    if (!scene.active)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.62f, 0.62f, 1.0f));

    bool hasChildren = scene.active && !m_hierarchyEntities.empty();
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanFullWidth |
        (scene.active ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

    const std::string label = std::string(ICON_FA_GLOBE) + " " + scene.name + "##" + scene.relativePath;
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemHovered() && !scene.relativePath.empty())
        ImGui::SetTooltip("%s", scene.relativePath.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen() && !scene.active && !scene.path.empty())
    {
        if (SceneManager::Instance().LoadScene(scene.path.string()))
            m_projectStatus = "Scene loaded: " + scene.relativePath;
    }
    if (scene.active && ImGui::BeginPopupContextItem("##scene_context"))
    {
        if (ImGui::MenuItem(ICON_FA_MOUNTAIN " Create Terrain"))
        {
            if (m_terrainState.exists)
                m_replaceTerrainConfirmOpen = true;
            else
                m_createTerrainModalOpen = true;
        }
        ImGui::EndPopup();
    }

    if (scene.active && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            const auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
            if (entry && entry->category == AssetLibrary::Category::Model)
            {
                m_commands.addMeshEntity = true;
                m_commands.meshAssetId = entry->id;
                m_assetStatus = "Mesh entity queued: " + entry->displayName;
                Tracenf("[MESH-ENTITY] Hierarchy drop queued: asset_id=%s", entry->id.c_str());
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (!scene.active)
        ImGui::PopStyleColor();

    ImGui::TableSetColumnIndex(1);
    const ImVec4 eyeColor = scene.active ? ImVec4(0.88f, 0.88f, 0.88f, 1.0f) : ImVec4(0.48f, 0.48f, 0.48f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, eyeColor);
    if (ImGui::SmallButton(scene.active ? ICON_FA_EYE : ICON_FA_EYE_SLASH))
    {
        if (!scene.active && !scene.path.empty())
        {
            if (SceneManager::Instance().LoadScene(scene.path.string()))
            {
                m_projectStatus = "Scene enabled: " + scene.relativePath;
                Tracenf("[HIERARCHY] Scene enabled: %s", scene.relativePath.c_str());
            }
        }
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.36f, 0.36f, 1.0f));
    if (ImGui::SmallButton(ICON_FA_TRASH))
        DetachSceneFromHierarchy(scene);
    ImGui::PopStyleColor();

    if (hasChildren && open)
    {
        for (const HierarchySceneEntity& entity : m_hierarchyEntities)
        {
            if (entity.parent == m_sceneRootEntity || entity.parent == 0)
                RenderHierarchyEntityNode(entity.entity);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void EditorImGui::RenderHierarchyContextMenu(const HierarchySceneEntity& entity)
{
    ImGui::TextUnformatted(entity.name.c_str());
    ImGui::Separator();

    if (ImGui::MenuItem(ICON_FA_BULLSEYE " Focus Camera", "F"))
        QueueHierarchyFocus(entity);
    if (ImGui::MenuItem(ICON_FA_COPY " Duplicate"))
    {
        m_commands.hierarchyDuplicateEntity = true;
        m_commands.hierarchyEntityType = entity.type;
        m_commands.hierarchyEntityId = entity.objectId;
        m_commands.hierarchyEntityHandle = entity.entity;
        Tracenf("[HIERARCHY] Duplicate requested: flecs=%llu object=%u type=%d",
            static_cast<unsigned long long>(entity.entity),
            entity.objectId,
            static_cast<int>(entity.type));
    }
    if (ImGui::MenuItem(ICON_FA_PEN " Rename", "F2"))
        StartHierarchyRename(entity);

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.30f, 1.0f));
    if (ImGui::MenuItem(ICON_FA_TRASH " Delete", "Del"))
    {
        m_commands.hierarchyDeleteEntity = true;
        m_commands.hierarchyEntityType = entity.type;
        m_commands.hierarchyEntityId = entity.objectId;
        m_commands.hierarchyEntityHandle = entity.entity;
        Tracenf("[HIERARCHY] Delete requested: flecs=%llu object=%u type=%d",
            static_cast<unsigned long long>(entity.entity),
            entity.objectId,
            static_cast<int>(entity.type));
    }
    ImGui::PopStyleColor();
}

void EditorImGui::RenderHierarchyEntityNode(std::uint64_t entityHandle)
{
    const HierarchySceneEntity* entity = FindHierarchyEntity(entityHandle);
    if (!entity)
        return;
    if (m_hierarchySearchBuffer[0] != '\0' && !HierarchySubtreePassesSearch(entityHandle))
        return;

    std::vector<std::uint64_t> children;
    for (const HierarchySceneEntity& candidate : m_hierarchyEntities)
    {
        if (candidate.parent == entityHandle)
            children.push_back(candidate.entity);
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushID(static_cast<int>(entity->entity & 0xffffffffu));

    if (entity->editorHidden)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.60f, 0.60f, 1.0f));

    const bool selected = entity->selected || m_selectedHierarchyEntity == entity->entity;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
    if (children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected)
        flags |= ImGuiTreeNodeFlags_Selected;

    const char* icon = ICON_FA_CUBE;
    if (entity->type == HierarchyEntityType::Terrain)
        icon = ICON_FA_MOUNTAIN;
    else if (entity->type == HierarchyEntityType::WaterBody)
        icon = ICON_FA_DROPLET;
    else if (entity->type == HierarchyEntityType::PointLight)
        icon = ICON_FA_LIGHTBULB;
    else if (entity->type == HierarchyEntityType::SpotLight)
        icon = ICON_FA_BULLSEYE;
    else if (entity->type == HierarchyEntityType::MeshEntity)
        icon = ICON_FA_CUBE;

    bool open = false;
    if (m_hierarchyRenamingEntity == entity->entity)
    {
        ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename",
                m_hierarchyRenameBuffer,
                sizeof(m_hierarchyRenameBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            m_commands.hierarchyRenameEntity = true;
            m_commands.hierarchyEntityType = entity->type;
            m_commands.hierarchyEntityId = entity->objectId;
            m_commands.hierarchyEntityHandle = entity->entity;
            m_commands.hierarchyRenameValue = m_hierarchyRenameBuffer;
            m_hierarchyRenamingEntity = 0;
            Tracenf("[HIERARCHY] Rename requested: flecs=%llu new_name=%s",
                static_cast<unsigned long long>(entity->entity),
                m_hierarchyRenameBuffer);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            m_hierarchyRenamingEntity = 0;
        ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
    }
    else
    {
        const std::string label = std::string(icon) + " " + entity->name + "##" + std::to_string(entity->entity);
        open = ImGui::TreeNodeEx(label.c_str(), flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            QueueHierarchySelection(*entity);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                QueueHierarchyFocus(*entity);
        }
        if (selected)
            ImGui::SetScrollHereY(0.5f);
    }

    if (entity->editorHidden)
        ImGui::PopStyleColor();

    if (ImGui::BeginPopupContextItem("##hierarchy_context"))
    {
        RenderHierarchyContextMenu(*entity);
        ImGui::EndPopup();
    }

    ImGui::TableSetColumnIndex(1);
    const ImVec4 eyeColor = entity->editorHidden ? ImVec4(0.48f, 0.48f, 0.48f, 1.0f) : ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, eyeColor);
    if (ImGui::SmallButton(entity->editorHidden ? ICON_FA_EYE_SLASH : ICON_FA_EYE))
    {
        m_commands.hierarchyToggleHidden = true;
        m_commands.hierarchyEntityType = entity->type;
        m_commands.hierarchyEntityId = entity->objectId;
        m_commands.hierarchyEntityHandle = entity->entity;
        Tracenf("[HIERARCHY] Toggle visibility requested: flecs=%llu object=%u type=%d",
            static_cast<unsigned long long>(entity->entity),
            entity->objectId,
            static_cast<int>(entity->type));
    }
    ImGui::PopStyleColor();

    if (!children.empty() && open)
    {
        for (std::uint64_t child : children)
            RenderHierarchyEntityNode(child);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void EditorImGui::RenderHierarchyPanel()
{
    if (ImGui::Begin(ICON_FA_LIST_TREE " Hierarchy"))
    {
        RenderHierarchyToolbar();
        ImGui::Separator();

        const std::vector<ProjectSceneEntry> projectScenes = QueryProjectScenes();
        if (projectScenes.empty())
        {
            ImGui::TextDisabled("No scene open");
            ImGui::TextWrapped(ProjectManager::Instance().HasProject()
                    ? "Create a scene from File to add it to this project."
                    : "Open a project or create a scene from File.");
        }
        else if (ImGui::BeginTable("HierarchyEntityTree", 2,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Visibility", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableHeadersRow();

            if (ProjectManager::Instance().HasProject())
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const ProjectData& project = ProjectManager::Instance().CurrentProject();
                const std::string rootLabel = std::string(ICON_FA_FOLDER_OPEN) + " " + project.name;
                const bool projectOpen = ImGui::TreeNodeEx(rootLabel.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth);
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
                    {
                        const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                        const std::vector<AssetLibrary::Entry> sceneAssets = QuerySceneAssets();
                        const auto sceneIt = std::find_if(sceneAssets.begin(), sceneAssets.end(), [&assetId](const AssetLibrary::Entry& entry) {
                            return entry.id == assetId;
                        });
                        if (sceneIt != sceneAssets.end())
                            AttachSceneToHierarchy(*sceneIt);
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("-");
                if (projectOpen)
                {
                    for (const ProjectSceneEntry& scene : projectScenes)
                        RenderProjectSceneNode(scene);
                    ImGui::TreePop();
                }
            }
            else
            {
                for (const ProjectSceneEntry& scene : projectScenes)
                    RenderProjectSceneNode(scene);
            }
            ImGui::EndTable();

            if (!m_logHierarchyRendered)
            {
                m_logHierarchyRendered = true;
                Tracenf("[HIERARCHY] Project scene tree rendered, root=%llu scenes=%zu entities=%zu",
                    static_cast<unsigned long long>(m_sceneRootEntity),
                    projectScenes.size(),
                    m_hierarchyEntities.size());
            }
        }
    }
    ImGui::End();
}
void EditorImGui::RenderToolsPanel()
{
    if (ImGui::Begin("Tools"))
    {
        UI::SectionHeader(ICON_FA_WRENCH " Tools");
        const bool toolsEnabled = CanUseEditorTools();
        if (!toolsEnabled)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.30f, 1.0f), "Tools disabled in Play Mode");
            ImGui::BeginDisabled();
        }
        if (UI::IconButton(ICON_FA_DROPLET, "Water", ImVec2(-1.0f, 0.0f)))
        {
            m_commands.addWaterBody = true;
            Tracen("[EDITOR-3D-SPAWN] Add water requested");
        }

        ImGui::Separator();
        UI::SectionHeader(ICON_FA_HAMMER " Editing");
        if (UI::IconButton(ICON_FA_WATER, "Water Sculpt", ImVec2(-1.0f, 0.0f)))
            m_waterSculptToolOpen = !m_waterSculptToolOpen;
        if (UI::IconButton(ICON_FA_MOUNTAIN, "Heightmap", ImVec2(-1.0f, 0.0f)))
            m_heightmapToolOpen = !m_heightmapToolOpen;
        if (UI::IconButton(ICON_FA_PAINTBRUSH, "Splat Paint", ImVec2(-1.0f, 0.0f)))
            m_splatPaintToolOpen = !m_splatPaintToolOpen;

        ImGui::Separator();
        if (UI::IconButton(ICON_FA_UNDO, "Undo", ImVec2(-1.0f, 0.0f)))
            m_commands.undo = true;
        if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Save", ImVec2(-1.0f, 0.0f)))
            m_commands.save = true;
        if (UI::IconButton(ICON_FA_ROTATE, "Reload", ImVec2(-1.0f, 0.0f)))
            m_commands.reload = true;

        if (!toolsEnabled)
            ImGui::EndDisabled();
    }
    ImGui::End();

    if (!m_logToolsRendered)
    {
        m_logToolsRendered = true;
        Tracen("[EDITOR-IMGUI-2] Tools panel rendered");
    }
}

void EditorImGui::MarkSelectedWaterBodyChanged()
{
    if (!m_waterBodyState.selected)
        return;
    m_waterBodyState.center[1] = m_waterBodyState.config.waterLevelY;
    SceneManager::Instance().MarkDirty();
    m_commands.selectedWaterBodyChanged = true;
    m_commands.selectedWaterBody = m_waterBodyState;
}

void EditorImGui::MarkSelectedLightChanged()
{
    if (m_dynamicLightState.type != DynamicLightType::Point &&
        m_dynamicLightState.type != DynamicLightType::Spot)
    {
        return;
    }
    SceneManager::Instance().MarkDirty();
    m_commands.selectedLightChanged = true;
    m_commands.selectedLight = m_dynamicLightState;
}

void EditorImGui::MarkSelectedMeshRendererChanged()
{
    if (!m_meshRendererState.selected)
        return;
    SceneManager::Instance().MarkDirty();
    m_commands.selectedMeshEntityChanged = true;
    m_commands.selectedMeshEntity = m_meshRendererState;
}

bool EditorImGui::RenderAxisFloat(const char* axis,
                                  float& value,
                                  float r,
                                  float g,
                                  float b,
                                  float speed,
                                  float minValue,
                                  float maxValue)
{
    ImGui::PushID(axis);
    ImGui::TextColored(ImVec4(r, g, b, 1.0f), "%s", axis);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::DragFloat("##value", &value, speed, minValue, maxValue, "%.2f");
    ImGui::PopID();
    return changed;
}

bool EditorImGui::RenderTransformComponent(float* position, float* rotation, float* scale)
{
    bool changed = false;
    if (ImGui::CollapsingHeader(ICON_FA_CUBE " Transform", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Position");
        changed |= RenderAxisFloat("X", position[0], 0.86f, 0.25f, 0.25f, 0.1f, -500.0f, 500.0f);
        changed |= RenderAxisFloat("Y", position[1], 0.30f, 0.78f, 0.34f, 0.1f, -100.0f, 200.0f);
        changed |= RenderAxisFloat("Z", position[2], 0.28f, 0.45f, 0.92f, 0.1f, -500.0f, 500.0f);

        if (rotation)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Rotation");
            changed |= RenderAxisFloat("Pitch", rotation[0], 0.86f, 0.25f, 0.25f, 0.5f, -90.0f, 90.0f);
            changed |= RenderAxisFloat("Yaw", rotation[1], 0.30f, 0.78f, 0.34f, 0.5f, -180.0f, 180.0f);
            changed |= RenderAxisFloat("Roll", rotation[2], 0.28f, 0.45f, 0.92f, 0.5f, -180.0f, 180.0f);
        }

        if (scale)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Scale");
            changed |= RenderAxisFloat("W", scale[0], 0.86f, 0.25f, 0.25f, 0.1f, 0.1f, 200.0f);
            changed |= RenderAxisFloat("H", scale[1], 0.30f, 0.78f, 0.34f, 0.1f, 0.1f, 200.0f);
            changed |= RenderAxisFloat("D", scale[2], 0.28f, 0.45f, 0.92f, 0.1f, 0.1f, 200.0f);
        }
    }
    return changed;
}

void EditorImGui::RenderAddComponentMenu()
{
    const bool hasWater = m_waterBodyState.selected;
    const bool hasPoint = m_dynamicLightState.type == DynamicLightType::Point;
    const bool hasSpot = m_dynamicLightState.type == DynamicLightType::Spot;
    const bool hasMesh = m_meshRendererState.selected;
    const bool hasSelection = hasWater || hasPoint || hasSpot || hasMesh;

    if (!hasSelection)
    {
        ImGui::BeginDisabled();
        UI::IconButton(ICON_FA_PLUS, "Add Component", ImVec2(-1.0f, 0.0f));
        ImGui::EndDisabled();
        return;
    }

    if (UI::IconButton(ICON_FA_PLUS, "Add Component", ImVec2(-1.0f, 0.0f)))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        if (hasWater)
            ImGui::BeginDisabled();
        if (ImGui::MenuItem(ICON_FA_DROPLET " Water Body"))
        {
            m_commands.addComponentToSelectedEntity = true;
            m_commands.addComponentType = EditorComponentType::WaterBody;
        }
        if (hasWater)
            ImGui::EndDisabled();

        if (hasPoint)
            ImGui::BeginDisabled();
        if (ImGui::MenuItem(ICON_FA_LIGHTBULB " Point Light"))
        {
            m_commands.addComponentToSelectedEntity = true;
            m_commands.addComponentType = EditorComponentType::PointLight;
        }
        if (hasPoint)
            ImGui::EndDisabled();

        if (hasSpot)
            ImGui::BeginDisabled();
        if (ImGui::MenuItem(ICON_FA_BULLSEYE " Spot Light"))
        {
            m_commands.addComponentToSelectedEntity = true;
            m_commands.addComponentType = EditorComponentType::SpotLight;
        }
        if (hasSpot)
            ImGui::EndDisabled();

        if (hasMesh)
            ImGui::BeginDisabled();
        if (ImGui::MenuItem(ICON_FA_CUBE " MeshRenderer"))
        {
            m_commands.addComponentToSelectedEntity = true;
            m_commands.addComponentType = EditorComponentType::MeshRenderer;
        }
        if (hasMesh)
            ImGui::EndDisabled();

        ImGui::EndPopup();
    }
}

void EditorImGui::RenderSelectedWaterBodyInspector()
{
    if (!m_waterBodyState.selected)
        return;

    UI::SectionHeader(ICON_FA_CUBE " Entity");
    ImGui::TextDisabled("flecs=%llu  object=%u",
        static_cast<unsigned long long>(m_selectedHierarchyEntity),
        m_waterBodyState.id);

    char nameBuffer[96]{};
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_waterBodyState.name.c_str());
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
    {
        m_waterBodyState.name = nameBuffer[0] != '\0' ? nameBuffer : ("Water_" + std::to_string(m_waterBodyState.id));
        MarkSelectedWaterBodyChanged();
    }

    RenderAddComponentMenu();

    float position[3] = {m_waterBodyState.center[0], m_waterBodyState.config.waterLevelY, m_waterBodyState.center[2]};
    float scale[3] = {m_waterBodyState.width, 1.0f, m_waterBodyState.depth};
    if (RenderTransformComponent(position, nullptr, scale))
    {
        m_waterBodyState.center[0] = position[0];
        m_waterBodyState.config.waterLevelY = position[1];
        m_waterBodyState.center[1] = position[1];
        m_waterBodyState.center[2] = position[2];
        m_waterBodyState.width = scale[0];
        m_waterBodyState.depth = scale[2];
        MarkSelectedWaterBodyChanged();
    }

    if (!ImGui::CollapsingHeader(ICON_FA_WATER " Water Body", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const std::string materialLabel = m_waterBodyState.materialName.empty()
        ? (m_waterBodyState.materialId.empty() ? std::string("Inline Water") : m_waterBodyState.materialId)
        : m_waterBodyState.materialName;
    ImGui::TextUnformatted("Material");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.46f, 1.0f));
    ImGui::Button(materialLabel.c_str(), ImVec2(-1.0f, 42.0f));
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            AssignAssetToSelectedWaterBody(assetId);
        }
        ImGui::EndDragDropTarget();
    }

    if (UI::IconButton(ICON_FA_PALETTE, "Edit Material"))
    {
        m_commands.selectedWaterBodyChanged = true;
        m_commands.selectedWaterBody = m_waterBodyState;
        m_commands.openSelectedWaterMaterialEditor = true;
        OpenWaterMaterialEditor(m_waterBodyState.materialId);
    }
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_FOLDER_OPEN, "Change Material"))
        ImGui::OpenPopup("ChangeWaterMaterial");

    if (ImGui::BeginPopup("ChangeWaterMaterial"))
    {
        ImGui::TextUnformatted("Select water material:");
        ImGui::Separator();
        for (const auto& material : m_waterMaterials)
        {
            const bool selected = material.first == m_waterBodyState.materialId;
            if (ImGui::Selectable(material.first.c_str(), selected))
            {
                const std::string oldId = m_waterBodyState.materialId;
                m_waterBodyState.materialId = material.first;
                m_waterBodyState.materialName = material.first;
                Tracenf("[EDITOR-IMGUI-2] Material change: body_id=%u from=%s to=%s",
                    m_waterBodyState.id,
                    oldId.c_str(),
                    m_waterBodyState.materialId.c_str());
                MarkSelectedWaterBodyChanged();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    if (UI::IconButton(ICON_FA_TRASH, "Delete Water Body", ImVec2(-1.0f, 0.0f)))
        m_commands.deleteSelectedWaterBody = true;
}

void EditorImGui::RenderSelectedLightInspector()
{
    if (m_dynamicLightState.type == DynamicLightType::None)
        return;

    const bool isPoint = m_dynamicLightState.type == DynamicLightType::Point;
    const bool isSpot = m_dynamicLightState.type == DynamicLightType::Spot;
    UI::SectionHeader(ICON_FA_CUBE " Entity");
    ImGui::TextDisabled("flecs=%llu  object=%u",
        static_cast<unsigned long long>(m_selectedHierarchyEntity),
        m_dynamicLightState.id);

    if (isPoint)
    {
        PointLight& point = m_dynamicLightState.point;
        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", PointLightDisplayName(point).c_str());
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
        {
            point.name = nameBuffer;
            MarkSelectedLightChanged();
        }
        RenderAddComponentMenu();

        float position[3] = {point.position[0], point.position[1], point.position[2]};
        float scale[3] = {point.radius, point.radius, point.radius};
        if (RenderTransformComponent(position, nullptr, scale))
        {
            point.position[0] = position[0];
            point.position[1] = position[1];
            point.position[2] = position[2];
            point.radius = std::clamp(scale[0], 0.5f, 100.0f);
            MarkSelectedLightChanged();
        }

        if (!ImGui::CollapsingHeader(ICON_FA_LIGHTBULB " Point Light", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        bool changed = false;
        changed |= ImGui::Checkbox("Enabled", &point.enabled);
        changed |= ImGui::ColorEdit3("Color", &point.r);
        changed |= ImGui::SliderFloat("Intensity", &point.intensity, 0.0f, 10.0f, "%.2f");
        changed |= ImGui::SliderFloat("Radius", &point.radius, 0.5f, 100.0f, "%.1f m");
        if (changed)
            MarkSelectedLightChanged();
    }
    else if (isSpot)
    {
        SpotLight& spot = m_dynamicLightState.spot;
        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", SpotLightDisplayName(spot).c_str());
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
        {
            spot.name = nameBuffer;
            MarkSelectedLightChanged();
        }
        RenderAddComponentMenu();

        float position[3] = {spot.position[0], spot.position[1], spot.position[2]};
        float rotation[3] = {spot.rotation[0] * 57.2957795f, spot.rotation[1] * 57.2957795f, spot.rotation[2] * 57.2957795f};
        float scale[3] = {spot.radius, spot.radius, spot.radius};
        if (RenderTransformComponent(position, rotation, scale))
        {
            spot.position[0] = position[0];
            spot.position[1] = position[1];
            spot.position[2] = position[2];
            spot.rotation[0] = rotation[0] / 57.2957795f;
            spot.rotation[1] = rotation[1] / 57.2957795f;
            spot.rotation[2] = rotation[2] / 57.2957795f;
            spot.radius = std::clamp(scale[0], 0.5f, 100.0f);
            MarkSelectedLightChanged();
        }

        if (!ImGui::CollapsingHeader(ICON_FA_BULLSEYE " Spot Light", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        bool changed = false;
        changed |= ImGui::Checkbox("Enabled", &spot.enabled);
        changed |= ImGui::ColorEdit3("Color", &spot.r);
        changed |= ImGui::SliderFloat("Intensity", &spot.intensity, 0.0f, 10.0f, "%.2f");
        changed |= ImGui::SliderFloat("Radius", &spot.radius, 0.5f, 100.0f, "%.1f m");
        float pitch = spot.rotation[0] * 57.2957795f;
        float yaw = spot.rotation[1] * 57.2957795f;
        if (ImGui::SliderFloat("Pitch", &pitch, -90.0f, 90.0f, "%.1f deg"))
        {
            spot.rotation[0] = pitch / 57.2957795f;
            changed = true;
        }
        if (ImGui::SliderFloat("Yaw", &yaw, -180.0f, 180.0f, "%.1f deg"))
        {
            spot.rotation[1] = yaw / 57.2957795f;
            changed = true;
        }
        changed |= ImGui::SliderFloat("Inner Cone", &spot.innerConeDegrees, 1.0f, 89.0f, "%.1f deg");
        changed |= ImGui::SliderFloat("Outer Cone", &spot.outerConeDegrees, 1.0f, 90.0f, "%.1f deg");
        spot.outerConeDegrees = std::max(spot.outerConeDegrees, spot.innerConeDegrees);
        if (changed)
            MarkSelectedLightChanged();
    }

    ImGui::Separator();
    if (UI::IconButton(ICON_FA_TRASH, "Delete Light", ImVec2(-1.0f, 0.0f)))
        m_commands.deleteSelectedLight = true;
}

void EditorImGui::RenderSelectedMeshRendererInspector()
{
    if (!m_meshRendererState.selected)
        return;

    UI::SectionHeader(ICON_FA_CUBE " Entity");
    ImGui::TextDisabled("flecs=%llu  object=%u",
        static_cast<unsigned long long>(m_selectedHierarchyEntity),
        m_meshRendererState.id);

    char nameBuffer[96]{};
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_meshRendererState.name.c_str());
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
    {
        m_meshRendererState.name = nameBuffer[0] != '\0' ? nameBuffer : ("Mesh Entity " + std::to_string(m_meshRendererState.id));
        MarkSelectedMeshRendererChanged();
    }

    RenderAddComponentMenu();

    float position[3] = {m_meshRendererState.position[0], m_meshRendererState.position[1], m_meshRendererState.position[2]};
    float rotation[3] = {
        m_meshRendererState.rotation[0] * 57.2957795f,
        m_meshRendererState.rotation[1] * 57.2957795f,
        m_meshRendererState.rotation[2] * 57.2957795f};
    float scale[3] = {m_meshRendererState.scale[0], m_meshRendererState.scale[1], m_meshRendererState.scale[2]};
    if (RenderTransformComponent(position, rotation, scale))
    {
        m_meshRendererState.position[0] = position[0];
        m_meshRendererState.position[1] = position[1];
        m_meshRendererState.position[2] = position[2];
        m_meshRendererState.rotation[0] = rotation[0] / 57.2957795f;
        m_meshRendererState.rotation[1] = rotation[1] / 57.2957795f;
        m_meshRendererState.rotation[2] = rotation[2] / 57.2957795f;
        m_meshRendererState.scale[0] = std::max(scale[0], 0.001f);
        m_meshRendererState.scale[1] = std::max(scale[1], 0.001f);
        m_meshRendererState.scale[2] = std::max(scale[2], 0.001f);
        MarkSelectedMeshRendererChanged();
    }

    if (ImGui::CollapsingHeader(ICON_FA_CUBE " MeshRenderer", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const std::string meshLabel = m_meshRendererState.meshDisplayName.empty()
            ? (m_meshRendererState.meshAssetId.empty() ? std::string("No model assigned") : m_meshRendererState.meshAssetId)
            : m_meshRendererState.meshDisplayName;
        ImGui::TextUnformatted("Mesh");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.34f, 0.64f, 1.0f));
        ImGui::Button(meshLabel.c_str(), ImVec2(-1.0f, 42.0f));
        ImGui::PopStyleColor();
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
            {
                const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                AssignAssetToSelectedMeshRenderer(assetId);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::TextDisabled("Asset ID: %s", m_meshRendererState.meshAssetId.empty() ? "<none>" : m_meshRendererState.meshAssetId.c_str());
        ImGui::TextDisabled("Path: %s", m_meshRendererState.meshAssetPath.empty() ? "<none>" : m_meshRendererState.meshAssetPath.c_str());
        ImGui::TextDisabled("Render path: %s", m_meshRendererState.skinned ? "SkinnedMeshRenderer" : "static mesh pending");
    }

    ImGui::Separator();
    if (UI::IconButton(ICON_FA_TRASH, "Delete Mesh Entity", ImVec2(-1.0f, 0.0f)))
        m_commands.deleteSelectedMeshEntity = true;
}

void EditorImGui::ApplyTimeOfDayPreset(float hour)
{
    if (hour < 6.0f)
    {
        m_lightingState.directional.azimuthDegrees = 180.0f;
        m_lightingState.directional.elevationDegrees = 4.0f;
        m_lightingState.directional.intensity = 0.2f;
        m_lightingState.ambient.intensity = 0.35f;
    }
    else if (hour < 10.0f)
    {
        m_lightingState.directional.azimuthDegrees = 90.0f;
        m_lightingState.directional.elevationDegrees = 18.0f;
        m_lightingState.directional.intensity = 0.9f;
        m_lightingState.ambient.intensity = 0.6f;
    }
    else if (hour < 17.0f)
    {
        m_lightingState.directional.azimuthDegrees = 180.0f;
        m_lightingState.directional.elevationDegrees = 75.0f;
        m_lightingState.directional.intensity = 1.2f;
        m_lightingState.ambient.intensity = 0.8f;
    }
    else
    {
        m_lightingState.directional.azimuthDegrees = 270.0f;
        m_lightingState.directional.elevationDegrees = 10.0f;
        m_lightingState.directional.intensity = 0.7f;
        m_lightingState.ambient.intensity = 0.7f;
    }
}

void EditorImGui::RenderLightingPanel()
{
    UI::SectionHeader(ICON_FA_LIGHTBULB " Directional Light");
    ImGui::Checkbox("Sun Enabled", &m_lightingState.directional.enabled);
    ImGui::Checkbox("Sun Shadows", &m_lightingState.sunShadowsEnabled);
    ImGui::ColorEdit3("Sun Color", &m_lightingState.directional.r);
    ImGui::SliderFloat("Sun Intensity", &m_lightingState.directional.intensity, 0.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("Sun Angle X", &m_lightingState.directional.elevationDegrees, -90.0f, 90.0f, "%.1f deg");
    ImGui::SliderFloat("Sun Angle Y", &m_lightingState.directional.azimuthDegrees, 0.0f, 360.0f, "%.1f deg");

    ImGui::Spacing();
    UI::SectionHeader(ICON_FA_GEAR " Ambient Light");
    ImGui::ColorEdit3("Ambient Color", &m_lightingState.ambient.r);
    ImGui::SliderFloat("Ambient Intensity", &m_lightingState.ambient.intensity, 0.0f, 3.0f, "%.2f");

    ImGui::Spacing();
    UI::SectionHeader(ICON_FA_GEAR " Time of Day");
    ImGui::SliderFloat("Hour", &m_timeOfDayHours, 0.0f, 24.0f, "%.1f h");
    if (UI::IconButton(ICON_FA_CHECK, "Apply Preset"))
        ApplyTimeOfDayPreset(m_timeOfDayHours);
}

void EditorImGui::RenderDynamicLightsPanel()
{
    const uint32_t total = m_lightingState.numPointLights + m_lightingState.numSpotLights;
    ImGui::Text("Total: %u / %u", total, kMaxDynamicPointLights + kMaxDynamicSpotLights);

    if (UI::IconButton(ICON_FA_LIGHTBULB, "Point Light"))
    {
        m_commands.addPointLight = true;
        Tracen("[EDITOR-IMGUI-2] Light spawn: type=point id=pending");
    }
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_LIGHTBULB, "Spot Light"))
    {
        m_commands.addSpotLight = true;
        Tracen("[EDITOR-IMGUI-2] Light spawn: type=spot id=pending");
    }
}

void EditorImGui::RenderWorldPanel()
{
    if (ImGui::Begin(ICON_FA_GLOBE " World"))
    {
        if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen))
            RenderPerformancePanel();
        if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (UI::IconButton(ICON_FA_MOUNTAIN, "Create Terrain"))
            {
                if (m_terrainState.exists)
                    m_replaceTerrainConfirmOpen = true;
                else
                    m_createTerrainModalOpen = true;
            }
            if (m_terrainState.exists)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%.0fm x %.0fm, %.2fm/cell",
                    m_terrainState.widthMeters,
                    m_terrainState.depthMeters,
                    m_terrainState.cellSizeMeters);
            }
        }
        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
            RenderLightingPanel();
        if (ImGui::CollapsingHeader("Dynamic Lights", ImGuiTreeNodeFlags_DefaultOpen))
            RenderDynamicLightsPanel();
    }
    ImGui::End();
    RenderCreateTerrainModal();
}

void EditorImGui::RenderPerformancePanel()
{
    const double frameMs = m_engineStats.averageFrameMs > 0.0 ? m_engineStats.averageFrameMs : m_engineStats.frameMs;
    ImGui::Text("FPS: %.1f", m_engineStats.fps);
    ImGui::Text("Frame: %.2f ms  (min %.2f / max %.2f)",
        frameMs,
        m_engineStats.minFrameMs,
        m_engineStats.maxFrameMs);
    ImGui::Text("Swapchain: %u x %u",
        m_engineStats.swapchainWidth,
        m_engineStats.swapchainHeight);

    const float budgetFraction = static_cast<float>(std::clamp(m_engineStats.frameBudgetPercent / 100.0, 0.0, 1.0));
    const std::string budgetLabel =
        std::to_string(static_cast<int>(std::round(m_engineStats.frameBudgetPercent))) + "%";
    ImGui::TextUnformatted("Frame Budget @60 FPS");
    ImGui::ProgressBar(budgetFraction, ImVec2(-1.0f, 0.0f), budgetLabel.c_str());

    const float cpuFraction = static_cast<float>(std::clamp(m_engineStats.processCpuPercent / 100.0, 0.0, 1.0));
    const std::string cpuLabel =
        std::to_string(static_cast<int>(std::round(m_engineStats.processCpuPercent))) + "%";
    ImGui::TextUnformatted("Process CPU");
    ImGui::ProgressBar(cpuFraction, ImVec2(-1.0f, 0.0f), cpuLabel.c_str());

    ImGui::TextDisabled("Frame #%llu | scene entities=%zu | static submitted=%zu drawcalls=%zu",
        static_cast<unsigned long long>(m_engineStats.frameNumber),
        m_engineStats.sceneEntityCount,
        m_engineStats.staticMeshSubmitted,
        m_engineStats.staticMeshDrawCalls);
}

void EditorImGui::RenderCreateTerrainModal()
{
    if (m_replaceTerrainConfirmOpen)
    {
        ImGui::OpenPopup("Replace Terrain?");
        m_replaceTerrainConfirmOpen = false;
    }
    if (ImGui::BeginPopupModal("Replace Terrain?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("The current scene already has a terrain. Creating a new terrain will replace it.");
        ImGui::Separator();
        if (ImGui::Button("Replace", ImVec2(120.0f, 0.0f)))
        {
            m_createTerrainModalOpen = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_createTerrainModalOpen)
    {
        ImGui::OpenPopup("Create Terrain");
        m_createTerrainModalOpen = false;
    }
    if (ImGui::BeginPopupModal("Create Terrain", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputFloat("Width (m)", &m_createTerrainWidthMeters, 10.0f, 100.0f, "%.1f");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputFloat("Depth (m)", &m_createTerrainDepthMeters, 10.0f, 100.0f, "%.1f");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputFloat("Cell size (m/cell)", &m_createTerrainCellSizeMeters, 0.25f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("Chunk size (cells/chunk)", &m_createTerrainChunkSizeCells, 16, 32);

        m_createTerrainWidthMeters = std::max(1.0f, m_createTerrainWidthMeters);
        m_createTerrainDepthMeters = std::max(1.0f, m_createTerrainDepthMeters);
        m_createTerrainCellSizeMeters = std::max(0.01f, m_createTerrainCellSizeMeters);
        m_createTerrainChunkSizeCells = std::clamp(m_createTerrainChunkSizeCells, 32, 256);
        const std::uint32_t cellsX = std::max(1u,
            static_cast<std::uint32_t>(std::lround(m_createTerrainWidthMeters / m_createTerrainCellSizeMeters)));
        const std::uint32_t cellsZ = std::max(1u,
            static_cast<std::uint32_t>(std::lround(m_createTerrainDepthMeters / m_createTerrainCellSizeMeters)));
        const std::uint32_t chunkSize = static_cast<std::uint32_t>(m_createTerrainChunkSizeCells);
        const std::uint32_t chunksX = (cellsX + chunkSize - 1u) / chunkSize;
        const std::uint32_t chunksZ = (cellsZ + chunkSize - 1u) / chunkSize;
        const std::uint64_t vertexCount =
            static_cast<std::uint64_t>(cellsX + 1u) * static_cast<std::uint64_t>(cellsZ + 1u);
        ImGui::Text("Cells: %u x %u", cellsX, cellsZ);
        ImGui::Text("Chunk grid: %u x %u", chunksX, chunksZ);
        ImGui::Text("Vertices: %llu", static_cast<unsigned long long>(vertexCount));
        if (vertexCount > 4000000ull)
            ImGui::TextColored(ImVec4(0.95f, 0.58f, 0.22f, 1.0f), "Large terrain: this may be heavy to edit/render.");

        ImGui::Separator();
        if (ImGui::Button("Create", ImVec2(120.0f, 0.0f)))
        {
            TerrainSceneData terrain{};
            terrain.exists = true;
            terrain.name = "Terrain";
            terrain.cellSizeMeters = m_createTerrainCellSizeMeters;
            terrain.cellsX = cellsX;
            terrain.cellsZ = cellsZ;
            terrain.chunkSizeCells = chunkSize;
            terrain.widthMeters = static_cast<float>(cellsX) * terrain.cellSizeMeters;
            terrain.depthMeters = static_cast<float>(cellsZ) * terrain.cellSizeMeters;
            m_commands.createTerrain = true;
            m_commands.terrainCreate = terrain;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorImGui::RenderWaterSculptToolPanel()
{
    if (!m_editorModeActive || !m_waterSculptToolOpen)
        return;

    if (ImGui::Begin("Water Sculpt Tool", &m_waterSculptToolOpen))
    {
        if (!m_waterBodyState.selected)
        {
            ImGui::TextWrapped("Select a water body to sculpt its shape.");
        }

        bool active = m_editorSettings.toolMode == MapEditorToolMode::WaterSculpt;
        if (ImGui::Checkbox("Sculpt Mode Active", &active))
            SetToolMode(active ? MapEditorToolMode::WaterSculpt : MapEditorToolMode::None);

        if (!m_waterBodyState.selected)
            ImGui::BeginDisabled();
        const char* modes[] = {"Add Water", "Remove Water"};
        int mode = m_editorSettings.waterSculptAdd ? 0 : 1;
        if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes)))
        {
            m_editorSettings.waterSculptAdd = mode == 0;
            Tracenf("[EDITOR-IMGUI-5] Water sculpt brush mode: %s",
                m_editorSettings.waterSculptAdd ? "add" : "remove");
        }
        ImGui::SliderFloat("Radius", &m_editorSettings.waterSculptRadiusMeters, 0.5f, 20.0f, "%.1f m");
        if (!m_waterBodyState.selected)
            ImGui::EndDisabled();
    }
    ImGui::End();
}

void EditorImGui::RenderHeightmapToolPanel()
{
    if (!m_editorModeActive || !m_heightmapToolOpen)
        return;

    if (ImGui::Begin("Heightmap Tool", &m_heightmapToolOpen))
    {
        bool active = m_editorSettings.toolMode == MapEditorToolMode::Heightmap;
        if (ImGui::Checkbox("Heightmap Editing Active", &active))
            SetToolMode(active ? MapEditorToolMode::Heightmap : MapEditorToolMode::None);

        auto modeRadio = [this](const char* label, MapEditorTool tool) {
            if (ImGui::RadioButton(label, m_editorSettings.tool == tool))
            {
                m_editorSettings.tool = tool;
                if (m_editorSettings.toolMode != MapEditorToolMode::Heightmap)
                    SetToolMode(MapEditorToolMode::Heightmap);
            }
        };
        modeRadio("Raise", MapEditorTool::Raise);
        ImGui::SameLine();
        modeRadio("Lower", MapEditorTool::Lower);
        modeRadio("Smooth", MapEditorTool::Smooth);
        ImGui::SameLine();
        modeRadio("Flatten", MapEditorTool::Flatten);

        ImGui::Separator();
        ImGui::SliderFloat("Radius", &m_editorSettings.brushRadiusMeters, 1.0f, 100.0f, "%.1f m");
        ImGui::SliderFloat("Strength", &m_editorSettings.brushStrength, 0.1f, 5.0f, "%.2f");
        ImGui::SliderFloat("Falloff", &m_editorSettings.brushFalloff, 0.0f, 1.0f, "%.2f");
        if (m_editorSettings.tool == MapEditorTool::Flatten)
            ImGui::SliderFloat("Target Height", &m_editorSettings.flattenTargetY, -50.0f, 100.0f, "%.2f m");
    }
    ImGui::End();
}

void EditorImGui::RenderSplatLayerSlot(std::uint32_t slotIndex)
{
    if (slotIndex >= m_paletteSlots.size())
        return;

    MapEditorPaletteSlot& slot = m_paletteSlots[slotIndex];
    ImGui::PushID(static_cast<int>(slotIndex));
    const bool selected = m_editorSettings.textureSlot == slotIndex;
    const ImVec4 selectedColor = ImVec4(0.20f, 0.34f, 0.56f, 1.0f);
    const ImVec4 selectedHover = ImVec4(0.24f, 0.42f, 0.68f, 1.0f);
    const ImVec4 idleColor = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? selectedColor : idleColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? selectedHover : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, selected ? selectedHover : ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const std::string label = std::to_string(slotIndex) + "##splat_slot";
    if (ImGui::Button(label.c_str(), ImVec2(48.0f, 38.0f)))
    {
        m_editorSettings.textureSlot = slotIndex;
        m_editorSettings.tool = MapEditorTool::Paint;
        if (m_editorSettings.toolMode != MapEditorToolMode::SplatPaint)
            SetToolMode(MapEditorToolMode::SplatPaint);
    }
    ImGui::PopStyleColor(3);

    if (m_assetFilter != AssetBrowserFilter::Scene && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
            if (entry && (entry->category == AssetLibrary::Category::Texture ||
                entry->category == AssetLibrary::Category::Material))
            {
                MapEditorPaletteSlot next = BuildPaletteSlotFromAsset(slotIndex, *entry);
                if (!next.texturePath.empty())
                {
                    slot = next;
                    m_commands.paletteSlotChanged = true;
                    m_commands.paletteSlot = slotIndex;
                    m_commands.paletteAssetId = next.assetId;
                    m_commands.paletteTexturePath = next.texturePath;
                    m_commands.paletteSlotData = next;
                    m_assetStatus = "Splat layer texture <- " + entry->displayName;
                    Tracenf("[EDITOR-IMGUI-5] Splat layer texture changed: layer=%u asset_id=%s",
                        slotIndex,
                        entry->id.c_str());
                }
            }
            else
            {
                m_assetStatus = "Splat slots accept texture or material assets";
            }
        }
        ImGui::EndDragDropTarget();
    }

    const std::string name = slot.displayName.empty()
        ? (slot.texturePath.empty() ? std::string("empty") : slot.texturePath)
        : slot.displayName;
    ImGui::TextWrapped("%s", name.c_str());
    ImGui::PopID();
}

void EditorImGui::RenderSplatPaintToolPanel()
{
    if (!m_editorModeActive || !m_splatPaintToolOpen)
        return;

    if (ImGui::Begin("Splat Paint Tool", &m_splatPaintToolOpen))
    {
        bool active = m_editorSettings.toolMode == MapEditorToolMode::SplatPaint;
        if (ImGui::Checkbox("Splat Paint Active", &active))
            SetToolMode(active ? MapEditorToolMode::SplatPaint : MapEditorToolMode::None);

        ImGui::TextUnformatted("Active Layer");
        for (std::uint32_t i = 0; i < m_paletteSlots.size(); ++i)
        {
            ImGui::BeginGroup();
            RenderSplatLayerSlot(i);
            ImGui::EndGroup();
            if (i % 4 != 3)
                ImGui::SameLine();
        }

        ImGui::Separator();
        m_editorSettings.tool = MapEditorTool::Paint;
        int paintMode = m_editorSettings.paintMode == MapEditorPaintMode::Mix ? 1 : 0;
        const char* modes[] = {"Replace", "Mix"};
        if (ImGui::Combo("Paint Mode", &paintMode, modes, IM_ARRAYSIZE(modes)))
            m_editorSettings.paintMode = paintMode == 1 ? MapEditorPaintMode::Mix : MapEditorPaintMode::Replace;
        ImGui::SliderFloat("Radius", &m_editorSettings.brushRadiusMeters, 1.0f, 100.0f, "%.1f m");
        ImGui::SliderFloat("Strength", &m_editorSettings.brushStrength, 0.1f, 1.0f, "%.2f");
        ImGui::SliderFloat("Falloff", &m_editorSettings.brushFalloff, 0.0f, 1.0f, "%.2f");

        ImGui::Separator();
        UI::SectionHeader(ICON_FA_PALETTE " Terrain Material");
        if (m_terrainState.exists)
        {
            bool triplanarEnabled = m_terrainState.triplanarEnabled;
            float triplanarSharpness = std::clamp(m_terrainState.triplanarSharpness, 1.0f, 16.0f);
            float triplanarSlopeThreshold = std::clamp(m_terrainState.triplanarSlopeThreshold, 0.0f, 1.0f);
            float triplanarSlopeTransition = std::clamp(m_terrainState.triplanarSlopeTransition, 0.001f, 1.0f);
            bool triplanarChanged = false;
            triplanarChanged |= ImGui::Checkbox("Triplanar Mapping", &triplanarEnabled);
            triplanarChanged |= ImGui::SliderFloat("Triplanar Sharpness", &triplanarSharpness, 1.0f, 16.0f, "%.2f");
            triplanarChanged |= ImGui::SliderFloat("Slope Threshold", &triplanarSlopeThreshold, 0.0f, 1.0f, "%.3f");
            triplanarChanged |= ImGui::SliderFloat("Slope Transition", &triplanarSlopeTransition, 0.001f, 1.0f, "%.3f");
            if (triplanarChanged)
            {
                m_terrainState.triplanarEnabled = triplanarEnabled;
                m_terrainState.triplanarSharpness = triplanarSharpness;
                m_terrainState.triplanarSlopeThreshold = triplanarSlopeThreshold;
                m_terrainState.triplanarSlopeTransition = triplanarSlopeTransition;
                m_commands.terrainTriplanarChanged = true;
                m_commands.terrainTriplanarEnabled = triplanarEnabled;
                m_commands.terrainTriplanarSharpness = triplanarSharpness;
                m_commands.terrainTriplanarSlopeThreshold = triplanarSlopeThreshold;
                m_commands.terrainTriplanarSlopeTransition = triplanarSlopeTransition;
                SceneManager::Instance().MarkDirty();
            }
        }
        else
        {
            ImGui::TextDisabled("Create a terrain to edit triplanar sampling.");
        }
        ImGui::Separator();
        const std::uint32_t selectedSlot = std::min<std::uint32_t>(m_editorSettings.textureSlot, 7u);
        MapEditorPaletteSlot& materialSlot = m_paletteSlots[selectedSlot];
        ImGui::Text("Layer %u: %s",
            selectedSlot,
            materialSlot.displayName.empty() ? "empty" : materialSlot.displayName.c_str());

        auto commitMaterialParams = [&]() {
            materialSlot.slot = selectedSlot;
            materialSlot.tilingScaleX = std::clamp(materialSlot.tilingScaleX, 0.01f, 64.0f);
            materialSlot.tilingScaleY = std::clamp(materialSlot.tilingScaleY, 0.01f, 64.0f);
            materialSlot.normalStrength = std::clamp(materialSlot.normalStrength, 0.0f, 4.0f);
            materialSlot.roughnessStrength = std::clamp(materialSlot.roughnessStrength, 0.0f, 4.0f);
            m_commands.paletteSlotParamsChanged = true;
            m_commands.paletteSlot = selectedSlot;
            m_commands.paletteSlotData = materialSlot;
            SceneManager::Instance().MarkDirty();
        };

        float tiling = (materialSlot.tilingScaleX + materialSlot.tilingScaleY) * 0.5f;
        if (ImGui::SliderFloat("Tiling", &tiling, 0.05f, 32.0f, "%.2f"))
        {
            materialSlot.tilingScaleX = tiling;
            materialSlot.tilingScaleY = tiling;
            commitMaterialParams();
        }
        if (ImGui::ColorEdit3("Tint", materialSlot.colorTint))
            commitMaterialParams();
        if (ImGui::SliderFloat("Normal Strength", &materialSlot.normalStrength, 0.0f, 3.0f, "%.2f"))
            commitMaterialParams();
        if (ImGui::SliderFloat("Roughness", &materialSlot.roughnessStrength, 0.0f, 2.0f, "%.2f"))
            commitMaterialParams();
        if (ImGui::SliderFloat("Metallic", &materialSlot.metallicStrength, 0.0f, 1.0f, "%.2f"))
            commitMaterialParams();
        if (ImGui::SliderFloat("AO Strength", &materialSlot.aoStrength, 0.0f, 1.0f, "%.2f"))
            commitMaterialParams();
        if (ImGui::SliderFloat2("UV Offset", materialSlot.uvOffset, -10.0f, 10.0f, "%.3f"))
            commitMaterialParams();
        if (ImGui::SliderFloat("UV Rotation", &materialSlot.uvRotationDegrees, -180.0f, 180.0f, "%.1f deg"))
            commitMaterialParams();
    }
    ImGui::End();
}

void EditorImGui::RenderAssetBrowserToolbar()
{
    if (UI::IconButton(ICON_FA_PLUS, "Texture"))
        ImportAssetWithDialog(AssetLibrary::Category::Texture);
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_CUBE, "Model"))
        ImportAssetWithDialog(AssetLibrary::Category::Model);
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_PERSON_RUNNING, "Anim"))
        ImportAssetWithDialog(AssetLibrary::Category::Animation);
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_PALETTE, "Material"))
        CreatePbrMaterialAsset();
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_DROPLET, "Water Mat"))
        CreateWaterMaterialAsset();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##asset_search", "Search assets...", m_assetSearchBuffer, sizeof(m_assetSearchBuffer));
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_XMARK, "Clear"))
    {
        m_assetSearchBuffer[0] = '\0';
        m_activeAssetTags.clear();
    }
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_ROTATE, "Refresh"))
        RefreshAssetLibrary();
}

void EditorImGui::RenderAssetTypeTabs()
{
    const auto tab = [this](const char* label, AssetBrowserFilter filter) {
        const bool active = m_assetFilter == filter;
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_TabActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_TabActive));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_TabActive));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Tab));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_TabActive));
        }

        if (ImGui::Button(label))
        {
            if (m_assetFilter != filter)
            {
                m_assetFilter = filter;
                m_assetSubpath.clear();
                m_activeAssetTags.clear();
                m_selectedAssetId.clear();
            }
        }
        ImGui::PopStyleColor(3);
    };

    tab("All", AssetBrowserFilter::All);
    ImGui::SameLine();
    tab("Textures", AssetBrowserFilter::Texture);
    ImGui::SameLine();
    tab("Models", AssetBrowserFilter::Model);
    ImGui::SameLine();
    tab("Anims", AssetBrowserFilter::Animation);
    ImGui::SameLine();
    tab("Materials", AssetBrowserFilter::Material);
    ImGui::SameLine();
    tab("Water Mats", AssetBrowserFilter::WaterMaterial);
    ImGui::SameLine();
    tab("Scenes", AssetBrowserFilter::Scene);
}

void EditorImGui::RenderAssetFolderNode(const std::string& path, const std::vector<std::string>& folders)
{
    const std::string nodeId = path.empty() ? std::string("__asset_root__") : path;
    ImGui::PushID(nodeId.c_str());

    const bool selected = AssetLibrary::NormalizeSubpath(path) == AssetLibrary::NormalizeSubpath(m_assetSubpath);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected)
        flags |= ImGuiTreeNodeFlags_Selected;

    bool hasChildren = false;
    for (const std::string& folder : folders)
    {
        if (IsDirectChildFolder(path, folder))
        {
            hasChildren = true;
            break;
        }
    }
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf;

    const std::string label = FolderDisplayName(path);
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        m_assetSubpath = AssetLibrary::NormalizeSubpath(path);
        m_selectedAssetId.clear();
    }

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            AssetLibrary::Entry moved{};
            std::string error;
            if (m_assetLibrary && m_assetLibrary->MoveAssetToSubpath(assetId, path, moved, error))
            {
                m_assetStatus = "Moved asset to " + FolderDisplayName(path);
                Tracenf("[EDITOR-IMGUI-3] Drag dropped: asset_id=%s target_type=folder path=%s",
                    assetId.c_str(),
                    path.c_str());
            }
            else
            {
                m_assetStatus = "Move failed: " + error;
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (open)
    {
        for (const std::string& folder : folders)
        {
            if (IsDirectChildFolder(path, folder))
                RenderAssetFolderNode(folder, folders);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

void EditorImGui::RenderAssetFolderPanel()
{
    const bool sceneCategory = m_assetFilter == AssetBrowserFilter::Scene;
    if (UI::IconButton(ICON_FA_HOUSE, "Home"))
    {
        m_assetSubpath.clear();
        m_selectedAssetId.clear();
    }
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_FOLDER_OPEN, "Up"))
    {
        m_assetSubpath = ParentSubpath(m_assetSubpath);
        m_selectedAssetId.clear();
    }

    if (sceneCategory)
        ImGui::BeginDisabled();
    if (UI::IconButton(ICON_FA_FOLDER_PLUS, "Folder"))
        ImGui::OpenPopup("NewAssetFolder");
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_TRASH, "Delete") && !m_assetSubpath.empty())
        DeleteAssetFolder();
    if (sceneCategory)
        ImGui::EndDisabled();

    if (ImGui::BeginPopup("NewAssetFolder"))
    {
        ImGui::InputText("Name", m_newAssetFolderName, sizeof(m_newAssetFolderName));
        if (ImGui::Button("Create"))
        {
            CreateAssetFolder();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();
    const std::vector<std::string> folders = QueryVisibleFolders();
    RenderAssetFolderNode("", folders);
}

void EditorImGui::RenderAssetTile(const AssetLibrary::Entry& entry, float tileSize)
{
    ImGui::PushID(entry.id.c_str());
    const bool selected = entry.id == m_selectedAssetId;
    const ImVec4 categoryColor = AssetCategoryColor(entry.category);
    AssetPreviewTexture* preview = GetAssetPreviewTexture(entry);

    const ImVec2 previewMin = ImGui::GetCursorScreenPos();
    const ImVec2 previewMax(previewMin.x + tileSize, previewMin.y + tileSize);
    ImGui::InvisibleButton("##asset_tile", ImVec2(tileSize, tileSize));
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 baseColor = ImGui::ColorConvertFloat4ToU32(hovered
        ? ImVec4(categoryColor.x + 0.08f, categoryColor.y + 0.08f, categoryColor.z + 0.08f, 1.0f)
        : categoryColor);
    drawList->AddRectFilled(previewMin, previewMax, baseColor, 5.0f);
    if (preview && preview->descriptor)
    {
        drawList->AddImage(
            reinterpret_cast<ImTextureID>(preview->descriptor),
            previewMin,
            previewMax,
            ImVec2(0.0f, 0.0f),
            ImVec2(1.0f, 1.0f),
            IM_COL32_WHITE);
        drawList->AddRectFilled(
            ImVec2(previewMin.x, previewMax.y - 18.0f),
            previewMax,
            IM_COL32(10, 12, 16, 145),
            0.0f);
        drawList->AddText(ImVec2(previewMin.x + 6.0f, previewMax.y - 16.0f),
            IM_COL32(235, 238, 244, 235),
            AssetLibrary::TextureRoleBadge(entry.textureRole));
    }
    else
    {
        const char* icon = AssetCategoryIcon(entry.category);
        if (UI::GetEditorFonts().bold)
            ImGui::PushFont(UI::GetEditorFonts().bold);
        const ImVec2 iconSize = ImGui::CalcTextSize(icon);
        drawList->AddText(
            ImVec2(previewMin.x + (tileSize - iconSize.x) * 0.5f, previewMin.y + (tileSize - iconSize.y) * 0.5f),
            IM_COL32(245, 248, 255, 235),
            icon);
        if (UI::GetEditorFonts().bold)
            ImGui::PopFont();
    }
    drawList->AddRect(previewMin, previewMax,
        selected ? IM_COL32(255, 210, 92, 255) : IM_COL32(55, 60, 70, 255),
        5.0f,
        0,
        selected ? 3.0f : 1.0f);

    if (clicked)
    {
        m_selectedAssetId = entry.id;
        m_assetStatus = "Selected: " + entry.displayName;
        if (doubleClicked && entry.category == AssetLibrary::Category::WaterMaterial)
        {
            OpenWaterMaterialEditor(entry.id);
        }
        else if (doubleClicked && entry.category == AssetLibrary::Category::Material)
        {
            OpenPbrMaterialEditor(entry.id);
        }
        else if (doubleClicked && entry.category == AssetLibrary::Category::Model)
        {
            m_commands.addMeshEntity = true;
            m_commands.meshAssetId = entry.id;
            m_assetStatus = "Mesh entity queued: " + entry.displayName;
            Tracenf("[MESH-ENTITY] Asset browser model spawn queued: asset_id=%s", entry.id.c_str());
        }
        else if (doubleClicked && entry.category == AssetLibrary::Category::Scene)
        {
            if (AttachSceneToHierarchy(entry))
                m_assetStatus = "Scene added to Hierarchy: " + entry.displayName;
        }
    }

    if (ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload(kAssetPayloadType, entry.id.data(), entry.id.size());
        ImGui::Text("%s", entry.displayName.c_str());
        ImGui::TextDisabled("%s", AssetLibrary::CategoryName(entry.category));
        if (m_loggedDragAssetId != entry.id)
        {
            m_loggedDragAssetId = entry.id;
            Tracenf("[EDITOR-IMGUI-3] Drag started: asset_id=%s type=%s",
                entry.id.c_str(),
                AssetLibrary::CategoryName(entry.category));
        }
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginPopupContextItem("AssetTileContext"))
    {
        ImGui::TextDisabled("%s", entry.displayName.c_str());
        if (entry.category == AssetLibrary::Category::Scene)
        {
            if (ImGui::MenuItem("Add to Hierarchy"))
                AttachSceneToHierarchy(entry);
        }
        else
        {
            ImGui::Separator();
            if (ImGui::MenuItem("New Material"))
                CreatePbrMaterialAsset();
            if (ImGui::MenuItem("New Water Material"))
                CreateWaterMaterialAsset();
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Asset"))
                DeleteAsset(entry);
        }
        ImGui::EndPopup();
    }

    const std::string shortName = ShortAssetFilename(entry);
    ImGui::TextUnformatted(shortName.c_str());
    const bool labelHovered = ImGui::IsItemHovered();
    if (hovered || labelHovered)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(entry.filename.empty() ? entry.displayName.c_str() : entry.filename.c_str());
        if (!entry.displayName.empty() && entry.displayName != entry.filename)
            ImGui::TextDisabled("Name: %s", entry.displayName.c_str());
        ImGui::TextDisabled("Type: %s", AssetLibrary::CategoryName(entry.category));
        if (!entry.subpath.empty())
            ImGui::TextDisabled("Folder: %s", entry.subpath.c_str());
        if (entry.category == AssetLibrary::Category::Texture)
        {
            ImGui::TextDisabled("Role: %s", AssetLibrary::TextureRoleName(entry.textureRole));
            if (entry.resolutionWidth > 0 && entry.resolutionHeight > 0)
                ImGui::TextDisabled("Size: %ux%u", entry.resolutionWidth, entry.resolutionHeight);
        }
        if (!entry.thumbnail.empty())
            ImGui::TextDisabled("Preview: %s", entry.thumbnail.c_str());
        if (!entry.tags.empty())
            ImGui::TextDisabled("Tags: %s", AssetLibrary::TagsToCsv(entry.tags).c_str());
        if (!entry.originalPath.empty())
            ImGui::TextDisabled("Source: %s", entry.originalPath.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

void EditorImGui::RenderAssetGrid()
{
    const std::vector<AssetLibrary::Entry> visibleAssets = QueryVisibleAssets();
    const std::vector<std::string> folders = QueryVisibleFolders();
    uint32_t directFolders = 0;
    for (const std::string& folder : folders)
    {
        if (IsDirectChildFolder(m_assetSubpath, folder))
            ++directFolders;
    }

    ImGui::Text("%zu assets, %u folders | %s | %s",
        visibleAssets.size(),
        directFolders,
        AssetFilterName(),
        m_assetSubpath.empty() ? "Home" : m_assetSubpath.c_str());
    ImGui::Separator();

    const float tileSize = 86.0f;
    const float cellWidth = 142.0f;
    const float panelWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float columnStride = cellWidth + ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, static_cast<int>((panelWidth + ImGui::GetStyle().ItemSpacing.x) / columnStride));
    if (ImGui::BeginTable("AssetGridTiles", columns, ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_NoSavedSettings))
    {
        for (int i = 0; i < columns; ++i)
            ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, cellWidth);

        for (const AssetLibrary::Entry& entry : visibleAssets)
        {
            ImGui::TableNextColumn();
            ImGui::BeginGroup();
            RenderAssetTile(entry, tileSize);
            ImGui::EndGroup();
        }
        ImGui::EndTable();
    }

    if (visibleAssets.empty())
        ImGui::TextDisabled("No assets in this view.");

    if (ImGui::BeginPopupContextWindow("AssetGridContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("New Material"))
            CreatePbrMaterialAsset();
        if (ImGui::MenuItem("New Water Material"))
            CreateWaterMaterialAsset();
        ImGui::EndPopup();
    }

    if (!m_assetBrowserLogged)
    {
        m_assetBrowserLogged = true;
        Tracenf("[EDITOR-IMGUI-3] Asset Browser rendered, current_path=%s filter_type=%s visible_assets=%zu",
            m_assetSubpath.c_str(),
            AssetFilterName(),
            visibleAssets.size());
    }
}

void EditorImGui::RenderAssetTagFilters()
{
    const std::vector<std::pair<std::string, std::uint32_t>> tags = QueryVisibleTags();
    ImGui::TextUnformatted("Tags");
    ImGui::Separator();
    for (const auto& tag : tags)
    {
        bool active = std::find(m_activeAssetTags.begin(), m_activeAssetTags.end(), tag.first) != m_activeAssetTags.end();
        const std::string label = tag.first + " (" + std::to_string(tag.second) + ")";
        if (ImGui::Checkbox(label.c_str(), &active))
        {
            if (active)
                m_activeAssetTags.push_back(tag.first);
            else
                m_activeAssetTags.erase(std::remove(m_activeAssetTags.begin(), m_activeAssetTags.end(), tag.first), m_activeAssetTags.end());
        }
    }
    if (!m_activeAssetTags.empty() && ImGui::Button("Clear Filters"))
        m_activeAssetTags.clear();
}

void EditorImGui::RenderAssetBrowser()
{
    if (!m_editorModeActive)
        return;

    if (ImGui::Begin("Asset Browser"))
    {
        if (!m_assetLibrary)
        {
            ImGui::TextDisabled("Asset library is not initialized.");
            ImGui::End();
            return;
        }

        RenderAssetBrowserToolbar();
        ImGui::Separator();
        RenderAssetTypeTabs();
        ImGui::Separator();

        if (!m_assetStatus.empty())
        {
            ImGui::TextDisabled("%s", m_assetStatus.c_str());
            ImGui::Separator();
        }

        const float browserPanelHeight = std::max(140.0f, ImGui::GetContentRegionAvail().y);
        if (ImGui::BeginTable("AssetBrowserLayout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 190.0f);
            ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthFixed, 170.0f);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (ImGui::BeginChild("AssetFoldersScroll", ImVec2(0.0f, browserPanelHeight), false,
                    ImGuiWindowFlags_HorizontalScrollbar))
            {
                RenderAssetFolderPanel();
            }
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(1);
            if (ImGui::BeginChild("AssetGridScroll", ImVec2(0.0f, browserPanelHeight), false,
                    ImGuiWindowFlags_HorizontalScrollbar))
            {
                RenderAssetGrid();
            }
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(2);
            if (ImGui::BeginChild("AssetTagsScroll", ImVec2(0.0f, browserPanelHeight), false,
                    ImGuiWindowFlags_HorizontalScrollbar))
            {
                RenderAssetTagFilters();
            }
            ImGui::EndChild();

            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void EditorImGui::RenderGizmoControls()
{
    auto publish = [this]() {
        m_commands.gizmoSettingsChanged = true;
        m_commands.gizmoOperation = m_gizmoOperation;
        m_commands.gizmoSnapEnabled = m_gizmoSnapEnabled;
        m_commands.gizmoSnapValue = m_gizmoSnapValue;
    };

    ImGui::TextUnformatted("Gizmo");
    ImGui::SameLine();

    auto operationButton = [&](const char* label, const char* tooltip, MapEditorGizmoOperation operation, const char* trace) {
        const bool active = m_gizmoOperation == operation;
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.48f, 0.86f, 1.0f));
        if (ImGui::SmallButton(label))
        {
            m_gizmoOperation = operation;
            publish();
            Tracen(trace);
        }
        if (active)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
    };

    operationButton("W", "Translate", MapEditorGizmoOperation::Translate, "[EDITOR-GIZMO] Gizmo operation changed: translate");
    operationButton("E", "Rotate", MapEditorGizmoOperation::Rotate, "[EDITOR-GIZMO] Gizmo operation changed: rotate");
    operationButton("R", "Scale", MapEditorGizmoOperation::Scale, "[EDITOR-GIZMO] Gizmo operation changed: scale");

    bool snapChanged = ImGui::Checkbox("Snap", &m_gizmoSnapEnabled);
    if (m_gizmoSnapEnabled)
    {
        ImGui::SameLine();
        const char* labels[] = {"0.1", "0.5", "1.0", "5.0"};
        ImGui::SetNextItemWidth(72.0f);
        snapChanged = ImGui::Combo("##GizmoSnap", &m_gizmoSnapIndex, labels, IM_ARRAYSIZE(labels)) || snapChanged;
        constexpr float values[] = {0.1f, 0.5f, 1.0f, 5.0f};
        m_gizmoSnapIndex = std::clamp(m_gizmoSnapIndex, 0, 3);
        m_gizmoSnapValue = values[m_gizmoSnapIndex];
    }
    if (snapChanged)
    {
        publish();
        Tracenf("[EDITOR-GIZMO] Snapping: enabled=%d value=%.2f",
            m_gizmoSnapEnabled ? 1 : 0,
            m_gizmoSnapValue);
    }
}

void EditorImGui::RenderWaterMaterialHeader()
{
    if (!m_assetLibrary)
        return;

    auto entry = m_assetLibrary->FindById(m_waterMaterialEditor.materialId);
    const std::string title = entry ? entry->displayName : m_waterMaterialEditor.materialId;
    UI::SectionHeader(ICON_FA_PALETTE " Water Material");
    ImGui::Text("Editing: %s%s", title.c_str(), m_waterMaterialEditor.dirty ? " *" : "");
    ImGui::InputText("Name", m_waterMaterialEditor.name, sizeof(m_waterMaterialEditor.name));

    if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Save"))
        SaveWaterMaterialEditor();
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_PLUS, "New Material"))
        ImGui::OpenPopup("NewWaterMaterialPopup");
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_TRASH, "Delete"))
        ImGui::OpenPopup("DeleteWaterMaterialConfirm");

    if (ImGui::BeginPopup("NewWaterMaterialPopup"))
    {
        ImGui::InputText("Name", m_waterMaterialEditor.newName, sizeof(m_waterMaterialEditor.newName));
        if (UI::IconButton(ICON_FA_CHECK, "Create"))
        {
            AssetLibrary::Entry created{};
            if (CreateWaterMaterialAsset(m_waterMaterialEditor.newName, created))
                OpenWaterMaterialEditor(created.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (UI::IconButton(ICON_FA_XMARK, "Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("DeleteWaterMaterialConfirm"))
    {
        const auto usageIt = m_waterMaterialUsageCounts.find(m_waterMaterialEditor.materialId);
        const std::uint32_t users = usageIt != m_waterMaterialUsageCounts.end() ? usageIt->second : 0u;
        ImGui::Text("Delete '%s'?", title.c_str());
        if (users > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%u water bodies will use the default material.", users);
        if (UI::IconButton(ICON_FA_TRASH, "Yes, delete"))
        {
            DeleteWaterMaterialEditor();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (UI::IconButton(ICON_FA_XMARK, "Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    if (ImGui::BeginListBox("Water Materials", ImVec2(-1.0f, 110.0f)))
    {
        for (const AssetLibrary::Entry& material : m_assetLibrary->Entries())
        {
            if (material.category != AssetLibrary::Category::WaterMaterial)
                continue;
            const bool selected = material.id == m_waterMaterialEditor.materialId;
            if (ImGui::Selectable(material.displayName.c_str(), selected))
                OpenWaterMaterialEditor(material.id);
        }
        ImGui::EndListBox();
    }
}

void EditorImGui::RenderWaterMaterialColorsSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Colors", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    WaterConfig& config = material.config;
    if (ImGui::Checkbox("Water Enabled", &config.enabled)) MarkWaterMaterialChanged("enabled");
    if (ImGui::ColorEdit4("Base Color", config.baseColor)) MarkWaterMaterialChanged("base_color");
    if (ImGui::ColorEdit3("Deep Color", config.deepColor)) MarkWaterMaterialChanged("deep_color");
    if (ImGui::ColorEdit3("Shallow Color", config.shallowColor)) MarkWaterMaterialChanged("shallow_color");
    if (ImGui::SliderFloat("Color Depth Min", &config.depthColorMin, 0.0f, 50.0f, "%.2f m")) MarkWaterMaterialChanged("depth_color_min");
    if (ImGui::SliderFloat("Color Depth Max", &config.depthColorMax, 0.01f, 50.0f, "%.2f m")) MarkWaterMaterialChanged("depth_color_max");
    config.depthColorMax = std::max(config.depthColorMax, config.depthColorMin + 0.01f);
    if (ImGui::SliderFloat("Fade Distance", &config.depthFadeDistance, 0.01f, 50.0f, "%.2f m")) MarkWaterMaterialChanged("depth_fade_distance");
}

void EditorImGui::RenderWaterMaterialWaveSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Wave", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    WaterConfig& config = material.config;
    if (ImGui::SliderFloat("Wave Scale Small", &config.waveScaleSmall, 0.001f, 0.12f, "%.3f")) MarkWaterMaterialChanged("wave_scale_small");
    if (ImGui::SliderFloat("Wave Scale Large", &config.waveScaleLarge, 0.001f, 0.08f, "%.3f")) MarkWaterMaterialChanged("wave_scale_large");
    if (ImGui::SliderFloat("Wave Speed Small", &config.waveSpeedSmall, 0.0f, 0.5f, "%.3f")) MarkWaterMaterialChanged("wave_speed_small");
    if (ImGui::SliderFloat("Wave Speed Large", &config.waveSpeedLarge, 0.0f, 0.5f, "%.3f")) MarkWaterMaterialChanged("wave_speed_large");
    if (ImGui::SliderFloat("Normal Strength", &config.normalStrength, 0.0f, 2.0f, "%.2f")) MarkWaterMaterialChanged("normal_strength");
    if (ImGui::SliderFloat("Fresnel Power", &config.fresnelPower, 1.0f, 10.0f, "%.2f")) MarkWaterMaterialChanged("fresnel_power");
    if (ImGui::SliderFloat("Fresnel Min", &config.fresnelMin, 0.0f, 0.5f, "%.3f")) MarkWaterMaterialChanged("fresnel_min");
}

void EditorImGui::RenderWaterMaterialFoamSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Foam"))
        return;
    WaterConfig& config = material.config;
    if (ImGui::Checkbox("Foam Enabled", &config.foamEnabled)) MarkWaterMaterialChanged("foam_enabled");
    if (!config.foamEnabled)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Foam Distance", &config.foamDistance, 0.02f, 1.5f, "%.3f m")) MarkWaterMaterialChanged("foam_distance");
    if (ImGui::SliderFloat("Foam Softness", &config.foamSoftness, 0.0f, 0.5f, "%.3f")) MarkWaterMaterialChanged("foam_softness");
    if (ImGui::SliderFloat("Foam Intensity", &config.foamIntensity, 0.0f, 2.0f, "%.2f")) MarkWaterMaterialChanged("foam_intensity");
    if (ImGui::SliderFloat("Foam Scroll Speed", &config.foamScrollSpeed, -1.0f, 1.0f, "%.3f")) MarkWaterMaterialChanged("foam_scroll_speed");
    if (ImGui::SliderFloat("Foam Scale", &config.foamScale, 0.1f, 2.0f, "%.2f")) MarkWaterMaterialChanged("foam_scale");
    if (ImGui::SliderFloat("Terrain Foam Thickness", &config.foamTerrainThickness, 0.0f, 1.0f, "%.3f m")) MarkWaterMaterialChanged("foam_terrain_thickness");
    if (!config.foamEnabled)
        ImGui::EndDisabled();
}

void EditorImGui::RenderWaterMaterialCausticSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Caustic"))
        return;
    WaterConfig& config = material.config;
    const char* modes[] = {"Off", "Animated", "Procedural"};
    int mode = static_cast<int>(config.causticMode);
    if (ImGui::Combo("Caustic Mode", &mode, modes, IM_ARRAYSIZE(modes)))
    {
        config.causticMode = static_cast<WaterConfig::CausticMode>(std::clamp(mode, 0, 2));
        MarkWaterMaterialChanged("caustic_mode");
    }
    if (config.causticMode == WaterConfig::CausticMode::Off)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Caustic Intensity", &config.causticIntensity, 0.0f, 3.0f, "%.2f")) MarkWaterMaterialChanged("caustic_intensity");
    if (ImGui::SliderFloat("Caustic Scale", &config.causticScale, 0.1f, 2.0f, "%.2f m")) MarkWaterMaterialChanged("caustic_scale");
    if (ImGui::SliderFloat("Caustic Speed", &config.causticSpeed, 0.0f, 2.0f, "%.2f")) MarkWaterMaterialChanged("caustic_speed");
    if (ImGui::SliderFloat("Caustic Max Depth", &config.causticMaxDepth, 1.0f, 30.0f, "%.1f m")) MarkWaterMaterialChanged("caustic_max_depth");
    if (config.causticMode == WaterConfig::CausticMode::Off)
        ImGui::EndDisabled();
}

void EditorImGui::RenderWaterMaterialReflectionSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Reflection"))
        return;
    WaterConfig& config = material.config;
    if (ImGui::Checkbox("Reflection Enabled", &config.reflectionEnabled)) MarkWaterMaterialChanged("reflection_enabled");
    if (!config.reflectionEnabled)
        ImGui::BeginDisabled();
    if (ImGui::ColorEdit3("Reflection Color", config.reflectionColor)) MarkWaterMaterialChanged("reflection_color");
    const char* qualities[] = {"Low", "Medium", "High"};
    int quality = static_cast<int>(config.reflectionQuality);
    if (ImGui::Combo("Reflection Quality", &quality, qualities, IM_ARRAYSIZE(qualities)))
    {
        config.reflectionQuality = static_cast<WaterConfig::ReflectionQuality>(std::clamp(quality, 0, 2));
        MarkWaterMaterialChanged("reflection_quality");
    }
    if (ImGui::SliderFloat("Distortion Strength", &config.reflectionDistortionStrength, 0.0f, 0.2f, "%.3f")) MarkWaterMaterialChanged("reflection_distortion_strength");
    if (!config.reflectionEnabled)
        ImGui::EndDisabled();
}

void EditorImGui::RenderWaterMaterialRefractionSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Refraction"))
        return;
    WaterConfig& config = material.config;
    if (ImGui::Checkbox("Refraction Enabled", &config.refractionEnabled)) MarkWaterMaterialChanged("refraction_enabled");
    if (!config.refractionEnabled)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Refraction Strength", &config.refractionStrength, 0.0f, 0.1f, "%.3f")) MarkWaterMaterialChanged("refraction_strength");
    if (ImGui::SliderFloat("Depth Multiplier", &config.refractionDepthStrength, 0.0f, 2.0f, "%.2f")) MarkWaterMaterialChanged("refraction_depth_strength");
    if (!config.refractionEnabled)
        ImGui::EndDisabled();
}

void EditorImGui::RenderWaterMaterialEdgeFadeSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Edge Fade", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    WaterConfig& config = material.config;
    if (ImGui::SliderFloat("Edge Fade Distance", &config.edgeFadeDistance, 0.0f, 3.0f, "%.2f m")) MarkWaterMaterialChanged("edge_fade_distance");
    const char* curves[] = {"Linear", "Smooth", "Exponential"};
    int curve = static_cast<int>(config.edgeFadeCurve);
    if (ImGui::Combo("Edge Fade Curve", &curve, curves, IM_ARRAYSIZE(curves)))
    {
        config.edgeFadeCurve = static_cast<WaterConfig::EdgeFadeCurve>(std::clamp(curve, 0, 2));
        MarkWaterMaterialChanged("edge_fade_curve");
    }
}

void EditorImGui::RenderWaterTextureSlot(const char* label, std::string& texturePath, bool& changed)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    std::string display = texturePath.empty() ? std::string("empty") : texturePath;
    if (m_assetLibrary)
    {
        for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
        {
            if (entry.category == AssetLibrary::Category::Texture && m_assetLibrary->AssetRelativePath(entry) == texturePath)
            {
                display = entry.displayName;
                break;
            }
        }
    }

    ImGui::Button("##texture_slot", ImVec2(70.0f, 70.0f));
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
            if (entry && entry->category == AssetLibrary::Category::Texture)
            {
                texturePath = m_assetLibrary->AssetRelativePath(*entry);
                changed = true;
                Tracenf("[EDITOR-IMGUI-4] Texture slot assigned: material_id=%s slot=%s asset_id=%s",
                    m_waterMaterialEditor.materialId.c_str(),
                    label,
                    entry->id.c_str());
            }
            else
            {
                m_assetStatus = "Texture slot accepts texture assets only";
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", display.c_str());
    if (!texturePath.empty() && ImGui::SmallButton("Clear"))
    {
        texturePath.clear();
        changed = true;
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

void EditorImGui::RenderWaterMaterialTexturesSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Textures"))
        return;

    bool changed = false;
    RenderWaterTextureSlot("Normal Map A", material.normalMapA, changed);
    RenderWaterTextureSlot("Normal Map B", material.normalMapB, changed);
    RenderWaterTextureSlot("Diffuse Map", material.diffuseMap, changed);
    if (changed)
        MarkWaterMaterialChanged("texture_slot");

    if (ImGui::SliderFloat2("Scroll Speed A", material.scrollSpeedA, -1.0f, 1.0f, "%.3f")) MarkWaterMaterialChanged("scroll_speed_a");
    if (ImGui::SliderFloat2("Scroll Speed B", material.scrollSpeedB, -1.0f, 1.0f, "%.3f")) MarkWaterMaterialChanged("scroll_speed_b");
    if (ImGui::SliderFloat("Normal Tiling", &material.normalTiling, 0.1f, 20.0f, "%.2f")) MarkWaterMaterialChanged("normal_tiling");
}

void EditorImGui::RenderWaterMaterialEditor()
{
    if (!m_editorModeActive || !m_waterMaterialEditor.windowOpen)
        return;

    if (ImGui::Begin("Water Material Editor", &m_waterMaterialEditor.windowOpen))
    {
        const bool toolsEnabled = CanUseEditorTools();
        if (!toolsEnabled)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.30f, 1.0f), "Read-only during Play Mode");
            ImGui::BeginDisabled();
        }
        if (m_waterMaterialEditor.materialId.empty())
        {
            ImGui::TextDisabled("No water material loaded.");
            if (!toolsEnabled)
                ImGui::EndDisabled();
            ImGui::End();
            return;
        }

        RenderWaterMaterialHeader();
        ImGui::Separator();
        WaterMaterialData& material = m_waterMaterialEditor.draft;
        RenderWaterMaterialColorsSection(material);
        RenderWaterMaterialWaveSection(material);
        RenderWaterMaterialFoamSection(material);
        RenderWaterMaterialCausticSection(material);
        RenderWaterMaterialReflectionSection(material);
        RenderWaterMaterialRefractionSection(material);
        RenderWaterMaterialEdgeFadeSection(material);
        RenderWaterMaterialTexturesSection(material);
        if (!toolsEnabled)
            ImGui::EndDisabled();
    }
    ImGui::End();
}

void EditorImGui::RenderPbrMaterialHeader()
{
    UI::SectionHeader(ICON_FA_PALETTE " PBR Material");
    ImGui::Text("Editing: %s%s", m_pbrMaterialEditor.name, m_pbrMaterialEditor.dirty ? " *" : "");
    ImGui::InputText("Name", m_pbrMaterialEditor.name, sizeof(m_pbrMaterialEditor.name));
    if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Save"))
        SavePbrMaterialEditor();
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_PLUS, "New Material"))
        CreatePbrMaterialAsset();
}

void EditorImGui::RenderPbrTextureSlot(const char* label, std::string& textureId, bool& changed)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    std::string display = "(empty)";
    if (m_assetLibrary && !textureId.empty())
    {
        auto entry = m_assetLibrary->FindById(textureId);
        if (entry)
            display = entry->displayName;
    }

    ImGui::Button("##pbr_texture_slot", ImVec2(70.0f, 70.0f));
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
            if (entry && entry->category == AssetLibrary::Category::Texture)
            {
                textureId = entry->id;
                changed = true;
                Tracenf("[EDITOR-IMGUI-4] Texture slot assigned: material_id=%s slot=%s asset_id=%s",
                    m_pbrMaterialEditor.materialId.c_str(),
                    label,
                    entry->id.c_str());
            }
            else
            {
                m_assetStatus = "PBR texture slot accepts texture assets only";
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", display.c_str());
    if (!textureId.empty() && ImGui::SmallButton("Clear"))
    {
        textureId.clear();
        changed = true;
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

void EditorImGui::RenderPbrMaterialEditor()
{
    if (!m_editorModeActive || !m_pbrMaterialEditor.windowOpen)
        return;

    if (ImGui::Begin("PBR Material Editor", &m_pbrMaterialEditor.windowOpen))
    {
        const bool toolsEnabled = CanUseEditorTools();
        if (!toolsEnabled)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.30f, 1.0f), "Read-only during Play Mode");
            ImGui::BeginDisabled();
        }
        RenderPbrMaterialHeader();
        ImGui::Separator();
        AssetLibrary::MaterialData& material = m_pbrMaterialEditor.draft;
        if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool changed = false;
            RenderPbrTextureSlot("Diffuse / Albedo", material.diffuseTextureId, changed);
            RenderPbrTextureSlot("Normal Map", material.normalTextureId, changed);
            RenderPbrTextureSlot("AO", material.aoTextureId, changed);
            RenderPbrTextureSlot("Roughness", material.roughnessTextureId, changed);
            RenderPbrTextureSlot("Metallic", material.metallicTextureId, changed);
            RenderPbrTextureSlot("Height", material.heightTextureId, changed);
            if (changed)
                MarkPbrMaterialChanged("texture_slot");
        }
        if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::SliderFloat("Tiling X", &material.tilingScaleX, 0.01f, 32.0f, "%.2f")) MarkPbrMaterialChanged("tiling_x");
            if (ImGui::SliderFloat("Tiling Y", &material.tilingScaleY, 0.01f, 32.0f, "%.2f")) MarkPbrMaterialChanged("tiling_y");
            if (ImGui::ColorEdit3("Tint", material.colorTint)) MarkPbrMaterialChanged("color_tint");
            if (ImGui::SliderFloat("Normal Strength", &material.normalStrength, 0.0f, 2.0f, "%.2f")) MarkPbrMaterialChanged("normal_strength");
            if (ImGui::SliderFloat("AO Strength", &material.aoStrength, 0.0f, 2.0f, "%.2f")) MarkPbrMaterialChanged("ao_strength");
            if (ImGui::SliderFloat("Roughness Strength", &material.roughnessStrength, 0.0f, 2.0f, "%.2f")) MarkPbrMaterialChanged("roughness_strength");
            if (ImGui::SliderFloat("Metallic Strength", &material.metallicStrength, 0.0f, 2.0f, "%.2f")) MarkPbrMaterialChanged("metallic_strength");
        }
        if (!toolsEnabled)
            ImGui::EndDisabled();
    }
    ImGui::End();
}

void EditorImGui::RenderSelectedTerrainInspector()
{
    if (!m_terrainState.selected)
        return;

    UI::SectionHeader(ICON_FA_MOUNTAIN " Terrain");
    ImGui::TextDisabled("flecs=%llu  object=1",
        static_cast<unsigned long long>(m_selectedHierarchyEntity));
    ImGui::Text("Name: %s", m_terrainState.name.c_str());
    ImGui::Separator();
    ImGui::Text("Size: %.2f m x %.2f m",
        m_terrainState.widthMeters,
        m_terrainState.depthMeters);
    ImGui::Text("Cell size: %.2f m/cell", m_terrainState.cellSizeMeters);
    ImGui::Text("Cells: %u x %u", m_terrainState.cellsX, m_terrainState.cellsZ);
    const std::uint64_t verts =
        static_cast<std::uint64_t>(m_terrainState.cellsX + 1u) *
        static_cast<std::uint64_t>(m_terrainState.cellsZ + 1u);
    ImGui::Text("Vertices: %llu", static_cast<unsigned long long>(verts));
}

void EditorImGui::RenderInspector()
{
    if (ImGui::Begin("Inspector"))
    {
        if (m_terrainState.selected)
            RenderSelectedTerrainInspector();
        else if (m_waterBodyState.selected)
            RenderSelectedWaterBodyInspector();
        else if (m_dynamicLightState.type != DynamicLightType::None)
            RenderSelectedLightInspector();
        else if (m_meshRendererState.selected)
            RenderSelectedMeshRendererInspector();
        else
        {
            ImGui::TextUnformatted("Nothing selected");
            ImGui::TextWrapped("Select an entity in the Hierarchy or 3D viewport to edit its components.");
        }
    }
    ImGui::End();

    if (!m_logInspectorRendered)
    {
        m_logInspectorRendered = true;
        Tracenf("[EDITOR-IMGUI-2] Inspector active: selected_water_body=%u selected_light=%u",
            m_waterBodyState.selected ? m_waterBodyState.id : 0u,
            m_dynamicLightState.id);
    }
}

void EditorImGui::RenderEditorPanels()
{
    if (!m_editorModeActive)
        return;

    RenderDockSpace();
    RenderMenuBar();
    RenderProjectModal();
    HandleEditorHotkeys();
    RenderEditorToolbar();
    RenderHierarchyPanel();
    RenderSceneSettingsPanel();
    RenderWorldPanel();
    RenderToolsPanel();
    RenderAssetBrowser();
    RenderInspector();
    RenderSceneViewDropTarget();
    RenderWaterSculptToolPanel();
    RenderHeightmapToolPanel();
    RenderSplatPaintToolPanel();
    RenderWaterMaterialEditor();
    RenderPbrMaterialEditor();
}

void EditorImGui::Render(VulkanDevice& device)
{
    if (!m_initialized || !m_vulkanBackendReady || !m_frameActive)
    {
        static uint32_t renderSkippedLogs = 0;
        if (renderSkippedLogs < 3)
        {
            ++renderSkippedLogs;
            Tracenf("[FRAME] imgui_render called = no, initialized=%d backend_ready=%d frame_active=%d editor_mode=%d",
                m_initialized ? 1 : 0,
                m_vulkanBackendReady ? 1 : 0,
                m_frameActive ? 1 : 0,
                m_editorModeActive ? 1 : 0);
        }
        return;
    }

    RenderEditorPanels();
    RenderDemoPanels();
    ImGui::Render();

    ImDrawData* drawData = ImGui::GetDrawData();
    const uint64_t frameNumber = device.GetFrameNumber();
    if (frameNumber < 3 || (frameNumber % 60u) == 0u)
    {
        Tracenf("[FRAME] imgui_render called = yes, editor_mode=%d draw_lists=%d draw_cmds=%u",
            m_editorModeActive ? 1 : 0,
            drawData ? drawData->CmdListsCount : 0,
            CountDrawCommands(drawData));
    }
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

    vkDeviceWaitIdle(device.GetDevice());

    ImGui_ImplVulkan_PipelineInfo pipeline{};
    pipeline.RenderPass = device.GetRenderPass();
    pipeline.Subpass = 0;
    pipeline.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_CreateMainPipeline(&pipeline);

    Tracen("[EDITOR-IMGUI] Vulkan main pipeline recreated for resized render pass");
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

bool EditorImGui::IsSceneViewInputTarget(const InputEvent& event) const
{
    const auto& diag = m_viewportInputDiagnostics;
    switch (event.type)
    {
    case InputEvent::MouseMove:
    case InputEvent::MouseDown:
    case InputEvent::MouseUp:
    case InputEvent::MouseWheel:
    {
        if (!diag.sceneViewRectValid)
            return false;
        const float minX = diag.sceneViewMin[0];
        const float minY = diag.sceneViewMin[1];
        const float maxX = minX + diag.sceneViewSize[0];
        const float maxY = minY + diag.sceneViewSize[1];
        return static_cast<float>(event.x) >= minX &&
            static_cast<float>(event.x) < maxX &&
            static_cast<float>(event.y) >= minY &&
            static_cast<float>(event.y) < maxY;
    }
    case InputEvent::KeyDown:
    case InputEvent::KeyUp:
        return m_sceneViewKeyboardFocus || diag.sceneViewFocused || diag.sceneViewHovered;
    default:
        return false;
    }
}

InputEvent EditorImGui::MapInputToSceneView(const InputEvent& event) const
{
    InputEvent mapped = event;
    const auto& diag = m_viewportInputDiagnostics;
    if (!diag.sceneViewRectValid ||
        diag.sceneViewSize[0] <= 1.0f ||
        diag.sceneViewSize[1] <= 1.0f ||
        diag.sceneViewExtent[0] == 0u ||
        diag.sceneViewExtent[1] == 0u)
    {
        return mapped;
    }

    switch (event.type)
    {
    case InputEvent::MouseMove:
    case InputEvent::MouseDown:
    case InputEvent::MouseUp:
    case InputEvent::MouseWheel:
    {
        const float u = std::clamp((static_cast<float>(event.x) - diag.sceneViewMin[0]) / diag.sceneViewSize[0], 0.0f, 1.0f);
        const float v = std::clamp((static_cast<float>(event.y) - diag.sceneViewMin[1]) / diag.sceneViewSize[1], 0.0f, 1.0f);
        mapped.x = static_cast<int>(std::round(u * static_cast<float>(diag.sceneViewExtent[0] - 1u)));
        mapped.y = static_cast<int>(std::round(v * static_cast<float>(diag.sceneViewExtent[1] - 1u)));
        break;
    }
    default:
        break;
    }
    return mapped;
}

void EditorImGui::SetSceneViewKeyboardFocus(bool focused)
{
    if (m_sceneViewKeyboardFocus == focused)
        return;
    m_sceneViewKeyboardFocus = focused;
    Tracenf("[EDITOR-SCENE-VIEW] keyboard focus=%d", focused ? 1 : 0);
}

void EditorImGui::Destroy()
{
    DestroyAssetPreviewTextures();
    ReleaseSceneViewTextureDescriptor();

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
    m_physicalDevice = VK_NULL_HANDLE;
    m_graphicsQueue = VK_NULL_HANDLE;
    m_graphicsQueueFamily = UINT32_MAX;
    m_frameActive = false;
}
#else
EditorImGui::~EditorImGui() = default;

#if defined(_WIN32)
bool EditorImGui::Create(VulkanDevice&, HWND)
{
    return true;
}

bool EditorImGui::HandleWin32Message(HWND, UINT, WPARAM, LPARAM, LRESULT&)
{
    return false;
}
#else
bool EditorImGui::Create(VulkanDevice&, void*)
{
    return true;
}
#endif

void EditorImGui::BeginFrame(bool)
{
}

void EditorImGui::Render(VulkanDevice&)
{
}

void EditorImGui::OnRenderPassChanged(VulkanDevice&)
{
}

void EditorImGui::SetSceneViewTexture(VkSampler, VkImageView, VkImageLayout, VkExtent2D)
{
}

bool EditorImGui::WantsInputCapture(const InputEvent&) const
{
    return false;
}

bool EditorImGui::IsSceneViewInputTarget(const InputEvent&) const
{
    return false;
}

InputEvent EditorImGui::MapInputToSceneView(const InputEvent& event) const
{
    return event;
}

void EditorImGui::SetSceneViewKeyboardFocus(bool)
{
}

void EditorImGui::SetMapEditorSettings(const MapEditorSettings&)
{
}

void EditorImGui::SetLightingState(const LightingState&)
{
}

void EditorImGui::SetDynamicLightEditorState(const DynamicLightEditorState&)
{
}

void EditorImGui::SetWaterBodyEditorState(const WaterBodyEditorState&)
{
}

void EditorImGui::SetMeshRendererEditorState(const MeshRendererEditorState&)
{
}

void EditorImGui::SetTerrainEditorState(const TerrainEditorState&)
{
}

void EditorImGui::SetEngineStats(const EngineStats&)
{
}

void EditorImGui::SetHierarchySceneState(std::uint64_t, std::string, std::vector<HierarchySceneEntity>)
{
}

void EditorImGui::SetWaterMaterials(std::vector<std::pair<std::string, WaterMaterialData>>)
{
}

void EditorImGui::SetPaletteSlots(const std::array<MapEditorPaletteSlot, 8>&)
{
}

void EditorImGui::SetWaterMaterialUsageCounts(std::vector<std::pair<std::string, std::uint32_t>>)
{
}

std::vector<std::pair<std::string, WaterMaterialData>> EditorImGui::GetWaterMaterialsSnapshot() const
{
    return {};
}

void EditorImGui::InitializeAssetLibrary(const std::filesystem::path&)
{
}

void EditorImGui::RefreshAssetLibrary()
{
}

bool EditorImGui::OpenWaterMaterialEditor(const std::string&)
{
    return false;
}

bool EditorImGui::OpenPbrMaterialEditor(const std::string&)
{
    return false;
}

MapEditorCommands EditorImGui::ConsumeCommands()
{
    return {};
}

void EditorImGui::Destroy()
{
}
#endif
