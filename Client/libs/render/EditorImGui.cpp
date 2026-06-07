#include "EditorImGui.h"

#include "Debug.h"
#include "VulkanDevice.h"

#if defined(IXTREEME_WITH_EDITOR) && defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>
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

bool ContainsCaseInsensitive(const std::string& value, const std::string& needle)
{
    if (needle.empty())
        return true;
    return ToLowerAscii(value).find(ToLowerAscii(needle)) != std::string::npos;
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

ImVec4 AssetCategoryColor(AssetLibrary::Category category)
{
    switch (category)
    {
    case AssetLibrary::Category::Texture: return ImVec4(0.20f, 0.42f, 0.72f, 1.0f);
    case AssetLibrary::Category::Model: return ImVec4(0.48f, 0.38f, 0.70f, 1.0f);
    case AssetLibrary::Category::Animation: return ImVec4(0.72f, 0.50f, 0.20f, 1.0f);
    case AssetLibrary::Category::Material: return ImVec4(0.38f, 0.58f, 0.36f, 1.0f);
    case AssetLibrary::Category::WaterMaterial: return ImVec4(0.16f, 0.58f, 0.64f, 1.0f);
    default: return ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
    }
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
    m_applyDefaultDockLayout = !std::filesystem::exists(kLayoutFile);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 4.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
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

void EditorImGui::SetMapEditorSettings(const MapEditorSettings& settings)
{
    m_editorSettings = settings;
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

void EditorImGui::InitializeAssetLibrary(const std::filesystem::path& clientRoot)
{
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

MapEditorCommands EditorImGui::ConsumeCommands()
{
    MapEditorCommands commands = m_commands;
    m_commands = {};
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
    if (!m_assetLibrary)
        return result;

    for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
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
    };
    for (AssetLibrary::Category category : categories)
    {
        if (!ActiveAssetCategory(category))
            continue;
        for (const std::string& folder : m_assetLibrary->FolderSubpathsFor(category))
            folders.insert(AssetLibrary::NormalizeSubpath(folder));
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

    std::vector<std::pair<std::string, std::uint32_t>> result(counts.begin(), counts.end());
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second)
            return a.second > b.second;
        return a.first < b.first;
    });
    return result;
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
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Left, 0.18f, &leftId, &mainId);
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.24f, &rightId, &mainId);
        ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Down, 0.32f, &bottomId, &mainId);
        ImGui::DockBuilderDockWindow("Tools", leftId);
        ImGui::DockBuilderDockWindow("Inspector", rightId);
        ImGui::DockBuilderDockWindow("Asset Browser", bottomId);
        ImGui::DockBuilderFinish(dockspaceId);
    }
    ImGui::End();
}

