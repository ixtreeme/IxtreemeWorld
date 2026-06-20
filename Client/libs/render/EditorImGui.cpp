#include "EditorImGui.h"

#include "AssetDatabase.h"
#include "AssimpExporter.h"
#include "AssetWatcher.h"
#include "Common.h"
#include "Debug.h"
#include "MaterialAssetManager.h"
#include "ProjectManager.h"
#include "SceneManager.h"
#include "VulkanDevice.h"
#include "math/IXMath.h"
#include "platform/trash.h"
#include "tools/tree/TreeTexturePalette.h"

#if defined(IXTREEME_WITH_EDITOR) && defined(_WIN32)
#include "IconsFontAwesome6.h"
#include "UIHelpers.h"
#define VK_USE_PLATFORM_WIN32_KHR
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
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
#include <limits>
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
constexpr const char* kAssetFolderPayloadType = "ASSET_FOLDER_PATH";
constexpr const char* kEditorNoteComponentId = "editor.note";
constexpr const char* kLodComponentId = "rendering.lod";
constexpr double kProjectAutoSaveIntervalSeconds = 5.0 * 60.0;
constexpr float kPi = ixtreeme::math::Pi;
constexpr float kGizmoPlaneScaleUnitsPerPixel = 0.01f;

float Degrees(float radians)
{
    return ixtreeme::math::RadiansToDegrees(radians);
}

float Radians(float degrees)
{
    return ixtreeme::math::DegreesToRadians(degrees);
}

float UnwrapDegreesNear(float value, float reference)
{
    while (value - reference > 180.0f)
        value -= 360.0f;
    while (value - reference < -180.0f)
        value += 360.0f;
    return value;
}

ImGuizmo::OPERATION ToImGuizmoOperation(MapEditorGizmoOperation operation)
{
    switch (operation)
    {
    case MapEditorGizmoOperation::Rotate:
        return ImGuizmo::ROTATE;
    case MapEditorGizmoOperation::Scale:
        return ImGuizmo::SCALE;
    case MapEditorGizmoOperation::Translate:
    default:
        return ImGuizmo::TRANSLATE;
    }
}

ImGuizmo::MODE ToImGuizmoMode(MapEditorGizmoOperation operation)
{
    return operation == MapEditorGizmoOperation::Rotate ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
}

WorldMat4 ImGuizmoPerspective(float fovYRadians, float aspect, float zNear, float zFar)
{
    WorldMat4 projection = WorldPerspective(fovYRadians, aspect, zNear, zFar);
    projection.m[5] = ixtreeme::math::Abs(projection.m[5]);
    return projection;
}

std::filesystem::path InternalAssetRootFor(const std::filesystem::path& engineRoot)
{
    if (engineRoot.empty())
        return std::filesystem::path{};
    std::error_code ec;
    const std::filesystem::path direct = engineRoot / "internal";
    if (std::filesystem::exists(direct, ec))
        return direct;
    const std::filesystem::path underAssets = engineRoot / "assets" / "internal";
    if (std::filesystem::exists(underAssets, ec))
        return underAssets;
    return direct;
}

struct InspectorComponentDefinition
{
    const char* id;
    const char* displayName;
    const char* category;
    EditorComponentType legacyType;
    bool addableToMesh;
};

const std::array<InspectorComponentDefinition, 7>& InspectorComponentRegistry()
{
    static const std::array<InspectorComponentDefinition, 7> registry{{
        {"builtin.transform", "Transform", "Core", EditorComponentType::None, false},
        {"builtin.mesh_renderer", "MeshRenderer", "Rendering", EditorComponentType::MeshRenderer, false},
        {kLodComponentId, "LOD Group", "Rendering", EditorComponentType::None, true},
        {"builtin.water_body", "Water Body", "Rendering", EditorComponentType::WaterBody, false},
        {"builtin.point_light", "Point Light", "Lighting", EditorComponentType::PointLight, false},
        {"builtin.spot_light", "Spot Light", "Lighting", EditorComponentType::SpotLight, false},
        {kEditorNoteComponentId, "Note", "Editor", EditorComponentType::None, true},
    }};
    return registry;
}

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
    return ixtreeme::common::ToLowerAscii(std::move(value));
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

std::filesystem::path MetaSidecarPath(const std::filesystem::path& path)
{
    return std::filesystem::path(path.string() + ".meta");
}

bool IsMetaFile(const std::filesystem::path& path)
{
    return path.extension() == ".meta";
}

std::string UniqueFolderName(const std::filesystem::path& parent)
{
    std::error_code ec;
    const std::string base = "New Folder";
    if (!std::filesystem::exists(parent / base, ec))
        return base;
    for (int i = 2; i < 1000; ++i)
    {
        const std::string candidate = base + " " + std::to_string(i);
        ec.clear();
        if (!std::filesystem::exists(parent / candidate, ec))
            return candidate;
    }
    return base + " 1000";
}

