#include "EditorImGui.h"

#include "NativeBackend.h"  // ixscript::NativeBackend::RegisteredNames() for the Script inspector
#include "platform/open_external.h"  // open .lua/.cpp scripts in the OS default editor

#include "AssetDatabase.h"
#include "AssimpExporter.h"
#include "AssetWatcher.h"
#include "Common.h"
#include "Debug.h"
#include "MaterialAssetManager.h"
#include "ProjectManager.h"
#include "SceneManager.h"
#include "map/LayeredWorld.h"
#include "math/IXMath.h"
#include "platform/trash.h"
#include "tools/tree/TreeTexturePalette.h"

#include <algorithm>  // script prompt handoff (both editor and runtime builds)

#if defined(IXTREEME_WITH_EDITOR) && defined(_WIN32)
#include "IconsFontAwesome6.h"
#include "UIHelpers.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include <commdlg.h>

namespace
{
constexpr const char* kLayoutFile = "editor_layout.ini";
constexpr const char* kRecentProjectsFile = "editor_recent_projects.txt";  // beside the layout
constexpr const char* kAssetPayloadType = "ASSET_ID";
constexpr const char* kAssetFolderPayloadType = "ASSET_FOLDER_PATH";
constexpr const char* kHierarchyEntityPayloadType = "HIERARCHY_ENTITY";
constexpr const char* kNativeClassPayloadType = "IXSCRIPT_NATIVE_CLASS";  // drag a registered C++ class
constexpr const char* kEditorNoteComponentId = "editor.note";
constexpr const char* kLodComponentId = "rendering.lod";
constexpr double kProjectAutoSaveIntervalSeconds = 5.0 * 60.0;
constexpr double kScriptFilePollIntervalSeconds = 0.5;
constexpr float kPi = ixtreeme::math::Pi;
constexpr float kGizmoPlaneScaleUnitsPerPixel = 0.01f;

struct HierarchyEntityDragPayload
{
    int type = 0;
    std::uint32_t id = 0;
    std::uint64_t entity = 0;
};

struct PrefabInspectorEntity
{
    std::uint32_t localId = 0;
    std::uint32_t parentLocalId = 0;
    std::string type;
    std::string lightType;
    std::string name;
    std::string meshAssetId;
    std::string meshAssetPath;
    std::string prefabAssetId;
    std::vector<std::string> materialSlots;
    bool transformValid = false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[3] = {0.0f, 0.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
    bool lightValid = false;
    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 1.0f;
    float innerConeDegrees = 20.0f;
    float outerConeDegrees = 35.0f;
    bool enabled = true;
};

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

WorldVec3 TransformEditorColliderLocalPoint(const MeshRendererEditorState& mesh, WorldVec3 local)
{
    namespace xm = ixtreeme::math;
    xm::Mat4 model = xm::MultiplyRowMajor(
        xm::MultiplyRowMajor(
            xm::MultiplyRowMajor(
                xm::MultiplyRowMajor(xm::Scale({mesh.scale[0], mesh.scale[1], mesh.scale[2]}),
                    xm::RotationX(mesh.rotation[0])),
                xm::RotationYRowMajor(mesh.rotation[1])),
            xm::RotationZ(mesh.rotation[2])),
        xm::Translation({mesh.position[0], mesh.position[1], mesh.position[2]}));
    const xm::Vec3 world = xm::TransformPointRowVector(model.m, {local.x, local.y, local.z});
    return {world.x, world.y, world.z};
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

const std::array<InspectorComponentDefinition, 20>& InspectorComponentRegistry()
{
    static const std::array<InspectorComponentDefinition, 20> registry{{
        {"builtin.transform", "Transform", "Core", EditorComponentType::None, false},
        {"builtin.mesh_renderer", "MeshRenderer", "Rendering", EditorComponentType::MeshRenderer, false},
        {kLodComponentId, "LOD Group", "Rendering", EditorComponentType::None, true},
        {"physics.rigidbody", "Rigidbody", "Physics", EditorComponentType::Rigidbody, true},
        {"physics.box_collider", "Box Collider", "Physics", EditorComponentType::BoxCollider, true},
        {"physics.sphere_collider", "Sphere Collider", "Physics", EditorComponentType::SphereCollider, true},
        {"physics.capsule_collider", "Capsule Collider", "Physics", EditorComponentType::CapsuleCollider, true},
        {"physics.trigger_box", "Trigger Box", "Physics", EditorComponentType::TriggerBox, true},
        {"physics.trigger_sphere", "Trigger Sphere", "Physics", EditorComponentType::TriggerSphere, true},
        {"physics.trigger_capsule", "Trigger Capsule", "Physics", EditorComponentType::TriggerCapsule, true},
        {"physics.fixed_joint", "Fixed Joint", "Physics", EditorComponentType::FixedJoint, true},
        {"physics.hinge_joint", "Hinge Joint", "Physics", EditorComponentType::HingeJoint, true},
        {"physics.character_controller", "Character Controller", "Physics", EditorComponentType::CharacterController, true},
        {"audio.audio_source", "Audio Source", "Audio", EditorComponentType::AudioSource, true},
        {"audio.audio_listener", "Audio Listener", "Audio", EditorComponentType::AudioListener, true},
        {"scripting.script", "Script", "Scripting", EditorComponentType::Script, true},
        {"builtin.water_body", "Water Body", "Rendering", EditorComponentType::WaterBody, false},
        {"builtin.point_light", "Point Light", "Lighting", EditorComponentType::PointLight, false},
        {"builtin.spot_light", "Spot Light", "Lighting", EditorComponentType::SpotLight, false},
        {kEditorNoteComponentId, "Note", "Editor", EditorComponentType::None, true},
    }};
    return registry;
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
    const std::string base = "NewFolder";  // a valid name as is (IsValidRenameName rejects spaces)
    if (!std::filesystem::exists(parent / base, ec))
        return base;
    for (int i = 2; i < 1000; ++i)
    {
        const std::string candidate = base + "_" + std::to_string(i);
        ec.clear();
        if (!std::filesystem::exists(parent / candidate, ec))
            return candidate;
    }
    return base + "_1000";
}

bool IsSubpathOrSelf(const std::string& maybeChild, const std::string& maybeParent)
{
    const std::string child = AssetLibrary::NormalizeSubpath(maybeChild);
    const std::string parent = AssetLibrary::NormalizeSubpath(maybeParent);
    if (parent.empty())
        return true;
    return child == parent || child.rfind(parent + "/", 0) == 0;
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
    case AssetLibrary::Category::PhysicsMaterial: return ImVec4(0.64f, 0.54f, 0.30f, 1.0f);
    case AssetLibrary::Category::Scene: return ImVec4(0.42f, 0.50f, 0.66f, 1.0f);
    case AssetLibrary::Category::Prefab: return ImVec4(0.67f, 0.48f, 0.82f, 1.0f);
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
    case AssetLibrary::Category::PhysicsMaterial: return ICON_FA_GEAR;
    case AssetLibrary::Category::Scene: return ICON_FA_GLOBE;
    case AssetLibrary::Category::Prefab: return ICON_FA_LAYER_GROUP;
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
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac")
        return "Audio";
    if (ext == ".lua")
        return "Script";
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

void EditorImGui::SetTextureProvider(ixeditor::graphics::IEditorTextureProvider* provider)
{
    m_textureProvider = provider;
}

EditorImGui::~EditorImGui()
{
    UnloadGameModules();  // close any loaded game-module DLLs (purges their registry entries first)
    Destroy();
}

bool EditorImGui::Create()
{
    if (m_initialized)
        return true;

    // Generic UI init only (context/IO/fonts/style). The backend adapter owns
    // the ImGui context lifecycle and the Vulkan/Win32 backend init.
    if (ImGui::GetCurrentContext() == nullptr)
    {
        TraceError("[EDITOR-IMGUI] No ImGui context: create the backend adapter first");
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.IniFilename = kLayoutFile;
    ProjectManager::Instance().SetRecentProjectsFile(kRecentProjectsFile);
    m_applyDefaultDockLayout = !std::filesystem::exists(kLayoutFile);

    LoadEditorFonts();
    ApplyEditorStyle();

    m_initialized = true;
    Tracenf("[EDITOR-IMGUI] Initialized with imgui version %s", IMGUI_VERSION);
    Tracen("[EDITOR-IMGUI] Docking enabled, multi-viewport enabled");
    Tracenf("[EDITOR-IMGUI] Layout file: %s", kLayoutFile);
    if (!m_applyDefaultDockLayout)
        Tracen("[EDITOR-LAYOUT] Loaded layout from editor_layout.ini");
    return true;
}

void EditorImGui::SetMapEditorSettings(const MapEditorSettings& settings)
{
    m_editorSettings = settings;
}

void EditorImGui::SetEditorPlayModeState(const EditorPlayModeState& state)
{
    const bool wasEditing = m_playModeState.mode == EditorPlayMode::Edit;
    const bool nowEditing = state.mode == EditorPlayMode::Edit;
    if (wasEditing && !nowEditing)
        m_pendingViewFocusWindow = "Game";        // entered Play: show the Main Camera
    else if (!wasEditing && nowEditing)
        m_pendingViewFocusWindow = "Scene View";  // stopped Play: back to free-fly editor camera
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

void EditorImGui::SetCameraEditorState(const CameraEditorState& state)
{
    m_cameraEditorState = state;
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

void EditorImGui::SetPhysicsEvents(std::vector<PhysicsEventEditorState> events)
{
    m_physicsEvents = std::move(events);
}

void EditorImGui::SetHierarchySceneState(std::uint64_t sceneRootEntity,
                                         std::string sceneRootName,
                                         std::vector<HierarchySceneEntity> entities)
{
    const std::uint64_t previousSelectedEntity = m_selectedHierarchyEntity;
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
    if (m_selectedHierarchyEntity != 0 && m_selectedHierarchyEntity != previousSelectedEntity)
        m_assetInspectorSelectionActive = false;
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
    InvalidateAssetBrowserCache();
    m_assetLibrary = std::make_unique<AssetLibrary>(clientRoot);
    if (!m_assetLibrary->Initialize())
    {
        m_assetLibrary.reset();
        m_assetStatus = "Asset library init failed";
        TraceError("[EDITOR-IMGUI-3] Asset library initialization failed");
        return;
    }

    m_assetStatus = "Asset library ready";
    m_selectedAssetId.clear();
    m_assetInspectorSelectionActive = false;
    tree_tool::TreeTexturePalette::Instance().EnsureLoaded(InternalAssetRootFor(m_engineRoot));
    SyncWaterMaterialSnapshot();
    Tracenf("[EDITOR-IMGUI-3] Asset library root=%s", m_assetLibrary->LibraryRoot().generic_string().c_str());
}

void EditorImGui::InitializeProjectAssetLibrary(const std::filesystem::path& projectRoot,
                                                const std::filesystem::path& assetRoot)
{
    DestroyAssetPreviewTextures();
    InvalidateAssetBrowserCache();
    m_assetLibrary = std::make_unique<AssetLibrary>(projectRoot, assetRoot);
    if (!m_assetLibrary->Initialize())
    {
        m_assetLibrary.reset();
        m_assetStatus = "Project asset library init failed";
        TraceError("[PROJECT] asset library initialization failed: %s", assetRoot.generic_string().c_str());
        return;
    }

    m_assetSubpath.clear();
    m_selectedAssetId.clear();
    m_assetInspectorSelectionActive = false;
    m_activeAssetTags.clear();
    m_assetStatus = "Project assets ready";
    tree_tool::TreeTexturePalette::Instance().EnsureLoaded(InternalAssetRootFor(m_engineRoot));
    SyncWaterMaterialSnapshot();
    Tracenf("[PROJECT] asset browser root=%s", m_assetLibrary->LibraryRoot().generic_string().c_str());
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

    if (m_sceneGizmoVisible &&
        m_editSelectedColliderInScene &&
        type == HierarchyEntityType::MeshEntity &&
        m_meshRendererState.selected &&
        m_meshRendererState.id == id &&
        m_meshRendererState.hasCollider)
    {
        const auto& collider = m_meshRendererState.collider;
        const WorldVec3 colliderWorldCenter = TransformEditorColliderLocalPoint(
            m_meshRendererState,
            {collider.center[0], collider.center[1], collider.center[2]});
        m_sceneGizmoPosition[0] = colliderWorldCenter.x;
        m_sceneGizmoPosition[1] = colliderWorldCenter.y;
        m_sceneGizmoPosition[2] = colliderWorldCenter.z;
        m_sceneGizmoRotation[0] = m_meshRendererState.rotation[0];
        m_sceneGizmoRotation[1] = m_meshRendererState.rotation[1];
        m_sceneGizmoRotation[2] = m_meshRendererState.rotation[2];
        if (operation == MapEditorGizmoOperation::Scale)
        {
            if (collider.shape == ixtreeme::physics::ColliderShape::Sphere)
            {
                const float diameter = std::max(0.001f, collider.radius * 2.0f);
                m_sceneGizmoScale[0] = diameter * std::max(0.001f, m_meshRendererState.scale[0]);
                m_sceneGizmoScale[1] = diameter * std::max(0.001f, m_meshRendererState.scale[1]);
                m_sceneGizmoScale[2] = diameter * std::max(0.001f, m_meshRendererState.scale[2]);
            }
            else if (collider.shape == ixtreeme::physics::ColliderShape::Capsule)
            {
                const float diameter = std::max(0.001f, collider.radius * 2.0f);
                m_sceneGizmoScale[0] = diameter * std::max(0.001f, m_meshRendererState.scale[0]);
                m_sceneGizmoScale[1] = std::max(0.001f, collider.height) * std::max(0.001f, m_meshRendererState.scale[1]);
                m_sceneGizmoScale[2] = diameter * std::max(0.001f, m_meshRendererState.scale[2]);
            }
            else
            {
                m_sceneGizmoScale[0] = std::max(0.001f, collider.size[0]) * std::max(0.001f, m_meshRendererState.scale[0]);
                m_sceneGizmoScale[1] = std::max(0.001f, collider.size[1]) * std::max(0.001f, m_meshRendererState.scale[1]);
                m_sceneGizmoScale[2] = std::max(0.001f, collider.size[2]) * std::max(0.001f, m_meshRendererState.scale[2]);
            }
            m_sceneGizmoOperation = MapEditorGizmoOperation::Scale;
        }
        else
        {
            m_sceneGizmoScale[0] = 0.35f;
            m_sceneGizmoScale[1] = 0.35f;
            m_sceneGizmoScale[2] = 0.35f;
            m_sceneGizmoOperation = MapEditorGizmoOperation::Translate;
        }
    }
}

std::uint32_t JsonU32ValueForInspector(const std::string& object, const std::string& key, std::uint32_t fallback = 0)
{
    const float value = ixtreeme::common::JsonFloatValue(object, key, static_cast<float>(fallback));
    return value < 0.0f ? fallback : static_cast<std::uint32_t>(value);
}

std::size_t FindMatchingJsonBracket(const std::string& text, std::size_t open, char openCh, char closeCh)
{
    bool inString = false;
    bool escaping = false;
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (inString)
        {
            if (escaping)
                escaping = false;
            else if (ch == '\\')
                escaping = true;
            else if (ch == '"')
                inString = false;
            continue;
        }
        if (ch == '"')
        {
            inString = true;
            continue;
        }
        if (ch == openCh)
            ++depth;
        else if (ch == closeCh)
        {
            --depth;
            if (depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

std::vector<std::string> ExtractJsonArrayObjectsForInspector(const std::string& text, const std::string& key)
{
    std::vector<std::string> objects;
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos)
        return objects;
    const std::size_t openArray = text.find('[', keyPos + needle.size());
    if (openArray == std::string::npos)
        return objects;
    const std::size_t closeArray = FindMatchingJsonBracket(text, openArray, '[', ']');
    if (closeArray == std::string::npos)
        return objects;

    std::size_t cursor = openArray + 1;
    while (cursor < closeArray)
    {
        const std::size_t openObject = text.find('{', cursor);
        if (openObject == std::string::npos || openObject >= closeArray)
            break;
        const std::size_t closeObject = FindMatchingJsonBracket(text, openObject, '{', '}');
        if (closeObject == std::string::npos || closeObject > closeArray)
            break;
        objects.push_back(text.substr(openObject, closeObject - openObject + 1));
        cursor = closeObject + 1;
    }
    return objects;
}

std::string ExtractJsonObjectForInspector(const std::string& text, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos)
        return {};
    const std::size_t openObject = text.find('{', keyPos + needle.size());
    if (openObject == std::string::npos)
        return {};
    const std::size_t closeObject = FindMatchingJsonBracket(text, openObject, '{', '}');
    return closeObject == std::string::npos ? std::string{} : text.substr(openObject, closeObject - openObject + 1);
}

bool ReadPrefabAssetForInspector(const std::filesystem::path& path,
                                 std::string& outName,
                                 std::vector<PrefabInspectorEntity>& outEntities,
                                 std::string& outError)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        outError = "failed to open prefab file";
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    outName = ixtreeme::common::JsonStringValue(text, "name");

    std::vector<std::string> entityObjects = ExtractJsonArrayObjectsForInspector(text, "entities");
    if (entityObjects.empty())
    {
        const std::string legacyEntity = ExtractJsonObjectForInspector(text, "entity");
        if (!legacyEntity.empty())
            entityObjects.push_back(legacyEntity);
    }

    outEntities.clear();
    outEntities.reserve(entityObjects.size());
    for (const std::string& object : entityObjects)
    {
        PrefabInspectorEntity entity{};
        entity.localId = JsonU32ValueForInspector(object, "local_id", static_cast<std::uint32_t>(outEntities.size() + 1u));
        entity.parentLocalId = JsonU32ValueForInspector(object, "parent_local_id", 0);
        entity.type = ixtreeme::common::JsonStringValue(object, "type");
        entity.lightType = ixtreeme::common::JsonStringValue(object, "light_type");
        entity.name = ixtreeme::common::JsonStringValue(object, "name");
        entity.prefabAssetId = ixtreeme::common::JsonStringValue(object, "prefab_asset_id");
        entity.meshAssetId = ixtreeme::common::JsonStringValue(object, "mesh_asset_id");
        entity.meshAssetPath = ixtreeme::common::JsonStringValue(object, "mesh_asset_path");
        if (entity.type == "mesh_entity")
        {
            entity.transformValid = true;
            entity.materialSlots = ixtreeme::common::JsonStringArrayValue(object, "materials");
            ixtreeme::common::JsonFloatArrayValue(object, "position", entity.position, 3);
            ixtreeme::common::JsonFloatArrayValue(object, "rotation", entity.rotation, 3);
            ixtreeme::common::JsonFloatArrayValue(object, "scale", entity.scale, 3);
        }
        else if (entity.type == "dynamic_light")
        {
            entity.transformValid = true;
            entity.lightValid = true;
            ixtreeme::common::JsonFloatArrayValue(object, "position", entity.position, 3);
            if (entity.lightType == "spot")
                ixtreeme::common::JsonFloatArrayValue(object, "rotation", entity.rotation, 3);
            ixtreeme::common::JsonFloatArrayValue(object, "color", entity.color, 3);
            entity.intensity = ixtreeme::common::JsonFloatValue(object, "intensity", entity.intensity);
            entity.radius = ixtreeme::common::JsonFloatValue(object, "radius", entity.radius);
            entity.innerConeDegrees = ixtreeme::common::JsonFloatValue(object, "inner_cone_deg", entity.innerConeDegrees);
            entity.outerConeDegrees = ixtreeme::common::JsonFloatValue(object, "outer_cone_deg", entity.outerConeDegrees);
            entity.enabled = ixtreeme::common::JsonBoolValue(object, "enabled", entity.enabled);
        }
        if (entity.name.empty())
            entity.name = entity.type.empty() ? "Entity" : entity.type;
        outEntities.push_back(std::move(entity));
    }
    return true;
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
        const bool showPhysicsColliders = commands.showPhysicsColliders;
        const bool showPhysicsContacts = commands.showPhysicsContacts;
        const bool showPhysicsBodyCenters = commands.showPhysicsBodyCenters;
        const bool physicsLayerMatrixChanged = commands.physicsLayerMatrixChanged;
        const PhysicsLayerMatrix physicsLayerMatrix = commands.physicsLayerMatrix;
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
        commands.showPhysicsColliders = showPhysicsColliders;
        commands.showPhysicsContacts = showPhysicsContacts;
        commands.showPhysicsBodyCenters = showPhysicsBodyCenters;
        commands.physicsLayerMatrixChanged = physicsLayerMatrixChanged;
        commands.physicsLayerMatrix = physicsLayerMatrix;
        commands.renderResolutionChanged = renderResolutionChanged;
        commands.renderResolutionUseNative = renderResolutionUseNative;
        commands.renderResolutionWidth = renderResolutionWidth;
        commands.renderResolutionHeight = renderResolutionHeight;
    }
    commands.showLayerVolumes = m_showLayerVolumes;
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

void EditorImGui::EnsureModelAnimationClips(const std::filesystem::path& modelPath,
    const std::vector<std::string>& jointNames)
{
    if (!m_assetLibrary || jointNames.empty() || modelPath.empty())
        return;

    const std::filesystem::path modelDir = modelPath.parent_path();
    const std::string stem = modelPath.stem().string();
    // The clips go next to the model, in <model>_clips (no per-type folder).
    std::string clipFolder;
    clipFolder.reserve(stem.size() + 6);
    for (char c : stem)
        clipFolder.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    clipFolder += "_clips";
    const std::string modelSubpath = AssetBrowserSubpath(modelDir);
    const std::string subpath = modelSubpath.empty() ? clipFolder : modelSubpath + "/" + clipFolder;

    bool createdAny = false;
    for (int i = 0; i < 8; ++i)
    {
        std::error_code ec;
        const std::filesystem::path animPath = modelDir / (stem + "_anim_" + std::to_string(i) + ".ozz");
        if (!std::filesystem::exists(animPath, ec))
            continue;  // clip indices may be sparse

        const std::string displayName = stem + "_anim_" + std::to_string(i);
        bool alreadyPresent = false;
        for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
        {
            if (entry.category == AssetLibrary::Category::AnimationClip &&
                entry.displayName == displayName &&
                AssetLibrary::NormalizeSubpath(entry.subpath) == AssetLibrary::NormalizeSubpath(subpath))
            {
                alreadyPresent = true;
                break;
            }
        }
        if (alreadyPresent)
            continue;

        AssetLibrary::ImportOptions options;
        options.displayName = displayName;
        options.subpath = subpath;
        AssetLibrary::AnimationClipData clip;
        clip.jointNames = jointNames;
        clip.loop = true;
        clip.rootJoint = jointNames.front();
        clip.sourceAnimPath = animPath.generic_string();
        AssetLibrary::Entry outEntry;
        std::string error;
        if (m_assetLibrary->CreateAnimationClip(options, clip, outEntry, error))
        {
            createdAny = true;
            Tracenf("[ANIM-CLIP] generated from model id=%s model=%s joints=%zu",
                outEntry.id.c_str(), stem.c_str(), jointNames.size());
        }
        else
        {
            Tracenf("[ANIM-CLIP] generate failed model=%s anim=%d error=%s", stem.c_str(), i, error.c_str());
        }
    }
    if (createdAny)
        m_assetStatus = "Animation clips generated for " + stem;
}

std::string EditorImGui::AnimationClipFilePath(const std::string& clipId) const
{
    if (!m_assetLibrary || clipId.empty())
        return {};
    const auto entry = m_assetLibrary->FindById(clipId);
    if (!entry || entry->category != AssetLibrary::Category::AnimationClip)
        return {};
    return m_assetLibrary->AbsolutePath(*entry).generic_string();
}

std::string EditorImGui::AnimatorControllerFilePath(const std::string& controllerId) const
{
    if (!m_assetLibrary || controllerId.empty())
        return {};
    const auto entry = m_assetLibrary->FindById(controllerId);
    if (!entry || entry->category != AssetLibrary::Category::AnimatorController)
        return {};
    return m_assetLibrary->AbsolutePath(*entry).generic_string();
}

std::string EditorImGui::AudioClipFilePath(const std::string& clipId) const
{
    if (!m_assetLibrary || clipId.empty())
        return {};
    const auto entry = m_assetLibrary->FindById(clipId);
    if (!entry || entry->category != AssetLibrary::Category::Audio)
        return {};
    return m_assetLibrary->AbsolutePath(*entry).generic_string();
}

std::string EditorImGui::ScriptSourceFilePath(const std::string& scriptId) const
{
    if (!m_assetLibrary || scriptId.empty())
        return {};
    const auto entry = m_assetLibrary->FindById(scriptId);
    if (!entry || entry->category != AssetLibrary::Category::Script)
        return {};
    return m_assetLibrary->AbsolutePath(*entry).generic_string();
}

std::string EditorImGui::FindAnimationClipIdByDisplayName(const std::string& displayName) const
{
    if (!m_assetLibrary || displayName.empty())
        return {};
    for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
    {
        if (entry.category == AssetLibrary::Category::AnimationClip && entry.displayName == displayName)
            return entry.id;
    }
    return {};
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
    m_assetInspectorSelectionActive = false;
    m_activeAssetTags.clear();
}

void EditorImGui::ValidateAssetBrowserCache() const
{
    constexpr double kAssetBrowserCacheSeconds = 10.0;
    const double now = ImGui::GetTime();
    const std::uint64_t revision = m_assetLibrary ? m_assetLibrary->Revision() : 0;
    AssetBrowserCache& cache = m_assetBrowserCache;
    if (cache.builtAt >= 0.0 && now >= cache.builtAt && now - cache.builtAt < kAssetBrowserCacheSeconds &&
        cache.library == m_assetLibrary.get() && cache.revision == revision)
        return;
    cache = {};
    cache.library = m_assetLibrary.get();
    cache.revision = revision;
    cache.builtAt = now;
}

std::string EditorImGui::CachedComparablePath(const std::filesystem::path& path) const
{
    ValidateAssetBrowserCache();
    auto [cached, inserted] = m_assetBrowserCache.comparablePaths.try_emplace(path.generic_string());
    if (inserted)
        cached->second = ComparablePath(path);
    return cached->second;
}

std::vector<std::string> EditorImGui::QueryFilesystemChildFolders(const std::string& subpath) const
{
    ValidateAssetBrowserCache();
    if (const auto cached = m_assetBrowserCache.childFolders.find(subpath); cached != m_assetBrowserCache.childFolders.end())
        return cached->second;
    std::vector<std::string>& folders = m_assetBrowserCache.childFolders[subpath];
    const std::filesystem::path directory = AssetBrowserPath(subpath);
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec))
        return folders;

    for (const auto& entry : std::filesystem::directory_iterator(directory, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec)
            break;
        try
        {
            std::error_code itemEc;
            if (!entry.is_directory(itemEc))
                continue;
            const std::string name = entry.path().filename().string();
            // Hidden folders and the library's generated thumbnails are not part of the user's tree.
            if (name.empty() || name.front() == '.' || name.find(".delete_tmp") != std::string::npos)
                continue;
            const std::string child = AssetBrowserSubpath(entry.path());
            if (AssetLibrary::IsInternalFolder(child))
                continue;
            folders.push_back(child);
        }
        catch (const std::exception&)
        {
            // a folder name the narrow path API cannot represent: not listed
        }
    }
    std::sort(folders.begin(), folders.end(), [](const std::string& a, const std::string& b) {
        return ToLowerAscii(FolderDisplayName(a)) < ToLowerAscii(FolderDisplayName(b));
    });
    return folders;
}

std::vector<AssetLibrary::Entry> EditorImGui::QueryFilesystemAssetsInFolder(const std::string& subpath) const
{
    if (!m_assetLibrary)
        return {};
    ValidateAssetBrowserCache();
    if (const auto cached = m_assetBrowserCache.folderAssets.find(subpath); cached != m_assetBrowserCache.folderAssets.end())
        return cached->second;
    std::vector<AssetLibrary::Entry>& result = m_assetBrowserCache.folderAssets[subpath];

    // Library entries know their folder (relative to the asset root): no path canonicalization needed.
    const std::string target = ToLowerAscii(AssetLibrary::NormalizeSubpath(subpath));
    for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
    {
        if (ToLowerAscii(AssetLibrary::NormalizeSubpath(entry.subpath)) == target && !IsMetaFile(entry.filename))
            result.push_back(entry);
    }

    const std::string targetComparable = ComparablePath(AssetBrowserPath(subpath));
    for (const AssetLibrary::Entry& entry : QuerySceneAssets())
    {
        if (entry.originalPath.empty())
            continue;
        const std::filesystem::path scenePath(entry.originalPath);
        if (ComparablePath(scenePath.parent_path()) == targetComparable)
            result.push_back(entry);
    }

    std::sort(result.begin(), result.end(), [](const AssetLibrary::Entry& a, const AssetLibrary::Entry& b) {
        return ToLowerAscii(a.filename.empty() ? a.displayName : a.filename) <
            ToLowerAscii(b.filename.empty() ? b.displayName : b.filename);
    });
    return result;
}

void EditorImGui::DestroyAssetPreviewTexture(AssetPreviewTexture& texture)
{
    if (texture.handle.IsValid() && m_textureProvider)
        m_textureProvider->ReleasePreviewTexture(texture.handle);
    texture = {};
}

void EditorImGui::DestroyAssetPreviewTextures()
{
    // Backend waits idle internally (parity: previews were destroyed under
    // idle before); UI registrations are released with the textures.
    if (m_textureProvider)
        m_textureProvider->ReleaseAllPreviewTextures();
    for (auto& preview : m_assetPreviewTextures)
        preview.second = {};
    m_assetPreviewTextures.clear();
}

std::optional<std::filesystem::path> EditorImGui::AssetPreviewPathFor(const AssetLibrary::Entry& entry) const
{
    if (!m_assetLibrary)
        return std::nullopt;
    ValidateAssetBrowserCache();
    auto [cached, inserted] = m_assetBrowserCache.previewPaths.try_emplace(entry.id);
    if (inserted)
        cached->second = ResolveAssetPreviewPath(entry);
    return cached->second;
}

std::optional<std::filesystem::path> EditorImGui::ResolveAssetPreviewPath(const AssetLibrary::Entry& entry) const
{
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
    outTexture = {};
    if (!m_textureProvider || !m_textureProvider->IsReady())
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

    // Upload + UI registration through the backend provider (was: inline
    // staging upload + inline view/sampler/AddTexture here).
    outTexture.handle = m_textureProvider->UploadPreviewTexture(decoded,
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
        path.filename().generic_string().c_str());
    stbi_image_free(decoded);
    if (!outTexture.handle.IsValid())
        return false;

    outTexture.width = static_cast<uint32_t>(width);
    outTexture.height = static_cast<uint32_t>(height);
    Tracenf("[EDITOR-ASSET-PREVIEW] Loaded thumbnail: %s %dx%d", path.string().c_str(), width, height);
    return true;
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
    if (it->second.failed || !it->second.handle.IsValid())
        return nullptr;
    if (m_textureProvider == nullptr ||
        m_textureProvider->GetPreviewTexture(it->second.handle) == nullptr)
        return nullptr;
    return &it->second;
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
    m_assetRenameAssetId = m_assetLibrary->FindById(entry.id) ? entry.id : std::string{};
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
    m_assetRenameAssetId.clear();
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

    // A library asset is renamed by the library: file and .meta together, keeping its id (and with
    // it every reference to the asset).
    if (!m_assetRenameIsFolder && m_assetLibrary && !m_assetRenameAssetId.empty())
    {
        AssetLibrary::Entry renamed;
        std::string error;
        if (!m_assetLibrary->RenameAsset(m_assetRenameAssetId, requested, true, renamed, error))
        {
            m_assetStatus = "Rename failed: " + error;
            return;
        }
        m_assetStatus = "Renamed: " + renamed.filename;
        Tracenf("[EDITOR-ASSET-BROWSER] renamed asset id=%s dst=%s",
            renamed.id.c_str(),
            renamed.originalPath.c_str());
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

    const std::string sourceSubpath = AssetBrowserSubpath(source);
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

    // The assets inside a renamed folder keep their ids: their entries follow the folder.
    std::string relocateError;
    if (m_assetRenameIsFolder && m_assetLibrary &&
        !m_assetLibrary->RelocateFolder(sourceSubpath, AssetBrowserSubpath(destination), relocateError))
    {
        TraceError("[EDITOR-ASSET-BROWSER] folder rename manifest update failed: %s", relocateError.c_str());
    }
    RefreshAssetLibrary();
    if (m_assetRenameIsFolder && AssetLibrary::NormalizeSubpath(m_assetSubpath).rfind(sourceSubpath, 0) == 0)
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
    {
        m_selectedAssetId.clear();
        m_assetInspectorSelectionActive = false;
    }
    // Drop the deleted assets' entries outright, so the refresh never mistakes another file with the
    // same name for one of them having moved.
    std::string forgetError;
    if (m_assetLibrary && !m_assetLibrary->ForgetPath(path, forgetError))
        TraceError("[EDITOR-ASSET-BROWSER] delete manifest update failed: %s", forgetError.c_str());
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
    const std::string target = AssetLibrary::NormalizeSubpath(targetFolderSubpath);
    if (ToLowerAscii(AssetLibrary::NormalizeSubpath(entry->subpath)) == ToLowerAscii(target))
        return true;

    // The library moves the file and its .meta and keeps the asset's id.
    const std::filesystem::path source = m_assetLibrary->AbsolutePath(*entry);
    AssetLibrary::Entry moved;
    std::string error;
    if (!m_assetLibrary->MoveAssetToSubpath(assetId, target, moved, error))
    {
        m_assetStatus = "Move failed: " + error;
        Tracenf("[EDITOR-ASSET-BROWSER] move_failed src=%s target=Assets/%s reason=%s",
            source.generic_string().c_str(),
            target.c_str(),
            error.c_str());
        return false;
    }
    m_assetStatus = "Moved: " + moved.filename;
    Tracenf("[EDITOR-ASSET-BROWSER] moved id=%s src=%s dst=%s",
        moved.id.c_str(),
        source.generic_string().c_str(),
        moved.originalPath.c_str());
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
    // The assets inside keep their ids: their entries follow the folder.
    std::string relocateError;
    if (m_assetLibrary && !m_assetLibrary->RelocateFolder(sourceNorm, AssetBrowserSubpath(destination), relocateError))
        TraceError("[EDITOR-ASSET-BROWSER] folder move manifest update failed: %s", relocateError.c_str());
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
    {
        const std::filesystem::path source(path);
        std::error_code ec;
        if (std::filesystem::is_directory(source, ec))
            ImportFolderFromPath(source, target);
        else
            ImportAssetFromPath(source, target, trigger ? trigger : "dragdrop");
    }
}

void EditorImGui::ImportFolderFromPath(const std::filesystem::path& sourceFolder, const std::string& targetSubpath)
{
    if (!m_assetLibrary)
        return;
    std::error_code ec;
    std::filesystem::path source = std::filesystem::absolute(sourceFolder, ec).lexically_normal();
    if (!source.has_filename())
        source = source.parent_path();
    // A folder that already is (or holds) the asset folder is not copied into itself.
    const std::string sourceKey = ComparablePath(source);
    const std::string rootKey = ComparablePath(AssetBrowserRoot());
    if (sourceKey == rootKey || rootKey.rfind(sourceKey + "/", 0) == 0 || sourceKey.rfind(rootKey + "/", 0) == 0)
    {
        m_assetStatus = "Import skipped: that folder is already part of the project assets";
        return;
    }

    const std::filesystem::path targetParent = AssetBrowserPath(targetSubpath);
    std::filesystem::path destination = targetParent / source.filename();
    for (int i = 2; std::filesystem::exists(destination, ec); ++i)
        destination = targetParent / (source.filename().string() + "_" + std::to_string(i));
    std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive, ec);
    if (ec)
    {
        m_assetStatus = "Folder import failed: " + ec.message();
        return;
    }
    RefreshAssetLibrary();  // registers every asset inside the copied folder
    SelectAssetBrowserFolder(AssetBrowserSubpath(destination));
    m_assetStatus = "Imported folder: " + destination.filename().string();
    Tracenf("[EDITOR-ASSET-BROWSER] folder_imported src=%s dst=%s",
        source.generic_string().c_str(),
        destination.generic_string().c_str());
}

std::string EditorImGui::CreateTargetSubpath()
{
    const std::string target = m_assetCreateTarget.value_or(m_assetSubpath);
    m_assetCreateTarget.reset();
    return AssetLibrary::NormalizeSubpath(target);
}

void EditorImGui::RevealCreatedAsset(const AssetLibrary::Entry& entry)
{
    SelectAssetBrowserFolder(entry.subpath);
    m_selectedAssetId = entry.id;
}

void EditorImGui::CreatePbrMaterialAsset()
{
    if (!m_assetLibrary)
        return;
    m_createMaterialTargetSubpath = CreateTargetSubpath();
    CopyToBuffer(m_createMaterialName, sizeof(m_createMaterialName), "material");
    m_createMaterialShadingMode = -1;
    m_assetOpenCreateMaterialPopup = true;
}

void EditorImGui::CreateLuaScriptAsset()
{
    if (!m_assetLibrary)
        return;
    AssetLibrary::ImportOptions options;
    options.displayName = "Script";  // CreateLuaScript uniquifies (Script_2, ...); rename via F2 after
    options.subpath = CreateTargetSubpath();
    options.tags = {"script", "lua"};
    AssetLibrary::Entry entry;
    std::string error;
    if (m_assetLibrary->CreateLuaScript(options, entry, error))
    {
        RevealCreatedAsset(entry);
        m_assetInspectorSelectionActive = true;
        m_assetStatus = "Created Lua script: " + entry.displayName + " (drag it onto an entity to attach)";
    }
    else
    {
        m_assetStatus = "Create Lua script failed: " + error;
    }
}

// A native C++ script source (.cpp/.h/...) under the asset folder. Only the path BELOW that folder is
// checked for a CMake "build" tree: the project itself may well live under a "build" directory.
static bool IsNativeScriptSource(const std::filesystem::path& scriptsDir, const std::filesystem::path& file)
{
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".cxx" && ext != ".cc")
        return false;
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(file, scriptsDir, ec);
    if (ec || relative.empty())
        return false;
    for (const std::filesystem::path& part : relative.parent_path())
    {
        if (part == "build" || part == "..")
            return false;
    }
    return true;
}

std::filesystem::path EditorImGui::ProjectScriptSourceDir() const
{
    // Native .cpp game scripts are assets like any other: they may sit anywhere in the asset folder.
    return ProjectManager::Instance().AssetRootPath();
}

void EditorImGui::CreateNativeScriptAsset()
{
    CreateNativeScriptFile("MyScript");  // default name; CreateNativeScript uniquifies + rename via F2
}

void EditorImGui::CreateNativeScriptFile(const std::string& className)
{
    if (!m_assetLibrary)
    {
        m_projectStatus = "Open a project to create a C++ script";
        return;
    }
    // Create the .cpp as a first-class Script asset (same path as a Lua script): written into the
    // target folder, registered, browsable + drag-attachable. The class is named after the file.
    AssetLibrary::ImportOptions options;
    options.displayName = className.empty() ? "MyScript" : className;
    options.subpath = CreateTargetSubpath();
    options.tags = {"script", "cpp"};
    AssetLibrary::Entry entry;
    std::string error;
    if (!m_assetLibrary->CreateNativeScript(options, entry, error))
    {
        m_assetStatus = "Create C++ script failed: " + error;
        return;
    }
    RevealCreatedAsset(entry);
    m_assetInspectorSelectionActive = true;
    // Stamp the new file so the save-to-live poll doesn't see it as a spurious "new .cpp" change.
    std::error_code ec;
    const std::filesystem::path dest = m_assetLibrary->AbsolutePath(entry);
    if (const auto mt = std::filesystem::last_write_time(dest, ec); !ec)
        m_cppMtimes[dest.generic_string()] = mt;
    m_assetStatus = "Created C++ script: " + entry.filename + " (drag it onto an entity, then Build)";
}

void EditorImGui::CreateAnimatorControllerAsset()
{
    if (!m_assetLibrary)
        return;
    AssetLibrary::ImportOptions options;
    options.displayName = "Animator_Controller";
    options.subpath = CreateTargetSubpath();
    AssetLibrary::Entry entry;
    std::string error;
    if (!m_assetLibrary->CreateAnimatorController(options, entry, error))
    {
        m_assetStatus = "Create animator controller failed: " + error;
        return;
    }
    RevealCreatedAsset(entry);
    m_assetInspectorSelectionActive = true;
    m_assetStatus = "Animator controller created: " + entry.displayName;
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
    options.subpath = CreateTargetSubpath();
    options.tags = {"water", "material"};
    AssetLibrary::Entry entry{};
    std::string error;
    if (!m_assetLibrary->CreateWaterMaterial(options, {}, entry, error))
    {
        m_assetStatus = "Water material create failed: " + error;
        return false;
    }
    RevealCreatedAsset(entry);
    SyncWaterMaterialSnapshot();
    m_assetStatus = "Water material created: " + entry.displayName;
    Tracenf("[EDITOR-IMGUI-4] Material saved: id=%s name=%s",
        entry.id.c_str(),
        entry.displayName.c_str());
    outEntry = entry;
    return true;
}

void EditorImGui::CreatePhysicsMaterialAsset()
{
    if (!m_assetLibrary)
    {
        m_assetStatus = "Physics material create failed: asset library unavailable";
        return;
    }

    AssetLibrary::ImportOptions options{};
    options.displayName = "Physics_Material";
    options.subpath = CreateTargetSubpath();
    options.tags = {"physics", "material"};
    AssetLibrary::PhysicsMaterialData material{};
    AssetLibrary::Entry entry{};
    std::string error;
    if (!m_assetLibrary->CreatePhysicsMaterial(options, material, entry, error))
    {
        m_assetStatus = "Physics material create failed: " + error;
        return;
    }

    RevealCreatedAsset(entry);
    m_assetInspectorSelectionActive = true;
    m_assetStatus = "Physics material created: " + entry.displayName;
    Tracenf("[PHYSICS-MAT] asset created path=%s", m_assetLibrary->AbsolutePath(entry).generic_string().c_str());
}

std::optional<AssetLibrary::PhysicsMaterialData> EditorImGui::FindPhysicsMaterial(const std::string& id) const
{
    if (!m_assetLibrary || id.empty())
        return std::nullopt;
    const auto entry = m_assetLibrary->FindById(id);
    if (!entry || entry->category != AssetLibrary::Category::PhysicsMaterial)
        return std::nullopt;
    return entry->physicsMaterial;
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
    {
        m_selectedAssetId.clear();
        m_assetInspectorSelectionActive = false;
    }
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

void EditorImGui::BeginFrame(bool editorModeActive)
{
    const bool backendReady = m_textureProvider && m_textureProvider->IsReady();
    if (!m_initialized || !backendReady || m_frameActive)
    {
        static uint32_t beginSkippedLogs = 0;
        if (!QuietLogsForLodDiag() && beginSkippedLogs < 3)
        {
            ++beginSkippedLogs;
            TraceDiagf("[FRAME] imgui_begin called = no, initialized=%d backend_ready=%d frame_active=%d editor_mode=%d",
                m_initialized ? 1 : 0,
                backendReady ? 1 : 0,
                m_frameActive ? 1 : 0,
                editorModeActive ? 1 : 0);
        }
        return;
    }

    m_editorModeActive = editorModeActive;
    if (m_textureProvider)
        m_textureProvider->BeginBackendFrame();
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
#include "editor_panels/EditorImGuiAnimatorPanels.inl"
#include "editor_panels/EditorImGuiProjectPanels.inl"
#include "editor_panels/EditorImGuiMenuToolbarPanels.inl"
#include "editor_panels/EditorImGuiHierarchyPanels.inl"
#include "editor_panels/EditorImGuiInspectorPanels.inl"
#include "editor_panels/EditorImGuiAssetBrowserPanels.inl"
#include "editor_panels/EditorImGuiMaterialPanels.inl"
#include "editor_panels/EditorImGuiPanelDispatcher.inl"
void EditorImGui::RenderScriptPrompts()
{
    if (m_scriptPrompts.empty())
        return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 center = viewport->GetCenter();
    float stagger = 0.0f;
    for (auto it = m_scriptPrompts.begin(); it != m_scriptPrompts.end();)
    {
        ScriptPromptBuffer& prompt = *it;
        ImGui::SetNextWindowPos(ImVec2(center.x + stagger, center.y + stagger), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        stagger += 24.0f;
        const std::string windowName = (prompt.view.title.empty() ? std::string("Script") : prompt.view.title) +
            "###ScriptPrompt" + std::to_string(prompt.view.id);
        bool open = true;
        int outcome = 0;  // 1 submit, -1 cancel
        if (ImGui::Begin(windowName.c_str(), &open,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking))
        {
            if (!prompt.view.label.empty())
                ImGui::TextUnformatted(prompt.view.label.c_str());
            if (prompt.focusPending)
            {
                ImGui::SetKeyboardFocusHere();
                prompt.focusPending = false;
            }
            ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
            if (prompt.view.secret)
                flags |= ImGuiInputTextFlags_Password | ImGuiInputTextFlags_NoUndoRedo;
            ImGui::SetNextItemWidth(280.0f);
            if (ImGui::InputText("##value", prompt.text.data(), prompt.text.size(), flags))
                outcome = 1;
            if (ImGui::Button("OK"))
                outcome = 1;
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                outcome = -1;
        }
        ImGui::End();
        if (!open)
            outcome = -1;
        if (outcome == 0)
        {
            ++it;
            continue;
        }
        ScriptPromptAnswer answer;
        answer.id = prompt.view.id;
        answer.submitted = outcome == 1;
        if (answer.submitted)
            answer.text = prompt.text.data();
        std::fill(prompt.text.begin(), prompt.text.end(), '\0');
        if (prompt.view.secret)
            ImGui::ClearActiveID();  // ImGui's own edit state must not keep the secret around
        m_scriptPromptAnswers.push_back(std::move(answer));
        it = m_scriptPrompts.erase(it);
    }
}

void EditorImGui::RenderPanels()
{
    const bool backendReady = m_textureProvider && m_textureProvider->IsReady();
    const bool backendFrameOpen = m_textureProvider && m_textureProvider->IsBackendFrameActive();
    if (!m_initialized || !backendReady || !m_frameActive || !backendFrameOpen)
    {
        m_frameActive = false;
        static uint32_t renderSkippedLogs = 0;
        if (!QuietLogsForLodDiag() && renderSkippedLogs < 3)
        {
            ++renderSkippedLogs;
            TraceDiagf("[FRAME] imgui_render called = no, initialized=%d backend_ready=%d frame_active=%d editor_mode=%d",
                m_initialized ? 1 : 0,
                backendReady ? 1 : 0,
                m_frameActive ? 1 : 0,
                m_editorModeActive ? 1 : 0);
        }
        return;
    }

    RenderEditorPanels();
    RenderDemoPanels();
    RenderScriptPrompts();
    ImGui::Render();

    m_frameActive = false;
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
    // Generic teardown only: preview UI registrations release through the
    // provider (backend stays alive for pool/descriptor teardown, which the
    // frame owner performs through the backend adapter afterwards).
    DestroyAssetPreviewTextures();
    m_textureProvider = nullptr;
    m_initialized = false;
    m_frameActive = false;
}
#else
EditorImGui::~EditorImGui() = default;

bool EditorImGui::Create()
{
    return true;
}

void EditorImGui::SetTextureProvider(ixeditor::graphics::IEditorTextureProvider*)
{
}

void EditorImGui::BeginFrame(bool)
{
}

void EditorImGui::RenderPanels()
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

void EditorImGui::SetCameraEditorState(const CameraEditorState&)
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

// Real (non-stub) project asset-library management for the runtime: it owns m_assetLibrary, which the
// shared asset-path resolvers (AudioClipFilePath/ScriptSourceFilePath) query. No editor UI involved.
void EditorImGui::InitializeProjectAssetLibrary(const std::filesystem::path& projectRoot,
                                                const std::filesystem::path& assetRoot)
{
    m_assetLibrary = std::make_unique<AssetLibrary>(projectRoot, assetRoot);
    // The shipped game only reads the manifest the editor wrote: no folder scan, nothing written.
    if (!m_assetLibrary->InitializeReadOnly())
        m_assetLibrary.reset();
}

void EditorImGui::RefreshAssetLibrary()
{
    if (!m_assetLibrary)
        return;
    // The game only re-reads the manifest the editor wrote: it never scans or writes its own folder.
    m_assetLibrary->InitializeReadOnly();
}

// Real (non-stub) native game-module loader for the runtime: scans <ProjectRoot>/Binaries for module
// DLLs and registers their native script classes (so a shipped game's C++ scripts run). Imgui-free; the
// runtime loads once at boot (no project-switch reload, so no UnloadGameModules dance).
void EditorImGui::LoadProjectGameModules(const std::filesystem::path& projectRoot)
{
    const std::filesystem::path modulesDir = projectRoot / "Binaries";
    std::error_code ec;
    if (!std::filesystem::is_directory(modulesDir, ec))
        return;
#if defined(_WIN32)
    const std::string moduleExt = ".dll";
#else
    const std::string moduleExt = ".so";
#endif
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(modulesDir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != moduleExt)
            continue;
        std::string loadError;
        platform::DynamicLibraryHandle handle = platform::OpenLibrary(entry.path(), &loadError);
        if (!handle)
        {
            TraceError("[SCRIPT] game module failed to load: %s (%s)",
                entry.path().filename().string().c_str(), loadError.c_str());
            continue;
        }
        auto entryFn = reinterpret_cast<ixscript::IxModuleEntryFn>(
            platform::GetLibrarySymbol(handle, IXTREEME_MODULE_ENTRY_SYMBOL));
        if (!entryFn)
        {
            platform::CloseLibrary(handle);
            continue;
        }
        const ixscript::ModuleLoadResult result = ixscript::InvokeGameModule(entryFn);
        if (!result.versionOk)
        {
            platform::CloseLibrary(handle);
            continue;
        }
        m_loadedGameModules.push_back(handle);
        Tracenf("[SCRIPT] loaded game module %s: %d native class(es) registered",
            entry.path().filename().string().c_str(), result.registeredCount);
    }
}

bool EditorImGui::OpenWaterMaterialEditor(const std::string&)
{
    return false;
}

bool EditorImGui::OpenPbrMaterialEditor(const std::string&)
{
    return false;
}

std::optional<AssetLibrary::PhysicsMaterialData> EditorImGui::FindPhysicsMaterial(const std::string&) const
{
    return std::nullopt;
}

MapEditorCommands EditorImGui::ConsumeCommands()
{
    return {};
}

bool EditorImGui::IsTextInputActive() const
{
    return false;
}

std::optional<LodConfig> EditorImGui::FindModelLodDefault(const std::string&) const
{
    return std::nullopt;
}

// Asset-path resolvers are NOT editor UI — they're plain asset-library lookups (imgui-free), so they
// get REAL implementations in the runtime build too. They resolve once the runtime initializes the
// project asset library (InitializeProjectAssetLibrary), letting the shared sim load audio + Lua.
std::string EditorImGui::AudioClipFilePath(const std::string& clipId) const
{
    if (!m_assetLibrary || clipId.empty())
        return {};
    const auto entry = m_assetLibrary->FindById(clipId);
    if (!entry || entry->category != AssetLibrary::Category::Audio)
        return {};
    return m_assetLibrary->AbsolutePath(*entry).generic_string();
}

std::string EditorImGui::ScriptSourceFilePath(const std::string& scriptId) const
{
    if (!m_assetLibrary || scriptId.empty())
        return {};
    const auto entry = m_assetLibrary->FindById(scriptId);
    if (!entry || entry->category != AssetLibrary::Category::Script)
        return {};
    return m_assetLibrary->AbsolutePath(*entry).generic_string();
}

void EditorImGui::Destroy()
{
}
#endif

// Script text prompts: the data handoff is build-agnostic; only the drawing needs the editor UI.
void EditorImGui::SetScriptPrompts(std::vector<ScriptPromptView> prompts)
{
    // Drop (and wipe) buffers whose prompt is no longer open.
    for (auto it = m_scriptPrompts.begin(); it != m_scriptPrompts.end();)
    {
        const bool listed = std::any_of(prompts.begin(), prompts.end(),
            [&](const ScriptPromptView& view) { return view.id == it->view.id; });
        if (listed)
        {
            ++it;
            continue;
        }
        std::fill(it->text.begin(), it->text.end(), '\0');
        it = m_scriptPrompts.erase(it);
    }
    for (ScriptPromptView& view : prompts)
    {
        const bool known = std::any_of(m_scriptPrompts.begin(), m_scriptPrompts.end(),
            [&](const ScriptPromptBuffer& buffer) { return buffer.view.id == view.id; });
        if (known)
            continue;
        ScriptPromptBuffer buffer;
        buffer.view = std::move(view);
        m_scriptPrompts.push_back(std::move(buffer));
    }
}

std::vector<EditorImGui::ScriptPromptAnswer> EditorImGui::TakeScriptPromptAnswers()
{
    std::vector<ScriptPromptAnswer> answers;
    answers.swap(m_scriptPromptAnswers);
    return answers;
}