void EditorImGui::RenderToolsPanel()
{
    if (ImGui::Begin("Tools"))
    {
        if (ImGui::Button("+ Water", ImVec2(-1.0f, 0.0f)))
        {
            m_commands.addWaterBody = true;
            Tracen("[EDITOR-3D-SPAWN] Add water requested");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Editing Tools");
        if (ImGui::Button("Water Sculpt Tool", ImVec2(-1.0f, 0.0f)))
            m_waterSculptToolOpen = !m_waterSculptToolOpen;
        if (ImGui::Button("Heightmap Tool", ImVec2(-1.0f, 0.0f)))
            m_heightmapToolOpen = !m_heightmapToolOpen;
        if (ImGui::Button("Splat Paint Tool", ImVec2(-1.0f, 0.0f)))
            m_splatPaintToolOpen = !m_splatPaintToolOpen;

        ImGui::Separator();
        if (ImGui::Button("Undo", ImVec2(-1.0f, 0.0f)))
            m_commands.undo = true;
        if (ImGui::Button("Save", ImVec2(-1.0f, 0.0f)))
            m_commands.save = true;
        if (ImGui::Button("Reload", ImVec2(-1.0f, 0.0f)))
            m_commands.reload = true;

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
    m_commands.selectedLightChanged = true;
    m_commands.selectedLight = m_dynamicLightState;
}

void EditorImGui::RenderSelectedWaterBodyInspector()
{
    if (!m_waterBodyState.selected)
        return;

    ImGui::Text("WATER BODY #%u", m_waterBodyState.id);
    ImGui::Separator();

    char nameBuffer[96]{};
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_waterBodyState.name.c_str());
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
    {
        m_waterBodyState.name = nameBuffer[0] != '\0' ? nameBuffer : ("Water_" + std::to_string(m_waterBodyState.id));
        MarkSelectedWaterBodyChanged();
    }

    bool changed = false;
    changed |= ImGui::SliderFloat("X", &m_waterBodyState.center[0], -500.0f, 500.0f, "%.1f");
    changed |= ImGui::SliderFloat("Water Level Y", &m_waterBodyState.config.waterLevelY, -50.0f, 100.0f, "%.2f m");
    changed |= ImGui::SliderFloat("Z", &m_waterBodyState.center[2], -500.0f, 500.0f, "%.1f");
    changed |= ImGui::SliderFloat("Bbox Width", &m_waterBodyState.width, 1.0f, 200.0f, "%.1f m");
    changed |= ImGui::SliderFloat("Bbox Depth", &m_waterBodyState.depth, 1.0f, 200.0f, "%.1f m");
    if (changed)
        MarkSelectedWaterBodyChanged();

    ImGui::Separator();
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

    if (ImGui::Button("Edit Material"))
    {
        m_commands.selectedWaterBodyChanged = true;
        m_commands.selectedWaterBody = m_waterBodyState;
        m_commands.openSelectedWaterMaterialEditor = true;
        OpenWaterMaterialEditor(m_waterBodyState.materialId);
    }
    ImGui::SameLine();
    if (ImGui::Button("Change Material"))
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
    if (ImGui::Button("Delete Water Body", ImVec2(-1.0f, 0.0f)))
        m_commands.deleteSelectedWaterBody = true;
}

void EditorImGui::RenderSelectedLightInspector()
{
    if (m_dynamicLightState.type == DynamicLightType::None)
        return;

    const bool isPoint = m_dynamicLightState.type == DynamicLightType::Point;
    const bool isSpot = m_dynamicLightState.type == DynamicLightType::Spot;
    ImGui::Text("%s LIGHT #%u", isPoint ? "POINT" : "SPOT", m_dynamicLightState.id);
    ImGui::Separator();

    if (isPoint)
    {
        PointLight& point = m_dynamicLightState.point;
        bool changed = false;
        changed |= ImGui::Checkbox("Enabled", &point.enabled);
        changed |= ImGui::SliderFloat("X", &point.position[0], -500.0f, 500.0f, "%.1f");
        changed |= ImGui::SliderFloat("Y", &point.position[1], -50.0f, 100.0f, "%.1f");
        changed |= ImGui::SliderFloat("Z", &point.position[2], -500.0f, 500.0f, "%.1f");
        changed |= ImGui::ColorEdit3("Color", &point.r);
        changed |= ImGui::SliderFloat("Intensity", &point.intensity, 0.0f, 10.0f, "%.2f");
        changed |= ImGui::SliderFloat("Radius", &point.radius, 0.5f, 100.0f, "%.1f m");
        if (changed)
            MarkSelectedLightChanged();
    }
    else if (isSpot)
    {
        SpotLight& spot = m_dynamicLightState.spot;
        bool changed = false;
        changed |= ImGui::Checkbox("Enabled", &spot.enabled);
        changed |= ImGui::SliderFloat("X", &spot.position[0], -500.0f, 500.0f, "%.1f");
        changed |= ImGui::SliderFloat("Y", &spot.position[1], -50.0f, 100.0f, "%.1f");
        changed |= ImGui::SliderFloat("Z", &spot.position[2], -500.0f, 500.0f, "%.1f");
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
    if (ImGui::Button("Delete Light", ImVec2(-1.0f, 0.0f)))
        m_commands.deleteSelectedLight = true;
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
    ImGui::TextUnformatted("Directional Light");
    ImGui::Checkbox("Sun Enabled", &m_lightingState.directional.enabled);
    ImGui::Checkbox("Sun Shadows", &m_lightingState.sunShadowsEnabled);
    ImGui::ColorEdit3("Sun Color", &m_lightingState.directional.r);
    ImGui::SliderFloat("Sun Intensity", &m_lightingState.directional.intensity, 0.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("Sun Angle X", &m_lightingState.directional.elevationDegrees, -90.0f, 90.0f, "%.1f deg");
    ImGui::SliderFloat("Sun Angle Y", &m_lightingState.directional.azimuthDegrees, 0.0f, 360.0f, "%.1f deg");

    ImGui::Spacing();
    ImGui::TextUnformatted("Ambient Light");
    ImGui::ColorEdit3("Ambient Color", &m_lightingState.ambient.r);
    ImGui::SliderFloat("Ambient Intensity", &m_lightingState.ambient.intensity, 0.0f, 3.0f, "%.2f");

    ImGui::Spacing();
    ImGui::TextUnformatted("Time of Day");
    ImGui::SliderFloat("Hour", &m_timeOfDayHours, 0.0f, 24.0f, "%.1f h");
    if (ImGui::Button("Apply Preset"))
        ApplyTimeOfDayPreset(m_timeOfDayHours);
}

void EditorImGui::RenderDynamicLightsPanel()
{
    const uint32_t total = m_lightingState.numPointLights + m_lightingState.numSpotLights;
    ImGui::Text("Total: %u / %u", total, kMaxDynamicPointLights + kMaxDynamicSpotLights);

    if (ImGui::Button("+ Point Light"))
    {
        m_commands.addPointLight = true;
        Tracen("[EDITOR-IMGUI-2] Light spawn: type=point id=pending");
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Spot Light"))
    {
        m_commands.addSpotLight = true;
        Tracen("[EDITOR-IMGUI-2] Light spawn: type=spot id=pending");
    }

    ImGui::Separator();
    for (uint32_t i = 0; i < m_lightingState.numPointLights; ++i)
    {
        PointLight point = m_lightingState.pointLights[i];
        ImGui::PushID(static_cast<int>(point.id));
        char label[64];
        std::snprintf(label, sizeof(label), "Light #%u (Point)", point.id);
        const bool selected = m_dynamicLightState.type == DynamicLightType::Point && m_dynamicLightState.id == point.id;
        if (ImGui::Selectable(label, selected))
        {
            m_dynamicLightState.type = DynamicLightType::Point;
            m_dynamicLightState.id = point.id;
            m_dynamicLightState.point = point;
            MarkSelectedLightChanged();
        }
        ImGui::PopID();
    }
    for (uint32_t i = 0; i < m_lightingState.numSpotLights; ++i)
    {
        SpotLight spot = m_lightingState.spotLights[i];
        ImGui::PushID(static_cast<int>(spot.id));
        char label[64];
        std::snprintf(label, sizeof(label), "Light #%u (Spot)", spot.id);
        const bool selected = m_dynamicLightState.type == DynamicLightType::Spot && m_dynamicLightState.id == spot.id;
        if (ImGui::Selectable(label, selected))
        {
            m_dynamicLightState.type = DynamicLightType::Spot;
            m_dynamicLightState.id = spot.id;
            m_dynamicLightState.spot = spot;
            MarkSelectedLightChanged();
        }
        ImGui::PopID();
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
    ImVec4 color = selected ? ImVec4(0.82f, 0.68f, 0.18f, 1.0f) : ImVec4(0.28f, 0.34f, 0.40f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    const std::string label = std::to_string(slotIndex) + "##splat_slot";
    if (ImGui::Button(label.c_str(), ImVec2(58.0f, 52.0f)))
    {
        m_editorSettings.textureSlot = slotIndex;
        m_editorSettings.tool = MapEditorTool::Paint;
        if (m_editorSettings.toolMode != MapEditorToolMode::SplatPaint)
            SetToolMode(MapEditorToolMode::SplatPaint);
    }
    ImGui::PopStyleColor();

    if (ImGui::BeginDragDropTarget())
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
    }
    ImGui::End();
}

void EditorImGui::RenderAssetBrowserToolbar()
{
    if (ImGui::Button("+ Texture"))
        ImportAssetWithDialog(AssetLibrary::Category::Texture);
    ImGui::SameLine();
    if (ImGui::Button("+ Model"))
        ImportAssetWithDialog(AssetLibrary::Category::Model);
    ImGui::SameLine();
    if (ImGui::Button("+ Anim"))
        ImportAssetWithDialog(AssetLibrary::Category::Animation);
    ImGui::SameLine();
    if (ImGui::Button("+ Material"))
        CreatePbrMaterialAsset();
    ImGui::SameLine();
    if (ImGui::Button("+ Water Mat"))
        CreateWaterMaterialAsset();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##asset_search", "Search assets...", m_assetSearchBuffer, sizeof(m_assetSearchBuffer));
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        m_assetSearchBuffer[0] = '\0';
        m_activeAssetTags.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        RefreshAssetLibrary();
}

void EditorImGui::RenderAssetTypeTabs()
{
    if (!ImGui::BeginTabBar("AssetTypes"))
        return;

    const auto tab = [this](const char* label, AssetBrowserFilter filter) {
        if (ImGui::BeginTabItem(label, nullptr, m_assetFilter == filter ? ImGuiTabItemFlags_SetSelected : 0))
        {
            if (m_assetFilter != filter)
            {
                m_assetFilter = filter;
                m_assetSubpath.clear();
                m_activeAssetTags.clear();
                m_selectedAssetId.clear();
            }
            ImGui::EndTabItem();
        }
    };

    tab("All", AssetBrowserFilter::All);
    tab("Textures", AssetBrowserFilter::Texture);
    tab("Models", AssetBrowserFilter::Model);
    tab("Anims", AssetBrowserFilter::Animation);
    tab("Materials", AssetBrowserFilter::Material);
    tab("Water Mats", AssetBrowserFilter::WaterMaterial);
    ImGui::EndTabBar();
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
    if (ImGui::Button("Home"))
    {
        m_assetSubpath.clear();
        m_selectedAssetId.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Up"))
    {
        m_assetSubpath = ParentSubpath(m_assetSubpath);
        m_selectedAssetId.clear();
    }

    if (ImGui::Button("+ Folder"))
        ImGui::OpenPopup("NewAssetFolder");
    ImGui::SameLine();
    if (ImGui::Button("Delete") && !m_assetSubpath.empty())
        DeleteAssetFolder();

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
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? ImVec4(0.80f, 0.68f, 0.22f, 1.0f) : categoryColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(categoryColor.x + 0.08f, categoryColor.y + 0.08f, categoryColor.z + 0.08f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(categoryColor.x * 0.8f, categoryColor.y * 0.8f, categoryColor.z * 0.8f, 1.0f));
    if (ImGui::Button("##asset_tile", ImVec2(tileSize, tileSize)))
    {
        m_selectedAssetId = entry.id;
        m_assetStatus = "Selected: " + entry.displayName;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            entry.category == AssetLibrary::Category::WaterMaterial)
        {
            OpenWaterMaterialEditor(entry.id);
        }
        else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            entry.category == AssetLibrary::Category::Material)
        {
            OpenPbrMaterialEditor(entry.id);
        }
    }
    ImGui::PopStyleColor(3);

    if (ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload(kAssetPayloadType, entry.id.data(), entry.id.size());
        ImGui::Text("%s", entry.displayName.c_str());
        ImGui::TextDisabled("%s", AssetLibrary::CategoryName(entry.category));
        ImGui::EndDragDropSource();
        Tracenf("[EDITOR-IMGUI-3] Drag started: asset_id=%s type=%s",
            entry.id.c_str(),
            AssetLibrary::CategoryName(entry.category));
    }

    if (ImGui::BeginPopupContextItem("AssetTileContext"))
    {
        if (ImGui::MenuItem("New Material"))
            CreatePbrMaterialAsset();
        if (ImGui::MenuItem("New Water Material"))
            CreateWaterMaterialAsset();
        ImGui::Separator();
        ImGui::TextDisabled("%s", entry.displayName.c_str());
        if (ImGui::MenuItem("Delete Asset"))
            DeleteAsset(entry);
        ImGui::EndPopup();
    }

    const char* badge = entry.category == AssetLibrary::Category::Texture
        ? AssetLibrary::TextureRoleBadge(entry.textureRole)
        : AssetLibrary::CategoryName(entry.category);
    ImGui::TextWrapped("%s", entry.displayName.c_str());
    ImGui::TextDisabled("%s", badge);
    if (!entry.thumbnail.empty())
        ImGui::TextDisabled("%s", entry.thumbnail.c_str());
    if (!entry.tags.empty())
        ImGui::TextDisabled("#%s", AssetLibrary::TagsToCsv(entry.tags).c_str());
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
    const float cellWidth = 128.0f;
    const float panelWidth = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>(panelWidth / cellWidth));
    int column = 0;
    for (const AssetLibrary::Entry& entry : visibleAssets)
    {
        ImGui::BeginGroup();
        RenderAssetTile(entry, tileSize);
        ImGui::EndGroup();

        ++column;
        if (column < columns)
            ImGui::SameLine();
        else
            column = 0;
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

        if (ImGui::BeginTable("AssetBrowserLayout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 190.0f);
            ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Tags", ImGuiTableColumnFlags_WidthFixed, 170.0f);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            RenderAssetFolderPanel();

            ImGui::TableSetColumnIndex(1);
            RenderAssetGrid();

            ImGui::TableSetColumnIndex(2);
            RenderAssetTagFilters();

            ImGui::EndTable();
        }

        if (!m_assetStatus.empty())
        {
            ImGui::Separator();
            ImGui::TextDisabled("%s", m_assetStatus.c_str());
        }
    }
    ImGui::End();
}

void EditorImGui::RenderGizmoControls()
{
    ImGui::Separator();
    ImGui::TextUnformatted("Gizmo");

    auto publish = [this]() {
        m_commands.gizmoSettingsChanged = true;
        m_commands.gizmoOperation = m_gizmoOperation;
        m_commands.gizmoSnapEnabled = m_gizmoSnapEnabled;
        m_commands.gizmoSnapValue = m_gizmoSnapValue;
    };

    if (ImGui::RadioButton("Translate (W)", m_gizmoOperation == MapEditorGizmoOperation::Translate))
    {
        m_gizmoOperation = MapEditorGizmoOperation::Translate;
        publish();
        Tracen("[EDITOR-GIZMO] Gizmo operation changed: translate");
    }
    if (ImGui::RadioButton("Rotate (E)", m_gizmoOperation == MapEditorGizmoOperation::Rotate))
    {
        m_gizmoOperation = MapEditorGizmoOperation::Rotate;
        publish();
        Tracen("[EDITOR-GIZMO] Gizmo operation changed: rotate");
    }
    if (ImGui::RadioButton("Scale (R)", m_gizmoOperation == MapEditorGizmoOperation::Scale))
    {
        m_gizmoOperation = MapEditorGizmoOperation::Scale;
        publish();
        Tracen("[EDITOR-GIZMO] Gizmo operation changed: scale");
    }

    bool snapChanged = ImGui::Checkbox("Snapping", &m_gizmoSnapEnabled);
    if (m_gizmoSnapEnabled)
    {
        const char* labels[] = {"0.1", "0.5", "1.0", "5.0"};
        snapChanged = ImGui::Combo("Snap", &m_gizmoSnapIndex, labels, IM_ARRAYSIZE(labels)) || snapChanged;
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
    ImGui::Text("Editing: %s%s", title.c_str(), m_waterMaterialEditor.dirty ? " *" : "");
    ImGui::InputText("Name", m_waterMaterialEditor.name, sizeof(m_waterMaterialEditor.name));

    if (ImGui::Button("Save"))
        SaveWaterMaterialEditor();
    ImGui::SameLine();
    if (ImGui::Button("New Material"))
        ImGui::OpenPopup("NewWaterMaterialPopup");
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
        ImGui::OpenPopup("DeleteWaterMaterialConfirm");

    if (ImGui::BeginPopup("NewWaterMaterialPopup"))
    {
        ImGui::InputText("Name", m_waterMaterialEditor.newName, sizeof(m_waterMaterialEditor.newName));
        if (ImGui::Button("Create"))
        {
            AssetLibrary::Entry created{};
            if (CreateWaterMaterialAsset(m_waterMaterialEditor.newName, created))
                OpenWaterMaterialEditor(created.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
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
        if (ImGui::Button("Yes, delete"))
        {
            DeleteWaterMaterialEditor();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
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
        if (m_waterMaterialEditor.materialId.empty())
        {
            ImGui::TextDisabled("No water material loaded.");
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
    }
    ImGui::End();
}

void EditorImGui::RenderPbrMaterialHeader()
{
    ImGui::Text("Editing: %s%s", m_pbrMaterialEditor.name, m_pbrMaterialEditor.dirty ? " *" : "");
    ImGui::InputText("Name", m_pbrMaterialEditor.name, sizeof(m_pbrMaterialEditor.name));
    if (ImGui::Button("Save"))
        SavePbrMaterialEditor();
    ImGui::SameLine();
    if (ImGui::Button("New Material"))
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
    }
    ImGui::End();
}

void EditorImGui::RenderInspector()
{
    if (ImGui::Begin("Inspector"))
    {
        if (m_waterBodyState.selected)
            RenderSelectedWaterBodyInspector();
        else if (m_dynamicLightState.type != DynamicLightType::None)
            RenderSelectedLightInspector();
        else
        {
            ImGui::TextUnformatted("Nothing selected");
            ImGui::TextWrapped("Click a water body or light in the 3D viewport, or select a light from Dynamic Lights.");
        }

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
            RenderLightingPanel();
        if (ImGui::CollapsingHeader("Dynamic Lights", ImGuiTreeNodeFlags_DefaultOpen))
            RenderDynamicLightsPanel();
        RenderGizmoControls();
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
    RenderToolsPanel();
    RenderAssetBrowser();
    RenderInspector();
    RenderWaterSculptToolPanel();
    RenderHeightmapToolPanel();
    RenderSplatPaintToolPanel();
    RenderWaterMaterialEditor();
    RenderPbrMaterialEditor();
}

void EditorImGui::Render(VulkanDevice& device)
{
    if (!m_initialized || !m_vulkanBackendReady || !m_frameActive)
        return;

    RenderEditorPanels();
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

bool EditorImGui::WantsInputCapture(const InputEvent&) const
{
    return false;
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