bool IsSubpathOrSelf(const std::string& maybeChild, const std::string& maybeParent)
{
    const std::string child = AssetLibrary::NormalizeSubpath(maybeChild);
    const std::string parent = AssetLibrary::NormalizeSubpath(maybeParent);
    if (parent.empty())
        return true;
    return child == parent || child.rfind(parent + "/", 0) == 0;
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

const char* ImportDetectedTypeName(const std::filesystem::path& path)
{
    const std::string ext = ToLowerAscii(path.extension().string());
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" ||
        ext == ".bmp" || ext == ".dds" || ext == ".ktx" || ext == ".ktx2")
        return "Texture";
    if (ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj")
        return "Model";
    if (ext == ".material")
        return "Material";
    if (ext == ".anim" || ext == ".ozz")
        return "Anim";
    if (ext == ".scene")
        return "Scene";
    return "Unknown";
}

std::filesystem::path DefaultExportDirectory()
{
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

std::filesystem::path DefaultFbxExportPath(const std::string& stem)
{
    std::string safeStem = stem.empty() ? "export" : stem;
    for (char& ch : safeStem)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isalnum(uch) && ch != '_' && ch != '-')
            ch = '_';
    }
    return DefaultExportDirectory() / (safeStem + ".fbx");
}

bool IsSupportedImportFile(const std::filesystem::path& path)
{
    return std::strcmp(ImportDetectedTypeName(path), "Unknown") != 0;
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
        return srgb <= 0.04045f ? srgb / 12.92f : ixtreeme::math::Pow((srgb + 0.055f) / 1.055f, 2.4f);
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
    const std::uint32_t previousSlot =
        (m_meshRendererState.selected && state.selected && m_meshRendererState.id == state.id)
            ? m_meshRendererState.selectedMaterialSlot
            : 0u;
    m_meshRendererState = state;
    m_meshRendererState.selectedMaterialSlot = std::min(previousSlot,
        m_meshRendererState.materialSlotCount > 0 ? m_meshRendererState.materialSlotCount - 1u : 0u);
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
    tree_tool::TreeTexturePalette::Instance().EnsureLoaded(InternalAssetRootFor(m_engineRoot));
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
    tree_tool::TreeTexturePalette::Instance().EnsureLoaded(InternalAssetRootFor(m_engineRoot));
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

void EditorImGui::SetSceneViewSelectionOutline(std::vector<std::array<float, 4>> segments)
{
    m_sceneViewSelectionOutline = std::move(segments);
}

void EditorImGui::SetSceneViewGizmo(HierarchyEntityType type,
                                    std::uint32_t id,
                                    const WorldCamera& camera,
                                    const float* position,
                                    const float* rotation,
                                    const float* scale,
                                    MapEditorGizmoOperation operation,
                                    bool snapEnabled,
                                    float snapValue)
{
    m_sceneGizmoVisible = type != HierarchyEntityType::None && id != 0 && position && rotation && scale;
    m_sceneGizmoEntityType = type;
    m_sceneGizmoEntityId = id;
    m_sceneGizmoCamera = camera;
    if (position)
        std::copy(position, position + 3, m_sceneGizmoPosition);
    if (rotation)
        std::copy(rotation, rotation + 3, m_sceneGizmoRotation);
    if (scale)
        std::copy(scale, scale + 3, m_sceneGizmoScale);
    m_sceneGizmoOperation = operation;
    m_gizmoOperation = operation;
    m_sceneGizmoSnapEnabled = snapEnabled;
    m_sceneGizmoSnapValue = std::max(0.001f, snapValue);
}

void EditorImGui::ClearSceneViewGizmo()
{
    m_sceneGizmoVisible = false;
    m_sceneGizmoInputActive = false;
    m_sceneGizmoEntityType = HierarchyEntityType::None;
    m_sceneGizmoEntityId = 0;
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
        const bool captureGpuFrame = commands.captureGpuFrame;
        const bool dumpFrameProfile = commands.dumpFrameProfile;
        const bool debugPerfTogglesChanged = commands.debugPerfTogglesChanged;
        const bool disableShadowPass = commands.disableShadowPass;
        const bool disableWaterReflectionPass = commands.disableWaterReflectionPass;
        const bool disableAssetLibraryDiscovery = commands.disableAssetLibraryDiscovery;
        const bool disableAssetWatcherPoll = commands.disableAssetWatcherPoll;
        const bool disableHierarchyIteration = commands.disableHierarchyIteration;
        const bool renderResolutionChanged = commands.renderResolutionChanged;
        const bool renderResolutionUseNative = commands.renderResolutionUseNative;
        const std::uint32_t renderResolutionWidth = commands.renderResolutionWidth;
        const std::uint32_t renderResolutionHeight = commands.renderResolutionHeight;
        commands = {};
        commands.enterPlayMode = enterPlayMode;
        commands.exitPlayMode = exitPlayMode;
        commands.pausePlayMode = pausePlayMode;
        commands.resumePlayMode = resumePlayMode;
        commands.captureGpuFrame = captureGpuFrame;
        commands.dumpFrameProfile = dumpFrameProfile;
        commands.debugPerfTogglesChanged = debugPerfTogglesChanged;
        commands.disableShadowPass = disableShadowPass;
        commands.disableWaterReflectionPass = disableWaterReflectionPass;
        commands.disableAssetLibraryDiscovery = disableAssetLibraryDiscovery;
        commands.disableAssetWatcherPoll = disableAssetWatcherPoll;
        commands.disableHierarchyIteration = disableHierarchyIteration;
        commands.renderResolutionChanged = renderResolutionChanged;
        commands.renderResolutionUseNative = renderResolutionUseNative;
        commands.renderResolutionWidth = renderResolutionWidth;
        commands.renderResolutionHeight = renderResolutionHeight;
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

std::optional<LodConfig> EditorImGui::FindModelLodDefault(const std::string& assetId) const
{
    if (!m_assetLibrary || assetId.empty())
        return std::nullopt;
    const auto entry = m_assetLibrary->FindById(assetId);
    if (!entry || entry->category != AssetLibrary::Category::Model || !entry->hasLodDefault)
        return std::nullopt;
    return entry->lodDefault;
}

bool EditorImGui::SaveModelLodDefault(const std::string& assetId, const LodConfig& config)
{
    if (!m_assetLibrary || assetId.empty())
        return false;
    AssetLibrary::Entry updated;
    std::string error;
    if (!m_assetLibrary->UpdateModelLodDefault(assetId, config, updated, error))
    {
        m_assetStatus = "LOD asset default failed: " + error;
        return false;
    }
    m_assetStatus = "LOD asset default saved: " + updated.displayName;
    RefreshAssetLibrary();
    return true;
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

std::filesystem::path EditorImGui::AssetBrowserRoot() const
{
    return m_assetLibrary ? m_assetLibrary->LibraryRoot() : std::filesystem::path{};
}

std::filesystem::path EditorImGui::AssetBrowserPath(const std::string& subpath) const
{
    return AssetBrowserRoot() / AssetLibrary::NormalizeSubpath(subpath);
}

std::string EditorImGui::AssetBrowserSubpath(const std::filesystem::path& path) const
{
    const std::filesystem::path root = AssetBrowserRoot();
    if (root.empty())
        return {};
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    if (ec)
        return {};
    return AssetLibrary::NormalizeSubpath(relative.generic_string());
}

void EditorImGui::SelectAssetBrowserFolder(const std::string& subpath)
{
    m_assetSubpath = AssetLibrary::NormalizeSubpath(subpath);
    m_selectedAssetId.clear();
    m_activeAssetTags.clear();
}

std::vector<std::string> EditorImGui::QueryFilesystemChildFolders(const std::string& subpath) const
{
    std::vector<std::string> folders;
    const std::filesystem::path directory = AssetBrowserPath(subpath);
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec))
        return folders;

    for (const auto& entry : std::filesystem::directory_iterator(directory, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec)
            break;
        std::error_code itemEc;
        if (!entry.is_directory(itemEc))
            continue;
        folders.push_back(AssetBrowserSubpath(entry.path()));
    }
    std::sort(folders.begin(), folders.end(), [](const std::string& a, const std::string& b) {
        return ToLowerAscii(FolderDisplayName(a)) < ToLowerAscii(FolderDisplayName(b));
    });
    return folders;
}

std::vector<AssetLibrary::Entry> EditorImGui::QueryFilesystemAssetsInFolder(const std::string& subpath) const
{
    std::vector<AssetLibrary::Entry> result;
    if (!m_assetLibrary)
        return result;

    const std::string target = ComparablePath(AssetBrowserPath(subpath));
    auto addIfInFolder = [&](const AssetLibrary::Entry& entry, const std::filesystem::path& absolutePath) {
        if (absolutePath.empty() || IsMetaFile(absolutePath))
            return;
        if (ComparablePath(absolutePath.parent_path()) == target)
            result.push_back(entry);
    };

    for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
        addIfInFolder(entry, m_assetLibrary->AbsolutePath(entry));

    for (const AssetLibrary::Entry& entry : QuerySceneAssets())
    {
        if (entry.originalPath.empty())
            continue;
        const std::filesystem::path scenePath(entry.originalPath);
        const std::string sceneComparable = ComparablePath(scenePath);
        const std::string rootComparable = ComparablePath(AssetBrowserRoot());
        if (sceneComparable.rfind(rootComparable + "/", 0) == 0)
            addIfInFolder(entry, scenePath);
    }

    std::sort(result.begin(), result.end(), [](const AssetLibrary::Entry& a, const AssetLibrary::Entry& b) {
        return ToLowerAscii(a.filename.empty() ? a.displayName : a.filename) <
            ToLowerAscii(b.filename.empty() ? b.displayName : b.filename);
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

void EditorImGui::CreateFilesystemFolder(const std::string& parentSubpath, const std::string& requestedName)
{
    const std::filesystem::path parent = AssetBrowserPath(parentSubpath);
    const std::string name = requestedName.empty() ? UniqueFolderName(parent) : requestedName;
    if (!AssetLibrary::IsValidRenameName(name))
    {
        m_assetStatus = "Folder create failed: invalid name";
        return;
    }

    std::error_code ec;
    const std::filesystem::path created = parent / name;
    std::filesystem::create_directories(created, ec);
    if (ec)
    {
        m_assetStatus = "Folder create failed: " + ec.message();
        return;
    }
    RefreshAssetLibrary();
    SelectAssetBrowserFolder(AssetBrowserSubpath(created));
    m_assetStatus = "Folder created: " + name;
    Tracenf("[EDITOR-ASSET-BROWSER] folder_created path=Assets/%s",
        AssetBrowserSubpath(created).c_str());
}

void EditorImGui::BeginAssetRename(const AssetLibrary::Entry& entry)
{
    if (!m_assetLibrary)
        return;
    const std::filesystem::path path = entry.originalPath.empty() ? m_assetLibrary->AbsolutePath(entry) : std::filesystem::path(entry.originalPath);
    m_assetRenamePath = path.generic_string();
    m_assetRenameIsFolder = false;
    CopyToBuffer(m_assetRenameBuffer, sizeof(m_assetRenameBuffer), path.filename().string());
    m_assetOpenRenamePopup = true;
}

void EditorImGui::BeginFolderRename(const std::string& subpath)
{
    if (subpath.empty())
        return;
    const std::filesystem::path path = AssetBrowserPath(subpath);
    m_assetRenamePath = path.generic_string();
    m_assetRenameIsFolder = true;
    CopyToBuffer(m_assetRenameBuffer, sizeof(m_assetRenameBuffer), path.filename().string());
    m_assetOpenRenamePopup = true;
}

void EditorImGui::RenameFilesystemSelection()
{
    if (m_assetRenamePath.empty())
        return;
    const std::filesystem::path source(m_assetRenamePath);
    const std::string requested = m_assetRenameBuffer;
    if (!AssetLibrary::IsValidRenameName(std::filesystem::path(requested).stem().string()))
    {
        m_assetStatus = "Rename failed: invalid name";
        return;
    }

    std::filesystem::path targetName(requested);
    if (!m_assetRenameIsFolder && targetName.extension().empty())
        targetName += source.extension();
    const std::filesystem::path destination = source.parent_path() / targetName.filename();
    std::error_code ec;
    if (std::filesystem::exists(destination, ec))
    {
        m_assetStatus = "Rename failed: target already exists";
        return;
    }

    bool metaMoved = false;
    const std::filesystem::path sourceMeta = MetaSidecarPath(source);
    const std::filesystem::path destinationMeta = MetaSidecarPath(destination);
    if (!m_assetRenameIsFolder && std::filesystem::exists(sourceMeta, ec))
    {
        std::filesystem::rename(sourceMeta, destinationMeta, ec);
        if (ec)
        {
            m_assetStatus = "Rename failed moving .meta: " + ec.message();
            return;
        }
        metaMoved = true;
    }

    std::filesystem::rename(source, destination, ec);
    if (ec)
    {
        if (metaMoved)
        {
            std::error_code rollbackEc;
            std::filesystem::rename(destinationMeta, sourceMeta, rollbackEc);
        }
        m_assetStatus = "Rename failed: " + ec.message();
        return;
    }

    RefreshAssetLibrary();
    if (m_assetRenameIsFolder && AssetLibrary::NormalizeSubpath(m_assetSubpath).rfind(AssetBrowserSubpath(source), 0) == 0)
        SelectAssetBrowserFolder(AssetBrowserSubpath(destination));
    m_assetStatus = "Renamed: " + destination.filename().string();
    Tracenf("[EDITOR-ASSET-BROWSER] renamed src=%s dst=%s metaMoved=%s",
        source.generic_string().c_str(),
        destination.generic_string().c_str(),
        metaMoved ? "yes" : "no");
}

void EditorImGui::DeleteFilesystemSelection()
{
    if (m_assetDeletePath.empty())
        return;
    const std::filesystem::path path(m_assetDeletePath);
    std::string error;
    bool metaDeleted = false;
    if (!m_assetDeleteIsFolder)
    {
        const std::filesystem::path meta = MetaSidecarPath(path);
        std::error_code ec;
        if (std::filesystem::exists(meta, ec))
        {
            if (!platform::move_to_trash(meta, &error))
            {
                m_assetStatus = "Delete .meta failed: " + error;
                return;
            }
            metaDeleted = true;
        }
    }

    if (!platform::move_to_trash(path, &error))
    {
        m_assetStatus = "Delete failed: " + error;
        return;
    }

    if (m_assetDeleteIsFolder && IsSubpathOrSelf(m_assetSubpath, AssetBrowserSubpath(path)))
        SelectAssetBrowserFolder(ParentSubpath(AssetBrowserSubpath(path)));
    if (!m_assetDeleteIsFolder)
        m_selectedAssetId.clear();
    RefreshAssetLibrary();
    m_assetStatus = "Deleted: " + path.filename().string();
    Tracenf("[EDITOR-ASSET-BROWSER] deleted path=Assets/%s metaDeleted=%s targetTrash=ok",
        AssetBrowserSubpath(path).c_str(),
        metaDeleted ? "yes" : "no");
}

bool EditorImGui::MoveAssetEntryToFolder(const std::string& assetId, const std::string& targetFolderSubpath)
{
    if (!m_assetLibrary)
        return false;
    auto entry = m_assetLibrary->FindById(assetId);
    if (!entry)
        return false;
    const std::filesystem::path source = entry->originalPath.empty() ? m_assetLibrary->AbsolutePath(*entry) : std::filesystem::path(entry->originalPath);
    const std::filesystem::path targetFolder = AssetBrowserPath(targetFolderSubpath);
    const std::filesystem::path destination = targetFolder / source.filename();
    std::error_code ec;
    if (ComparablePath(source.parent_path()) == ComparablePath(targetFolder))
        return true;
    if (std::filesystem::exists(destination, ec))
    {
        m_assetStatus = "Move failed: target already exists";
        Tracenf("[EDITOR-ASSET-BROWSER] move_failed reason=target_exists src=%s dst=%s",
            source.generic_string().c_str(),
            destination.generic_string().c_str());
        return false;
    }
    std::filesystem::create_directories(targetFolder, ec);
    if (ec)
    {
        m_assetStatus = "Move failed: " + ec.message();
        return false;
    }

    bool metaMoved = false;
    const std::filesystem::path sourceMeta = MetaSidecarPath(source);
    const std::filesystem::path destinationMeta = MetaSidecarPath(destination);
    if (std::filesystem::exists(sourceMeta, ec))
    {
        std::filesystem::rename(sourceMeta, destinationMeta, ec);
        if (ec)
        {
            m_assetStatus = "Move failed moving .meta: " + ec.message();
            return false;
        }
        metaMoved = true;
    }

    std::filesystem::rename(source, destination, ec);
    if (ec)
    {
        if (metaMoved)
        {
            std::error_code rollbackEc;
            std::filesystem::rename(destinationMeta, sourceMeta, rollbackEc);
        }
        m_assetStatus = "Move failed: " + ec.message();
        return false;
    }
    RefreshAssetLibrary();
    m_assetStatus = "Moved: " + source.filename().string();
    Tracenf("[EDITOR-ASSET-BROWSER] moved src=%s dst=%s metaMoved=%s",
        source.generic_string().c_str(),
        destination.generic_string().c_str(),
        metaMoved ? "yes" : "no");
    return true;
}

bool EditorImGui::MoveFolderToFolder(const std::string& sourceSubpath, const std::string& targetFolderSubpath)
{
    const std::string sourceNorm = AssetLibrary::NormalizeSubpath(sourceSubpath);
    const std::string targetNorm = AssetLibrary::NormalizeSubpath(targetFolderSubpath);
    if (sourceNorm.empty() || IsSubpathOrSelf(targetNorm, sourceNorm))
        return false;

    const std::filesystem::path source = AssetBrowserPath(sourceNorm);
    if (ComparablePath(source.parent_path()) == ComparablePath(AssetBrowserPath(targetNorm)))
        return true;
    const std::filesystem::path destination = AssetBrowserPath(targetNorm) / source.filename();
    std::error_code ec;
    if (std::filesystem::exists(destination, ec))
    {
        m_assetStatus = "Move failed: target already exists";
        Tracenf("[EDITOR-ASSET-BROWSER] move_failed reason=target_exists src=%s dst=%s",
            source.generic_string().c_str(),
            destination.generic_string().c_str());
        return false;
    }
    std::filesystem::rename(source, destination, ec);
    if (ec)
    {
        m_assetStatus = "Move failed: " + ec.message();
        return false;
    }
    RefreshAssetLibrary();
    if (IsSubpathOrSelf(m_assetSubpath, sourceNorm))
        SelectAssetBrowserFolder(AssetBrowserSubpath(destination));
    Tracenf("[EDITOR-ASSET-BROWSER] moved src=%s dst=%s metaMoved=folder_subtree",
        source.generic_string().c_str(),
        destination.generic_string().c_str());
    return true;
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

void EditorImGui::OpenImportAssetDialog(const std::string& targetSubpath)
{
    m_assetImportTargetSubpath = AssetLibrary::NormalizeSubpath(targetSubpath);
    m_assetImportPathBuffer[0] = '\0';
    std::error_code ec;
    if (m_assetImportBrowserPath.empty() || !std::filesystem::exists(m_assetImportBrowserPath, ec))
        m_assetImportBrowserPath = std::filesystem::current_path(ec);
    CopyToBuffer(m_assetImportBrowserPathBuffer, sizeof(m_assetImportBrowserPathBuffer), m_assetImportBrowserPath.string());
    m_assetImportFilterBuffer[0] = '\0';
    m_assetOpenImportPopup = true;
}

void EditorImGui::ImportAssetFromPath(const std::filesystem::path& sourcePath,
                                      const std::string& targetSubpath,
                                      const char* trigger)
{
    if (!m_assetLibrary)
        return;

    if (sourcePath.empty())
    {
        m_assetStatus = "Import skipped: no source file selected";
        return;
    }

    const std::string normalizedTarget = AssetLibrary::NormalizeSubpath(targetSubpath);
    const std::filesystem::path targetFolder = AssetBrowserPath(normalizedTarget);
    const char* detectedType = ImportDetectedTypeName(sourcePath);
    const std::filesystem::path expectedFinal = targetFolder / sourcePath.filename();
    Tracenf("[IMPORT-DIAG] trigger=%s targetFolder=%s userSelected=%s finalPath=%s detectedType=%s",
        trigger ? trigger : "unknown",
        targetFolder.generic_string().c_str(),
        sourcePath.generic_string().c_str(),
        expectedFinal.generic_string().c_str(),
        detectedType);

    if (std::strcmp(detectedType, "Unknown") == 0)
    {
        m_assetStatus = "Import skipped: unsupported file type";
        Tracenf("[IMPORT-DIAG] Unsupported file type: %s", sourcePath.extension().string().c_str());
        return;
    }

    AssetLibrary::Entry entry{};
    std::filesystem::path finalPath;
    std::string error;
    if (!m_assetLibrary->ImportFileToFolder(sourcePath, targetFolder, entry, finalPath, error))
    {
        m_assetStatus = "Import failed: " + error;
        TraceError("[IMPORT-DIAG] copy failed: %s -> %s error=%s",
            sourcePath.generic_string().c_str(),
            expectedFinal.generic_string().c_str(),
            error.c_str());
        return;
    }

    RefreshAssetLibrary();
    SelectAssetBrowserFolder(normalizedTarget);
    m_selectedAssetId = entry.id;
    m_assetStatus = "Imported: " + entry.filename;
}

void EditorImGui::ImportExternalFiles(const std::vector<std::string>& paths, const char* trigger)
{
    const std::string target = AssetLibrary::NormalizeSubpath(m_assetSubpath);
    for (const std::string& path : paths)
        ImportAssetFromPath(std::filesystem::path(path), target, trigger ? trigger : "dragdrop");
}

void EditorImGui::CreatePbrMaterialAsset()
{
    if (!m_assetLibrary)
        return;
    CopyToBuffer(m_createMaterialName, sizeof(m_createMaterialName), "material");
    m_createMaterialShadingMode = -1;
    m_assetOpenCreateMaterialPopup = true;
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
    if (!entry->originalPath.empty() && ToLowerAscii(std::filesystem::path(entry->originalPath).extension().string()) == ".material")
    {
        const std::filesystem::path materialPath(entry->originalPath);
        const Guid guid = AssetDatabase::Instance().getOrCreateGuid(materialPath);
        if (MaterialAsset* material = MaterialAssetManager::Instance().getOrLoad(guid))
        {
            m_pbrMaterialEditor.draft.alphaMode =
                material->alphaMode == MaterialAsset::AlphaMode::Mask ? "mask" :
                (material->alphaMode == MaterialAsset::AlphaMode::Blend ? "blend" : "opaque");
            m_pbrMaterialEditor.draft.alphaCutoff = material->alphaCutoff;
            m_pbrMaterialEditor.draft.colorTint[0] = material->baseColor[0];
            m_pbrMaterialEditor.draft.colorTint[1] = material->baseColor[1];
            m_pbrMaterialEditor.draft.colorTint[2] = material->baseColor[2];
            m_pbrMaterialEditor.draft.normalStrength = material->normalStrength;
            m_pbrMaterialEditor.draft.aoStrength = material->aoStrength;
            m_pbrMaterialEditor.draft.roughnessStrength = material->roughness;
            m_pbrMaterialEditor.draft.metallicStrength = material->metallic;
            m_pbrMaterialEditor.draft.shadingMode =
                material->shadingMode == MaterialAsset::ShadingMode::Unlit ? "unlit" : "lit";
        }
    }
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

    if (!current.originalPath.empty() && ToLowerAscii(std::filesystem::path(current.originalPath).extension().string()) == ".material")
    {
        const std::filesystem::path materialPath(current.originalPath);
        const Guid guid = AssetDatabase::Instance().getOrCreateGuid(materialPath);
        MaterialAsset* material = MaterialAssetManager::Instance().getOrLoad(guid);
        if (!material)
        {
            m_assetStatus = "Material save failed: unable to load material asset";
            return false;
        }
        material->name = requestedName;
        material->baseColor[0] = m_pbrMaterialEditor.draft.colorTint[0];
        material->baseColor[1] = m_pbrMaterialEditor.draft.colorTint[1];
        material->baseColor[2] = m_pbrMaterialEditor.draft.colorTint[2];
        material->metallic = m_pbrMaterialEditor.draft.metallicStrength;
        material->roughness = m_pbrMaterialEditor.draft.roughnessStrength;
        material->normalStrength = m_pbrMaterialEditor.draft.normalStrength;
        material->aoStrength = m_pbrMaterialEditor.draft.aoStrength;
        const std::string alphaMode = ToLowerAscii(m_pbrMaterialEditor.draft.alphaMode);
        material->alphaMode = alphaMode == "mask" ? MaterialAsset::AlphaMode::Mask :
            (alphaMode == "blend" ? MaterialAsset::AlphaMode::Blend : MaterialAsset::AlphaMode::Opaque);
        material->alphaCutoff = std::clamp(m_pbrMaterialEditor.draft.alphaCutoff, 0.0f, 1.0f);
        if (!MaterialAssetManager::Instance().save(*material))
        {
            m_assetStatus = "Material save failed: write failed";
            return false;
        }
        m_pbrMaterialEditor.draft.alphaMode = alphaMode == "mask" || alphaMode == "blend" ? alphaMode : "opaque";
        m_pbrMaterialEditor.draft.alphaCutoff = material->alphaCutoff;
        m_pbrMaterialEditor.dirty = false;
        m_selectedAssetId = current.id;
        RefreshAssetLibrary();
        m_assetStatus = "Material saved: " + current.displayName;
        Tracenf("[EDITOR-IMGUI-4] Material saved: id=%s name=%s alphaMode=%s alphaCutoff=%.3f",
            current.id.c_str(),
            current.displayName.c_str(),
            m_pbrMaterialEditor.draft.alphaMode.c_str(),
            m_pbrMaterialEditor.draft.alphaCutoff);
        return true;
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

    if (!current.originalPath.empty() && ToLowerAscii(std::filesystem::path(current.originalPath).extension().string()) == ".material")
    {
        const std::filesystem::path materialPath(current.originalPath);
        const Guid guid = AssetDatabase::Instance().getOrCreateGuid(materialPath);
        MaterialAsset* material = MaterialAssetManager::Instance().getOrLoad(guid);
        if (!material)
        {
            m_assetStatus = "Material save failed: unable to load material asset";
            return false;
        }
        material->name = requestedName;
        material->baseColor[0] = m_pbrMaterialEditor.draft.colorTint[0];
        material->baseColor[1] = m_pbrMaterialEditor.draft.colorTint[1];
        material->baseColor[2] = m_pbrMaterialEditor.draft.colorTint[2];
        material->metallic = m_pbrMaterialEditor.draft.metallicStrength;
        material->roughness = m_pbrMaterialEditor.draft.roughnessStrength;
        material->normalStrength = m_pbrMaterialEditor.draft.normalStrength;
        material->aoStrength = m_pbrMaterialEditor.draft.aoStrength;
        material->shadingMode =
            ToLowerAscii(m_pbrMaterialEditor.draft.shadingMode) == "unlit"
                ? MaterialAsset::ShadingMode::Unlit
                : MaterialAsset::ShadingMode::Lit;
        const std::string alphaMode = ToLowerAscii(m_pbrMaterialEditor.draft.alphaMode);
        material->alphaMode = alphaMode == "mask" ? MaterialAsset::AlphaMode::Mask :
            (alphaMode == "blend" ? MaterialAsset::AlphaMode::Blend : MaterialAsset::AlphaMode::Opaque);
        material->alphaCutoff = std::clamp(m_pbrMaterialEditor.draft.alphaCutoff, 0.0f, 1.0f);
        if (!MaterialAssetManager::Instance().save(*material))
        {
            m_assetStatus = "Material save failed: write failed";
            return false;
        }

        m_pbrMaterialEditor.draft.shadingMode =
            material->shadingMode == MaterialAsset::ShadingMode::Unlit ? "unlit" : "lit";
        m_pbrMaterialEditor.draft.alphaMode = alphaMode == "mask" || alphaMode == "blend" ? alphaMode : "opaque";
        m_pbrMaterialEditor.draft.alphaCutoff = material->alphaCutoff;
        m_pbrMaterialEditor.dirty = false;
        m_selectedAssetId = current.id;
        RefreshAssetLibrary();
        m_assetStatus = "Material saved: " + current.displayName;
        Tracenf("[MATERIAL] saved path=%s shadingMode=%s alphaMode=%s",
            materialPath.generic_string().c_str(),
            m_pbrMaterialEditor.draft.shadingMode == "unlit" ? "Unlit" : "Lit",
            m_pbrMaterialEditor.draft.alphaMode.c_str());
        return true;
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
    m_meshRendererState.materialSlots.clear();
    std::filesystem::path modelPath =
        entry->originalPath.empty() ? m_assetLibrary->AbsolutePath(*entry) : std::filesystem::path(entry->originalPath);
    if (modelPath.is_relative())
        modelPath = m_assetLibrary->AbsolutePath(*entry);
    const std::vector<Guid> defaults = AssetDatabase::Instance().loadDefaultMaterials(modelPath);
    m_meshRendererState.materialSlots.reserve(defaults.size());
    for (const Guid& guid : defaults)
        m_meshRendererState.materialSlots.push_back(guid.toString());
    m_meshRendererState.materialSlotCount = std::max<std::uint32_t>(1u,
        static_cast<std::uint32_t>(m_meshRendererState.materialSlots.size()));
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
        if (!QuietLogsForLodDiag() && beginSkippedLogs < 3)
        {
            ++beginSkippedLogs;
            TraceDiagf("[FRAME] imgui_begin called = no, initialized=%d backend_ready=%d frame_active=%d editor_mode=%d",
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
    if (!QuietLogsForLodDiag() && beginLogs < 3)
    {
        ++beginLogs;
        TraceDiagf("[FRAME] imgui_begin called = yes, editor_mode=%d", editorModeActive ? 1 : 0);
    }
}

#include "editor_panels/EditorImGuiSceneViewPanels.inl"
#include "editor_panels/EditorImGuiProjectPanels.inl"
#include "editor_panels/EditorImGuiMenuToolbarPanels.inl"
#include "editor_panels/EditorImGuiHierarchyPanels.inl"
#include "editor_panels/EditorImGuiInspectorPanels.inl"
#include "editor_panels/EditorImGuiAssetBrowserPanels.inl"
#include "editor_panels/EditorImGuiMaterialPanels.inl"
#include "editor_panels/EditorImGuiPanelDispatcher.inl"
void EditorImGui::Render(VulkanDevice& device)
{
    if (!m_initialized || !m_vulkanBackendReady || !m_frameActive)
    {
        static uint32_t renderSkippedLogs = 0;
        if (!QuietLogsForLodDiag() && renderSkippedLogs < 3)
        {
            ++renderSkippedLogs;
            TraceDiagf("[FRAME] imgui_render called = no, initialized=%d backend_ready=%d frame_active=%d editor_mode=%d",
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
    if (!QuietLogsForLodDiag() && (frameNumber < 3 || (frameNumber % 60u) == 0u))
    {
        TraceDiagf("[FRAME] imgui_render called = yes, editor_mode=%d draw_lists=%d draw_cmds=%u",
            m_editorModeActive ? 1 : 0,
            drawData ? drawData->CmdListsCount : 0,
            CountDrawCommands(drawData));
    }
    ImGui_ImplVulkan_RenderDrawData(drawData, device.GetCommandBuffer());

    static uint32_t lastLoggedDrawCommands = std::numeric_limits<uint32_t>::max();
    const uint32_t drawCommands = CountDrawCommands(drawData);
    const bool drawCommandsChanged = lastLoggedDrawCommands != drawCommands;
    if (device.GetFrameNumber() != m_lastLoggedFrame &&
        device.GetFrameNumber() % 300 == 0 &&
        (!QuietLogsForLodDiag() || drawCommandsChanged))
    {
        m_lastLoggedFrame = device.GetFrameNumber();
        TraceDiagf("[EDITOR-IMGUI] Frame %llu rendered with %u draw calls",
            static_cast<unsigned long long>(device.GetFrameNumber()),
            drawCommands);
        lastLoggedDrawCommands = drawCommands;
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

bool EditorImGui::IsTextInputActive() const
{
    if (!m_initialized)
        return false;

    ImGuiContext* context = ImGui::GetCurrentContext();
    if (!context)
        return false;
    return context->ActiveId != 0 && context->InputTextState.ID == context->ActiveId;
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
        mapped.x = static_cast<int>(ixtreeme::math::Round(u * static_cast<float>(diag.sceneViewExtent[0] - 1u)));
        mapped.y = static_cast<int>(ixtreeme::math::Round(v * static_cast<float>(diag.sceneViewExtent[1] - 1u)));
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
