#include "NoesisLayer.h"
#include "AssetLibrary.h"
#include "VulkanDevice.h"
#include "WorldComponents.h"
#include "Debug.h"
#include "asset/IAssetReader.h"
#include "network/ClientSession.h"
#if defined(__ANDROID__)
#include "SoftKeyboard.h"
#include <jni.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#endif

#include <NsApp/DelegateCommand.h>
#include <NsApp/NotifyPropertyChangedBase.h>
#include <NsCore/DynamicCast.h>
#include <NsCore/Ptr.h>
#include <NsCore/ReflectionImplement.h>
#include <NsCore/String.h>
#include <NsDrawing/Color.h>
#include <NsDrawing/Point.h>
#include <NsDrawing/Thickness.h>
#include <NsGui/CachedFontProvider.h>
#include <NsGui/Brush.h>
#include <NsGui/Button.h>
#include <NsGui/CheckBox.h>
#include <NsGui/ContentControl.h>
#include <NsGui/Control.h>
#include <NsGui/Enums.h>
#include <NsGui/FrameworkElement.h>
#include <NsGui/BitmapImage.h>
#include <NsGui/Image.h>
#include <NsGui/IntegrationAPI.h>
#include <NsGui/InputEnums.h>
#include <NsGui/IRenderer.h>
#include <NsGui/IView.h>
#include <NsGui/ItemCollection.h>
#include <NsGui/Keyboard.h>
#include <NsGui/MemoryStream.h>
#include <NsGui/ObservableCollection.h>
#include <NsGui/PasswordBox.h>
#include <NsGui/RadioButton.h>
#include <NsGui/RoutedEvent.h>
#include <NsGui/ScrollViewer.h>
#include <NsGui/Slider.h>
#include <NsGui/SolidColorBrush.h>
#include <NsGui/StackPanel.h>
#include <NsGui/Stream.h>
#include <NsGui/Style.h>
#include <NsGui/TextBlock.h>
#include <NsGui/TextBox.h>
#include <NsGui/TextureProvider.h>
#include <NsGui/TreeView.h>
#include <NsGui/TreeViewItem.h>
#include <NsGui/UIElement.h>
#include <NsGui/UIElementCollection.h>
#include <NsGui/UIElementEvents.h>
#include <NsGui/Uri.h>
#include <NsGui/XamlProvider.h>
#include <NsCore/Delegate.h>
#include <NsCore/Nullable.h>
#include <NsRender/RenderDevice.h>
#include <NsRender/Texture.h>
#include <NsRender/VKFactory.h>

#include <stb_image.h>

#include <flecs.h>

#include <array>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
extern android_app* g_androidApp;

enum class TextInputEventType
{
    Text,
    Backspace,
    KeyDown,
    KeyUp
};

struct TextInputEvent
{
    TextInputEventType type = TextInputEventType::Text;
    std::string text;
    int keyCode = 0;
};

std::mutex g_textInputMutex;
std::vector<TextInputEvent> g_pendingTextInput;
Noesis::TextBox* g_focusedTextBox = nullptr;
Noesis::PasswordBox* g_focusedPasswordBox = nullptr;
#endif

namespace
{
constexpr uint32_t kPlayerSlots = 4;
constexpr double kServerTickSeconds = 0.05;
constexpr double kInterpolationDelaySeconds = 0.10;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr const char* kRootAssetFolderPath = "\x1Froot";
constexpr double kFolderDoubleClickSeconds = 0.42;

bool IsAllAssetFolderPath(const std::string& path)
{
    return path.empty();
}

bool IsRootAssetFolderPath(const std::string& path)
{
    return path == kRootAssetFolderPath;
}

std::string AssetFolderQuerySubpath(const std::string& path)
{
    return IsRootAssetFolderPath(path) ? std::string{} : path;
}

std::string ParentAssetFolderPath(const std::string& path)
{
    if (IsAllAssetFolderPath(path))
        return {};
    if (IsRootAssetFolderPath(path))
        return {};
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

std::vector<std::string> SplitAssetFolderPath(const std::string& path)
{
    std::vector<std::string> segments;
    std::string segment;
    for (char c : path)
    {
        if (c == '/')
        {
            if (!segment.empty())
                segments.push_back(segment);
            segment.clear();
            continue;
        }
        segment += c;
    }
    if (!segment.empty())
        segments.push_back(segment);
    return segments;
}

enum class DragPayloadType
{
    None,
    AssetTexture,
    AssetMaterial,
    AssetModel,
    AssetAnimation,
    OsFiles
};

struct DragPayload
{
    DragPayloadType type = DragPayloadType::None;
    std::string assetId;
    AssetLibrary::TextureRole textureRole = AssetLibrary::TextureRole::Unknown;
};

enum class RenameTargetType
{
    None,
    Asset,
    Folder
};

bool AssetFolderContains(const std::string& value, const std::string& parent)
{
    return value == parent || value.rfind(parent + "/", 0) == 0;
}

std::string ReplaceAssetFolderPrefix(const std::string& value,
                                     const std::string& oldPrefix,
                                     const std::string& newPrefix)
{
    if (value == oldPrefix)
        return newPrefix;
    if (value.rfind(oldPrefix + "/", 0) == 0)
        return newPrefix + value.substr(oldPrefix.size());
    return value;
}

std::filesystem::path FindClientRoot()
{
    std::filesystem::path probe = std::filesystem::current_path();
    for (std::filesystem::path sourceProbe = probe;;)
    {
        if (std::filesystem::exists(sourceProbe / "CMakeLists.txt") &&
            std::filesystem::exists(sourceProbe / "assets" / "xaml" / "EditorPanel.xaml"))
        {
            return sourceProbe;
        }
        if (std::filesystem::exists(sourceProbe / "Client" / "CMakeLists.txt") &&
            std::filesystem::exists(sourceProbe / "Client" / "assets" / "xaml" / "EditorPanel.xaml"))
        {
            return sourceProbe / "Client";
        }
        if (!sourceProbe.has_parent_path() || sourceProbe.parent_path() == sourceProbe)
            break;
        sourceProbe = sourceProbe.parent_path();
    }

    for (;;)
    {
        if (std::filesystem::exists(probe / "assets" / "xaml" / "EditorPanel.xaml"))
            return probe;
        if (std::filesystem::exists(probe / "Client" / "assets" / "xaml" / "EditorPanel.xaml"))
            return probe / "Client";
        if (!probe.has_parent_path() || probe.parent_path() == probe)
            break;
        probe = probe.parent_path();
    }
    return std::filesystem::current_path();
}

std::string CompactAssetName(const std::string& value, size_t limit = 22)
{
    if (value.size() <= limit)
        return value;
    if (limit <= 3)
        return value.substr(0, limit);
    return value.substr(0, limit - 3) + "...";
}

#if defined(_WIN32)
std::optional<std::filesystem::path> PickAssetFile(AssetLibrary::Category category)
{
    const wchar_t* filter = L"All files (*.*)\0*.*\0\0";
    switch (category)
    {
    case AssetLibrary::Category::Texture:
        filter = L"Textures (*.png;*.jpg;*.jpeg;*.dds;*.tga)\0*.png;*.jpg;*.jpeg;*.dds;*.tga\0All files (*.*)\0*.*\0\0";
        break;
    case AssetLibrary::Category::Model:
        filter = L"Models (*.gltf;*.glb)\0*.gltf;*.glb\0All files (*.*)\0*.*\0\0";
        break;
    case AssetLibrary::Category::Animation:
        filter = L"Animations (*.gltf;*.glb;*.ozz)\0*.gltf;*.glb;*.ozz\0All files (*.*)\0*.*\0\0";
        break;
    case AssetLibrary::Category::Material:
        filter = L"Materials (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
        break;
    }

    std::array<wchar_t, 32768> file{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return std::nullopt;
    return std::filesystem::path(file.data());
}
#else
std::optional<std::filesystem::path> PickAssetFile(AssetLibrary::Category)
{
    return std::nullopt;
}
#endif

#if defined(__ANDROID__)
constexpr int kAndroidKeycodeDel = 67;

void QueueTextInputEvent(TextInputEvent event)
{
    std::lock_guard<std::mutex> lock(g_textInputMutex);
    g_pendingTextInput.push_back(std::move(event));
}

void SetFocusedTextInput(Noesis::TextBox* textBox, Noesis::PasswordBox* passwordBox)
{
    g_focusedTextBox = textBox;
    g_focusedPasswordBox = passwordBox;
}

void RemoveLastUtf8Codepoint(std::string& text)
{
    if (text.empty())
        return;

    size_t offset = text.size() - 1;
    while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80)
        --offset;

    text.erase(offset);
}

void AppendTextToFocusedInput(const std::string& text)
{
    if (text.empty())
        return;

    if (g_focusedTextBox)
    {
        const char* current = g_focusedTextBox->GetText();
        std::string updated = current ? current : "";
        updated += text;
        g_focusedTextBox->SetText(updated.c_str());
        g_focusedTextBox->SetCaretIndex(static_cast<uint32_t>(updated.size()));
        Tracenf("[KEYBOARD] Text input: '%s' -> TextBox now: '%s'", text.c_str(), updated.c_str());
        return;
    }

    if (g_focusedPasswordBox)
    {
        const char* current = g_focusedPasswordBox->GetPassword();
        std::string updated = current ? current : "";
        updated += text;
        g_focusedPasswordBox->SetPassword(updated.c_str());
        Tracenf("[KEYBOARD] Text input: %zu byte(s) -> PasswordBox length: %zu",
            text.size(), updated.size());
    }
}

void DeleteBackwardFromFocusedInput()
{
    if (g_focusedTextBox)
    {
        const char* current = g_focusedTextBox->GetText();
        std::string updated = current ? current : "";
        RemoveLastUtf8Codepoint(updated);
        g_focusedTextBox->SetText(updated.c_str());
        g_focusedTextBox->SetCaretIndex(static_cast<uint32_t>(updated.size()));
        Tracenf("[KEYBOARD] Backspace -> TextBox now: '%s'", updated.c_str());
        return;
    }

    if (g_focusedPasswordBox)
    {
        const char* current = g_focusedPasswordBox->GetPassword();
        std::string updated = current ? current : "";
        RemoveLastUtf8Codepoint(updated);
        g_focusedPasswordBox->SetPassword(updated.c_str());
        Tracenf("[KEYBOARD] Backspace -> PasswordBox length: %zu", updated.size());
    }
}

void DrainPendingTextInput()
{
    std::vector<TextInputEvent> events;
    {
        std::lock_guard<std::mutex> lock(g_textInputMutex);
        events.swap(g_pendingTextInput);
    }

    for (const TextInputEvent& event : events)
    {
        switch (event.type)
        {
        case TextInputEventType::Text:
            AppendTextToFocusedInput(event.text);
            break;
        case TextInputEventType::Backspace:
            DeleteBackwardFromFocusedInput();
            break;
        case TextInputEventType::KeyDown:
            Tracenf("[KEYBOARD] KeyDown: %d", event.keyCode);
            if (event.keyCode == kAndroidKeycodeDel)
                DeleteBackwardFromFocusedInput();
            break;
        case TextInputEventType::KeyUp:
            break;
        }
    }
}

void OnTextBoxGotFocus(Noesis::BaseComponent* sender, const Noesis::KeyboardFocusChangedEventArgs&)
{
    SetFocusedTextInput(static_cast<Noesis::TextBox*>(sender), nullptr);
    ShowSoftKeyboard(g_androidApp);
}

void OnPasswordBoxGotFocus(Noesis::BaseComponent* sender,
    const Noesis::KeyboardFocusChangedEventArgs&)
{
    SetFocusedTextInput(nullptr, static_cast<Noesis::PasswordBox*>(sender));
    ShowSoftKeyboard(g_androidApp);
}

void OnTextInputLostFocus(Noesis::BaseComponent* sender,
    const Noesis::KeyboardFocusChangedEventArgs&)
{
    if (sender == g_focusedTextBox || sender == g_focusedPasswordBox)
        SetFocusedTextInput(nullptr, nullptr);
    HideSoftKeyboard(g_androidApp);
}
#endif

void Log(const char* text)
{
    Tracen(text);
}

void LogFormat(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Log(buffer);
}

float DequantizeHeading(std::uint16_t value)
{
    return (static_cast<float>(value) / 65535.0f) * kTwoPi;
}

std::uint16_t QuantizeHeading(float angle)
{
    while (angle < 0.0f)
        angle += kTwoPi;
    while (angle >= kTwoPi)
        angle -= kTwoPi;
    return static_cast<std::uint16_t>(std::lround((angle / kTwoPi) * 65535.0f));
}

float NormalizeAngleDelta(float delta)
{
    constexpr float kPi = 3.14159265358979323846f;
    while (delta > kPi)
        delta -= kTwoPi;
    while (delta < -kPi)
        delta += kTwoPi;
    return delta;
}

float Lerp(float a, float b, double t)
{
    return static_cast<float>(static_cast<double>(a) + (static_cast<double>(b) - a) * t);
}

client::ecs::Position LerpPosition(const client::ecs::Position& a,
                                   const client::ecs::Position& b,
                                   double t)
{
    return {Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t)};
}

client::ecs::Heading LerpHeadingShortest(client::ecs::Heading a,
                                         client::ecs::Heading b,
                                         double t)
{
    const float start = DequantizeHeading(a.angle);
    const float delta = NormalizeAngleDelta(DequantizeHeading(b.angle) - start);
    return {QuantizeHeading(start + delta * static_cast<float>(t))};
}

std::string AssetPath(std::string prefix, std::string path)
{
    for (char& c : path)
    {
        if (c == '\\')
            c = '/';
    }

    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());

    if (path.rfind("assets/", 0) == 0)
        return path;
    if (!prefix.empty() && prefix.back() != '/')
        prefix.push_back('/');
    return prefix + path;
}

Noesis::MouseButton ToNoesisMouseButton(MouseButton button)
{
    switch (button)
    {
    case MouseButton_Left: return Noesis::MouseButton_Left;
    case MouseButton_Right: return Noesis::MouseButton_Right;
    case MouseButton_Middle: return Noesis::MouseButton_Middle;
    default: return Noesis::MouseButton_Left;
    }
}

Noesis::Key ToNoesisKey(Key key)
{
    switch (key)
    {
    case Key_A: return Noesis::Key_A;
    case Key_B: return Noesis::Key_B;
    case Key_C: return Noesis::Key_C;
    case Key_D: return Noesis::Key_D;
    case Key_E: return Noesis::Key_E;
    case Key_F: return Noesis::Key_F;
    case Key_G: return Noesis::Key_G;
    case Key_H: return Noesis::Key_H;
    case Key_I: return Noesis::Key_I;
    case Key_J: return Noesis::Key_J;
    case Key_K: return Noesis::Key_K;
    case Key_L: return Noesis::Key_L;
    case Key_M: return Noesis::Key_M;
    case Key_N: return Noesis::Key_N;
    case Key_O: return Noesis::Key_O;
    case Key_P: return Noesis::Key_P;
    case Key_Q: return Noesis::Key_Q;
    case Key_R: return Noesis::Key_R;
    case Key_S: return Noesis::Key_S;
    case Key_T: return Noesis::Key_T;
    case Key_U: return Noesis::Key_U;
    case Key_V: return Noesis::Key_V;
    case Key_W: return Noesis::Key_W;
    case Key_X: return Noesis::Key_X;
    case Key_Y: return Noesis::Key_Y;
    case Key_Z: return Noesis::Key_Z;
    case Key_0: return Noesis::Key_D0;
    case Key_1: return Noesis::Key_D1;
    case Key_2: return Noesis::Key_D2;
    case Key_3: return Noesis::Key_D3;
    case Key_4: return Noesis::Key_D4;
    case Key_5: return Noesis::Key_D5;
    case Key_6: return Noesis::Key_D6;
    case Key_7: return Noesis::Key_D7;
    case Key_8: return Noesis::Key_D8;
    case Key_9: return Noesis::Key_D9;
    case Key_Enter: return Noesis::Key_Return;
    case Key_Escape: return Noesis::Key_Escape;
    case Key_Backspace: return Noesis::Key_Back;
    case Key_Tab: return Noesis::Key_Tab;
    case Key_Space: return Noesis::Key_Space;
    case Key_Left: return Noesis::Key_Left;
    case Key_Right: return Noesis::Key_Right;
    case Key_Up: return Noesis::Key_Up;
    case Key_Down: return Noesis::Key_Down;
    case Key_Shift: return Noesis::Key_LeftShift;
    case Key_Control: return Noesis::Key_LeftCtrl;
    case Key_Delete: return Noesis::Key_Delete;
    case Key_Home: return Noesis::Key_Home;
    case Key_End: return Noesis::Key_End;
    default: return Noesis::Key_None;
    }
}

class AssetMemoryStream final : public Noesis::MemoryStream
{
public:
    explicit AssetMemoryStream(std::vector<std::uint8_t> bytes)
        : AssetMemoryStream(std::make_shared<std::vector<std::uint8_t>>(std::move(bytes)))
    {
    }

private:
    explicit AssetMemoryStream(std::shared_ptr<std::vector<std::uint8_t>> bytes)
        : Noesis::MemoryStream(bytes->data(), static_cast<uint32_t>(bytes->size()))
        , m_bytes(std::move(bytes))
    {
    }

    std::shared_ptr<std::vector<std::uint8_t>> m_bytes;
};

class AssetXamlProvider final : public Noesis::XamlProvider
{
public:
    explicit AssetXamlProvider(client::asset::IAssetReader& assets) : m_assets(assets) {}

private:
    Noesis::Ptr<Noesis::Stream> LoadXaml(const Noesis::Uri& uri) override
    {
        Noesis::FixedString<512> path;
        uri.GetPath(path);

        auto bytes = m_assets.ReadAll(AssetPath("assets/xaml", path.Str()));
        if (!bytes)
            return nullptr;
        return *new AssetMemoryStream(std::move(*bytes));
    }

    client::asset::IAssetReader& m_assets;
};

class AssetTextureProvider final : public Noesis::TextureProvider
{
public:
    explicit AssetTextureProvider(client::asset::IAssetReader& assets) : m_assets(assets) {}

private:
    std::optional<std::string> ResolvePath(const Noesis::Uri& uri) const
    {
        Noesis::FixedString<512> path;
        uri.GetPath(path);
        std::string normalized = path.Str();
        if (normalized.empty())
            return std::nullopt;
        return AssetPath("", normalized);
    }

    std::optional<std::vector<std::uint8_t>> ReadTextureBytes(const std::string& path) const
    {
        if (auto bytes = m_assets.ReadAll(path))
        {
            Tracenf("[NOESIS-THUMB] read via IAssetReader path=%s size=%zu",
                path.c_str(),
                bytes->size());
            return bytes;
        }

        const std::filesystem::path direct = FindClientRoot() / std::filesystem::path(path);
        std::ifstream file(direct, std::ios::binary | std::ios::ate);
        if (!file)
        {
            Tracenf("[NOESIS-THUMB] missing path=%s fallback=%s",
                path.c_str(),
                direct.string().c_str());
            return std::nullopt;
        }

        const auto end = file.tellg();
        if (end < 0)
            return std::nullopt;
        std::vector<std::uint8_t> bytes(static_cast<size_t>(end));
        file.seekg(0);
        if (!bytes.empty())
        {
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!file)
                return std::nullopt;
        }
        Tracenf("[NOESIS-THUMB] read via filesystem path=%s size=%zu",
            direct.string().c_str(),
            bytes.size());
        return bytes;
    }

    Noesis::TextureInfo GetTextureInfo(const Noesis::Uri& uri) override
    {
        Noesis::TextureInfo info{};
        const auto path = ResolvePath(uri);
        if (!path)
            return info;

        auto bytes = ReadTextureBytes(*path);
        if (!bytes)
            return info;

        int width = 0;
        int height = 0;
        int channels = 0;
        if (!stbi_info_from_memory(bytes->data(), static_cast<int>(bytes->size()), &width, &height, &channels))
            return info;
        if (width <= 0 || height <= 0)
            return info;

        info.width = static_cast<uint32_t>(width);
        info.height = static_cast<uint32_t>(height);
        return info;
    }

    Noesis::Ptr<Noesis::Texture> LoadTexture(const Noesis::Uri& uri, Noesis::RenderDevice* device) override
    {
        const auto path = ResolvePath(uri);
        if (!path || !device)
            return nullptr;

        auto bytes = ReadTextureBytes(*path);
        if (!bytes)
            return nullptr;

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* decoded = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &width, &height, &channels, 4);
        if (!decoded || width <= 0 || height <= 0)
        {
            if (decoded)
                stbi_image_free(decoded);
            return nullptr;
        }

        const void* levels[] = {decoded};
        Noesis::Ptr<Noesis::Texture> texture = device->CreateTexture(
            path->c_str(),
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height),
            1,
            Noesis::TextureFormat::RGBA8,
            levels);
        stbi_image_free(decoded);
        return texture;
    }

    client::asset::IAssetReader& m_assets;
};

class AssetFontProvider final : public Noesis::CachedFontProvider
{
public:
    explicit AssetFontProvider(client::asset::IAssetReader& assets) : m_assets(assets) {}

private:
    void ScanFolder(const Noesis::Uri& folder) override
    {
        RegisterFont(folder, "Roboto-Regular.ttf", 0, "Roboto", "Regular",
            Noesis::FontWeight_Normal, Noesis::FontStretch_Normal, Noesis::FontStyle_Normal);
    }

    Noesis::Ptr<Noesis::Stream> OpenFont(const Noesis::Uri& folder, const char* filename) const override
    {
        Noesis::FixedString<512> path;
        folder.GetPath(path);

        const std::string folderPath = path.Str()[0] != '\0' ? path.Str() : "fonts";
        auto bytes = m_assets.ReadAll(AssetPath("assets", folderPath + std::string("/") + filename));
        if (!bytes)
            return nullptr;
        return *new AssetMemoryStream(std::move(*bytes));
    }

    client::asset::IAssetReader& m_assets;
};

class LobbyCharacter final : public Noesis::BaseComponent
{
public:
    explicit LobbyCharacter(uint32_t slot)
        : m_slot(slot)
        , m_name("Empty slot")
        , m_detail("Character creation not available yet")
        , m_empty(true)
    {
    }

    explicit LobbyCharacter(const client::net::CharacterListItem& info)
        : m_id(info.id)
        , m_slot(info.slot)
        , m_name(info.name.c_str())
        , m_level(info.level)
        , m_classId(info.classId)
        , m_empty(info.id == 0)
    {
        char line[128];
        if (m_empty)
            std::snprintf(line, sizeof(line), "Character creation not available yet");
        else
            std::snprintf(line, sizeof(line), "Level %u  Class %u", m_level, m_classId);
        m_detail = line;
    }

    uint64_t GetId() const { return m_id; }
    uint32_t GetSlot() const { return m_slot; }
    const char* GetName() const { return m_name.Str(); }
    const char* GetDetail() const { return m_detail.Str(); }
    uint32_t GetLevel() const { return m_level; }
    uint32_t GetClassId() const { return m_classId; }
    bool GetCanEnter() const { return !m_empty; }

private:
    uint64_t m_id = 0;
    uint32_t m_slot = 0;
    Noesis::String m_name;
    Noesis::String m_detail;
    uint32_t m_level = 0;
    uint32_t m_classId = 0;
    bool m_empty = true;

    NS_IMPLEMENT_INLINE_REFLECTION(LobbyCharacter, Noesis::BaseComponent, "Lobby.Character")
    {
        NsProp("Id", &LobbyCharacter::GetId);
        NsProp("Slot", &LobbyCharacter::GetSlot);
        NsProp("Name", &LobbyCharacter::GetName);
        NsProp("Detail", &LobbyCharacter::GetDetail);
        NsProp("Level", &LobbyCharacter::GetLevel);
        NsProp("ClassId", &LobbyCharacter::GetClassId);
        NsProp("CanEnter", &LobbyCharacter::GetCanEnter);
    }
};

class LobbyViewModel final : public NoesisApp::NotifyPropertyChangedBase
{
public:
    explicit LobbyViewModel(std::function<void(uint64_t)> enterCallback)
        : m_enterCallback(std::move(enterCallback))
    {
        m_characters = *new Noesis::ObservableCollection<LobbyCharacter>();
        SetStatus("Fetching characters...");
        m_enterCommand.SetExecuteFunc(Noesis::MakeDelegate(this, &LobbyViewModel::Enter));
    }

    Noesis::ObservableCollection<LobbyCharacter>* GetCharacters() const { return m_characters; }
    LobbyCharacter* GetSelectedCharacter() const { return m_selectedCharacter; }
    void SetSelectedCharacter(LobbyCharacter* value)
    {
        if (m_selectedCharacter != value)
        {
            m_selectedCharacter = value;
            OnPropertyChanged("SelectedCharacter");
            OnPropertyChanged("CanEnter");

            if (m_selectedCharacter && !m_selectedCharacter->GetCanEnter())
                SetStatus("Character creation not available yet");
        }
    }
    bool GetCanEnter() const
    {
        return m_selectedCharacter && m_selectedCharacter->GetCanEnter();
    }
    const NoesisApp::DelegateCommand* GetEnterCommand() const { return &m_enterCommand; }
    const char* GetStatus() const { return m_status.Str(); }

    void SetStatus(const char* status)
    {
        m_status = status ? status : "";
        OnPropertyChanged("Status");
    }

    void OnCharacterList(const std::vector<client::net::CharacterListItem>& characters)
    {
        m_characters->Clear();
        m_selectedCharacter = nullptr;
        m_characterCount = static_cast<uint32_t>(characters.size());
        m_filledCharacterCount = 0;

        for (const auto& character : characters)
        {
            if (character.id != 0)
                ++m_filledCharacterCount;

            m_characters->Add(Noesis::MakePtr<LobbyCharacter>(character));
        }

        for (uint32_t slot = static_cast<uint32_t>(characters.size()); slot < kPlayerSlots; ++slot)
        {
            m_characters->Add(Noesis::MakePtr<LobbyCharacter>(slot));
        }

        SetStatus("Select a character");
        OnPropertyChanged("Characters");
        OnPropertyChanged("SelectedCharacter");
        OnPropertyChanged("CanEnter");
    }

private:
    void Enter(Noesis::BaseComponent*)
    {
        if (!m_selectedCharacter || !m_selectedCharacter->GetCanEnter())
        {
            SetStatus("Select a character");
            return;
        }

        SetStatus("Entering world...");
        if (m_enterCallback)
            m_enterCallback(m_selectedCharacter->GetId());
    }

private:
    Noesis::Ptr<Noesis::ObservableCollection<LobbyCharacter>> m_characters;
    std::function<void(uint64_t)> m_enterCallback;
    LobbyCharacter* m_selectedCharacter = nullptr;
    NoesisApp::DelegateCommand m_enterCommand;
    Noesis::String m_status;
    uint32_t m_characterCount = 0;
    uint32_t m_filledCharacterCount = 0;

    NS_IMPLEMENT_INLINE_REFLECTION(LobbyViewModel, NoesisApp::NotifyPropertyChangedBase, "Lobby.ViewModel")
    {
        NsProp("Characters", &LobbyViewModel::GetCharacters);
        NsProp("SelectedCharacter", &LobbyViewModel::GetSelectedCharacter, &LobbyViewModel::SetSelectedCharacter);
        NsProp("CanEnter", &LobbyViewModel::GetCanEnter);
        NsProp("EnterCommand", &LobbyViewModel::GetEnterCommand);
        NsProp("Status", &LobbyViewModel::GetStatus);
    }
};
}

#if defined(__ANDROID__)
extern "C"
{
JNIEXPORT void JNICALL Java_com_ixtreeme_client_IxtreemeNativeActivity_nativeOnTextInput(
    JNIEnv* env, jclass, jstring text)
{
    if (!text)
        return;

    const char* utf8 = env->GetStringUTFChars(text, nullptr);
    if (!utf8)
        return;

    QueueTextInputEvent(TextInputEvent{TextInputEventType::Text, utf8, 0});
    env->ReleaseStringUTFChars(text, utf8);
}

JNIEXPORT void JNICALL Java_com_ixtreeme_client_IxtreemeNativeActivity_nativeOnKeyDown(
    JNIEnv*, jclass, jint keyCode)
{
    QueueTextInputEvent(TextInputEvent{TextInputEventType::KeyDown, {}, static_cast<int>(keyCode)});
}

JNIEXPORT void JNICALL Java_com_ixtreeme_client_IxtreemeNativeActivity_nativeOnKeyUp(
    JNIEnv*, jclass, jint keyCode)
{
    QueueTextInputEvent(TextInputEvent{TextInputEventType::KeyUp, {}, static_cast<int>(keyCode)});
}

JNIEXPORT void JNICALL Java_com_ixtreeme_client_IxtreemeNativeActivity_nativeOnDeleteBackward(
    JNIEnv*, jclass)
{
    QueueTextInputEvent(TextInputEvent{TextInputEventType::Backspace, {}, 0});
}
}
#endif

struct NoesisLayer::Impl
{
    bool CreateView(Noesis::FrameworkElement* root, uint32_t width, uint32_t height)
    {
        if (!root)
        {
            Log("[NOESIS] FATAL: cannot create view from null root element.");
            return false;
        }

        if (view)
        {
            view->GetRenderer()->Shutdown();
            view.Reset();
        }

        view = Noesis::GUI::CreateView(root);
        view->SetSize(width, height);
        view->SetFlags(Noesis::RenderFlags_PPAA | Noesis::RenderFlags_LCD);
        view->GetRenderer()->Init(renderDevice);
        currentWidth = width;
        currentHeight = height;
        return true;
    }

    bool CreateMenuView(Noesis::FrameworkElement* root, uint32_t width, uint32_t height)
    {
        if (!root)
        {
            Log("[NOESIS] FATAL: cannot create menu view from null root element.");
            return false;
        }

        if (menuView)
        {
            menuView->GetRenderer()->Shutdown();
            menuView.Reset();
        }

        menuView = Noesis::GUI::CreateView(root);
        menuView->SetSize(width, height);
        menuView->SetFlags(Noesis::RenderFlags_PPAA | Noesis::RenderFlags_LCD);
        menuView->GetRenderer()->Init(renderDevice);
        return true;
    }

    bool CreateEditorView(Noesis::FrameworkElement* root, uint32_t width, uint32_t height)
    {
        if (!root)
        {
            Log("[NOESIS] FATAL: cannot create editor view from null root element.");
            return false;
        }

        if (editorView)
        {
            editorView->GetRenderer()->Shutdown();
            editorView.Reset();
        }

        editorView = Noesis::GUI::CreateView(root);
        editorView->SetSize(width, height);
        editorView->SetFlags(Noesis::RenderFlags_PPAA | Noesis::RenderFlags_LCD);
        editorView->GetRenderer()->Init(renderDevice);
        return true;
    }

    bool LoadLoginView(uint32_t width, uint32_t height)
    {
        lobbyActive = false;
        inGameMenuOpen = false;
        lobbyViewModel.Reset();

        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("Login.xaml");
        if (!root)
        {
            Log("[NOESIS] FATAL: Login.xaml could not be loaded. Aborting login view setup.");
            return false;
        }

        usernameBox = root->FindName<Noesis::TextBox>("UsernameBox");
        if (!usernameBox)
            Log("[NOESIS] WARNING: 'UsernameBox' not found in Login.xaml.");
#if defined(__ANDROID__)
        else
        {
            usernameBox->GotKeyboardFocus() += OnTextBoxGotFocus;
            usernameBox->LostKeyboardFocus() += OnTextInputLostFocus;
        }
#endif
        passwordBox = root->FindName<Noesis::PasswordBox>("PasswordBox");
        if (!passwordBox)
            Log("[NOESIS] WARNING: 'PasswordBox' not found in Login.xaml.");
#if defined(__ANDROID__)
        else
        {
            passwordBox->GotKeyboardFocus() += OnPasswordBoxGotFocus;
            passwordBox->LostKeyboardFocus() += OnTextInputLostFocus;
        }
#endif
        rememberBox = root->FindName<Noesis::CheckBox>("RememberBox");
        if (!rememberBox)
            Log("[NOESIS] WARNING: 'RememberBox' not found in Login.xaml.");
        statusText = root->FindName<Noesis::TextBlock>("StatusText");
        if (!statusText)
            Log("[NOESIS] WARNING: 'StatusText' not found in Login.xaml.");
        Noesis::Button* loginButton = root->FindName<Noesis::Button>("LoginButton");
        if (loginButton)
            loginButton->Click() += Noesis::MakeDelegate(this, &Impl::OnLoginClicked);
        else
            Log("[NOESIS] WARNING: 'LoginButton' not found in Login.xaml.");

        return CreateView(root, width, height);
    }

    bool LoadInGameMenuView(uint32_t width, uint32_t height)
    {
        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("InGameMenu.xaml");
        if (!root)
        {
            Log("[NOESIS] FATAL: InGameMenu.xaml could not be loaded. Aborting menu view setup.");
            return false;
        }

        Noesis::Button* button = root->FindName<Noesis::Button>("BtnCharacterSwitch");
        if (button)
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnCharacterSwitchClicked);
        else
            Log("[NOESIS] WARNING: 'BtnCharacterSwitch' not found in InGameMenu.xaml.");
        button = root->FindName<Noesis::Button>("BtnLogout");
        if (button)
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLogoutClicked);
        else
            Log("[NOESIS] WARNING: 'BtnLogout' not found in InGameMenu.xaml.");
        button = root->FindName<Noesis::Button>("BtnQuit");
        if (button)
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnQuitClicked);
        else
            Log("[NOESIS] WARNING: 'BtnQuit' not found in InGameMenu.xaml.");
        button = root->FindName<Noesis::Button>("BtnClose");
        if (button)
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnCloseMenuClicked);
        else
            Log("[NOESIS] WARNING: 'BtnClose' not found in InGameMenu.xaml.");
        return CreateMenuView(root, width, height);
    }

    bool LoadMapEditorView(uint32_t width, uint32_t height)
    {
        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("EditorPanel.xaml");
        if (!root)
        {
            Log("[NOESIS] FATAL: EditorPanel.xaml could not be loaded.");
            return false;
        }

        editorRaise = root->FindName<Noesis::RadioButton>("ToolRaise");
        editorLower = root->FindName<Noesis::RadioButton>("ToolLower");
        editorSmooth = root->FindName<Noesis::RadioButton>("ToolSmooth");
        editorFlatten = root->FindName<Noesis::RadioButton>("ToolFlatten");
        editorPaint = root->FindName<Noesis::RadioButton>("ToolPaint");
        editorRadius = root->FindName<Noesis::Slider>("BrushRadiusSlider");
        editorStrength = root->FindName<Noesis::Slider>("BrushStrengthSlider");
        editorRadiusText = root->FindName<Noesis::TextBlock>("BrushRadiusText");
        editorStrengthText = root->FindName<Noesis::TextBlock>("BrushStrengthText");
        editorTextureText = root->FindName<Noesis::TextBlock>("TextureSlotText");
        editorPaintReplace = root->FindName<Noesis::RadioButton>("PaintModeReplace");
        editorPaintMix = root->FindName<Noesis::RadioButton>("PaintModeMix");
        assetSearchBox = root->FindName<Noesis::TextBox>("AssetSearchBox");
        assetNameBox = root->FindName<Noesis::TextBox>("AssetNameBox");
        assetSubpathBox = root->FindName<Noesis::TextBox>("AssetSubpathBox");
        assetTagsBox = root->FindName<Noesis::TextBox>("AssetTagsBox");
        materialTilingXBox = root->FindName<Noesis::TextBox>("MaterialTilingXBox");
        materialTilingYBox = root->FindName<Noesis::TextBox>("MaterialTilingYBox");
        materialNormalStrengthBox = root->FindName<Noesis::TextBox>("MaterialNormalStrengthBox");
        materialTintBox = root->FindName<Noesis::TextBox>("MaterialTintBox");
        materialDiffuseText = root->FindName<Noesis::TextBlock>("MaterialDiffuseText");
        materialNormalText = root->FindName<Noesis::TextBlock>("MaterialNormalText");
        materialAoText = root->FindName<Noesis::TextBlock>("MaterialAoText");
        materialRoughnessText = root->FindName<Noesis::TextBlock>("MaterialRoughnessText");
        materialMetallicText = root->FindName<Noesis::TextBlock>("MaterialMetallicText");
        materialHeightText = root->FindName<Noesis::TextBlock>("MaterialHeightText");
        materialSlotButtons[0] = root->FindName<Noesis::Button>("MaterialDiffuseButton");
        materialSlotButtons[1] = root->FindName<Noesis::Button>("MaterialNormalButton");
        materialSlotButtons[2] = root->FindName<Noesis::Button>("MaterialAoButton");
        materialSlotButtons[3] = root->FindName<Noesis::Button>("MaterialRoughnessButton");
        materialSlotButtons[4] = root->FindName<Noesis::Button>("MaterialMetallicButton");
        materialSlotButtons[5] = root->FindName<Noesis::Button>("MaterialHeightButton");
        materialAoStrengthText = root->FindName<Noesis::TextBlock>("MaterialAoStrengthText");
        materialRoughnessStrengthText = root->FindName<Noesis::TextBlock>("MaterialRoughnessStrengthText");
        materialMetallicStrengthText = root->FindName<Noesis::TextBlock>("MaterialMetallicStrengthText");
        materialAoStrengthSlider = root->FindName<Noesis::Slider>("MaterialAoStrengthSlider");
        materialRoughnessStrengthSlider = root->FindName<Noesis::Slider>("MaterialRoughnessStrengthSlider");
        materialMetallicStrengthSlider = root->FindName<Noesis::Slider>("MaterialMetallicStrengthSlider");
        inspectorContextPanel = root->FindName<Noesis::FrameworkElement>("InspectorContextPanel");
        inspectorScrollViewport = root->FindName<Noesis::FrameworkElement>("InspectorScrollViewport");
        inspectorScrollContent = root->FindName<Noesis::FrameworkElement>("InspectorScrollContent");
        lightingPanel = root->FindName<Noesis::FrameworkElement>("LightingPanel");
        lightingMainSectionButton = root->FindName<Noesis::Button>("LightingMainSectionButton");
        lightingMainSection = root->FindName<Noesis::FrameworkElement>("LightingMainSection");
        waterBaseSectionButton = root->FindName<Noesis::Button>("WaterBaseSectionButton");
        waterBaseSection = root->FindName<Noesis::FrameworkElement>("WaterBaseSection");
        waterReflectionSectionButton = root->FindName<Noesis::Button>("WaterReflectionSectionButton");
        waterReflectionSection = root->FindName<Noesis::FrameworkElement>("WaterReflectionSection");
        waterRefractionSectionButton = root->FindName<Noesis::Button>("WaterRefractionSectionButton");
        waterRefractionSection = root->FindName<Noesis::FrameworkElement>("WaterRefractionSection");
        waterFoamSectionButton = root->FindName<Noesis::Button>("WaterFoamSectionButton");
        waterFoamSection = root->FindName<Noesis::FrameworkElement>("WaterFoamSection");
        waterCausticSectionButton = root->FindName<Noesis::Button>("WaterCausticSectionButton");
        waterCausticSection = root->FindName<Noesis::FrameworkElement>("WaterCausticSection");
        dynamicLightsSectionButton = root->FindName<Noesis::Button>("DynamicLightsSectionButton");
        dynamicLightsSection = root->FindName<Noesis::FrameworkElement>("DynamicLightsSection");
        selectedLightSectionButton = root->FindName<Noesis::Button>("SelectedLightSectionButton");
        selectedLightSection = root->FindName<Noesis::FrameworkElement>("SelectedLightSection");
        lightingAzimuthText = root->FindName<Noesis::TextBlock>("LightingAzimuthText");
        lightingElevationText = root->FindName<Noesis::TextBlock>("LightingElevationText");
        lightingSunIntensityText = root->FindName<Noesis::TextBlock>("LightingSunIntensityText");
        lightingSunRText = root->FindName<Noesis::TextBlock>("LightingSunRText");
        lightingSunGText = root->FindName<Noesis::TextBlock>("LightingSunGText");
        lightingSunBText = root->FindName<Noesis::TextBlock>("LightingSunBText");
        lightingAmbientIntensityText = root->FindName<Noesis::TextBlock>("LightingAmbientIntensityText");
        lightingAmbientRText = root->FindName<Noesis::TextBlock>("LightingAmbientRText");
        lightingAmbientGText = root->FindName<Noesis::TextBlock>("LightingAmbientGText");
        lightingAmbientBText = root->FindName<Noesis::TextBlock>("LightingAmbientBText");
        lightingAzimuthSlider = root->FindName<Noesis::Slider>("LightingAzimuthSlider");
        lightingElevationSlider = root->FindName<Noesis::Slider>("LightingElevationSlider");
        lightingSunIntensitySlider = root->FindName<Noesis::Slider>("LightingSunIntensitySlider");
        lightingSunRSlider = root->FindName<Noesis::Slider>("LightingSunRSlider");
        lightingSunGSlider = root->FindName<Noesis::Slider>("LightingSunGSlider");
        lightingSunBSlider = root->FindName<Noesis::Slider>("LightingSunBSlider");
        sunShadowsButton = root->FindName<Noesis::Button>("SunShadowsButton");
        lightingAmbientIntensitySlider = root->FindName<Noesis::Slider>("LightingAmbientIntensitySlider");
        lightingAmbientRSlider = root->FindName<Noesis::Slider>("LightingAmbientRSlider");
        lightingAmbientGSlider = root->FindName<Noesis::Slider>("LightingAmbientGSlider");
        lightingAmbientBSlider = root->FindName<Noesis::Slider>("LightingAmbientBSlider");
        waterEnabledButton = root->FindName<Noesis::Button>("WaterEnabledButton");
        waterLevelText = root->FindName<Noesis::TextBlock>("WaterLevelText");
        waterBaseRText = root->FindName<Noesis::TextBlock>("WaterBaseRText");
        waterBaseGText = root->FindName<Noesis::TextBlock>("WaterBaseGText");
        waterBaseBText = root->FindName<Noesis::TextBlock>("WaterBaseBText");
        waterAlphaText = root->FindName<Noesis::TextBlock>("WaterAlphaText");
        waterWaveScaleSmallText = root->FindName<Noesis::TextBlock>("WaterWaveScaleSmallText");
        waterWaveScaleLargeText = root->FindName<Noesis::TextBlock>("WaterWaveScaleLargeText");
        waterWaveSpeedSmallText = root->FindName<Noesis::TextBlock>("WaterWaveSpeedSmallText");
        waterWaveSpeedLargeText = root->FindName<Noesis::TextBlock>("WaterWaveSpeedLargeText");
        waterNormalStrengthText = root->FindName<Noesis::TextBlock>("WaterNormalStrengthText");
        waterFresnelPowerText = root->FindName<Noesis::TextBlock>("WaterFresnelPowerText");
        waterFresnelMinText = root->FindName<Noesis::TextBlock>("WaterFresnelMinText");
        waterReflectionRText = root->FindName<Noesis::TextBlock>("WaterReflectionRText");
        waterReflectionGText = root->FindName<Noesis::TextBlock>("WaterReflectionGText");
        waterReflectionBText = root->FindName<Noesis::TextBlock>("WaterReflectionBText");
        waterReflectionDistortionText = root->FindName<Noesis::TextBlock>("WaterReflectionDistortionText");
        waterShallowRText = root->FindName<Noesis::TextBlock>("WaterShallowRText");
        waterShallowGText = root->FindName<Noesis::TextBlock>("WaterShallowGText");
        waterShallowBText = root->FindName<Noesis::TextBlock>("WaterShallowBText");
        waterDeepRText = root->FindName<Noesis::TextBlock>("WaterDeepRText");
        waterDeepGText = root->FindName<Noesis::TextBlock>("WaterDeepGText");
        waterDeepBText = root->FindName<Noesis::TextBlock>("WaterDeepBText");
        waterDepthColorMinText = root->FindName<Noesis::TextBlock>("WaterDepthColorMinText");
        waterDepthColorMaxText = root->FindName<Noesis::TextBlock>("WaterDepthColorMaxText");
        waterDepthFadeText = root->FindName<Noesis::TextBlock>("WaterDepthFadeText");
        waterRefractionStrengthText = root->FindName<Noesis::TextBlock>("WaterRefractionStrengthText");
        waterRefractionDepthStrengthText = root->FindName<Noesis::TextBlock>("WaterRefractionDepthStrengthText");
        waterFoamDistanceText = root->FindName<Noesis::TextBlock>("WaterFoamDistanceText");
        waterFoamIntensityText = root->FindName<Noesis::TextBlock>("WaterFoamIntensityText");
        waterFoamScaleText = root->FindName<Noesis::TextBlock>("WaterFoamScaleText");
        waterFoamTerrainThicknessText = root->FindName<Noesis::TextBlock>("WaterFoamTerrainThicknessText");
        waterCausticIntensityText = root->FindName<Noesis::TextBlock>("WaterCausticIntensityText");
        waterCausticScaleText = root->FindName<Noesis::TextBlock>("WaterCausticScaleText");
        waterCausticSpeedText = root->FindName<Noesis::TextBlock>("WaterCausticSpeedText");
        waterCausticMaxDepthText = root->FindName<Noesis::TextBlock>("WaterCausticMaxDepthText");
        waterLevelSlider = root->FindName<Noesis::Slider>("WaterLevelSlider");
        waterBaseRSlider = root->FindName<Noesis::Slider>("WaterBaseRSlider");
        waterBaseGSlider = root->FindName<Noesis::Slider>("WaterBaseGSlider");
        waterBaseBSlider = root->FindName<Noesis::Slider>("WaterBaseBSlider");
        waterAlphaSlider = root->FindName<Noesis::Slider>("WaterAlphaSlider");
        waterWaveScaleSmallSlider = root->FindName<Noesis::Slider>("WaterWaveScaleSmallSlider");
        waterWaveScaleLargeSlider = root->FindName<Noesis::Slider>("WaterWaveScaleLargeSlider");
        waterWaveSpeedSmallSlider = root->FindName<Noesis::Slider>("WaterWaveSpeedSmallSlider");
        waterWaveSpeedLargeSlider = root->FindName<Noesis::Slider>("WaterWaveSpeedLargeSlider");
        waterNormalStrengthSlider = root->FindName<Noesis::Slider>("WaterNormalStrengthSlider");
        waterFresnelPowerSlider = root->FindName<Noesis::Slider>("WaterFresnelPowerSlider");
        waterFresnelMinSlider = root->FindName<Noesis::Slider>("WaterFresnelMinSlider");
        waterReflectionRSlider = root->FindName<Noesis::Slider>("WaterReflectionRSlider");
        waterReflectionGSlider = root->FindName<Noesis::Slider>("WaterReflectionGSlider");
        waterReflectionBSlider = root->FindName<Noesis::Slider>("WaterReflectionBSlider");
        waterReflectionDistortionSlider = root->FindName<Noesis::Slider>("WaterReflectionDistortionSlider");
        waterReflectionEnabledButton = root->FindName<Noesis::Button>("WaterReflectionEnabledButton");
        waterRefractionEnabledButton = root->FindName<Noesis::Button>("WaterRefractionEnabledButton");
        waterReflectionQuarterButton = root->FindName<Noesis::Button>("WaterReflectionQuarterButton");
        waterReflectionHalfButton = root->FindName<Noesis::Button>("WaterReflectionHalfButton");
        waterReflectionFullButton = root->FindName<Noesis::Button>("WaterReflectionFullButton");
        waterShallowRSlider = root->FindName<Noesis::Slider>("WaterShallowRSlider");
        waterShallowGSlider = root->FindName<Noesis::Slider>("WaterShallowGSlider");
        waterShallowBSlider = root->FindName<Noesis::Slider>("WaterShallowBSlider");
        waterDeepRSlider = root->FindName<Noesis::Slider>("WaterDeepRSlider");
        waterDeepGSlider = root->FindName<Noesis::Slider>("WaterDeepGSlider");
        waterDeepBSlider = root->FindName<Noesis::Slider>("WaterDeepBSlider");
        waterDepthColorMinSlider = root->FindName<Noesis::Slider>("WaterDepthColorMinSlider");
        waterDepthColorMaxSlider = root->FindName<Noesis::Slider>("WaterDepthColorMaxSlider");
        waterDepthFadeSlider = root->FindName<Noesis::Slider>("WaterDepthFadeSlider");
        waterRefractionStrengthSlider = root->FindName<Noesis::Slider>("WaterRefractionStrengthSlider");
        waterRefractionDepthStrengthSlider = root->FindName<Noesis::Slider>("WaterRefractionDepthStrengthSlider");
        waterFoamEnabledButton = root->FindName<Noesis::Button>("WaterFoamEnabledButton");
        waterFoamDistanceSlider = root->FindName<Noesis::Slider>("WaterFoamDistanceSlider");
        waterFoamIntensitySlider = root->FindName<Noesis::Slider>("WaterFoamIntensitySlider");
        waterFoamScaleSlider = root->FindName<Noesis::Slider>("WaterFoamScaleSlider");
        waterFoamTerrainThicknessSlider = root->FindName<Noesis::Slider>("WaterFoamTerrainThicknessSlider");
        waterCausticOffButton = root->FindName<Noesis::Button>("WaterCausticOffButton");
        waterCausticAnimatedButton = root->FindName<Noesis::Button>("WaterCausticAnimatedButton");
        waterCausticProceduralButton = root->FindName<Noesis::Button>("WaterCausticProceduralButton");
        waterCausticIntensitySlider = root->FindName<Noesis::Slider>("WaterCausticIntensitySlider");
        waterCausticScaleSlider = root->FindName<Noesis::Slider>("WaterCausticScaleSlider");
        waterCausticSpeedSlider = root->FindName<Noesis::Slider>("WaterCausticSpeedSlider");
        waterCausticMaxDepthSlider = root->FindName<Noesis::Slider>("WaterCausticMaxDepthSlider");
        addPointLightButton = root->FindName<Noesis::Button>("AddPointLightButton");
        addSpotLightButton = root->FindName<Noesis::Button>("AddSpotLightButton");
        dynamicLightCountText = root->FindName<Noesis::TextBlock>("DynamicLightCountText");
        selectedLightTitleText = root->FindName<Noesis::TextBlock>("SelectedLightTitleText");
        selectedLightXText = root->FindName<Noesis::TextBlock>("SelectedLightXText");
        selectedLightYText = root->FindName<Noesis::TextBlock>("SelectedLightYText");
        selectedLightZText = root->FindName<Noesis::TextBlock>("SelectedLightZText");
        selectedLightIntensityText = root->FindName<Noesis::TextBlock>("SelectedLightIntensityText");
        selectedLightRadiusText = root->FindName<Noesis::TextBlock>("SelectedLightRadiusText");
        selectedLightRText = root->FindName<Noesis::TextBlock>("SelectedLightRText");
        selectedLightGText = root->FindName<Noesis::TextBlock>("SelectedLightGText");
        selectedLightBText = root->FindName<Noesis::TextBlock>("SelectedLightBText");
        selectedSpotPitchText = root->FindName<Noesis::TextBlock>("SelectedSpotPitchText");
        selectedSpotYawText = root->FindName<Noesis::TextBlock>("SelectedSpotYawText");
        selectedSpotInnerText = root->FindName<Noesis::TextBlock>("SelectedSpotInnerText");
        selectedSpotOuterText = root->FindName<Noesis::TextBlock>("SelectedSpotOuterText");
        selectedLightXSlider = root->FindName<Noesis::Slider>("SelectedLightXSlider");
        selectedLightYSlider = root->FindName<Noesis::Slider>("SelectedLightYSlider");
        selectedLightZSlider = root->FindName<Noesis::Slider>("SelectedLightZSlider");
        selectedLightIntensitySlider = root->FindName<Noesis::Slider>("SelectedLightIntensitySlider");
        selectedLightRadiusSlider = root->FindName<Noesis::Slider>("SelectedLightRadiusSlider");
        selectedLightRSlider = root->FindName<Noesis::Slider>("SelectedLightRSlider");
        selectedLightGSlider = root->FindName<Noesis::Slider>("SelectedLightGSlider");
        selectedLightBSlider = root->FindName<Noesis::Slider>("SelectedLightBSlider");
        selectedSpotPitchSlider = root->FindName<Noesis::Slider>("SelectedSpotPitchSlider");
        selectedSpotYawSlider = root->FindName<Noesis::Slider>("SelectedSpotYawSlider");
        selectedSpotInnerSlider = root->FindName<Noesis::Slider>("SelectedSpotInnerSlider");
        selectedSpotOuterSlider = root->FindName<Noesis::Slider>("SelectedSpotOuterSlider");
        assetStatusText = root->FindName<Noesis::TextBlock>("AssetStatusText");
        assetFolderTree = root->FindName<Noesis::TreeView>("AssetFolderTree");
        assetBreadcrumbPanel = root->FindName<Noesis::StackPanel>("AssetBreadcrumbPanel");
        assetFolderCountText = root->FindName<Noesis::TextBlock>("AssetFolderCountText");

        if (editorRadius)
        {
            editorRadius->SetMinimum(0.5f);
            editorRadius->SetMaximum(50.0f);
            editorRadius->SetLargeChange(5.0f);
            editorRadius->SetSmallChange(0.5f);
            editorRadius->SetIsMoveToPointEnabled(true);
            editorRadius->SetValue(editorBrushRadiusMeters);
            editorRadius->ValueChanged() += Noesis::MakeDelegate(this, &Impl::OnEditorRadiusChanged);
        }
        if (editorStrength)
        {
            editorStrength->SetMinimum(0.1f);
            editorStrength->SetMaximum(5.0f);
            editorStrength->SetLargeChange(0.5f);
            editorStrength->SetSmallChange(0.1f);
            editorStrength->SetIsMoveToPointEnabled(true);
            editorStrength->SetValue(editorBrushStrength);
            editorStrength->ValueChanged() += Noesis::MakeDelegate(this, &Impl::OnEditorStrengthChanged);
        }
        if (editorPaintReplace)
            editorPaintReplace->SetIsChecked(true);
        if (assetSearchBox)
            assetSearchBox->TextChanged() += Noesis::MakeDelegate(this, &Impl::OnAssetSearchChanged);
        if (materialAoStrengthSlider)
            materialAoStrengthSlider->ValueChanged() += Noesis::MakeDelegate(this, &Impl::OnMaterialAoStrengthChanged);
        if (materialRoughnessStrengthSlider)
            materialRoughnessStrengthSlider->ValueChanged() += Noesis::MakeDelegate(this, &Impl::OnMaterialRoughnessStrengthChanged);
        if (materialMetallicStrengthSlider)
            materialMetallicStrengthSlider->ValueChanged() += Noesis::MakeDelegate(this, &Impl::OnMaterialMetallicStrengthChanged);
        ConfigureLightingSlider(lightingAzimuthSlider, 0.0f, 360.0f, 15.0f, 1.0f, lightingState.directional.azimuthDegrees,
            &Impl::OnLightingAzimuthChanged);
        ConfigureLightingSlider(lightingElevationSlider, 0.0f, 90.0f, 10.0f, 1.0f, lightingState.directional.elevationDegrees,
            &Impl::OnLightingElevationChanged);
        ConfigureLightingSlider(lightingSunIntensitySlider, 0.0f, 5.0f, 0.5f, 0.05f, lightingState.directional.intensity,
            &Impl::OnLightingSunIntensityChanged);
        ConfigureLightingSlider(lightingSunRSlider, 0.0f, 1.0f, 0.1f, 0.01f, lightingState.directional.r,
            &Impl::OnLightingSunRChanged);
        ConfigureLightingSlider(lightingSunGSlider, 0.0f, 1.0f, 0.1f, 0.01f, lightingState.directional.g,
            &Impl::OnLightingSunGChanged);
        ConfigureLightingSlider(lightingSunBSlider, 0.0f, 1.0f, 0.1f, 0.01f, lightingState.directional.b,
            &Impl::OnLightingSunBChanged);
        ConfigureLightingSlider(lightingAmbientIntensitySlider, 0.0f, 3.0f, 0.3f, 0.05f, lightingState.ambient.intensity,
            &Impl::OnLightingAmbientIntensityChanged);
        ConfigureLightingSlider(lightingAmbientRSlider, 0.0f, 1.0f, 0.1f, 0.01f, lightingState.ambient.r,
            &Impl::OnLightingAmbientRChanged);
        ConfigureLightingSlider(lightingAmbientGSlider, 0.0f, 1.0f, 0.1f, 0.01f, lightingState.ambient.g,
            &Impl::OnLightingAmbientGChanged);
        ConfigureLightingSlider(lightingAmbientBSlider, 0.0f, 1.0f, 0.1f, 0.01f, lightingState.ambient.b,
            &Impl::OnLightingAmbientBChanged);
        ConfigureLightingSlider(waterLevelSlider, -50.0f, 50.0f, 5.0f, 0.1f, waterConfig.waterLevelY, &Impl::OnWaterLevelChanged);
        ConfigureLightingSlider(waterBaseRSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.baseColor[0], &Impl::OnWaterBaseRChanged);
        ConfigureLightingSlider(waterBaseGSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.baseColor[1], &Impl::OnWaterBaseGChanged);
        ConfigureLightingSlider(waterBaseBSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.baseColor[2], &Impl::OnWaterBaseBChanged);
        ConfigureLightingSlider(waterAlphaSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.baseColor[3], &Impl::OnWaterAlphaChanged);
        ConfigureLightingSlider(waterWaveScaleSmallSlider, 0.001f, 0.12f, 0.01f, 0.001f, waterConfig.waveScaleSmall, &Impl::OnWaterWaveScaleSmallChanged);
        ConfigureLightingSlider(waterWaveScaleLargeSlider, 0.001f, 0.08f, 0.01f, 0.001f, waterConfig.waveScaleLarge, &Impl::OnWaterWaveScaleLargeChanged);
        ConfigureLightingSlider(waterWaveSpeedSmallSlider, 0.0f, 0.5f, 0.05f, 0.005f, waterConfig.waveSpeedSmall, &Impl::OnWaterWaveSpeedSmallChanged);
        ConfigureLightingSlider(waterWaveSpeedLargeSlider, 0.0f, 0.5f, 0.05f, 0.005f, waterConfig.waveSpeedLarge, &Impl::OnWaterWaveSpeedLargeChanged);
        ConfigureLightingSlider(waterNormalStrengthSlider, 0.0f, 2.0f, 0.2f, 0.01f, waterConfig.normalStrength, &Impl::OnWaterNormalStrengthChanged);
        ConfigureLightingSlider(waterFresnelPowerSlider, 1.0f, 10.0f, 1.0f, 0.1f, waterConfig.fresnelPower, &Impl::OnWaterFresnelPowerChanged);
        ConfigureLightingSlider(waterFresnelMinSlider, 0.0f, 0.5f, 0.05f, 0.005f, waterConfig.fresnelMin, &Impl::OnWaterFresnelMinChanged);
        ConfigureLightingSlider(waterReflectionRSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.reflectionColor[0], &Impl::OnWaterReflectionRChanged);
        ConfigureLightingSlider(waterReflectionGSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.reflectionColor[1], &Impl::OnWaterReflectionGChanged);
        ConfigureLightingSlider(waterReflectionBSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.reflectionColor[2], &Impl::OnWaterReflectionBChanged);
        ConfigureLightingSlider(waterReflectionDistortionSlider, 0.0f, 0.2f, 0.02f, 0.005f,
            waterConfig.reflectionDistortionStrength, &Impl::OnWaterReflectionDistortionChanged);
        ConfigureLightingSlider(waterShallowRSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.shallowColor[0], &Impl::OnWaterShallowRChanged);
        ConfigureLightingSlider(waterShallowGSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.shallowColor[1], &Impl::OnWaterShallowGChanged);
        ConfigureLightingSlider(waterShallowBSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.shallowColor[2], &Impl::OnWaterShallowBChanged);
        ConfigureLightingSlider(waterDeepRSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.deepColor[0], &Impl::OnWaterDeepRChanged);
        ConfigureLightingSlider(waterDeepGSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.deepColor[1], &Impl::OnWaterDeepGChanged);
        ConfigureLightingSlider(waterDeepBSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.deepColor[2], &Impl::OnWaterDeepBChanged);
        ConfigureLightingSlider(waterDepthColorMinSlider, 0.0f, 50.0f, 5.0f, 0.1f, waterConfig.depthColorMin, &Impl::OnWaterDepthColorMinChanged);
        ConfigureLightingSlider(waterDepthColorMaxSlider, 0.01f, 50.0f, 5.0f, 0.1f, waterConfig.depthColorMax, &Impl::OnWaterDepthColorMaxChanged);
        ConfigureLightingSlider(waterDepthFadeSlider, 0.01f, 50.0f, 5.0f, 0.1f, waterConfig.depthFadeDistance, &Impl::OnWaterDepthFadeChanged);
        ConfigureLightingSlider(waterRefractionStrengthSlider, 0.0f, 0.1f, 0.01f, 0.001f, waterConfig.refractionStrength, &Impl::OnWaterRefractionStrengthChanged);
        ConfigureLightingSlider(waterRefractionDepthStrengthSlider, 0.0f, 2.0f, 0.2f, 0.01f, waterConfig.refractionDepthStrength, &Impl::OnWaterRefractionDepthStrengthChanged);
        ConfigureLightingSlider(waterFoamDistanceSlider, 0.02f, 1.5f, 0.1f, 0.01f, waterConfig.foamDistance, &Impl::OnWaterFoamDistanceChanged);
        ConfigureLightingSlider(waterFoamIntensitySlider, 0.0f, 2.0f, 0.2f, 0.01f, waterConfig.foamIntensity, &Impl::OnWaterFoamIntensityChanged);
        ConfigureLightingSlider(waterFoamScaleSlider, 0.1f, 2.0f, 0.2f, 0.01f, waterConfig.foamScale, &Impl::OnWaterFoamScaleChanged);
        ConfigureLightingSlider(waterFoamTerrainThicknessSlider, 0.0f, 1.0f, 0.1f, 0.01f, waterConfig.foamTerrainThickness, &Impl::OnWaterFoamTerrainThicknessChanged);
        ConfigureLightingSlider(waterCausticIntensitySlider, 0.0f, 3.0f, 0.3f, 0.01f, waterConfig.causticIntensity, &Impl::OnWaterCausticIntensityChanged);
        ConfigureLightingSlider(waterCausticScaleSlider, 0.1f, 2.0f, 0.2f, 0.01f, waterConfig.causticScale, &Impl::OnWaterCausticScaleChanged);
        ConfigureLightingSlider(waterCausticSpeedSlider, 0.0f, 2.0f, 0.2f, 0.01f, waterConfig.causticSpeed, &Impl::OnWaterCausticSpeedChanged);
        ConfigureLightingSlider(waterCausticMaxDepthSlider, 1.0f, 30.0f, 2.0f, 0.1f, waterConfig.causticMaxDepth, &Impl::OnWaterCausticMaxDepthChanged);
        ConfigureLightingSlider(selectedLightXSlider, -200.0f, 200.0f, 10.0f, 0.1f, 0.0f, &Impl::OnSelectedLightXChanged);
        ConfigureLightingSlider(selectedLightYSlider, -50.0f, 100.0f, 5.0f, 0.1f, 0.0f, &Impl::OnSelectedLightYChanged);
        ConfigureLightingSlider(selectedLightZSlider, -200.0f, 200.0f, 10.0f, 0.1f, 0.0f, &Impl::OnSelectedLightZChanged);
        ConfigureLightingSlider(selectedLightIntensitySlider, 0.0f, 10.0f, 1.0f, 0.1f, 1.0f, &Impl::OnSelectedLightIntensityChanged);
        ConfigureLightingSlider(selectedLightRadiusSlider, 0.1f, 100.0f, 5.0f, 0.1f, 10.0f, &Impl::OnSelectedLightRadiusChanged);
        ConfigureLightingSlider(selectedLightRSlider, 0.0f, 1.0f, 0.1f, 0.01f, 1.0f, &Impl::OnSelectedLightRChanged);
        ConfigureLightingSlider(selectedLightGSlider, 0.0f, 1.0f, 0.1f, 0.01f, 0.95f, &Impl::OnSelectedLightGChanged);
        ConfigureLightingSlider(selectedLightBSlider, 0.0f, 1.0f, 0.1f, 0.01f, 0.75f, &Impl::OnSelectedLightBChanged);
        ConfigureLightingSlider(selectedSpotPitchSlider, -90.0f, 90.0f, 10.0f, 0.5f, -90.0f, &Impl::OnSelectedSpotPitchChanged);
        ConfigureLightingSlider(selectedSpotYawSlider, -180.0f, 180.0f, 10.0f, 0.5f, 0.0f, &Impl::OnSelectedSpotYawChanged);
        ConfigureLightingSlider(selectedSpotInnerSlider, 1.0f, 89.0f, 5.0f, 0.5f, 20.0f, &Impl::OnSelectedSpotInnerChanged);
        ConfigureLightingSlider(selectedSpotOuterSlider, 1.0f, 90.0f, 5.0f, 0.5f, 35.0f, &Impl::OnSelectedSpotOuterChanged);

        if (Noesis::Button* button = root->FindName<Noesis::Button>("SaveButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnEditorSaveClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("ReloadButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnEditorReloadClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("UndoButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnEditorUndoClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("AddMarkerButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnAddMarkerClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("LightingButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLightingButtonClicked);
        if (lightingMainSectionButton)
            lightingMainSectionButton->Click() += Noesis::MakeDelegate(this, &Impl::OnLightingMainSectionClicked);
        if (waterBaseSectionButton)
            waterBaseSectionButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterBaseSectionClicked);
        if (waterReflectionSectionButton)
            waterReflectionSectionButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterReflectionSectionClicked);
        if (waterRefractionSectionButton)
            waterRefractionSectionButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterRefractionSectionClicked);
        if (waterFoamSectionButton)
            waterFoamSectionButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterFoamSectionClicked);
        if (waterCausticSectionButton)
            waterCausticSectionButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterCausticSectionClicked);
        if (dynamicLightsSectionButton)
            dynamicLightsSectionButton->Click() += Noesis::MakeDelegate(this, &Impl::OnDynamicLightsSectionClicked);
        if (selectedLightSectionButton)
            selectedLightSectionButton->Click() += Noesis::MakeDelegate(this, &Impl::OnSelectedLightSectionClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("LightingSunEnabledButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLightingSunEnabledClicked);
        if (sunShadowsButton)
            sunShadowsButton->Click() += Noesis::MakeDelegate(this, &Impl::OnSunShadowsClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("LightingResetButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLightingResetClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("LightingDawnButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLightingDawnClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("LightingNoonButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLightingNoonClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("LightingDuskButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLightingDuskClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("LightingNightButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLightingNightClicked);
        if (waterEnabledButton)
            waterEnabledButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterEnabledClicked);
        if (waterReflectionEnabledButton)
            waterReflectionEnabledButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterReflectionEnabledClicked);
        if (waterReflectionQuarterButton)
            waterReflectionQuarterButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterReflectionQuarterClicked);
        if (waterReflectionHalfButton)
            waterReflectionHalfButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterReflectionHalfClicked);
        if (waterReflectionFullButton)
            waterReflectionFullButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterReflectionFullClicked);
        if (waterRefractionEnabledButton)
            waterRefractionEnabledButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterRefractionEnabledClicked);
        if (waterFoamEnabledButton)
            waterFoamEnabledButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterFoamEnabledClicked);
        if (waterCausticOffButton)
            waterCausticOffButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterCausticOffClicked);
        if (waterCausticAnimatedButton)
            waterCausticAnimatedButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterCausticAnimatedClicked);
        if (waterCausticProceduralButton)
            waterCausticProceduralButton->Click() += Noesis::MakeDelegate(this, &Impl::OnWaterCausticProceduralClicked);
        if (addPointLightButton)
            addPointLightButton->Click() += Noesis::MakeDelegate(this, &Impl::OnAddPointLightClicked);
        if (addSpotLightButton)
            addSpotLightButton->Click() += Noesis::MakeDelegate(this, &Impl::OnAddSpotLightClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("SelectedLightEnabledButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnSelectedLightEnabledClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("DeleteLightButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnDeleteLightClicked);

        for (uint32_t i = 0; i < 8; ++i)
        {
            char name[16];
            std::snprintf(name, sizeof(name), "Slot%uButton", i);
            Noesis::Button* button = root->FindName<Noesis::Button>(name);
            if (!button)
            {
                std::snprintf(name, sizeof(name), "Tex%uButton", i);
                button = root->FindName<Noesis::Button>(name);
            }
            if (button)
            {
                editorTextureButtons[i] = button;
                button->Click() += Noesis::MakeDelegate(this, &Impl::OnEditorTextureClicked);
            }

            std::snprintf(name, sizeof(name), "Slot%uText", i);
            editorTextureSlotTexts[i] = root->FindName<Noesis::TextBlock>(name);

            std::snprintf(name, sizeof(name), "Asset%uButton", i);
            if (Noesis::Button* assetButton = root->FindName<Noesis::Button>(name))
            {
                assetButtons[i] = assetButton;
                assetButton->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetClicked);
            }

            std::snprintf(name, sizeof(name), "Asset%uText", i);
            assetTexts[i] = root->FindName<Noesis::TextBlock>(name);

            std::snprintf(name, sizeof(name), "Asset%uImage", i);
            assetImages[i] = root->FindName<Noesis::Image>(name);
        }

        for (uint32_t i = 8; i < assetButtons.size(); ++i)
        {
            char name[16];
            std::snprintf(name, sizeof(name), "Asset%uButton", i);
            if (Noesis::Button* assetButton = root->FindName<Noesis::Button>(name))
            {
                assetButtons[i] = assetButton;
                assetButton->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetClicked);
            }

            std::snprintf(name, sizeof(name), "Asset%uText", i);
            assetTexts[i] = root->FindName<Noesis::TextBlock>(name);

            std::snprintf(name, sizeof(name), "Asset%uImage", i);
            assetImages[i] = root->FindName<Noesis::Image>(name);
        }

        for (uint32_t i = 0; i < assetTagButtons.size(); ++i)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "AssetTag%uButton", i);
            if (Noesis::Button* button = root->FindName<Noesis::Button>(name))
            {
                assetTagButtons[i] = button;
                button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetTagClicked);
            }
            std::snprintf(name, sizeof(name), "AssetTag%uText", i);
            assetTagTexts[i] = root->FindName<Noesis::TextBlock>(name);
        }

        if (Noesis::Button* button = root->FindName<Noesis::Button>("AssetTexturesButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetTexturesClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("AssetModelsButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetModelsClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("AssetAnimationsButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetAnimationsClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("AssetMaterialsButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetMaterialsClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("ImportTextureButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnImportTextureClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("ImportModelButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnImportModelClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("ImportAnimationButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnImportAnimationClicked);
        if (assetFolderTree)
            assetFolderTree->SelectedItemChanged() += Noesis::MakeDelegate(this, &Impl::OnAssetFolderTreeSelected);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("AssetFolderHomeButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetFolderHomeClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("AssetFolderUpButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetFolderUpClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("AssetClearButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetClearClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("AssetRefreshButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetRefreshClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("ApplyAssetMetaButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnApplyAssetMetaClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("NewMaterialButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnNewMaterialClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("SaveMaterialButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnSaveMaterialClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("SaveAsMaterialButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnSaveAsMaterialClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("LoadMaterialButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLoadMaterialClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("MaterialDiffuseButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnMaterialDiffuseSlotClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("MaterialNormalButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnMaterialNormalSlotClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("MaterialAoButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnMaterialAoSlotClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("MaterialRoughnessButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnMaterialRoughnessSlotClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("MaterialMetallicButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnMaterialMetallicSlotClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("MaterialHeightButton"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnMaterialHeightSlotClicked);

        if (EnsureAssetLibrary())
            RefreshAssetBrowser();

        if (editorRaise)
            editorRaise->SetIsChecked(true);
        ApplyBrushSliderState(editorRadius ? static_cast<float>(editorRadius->GetValue()) : editorBrushRadiusMeters,
            editorStrength ? static_cast<float>(editorStrength->GetValue()) : editorBrushStrength);
        ApplyLightingStateToControls();
        ResetMaterialEditor();
        UpdateEditorTextureText();
        RefreshPaletteSlotText();
        return CreateEditorView(root, width, height);
    }

    bool LoadLobbyView(uint32_t width, uint32_t height)
    {
        inGameMenuOpen = false;
        usernameBox = nullptr;
        passwordBox = nullptr;
        rememberBox = nullptr;
        statusText = nullptr;

        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("Lobby.xaml");
        if (!root)
        {
            Log("[NOESIS] FATAL: Lobby.xaml could not be loaded. Aborting lobby view setup.");
            return false;
        }

        lobbyViewModel = Noesis::MakePtr<LobbyViewModel>([this](uint64_t characterId) {
            if (clientSession)
                clientSession->SendCharacterSelect(characterId);
        });
        root->SetDataContext(lobbyViewModel);
        const bool created = CreateView(root, width, height);
        lobbyActive = created;
        return created;
    }

    bool LoadWorldHudView(uint32_t width, uint32_t height)
    {
        lobbyActive = false;
        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("WorldHud.xaml");
        if (!root)
        {
            Log("[NOESIS] FATAL: WorldHud.xaml could not be loaded.");
            return false;
        }
        return CreateView(root, width, height);
    }

    void ToggleInGameMenu()
    {
        inGameMenuOpen = !inGameMenuOpen;
        LogFormat("[MENU] in-game menu %s", inGameMenuOpen ? "open" : "closed");
    }

    void ToggleMapEditor()
    {
        mapEditorOpen = !mapEditorOpen;
        ClearKeyboardFocus();
        LogFormat("[MAP-EDITOR] panel %s", mapEditorOpen ? "open" : "closed");
    }

    Noesis::IView* ActiveInputView() const
    {
        if (inGameMenuOpen && menuView)
            return menuView.GetPtr();
        if (mapEditorOpen && editorView)
            return editorView.GetPtr();
        return view.GetPtr();
    }

    static void ClearViewKeyboardFocus(Noesis::IView* targetView)
    {
        if (!targetView || !targetView->GetContent())
            return;

        if (Noesis::Keyboard* keyboard = targetView->GetContent()->GetKeyboard())
            keyboard->ClearFocus();
    }

    void ClearKeyboardFocus()
    {
        ClearViewKeyboardFocus(view.GetPtr());
        ClearViewKeyboardFocus(menuView.GetPtr());
        ClearViewKeyboardFocus(editorView.GetPtr());
    }

    bool IsTextInputFocused() const
    {
        Noesis::IView* targetView = ActiveInputView();
        if (!targetView || !targetView->GetContent())
            return false;

        Noesis::Keyboard* keyboard = targetView->GetContent()->GetKeyboard();
        if (!keyboard)
            return false;

        Noesis::UIElement* focused = keyboard->GetFocused();
        return Noesis::DynamicCast<Noesis::TextBox*>(focused) != nullptr ||
               Noesis::DynamicCast<Noesis::PasswordBox*>(focused) != nullptr;
    }

    MapEditorSettings GetMapEditorSettings() const
    {
        MapEditorSettings settings{};
        if (editorLower && editorLower->GetIsChecked().GetValueOrDefault())
            settings.tool = MapEditorTool::Lower;
        else if (editorSmooth && editorSmooth->GetIsChecked().GetValueOrDefault())
            settings.tool = MapEditorTool::Smooth;
        else if (editorFlatten && editorFlatten->GetIsChecked().GetValueOrDefault())
            settings.tool = MapEditorTool::Flatten;
        else if (editorPaint && editorPaint->GetIsChecked().GetValueOrDefault())
            settings.tool = MapEditorTool::Paint;
        else
            settings.tool = MapEditorTool::Raise;

        settings.brushRadiusMeters = editorBrushRadiusMeters;
        settings.brushStrength = editorBrushStrength;
        settings.textureSlot = editorTextureSlot;
        settings.paintMode =
            editorPaintMix && editorPaintMix->GetIsChecked().GetValueOrDefault()
                ? MapEditorPaintMode::Mix
                : MapEditorPaintMode::Replace;
        return settings;
    }

    MapEditorCommands ConsumeMapEditorCommands()
    {
        MapEditorCommands commands = editorCommands;
        editorCommands = {};
        return commands;
    }

    void InitializeAssetLibrary(const std::string& mapDirectory,
                                const std::array<MapEditorPaletteSlot, 8>& defaultSlots)
    {
        assetMapDirectory = mapDirectory;
        editorPaletteSlots = defaultSlots;
        for (uint32_t i = 0; i < editorPaletteSlots.size(); ++i)
        {
            editorPaletteSlots[i].slot = i;
            if (editorPaletteSlots[i].displayName.empty())
                editorPaletteSlots[i].displayName = "Slot " + std::to_string(i);
        }

        if (EnsureAssetLibrary())
            editorPaletteSlots = assetLibrary->LoadWorldPalette(assetMapDirectory, editorPaletteSlots);

        RefreshPaletteSlotText();
        RefreshAssetBrowser();
    }

    std::array<MapEditorPaletteSlot, 8> GetPaletteSlots() const
    {
        return editorPaletteSlots;
    }

    bool EnsureAssetLibrary()
    {
        if (assetLibrary)
            return true;

        assetLibrary = std::make_unique<AssetLibrary>(FindClientRoot());
        if (!assetLibrary->Initialize())
        {
            assetLibrary.reset();
            SetAssetStatus("Asset library init failed");
            Log("[ASSET-LIBRARY] failed to initialize");
            return false;
        }
        return true;
    }

    static void SetText(Noesis::TextBlock* textBlock, const std::string& text)
    {
        if (textBlock)
            textBlock->SetText(text.c_str());
    }

    static void SetText(Noesis::TextBox* textBox, const std::string& text)
    {
        if (textBox)
            textBox->SetText(text.c_str());
    }

    static std::string GetText(Noesis::TextBox* textBox)
    {
        const char* text = textBox ? textBox->GetText() : "";
        return text ? text : "";
    }

    static float ParseFloatText(Noesis::TextBox* textBox, float fallback)
    {
        const std::string text = GetText(textBox);
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        return end != text.c_str() ? value : fallback;
    }

    void SetMaterialMetadataReadOnly(bool readOnly)
    {
        if (assetNameBox)
            assetNameBox->SetIsReadOnly(readOnly);
        if (assetSubpathBox)
            assetSubpathBox->SetIsReadOnly(readOnly);
        if (assetTagsBox)
            assetTagsBox->SetIsReadOnly(readOnly);
    }

    void ResetMaterialEditor()
    {
        editingMaterialId.reset();
        materialSaveAsEditMode = false;
        SetMaterialMetadataReadOnly(false);
        materialDraft = {};
        materialDraft.tilingScaleX = 1.0f;
        materialDraft.tilingScaleY = 1.0f;
        materialDraft.normalStrength = 1.0f;
        materialDraft.aoStrength = 1.0f;
        materialDraft.roughnessStrength = 1.0f;
        materialDraft.metallicStrength = 1.0f;
        activeMaterialTextureSlot = 0;
        SetText(assetNameBox, "");
        SetText(assetSubpathBox, "");
        SetText(assetTagsBox, "");
        SetText(materialTilingXBox, "1.0");
        SetText(materialTilingYBox, "1.0");
        SetText(materialNormalStrengthBox, "1.0");
        SetText(materialTintBox, "#ffffff");
        ApplyMaterialPbrStrengthSliders();
        UpdateMaterialEditorText();
    }

    void LoadMaterialDraftFromEntry(const AssetLibrary::Entry& entry)
    {
        pendingMaterialTargetSlotValid = false;
        materialSaveAsEditMode = false;
        editingMaterialId = entry.id;
        materialDraft = entry.material;
        SetText(assetNameBox, entry.displayName);
        SetText(assetSubpathBox, entry.subpath);
        SetText(assetTagsBox, AssetLibrary::TagsToCsv(entry.tags));
        SetMaterialMetadataReadOnly(true);
        SetText(materialTilingXBox, std::to_string(materialDraft.tilingScaleX));
        SetText(materialTilingYBox, std::to_string(materialDraft.tilingScaleY));
        SetText(materialNormalStrengthBox, std::to_string(materialDraft.normalStrength));
        SetText(materialTintBox, TintToText(materialDraft.colorTint));
        ApplyMaterialPbrStrengthSliders();
        UpdateMaterialEditorText();
    }

    static void ParseTint(const std::string& text, float out[3])
    {
        out[0] = 1.0f;
        out[1] = 1.0f;
        out[2] = 1.0f;
        if (text.size() != 7 || text[0] != '#')
            return;
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 15;
        };
        out[0] = static_cast<float>(hex(text[1]) * 16 + hex(text[2])) / 255.0f;
        out[1] = static_cast<float>(hex(text[3]) * 16 + hex(text[4])) / 255.0f;
        out[2] = static_cast<float>(hex(text[5]) * 16 + hex(text[6])) / 255.0f;
    }

    static std::string TintToText(const float tint[3])
    {
        auto tintByte = [](float value) {
            return std::clamp(static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)), 0, 255);
        };
        char text[16];
        std::snprintf(text, sizeof(text), "#%02x%02x%02x",
            tintByte(tint[0]), tintByte(tint[1]), tintByte(tint[2]));
        return text;
    }

    static std::string MaterialSlotName(uint32_t slot)
    {
        switch (slot)
        {
        case 0: return "diffuse";
        case 1: return "normal";
        case 2: return "AO";
        case 3: return "roughness";
        case 4: return "metallic";
        case 5: return "height";
        default: return "texture";
        }
    }

    static bool TextureRoleMatchesMaterialSlot(AssetLibrary::TextureRole role, uint32_t slot)
    {
        switch (slot)
        {
        case 0: return role == AssetLibrary::TextureRole::Diffuse;
        case 1: return role == AssetLibrary::TextureRole::Normal;
        case 2: return role == AssetLibrary::TextureRole::Ao || role == AssetLibrary::TextureRole::ArmPacked;
        case 3: return role == AssetLibrary::TextureRole::Roughness || role == AssetLibrary::TextureRole::ArmPacked;
        case 4: return role == AssetLibrary::TextureRole::Metallic || role == AssetLibrary::TextureRole::ArmPacked;
        case 5: return role == AssetLibrary::TextureRole::Height;
        default: return false;
        }
    }

    static std::string TextureResolutionLabel(const AssetLibrary::Entry& entry)
    {
        const uint32_t maxSide = std::max(entry.resolutionWidth, entry.resolutionHeight);
        if (maxSide == 0)
            return "-";
        if (maxSide >= 1024 && maxSide % 1024 == 0)
            return std::to_string(maxSide / 1024) + "K";
        return std::to_string(entry.resolutionWidth) + "x" + std::to_string(entry.resolutionHeight);
    }

    static std::string TextureRoleDescription(AssetLibrary::TextureRole role)
    {
        switch (role)
        {
        case AssetLibrary::TextureRole::Diffuse: return "Diffuse";
        case AssetLibrary::TextureRole::Normal: return "Normal";
        case AssetLibrary::TextureRole::Ao: return "AO";
        case AssetLibrary::TextureRole::Roughness: return "Roughness";
        case AssetLibrary::TextureRole::Metallic: return "Metallic";
        case AssetLibrary::TextureRole::Height: return "Height";
        case AssetLibrary::TextureRole::ArmPacked: return "ARM";
        case AssetLibrary::TextureRole::Unknown: return "Unknown";
        default: return "Unknown";
        }
    }

    static std::string WrongTextureRoleMessage(const AssetLibrary::Entry& entry, uint32_t slot)
    {
        if (entry.textureRole == AssetLibrary::TextureRole::Unknown)
            return "Cannot determine role; rename with a diffuse/normal/AO/roughness/metallic/height suffix.";
        return "This is a " + TextureRoleDescription(entry.textureRole) + " map; pick a " +
            MaterialSlotName(slot) + " texture or click the matching material slot first.";
    }

    void UpdateMaterialEditorText()
    {
        const std::string diffuse = materialDraft.diffuseTextureId.empty() ? "empty" : materialDraft.diffuseTextureId;
        const std::string normal = materialDraft.normalTextureId.empty() ? "flat" : materialDraft.normalTextureId;
        const std::string ao = materialDraft.aoTextureId.empty() ? "default" : materialDraft.aoTextureId;
        const std::string roughness = materialDraft.roughnessTextureId.empty() ? "0.5" : materialDraft.roughnessTextureId;
        const std::string metallic = materialDraft.metallicTextureId.empty() ? "0.0" : materialDraft.metallicTextureId;
        const std::string height = materialDraft.heightTextureId.empty() ? "empty" : materialDraft.heightTextureId;
        SetText(materialDiffuseText,
            std::string(activeMaterialTextureSlot == 0 ? "> " : "") + "Diffuse: " + CompactAssetName(diffuse, 22));
        SetText(materialNormalText,
            std::string(activeMaterialTextureSlot == 1 ? "> " : "") + "Normal: " + CompactAssetName(normal, 22));
        SetText(materialAoText,
            std::string(activeMaterialTextureSlot == 2 ? "> " : "") + "AO: " + CompactAssetName(ao, 22));
        SetText(materialRoughnessText,
            std::string(activeMaterialTextureSlot == 3 ? "> " : "") + "Rough: " + CompactAssetName(roughness, 22));
        SetText(materialMetallicText,
            std::string(activeMaterialTextureSlot == 4 ? "> " : "") + "Metal: " + CompactAssetName(metallic, 22));
        SetText(materialHeightText,
            std::string(activeMaterialTextureSlot == 5 ? "> " : "") + "Height: " + CompactAssetName(height, 22));
        UpdateMaterialPbrStrengthText();
    }

    void UpdateMaterialPbrStrengthText()
    {
        char buffer[80];
        std::snprintf(buffer, sizeof(buffer), "AO Strength: %.1f", materialDraft.aoStrength);
        SetText(materialAoStrengthText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Roughness Strength: %.1f", materialDraft.roughnessStrength);
        SetText(materialRoughnessStrengthText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Metallic Strength: %.1f", materialDraft.metallicStrength);
        SetText(materialMetallicStrengthText, buffer);
    }

    void ApplyMaterialPbrStrengthSliders()
    {
        if (materialAoStrengthSlider)
            materialAoStrengthSlider->SetValue(std::clamp(materialDraft.aoStrength, 0.0f, 2.0f));
        if (materialRoughnessStrengthSlider)
            materialRoughnessStrengthSlider->SetValue(std::clamp(materialDraft.roughnessStrength, 0.0f, 2.0f));
        if (materialMetallicStrengthSlider)
            materialMetallicStrengthSlider->SetValue(std::clamp(materialDraft.metallicStrength, 0.0f, 2.0f));
        UpdateMaterialPbrStrengthText();
    }

    void PullMaterialDraftFromUi()
    {
        materialDraft.tilingScaleX = std::clamp(ParseFloatText(materialTilingXBox, 1.0f), 0.1f, 10.0f);
        materialDraft.tilingScaleY = std::clamp(ParseFloatText(materialTilingYBox, 1.0f), 0.1f, 10.0f);
        materialDraft.normalStrength = std::clamp(ParseFloatText(materialNormalStrengthBox, 1.0f), 0.0f, 3.0f);
        materialDraft.aoStrength = materialAoStrengthSlider
            ? std::clamp(static_cast<float>(materialAoStrengthSlider->GetValue()), 0.0f, 2.0f)
            : std::clamp(materialDraft.aoStrength, 0.0f, 2.0f);
        materialDraft.roughnessStrength = materialRoughnessStrengthSlider
            ? std::clamp(static_cast<float>(materialRoughnessStrengthSlider->GetValue()), 0.0f, 2.0f)
            : std::clamp(materialDraft.roughnessStrength, 0.0f, 2.0f);
        materialDraft.metallicStrength = materialMetallicStrengthSlider
            ? std::clamp(static_cast<float>(materialMetallicStrengthSlider->GetValue()), 0.0f, 2.0f)
            : std::clamp(materialDraft.metallicStrength, 0.0f, 2.0f);
        ParseTint(GetText(materialTintBox), materialDraft.colorTint);
    }

    void OnMaterialAoStrengthChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        materialDraft.aoStrength = std::clamp(args.newValue, 0.0f, 2.0f);
        UpdateMaterialPbrStrengthText();
    }

    void OnMaterialRoughnessStrengthChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        materialDraft.roughnessStrength = std::clamp(args.newValue, 0.0f, 2.0f);
        UpdateMaterialPbrStrengthText();
    }

    void OnMaterialMetallicStrengthChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        materialDraft.metallicStrength = std::clamp(args.newValue, 0.0f, 2.0f);
        UpdateMaterialPbrStrengthText();
    }

    using SliderChangedHandler = void (Impl::*)(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>&);

    void ConfigureLightingSlider(Noesis::Slider* slider,
        float minimum,
        float maximum,
        float largeChange,
        float smallChange,
        float value,
        SliderChangedHandler handler)
    {
        if (!slider)
            return;
        slider->SetMinimum(minimum);
        slider->SetMaximum(maximum);
        slider->SetLargeChange(largeChange);
        slider->SetSmallChange(smallChange);
        slider->SetIsMoveToPointEnabled(true);
        slider->SetValue(value);
        slider->ValueChanged() += Noesis::MakeDelegate(this, handler);
    }

    void SetLightingPreset(const LightingState& preset, const char* name)
    {
        lightingState = preset;
        ApplyLightingStateToControls();
        SetAssetStatus(std::string("Lighting preset: ") + name);
    }

    void UpdateLightingText()
    {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "Azimuth: %.0f deg", lightingState.directional.azimuthDegrees);
        SetText(lightingAzimuthText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Elevation: %.0f deg", lightingState.directional.elevationDegrees);
        SetText(lightingElevationText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Sun Intensity: %.2f%s",
            lightingState.directional.intensity,
            lightingState.directional.enabled ? "" : " (OFF)");
        SetText(lightingSunIntensityText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Sun R: %.2f", lightingState.directional.r);
        SetText(lightingSunRText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Sun G: %.2f", lightingState.directional.g);
        SetText(lightingSunGText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Sun B: %.2f", lightingState.directional.b);
        SetText(lightingSunBText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Ambient Intensity: %.2f", lightingState.ambient.intensity);
        SetText(lightingAmbientIntensityText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Ambient R: %.2f", lightingState.ambient.r);
        SetText(lightingAmbientRText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Ambient G: %.2f", lightingState.ambient.g);
        SetText(lightingAmbientGText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Ambient B: %.2f", lightingState.ambient.b);
        SetText(lightingAmbientBText, buffer);
    }

    void UpdateWaterText()
    {
        char buffer[96];
        if (waterEnabledButton)
            waterEnabledButton->SetContent(waterConfig.enabled ? "Water: ON" : "Water: OFF");
        std::snprintf(buffer, sizeof(buffer), "Water Level Y: %.1f m", waterConfig.waterLevelY);
        SetText(waterLevelText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Base R: %.2f", waterConfig.baseColor[0]);
        SetText(waterBaseRText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Base G: %.2f", waterConfig.baseColor[1]);
        SetText(waterBaseGText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Base B: %.2f", waterConfig.baseColor[2]);
        SetText(waterBaseBText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Base Alpha: %.2f", waterConfig.baseColor[3]);
        SetText(waterAlphaText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Normal Tiling Fine: %.3f", waterConfig.waveScaleSmall);
        SetText(waterWaveScaleSmallText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Normal Tiling Broad: %.3f", waterConfig.waveScaleLarge);
        SetText(waterWaveScaleLargeText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Wave Speed Small: %.3f", waterConfig.waveSpeedSmall);
        SetText(waterWaveSpeedSmallText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Wave Speed Large: %.3f", waterConfig.waveSpeedLarge);
        SetText(waterWaveSpeedLargeText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Normal Strength: %.2f", waterConfig.normalStrength);
        SetText(waterNormalStrengthText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Fresnel Power: %.1f", waterConfig.fresnelPower);
        SetText(waterFresnelPowerText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Fresnel Min: %.2f", waterConfig.fresnelMin);
        SetText(waterFresnelMinText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Reflection R: %.2f", waterConfig.reflectionColor[0]);
        SetText(waterReflectionRText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Reflection G: %.2f", waterConfig.reflectionColor[1]);
        SetText(waterReflectionGText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Reflection B: %.2f", waterConfig.reflectionColor[2]);
        SetText(waterReflectionBText, buffer);
        if (waterReflectionEnabledButton)
            waterReflectionEnabledButton->SetContent(waterConfig.reflectionEnabled ? "Reflection: ON" : "Reflection: OFF");
        if (waterReflectionQuarterButton)
            waterReflectionQuarterButton->SetContent(
                waterConfig.reflectionQuality == WaterConfig::ReflectionQuality::Quarter ? "[Quarter]" : "Quarter");
        if (waterReflectionHalfButton)
            waterReflectionHalfButton->SetContent(
                waterConfig.reflectionQuality == WaterConfig::ReflectionQuality::Half ? "[Half]" : "Half");
        if (waterReflectionFullButton)
            waterReflectionFullButton->SetContent(
                waterConfig.reflectionQuality == WaterConfig::ReflectionQuality::Full ? "[Full]" : "Full");
        std::snprintf(buffer, sizeof(buffer), "Distortion Strength: %.3f", waterConfig.reflectionDistortionStrength);
        SetText(waterReflectionDistortionText, buffer);
        if (waterRefractionEnabledButton)
            waterRefractionEnabledButton->SetContent(waterConfig.refractionEnabled ? "Refraction: ON" : "Refraction: OFF");
        std::snprintf(buffer, sizeof(buffer), "Shallow R: %.2f", waterConfig.shallowColor[0]);
        SetText(waterShallowRText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Shallow G: %.2f", waterConfig.shallowColor[1]);
        SetText(waterShallowGText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Shallow B: %.2f", waterConfig.shallowColor[2]);
        SetText(waterShallowBText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Deep R: %.2f", waterConfig.deepColor[0]);
        SetText(waterDeepRText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Deep G: %.2f", waterConfig.deepColor[1]);
        SetText(waterDeepGText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Deep B: %.2f", waterConfig.deepColor[2]);
        SetText(waterDeepBText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Color Depth Min: %.1f m", waterConfig.depthColorMin);
        SetText(waterDepthColorMinText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Color Depth Max: %.1f m", waterConfig.depthColorMax);
        SetText(waterDepthColorMaxText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Fade Distance: %.1f m", waterConfig.depthFadeDistance);
        SetText(waterDepthFadeText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Refraction Strength: %.3f", waterConfig.refractionStrength);
        SetText(waterRefractionStrengthText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Refraction Depth Mult: %.2f", waterConfig.refractionDepthStrength);
        SetText(waterRefractionDepthStrengthText, buffer);
        if (waterFoamEnabledButton)
            waterFoamEnabledButton->SetContent(waterConfig.foamEnabled ? "Foam: ON" : "Foam: OFF");
        std::snprintf(buffer, sizeof(buffer), "Foam Distance: %.2f m", waterConfig.foamDistance);
        SetText(waterFoamDistanceText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Foam Intensity: %.2f", waterConfig.foamIntensity);
        SetText(waterFoamIntensityText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Foam Scale: %.2f m", waterConfig.foamScale);
        SetText(waterFoamScaleText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Terrain Foam Thickness: %.2f m", waterConfig.foamTerrainThickness);
        SetText(waterFoamTerrainThicknessText, buffer);
        if (waterCausticOffButton)
            waterCausticOffButton->SetContent(waterConfig.causticMode == WaterConfig::CausticMode::Off ? "[Off]" : "Off");
        if (waterCausticAnimatedButton)
            waterCausticAnimatedButton->SetContent(
                waterConfig.causticMode == WaterConfig::CausticMode::AnimatedTexture ? "[Animated]" : "Animated");
        if (waterCausticProceduralButton)
            waterCausticProceduralButton->SetContent(
                waterConfig.causticMode == WaterConfig::CausticMode::Procedural ? "[Procedural]" : "Procedural");
        std::snprintf(buffer, sizeof(buffer), "Caustic Intensity: %.2f", waterConfig.causticIntensity);
        SetText(waterCausticIntensityText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Caustic Scale: %.2f m", waterConfig.causticScale);
        SetText(waterCausticScaleText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Caustic Speed: %.2f", waterConfig.causticSpeed);
        SetText(waterCausticSpeedText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Caustic Max Depth: %.1f m", waterConfig.causticMaxDepth);
        SetText(waterCausticMaxDepthText, buffer);
    }

    void SetInspectorSection(Noesis::Button* button,
        Noesis::FrameworkElement* section,
        bool expanded,
        const char* label)
    {
        if (section)
            section->SetVisibility(expanded ? Noesis::Visibility_Visible : Noesis::Visibility_Collapsed);
        if (button)
        {
            const std::string content = std::string(expanded ? "[v] " : "[>] ") + label;
            button->SetContent(content.c_str());
        }
    }

    void ClampInspectorScrollOffset()
    {
        if (!inspectorScrollViewport || !inspectorScrollContent)
            return;
        const float viewportHeight = inspectorScrollViewport->GetActualHeight();
        const float contentHeight = inspectorScrollContent->GetActualHeight();
        const float scrollableHeight = std::max(0.0f, contentHeight - viewportHeight + 8.0f);
        inspectorManualScrollOffset = std::clamp(inspectorManualScrollOffset, 0.0f, scrollableHeight);
        inspectorScrollContent->SetMargin(Noesis::Thickness(0.0f, -inspectorManualScrollOffset, 6.0f, 0.0f));
    }

    void UpdateInspectorSections()
    {
        SetInspectorSection(lightingMainSectionButton, lightingMainSection, lightingMainExpanded, "Lighting");
        SetInspectorSection(waterBaseSectionButton, waterBaseSection, waterBaseExpanded, "Water");
        SetInspectorSection(waterReflectionSectionButton, waterReflectionSection, waterReflectionExpanded, "Reflection");
        SetInspectorSection(waterRefractionSectionButton, waterRefractionSection, waterRefractionExpanded, "Refraction and Depth");
        SetInspectorSection(waterFoamSectionButton, waterFoamSection, waterFoamExpanded, "Foam");
        SetInspectorSection(waterCausticSectionButton, waterCausticSection, waterCausticExpanded, "Caustic");
        SetInspectorSection(dynamicLightsSectionButton, dynamicLightsSection, dynamicLightsExpanded, "Dynamic Lights");
        SetInspectorSection(selectedLightSectionButton, selectedLightSection, selectedLightExpanded, "Selected Light");
        ClampInspectorScrollOffset();
    }

    void ApplyLightingStateToControls()
    {
        lightingControlsUpdating = true;
        if (lightingAzimuthSlider)
            lightingAzimuthSlider->SetValue(std::clamp(lightingState.directional.azimuthDegrees, 0.0f, 360.0f));
        if (lightingElevationSlider)
            lightingElevationSlider->SetValue(std::clamp(lightingState.directional.elevationDegrees, 0.0f, 90.0f));
        if (lightingSunIntensitySlider)
            lightingSunIntensitySlider->SetValue(std::clamp(lightingState.directional.intensity, 0.0f, 5.0f));
        if (lightingSunRSlider)
            lightingSunRSlider->SetValue(std::clamp(lightingState.directional.r, 0.0f, 1.0f));
        if (lightingSunGSlider)
            lightingSunGSlider->SetValue(std::clamp(lightingState.directional.g, 0.0f, 1.0f));
        if (lightingSunBSlider)
            lightingSunBSlider->SetValue(std::clamp(lightingState.directional.b, 0.0f, 1.0f));
        if (lightingAmbientIntensitySlider)
            lightingAmbientIntensitySlider->SetValue(std::clamp(lightingState.ambient.intensity, 0.0f, 3.0f));
        if (lightingAmbientRSlider)
            lightingAmbientRSlider->SetValue(std::clamp(lightingState.ambient.r, 0.0f, 1.0f));
        if (lightingAmbientGSlider)
            lightingAmbientGSlider->SetValue(std::clamp(lightingState.ambient.g, 0.0f, 1.0f));
        if (lightingAmbientBSlider)
            lightingAmbientBSlider->SetValue(std::clamp(lightingState.ambient.b, 0.0f, 1.0f));
        lightingControlsUpdating = false;
        UpdateLightingText();
        UpdateWaterText();
        UpdateInspectorSections();
    }

    void SetLightingModeActive(bool active)
    {
        lightingModeActive = active;
        if (inspectorContextPanel)
            inspectorContextPanel->SetVisibility(active ? Noesis::Visibility_Collapsed : Noesis::Visibility_Visible);
        if (lightingPanel)
            lightingPanel->SetVisibility(active ? Noesis::Visibility_Visible : Noesis::Visibility_Collapsed);
        if (active)
            UpdateInspectorSections();
    }

    void OnLightingButtonClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SetLightingModeActive(!lightingModeActive);
        SetAssetStatus(lightingModeActive ? "Lighting editor active" : "Lighting editor inactive");
    }

    void OnLightingMainSectionClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        lightingMainExpanded = !lightingMainExpanded;
        UpdateInspectorSections();
    }

    void OnWaterBaseSectionClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterBaseExpanded = !waterBaseExpanded;
        UpdateInspectorSections();
    }

    void OnWaterReflectionSectionClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterReflectionExpanded = !waterReflectionExpanded;
        UpdateInspectorSections();
    }

    void OnWaterRefractionSectionClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterRefractionExpanded = !waterRefractionExpanded;
        UpdateInspectorSections();
    }

    void OnWaterFoamSectionClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterFoamExpanded = !waterFoamExpanded;
        UpdateInspectorSections();
    }

    void OnWaterCausticSectionClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterCausticExpanded = !waterCausticExpanded;
        UpdateInspectorSections();
    }

    void OnDynamicLightsSectionClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        dynamicLightsExpanded = !dynamicLightsExpanded;
        UpdateInspectorSections();
    }

    void OnSelectedLightSectionClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        selectedLightExpanded = !selectedLightExpanded;
        UpdateInspectorSections();
    }

    void OnLightingSunEnabledClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        lightingState.directional.enabled = !lightingState.directional.enabled;
        UpdateLightingText();
        SetAssetStatus(lightingState.directional.enabled ? "Sun enabled" : "Sun disabled");
    }

    void OnSunShadowsClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        lightingState.sunShadowsEnabled = !lightingState.sunShadowsEnabled;
        SetAssetStatus(lightingState.sunShadowsEnabled ? "Sun shadows enabled" : "Sun shadows disabled");
    }

    void OnLightingResetClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SetLightingPreset(LightingState{}, "Defaults");
    }

    void OnLightingDawnClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        LightingState preset{};
        preset.directional.azimuthDegrees = 90.0f;
        preset.directional.elevationDegrees = 12.0f;
        preset.directional.intensity = 0.9f;
        preset.directional.r = 1.0f;
        preset.directional.g = 0.6f;
        preset.directional.b = 0.35f;
        preset.ambient.r = 0.35f;
        preset.ambient.g = 0.30f;
        preset.ambient.b = 0.40f;
        preset.ambient.intensity = 0.6f;
        SetLightingPreset(preset, "Dawn");
    }

    void OnLightingNoonClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        LightingState preset{};
        preset.directional.azimuthDegrees = 180.0f;
        preset.directional.elevationDegrees = 75.0f;
        preset.directional.intensity = 1.2f;
        preset.directional.r = 1.0f;
        preset.directional.g = 0.97f;
        preset.directional.b = 0.92f;
        preset.ambient.r = 0.40f;
        preset.ambient.g = 0.45f;
        preset.ambient.b = 0.55f;
        preset.ambient.intensity = 0.8f;
        SetLightingPreset(preset, "Noon");
    }

    void OnLightingDuskClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        LightingState preset{};
        preset.directional.azimuthDegrees = 270.0f;
        preset.directional.elevationDegrees = 8.0f;
        preset.directional.intensity = 0.7f;
        preset.directional.r = 1.0f;
        preset.directional.g = 0.5f;
        preset.directional.b = 0.25f;
        preset.ambient.r = 0.45f;
        preset.ambient.g = 0.30f;
        preset.ambient.b = 0.25f;
        preset.ambient.intensity = 0.7f;
        SetLightingPreset(preset, "Dusk");
    }

    void OnLightingNightClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        LightingState preset{};
        preset.directional.azimuthDegrees = 180.0f;
        preset.directional.elevationDegrees = 45.0f;
        preset.directional.intensity = 0.20f;
        preset.directional.r = 0.30f;
        preset.directional.g = 0.40f;
        preset.directional.b = 0.65f;
        preset.ambient.r = 0.05f;
        preset.ambient.g = 0.07f;
        preset.ambient.b = 0.15f;
        preset.ambient.intensity = 0.4f;
        SetLightingPreset(preset, "Night");
    }

    void OnLightingAzimuthChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.directional.azimuthDegrees = std::clamp(args.newValue, 0.0f, 360.0f);
        UpdateLightingText();
    }

    void OnLightingElevationChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.directional.elevationDegrees = std::clamp(args.newValue, 0.0f, 90.0f);
        UpdateLightingText();
    }

    void OnLightingSunIntensityChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.directional.intensity = std::clamp(args.newValue, 0.0f, 5.0f);
        UpdateLightingText();
    }

    void OnLightingSunRChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.directional.r = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateLightingText();
    }

    void OnLightingSunGChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.directional.g = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateLightingText();
    }

    void OnLightingSunBChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.directional.b = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateLightingText();
    }

    void OnLightingAmbientIntensityChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.ambient.intensity = std::clamp(args.newValue, 0.0f, 3.0f);
        UpdateLightingText();
    }

    void OnLightingAmbientRChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.ambient.r = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateLightingText();
    }

    void OnLightingAmbientGChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.ambient.g = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateLightingText();
    }

    void OnLightingAmbientBChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (!lightingControlsUpdating)
            lightingState.ambient.b = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateLightingText();
    }

    void OnWaterEnabledClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.enabled = !waterConfig.enabled;
        UpdateWaterText();
        SetAssetStatus(waterConfig.enabled ? "Water enabled" : "Water disabled");
    }

    void OnWaterLevelChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.waterLevelY = std::clamp(args.newValue, -50.0f, 50.0f);
        UpdateWaterText();
    }

    void OnWaterBaseRChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.baseColor[0] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterBaseGChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.baseColor[1] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterBaseBChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.baseColor[2] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterAlphaChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.baseColor[3] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterWaveScaleSmallChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.waveScaleSmall = std::clamp(args.newValue, 0.001f, 0.12f);
        UpdateWaterText();
    }

    void OnWaterWaveScaleLargeChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.waveScaleLarge = std::clamp(args.newValue, 0.001f, 0.08f);
        UpdateWaterText();
    }

    void OnWaterWaveSpeedSmallChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.waveSpeedSmall = std::clamp(args.newValue, 0.0f, 0.5f);
        UpdateWaterText();
    }

    void OnWaterWaveSpeedLargeChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.waveSpeedLarge = std::clamp(args.newValue, 0.0f, 0.5f);
        UpdateWaterText();
    }

    void OnWaterNormalStrengthChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.normalStrength = std::clamp(args.newValue, 0.0f, 2.0f);
        UpdateWaterText();
    }

    void OnWaterFresnelPowerChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.fresnelPower = std::clamp(args.newValue, 1.0f, 10.0f);
        UpdateWaterText();
    }

    void OnWaterFresnelMinChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.fresnelMin = std::clamp(args.newValue, 0.0f, 0.5f);
        UpdateWaterText();
    }

    void OnWaterReflectionRChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.reflectionColor[0] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterReflectionGChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.reflectionColor[1] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterReflectionBChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.reflectionColor[2] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterReflectionEnabledClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.reflectionEnabled = !waterConfig.reflectionEnabled;
        UpdateWaterText();
        SetAssetStatus(waterConfig.reflectionEnabled ? "Water reflection enabled" : "Water reflection disabled");
    }

    void OnWaterReflectionQuarterClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.reflectionQuality = WaterConfig::ReflectionQuality::Quarter;
        UpdateWaterText();
        SetAssetStatus("Water reflection quality: Quarter");
    }

    void OnWaterReflectionHalfClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.reflectionQuality = WaterConfig::ReflectionQuality::Half;
        UpdateWaterText();
        SetAssetStatus("Water reflection quality: Half");
    }

    void OnWaterReflectionFullClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.reflectionQuality = WaterConfig::ReflectionQuality::Full;
        UpdateWaterText();
        SetAssetStatus("Water reflection quality: Full");
    }

    void OnWaterReflectionDistortionChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.reflectionDistortionStrength = std::clamp(args.newValue, 0.0f, 0.2f);
        UpdateWaterText();
    }

    void OnWaterRefractionEnabledClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.refractionEnabled = !waterConfig.refractionEnabled;
        UpdateWaterText();
        SetAssetStatus(waterConfig.refractionEnabled ? "Water refraction enabled" : "Water refraction disabled");
    }

    void OnWaterShallowRChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.shallowColor[0] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterShallowGChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.shallowColor[1] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterShallowBChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.shallowColor[2] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterDeepRChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.deepColor[0] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterDeepGChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.deepColor[1] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterDeepBChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.deepColor[2] = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterDepthColorMinChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.depthColorMin = std::clamp(args.newValue, 0.0f, 50.0f);
        waterConfig.depthColorMax = std::max(waterConfig.depthColorMax, waterConfig.depthColorMin + 0.001f);
        UpdateWaterText();
    }

    void OnWaterDepthColorMaxChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.depthColorMax = std::max(waterConfig.depthColorMin + 0.001f, std::clamp(args.newValue, 0.01f, 50.0f));
        UpdateWaterText();
    }

    void OnWaterDepthFadeChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.depthFadeDistance = std::clamp(args.newValue, 0.01f, 50.0f);
        UpdateWaterText();
    }

    void OnWaterRefractionStrengthChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.refractionStrength = std::clamp(args.newValue, 0.0f, 0.1f);
        UpdateWaterText();
    }

    void OnWaterRefractionDepthStrengthChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.refractionDepthStrength = std::clamp(args.newValue, 0.0f, 2.0f);
        UpdateWaterText();
    }

    void OnWaterFoamEnabledClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.foamEnabled = !waterConfig.foamEnabled;
        UpdateWaterText();
    }

    void OnWaterFoamDistanceChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.foamDistance = std::clamp(args.newValue, 0.02f, 1.5f);
        UpdateWaterText();
    }

    void OnWaterFoamIntensityChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.foamIntensity = std::clamp(args.newValue, 0.0f, 2.0f);
        UpdateWaterText();
    }

    void OnWaterFoamScaleChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.foamScale = std::clamp(args.newValue, 0.1f, 2.0f);
        UpdateWaterText();
    }

    void OnWaterFoamTerrainThicknessChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.foamTerrainThickness = std::clamp(args.newValue, 0.0f, 1.0f);
        UpdateWaterText();
    }

    void OnWaterCausticOffClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.causticMode = WaterConfig::CausticMode::Off;
        UpdateWaterText();
    }

    void OnWaterCausticAnimatedClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.causticMode = WaterConfig::CausticMode::AnimatedTexture;
        UpdateWaterText();
    }

    void OnWaterCausticProceduralClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        waterConfig.causticMode = WaterConfig::CausticMode::Procedural;
        UpdateWaterText();
    }

    void OnWaterCausticIntensityChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.causticIntensity = std::clamp(args.newValue, 0.0f, 3.0f);
        UpdateWaterText();
    }

    void OnWaterCausticScaleChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.causticScale = std::clamp(args.newValue, 0.1f, 2.0f);
        UpdateWaterText();
    }

    void OnWaterCausticSpeedChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.causticSpeed = std::clamp(args.newValue, 0.0f, 2.0f);
        UpdateWaterText();
    }

    void OnWaterCausticMaxDepthChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        waterConfig.causticMaxDepth = std::clamp(args.newValue, 1.0f, 30.0f);
        UpdateWaterText();
    }

    static float ToDegrees(float radians)
    {
        return radians * 180.0f / 3.1415926535f;
    }

    static float ToRadians(float degrees)
    {
        return degrees * 3.1415926535f / 180.0f;
    }

    void MarkSelectedLightChanged()
    {
        if (dynamicLightControlsUpdating || dynamicLightEditorState.type == DynamicLightType::None)
            return;
        editorCommands.selectedLightChanged = true;
        editorCommands.selectedLight = dynamicLightEditorState;
    }

    void UpdateDynamicLightText()
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "Active: %u / %u",
            dynamicLightEditorState.pointCount + dynamicLightEditorState.spotCount,
            kMaxDynamicPointLights);
        SetText(dynamicLightCountText, buffer);

        if (dynamicLightEditorState.type == DynamicLightType::Point)
        {
            const PointLight& point = dynamicLightEditorState.point;
            std::snprintf(buffer, sizeof(buffer), "POINT LIGHT #%u", point.id);
            SetText(selectedLightTitleText, buffer);
            std::snprintf(buffer, sizeof(buffer), "X: %.1f", point.position[0]);
            SetText(selectedLightXText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Y: %.1f", point.position[1]);
            SetText(selectedLightYText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Z: %.1f", point.position[2]);
            SetText(selectedLightZText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Intensity: %.1f%s", point.intensity, point.enabled ? "" : " (OFF)");
            SetText(selectedLightIntensityText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Radius: %.1f m", point.radius);
            SetText(selectedLightRadiusText, buffer);
            std::snprintf(buffer, sizeof(buffer), "R: %.2f", point.r);
            SetText(selectedLightRText, buffer);
            std::snprintf(buffer, sizeof(buffer), "G: %.2f", point.g);
            SetText(selectedLightGText, buffer);
            std::snprintf(buffer, sizeof(buffer), "B: %.2f", point.b);
            SetText(selectedLightBText, buffer);
            SetText(selectedSpotPitchText, "Pitch: point light");
            SetText(selectedSpotYawText, "Yaw: point light");
            SetText(selectedSpotInnerText, "Inner Cone: point light");
            SetText(selectedSpotOuterText, "Outer Cone: point light");
            return;
        }

        if (dynamicLightEditorState.type == DynamicLightType::Spot)
        {
            const SpotLight& spot = dynamicLightEditorState.spot;
            std::snprintf(buffer, sizeof(buffer), "SPOT LIGHT #%u", spot.id);
            SetText(selectedLightTitleText, buffer);
            std::snprintf(buffer, sizeof(buffer), "X: %.1f", spot.position[0]);
            SetText(selectedLightXText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Y: %.1f", spot.position[1]);
            SetText(selectedLightYText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Z: %.1f", spot.position[2]);
            SetText(selectedLightZText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Intensity: %.1f%s", spot.intensity, spot.enabled ? "" : " (OFF)");
            SetText(selectedLightIntensityText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Radius: %.1f m", spot.radius);
            SetText(selectedLightRadiusText, buffer);
            std::snprintf(buffer, sizeof(buffer), "R: %.2f", spot.r);
            SetText(selectedLightRText, buffer);
            std::snprintf(buffer, sizeof(buffer), "G: %.2f", spot.g);
            SetText(selectedLightGText, buffer);
            std::snprintf(buffer, sizeof(buffer), "B: %.2f", spot.b);
            SetText(selectedLightBText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Pitch: %.0f deg", ToDegrees(spot.rotation[0]));
            SetText(selectedSpotPitchText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Yaw: %.0f deg", ToDegrees(spot.rotation[1]));
            SetText(selectedSpotYawText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Inner Cone: %.0f deg", spot.innerConeDegrees);
            SetText(selectedSpotInnerText, buffer);
            std::snprintf(buffer, sizeof(buffer), "Outer Cone: %.0f deg", spot.outerConeDegrees);
            SetText(selectedSpotOuterText, buffer);
            return;
        }

        SetText(selectedLightTitleText, "No light selected");
        SetText(selectedLightXText, "X: 0.0");
        SetText(selectedLightYText, "Y: 0.0");
        SetText(selectedLightZText, "Z: 0.0");
        SetText(selectedLightIntensityText, "Intensity: 0.0");
        SetText(selectedLightRadiusText, "Radius: 0.0 m");
        SetText(selectedLightRText, "R: 0.00");
        SetText(selectedLightGText, "G: 0.00");
        SetText(selectedLightBText, "B: 0.00");
        SetText(selectedSpotPitchText, "Pitch: none");
        SetText(selectedSpotYawText, "Yaw: none");
        SetText(selectedSpotInnerText, "Inner Cone: none");
        SetText(selectedSpotOuterText, "Outer Cone: none");
    }

    void ApplyDynamicLightEditorStateToControls()
    {
        dynamicLightControlsUpdating = true;
        const bool isPoint = dynamicLightEditorState.type == DynamicLightType::Point;
        const bool isSpot = dynamicLightEditorState.type == DynamicLightType::Spot;
        const float* position = isPoint ? dynamicLightEditorState.point.position :
            (isSpot ? dynamicLightEditorState.spot.position : nullptr);
        if (selectedLightXSlider)
            selectedLightXSlider->SetValue(position ? std::clamp(position[0], -200.0f, 200.0f) : 0.0f);
        if (selectedLightYSlider)
            selectedLightYSlider->SetValue(position ? std::clamp(position[1], -50.0f, 100.0f) : 0.0f);
        if (selectedLightZSlider)
            selectedLightZSlider->SetValue(position ? std::clamp(position[2], -200.0f, 200.0f) : 0.0f);
        if (selectedLightIntensitySlider)
            selectedLightIntensitySlider->SetValue(isPoint ? dynamicLightEditorState.point.intensity :
                (isSpot ? dynamicLightEditorState.spot.intensity : 0.0f));
        if (selectedLightRadiusSlider)
            selectedLightRadiusSlider->SetValue(isPoint ? dynamicLightEditorState.point.radius :
                (isSpot ? dynamicLightEditorState.spot.radius : 0.1f));
        if (selectedLightRSlider)
            selectedLightRSlider->SetValue(isPoint ? dynamicLightEditorState.point.r :
                (isSpot ? dynamicLightEditorState.spot.r : 0.0f));
        if (selectedLightGSlider)
            selectedLightGSlider->SetValue(isPoint ? dynamicLightEditorState.point.g :
                (isSpot ? dynamicLightEditorState.spot.g : 0.0f));
        if (selectedLightBSlider)
            selectedLightBSlider->SetValue(isPoint ? dynamicLightEditorState.point.b :
                (isSpot ? dynamicLightEditorState.spot.b : 0.0f));
        if (selectedSpotPitchSlider)
            selectedSpotPitchSlider->SetValue(isSpot ? std::clamp(ToDegrees(dynamicLightEditorState.spot.rotation[0]), -90.0f, 90.0f) : -90.0f);
        if (selectedSpotYawSlider)
            selectedSpotYawSlider->SetValue(isSpot ? std::clamp(ToDegrees(dynamicLightEditorState.spot.rotation[1]), -180.0f, 180.0f) : 0.0f);
        if (selectedSpotInnerSlider)
            selectedSpotInnerSlider->SetValue(isSpot ? dynamicLightEditorState.spot.innerConeDegrees : 20.0f);
        if (selectedSpotOuterSlider)
            selectedSpotOuterSlider->SetValue(isSpot ? dynamicLightEditorState.spot.outerConeDegrees : 35.0f);
        if (addPointLightButton)
            addPointLightButton->SetIsEnabled(dynamicLightEditorState.pointCount < kMaxDynamicPointLights);
        if (addSpotLightButton)
            addSpotLightButton->SetIsEnabled(dynamicLightEditorState.spotCount < kMaxDynamicSpotLights);
        dynamicLightControlsUpdating = false;
        UpdateDynamicLightText();
    }

    void SetDynamicLightEditorState(const DynamicLightEditorState& state)
    {
        dynamicLightEditorState = state;
        if (state.type == DynamicLightType::Point || state.type == DynamicLightType::Spot)
            SetLightingModeActive(true);
        ApplyDynamicLightEditorStateToControls();
    }

    void OnAddPointLightClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        editorCommands.addPointLight = true;
        SetLightingModeActive(true);
        SetAssetStatus("Adding point light");
    }

    void OnAddSpotLightClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        editorCommands.addSpotLight = true;
        SetLightingModeActive(true);
        SetAssetStatus("Adding spot light");
    }

    void OnDeleteLightClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        editorCommands.deleteSelectedLight = true;
        SetAssetStatus("Deleting selected light");
    }

    void OnSelectedLightEnabledClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        if (dynamicLightEditorState.type == DynamicLightType::Point)
            dynamicLightEditorState.point.enabled = !dynamicLightEditorState.point.enabled;
        else if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.enabled = !dynamicLightEditorState.spot.enabled;
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedLightXChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Point)
            dynamicLightEditorState.point.position[0] = args.newValue;
        else if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.position[0] = args.newValue;
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedLightYChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Point)
            dynamicLightEditorState.point.position[1] = args.newValue;
        else if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.position[1] = args.newValue;
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedLightZChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Point)
            dynamicLightEditorState.point.position[2] = args.newValue;
        else if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.position[2] = args.newValue;
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedLightIntensityChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Point)
            dynamicLightEditorState.point.intensity = std::clamp(args.newValue, 0.0f, 10.0f);
        else if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.intensity = std::clamp(args.newValue, 0.0f, 10.0f);
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedLightRadiusChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Point)
            dynamicLightEditorState.point.radius = std::clamp(args.newValue, 0.1f, 100.0f);
        else if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.radius = std::clamp(args.newValue, 0.1f, 100.0f);
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedLightRChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Point)
            dynamicLightEditorState.point.r = std::clamp(args.newValue, 0.0f, 1.0f);
        else if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.r = std::clamp(args.newValue, 0.0f, 1.0f);
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedLightGChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Point)
            dynamicLightEditorState.point.g = std::clamp(args.newValue, 0.0f, 1.0f);
        else if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.g = std::clamp(args.newValue, 0.0f, 1.0f);
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedLightBChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Point)
            dynamicLightEditorState.point.b = std::clamp(args.newValue, 0.0f, 1.0f);
        else if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.b = std::clamp(args.newValue, 0.0f, 1.0f);
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedSpotPitchChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.rotation[0] = ToRadians(std::clamp(args.newValue, -90.0f, 90.0f));
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedSpotYawChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Spot)
            dynamicLightEditorState.spot.rotation[1] = ToRadians(std::clamp(args.newValue, -180.0f, 180.0f));
        MarkSelectedLightChanged();
        UpdateDynamicLightText();
    }

    void OnSelectedSpotInnerChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Spot)
        {
            dynamicLightEditorState.spot.innerConeDegrees = std::clamp(args.newValue, 1.0f, 89.0f);
            dynamicLightEditorState.spot.outerConeDegrees =
                std::max(dynamicLightEditorState.spot.outerConeDegrees, dynamicLightEditorState.spot.innerConeDegrees);
        }
        MarkSelectedLightChanged();
        ApplyDynamicLightEditorStateToControls();
    }

    void OnSelectedSpotOuterChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        if (dynamicLightControlsUpdating)
            return;
        if (dynamicLightEditorState.type == DynamicLightType::Spot)
        {
            dynamicLightEditorState.spot.outerConeDegrees = std::clamp(args.newValue, 1.0f, 90.0f);
            dynamicLightEditorState.spot.innerConeDegrees =
                std::min(dynamicLightEditorState.spot.innerConeDegrees, dynamicLightEditorState.spot.outerConeDegrees);
        }
        MarkSelectedLightChanged();
        ApplyDynamicLightEditorStateToControls();
    }

    void UpdateBrushValueText()
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "Radius: %.1f m", editorBrushRadiusMeters);
        SetText(editorRadiusText, buffer);
        std::snprintf(buffer, sizeof(buffer), "Strength: %.1f", editorBrushStrength);
        SetText(editorStrengthText, buffer);
    }

    void ApplyBrushSliderState(float radiusMeters, float strength)
    {
        editorBrushRadiusMeters = std::clamp(radiusMeters, 0.5f, 50.0f);
        editorBrushStrength = std::clamp(strength, 0.1f, 5.0f);
        UpdateBrushValueText();
    }

    bool UpdateBrushSliderFromPointer(Noesis::Slider* slider, int x, int y, float& valueOut, bool requireHit)
    {
        if (!slider)
            return false;

        const float width = slider->GetActualWidth();
        const float height = slider->GetActualHeight();
        if (width <= 1.0f || height <= 1.0f)
            return false;

        const Noesis::Point local = slider->PointFromScreen(
            Noesis::Point(static_cast<float>(x), static_cast<float>(y)));
        if (requireHit)
        {
            const float hitPadding = 8.0f;
            if (local.x < -hitPadding || local.x > width + hitPadding ||
                local.y < -hitPadding || local.y > height + hitPadding)
            {
                return false;
            }
        }

        const float t = std::clamp(local.x / width, 0.0f, 1.0f);
        valueOut = slider->GetMinimum() + (slider->GetMaximum() - slider->GetMinimum()) * t;
        slider->SetValue(valueOut);
        return true;
    }

    static bool HitElement(Noesis::FrameworkElement* element, int x, int y, float padding = 2.0f)
    {
        if (!element || element->GetVisibility() != Noesis::Visibility_Visible)
            return false;
        const float width = element->GetActualWidth();
        const float height = element->GetActualHeight();
        if (width <= 0.0f || height <= 0.0f)
            return false;
        const Noesis::Point local = element->PointFromScreen(
            Noesis::Point(static_cast<float>(x), static_cast<float>(y)));
        return local.x >= -padding && local.x <= width + padding &&
               local.y >= -padding && local.y <= height + padding;
    }

    bool HandleInspectorScrollInput(const InputEvent& event)
    {
        if (!mapEditorOpen || event.type != InputEvent::MouseWheel || !inspectorScrollViewport || !inspectorScrollContent)
            return false;
        if (!HitElement(inspectorScrollViewport, event.x, event.y, 0.0f))
            return false;

        const float viewportHeight = inspectorScrollViewport->GetActualHeight();
        const float contentHeight = inspectorScrollContent->GetActualHeight();
        const float scrollableHeight = std::max(0.0f, contentHeight - viewportHeight + 8.0f);
        if (scrollableHeight <= 0.0f)
            return true;

        const float pixels = -static_cast<float>(event.wheelDelta) * 0.5f;
        inspectorManualScrollOffset = std::clamp(inspectorManualScrollOffset + pixels, 0.0f, scrollableHeight);
        inspectorScrollContent->SetMargin(Noesis::Thickness(0.0f, -inspectorManualScrollOffset, 6.0f, 0.0f));
        return true;
    }

    void ReleaseCachedNoesisReferences()
    {
        ClearDragHighlight();
        dragPreviousBorderBrush.Reset();
        renameTextBox.Reset();
        renameOriginalContent.Reset();
        renameAssetButton = nullptr;
        renameFolderItem = nullptr;
        assetFolderItems.clear();
        assetBreadcrumbButtons.clear();
        assetBreadcrumbSeparators.clear();
        assetFolderItemPaths.clear();
        assetBreadcrumbButtonPaths.clear();
        lobbyViewModel.Reset();
    }

    void ClearDragHighlight()
    {
        if (dragHighlightElement)
        {
            if (Noesis::Control* control = Noesis::DynamicCast<Noesis::Control*>(dragHighlightElement))
            {
                control->SetBorderBrush(dragPreviousBorderBrush.GetPtr());
                control->SetBorderThickness(dragPreviousBorderThickness);
            }
            dragHighlightElement->SetOpacity(1.0f);
            dragHighlightElement = nullptr;
        }
        dragPreviousBorderBrush.Reset();
    }

    void SetDragHighlight(Noesis::FrameworkElement* element, bool valid)
    {
        if (dragHighlightElement != element)
        {
            ClearDragHighlight();
            if (Noesis::Control* control = Noesis::DynamicCast<Noesis::Control*>(element))
            {
                dragPreviousBorderBrush.Reset(control->GetBorderBrush());
                dragPreviousBorderThickness = control->GetBorderThickness();
            }
        }
        dragHighlightElement = element;
        if (dragHighlightElement)
        {
            if (Noesis::Control* control = Noesis::DynamicCast<Noesis::Control*>(dragHighlightElement))
            {
                const Noesis::Color color = valid
                    ? Noesis::Color(88, 235, 141, 220)
                    : Noesis::Color(255, 96, 96, 230);
                control->SetBorderBrush(Noesis::MakePtr<Noesis::SolidColorBrush>(color).GetPtr());
                control->SetBorderThickness(Noesis::Thickness(2.0f));
            }
            dragHighlightElement->SetOpacity(valid ? 1.0f : 0.45f);
        }
    }

    DragPayload PayloadForEntry(const AssetLibrary::Entry& entry) const
    {
        DragPayload payload;
        payload.assetId = entry.id;
        payload.textureRole = entry.textureRole;
        switch (entry.category)
        {
        case AssetLibrary::Category::Texture: payload.type = DragPayloadType::AssetTexture; break;
        case AssetLibrary::Category::Material: payload.type = DragPayloadType::AssetMaterial; break;
        case AssetLibrary::Category::Model: payload.type = DragPayloadType::AssetModel; break;
        case AssetLibrary::Category::Animation: payload.type = DragPayloadType::AssetAnimation; break;
        default: payload.type = DragPayloadType::None; break;
        }
        return payload;
    }

    bool AssignTextureToMaterialSlot(uint32_t slot, const AssetLibrary::Entry& entry)
    {
        if (entry.category != AssetLibrary::Category::Texture || slot > 5)
            return false;
        if (!TextureRoleMatchesMaterialSlot(entry.textureRole, slot))
        {
            SetAssetStatus(WrongTextureRoleMessage(entry, slot));
            return false;
        }

        activeMaterialTextureSlot = slot;
        if (slot == 0)
            materialDraft.diffuseTextureId = entry.id;
        else if (slot == 1)
            materialDraft.normalTextureId = entry.id;
        else if (slot == 2)
            materialDraft.aoTextureId = entry.id;
        else if (slot == 3)
            materialDraft.roughnessTextureId = entry.id;
        else if (slot == 4)
            materialDraft.metallicTextureId = entry.id;
        else if (slot == 5)
            materialDraft.heightTextureId = entry.id;
        UpdateMaterialEditorText();
        SetAssetStatus("Material " + MaterialSlotName(slot) + " <- " + entry.displayName);
        return true;
    }

    std::optional<uint32_t> HitMaterialSlot(int x, int y) const
    {
        for (uint32_t i = 0; i < materialSlotButtons.size(); ++i)
        {
            if (HitElement(materialSlotButtons[i], x, y, 4.0f))
                return i;
        }
        return std::nullopt;
    }

    std::optional<uint32_t> HitPaintSlot(int x, int y) const
    {
        for (uint32_t i = 0; i < editorTextureButtons.size(); ++i)
        {
            if (HitElement(editorTextureButtons[i], x, y, 4.0f))
                return i;
        }
        return std::nullopt;
    }

    std::optional<std::string> HitFolderItem(int x, int y) const
    {
        for (const Noesis::Ptr<Noesis::TreeViewItem>& item : assetFolderItems)
        {
            if (!item || !HitElement(item.GetPtr(), x, y, 4.0f))
                continue;
            auto it = assetFolderItemPaths.find(item.GetPtr());
            if (it != assetFolderItemPaths.end())
                return it->second;
        }
        return std::nullopt;
    }

    Noesis::TreeViewItem* HitFolderTreeItem(int x, int y, std::string& path) const
    {
        for (const Noesis::Ptr<Noesis::TreeViewItem>& item : assetFolderItems)
        {
            if (!item || !HitElement(item.GetPtr(), x, y, 4.0f))
                continue;
            auto it = assetFolderItemPaths.find(item.GetPtr());
            if (it == assetFolderItemPaths.end())
                continue;
            path = it->second;
            return item.GetPtr();
        }
        return nullptr;
    }

    Noesis::Style* EditorTextBoxStyle() const
    {
        if (!editorView || !editorView->GetContent())
            return nullptr;
        return editorView->GetContent()->FindResource<Noesis::Style>("EditorTextBox");
    }

    void ResetRenameRoleConfirm()
    {
        renameRoleConfirmText.clear();
    }

    void SetRenameBorder(const Noesis::Color& color)
    {
        if (!renameTextBox)
            return;
        renameTextBox->SetBorderBrush(Noesis::MakePtr<Noesis::SolidColorBrush>(color).GetPtr());
        renameTextBox->SetBorderThickness(Noesis::Thickness(2.0f));
    }

    bool ValidateRenameText(std::string& error) const
    {
        if (!AssetLibrary::IsValidRenameName(renameEditText, &error))
            return false;

        if (!assetLibrary)
            return true;

        if (renameTargetType == RenameTargetType::Asset)
        {
            auto entry = assetLibrary->FindById(renameTargetId);
            if (!entry)
            {
                error = "asset not found";
                return false;
            }
            std::string baseName = renameEditText;
            const std::string oldExtension = std::filesystem::path(entry->filename).extension().string();
            std::filesystem::path typedName(baseName);
            if (!oldExtension.empty() && typedName.extension().string() == oldExtension)
                baseName = typedName.stem().string();
            const std::string candidate = baseName + oldExtension;
            for (const AssetLibrary::Entry& other : assetLibrary->Entries())
            {
                if (other.id == entry->id ||
                    other.category != entry->category ||
                    AssetLibrary::NormalizeSubpath(other.subpath) != AssetLibrary::NormalizeSubpath(entry->subpath))
                {
                    continue;
                }
                std::string a = other.filename;
                std::string b = candidate;
                std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (a == b)
                {
                    error = "name already used in this folder";
                    return false;
                }
            }
        }
        else if (renameTargetType == RenameTargetType::Folder)
        {
            const std::string oldPath = AssetFolderQuerySubpath(renameTargetId);
            const std::string parent = ParentAssetFolderPath(oldPath);
            const std::string normalizedName = AssetLibrary::NormalizeSubpath(renameEditText);
            if (normalizedName.empty() || normalizedName.find('/') != std::string::npos)
            {
                error = "invalid folder name";
                return false;
            }
            const std::string target = parent.empty() ? normalizedName : parent + "/" + normalizedName;
            for (const AssetLibrary::Entry& entry : assetLibrary->Entries())
            {
                if (entry.category == assetCategory &&
                    AssetFolderContains(AssetLibrary::NormalizeSubpath(entry.subpath), target) &&
                    !AssetFolderContains(AssetLibrary::NormalizeSubpath(entry.subpath), oldPath))
                {
                    error = "folder name already exists";
                    return false;
                }
            }
        }

        return true;
    }

    void UpdateRenameVisual()
    {
        if (!renameTextBox)
            return;
        renameTextBox->SetText(renameEditText.c_str());

        std::string error;
        if (renameEditText.empty())
        {
            SetRenameBorder(Noesis::Color(110, 122, 140, 220));
            return;
        }
        if (!ValidateRenameText(error))
        {
            const bool collision = error.find("used") != std::string::npos ||
                error.find("exists") != std::string::npos;
            SetRenameBorder(collision ? Noesis::Color(245, 166, 72, 230) : Noesis::Color(255, 96, 96, 230));
            SetAssetStatus("Rename: " + error);
            return;
        }
        SetRenameBorder(Noesis::Color(88, 235, 141, 220));
    }

    void RestoreRenameUi()
    {
        if (renameTargetType == RenameTargetType::Asset && renameAssetButton)
            renameAssetButton->SetContent(renameOriginalContent.GetPtr());
        else if (renameTargetType == RenameTargetType::Folder && renameFolderItem)
            renameFolderItem->SetHeader(renameOriginalName.c_str());

        renameTextBox.Reset();
        renameOriginalContent.Reset();
        renameAssetButton = nullptr;
        renameFolderItem = nullptr;
        renameAssetIndex = 0;
        renameTargetId.clear();
        renameOriginalName.clear();
        renameEditText.clear();
        renameRoleConfirmText.clear();
        renameTargetType = RenameTargetType::None;
    }

    void CancelRename()
    {
        if (renameTargetType == RenameTargetType::None)
            return;
        RestoreRenameUi();
        ClearKeyboardFocus();
        SetAssetStatus("Rename cancelled");
    }

    void BeginAssetRename(uint32_t index)
    {
        if (index >= visibleAssetEntries.size() || index >= assetButtons.size() || !assetButtons[index])
            return;

        RestoreRenameUi();
        const AssetLibrary::Entry& entry = visibleAssetEntries[index];
        selectedAssetId = entry.id;
        renameTargetType = RenameTargetType::Asset;
        renameTargetId = entry.id;
        renameOriginalName = entry.displayName;
        renameEditText = entry.displayName;
        renameAssetIndex = index;
        renameAssetButton = assetButtons[index];
        renameOriginalContent.Reset(renameAssetButton->GetContent());
        renameTextBox = Noesis::MakePtr<Noesis::TextBox>();
        renameTextBox->SetText(renameEditText.c_str());
        renameTextBox->SetMinWidth(120.0f);
        renameTextBox->SetHeight(34.0f);
        if (Noesis::Style* style = EditorTextBoxStyle())
            renameTextBox->SetStyle(style);
        renameAssetButton->SetContent(renameTextBox.GetPtr());
        renameTextBox->Focus();
        renameTextBox->SelectAll();
        UpdateRenameVisual();
        SetAssetStatus("Rename asset: Enter saves, Escape cancels");
    }

    void BeginFolderRename(const std::string& path, Noesis::TreeViewItem* item)
    {
        if (!item || IsAllAssetFolderPath(path) || IsRootAssetFolderPath(path))
        {
            SetAssetStatus("Root and All cannot be renamed");
            return;
        }

        RestoreRenameUi();
        selectedAssetFolderPath = path;
        renameTargetType = RenameTargetType::Folder;
        renameTargetId = path;
        renameOriginalName = AssetFolderDisplayName(path);
        renameEditText = renameOriginalName;
        renameFolderItem = item;
        renameTextBox = Noesis::MakePtr<Noesis::TextBox>();
        renameTextBox->SetText(renameEditText.c_str());
        renameTextBox->SetMinWidth(120.0f);
        renameTextBox->SetHeight(30.0f);
        if (Noesis::Style* style = EditorTextBoxStyle())
            renameTextBox->SetStyle(style);
        item->SetHeader(renameTextBox.GetPtr());
        renameTextBox->Focus();
        renameTextBox->SelectAll();
        UpdateRenameVisual();
        SetAssetStatus("Rename folder: Enter saves, Escape cancels");
    }

    bool CommitRename()
    {
        if (renameTargetType == RenameTargetType::None || !EnsureAssetLibrary())
            return false;

        std::string validationError;
        if (!ValidateRenameText(validationError))
        {
            SetAssetStatus("Rename failed: " + validationError);
            UpdateRenameVisual();
            return true;
        }

        if (renameTargetType == RenameTargetType::Asset)
        {
            AssetLibrary::Entry renamed{};
            std::string error;
            const bool allowRoleChange = renameRoleConfirmText == renameEditText;
            if (!assetLibrary->RenameAsset(renameTargetId, renameEditText, allowRoleChange, renamed, error))
            {
                if (error.find("role suffix suggests") != std::string::npos)
                    renameRoleConfirmText = renameEditText;
                SetAssetStatus("Rename failed: " + error);
                UpdateRenameVisual();
                return true;
            }

            RestoreRenameUi();
            selectedAssetId = renamed.id;
            if (editingMaterialId && *editingMaterialId == renamed.id)
                SetText(assetNameBox, renamed.displayName);
            if (renamed.id == selectedAssetId)
            {
                SetText(assetNameBox, renamed.displayName);
                SetText(assetSubpathBox, renamed.subpath);
                SetText(assetTagsBox, AssetLibrary::TagsToCsv(renamed.tags));
            }
            RefreshPaletteSlotsReferencingAsset(renamed);
            RefreshAssetBrowser();
            ClearKeyboardFocus();
            SetAssetStatus("Renamed asset to " + renamed.displayName);
            return true;
        }

        if (renameTargetType == RenameTargetType::Folder)
        {
            const std::string oldPath = renameTargetId;
            std::string newPath;
            std::string error;
            if (!assetLibrary->RenameFolder(assetCategory, AssetFolderQuerySubpath(oldPath), renameEditText, newPath, error))
            {
                SetAssetStatus("Folder rename failed: " + error);
                UpdateRenameVisual();
                return true;
            }

            RestoreRenameUi();
            if (AssetFolderContains(activeAssetSubpath, oldPath))
                activeAssetSubpath = ReplaceAssetFolderPrefix(activeAssetSubpath, oldPath, newPath);
            if (AssetFolderContains(selectedAssetFolderPath, oldPath))
                selectedAssetFolderPath = ReplaceAssetFolderPrefix(selectedAssetFolderPath, oldPath, newPath);
            expandedAssetFolders.erase(oldPath);
            expandedAssetFolders.insert(newPath);
            RefreshAssetBrowser();
            ClearKeyboardFocus();
            SetAssetStatus("Renamed folder to " + newPath);
            return true;
        }

        return false;
    }

    std::optional<uint32_t> HitAssetTile(int x, int y) const
    {
        for (uint32_t i = 0; i < assetButtons.size(); ++i)
        {
            if (i < visibleAssetEntries.size() && HitElement(assetButtons[i], x, y, 4.0f))
                return i;
        }
        return std::nullopt;
    }

    std::optional<uint32_t> SelectedAssetVisibleIndex() const
    {
        if (selectedAssetId.empty())
            return std::nullopt;
        for (uint32_t i = 0; i < visibleAssetEntries.size() && i < assetButtons.size(); ++i)
        {
            if (visibleAssetEntries[i].id == selectedAssetId)
                return i;
        }
        return std::nullopt;
    }

    bool HandleAssetRenameInput(const InputEvent& event)
    {
        if (!mapEditorOpen || !editorView)
            return false;

        if (renameTargetType != RenameTargetType::None)
        {
            if (event.type == InputEvent::KeyDown)
            {
                if (event.key == Key_Enter)
                    return CommitRename();
                if (event.key == Key_Escape)
                {
                    CancelRename();
                    return true;
                }
                if (event.key == Key_Backspace)
                {
                    if (!renameEditText.empty())
                        renameEditText.pop_back();
                    ResetRenameRoleConfirm();
                    UpdateRenameVisual();
                    return true;
                }
                return true;
            }
            if (event.type == InputEvent::Char)
            {
                if (event.codepoint >= 32 && event.codepoint < 127 && renameEditText.size() < 200)
                {
                    renameEditText.push_back(static_cast<char>(event.codepoint));
                    ResetRenameRoleConfirm();
                    UpdateRenameVisual();
                }
                return true;
            }
            if (event.type == InputEvent::MouseDown)
                return true;
            return event.type == InputEvent::KeyUp;
        }

        if (event.type == InputEvent::KeyDown && event.key == Key_F2)
        {
            if (auto index = SelectedAssetVisibleIndex())
            {
                BeginAssetRename(*index);
                return true;
            }
            if (!selectedAssetFolderPath.empty() && !IsAllAssetFolderPath(selectedAssetFolderPath) &&
                !IsRootAssetFolderPath(selectedAssetFolderPath))
            {
                for (const Noesis::Ptr<Noesis::TreeViewItem>& item : assetFolderItems)
                {
                    auto it = item ? assetFolderItemPaths.find(item.GetPtr()) : assetFolderItemPaths.end();
                    if (it != assetFolderItemPaths.end() && it->second == selectedAssetFolderPath)
                    {
                        BeginFolderRename(selectedAssetFolderPath, item.GetPtr());
                        return true;
                    }
                }
            }
            SetAssetStatus("Select an asset or folder before F2 rename");
            return true;
        }

        if (event.type == InputEvent::MouseDown && event.button == MouseButton_Right)
        {
            if (auto index = HitAssetTile(event.x, event.y))
            {
                BeginAssetRename(*index);
                return true;
            }
            std::string folderPath;
            if (Noesis::TreeViewItem* item = HitFolderTreeItem(event.x, event.y, folderPath))
            {
                BeginFolderRename(folderPath, item);
                return true;
            }
        }

        return false;
    }

    void UpdateDragFeedback(int x, int y)
    {
        if (!activeAssetDrag)
            return;

        if (auto slot = HitMaterialSlot(x, y))
        {
            const bool valid = currentDragPayload.type == DragPayloadType::AssetTexture &&
                TextureRoleMatchesMaterialSlot(currentDragPayload.textureRole, *slot);
            SetDragHighlight(materialSlotButtons[*slot], valid);
            return;
        }
        if (auto slot = HitPaintSlot(x, y))
        {
            const bool valid = currentDragPayload.type == DragPayloadType::AssetMaterial;
            SetDragHighlight(editorTextureButtons[*slot], valid);
            return;
        }
        if (auto folder = HitFolderItem(x, y))
        {
            const bool valid = !IsAllAssetFolderPath(*folder) &&
                (currentDragPayload.type == DragPayloadType::AssetTexture ||
                 currentDragPayload.type == DragPayloadType::AssetMaterial);
            auto itemIt = std::find_if(assetFolderItems.begin(), assetFolderItems.end(), [&](const auto& item) {
                auto pathIt = item ? assetFolderItemPaths.find(item.GetPtr()) : assetFolderItemPaths.end();
                return pathIt != assetFolderItemPaths.end() && pathIt->second == *folder;
            });
            SetDragHighlight(itemIt != assetFolderItems.end() ? itemIt->GetPtr() : nullptr, valid);
            return;
        }
        ClearDragHighlight();
    }

    bool CompleteAssetDrop(int x, int y)
    {
        if (!activeAssetDrag || !assetLibrary)
            return false;

        ClearDragHighlight();
        auto entry = assetLibrary->FindById(currentDragPayload.assetId);
        if (!entry)
            return false;

        if (auto slot = HitMaterialSlot(x, y))
            return AssignTextureToMaterialSlot(*slot, *entry);

        if (auto slot = HitPaintSlot(x, y))
        {
            if (currentDragPayload.type != DragPayloadType::AssetMaterial)
            {
                SetAssetStatus("Paint slots accept materials only");
                return true;
            }
            if (AssignMaterialToPaletteSlot(*slot, *entry))
                SetAssetStatus("Slot " + std::to_string(*slot) + " material <- " + entry->displayName);
            return true;
        }

        if (auto folder = HitFolderItem(x, y))
        {
            if (IsAllAssetFolderPath(*folder))
            {
                SetAssetStatus("Drop onto Root or a folder, not All");
                return true;
            }
            if (currentDragPayload.type != DragPayloadType::AssetTexture &&
                currentDragPayload.type != DragPayloadType::AssetMaterial)
            {
                SetAssetStatus("This asset type cannot be moved by folder drop yet");
                return true;
            }

            AssetLibrary::Entry moved{};
            std::string error;
            const std::string target = AssetFolderQuerySubpath(*folder);
            if (!assetLibrary->MoveAssetToSubpath(currentDragPayload.assetId, target, moved, error))
            {
                SetAssetStatus("Move failed: " + error);
                return true;
            }
            selectedAssetId = moved.id;
            activeAssetSubpath = moved.subpath.empty() ? std::string(kRootAssetFolderPath) : moved.subpath;
            selectedAssetFolderPath = activeAssetSubpath;
            RefreshPaletteSlotsReferencingAsset(moved);
            RefreshAssetBrowser();
            SetAssetStatus("Moved " + moved.displayName + " to " + (moved.subpath.empty() ? "Root" : moved.subpath));
            return true;
        }

        return false;
    }

    bool HandleAssetDragDropInput(const InputEvent& event)
    {
        if (!mapEditorOpen || !editorView || !assetLibrary)
            return false;

        if (event.type == InputEvent::MouseDown && event.button == MouseButton_Left)
        {
            for (uint32_t i = 0; i < assetButtons.size(); ++i)
            {
                if (i >= visibleAssetEntries.size() || !HitElement(assetButtons[i], event.x, event.y, 4.0f))
                    continue;
                potentialDragPayload = PayloadForEntry(visibleAssetEntries[i]);
                potentialAssetDrag = potentialDragPayload.type == DragPayloadType::AssetTexture ||
                    potentialDragPayload.type == DragPayloadType::AssetMaterial ||
                    potentialDragPayload.type == DragPayloadType::AssetModel ||
                    potentialDragPayload.type == DragPayloadType::AssetAnimation;
                dragStartX = event.x;
                dragStartY = event.y;
                return false;
            }
        }

        if (event.type == InputEvent::MouseMove && potentialAssetDrag && !activeAssetDrag)
        {
            const int dx = event.x - dragStartX;
            const int dy = event.y - dragStartY;
            if (dx * dx + dy * dy >= 25)
            {
                activeAssetDrag = true;
                currentDragPayload = potentialDragPayload;
                SetAssetStatus("Dragging asset " + currentDragPayload.assetId);
                UpdateDragFeedback(event.x, event.y);
                return true;
            }
        }

        if (event.type == InputEvent::MouseMove && activeAssetDrag)
        {
            UpdateDragFeedback(event.x, event.y);
            return true;
        }

        if (event.type == InputEvent::MouseUp && event.button == MouseButton_Left)
        {
            const bool wasDrag = activeAssetDrag;
            bool handled = false;
            if (activeAssetDrag)
                handled = CompleteAssetDrop(event.x, event.y);
            activeAssetDrag = false;
            potentialAssetDrag = false;
            currentDragPayload = {};
            potentialDragPayload = {};
            ClearDragHighlight();
            return wasDrag || handled;
        }

        return activeAssetDrag;
    }

    bool HandleEditorBrushSliderInput(const InputEvent& event)
    {
        if (!mapEditorOpen || !editorView)
            return false;

        auto hitAnyInspectorSectionButton = [&]() -> bool {
            return HitElement(lightingMainSectionButton, event.x, event.y, 0.0f) ||
                   HitElement(waterBaseSectionButton, event.x, event.y, 0.0f) ||
                   HitElement(waterReflectionSectionButton, event.x, event.y, 0.0f) ||
                   HitElement(waterRefractionSectionButton, event.x, event.y, 0.0f) ||
                   HitElement(waterFoamSectionButton, event.x, event.y, 0.0f) ||
                   HitElement(waterCausticSectionButton, event.x, event.y, 0.0f) ||
                   HitElement(dynamicLightsSectionButton, event.x, event.y, 0.0f) ||
                   HitElement(selectedLightSectionButton, event.x, event.y, 0.0f);
        };

        if (event.type == InputEvent::MouseDown && event.button == MouseButton_Left && hitAnyInspectorSectionButton())
            return false;

        auto applySliderValue = [&](std::uint32_t active, float value) -> bool {
            switch (active)
            {
            case 1:
                ApplyBrushSliderState(value, editorBrushStrength);
                return true;
            case 2:
                ApplyBrushSliderState(editorBrushRadiusMeters, value);
                return true;
            case 3:
                lightingState.directional.azimuthDegrees = std::clamp(value, 0.0f, 360.0f);
                break;
            case 4:
                lightingState.directional.elevationDegrees = std::clamp(value, 0.0f, 90.0f);
                break;
            case 5:
                lightingState.directional.intensity = std::clamp(value, 0.0f, 5.0f);
                break;
            case 6:
                lightingState.directional.r = std::clamp(value, 0.0f, 1.0f);
                break;
            case 7:
                lightingState.directional.g = std::clamp(value, 0.0f, 1.0f);
                break;
            case 8:
                lightingState.directional.b = std::clamp(value, 0.0f, 1.0f);
                break;
            case 9:
                lightingState.ambient.intensity = std::clamp(value, 0.0f, 3.0f);
                break;
            case 10:
                lightingState.ambient.r = std::clamp(value, 0.0f, 1.0f);
                break;
            case 11:
                lightingState.ambient.g = std::clamp(value, 0.0f, 1.0f);
                break;
            case 12:
                lightingState.ambient.b = std::clamp(value, 0.0f, 1.0f);
                break;
            case 13:
                if (dynamicLightEditorState.type == DynamicLightType::Point)
                    dynamicLightEditorState.point.position[0] = value;
                else if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.position[0] = value;
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 14:
                if (dynamicLightEditorState.type == DynamicLightType::Point)
                    dynamicLightEditorState.point.position[1] = value;
                else if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.position[1] = value;
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 15:
                if (dynamicLightEditorState.type == DynamicLightType::Point)
                    dynamicLightEditorState.point.position[2] = value;
                else if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.position[2] = value;
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 16:
                if (dynamicLightEditorState.type == DynamicLightType::Point)
                    dynamicLightEditorState.point.intensity = std::clamp(value, 0.0f, 10.0f);
                else if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.intensity = std::clamp(value, 0.0f, 10.0f);
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 17:
                if (dynamicLightEditorState.type == DynamicLightType::Point)
                    dynamicLightEditorState.point.radius = std::clamp(value, 0.1f, 100.0f);
                else if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.radius = std::clamp(value, 0.1f, 100.0f);
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 18:
                if (dynamicLightEditorState.type == DynamicLightType::Point)
                    dynamicLightEditorState.point.r = std::clamp(value, 0.0f, 1.0f);
                else if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.r = std::clamp(value, 0.0f, 1.0f);
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 19:
                if (dynamicLightEditorState.type == DynamicLightType::Point)
                    dynamicLightEditorState.point.g = std::clamp(value, 0.0f, 1.0f);
                else if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.g = std::clamp(value, 0.0f, 1.0f);
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 20:
                if (dynamicLightEditorState.type == DynamicLightType::Point)
                    dynamicLightEditorState.point.b = std::clamp(value, 0.0f, 1.0f);
                else if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.b = std::clamp(value, 0.0f, 1.0f);
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 21:
                if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.rotation[0] = ToRadians(std::clamp(value, -90.0f, 90.0f));
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 22:
                if (dynamicLightEditorState.type == DynamicLightType::Spot)
                    dynamicLightEditorState.spot.rotation[1] = ToRadians(std::clamp(value, -180.0f, 180.0f));
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 23:
                if (dynamicLightEditorState.type == DynamicLightType::Spot)
                {
                    dynamicLightEditorState.spot.innerConeDegrees = std::clamp(value, 1.0f, 89.0f);
                    dynamicLightEditorState.spot.outerConeDegrees =
                        std::max(dynamicLightEditorState.spot.outerConeDegrees, dynamicLightEditorState.spot.innerConeDegrees);
                }
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 24:
                if (dynamicLightEditorState.type == DynamicLightType::Spot)
                {
                    dynamicLightEditorState.spot.outerConeDegrees = std::clamp(value, 1.0f, 90.0f);
                    dynamicLightEditorState.spot.innerConeDegrees =
                        std::min(dynamicLightEditorState.spot.innerConeDegrees, dynamicLightEditorState.spot.outerConeDegrees);
                }
                MarkSelectedLightChanged();
                UpdateDynamicLightText();
                return true;
            case 25: waterConfig.waterLevelY = std::clamp(value, -50.0f, 50.0f); UpdateWaterText(); return true;
            case 26: waterConfig.baseColor[0] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 27: waterConfig.baseColor[1] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 28: waterConfig.baseColor[2] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 29: waterConfig.baseColor[3] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 30: waterConfig.waveScaleSmall = std::clamp(value, 0.001f, 0.12f); UpdateWaterText(); return true;
            case 31: waterConfig.waveScaleLarge = std::clamp(value, 0.001f, 0.08f); UpdateWaterText(); return true;
            case 32: waterConfig.waveSpeedSmall = std::clamp(value, 0.0f, 0.5f); UpdateWaterText(); return true;
            case 33: waterConfig.waveSpeedLarge = std::clamp(value, 0.0f, 0.5f); UpdateWaterText(); return true;
            case 34: waterConfig.normalStrength = std::clamp(value, 0.0f, 2.0f); UpdateWaterText(); return true;
            case 35: waterConfig.fresnelPower = std::clamp(value, 1.0f, 10.0f); UpdateWaterText(); return true;
            case 36: waterConfig.fresnelMin = std::clamp(value, 0.0f, 0.5f); UpdateWaterText(); return true;
            case 37: waterConfig.reflectionColor[0] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 38: waterConfig.reflectionColor[1] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 39: waterConfig.reflectionColor[2] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 40: waterConfig.reflectionDistortionStrength = std::clamp(value, 0.0f, 0.2f); UpdateWaterText(); return true;
            case 41: waterConfig.foamDistance = std::clamp(value, 0.02f, 1.5f); UpdateWaterText(); return true;
            case 42: waterConfig.foamIntensity = std::clamp(value, 0.0f, 2.0f); UpdateWaterText(); return true;
            case 43: waterConfig.foamScale = std::clamp(value, 0.1f, 2.0f); UpdateWaterText(); return true;
            case 44: waterConfig.foamTerrainThickness = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 45: waterConfig.causticIntensity = std::clamp(value, 0.0f, 3.0f); UpdateWaterText(); return true;
            case 46: waterConfig.causticScale = std::clamp(value, 0.1f, 2.0f); UpdateWaterText(); return true;
            case 47: waterConfig.causticSpeed = std::clamp(value, 0.0f, 2.0f); UpdateWaterText(); return true;
            case 48: waterConfig.causticMaxDepth = std::clamp(value, 1.0f, 30.0f); UpdateWaterText(); return true;
            case 49: waterConfig.shallowColor[0] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 50: waterConfig.shallowColor[1] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 51: waterConfig.shallowColor[2] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 52: waterConfig.deepColor[0] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 53: waterConfig.deepColor[1] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 54: waterConfig.deepColor[2] = std::clamp(value, 0.0f, 1.0f); UpdateWaterText(); return true;
            case 55: waterConfig.depthColorMin = std::clamp(value, 0.0f, 50.0f); waterConfig.depthColorMax = std::max(waterConfig.depthColorMax, waterConfig.depthColorMin + 0.001f); UpdateWaterText(); return true;
            case 56: waterConfig.depthColorMax = std::max(waterConfig.depthColorMin + 0.001f, std::clamp(value, 0.01f, 50.0f)); UpdateWaterText(); return true;
            case 57: waterConfig.depthFadeDistance = std::clamp(value, 0.01f, 50.0f); UpdateWaterText(); return true;
            case 58: waterConfig.refractionStrength = std::clamp(value, 0.0f, 0.1f); UpdateWaterText(); return true;
            case 59: waterConfig.refractionDepthStrength = std::clamp(value, 0.0f, 2.0f); UpdateWaterText(); return true;
            default:
                return false;
            }
            UpdateLightingText();
            return true;
        };

        auto isSliderActiveInUi = [&](std::uint32_t active) -> bool {
            switch (active)
            {
            case 1:
            case 2:
                return true;
            case 3:
            case 4:
            case 5:
            case 6:
            case 7:
            case 8:
            case 9:
            case 10:
            case 11:
            case 12:
                return lightingModeActive && lightingMainExpanded;
            case 13:
            case 14:
            case 15:
            case 16:
            case 17:
            case 18:
            case 19:
            case 20:
            case 21:
            case 22:
            case 23:
            case 24:
                return lightingModeActive && selectedLightExpanded;
            case 25:
            case 26:
            case 27:
            case 28:
            case 29:
            case 30:
            case 31:
            case 32:
            case 33:
            case 34:
            case 35:
            case 36:
            case 37:
            case 38:
            case 39:
                return lightingModeActive && waterBaseExpanded;
            case 40:
                return lightingModeActive && waterReflectionExpanded;
            case 41:
            case 42:
            case 43:
            case 44:
                return lightingModeActive && waterFoamExpanded;
            case 45:
            case 46:
            case 47:
            case 48:
                return lightingModeActive && waterCausticExpanded;
            case 49:
            case 50:
            case 51:
            case 52:
            case 53:
            case 54:
            case 55:
            case 56:
            case 57:
            case 58:
            case 59:
                return lightingModeActive && waterRefractionExpanded;
            default:
                return false;
            }
        };

        auto updateActiveSlider = [&](std::uint32_t active, int x, int y) -> bool {
            if (!isSliderActiveInUi(active))
                return false;
            Noesis::Slider* slider = nullptr;
            switch (active)
            {
            case 1: slider = editorRadius; break;
            case 2: slider = editorStrength; break;
            case 3: slider = lightingAzimuthSlider; break;
            case 4: slider = lightingElevationSlider; break;
            case 5: slider = lightingSunIntensitySlider; break;
            case 6: slider = lightingSunRSlider; break;
            case 7: slider = lightingSunGSlider; break;
            case 8: slider = lightingSunBSlider; break;
            case 9: slider = lightingAmbientIntensitySlider; break;
            case 10: slider = lightingAmbientRSlider; break;
            case 11: slider = lightingAmbientGSlider; break;
            case 12: slider = lightingAmbientBSlider; break;
            case 13: slider = selectedLightXSlider; break;
            case 14: slider = selectedLightYSlider; break;
            case 15: slider = selectedLightZSlider; break;
            case 16: slider = selectedLightIntensitySlider; break;
            case 17: slider = selectedLightRadiusSlider; break;
            case 18: slider = selectedLightRSlider; break;
            case 19: slider = selectedLightGSlider; break;
            case 20: slider = selectedLightBSlider; break;
            case 21: slider = selectedSpotPitchSlider; break;
            case 22: slider = selectedSpotYawSlider; break;
            case 23: slider = selectedSpotInnerSlider; break;
            case 24: slider = selectedSpotOuterSlider; break;
            case 25: slider = waterLevelSlider; break;
            case 26: slider = waterBaseRSlider; break;
            case 27: slider = waterBaseGSlider; break;
            case 28: slider = waterBaseBSlider; break;
            case 29: slider = waterAlphaSlider; break;
            case 30: slider = waterWaveScaleSmallSlider; break;
            case 31: slider = waterWaveScaleLargeSlider; break;
            case 32: slider = waterWaveSpeedSmallSlider; break;
            case 33: slider = waterWaveSpeedLargeSlider; break;
            case 34: slider = waterNormalStrengthSlider; break;
            case 35: slider = waterFresnelPowerSlider; break;
            case 36: slider = waterFresnelMinSlider; break;
            case 37: slider = waterReflectionRSlider; break;
            case 38: slider = waterReflectionGSlider; break;
            case 39: slider = waterReflectionBSlider; break;
            case 40: slider = waterReflectionDistortionSlider; break;
            case 41: slider = waterFoamDistanceSlider; break;
            case 42: slider = waterFoamIntensitySlider; break;
            case 43: slider = waterFoamScaleSlider; break;
            case 44: slider = waterFoamTerrainThicknessSlider; break;
            case 45: slider = waterCausticIntensitySlider; break;
            case 46: slider = waterCausticScaleSlider; break;
            case 47: slider = waterCausticSpeedSlider; break;
            case 48: slider = waterCausticMaxDepthSlider; break;
            case 49: slider = waterShallowRSlider; break;
            case 50: slider = waterShallowGSlider; break;
            case 51: slider = waterShallowBSlider; break;
            case 52: slider = waterDeepRSlider; break;
            case 53: slider = waterDeepGSlider; break;
            case 54: slider = waterDeepBSlider; break;
            case 55: slider = waterDepthColorMinSlider; break;
            case 56: slider = waterDepthColorMaxSlider; break;
            case 57: slider = waterDepthFadeSlider; break;
            case 58: slider = waterRefractionStrengthSlider; break;
            case 59: slider = waterRefractionDepthStrengthSlider; break;
            default: break;
            }
            float value = 0.0f;
            return UpdateBrushSliderFromPointer(slider, x, y, value, false) && applySliderValue(active, value);
        };

        auto beginSlider = [&](std::uint32_t active, Noesis::Slider* slider) -> bool {
            if (!isSliderActiveInUi(active))
                return false;
            if (active >= 3 && !HitElement(inspectorScrollViewport, event.x, event.y, 0.0f))
                return false;
            float value = 0.0f;
            if (!UpdateBrushSliderFromPointer(slider, event.x, event.y, value, true))
                return false;
            activeBrushSlider = active;
            return applySliderValue(active, value);
        };

        if (event.type == InputEvent::MouseDown && event.button == MouseButton_Left)
        {
            if (beginSlider(1, editorRadius) ||
                beginSlider(2, editorStrength) ||
                beginSlider(3, lightingAzimuthSlider) ||
                beginSlider(4, lightingElevationSlider) ||
                beginSlider(5, lightingSunIntensitySlider) ||
                beginSlider(6, lightingSunRSlider) ||
                beginSlider(7, lightingSunGSlider) ||
                beginSlider(8, lightingSunBSlider) ||
                beginSlider(9, lightingAmbientIntensitySlider) ||
                beginSlider(10, lightingAmbientRSlider) ||
                beginSlider(11, lightingAmbientGSlider) ||
                beginSlider(12, lightingAmbientBSlider) ||
                beginSlider(13, selectedLightXSlider) ||
                beginSlider(14, selectedLightYSlider) ||
                beginSlider(15, selectedLightZSlider) ||
                beginSlider(16, selectedLightIntensitySlider) ||
                beginSlider(17, selectedLightRadiusSlider) ||
                beginSlider(18, selectedLightRSlider) ||
                beginSlider(19, selectedLightGSlider) ||
                beginSlider(20, selectedLightBSlider) ||
                beginSlider(21, selectedSpotPitchSlider) ||
                beginSlider(22, selectedSpotYawSlider) ||
                beginSlider(23, selectedSpotInnerSlider) ||
                beginSlider(24, selectedSpotOuterSlider) ||
                beginSlider(25, waterLevelSlider) ||
                beginSlider(26, waterBaseRSlider) ||
                beginSlider(27, waterBaseGSlider) ||
                beginSlider(28, waterBaseBSlider) ||
                beginSlider(29, waterAlphaSlider) ||
                beginSlider(30, waterWaveScaleSmallSlider) ||
                beginSlider(31, waterWaveScaleLargeSlider) ||
                beginSlider(32, waterWaveSpeedSmallSlider) ||
                beginSlider(33, waterWaveSpeedLargeSlider) ||
                beginSlider(34, waterNormalStrengthSlider) ||
                beginSlider(35, waterFresnelPowerSlider) ||
                beginSlider(36, waterFresnelMinSlider) ||
                beginSlider(37, waterReflectionRSlider) ||
                beginSlider(38, waterReflectionGSlider) ||
                beginSlider(39, waterReflectionBSlider) ||
                beginSlider(40, waterReflectionDistortionSlider) ||
                beginSlider(41, waterFoamDistanceSlider) ||
                beginSlider(42, waterFoamIntensitySlider) ||
                beginSlider(43, waterFoamScaleSlider) ||
                beginSlider(44, waterFoamTerrainThicknessSlider) ||
                beginSlider(45, waterCausticIntensitySlider) ||
                beginSlider(46, waterCausticScaleSlider) ||
                beginSlider(47, waterCausticSpeedSlider) ||
                beginSlider(48, waterCausticMaxDepthSlider) ||
                beginSlider(49, waterShallowRSlider) ||
                beginSlider(50, waterShallowGSlider) ||
                beginSlider(51, waterShallowBSlider) ||
                beginSlider(52, waterDeepRSlider) ||
                beginSlider(53, waterDeepGSlider) ||
                beginSlider(54, waterDeepBSlider) ||
                beginSlider(55, waterDepthColorMinSlider) ||
                beginSlider(56, waterDepthColorMaxSlider) ||
                beginSlider(57, waterDepthFadeSlider) ||
                beginSlider(58, waterRefractionStrengthSlider) ||
                beginSlider(59, waterRefractionDepthStrengthSlider))
                return true;
        }

        if (event.type == InputEvent::MouseMove && activeBrushSlider != 0)
        {
            updateActiveSlider(activeBrushSlider, event.x, event.y);
            return true;
        }

        if (event.type == InputEvent::MouseUp && activeBrushSlider != 0)
        {
            updateActiveSlider(activeBrushSlider, event.x, event.y);
            activeBrushSlider = 0;
            return true;
        }

        return false;
    }

    void OnEditorRadiusChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        ApplyBrushSliderState(args.newValue, editorBrushStrength);
    }

    void OnEditorStrengthChanged(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<float>& args)
    {
        ApplyBrushSliderState(editorBrushRadiusMeters, args.newValue);
    }

    void SetAssetStatus(const std::string& text)
    {
        SetText(assetStatusText, text);
    }

    struct AssetFolderTreeData
    {
        std::map<std::string, std::set<std::string>> childrenByParent;
        std::map<std::string, std::uint32_t> directCounts;
        std::uint32_t totalCount = 0;
    };

    AssetFolderTreeData BuildAssetFolderTreeData() const
    {
        AssetFolderTreeData data;
        data.directCounts[""] = 0;
        if (!assetLibrary)
            return data;

        for (const AssetLibrary::Entry& entry : assetLibrary->Entries())
        {
            if (entry.category != assetCategory)
                continue;

            const std::string path = AssetLibrary::NormalizeSubpath(entry.subpath);
            ++data.totalCount;
            ++data.directCounts[path];

            std::string parent;
            for (const std::string& segment : SplitAssetFolderPath(path))
            {
                const std::string child = parent.empty() ? segment : parent + "/" + segment;
                data.childrenByParent[parent].insert(child);
                parent = child;
            }
        }
        return data;
    }

    std::string AssetFolderDisplayName(const std::string& path) const
    {
        if (IsAllAssetFolderPath(path))
            return "All";
        if (IsRootAssetFolderPath(path))
            return "Root";
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::string AssetFolderBreadcrumbLabel() const
    {
        if (IsAllAssetFolderPath(activeAssetSubpath))
            return "All";
        if (IsRootAssetFolderPath(activeAssetSubpath))
            return "Root";

        std::string label;
        for (const std::string& segment : SplitAssetFolderPath(activeAssetSubpath))
        {
            if (!label.empty())
                label += " > ";
            label += segment;
        }
        return label.empty() ? "All" : label;
    }

    void CaptureExpandedAssetFolders()
    {
        for (const Noesis::Ptr<Noesis::TreeViewItem>& item : assetFolderItems)
        {
            if (!item || !item->GetIsExpanded())
                continue;
            auto it = assetFolderItemPaths.find(item.GetPtr());
            if (it != assetFolderItemPaths.end() && !it->second.empty() && !IsRootAssetFolderPath(it->second))
                expandedAssetFolders.insert(it->second);
        }
    }

    void ExpandActiveFolderAncestors()
    {
        if (IsAllAssetFolderPath(activeAssetSubpath) || IsRootAssetFolderPath(activeAssetSubpath))
            return;

        std::string parent;
        for (const std::string& segment : SplitAssetFolderPath(activeAssetSubpath))
        {
            parent = parent.empty() ? segment : parent + "/" + segment;
            expandedAssetFolders.insert(parent);
        }
    }

    Noesis::Ptr<Noesis::TreeViewItem> CreateAssetFolderItem(const std::string& path, const std::string& header)
    {
        Noesis::Ptr<Noesis::TreeViewItem> item = Noesis::MakePtr<Noesis::TreeViewItem>();
        item->SetHeader(header.c_str());
        item->SetFocusable(false);
        item->SetIsTabStop(false);
        item->MouseLeftButtonDown() += Noesis::MakeDelegate(this, &Impl::OnAssetFolderTreeMouseDown);
        assetFolderItemPaths[item.GetPtr()] = path;
        assetFolderItems.push_back(item);
        return item;
    }

    void AddAssetFolderChildren(Noesis::TreeViewItem* parentItem,
                                const std::string& parentPath,
                                const AssetFolderTreeData& data)
    {
        auto childIt = data.childrenByParent.find(parentPath);
        if (childIt == data.childrenByParent.end())
            return;

        for (const std::string& childPath : childIt->second)
        {
            const auto countIt = data.directCounts.find(childPath);
            const std::uint32_t count = countIt != data.directCounts.end() ? countIt->second : 0;
            const std::string header = AssetFolderDisplayName(childPath) + " (" + std::to_string(count) + ")";
            Noesis::Ptr<Noesis::TreeViewItem> childItem = CreateAssetFolderItem(childPath, header);
            childItem->SetIsExpanded(expandedAssetFolders.find(childPath) != expandedAssetFolders.end());
            if (childPath == activeAssetSubpath || childPath == selectedAssetFolderPath)
                childItem->SetIsSelected(true);
            parentItem->GetItems()->Add(childItem.GetPtr());
            AddAssetFolderChildren(childItem.GetPtr(), childPath, data);
        }
    }

    void RebuildAssetFolderTree(const AssetFolderTreeData& data)
    {
        if (!assetFolderTree)
            return;

        CaptureExpandedAssetFolders();
        ExpandActiveFolderAncestors();
        assetFolderTree->GetItems()->Clear();
        assetFolderItemPaths.clear();
        assetFolderItems.clear();

        const std::string allHeader = "All (" + std::to_string(data.totalCount) + ")";
        Noesis::Ptr<Noesis::TreeViewItem> allItem = CreateAssetFolderItem("", allHeader);
        allItem->SetIsSelected(IsAllAssetFolderPath(activeAssetSubpath));
        assetFolderTree->GetItems()->Add(allItem.GetPtr());

        const auto rootCountIt = data.directCounts.find("");
        const std::uint32_t rootCount = rootCountIt != data.directCounts.end() ? rootCountIt->second : 0;
        Noesis::Ptr<Noesis::TreeViewItem> rootItem =
            CreateAssetFolderItem(kRootAssetFolderPath, "Root (" + std::to_string(rootCount) + ")");
        rootItem->SetIsSelected(IsRootAssetFolderPath(activeAssetSubpath) || selectedAssetFolderPath == kRootAssetFolderPath);
        assetFolderTree->GetItems()->Add(rootItem.GetPtr());

        auto rootChildren = data.childrenByParent.find("");
        if (rootChildren != data.childrenByParent.end())
        {
            for (const std::string& childPath : rootChildren->second)
            {
                const auto countIt = data.directCounts.find(childPath);
                const std::uint32_t count = countIt != data.directCounts.end() ? countIt->second : 0;
                Noesis::Ptr<Noesis::TreeViewItem> childItem =
                    CreateAssetFolderItem(childPath, AssetFolderDisplayName(childPath) + " (" + std::to_string(count) + ")");
                childItem->SetIsExpanded(expandedAssetFolders.find(childPath) != expandedAssetFolders.end());
                if (childPath == activeAssetSubpath || childPath == selectedAssetFolderPath)
                    childItem->SetIsSelected(true);
                assetFolderTree->GetItems()->Add(childItem.GetPtr());
                AddAssetFolderChildren(childItem.GetPtr(), childPath, data);
            }
        }
    }

    void SetCurrentAssetFolder(const std::string& path)
    {
        activeAssetSubpath = path;
        selectedAssetFolderPath = path;
        SetText(assetSubpathBox, IsRootAssetFolderPath(path) || IsAllAssetFolderPath(path) ? "" : path);
        RefreshAssetBrowser();
        SetAssetStatus(IsAllAssetFolderPath(path) ? "Folder: All" : "Folder: " + AssetFolderDisplayName(path));
    }

    Noesis::Style* EditorButtonStyle() const
    {
        if (!editorView || !editorView->GetContent())
            return nullptr;
        return editorView->GetContent()->FindResource<Noesis::Style>("EditorButton");
    }

    void AddBreadcrumbButton(const std::string& text, const std::string& path)
    {
        if (!assetBreadcrumbPanel)
            return;

        Noesis::Ptr<Noesis::Button> button = Noesis::MakePtr<Noesis::Button>();
        button->SetContent(text.c_str());
        button->SetFocusable(false);
        button->SetIsTabStop(false);
        if (Noesis::Style* style = EditorButtonStyle())
            button->SetStyle(style);
        button->Click() += Noesis::MakeDelegate(this, &Impl::OnAssetBreadcrumbClicked);
        assetBreadcrumbButtonPaths[button.GetPtr()] = path;
        assetBreadcrumbPanel->GetChildren()->Add(button.GetPtr());
        assetBreadcrumbButtons.push_back(button);
    }

    void AddBreadcrumbSeparator()
    {
        if (!assetBreadcrumbPanel)
            return;

        Noesis::Ptr<Noesis::TextBlock> separator = Noesis::MakePtr<Noesis::TextBlock>();
        separator->SetText(">");
        separator->SetMargin(Noesis::Thickness(0.0f, 5.0f, 6.0f, 0.0f));
        assetBreadcrumbPanel->GetChildren()->Add(separator.GetPtr());
        assetBreadcrumbSeparators.push_back(separator);
    }

    void UpdateAssetBreadcrumb()
    {
        if (assetBreadcrumbPanel)
        {
            assetBreadcrumbPanel->GetChildren()->Clear();
            assetBreadcrumbButtonPaths.clear();
            assetBreadcrumbButtons.clear();
            assetBreadcrumbSeparators.clear();

            if (IsAllAssetFolderPath(activeAssetSubpath))
            {
                AddBreadcrumbButton("All", "");
            }
            else if (IsRootAssetFolderPath(activeAssetSubpath))
            {
                AddBreadcrumbButton("Root", kRootAssetFolderPath);
            }
            else
            {
                std::string path;
                bool first = true;
                for (const std::string& segment : SplitAssetFolderPath(activeAssetSubpath))
                {
                    if (!first)
                        AddBreadcrumbSeparator();
                    path = path.empty() ? segment : path + "/" + segment;
                    AddBreadcrumbButton(segment, path);
                    first = false;
                }
            }
        }

        SetText(assetFolderCountText,
            std::to_string(visibleAssetEntries.size()) + " assets | " + AssetFolderBreadcrumbLabel());
    }

    void RefreshAssetBrowser()
    {
        visibleAssetEntries.clear();
        visibleAssetTags.clear();
        AssetFolderTreeData folderTree;
        if (EnsureAssetLibrary())
        {
            const std::string search = GetText(assetSearchBox);
            const bool showAll = IsAllAssetFolderPath(activeAssetSubpath);
            visibleAssetEntries = assetLibrary->QueryEntries(
                assetCategory,
                AssetFolderQuerySubpath(activeAssetSubpath),
                showAll,
                activeAssetTags,
                search);
            visibleAssetTags = assetLibrary->TagsFor(assetCategory);
            folderTree = BuildAssetFolderTreeData();
        }

        RebuildAssetFolderTree(folderTree);
        UpdateAssetBreadcrumb();

        for (uint32_t i = 0; i < assetTagTexts.size(); ++i)
        {
            if (i < visibleAssetTags.size())
            {
                const auto& tag = visibleAssetTags[i];
                const bool active = std::find(activeAssetTags.begin(), activeAssetTags.end(), tag.first) != activeAssetTags.end();
                const std::string label = (active ? "> " : "") + CompactAssetName(tag.first, 13) +
                    " (" + std::to_string(tag.second) + ")";
                SetText(assetTagTexts[i], label);
            }
            else
            {
                SetText(assetTagTexts[i], "-");
            }
        }

        for (uint32_t i = 0; i < assetTexts.size(); ++i)
        {
            if (i < visibleAssetEntries.size())
            {
                const AssetLibrary::Entry& entry = visibleAssetEntries[i];
                const bool wrongTextureRole = assetCategory == AssetLibrary::Category::Texture &&
                    !TextureRoleMatchesMaterialSlot(entry.textureRole, activeMaterialTextureSlot);
                if (assetButtons[i])
                    assetButtons[i]->SetOpacity(wrongTextureRole ? 0.45f : 1.0f);
                if (assetImages[i])
                {
                    if (entry.category == AssetLibrary::Category::Texture && !entry.thumbnail.empty())
                    {
                        const std::string thumbnailSource = "assets/library/" + entry.thumbnail;
                        Tracenf("[NOESIS-THUMB] requesting image source=%s asset_id=%s",
                            thumbnailSource.c_str(),
                            entry.id.c_str());
                        Noesis::Ptr<Noesis::BitmapImage> image = Noesis::MakePtr<Noesis::BitmapImage>(thumbnailSource.c_str());
                        assetImages[i]->SetSource(image.GetPtr());
                    }
                    else
                    {
                        assetImages[i]->SetSource(nullptr);
                    }
                }

                std::string line;
                if (entry.category == AssetLibrary::Category::Texture)
                {
                    line += std::string(wrongTextureRole ? "x " : "") +
                        "[" + AssetLibrary::TextureRoleBadge(entry.textureRole) + "] " +
                        TextureResolutionLabel(entry);
                    if (!entry.normalConvention.empty())
                        line += " " + entry.normalConvention;
                    line += "\n";
                }
                line += CompactAssetName(entry.displayName, 28);
                if (!entry.subpath.empty())
                    line += "\n" + CompactAssetName(entry.subpath, 28);
                if (!entry.tags.empty())
                {
                    line += "\n";
                    const size_t shownTags = std::min<size_t>(entry.tags.size(), 3);
                    for (size_t tagIndex = 0; tagIndex < shownTags; ++tagIndex)
                    {
                        if (tagIndex > 0)
                            line += " ";
                        line += "#" + entry.tags[tagIndex];
                    }
                    if (entry.tags.size() > shownTags)
                        line += " +" + std::to_string(entry.tags.size() - shownTags);
                }
                SetText(assetTexts[i], line);
            }
            else
            {
                if (assetButtons[i])
                    assetButtons[i]->SetOpacity(0.25f);
                if (assetImages[i])
                    assetImages[i]->SetSource(nullptr);
                SetText(assetTexts[i], "-");
            }
        }
    }

    void RefreshPaletteSlotText()
    {
        for (uint32_t i = 0; i < editorTextureSlotTexts.size(); ++i)
        {
            const std::string& name = editorPaletteSlots[i].displayName.empty()
                ? editorPaletteSlots[i].texturePath
                : editorPaletteSlots[i].displayName;
            const std::string prefix = i == editorTextureSlot ? "> " : "";
            SetText(editorTextureSlotTexts[i], prefix + std::to_string(i) + " " + CompactAssetName(name, 11));
        }
    }

    void UpdateEditorTextureText()
    {
        if (!editorTextureText)
            return;
        const std::string& name = editorPaletteSlots[editorTextureSlot].displayName.empty()
            ? editorPaletteSlots[editorTextureSlot].texturePath
            : editorPaletteSlots[editorTextureSlot].displayName;
        const std::string text = "Slot " + std::to_string(editorTextureSlot) + " " + CompactAssetName(name, 24);
        editorTextureText->SetText(text.c_str());
    }

    void SetAssetCategory(AssetLibrary::Category category)
    {
        assetCategory = category;
        activeAssetSubpath.clear();
        selectedAssetFolderPath.clear();
        activeAssetTags.clear();
        selectedAssetId.clear();
        RefreshAssetBrowser();
        SetAssetStatus(std::string(AssetLibrary::CategoryName(category)) + " assets");
    }

    void ImportAsset(AssetLibrary::Category category)
    {
        if (!EnsureAssetLibrary())
            return;

        std::optional<std::filesystem::path> picked = PickAssetFile(category);
        if (!picked)
        {
            SetAssetStatus("Import cancelled");
            return;
        }

        AssetLibrary::ImportOptions options{};
        options.displayName = GetText(assetNameBox);
        options.subpath = GetText(assetSubpathBox);
        options.tags = AssetLibrary::TagsFromCsv(GetText(assetTagsBox));

        AssetLibrary::Entry entry{};
        std::string error;
        if (!assetLibrary->Import(category, *picked, options, entry, error))
        {
            SetAssetStatus("Import failed: " + error);
            return;
        }

        assetCategory = category;
        activeAssetSubpath = entry.subpath.empty() ? std::string(kRootAssetFolderPath) : entry.subpath;
        selectedAssetFolderPath = activeAssetSubpath;
        selectedAssetId = entry.id;
        SetText(assetNameBox, entry.displayName);
        SetText(assetSubpathBox, entry.subpath);
        SetText(assetTagsBox, AssetLibrary::TagsToCsv(entry.tags));
        RefreshAssetBrowser();
        SetAssetStatus("Imported " + entry.displayName);
    }

    static std::optional<AssetLibrary::Category> CategoryForDroppedFile(const std::filesystem::path& path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".dds" || ext == ".tga")
            return AssetLibrary::Category::Texture;
        if (ext == ".gltf" || ext == ".glb")
            return AssetLibrary::Category::Model;
        if (ext == ".ozz")
            return AssetLibrary::Category::Animation;
        if (ext == ".json")
            return AssetLibrary::Category::Material;
        return std::nullopt;
    }

    std::string CurrentImportSubpath() const
    {
        if (IsAllAssetFolderPath(activeAssetSubpath) || IsRootAssetFolderPath(activeAssetSubpath))
            return {};
        return activeAssetSubpath;
    }

    void ImportDroppedFiles(const std::vector<std::string>& paths)
    {
        if (!EnsureAssetLibrary())
            return;

        const std::string baseSubpath = CurrentImportSubpath();
        std::uint32_t imported = 0;
        std::uint32_t failed = 0;
        std::string lastError;

        auto importFile = [&](const std::filesystem::path& file, const std::string& subpath) {
            const auto category = CategoryForDroppedFile(file);
            if (!category)
                return;

            AssetLibrary::ImportOptions options{};
            options.subpath = subpath;
            AssetLibrary::Entry entry{};
            std::string error;
            if (assetLibrary->Import(*category, file, options, entry, error))
            {
                ++imported;
                assetCategory = *category;
                selectedAssetId = entry.id;
                activeAssetSubpath = entry.subpath.empty() ? std::string(kRootAssetFolderPath) : entry.subpath;
                selectedAssetFolderPath = activeAssetSubpath;
            }
            else
            {
                ++failed;
                lastError = error;
            }
        };

        for (const std::string& rawPath : paths)
        {
            const std::filesystem::path path(rawPath);
            std::error_code ec;
            if (std::filesystem::is_directory(path, ec))
            {
                const std::string rootName = AssetLibrary::NormalizeSubpath(path.filename().generic_string());
                for (std::filesystem::recursive_directory_iterator it(path, ec), end; it != end && !ec; it.increment(ec))
                {
                    if (!it->is_regular_file(ec))
                        continue;
                    std::filesystem::path parentRel = std::filesystem::relative(it->path().parent_path(), path, ec);
                    std::string subpath = baseSubpath;
                    if (!rootName.empty())
                        subpath = subpath.empty() ? rootName : subpath + "/" + rootName;
                    if (!ec && !parentRel.empty() && parentRel != ".")
                    {
                        const std::string rel = AssetLibrary::NormalizeSubpath(parentRel.generic_string());
                        if (!rel.empty())
                            subpath = subpath.empty() ? rel : subpath + "/" + rel;
                    }
                    importFile(it->path(), subpath);
                }
                if (ec)
                {
                    ++failed;
                    lastError = ec.message();
                }
            }
            else
            {
                importFile(path, baseSubpath);
            }
        }

        RefreshAssetBrowser();
        SetAssetStatus("Drop import: " + std::to_string(imported) + " imported, " +
            std::to_string(failed) + " failed" + (lastError.empty() ? "" : " (" + lastError + ")"));
    }

    MapEditorPaletteSlot BuildPaletteSlotFromMaterial(uint32_t slotIndex, const AssetLibrary::Entry& entry)
    {
        MapEditorPaletteSlot slot{};
        slot.slot = slotIndex;
        slot.assetId = entry.id;
        slot.displayName = entry.displayName;
        if (!assetLibrary)
            return slot;

        if (auto diffuse = assetLibrary->FindById(entry.material.diffuseTextureId))
            slot.texturePath = assetLibrary->AssetRelativePath(*diffuse);
        if (auto normal = assetLibrary->FindById(entry.material.normalTextureId))
            slot.normalTexturePath = assetLibrary->AssetRelativePath(*normal);
        if (auto ao = assetLibrary->FindById(entry.material.aoTextureId))
            slot.aoTexturePath = assetLibrary->AssetRelativePath(*ao);
        if (auto roughness = assetLibrary->FindById(entry.material.roughnessTextureId))
            slot.roughnessTexturePath = assetLibrary->AssetRelativePath(*roughness);
        if (auto metallic = assetLibrary->FindById(entry.material.metallicTextureId))
            slot.metallicTexturePath = assetLibrary->AssetRelativePath(*metallic);
        if (auto height = assetLibrary->FindById(entry.material.heightTextureId))
            slot.heightTexturePath = assetLibrary->AssetRelativePath(*height);
        slot.tilingScaleX = entry.material.tilingScaleX;
        slot.tilingScaleY = entry.material.tilingScaleY;
        slot.colorTint[0] = entry.material.colorTint[0];
        slot.colorTint[1] = entry.material.colorTint[1];
        slot.colorTint[2] = entry.material.colorTint[2];
        slot.normalStrength = entry.material.normalStrength;
        slot.aoStrength = entry.material.aoStrength;
        slot.roughnessStrength = entry.material.roughnessStrength;
        slot.metallicStrength = entry.material.metallicStrength;
        return slot;
    }

    bool AssignMaterialToPaletteSlot(uint32_t slotIndex, const AssetLibrary::Entry& entry)
    {
        if (!EnsureAssetLibrary() || slotIndex >= editorPaletteSlots.size())
            return false;

        MapEditorPaletteSlot slot = BuildPaletteSlotFromMaterial(slotIndex, entry);
        editorPaletteSlots[slotIndex] = slot;
        editorTextureSlot = slotIndex;

        editorCommands.paletteSlotChanged = true;
        editorCommands.paletteSlot = slotIndex;
        editorCommands.paletteAssetId = slot.assetId;
        editorCommands.paletteTexturePath = slot.texturePath;
        if (editorPaint)
            editorPaint->SetIsChecked(true);

        UpdateEditorTextureText();
        RefreshPaletteSlotText();
        return true;
    }

    void RefreshPaletteSlotsUsingMaterial(const AssetLibrary::Entry& entry)
    {
        std::optional<uint32_t> commandSlot;
        for (uint32_t i = 0; i < editorPaletteSlots.size(); ++i)
        {
            if (editorPaletteSlots[i].assetId != entry.id)
                continue;

            editorPaletteSlots[i] = BuildPaletteSlotFromMaterial(i, entry);
            if (i == editorTextureSlot)
                commandSlot = i;
            else if (!commandSlot)
                commandSlot = i;
        }

        if (commandSlot)
        {
            const MapEditorPaletteSlot& slot = editorPaletteSlots[*commandSlot];
            editorCommands.paletteSlotChanged = true;
            editorCommands.paletteSlot = *commandSlot;
            editorCommands.paletteAssetId = slot.assetId;
            editorCommands.paletteTexturePath = slot.texturePath;
        }

        UpdateEditorTextureText();
        RefreshPaletteSlotText();
    }

    void RefreshPaletteSlotsReferencingAsset(const AssetLibrary::Entry& moved)
    {
        if (!assetLibrary)
            return;

        std::optional<uint32_t> commandSlot;
        for (uint32_t i = 0; i < editorPaletteSlots.size(); ++i)
        {
            const std::string materialId = editorPaletteSlots[i].assetId;
            if (materialId.empty())
                continue;

            auto material = assetLibrary->FindById(materialId);
            if (!material || material->category != AssetLibrary::Category::Material)
                continue;

            const AssetLibrary::MaterialData& data = material->material;
            const bool referencesMovedAsset =
                material->id == moved.id ||
                data.diffuseTextureId == moved.id ||
                data.normalTextureId == moved.id ||
                data.aoTextureId == moved.id ||
                data.roughnessTextureId == moved.id ||
                data.metallicTextureId == moved.id ||
                data.heightTextureId == moved.id;
            if (!referencesMovedAsset)
                continue;

            editorPaletteSlots[i] = BuildPaletteSlotFromMaterial(i, *material);
            if (i == editorTextureSlot)
                commandSlot = i;
            else if (!commandSlot)
                commandSlot = i;
        }

        if (commandSlot)
        {
            const MapEditorPaletteSlot& slot = editorPaletteSlots[*commandSlot];
            editorCommands.paletteSlotChanged = true;
            editorCommands.paletteSlot = *commandSlot;
            editorCommands.paletteAssetId = slot.assetId;
            editorCommands.paletteTexturePath = slot.texturePath;
        }

        UpdateEditorTextureText();
        RefreshPaletteSlotText();
    }

    bool SaveMaterialAsNew(AssetLibrary::Entry& entry, std::string& error)
    {
        AssetLibrary::ImportOptions options{};
        options.displayName = GetText(assetNameBox).empty() ? "material" : GetText(assetNameBox);
        options.subpath = GetText(assetSubpathBox);
        options.tags = AssetLibrary::TagsFromCsv(GetText(assetTagsBox));
        return assetLibrary->CreateMaterial(options, materialDraft, entry, error);
    }

    void OnEditorSaveClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        editorCommands.save = true;
    }

    void OnEditorReloadClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        editorCommands.reload = true;
    }

    void OnEditorUndoClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        editorCommands.undo = true;
    }

    void OnAddMarkerClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SetLightingModeActive(false);
        editorCommands.addTestMarker = true;
        SetAssetStatus("Adding test marker in front of the editor camera");
    }

    void OnEditorTextureClicked(Noesis::BaseComponent* sender, const Noesis::RoutedEventArgs&)
    {
        for (uint32_t i = 0; i < 8; ++i)
        {
            if (sender == editorTextureButtons[i])
            {
                editorTextureSlot = i;
                break;
            }
        }
        UpdateEditorTextureText();
        RefreshPaletteSlotText();
    }

    void OnAssetTexturesClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SetAssetCategory(AssetLibrary::Category::Texture);
    }

    void OnAssetModelsClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SetAssetCategory(AssetLibrary::Category::Model);
    }

    void OnAssetAnimationsClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SetAssetCategory(AssetLibrary::Category::Animation);
    }

    void OnAssetMaterialsClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SetAssetCategory(AssetLibrary::Category::Material);
    }

    void OnImportTextureClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        ImportAsset(AssetLibrary::Category::Texture);
    }

    void OnImportModelClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        ImportAsset(AssetLibrary::Category::Model);
    }

    void OnImportAnimationClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        ImportAsset(AssetLibrary::Category::Animation);
    }

    void OnAssetClicked(Noesis::BaseComponent* sender, const Noesis::RoutedEventArgs&)
    {
        for (uint32_t i = 0; i < assetButtons.size(); ++i)
        {
            if (sender != assetButtons[i] || i >= visibleAssetEntries.size())
                continue;

            const AssetLibrary::Entry& entry = visibleAssetEntries[i];
            selectedAssetId = entry.id;
            if (!(assetCategory == AssetLibrary::Category::Texture && editingMaterialId))
            {
                SetMaterialMetadataReadOnly(false);
                SetText(assetNameBox, entry.displayName);
                SetText(assetSubpathBox, entry.subpath);
                SetText(assetTagsBox, AssetLibrary::TagsToCsv(entry.tags));
            }

            if (assetCategory == AssetLibrary::Category::Texture)
            {
                if (!EnsureAssetLibrary())
                    return;
                if (!TextureRoleMatchesMaterialSlot(entry.textureRole, activeMaterialTextureSlot))
                {
                    SetAssetStatus(WrongTextureRoleMessage(entry, activeMaterialTextureSlot));
                    return;
                }

                if (!pendingMaterialTargetSlotValid && activeMaterialTextureSlot == 0)
                {
                    pendingMaterialTargetSlot = editorTextureSlot;
                    pendingMaterialTargetSlotValid = true;
                    ResetMaterialEditor();
                    materialDraft.diffuseTextureId = entry.id;
                    activeMaterialTextureSlot = 0;
                    SetText(assetNameBox, entry.displayName);
                    SetText(assetSubpathBox, entry.subpath);
                    SetText(assetTagsBox, AssetLibrary::TagsToCsv(entry.tags));
                    UpdateMaterialEditorText();
                    SetAssetStatus("Texture loaded as Diffuse. Add Normal map + Apply to use in Slot " +
                        std::to_string(pendingMaterialTargetSlot) + ".");
                    return;
                }

                if (activeMaterialTextureSlot == 1)
                    materialDraft.normalTextureId = entry.id;
                else if (activeMaterialTextureSlot == 2)
                    materialDraft.aoTextureId = entry.id;
                else if (activeMaterialTextureSlot == 3)
                    materialDraft.roughnessTextureId = entry.id;
                else if (activeMaterialTextureSlot == 4)
                    materialDraft.metallicTextureId = entry.id;
                else if (activeMaterialTextureSlot == 5)
                    materialDraft.heightTextureId = entry.id;
                else
                    materialDraft.diffuseTextureId = entry.id;
                UpdateMaterialEditorText();
                std::string status = "Material " + MaterialSlotName(activeMaterialTextureSlot) + " <- " + entry.displayName;
                if (entry.textureRole == AssetLibrary::TextureRole::Normal && entry.normalConvention == "dx")
                    status += " (DX normal; shader expects GL)";
                SetAssetStatus(status);
                return;
            }

            if (assetCategory == AssetLibrary::Category::Material)
            {
                LoadMaterialDraftFromEntry(entry);
                const uint32_t targetSlot = editorTextureSlot;
                if (AssignMaterialToPaletteSlot(targetSlot, entry))
                {
                    pendingMaterialTargetSlotValid = false;
                    SetAssetStatus("Editing material " + entry.displayName +
                        "; assigned to Slot " + std::to_string(targetSlot));
                }
                return;
            }

            SetAssetStatus("Browse only: " + entry.displayName);
            return;
        }
    }

    void OnAssetSearchChanged(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        RefreshAssetBrowser();
    }

    void OnMaterialDiffuseSlotClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        activeMaterialTextureSlot = 0;
        UpdateMaterialEditorText();
        SetAssetCategory(AssetLibrary::Category::Texture);
        SetAssetStatus("Pick a diffuse texture");
    }

    void OnMaterialNormalSlotClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        activeMaterialTextureSlot = 1;
        UpdateMaterialEditorText();
        SetAssetCategory(AssetLibrary::Category::Texture);
        SetAssetStatus("Pick a normal texture");
    }

    void OnMaterialAoSlotClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        activeMaterialTextureSlot = 2;
        UpdateMaterialEditorText();
        SetAssetCategory(AssetLibrary::Category::Texture);
        SetAssetStatus("Pick an AO texture");
    }

    void OnMaterialRoughnessSlotClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        activeMaterialTextureSlot = 3;
        UpdateMaterialEditorText();
        SetAssetCategory(AssetLibrary::Category::Texture);
        SetAssetStatus("Pick a roughness texture");
    }

    void OnMaterialMetallicSlotClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        activeMaterialTextureSlot = 4;
        UpdateMaterialEditorText();
        SetAssetCategory(AssetLibrary::Category::Texture);
        SetAssetStatus("Pick a metallic texture");
    }

    void OnMaterialHeightSlotClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        activeMaterialTextureSlot = 5;
        UpdateMaterialEditorText();
        SetAssetCategory(AssetLibrary::Category::Texture);
        SetAssetStatus("Pick a height texture");
    }

    void OnNewMaterialClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        pendingMaterialTargetSlotValid = false;
        ResetMaterialEditor();
        SetAssetStatus("New material");
    }

    void OnLoadMaterialClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        if (selectedAssetId.empty() || !EnsureAssetLibrary())
        {
            SetAssetStatus("Select a material first");
            return;
        }
        auto entry = assetLibrary->FindById(selectedAssetId);
        if (!entry || entry->category != AssetLibrary::Category::Material)
        {
            SetAssetStatus("Selected asset is not a material");
            return;
        }
        LoadMaterialDraftFromEntry(*entry);
        SetAssetStatus("Loaded material " + entry->displayName);
    }

    void OnSaveMaterialClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        if (!EnsureAssetLibrary())
            return;
        PullMaterialDraftFromUi();

        AssetLibrary::Entry entry{};
        std::string error;
        if (editingMaterialId)
        {
            if (!assetLibrary->UpdateMaterial(*editingMaterialId, materialDraft, entry, error))
            {
                SetAssetStatus("Material update failed: " + error + ". Use Save As to create new.");
                return;
            }
            materialSaveAsEditMode = false;
            SetMaterialMetadataReadOnly(true);
            selectedAssetId = entry.id;
            RefreshPaletteSlotsUsingMaterial(entry);
            RefreshAssetBrowser();
            SetAssetStatus("Material saved: " + entry.displayName);
            return;
        }

        if (!SaveMaterialAsNew(entry, error))
        {
            SetAssetStatus("Material save failed: " + error);
            return;
        }

        editingMaterialId = entry.id;
        materialSaveAsEditMode = false;
        selectedAssetId = entry.id;
        assetCategory = AssetLibrary::Category::Material;
        activeAssetSubpath = entry.subpath.empty() ? std::string(kRootAssetFolderPath) : entry.subpath;
        selectedAssetFolderPath = activeAssetSubpath;
        SetMaterialMetadataReadOnly(true);
        RefreshAssetBrowser();
        if (pendingMaterialTargetSlotValid)
        {
            const uint32_t targetSlot = pendingMaterialTargetSlot;
            if (AssignMaterialToPaletteSlot(targetSlot, entry))
                SetAssetStatus("Material created and applied to Slot " + std::to_string(targetSlot) + ".");
            else
                SetAssetStatus("Created material " + entry.displayName + ", but slot apply failed");
            pendingMaterialTargetSlotValid = false;
        }
        else
        {
            SetAssetStatus("Created material " + entry.displayName);
        }
    }

    void OnSaveAsMaterialClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        if (!EnsureAssetLibrary())
            return;

        PullMaterialDraftFromUi();
        if (editingMaterialId && !materialSaveAsEditMode)
        {
            materialSaveAsEditMode = true;
            SetMaterialMetadataReadOnly(false);
            const std::string baseName = GetText(assetNameBox).empty() ? "material" : GetText(assetNameBox);
            SetText(assetNameBox, baseName + "_variant");
            SetAssetStatus("Save As: edit Name / Folder / Tags, then click Save As again");
            return;
        }

        const std::optional<std::string> previousEditingId = editingMaterialId;
        SetMaterialMetadataReadOnly(false);

        AssetLibrary::Entry entry{};
        std::string error;
        if (!SaveMaterialAsNew(entry, error))
        {
            editingMaterialId = previousEditingId;
            materialSaveAsEditMode = previousEditingId.has_value();
            SetMaterialMetadataReadOnly(false);
            SetAssetStatus("Material Save As failed: " + error);
            return;
        }

        editingMaterialId = entry.id;
        materialSaveAsEditMode = false;
        selectedAssetId = entry.id;
        assetCategory = AssetLibrary::Category::Material;
        activeAssetSubpath = entry.subpath.empty() ? std::string(kRootAssetFolderPath) : entry.subpath;
        selectedAssetFolderPath = activeAssetSubpath;
        SetText(assetNameBox, entry.displayName);
        SetText(assetSubpathBox, entry.subpath);
        SetText(assetTagsBox, AssetLibrary::TagsToCsv(entry.tags));
        SetMaterialMetadataReadOnly(true);
        RefreshAssetBrowser();
        SetAssetStatus("Material created: " + entry.displayName);
    }

    void OnAssetFolderTreeSelected(Noesis::BaseComponent*, const Noesis::RoutedPropertyChangedEventArgs<Noesis::Ptr<Noesis::BaseComponent>>& args)
    {
        auto it = assetFolderItemPaths.find(args.newValue.GetPtr());
        if (it == assetFolderItemPaths.end())
            return;

        selectedAssetFolderPath = it->second;
        const std::string queryPath = AssetFolderQuerySubpath(selectedAssetFolderPath);
        const std::uint32_t count = assetLibrary && !IsAllAssetFolderPath(selectedAssetFolderPath)
            ? assetLibrary->CountAssetsIn(assetCategory, queryPath)
            : static_cast<std::uint32_t>(visibleAssetEntries.size());
        SetAssetStatus("Selected folder: " + AssetFolderDisplayName(selectedAssetFolderPath) +
            " (" + std::to_string(count) + ")");
    }

    void OnAssetFolderTreeMouseDown(Noesis::BaseComponent* sender, const Noesis::MouseButtonEventArgs& args)
    {
        auto it = assetFolderItemPaths.find(sender);
        if (it == assetFolderItemPaths.end())
            return;

        const std::string path = it->second;
        const bool doubleClick = args.clickCount >= 2 ||
            (path == lastClickedAssetFolderPath &&
             lastAssetFolderClickSeconds >= 0.0 &&
             currentTimeSeconds - lastAssetFolderClickSeconds <= kFolderDoubleClickSeconds);

        lastClickedAssetFolderPath = path;
        lastAssetFolderClickSeconds = currentTimeSeconds;
        selectedAssetFolderPath = path;
        if (Noesis::TreeViewItem* item = Noesis::DynamicCast<Noesis::TreeViewItem*>(sender))
            item->SetIsSelected(true);

        if (!doubleClick)
            return;

        if (!IsAllAssetFolderPath(path) && !IsRootAssetFolderPath(path))
            expandedAssetFolders.insert(path);
        SetCurrentAssetFolder(path);
    }

    void OnAssetFolderHomeClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SetCurrentAssetFolder("");
    }

    void OnAssetFolderUpClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SetCurrentAssetFolder(ParentAssetFolderPath(activeAssetSubpath));
    }

    void OnAssetBreadcrumbClicked(Noesis::BaseComponent* sender, const Noesis::RoutedEventArgs&)
    {
        auto it = assetBreadcrumbButtonPaths.find(sender);
        if (it == assetBreadcrumbButtonPaths.end())
            return;
        SetCurrentAssetFolder(it->second);
    }

    void OnAssetTagClicked(Noesis::BaseComponent* sender, const Noesis::RoutedEventArgs&)
    {
        for (uint32_t i = 0; i < assetTagButtons.size(); ++i)
        {
            if (sender != assetTagButtons[i] || i >= visibleAssetTags.size())
                continue;
            const std::string& tag = visibleAssetTags[i].first;
            auto it = std::find(activeAssetTags.begin(), activeAssetTags.end(), tag);
            if (it == activeAssetTags.end())
                activeAssetTags.push_back(tag);
            else
                activeAssetTags.erase(it);
            RefreshAssetBrowser();
            SetAssetStatus("Tag filter: " + AssetLibrary::TagsToCsv(activeAssetTags));
            return;
        }
    }

    void OnAssetClearClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        activeAssetSubpath.clear();
        selectedAssetFolderPath.clear();
        activeAssetTags.clear();
        selectedAssetId.clear();
        SetText(assetSearchBox, "");
        SetText(assetNameBox, "");
        SetText(assetSubpathBox, "");
        SetText(assetTagsBox, "");
        RefreshAssetBrowser();
        SetAssetStatus("Filters cleared");
    }

    void OnAssetRefreshClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        if (!EnsureAssetLibrary())
            return;
        std::string error;
        if (!assetLibrary->Refresh(error))
        {
            SetAssetStatus("Refresh failed: " + error);
            return;
        }
        RefreshAssetBrowser();
        SetAssetStatus("Library refreshed");
    }

    void OnApplyAssetMetaClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        if (selectedAssetId.empty())
        {
            SetAssetStatus("Select an asset first");
            return;
        }
        if (!EnsureAssetLibrary())
            return;

        std::string error;
        if (!assetLibrary->UpdateAssetMetadata(
                selectedAssetId,
                GetText(assetNameBox),
                AssetLibrary::TagsFromCsv(GetText(assetTagsBox)),
                error))
        {
            SetAssetStatus("Metadata failed: " + error);
            return;
        }
        if (auto updated = assetLibrary->FindById(selectedAssetId))
        {
            for (MapEditorPaletteSlot& slot : editorPaletteSlots)
            {
                if (slot.assetId == selectedAssetId)
                    slot.displayName = updated->displayName;
            }
            UpdateEditorTextureText();
            RefreshPaletteSlotText();
        }
        RefreshAssetBrowser();
        SetAssetStatus("Metadata updated");
    }

    void CloseInGameMenu()
    {
        inGameMenuOpen = false;
    }

    void OnCharacterSwitchClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        Log("[MENU] character switch");
        CloseInGameMenu();
        LoadLobbyView(currentWidth, currentHeight);
        if (clientSession && clientSession->IsAuthenticated())
            clientSession->SendCharacterListRequest();
    }

    void OnLogoutClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        Log("[MENU] logout");
        CloseInGameMenu();
        if (clientSession)
            clientSession->Disconnect();
        LoadLoginView(currentWidth, currentHeight);
        SetStatus("Disconnected");
    }

    void OnQuitClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        Log("[MENU] quit");
        CloseInGameMenu();
        if (quitCallback)
            quitCallback();
    }

    void OnCloseMenuClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        CloseInGameMenu();
    }

    void SetStatus(const char* text)
    {
        if (statusText)
            statusText->SetText(text);
        else if (lobbyViewModel)
            lobbyViewModel->SetStatus(text);
    }

    bool IsLoginViewActive() const
    {
        return usernameBox != nullptr && passwordBox != nullptr && !lobbyActive && !inWorld;
    }

    void SubmitLogin()
    {
        const char* username = usernameBox ? usernameBox->GetText() : "";
        const char* password = passwordBox ? passwordBox->GetPassword() : "";
        const bool remember = rememberBox ? rememberBox->GetIsChecked().GetValueOrDefault() : false;
        LogFormat("[LOGIN] attempt user='%s' remember=%s", username, remember ? "true" : "false");

        if (!username[0] || !password[0])
        {
            SetStatus("Username and password required");
            return;
        }

        loginName = username;
        loginPassword = password;
        if (!clientSession)
        {
            SetStatus("Network session is not ready");
            return;
        }

        clientSession->Connect("127.0.0.1", 11000);
        SetStatus("Connecting...");
    }

    void OnLoginClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        SubmitLogin();
    }

    bool HandleLoginEnterInput(const InputEvent& event)
    {
        if (event.type != InputEvent::KeyDown || event.key != Key_Enter || !IsLoginViewActive())
            return false;

        SubmitLogin();
        return true;
    }

    void InitializeEntityWorld()
    {
        entitiesByNetId.clear();
        entityWorld = std::make_unique<flecs::world>();
        entityWorld->component<client::ecs::Position>();
        entityWorld->component<client::ecs::Heading>();
        entityWorld->component<client::ecs::NetId>();
        entityWorld->component<client::ecs::MoveState>();
        entityWorld->component<client::ecs::RenderableModel>();
        entityWorld->component<client::ecs::Nameplate>();
        entityWorld->component<client::ecs::Health>();
        entityWorld->component<client::ecs::LocalPlayerTag>();
        entityWorld->component<client::ecs::MobTag>();
        entityWorld->component<client::ecs::InterpolationBuffer>();
    }

    void ClearWorldEntities()
    {
        InitializeEntityWorld();
    }

    flecs::entity UpsertWorldEntity(std::uint32_t netId,
                                     const char* name,
                                     client::net::Vec3 position,
                                     std::uint16_t heading,
                                     client::net::MoveState moveState,
                                     bool localPlayer,
                                     std::uint32_t mobTypeId = 0,
                                     std::uint32_t level = 1,
                                     float hpCurrent = 1.0f,
                                     float hpMax = 1.0f)
    {
        if (!entityWorld)
            InitializeEntityWorld();

        auto it = entitiesByNetId.find(netId);
        flecs::entity entity;
        if (it == entitiesByNetId.end())
        {
            entity = entityWorld->entity();
            entitiesByNetId.emplace(netId, entity);
        }
        else
        {
            entity = it->second;
        }

        entity.set<client::ecs::NetId>({netId})
            .set<client::ecs::Position>({position.x, position.y, position.z})
            .set<client::ecs::Heading>({heading})
            .set<client::ecs::MoveState>({moveState})
            .set<client::ecs::RenderableModel>({0, mobTypeId})
            .set<client::ecs::Nameplate>({name ? name : "Player", level == 0 ? 1 : level})
            .set<client::ecs::Health>({hpCurrent, hpMax <= 0.0f ? 1.0f : hpMax, hpCurrent});

        if (localPlayer)
        {
            entity.add<client::ecs::LocalPlayerTag>();
            entity.remove<client::ecs::MobTag>();
            entity.remove<client::ecs::InterpolationBuffer>();
        }
        else if (mobTypeId != 0)
        {
            entity.add<client::ecs::MobTag>();
        }
        else
        {
            entity.remove<client::ecs::MobTag>();
        }

        if (!localPlayer && !entity.has<client::ecs::InterpolationBuffer>())
        {
            client::ecs::InterpolationBuffer buffer;
            AddInterpolationSample(buffer, position, heading, latestServerTimeSeconds);
            entity.set<client::ecs::InterpolationBuffer>(buffer);
        }

        return entity;
    }

    void AddInterpolationSample(client::ecs::InterpolationBuffer& buffer,
                                client::net::Vec3 position,
                                std::uint16_t heading,
                                double serverTimeSeconds) const
    {
        if (buffer.count > 0)
        {
            for (std::uint8_t i = 0; i < buffer.count; ++i)
            {
                if (buffer.samples[i].serverTimeSeconds == serverTimeSeconds)
                {
                    buffer.samples[i].position = {position.x, position.y, position.z};
                    buffer.samples[i].heading = {heading};
                    return;
                }
            }
        }

        buffer.samples[buffer.next] =
            client::ecs::InterpolationSample{{position.x, position.y, position.z},
                                             {heading},
                                             serverTimeSeconds};
        buffer.next = static_cast<std::uint8_t>((buffer.next + 1) % client::ecs::InterpolationBuffer::Capacity);
        if (buffer.count < client::ecs::InterpolationBuffer::Capacity)
            ++buffer.count;
    }

    void ApplyLatestTransform(flecs::entity entity, const client::net::EntityTransform& transform) const
    {
        entity.set<client::ecs::Position>(
                  {transform.position.x, transform.position.y, transform.position.z})
            .set<client::ecs::Heading>({transform.heading})
            .set<client::ecs::MoveState>({transform.moveState});
    }

    void BufferRemoteTransform(flecs::entity entity,
                               const client::net::EntityTransform& transform,
                               double serverTimeSeconds) const
    {
        if (!entity.has<client::ecs::InterpolationBuffer>())
            entity.set<client::ecs::InterpolationBuffer>({});

        auto& buffer = entity.get_mut<client::ecs::InterpolationBuffer>();
        const bool firstSample = buffer.count == 0;
        AddInterpolationSample(buffer, transform.position, transform.heading, serverTimeSeconds);
        entity.modified<client::ecs::InterpolationBuffer>();
        entity.set<client::ecs::MoveState>({transform.moveState});

        if (firstSample)
            ApplyLatestTransform(entity, transform);
    }

    void RunInterpolationSystem(double timeSeconds)
    {
        if (!entityWorld || latestServerTimeSeconds <= 0.0)
            return;

        const double estimatedServerTime =
            latestServerTimeSeconds + (timeSeconds - latestTransformReceiveLocalTimeSeconds);
        const double renderTime = estimatedServerTime - kInterpolationDelaySeconds;

        auto query = entityWorld->query_builder<client::ecs::Position,
                                                client::ecs::Heading,
                                                const client::ecs::InterpolationBuffer>()
                         .build();
        query.each([&](flecs::entity entity,
                       client::ecs::Position& position,
                       client::ecs::Heading& heading,
                       const client::ecs::InterpolationBuffer& buffer) {
            if (entity.has<client::ecs::LocalPlayerTag>() || buffer.count == 0)
                return;

            std::array<client::ecs::InterpolationSample, client::ecs::InterpolationBuffer::Capacity> samples{};
            for (std::uint8_t i = 0; i < buffer.count; ++i)
                samples[i] = buffer.samples[i];

            std::sort(samples.begin(),
                samples.begin() + buffer.count,
                [](const auto& a, const auto& b) {
                    return a.serverTimeSeconds < b.serverTimeSeconds;
                });

            const auto& first = samples[0];
            const auto& last = samples[buffer.count - 1];
            if (buffer.count == 1 || renderTime <= first.serverTimeSeconds)
            {
                position = first.position;
                heading = first.heading;
                return;
            }

            if (renderTime >= last.serverTimeSeconds)
            {
                position = last.position;
                heading = last.heading;
                return;
            }

            for (std::uint8_t i = 1; i < buffer.count; ++i)
            {
                const auto& previous = samples[i - 1];
                const auto& next = samples[i];
                if (renderTime > next.serverTimeSeconds)
                    continue;

                const double duration = next.serverTimeSeconds - previous.serverTimeSeconds;
                const double t = duration > 0.0
                                     ? (renderTime - previous.serverTimeSeconds) / duration
                                     : 1.0;
                position = LerpPosition(previous.position, next.position, t);
                heading = LerpHeadingShortest(previous.heading, next.heading, t);
                return;
            }
        });
    }

    void RemoveWorldEntity(std::uint32_t netId)
    {
        const auto it = entitiesByNetId.find(netId);
        if (it == entitiesByNetId.end())
            return;

        if (it->second.is_valid())
            it->second.destruct();
        entitiesByNetId.erase(it);
    }

    std::vector<WorldRenderEntity> CollectWorldEntities() const
    {
        std::vector<WorldRenderEntity> result;
        if (!entityWorld)
            return result;

        result.reserve(entitiesByNetId.size());
        auto query = entityWorld->query_builder<const client::ecs::NetId,
                                                const client::ecs::Position,
                                                const client::ecs::Heading,
                                                const client::ecs::MoveState,
                                                const client::ecs::RenderableModel,
                                                const client::ecs::Nameplate,
                                                const client::ecs::Health>()
                         .build();
        query.each([&](flecs::entity,
                       const client::ecs::NetId& netId,
                       const client::ecs::Position& position,
                       const client::ecs::Heading& heading,
                       const client::ecs::MoveState& moveState,
                       const client::ecs::RenderableModel& renderable,
                       const client::ecs::Nameplate& nameplate,
                       const client::ecs::Health& health) {
            WorldRenderEntity entity;
            entity.netId = netId.value;
            entity.name = nameplate.name;
            entity.position = {position.x, position.y, position.z};
            entity.heading = heading.angle;
            entity.moveState = moveState.value;
            entity.mobTypeId = renderable.mobTypeId;
            entity.level = nameplate.level == 0 ? 1 : nameplate.level;
            entity.hpCurrent = health.current;
            entity.hpMax = health.max <= 0.0f ? 1.0f : health.max;
            entity.hpDisplayed = health.displayed;
            result.push_back(std::move(entity));
        });
        return result;
    }

    void RunHealthInterpolation(double dt)
    {
        if (!entityWorld)
            return;

        const float alpha = static_cast<float>(std::clamp(dt / 0.2, 0.0, 1.0));
        auto query = entityWorld->query_builder<client::ecs::Health>().build();
        query.each([alpha](flecs::entity entity, client::ecs::Health& health) {
            health.max = health.max <= 0.0f ? 1.0f : health.max;
            health.current = std::clamp(health.current, 0.0f, health.max);
            health.displayed += (health.current - health.displayed) * alpha;
            entity.set<client::ecs::Health>(health);
        });
    }

    Noesis::Ptr<Noesis::RenderDevice> renderDevice;
    Noesis::Ptr<Noesis::IView> view;
    Noesis::Ptr<Noesis::IView> menuView;
    Noesis::Ptr<Noesis::IView> editorView;
    Noesis::Ptr<LobbyViewModel> lobbyViewModel;
    Noesis::TextBox* usernameBox = nullptr;
    Noesis::PasswordBox* passwordBox = nullptr;
    Noesis::CheckBox* rememberBox = nullptr;
    Noesis::TextBlock* statusText = nullptr;
    Noesis::RadioButton* editorRaise = nullptr;
    Noesis::RadioButton* editorLower = nullptr;
    Noesis::RadioButton* editorSmooth = nullptr;
    Noesis::RadioButton* editorFlatten = nullptr;
    Noesis::RadioButton* editorPaint = nullptr;
    Noesis::RadioButton* editorPaintReplace = nullptr;
    Noesis::RadioButton* editorPaintMix = nullptr;
    Noesis::Slider* editorRadius = nullptr;
    Noesis::Slider* editorStrength = nullptr;
    Noesis::TextBlock* editorRadiusText = nullptr;
    Noesis::TextBlock* editorStrengthText = nullptr;
    Noesis::TextBlock* editorTextureText = nullptr;
    Noesis::TextBox* assetSearchBox = nullptr;
    Noesis::TextBox* assetNameBox = nullptr;
    Noesis::TextBox* assetSubpathBox = nullptr;
    Noesis::TextBox* assetTagsBox = nullptr;
    Noesis::TextBox* materialTilingXBox = nullptr;
    Noesis::TextBox* materialTilingYBox = nullptr;
    Noesis::TextBox* materialNormalStrengthBox = nullptr;
    Noesis::TextBox* materialTintBox = nullptr;
    Noesis::TextBlock* materialDiffuseText = nullptr;
    Noesis::TextBlock* materialNormalText = nullptr;
    Noesis::TextBlock* materialAoText = nullptr;
    Noesis::TextBlock* materialRoughnessText = nullptr;
    Noesis::TextBlock* materialMetallicText = nullptr;
    Noesis::TextBlock* materialHeightText = nullptr;
    std::array<Noesis::Button*, 6> materialSlotButtons{};
    Noesis::TextBlock* materialAoStrengthText = nullptr;
    Noesis::TextBlock* materialRoughnessStrengthText = nullptr;
    Noesis::TextBlock* materialMetallicStrengthText = nullptr;
    Noesis::Slider* materialAoStrengthSlider = nullptr;
    Noesis::Slider* materialRoughnessStrengthSlider = nullptr;
    Noesis::Slider* materialMetallicStrengthSlider = nullptr;
    Noesis::FrameworkElement* inspectorContextPanel = nullptr;
    Noesis::FrameworkElement* inspectorScrollViewport = nullptr;
    Noesis::FrameworkElement* inspectorScrollContent = nullptr;
    Noesis::FrameworkElement* lightingPanel = nullptr;
    Noesis::Button* lightingMainSectionButton = nullptr;
    Noesis::FrameworkElement* lightingMainSection = nullptr;
    Noesis::Button* waterBaseSectionButton = nullptr;
    Noesis::FrameworkElement* waterBaseSection = nullptr;
    Noesis::Button* waterReflectionSectionButton = nullptr;
    Noesis::FrameworkElement* waterReflectionSection = nullptr;
    Noesis::Button* waterRefractionSectionButton = nullptr;
    Noesis::FrameworkElement* waterRefractionSection = nullptr;
    Noesis::Button* waterFoamSectionButton = nullptr;
    Noesis::FrameworkElement* waterFoamSection = nullptr;
    Noesis::Button* waterCausticSectionButton = nullptr;
    Noesis::FrameworkElement* waterCausticSection = nullptr;
    Noesis::Button* dynamicLightsSectionButton = nullptr;
    Noesis::FrameworkElement* dynamicLightsSection = nullptr;
    Noesis::Button* selectedLightSectionButton = nullptr;
    Noesis::FrameworkElement* selectedLightSection = nullptr;
    Noesis::TextBlock* lightingAzimuthText = nullptr;
    Noesis::TextBlock* lightingElevationText = nullptr;
    Noesis::TextBlock* lightingSunIntensityText = nullptr;
    Noesis::TextBlock* lightingSunRText = nullptr;
    Noesis::TextBlock* lightingSunGText = nullptr;
    Noesis::TextBlock* lightingSunBText = nullptr;
    Noesis::TextBlock* lightingAmbientIntensityText = nullptr;
    Noesis::TextBlock* lightingAmbientRText = nullptr;
    Noesis::TextBlock* lightingAmbientGText = nullptr;
    Noesis::TextBlock* lightingAmbientBText = nullptr;
    Noesis::Slider* lightingAzimuthSlider = nullptr;
    Noesis::Slider* lightingElevationSlider = nullptr;
    Noesis::Slider* lightingSunIntensitySlider = nullptr;
    Noesis::Slider* lightingSunRSlider = nullptr;
    Noesis::Slider* lightingSunGSlider = nullptr;
    Noesis::Slider* lightingSunBSlider = nullptr;
    Noesis::Button* sunShadowsButton = nullptr;
    Noesis::Slider* lightingAmbientIntensitySlider = nullptr;
    Noesis::Slider* lightingAmbientRSlider = nullptr;
    Noesis::Slider* lightingAmbientGSlider = nullptr;
    Noesis::Slider* lightingAmbientBSlider = nullptr;
    Noesis::Button* waterEnabledButton = nullptr;
    Noesis::Button* waterReflectionEnabledButton = nullptr;
    Noesis::Button* waterReflectionQuarterButton = nullptr;
    Noesis::Button* waterReflectionHalfButton = nullptr;
    Noesis::Button* waterReflectionFullButton = nullptr;
    Noesis::TextBlock* waterLevelText = nullptr;
    Noesis::TextBlock* waterBaseRText = nullptr;
    Noesis::TextBlock* waterBaseGText = nullptr;
    Noesis::TextBlock* waterBaseBText = nullptr;
    Noesis::TextBlock* waterAlphaText = nullptr;
    Noesis::TextBlock* waterWaveScaleSmallText = nullptr;
    Noesis::TextBlock* waterWaveScaleLargeText = nullptr;
    Noesis::TextBlock* waterWaveSpeedSmallText = nullptr;
    Noesis::TextBlock* waterWaveSpeedLargeText = nullptr;
    Noesis::TextBlock* waterNormalStrengthText = nullptr;
    Noesis::TextBlock* waterFresnelPowerText = nullptr;
    Noesis::TextBlock* waterFresnelMinText = nullptr;
    Noesis::TextBlock* waterReflectionRText = nullptr;
    Noesis::TextBlock* waterReflectionGText = nullptr;
    Noesis::TextBlock* waterReflectionBText = nullptr;
    Noesis::TextBlock* waterReflectionDistortionText = nullptr;
    Noesis::TextBlock* waterShallowRText = nullptr;
    Noesis::TextBlock* waterShallowGText = nullptr;
    Noesis::TextBlock* waterShallowBText = nullptr;
    Noesis::TextBlock* waterDeepRText = nullptr;
    Noesis::TextBlock* waterDeepGText = nullptr;
    Noesis::TextBlock* waterDeepBText = nullptr;
    Noesis::TextBlock* waterDepthColorMinText = nullptr;
    Noesis::TextBlock* waterDepthColorMaxText = nullptr;
    Noesis::TextBlock* waterDepthFadeText = nullptr;
    Noesis::TextBlock* waterRefractionStrengthText = nullptr;
    Noesis::TextBlock* waterRefractionDepthStrengthText = nullptr;
    Noesis::TextBlock* waterFoamDistanceText = nullptr;
    Noesis::TextBlock* waterFoamIntensityText = nullptr;
    Noesis::TextBlock* waterFoamScaleText = nullptr;
    Noesis::TextBlock* waterFoamTerrainThicknessText = nullptr;
    Noesis::TextBlock* waterCausticIntensityText = nullptr;
    Noesis::TextBlock* waterCausticScaleText = nullptr;
    Noesis::TextBlock* waterCausticSpeedText = nullptr;
    Noesis::TextBlock* waterCausticMaxDepthText = nullptr;
    Noesis::Slider* waterLevelSlider = nullptr;
    Noesis::Slider* waterBaseRSlider = nullptr;
    Noesis::Slider* waterBaseGSlider = nullptr;
    Noesis::Slider* waterBaseBSlider = nullptr;
    Noesis::Slider* waterAlphaSlider = nullptr;
    Noesis::Slider* waterWaveScaleSmallSlider = nullptr;
    Noesis::Slider* waterWaveScaleLargeSlider = nullptr;
    Noesis::Slider* waterWaveSpeedSmallSlider = nullptr;
    Noesis::Slider* waterWaveSpeedLargeSlider = nullptr;
    Noesis::Slider* waterNormalStrengthSlider = nullptr;
    Noesis::Slider* waterFresnelPowerSlider = nullptr;
    Noesis::Slider* waterFresnelMinSlider = nullptr;
    Noesis::Slider* waterReflectionRSlider = nullptr;
    Noesis::Slider* waterReflectionGSlider = nullptr;
    Noesis::Slider* waterReflectionBSlider = nullptr;
    Noesis::Slider* waterReflectionDistortionSlider = nullptr;
    Noesis::Button* waterRefractionEnabledButton = nullptr;
    Noesis::Slider* waterShallowRSlider = nullptr;
    Noesis::Slider* waterShallowGSlider = nullptr;
    Noesis::Slider* waterShallowBSlider = nullptr;
    Noesis::Slider* waterDeepRSlider = nullptr;
    Noesis::Slider* waterDeepGSlider = nullptr;
    Noesis::Slider* waterDeepBSlider = nullptr;
    Noesis::Slider* waterDepthColorMinSlider = nullptr;
    Noesis::Slider* waterDepthColorMaxSlider = nullptr;
    Noesis::Slider* waterDepthFadeSlider = nullptr;
    Noesis::Slider* waterRefractionStrengthSlider = nullptr;
    Noesis::Slider* waterRefractionDepthStrengthSlider = nullptr;
    Noesis::Button* waterFoamEnabledButton = nullptr;
    Noesis::Slider* waterFoamDistanceSlider = nullptr;
    Noesis::Slider* waterFoamIntensitySlider = nullptr;
    Noesis::Slider* waterFoamScaleSlider = nullptr;
    Noesis::Slider* waterFoamTerrainThicknessSlider = nullptr;
    Noesis::Button* waterCausticOffButton = nullptr;
    Noesis::Button* waterCausticAnimatedButton = nullptr;
    Noesis::Button* waterCausticProceduralButton = nullptr;
    Noesis::Slider* waterCausticIntensitySlider = nullptr;
    Noesis::Slider* waterCausticScaleSlider = nullptr;
    Noesis::Slider* waterCausticSpeedSlider = nullptr;
    Noesis::Slider* waterCausticMaxDepthSlider = nullptr;
    Noesis::Button* addPointLightButton = nullptr;
    Noesis::Button* addSpotLightButton = nullptr;
    Noesis::TextBlock* dynamicLightCountText = nullptr;
    Noesis::TextBlock* selectedLightTitleText = nullptr;
    Noesis::TextBlock* selectedLightXText = nullptr;
    Noesis::TextBlock* selectedLightYText = nullptr;
    Noesis::TextBlock* selectedLightZText = nullptr;
    Noesis::TextBlock* selectedLightIntensityText = nullptr;
    Noesis::TextBlock* selectedLightRadiusText = nullptr;
    Noesis::TextBlock* selectedLightRText = nullptr;
    Noesis::TextBlock* selectedLightGText = nullptr;
    Noesis::TextBlock* selectedLightBText = nullptr;
    Noesis::TextBlock* selectedSpotPitchText = nullptr;
    Noesis::TextBlock* selectedSpotYawText = nullptr;
    Noesis::TextBlock* selectedSpotInnerText = nullptr;
    Noesis::TextBlock* selectedSpotOuterText = nullptr;
    Noesis::Slider* selectedLightXSlider = nullptr;
    Noesis::Slider* selectedLightYSlider = nullptr;
    Noesis::Slider* selectedLightZSlider = nullptr;
    Noesis::Slider* selectedLightIntensitySlider = nullptr;
    Noesis::Slider* selectedLightRadiusSlider = nullptr;
    Noesis::Slider* selectedLightRSlider = nullptr;
    Noesis::Slider* selectedLightGSlider = nullptr;
    Noesis::Slider* selectedLightBSlider = nullptr;
    Noesis::Slider* selectedSpotPitchSlider = nullptr;
    Noesis::Slider* selectedSpotYawSlider = nullptr;
    Noesis::Slider* selectedSpotInnerSlider = nullptr;
    Noesis::Slider* selectedSpotOuterSlider = nullptr;
    std::array<Noesis::Button*, 8> editorTextureButtons{};
    std::array<Noesis::TextBlock*, 8> editorTextureSlotTexts{};
    std::array<Noesis::Button*, 12> assetButtons{};
    std::array<Noesis::TextBlock*, 12> assetTexts{};
    std::array<Noesis::Image*, 12> assetImages{};
    Noesis::TreeView* assetFolderTree = nullptr;
    Noesis::StackPanel* assetBreadcrumbPanel = nullptr;
    Noesis::TextBlock* assetFolderCountText = nullptr;
    std::array<Noesis::Button*, 6> assetTagButtons{};
    std::array<Noesis::TextBlock*, 6> assetTagTexts{};
    Noesis::TextBlock* assetStatusText = nullptr;
    std::unique_ptr<AssetLibrary> assetLibrary;
    AssetLibrary::Category assetCategory = AssetLibrary::Category::Texture;
    std::vector<AssetLibrary::Entry> visibleAssetEntries;
    std::vector<std::pair<std::string, std::uint32_t>> visibleAssetTags;
    std::vector<std::string> activeAssetTags;
    std::string activeAssetSubpath;
    std::string selectedAssetFolderPath;
    std::string lastClickedAssetFolderPath;
    double lastAssetFolderClickSeconds = -1.0;
    std::set<std::string> expandedAssetFolders;
    std::unordered_map<Noesis::BaseComponent*, std::string> assetFolderItemPaths;
    std::unordered_map<Noesis::BaseComponent*, std::string> assetBreadcrumbButtonPaths;
    std::vector<Noesis::Ptr<Noesis::TreeViewItem>> assetFolderItems;
    std::vector<Noesis::Ptr<Noesis::Button>> assetBreadcrumbButtons;
    std::vector<Noesis::Ptr<Noesis::TextBlock>> assetBreadcrumbSeparators;
    std::string selectedAssetId;
    RenameTargetType renameTargetType = RenameTargetType::None;
    std::string renameTargetId;
    std::string renameOriginalName;
    std::string renameEditText;
    std::string renameRoleConfirmText;
    uint32_t renameAssetIndex = 0;
    Noesis::Button* renameAssetButton = nullptr;
    Noesis::TreeViewItem* renameFolderItem = nullptr;
    Noesis::Ptr<Noesis::BaseComponent> renameOriginalContent;
    Noesis::Ptr<Noesis::TextBox> renameTextBox;
    AssetLibrary::MaterialData materialDraft;
    std::optional<std::string> editingMaterialId;
    bool materialSaveAsEditMode = false;
    DragPayload currentDragPayload;
    DragPayload potentialDragPayload;
    bool potentialAssetDrag = false;
    bool activeAssetDrag = false;
    int dragStartX = 0;
    int dragStartY = 0;
    Noesis::FrameworkElement* dragHighlightElement = nullptr;
    Noesis::Ptr<Noesis::Brush> dragPreviousBorderBrush;
    Noesis::Thickness dragPreviousBorderThickness{};
    uint32_t activeMaterialTextureSlot = 0;
    uint32_t pendingMaterialTargetSlot = 0;
    bool pendingMaterialTargetSlotValid = false;
    std::array<MapEditorPaletteSlot, 8> editorPaletteSlots{};
    std::string assetMapDirectory = "assets/Maps/test_zone";
    std::string loginName;
    std::string loginPassword;
    std::vector<std::uint8_t> pendingEnterWorldToken;
    std::string pendingGameHost;
    std::uint16_t pendingGamePort = 0;
    bool pendingGameConnect = false;
    bool pendingEnterWorldConnect = false;
    bool inWorld = false;
    std::uint32_t ownNetId = 0;
    double currentTimeSeconds = 0.0;
    double previousUpdateTimeSeconds = 0.0;
    double latestServerTimeSeconds = 0.0;
    double latestTransformReceiveLocalTimeSeconds = 0.0;
    std::unique_ptr<flecs::world> entityWorld;
    std::unordered_map<std::uint32_t, flecs::entity> entitiesByNetId;
    client::net::ClientSession* clientSession = nullptr;
    uint32_t currentWidth = 0;
    uint32_t currentHeight = 0;
    bool guiInitialized = false;
    bool lobbyActive = false;
    bool inGameMenuOpen = false;
    bool mapEditorOpen = false;
    std::uint32_t editorTextureSlot = 4;
    std::uint32_t activeBrushSlider = 0;
    float editorBrushRadiusMeters = 5.0f;
    float editorBrushStrength = 1.0f;
    LightingState lightingState;
    WaterConfig waterConfig;
    DynamicLightEditorState dynamicLightEditorState;
    bool lightingControlsUpdating = false;
    bool dynamicLightControlsUpdating = false;
    bool lightingModeActive = false;
    bool lightingMainExpanded = true;
    bool waterBaseExpanded = true;
    bool waterReflectionExpanded = false;
    bool waterRefractionExpanded = false;
    bool waterFoamExpanded = false;
    bool waterCausticExpanded = false;
    bool dynamicLightsExpanded = false;
    bool selectedLightExpanded = false;
    float inspectorManualScrollOffset = 0.0f;
    MapEditorCommands editorCommands;
    std::function<void()> quitCallback;
};

NoesisLayer::NoesisLayer() = default;
NoesisLayer::~NoesisLayer() { Destroy(); }

bool NoesisLayer::Create(VulkanDevice& device, client::asset::IAssetReader& assets, uint32_t width,
    uint32_t height)
{
    Destroy();
    m_impl = std::make_unique<Impl>();
    m_impl->InitializeEntityWorld();

    Noesis::GUI::SetLogHandler([](const char*, uint32_t, uint32_t level, const char*, const char* message)
    {
        const char* prefixes[] = {"T", "D", "I", "W", "E"};
        char buffer[2048];
        std::snprintf(buffer, sizeof(buffer), "[NOESIS/%s] %s", prefixes[level], message);
        Log(buffer);
    });

    Noesis::GUI::Init();
    m_impl->guiInitialized = true;

    Log("Noesis asset root: IAssetReader");
    Noesis::GUI::SetXamlProvider(Noesis::MakePtr<AssetXamlProvider>(assets));
    Noesis::GUI::SetFontProvider(Noesis::MakePtr<AssetFontProvider>(assets));
    Noesis::GUI::SetTextureProvider(Noesis::MakePtr<AssetTextureProvider>(assets));

    NoesisApp::VKFactory::InstanceInfo info{};
    info.instance = device.GetInstance();
    info.physicalDevice = device.GetPhysicalDevice();
    info.device = device.GetDevice();
    info.queueFamilyIndex = device.GetGraphicsQueueFamily();
    info.vkGetInstanceProcAddr = vkGetInstanceProcAddr;

    const bool sRGB = device.IsSwapchainFormatSrgb();
    Log(sRGB ? "Noesis VKRenderDevice using sRGB=true to match the swapchain format."
             : "Noesis VKRenderDevice using sRGB=false because the swapchain is not an _SRGB format.");

    m_impl->renderDevice = NoesisApp::VKFactory::CreateDevice(sRGB, info);
    NoesisApp::VKFactory::SetRenderPass(m_impl->renderDevice, device.GetRenderPass(), VK_SAMPLE_COUNT_1_BIT);
    NoesisApp::VKFactory::WarmUpRenderPass(m_impl->renderDevice, device.GetRenderPass(), VK_SAMPLE_COUNT_1_BIT);

    const bool login = m_impl->LoadLoginView(width, height);
    const bool menu = login ? m_impl->LoadInGameMenuView(width, height) : false;
    const bool editor = menu ? m_impl->LoadMapEditorView(width, height) : false;
    return login && menu && editor;
}

void NoesisLayer::Update(double timeSeconds)
{
#if defined(__ANDROID__)
    DrainPendingTextInput();
#endif
    double dt = 0.0;
    if (m_impl)
    {
        dt = m_impl->previousUpdateTimeSeconds > 0.0 ? timeSeconds - m_impl->previousUpdateTimeSeconds : 0.0;
        m_impl->previousUpdateTimeSeconds = timeSeconds;
        m_impl->currentTimeSeconds = timeSeconds;
    }

    if (m_impl && m_impl->pendingGameConnect && m_impl->clientSession &&
        !m_impl->clientSession->IsConnected())
    {
        m_impl->pendingGameConnect = false;
        m_impl->pendingEnterWorldConnect = true;
        LogFormat("[WORLD] deferred game connect to %s", m_impl->pendingGameHost.c_str());
        m_impl->clientSession->Connect(m_impl->pendingGameHost, m_impl->pendingGamePort);
    }

    if (m_impl)
    {
        m_impl->RunInterpolationSystem(timeSeconds);
        m_impl->RunHealthInterpolation(dt);
    }

    if (m_impl && m_impl->view)
        m_impl->view->Update(timeSeconds);
    if (m_impl && m_impl->menuView && m_impl->inGameMenuOpen)
        m_impl->menuView->Update(timeSeconds);
    if (m_impl && m_impl->editorView && m_impl->mapEditorOpen)
        m_impl->editorView->Update(timeSeconds);
    if (m_impl && m_impl->mapEditorOpen && !m_impl->IsTextInputFocused())
        m_impl->ClearKeyboardFocus();
}

void NoesisLayer::RenderOffscreen(VulkanDevice& device)
{
    if (!m_impl || !m_impl->view || !device.IsFrameActive())
        return;

    NoesisApp::VKFactory::RecordingInfo info{};
    info.commandBuffer = device.GetCommandBuffer();
    info.frameNumber = device.GetFrameNumber() + 2;
    info.safeFrameNumber = device.GetSafeFrameNumber();
    NoesisApp::VKFactory::SetCommandBuffer(m_impl->renderDevice, info);

    m_impl->view->GetRenderer()->UpdateRenderTree();
    m_impl->view->GetRenderer()->RenderOffscreen();
    if (m_impl->menuView && m_impl->inGameMenuOpen)
    {
        m_impl->menuView->GetRenderer()->UpdateRenderTree();
        m_impl->menuView->GetRenderer()->RenderOffscreen();
    }
    if (m_impl->editorView && m_impl->mapEditorOpen)
    {
        m_impl->editorView->GetRenderer()->UpdateRenderTree();
        m_impl->editorView->GetRenderer()->RenderOffscreen();
    }
}

void NoesisLayer::RenderOnscreen(VulkanDevice& device)
{
    if (!m_impl || !device.IsFrameActive())
        return;

    NoesisApp::VKFactory::RecordingInfo info{};
    info.commandBuffer = device.GetCommandBuffer();
    info.frameNumber = device.GetFrameNumber() + 2;
    info.safeFrameNumber = device.GetSafeFrameNumber();
    NoesisApp::VKFactory::SetCommandBuffer(m_impl->renderDevice, info);
    NoesisApp::VKFactory::SetRenderPass(m_impl->renderDevice, device.GetRenderPass(), VK_SAMPLE_COUNT_1_BIT);
    if (m_impl->view)
        m_impl->view->GetRenderer()->Render();
    if (m_impl->inGameMenuOpen && m_impl->menuView)
        m_impl->menuView->GetRenderer()->Render();
    if (m_impl->mapEditorOpen && m_impl->editorView)
        m_impl->editorView->GetRenderer()->Render();
}

void NoesisLayer::OnRenderPassChanged(VulkanDevice& device)
{
    if (!m_impl || !m_impl->renderDevice || device.GetRenderPass() == VK_NULL_HANDLE)
        return;

    NoesisApp::VKFactory::SetRenderPass(m_impl->renderDevice, device.GetRenderPass(), VK_SAMPLE_COUNT_1_BIT);
    NoesisApp::VKFactory::WarmUpRenderPass(m_impl->renderDevice, device.GetRenderPass(), VK_SAMPLE_COUNT_1_BIT);
}

void NoesisLayer::Resize(uint32_t width, uint32_t height)
{
    if (m_impl && m_impl->view)
        m_impl->view->SetSize(width, height);
    if (m_impl && m_impl->menuView)
        m_impl->menuView->SetSize(width, height);
    if (m_impl && m_impl->editorView)
        m_impl->editorView->SetSize(width, height);
    if (m_impl)
    {
        m_impl->currentWidth = width;
        m_impl->currentHeight = height;
    }
}

bool NoesisLayer::IsLobbyActive() const
{
    return m_impl && m_impl->lobbyActive;
}

bool NoesisLayer::IsInWorld() const
{
    return m_impl && m_impl->inWorld;
}

std::uint32_t NoesisLayer::GetOwnNetId() const
{
    return m_impl ? m_impl->ownNetId : 0;
}

std::vector<WorldRenderEntity> NoesisLayer::GetWorldEntities() const
{
    return m_impl ? m_impl->CollectWorldEntities() : std::vector<WorldRenderEntity>{};
}

bool NoesisLayer::IsInGameMenuOpen() const
{
    return m_impl && m_impl->inGameMenuOpen;
}

bool NoesisLayer::IsMapEditorOpen() const
{
    return m_impl && m_impl->mapEditorOpen;
}

void NoesisLayer::ToggleInGameMenu()
{
    if (m_impl)
        m_impl->ToggleInGameMenu();
}

bool NoesisLayer::LoadMapEditorView(uint32_t width, uint32_t height)
{
    return m_impl && m_impl->LoadMapEditorView(width, height);
}

void NoesisLayer::ToggleMapEditor()
{
    if (m_impl)
        m_impl->ToggleMapEditor();
}

void NoesisLayer::ClearKeyboardFocus()
{
    if (m_impl)
        m_impl->ClearKeyboardFocus();
}

bool NoesisLayer::IsTextInputFocused() const
{
    return m_impl && m_impl->IsTextInputFocused();
}

MapEditorSettings NoesisLayer::GetMapEditorSettings() const
{
    return m_impl ? m_impl->GetMapEditorSettings() : MapEditorSettings{};
}

MapEditorCommands NoesisLayer::ConsumeMapEditorCommands()
{
    return m_impl ? m_impl->ConsumeMapEditorCommands() : MapEditorCommands{};
}

LightingState NoesisLayer::GetLightingState() const
{
    return m_impl ? m_impl->lightingState : LightingState{};
}

WaterConfig NoesisLayer::GetWaterConfig() const
{
    return m_impl ? m_impl->waterConfig : WaterConfig{};
}

void NoesisLayer::SetDynamicLightEditorState(const DynamicLightEditorState& state)
{
    if (m_impl)
        m_impl->SetDynamicLightEditorState(state);
}

void NoesisLayer::InitializeAssetLibrary(const std::string& mapDirectory,
                                         const std::array<MapEditorPaletteSlot, 8>& defaultSlots)
{
    if (m_impl)
        m_impl->InitializeAssetLibrary(mapDirectory, defaultSlots);
}

std::array<MapEditorPaletteSlot, 8> NoesisLayer::GetPaletteSlots() const
{
    return m_impl ? m_impl->GetPaletteSlots() : std::array<MapEditorPaletteSlot, 8>{};
}

void NoesisLayer::SetEditorStatus(const std::string& status)
{
    if (m_impl)
        m_impl->SetAssetStatus(status);
}

void NoesisLayer::ImportDroppedFiles(const std::vector<std::string>& paths)
{
    if (m_impl)
        m_impl->ImportDroppedFiles(paths);
}

void NoesisLayer::SetQuitCallback(std::function<void()> callback)
{
    if (m_impl)
        m_impl->quitCallback = std::move(callback);
}

void NoesisLayer::SetClientSession(client::net::ClientSession* session)
{
    if (m_impl)
        m_impl->clientSession = session;
}

bool NoesisLayer::OnInput(const InputEvent& event)
{
    if (!m_impl || !m_impl->view)
        return false;

    Noesis::IView* targetView = m_impl->ActiveInputView();

    if (m_impl->HandleAssetRenameInput(event))
        return true;

    if (m_impl->HandleLoginEnterInput(event))
        return true;

    if (m_impl->HandleAssetDragDropInput(event))
        return true;

    if (m_impl->HandleEditorBrushSliderInput(event))
        return true;

    if (m_impl->HandleInspectorScrollInput(event))
        return true;

    switch (event.type)
    {
    case InputEvent::MouseMove:
        return targetView->MouseMove(event.x, event.y);
    case InputEvent::MouseDown:
        return targetView->MouseButtonDown(event.x, event.y, ToNoesisMouseButton(event.button));
    case InputEvent::MouseUp:
        return targetView->MouseButtonUp(event.x, event.y, ToNoesisMouseButton(event.button));
    case InputEvent::MouseWheel:
        return targetView->MouseWheel(event.x, event.y, event.wheelDelta);
    case InputEvent::KeyDown:
    {
        if (m_impl->mapEditorOpen && !m_impl->IsTextInputFocused())
        {
            m_impl->ClearKeyboardFocus();
            return false;
        }
        Noesis::Key key = ToNoesisKey(event.key);
        return key != Noesis::Key_None ? targetView->KeyDown(key) : false;
    }
    case InputEvent::KeyUp:
    {
        if (m_impl->mapEditorOpen && !m_impl->IsTextInputFocused())
        {
            m_impl->ClearKeyboardFocus();
            return false;
        }
        Noesis::Key key = ToNoesisKey(event.key);
        return key != Noesis::Key_None ? targetView->KeyUp(key) : false;
    }
    case InputEvent::Char:
        if (m_impl->mapEditorOpen && !m_impl->IsTextInputFocused())
        {
            m_impl->ClearKeyboardFocus();
            return false;
        }
        return targetView->Char(event.codepoint);
    case InputEvent::TouchDown:
    case InputEvent::TouchMove:
    case InputEvent::TouchUp:
        return false;
    default:
        return false;
    }
}

void NoesisLayer::OnConnectionFailed(const std::string& reason)
{
    if (m_impl)
    {
        if (m_impl->pendingEnterWorldConnect || m_impl->pendingGameConnect)
            LogFormat("[WORLD] game connect failed: %s", reason.c_str());
        m_impl->SetStatus(reason.empty() ? "Connection failed" : reason.c_str());
        m_impl->pendingGameConnect = false;
        m_impl->pendingEnterWorldConnect = false;
    }
}

void NoesisLayer::OnDisconnected()
{
    if (m_impl && !m_impl->pendingGameConnect)
        m_impl->SetStatus("Disconnected");
}

void NoesisLayer::OnHandshakeAccepted()
{
    if (!m_impl)
        return;

    if (m_impl->pendingEnterWorldConnect)
    {
        m_impl->pendingEnterWorldConnect = false;
        Log("[WORLD] game connect succeeded; sending EnterWorld token");
        m_impl->SetStatus("Entering world...");
        if (m_impl->clientSession)
            m_impl->clientSession->SendEnterWorld(m_impl->pendingEnterWorldToken);
        return;
    }

    m_impl->SetStatus("Logging in...");
    if (m_impl->clientSession)
        m_impl->clientSession->SendLogin(m_impl->loginName, m_impl->loginPassword);
}

void NoesisLayer::OnHandshakeRejected(const std::string& reason)
{
    if (m_impl)
        m_impl->SetStatus(reason.empty() ? "Handshake rejected" : reason.c_str());
}

void NoesisLayer::OnLoginAccepted(uint64_t account_id)
{
    if (!m_impl)
        return;

    LogFormat("[LOBBY] login accepted account_id=%llu",
        static_cast<unsigned long long>(account_id));
    m_impl->LoadLobbyView(m_impl->currentWidth, m_impl->currentHeight);
    if (m_impl->clientSession)
        m_impl->clientSession->SendCharacterListRequest();
}

void NoesisLayer::OnLoginRejected(const std::string& reason)
{
    if (m_impl)
        m_impl->SetStatus(reason.empty() ? "Login failed" : reason.c_str());
}

void NoesisLayer::OnCharacterList(const std::vector<client::net::CharacterListItem>& characters)
{
    if (!m_impl || !m_impl->lobbyViewModel)
        return;

    m_impl->lobbyViewModel->OnCharacterList(characters);
}

void NoesisLayer::OnEnterWorldToken(std::vector<std::uint8_t> token,
                                    const std::string& host,
                                    std::uint16_t port)
{
    if (!m_impl || !m_impl->clientSession)
        return;

    LogFormat("[WORLD] received handoff token, deferring game connect to %s", host.c_str());
    m_impl->pendingEnterWorldToken = std::move(token);
    m_impl->pendingGameHost = host;
    m_impl->pendingGamePort = port;
    m_impl->pendingGameConnect = true;
    m_impl->pendingEnterWorldConnect = false;
    m_impl->SetStatus("Connecting to world...");
    m_impl->clientSession->Disconnect();
}

void NoesisLayer::OnEnterWorldAccepted(std::uint32_t net_id, client::net::Vec3 spawn_pos)
{
    if (!m_impl)
        return;

    m_impl->inWorld = true;
    m_impl->lobbyActive = false;
    m_impl->ownNetId = net_id;
    m_impl->latestServerTimeSeconds = 0.0;
    m_impl->latestTransformReceiveLocalTimeSeconds = m_impl->currentTimeSeconds;
    m_impl->ClearWorldEntities();
    m_impl->UpsertWorldEntity(net_id, "You", spawn_pos, 0, client::net::MoveState::Idle, true, 0, 1);
    m_impl->LoadWorldHudView(m_impl->currentWidth, m_impl->currentHeight);

    LogFormat("[WORLD] enter accepted net_id=%u", net_id);
    char message[128];
    std::snprintf(message,
                  sizeof(message),
                  "Entered world at %.2f, %.2f",
                  spawn_pos.x,
                  spawn_pos.y);
    if (m_impl->lobbyViewModel)
        m_impl->lobbyViewModel->SetStatus(message);
}

void NoesisLayer::OnEnterWorldRejected(const std::string& reason)
{
    if (m_impl)
        m_impl->SetStatus(reason.empty() ? "Enter world failed" : reason.c_str());
}

void NoesisLayer::OnEntitySpawn(const client::net::EntitySpawnInfo& entity)
{
    if (m_impl)
        m_impl->UpsertWorldEntity(entity.netId,
            entity.name.c_str(),
            entity.spawnPos,
            entity.heading,
            client::net::MoveState::Idle,
            entity.netId == m_impl->ownNetId,
            entity.mobTypeId,
            entity.level,
            entity.hpCurrent,
            entity.hpMax);

    LogFormat("[WORLD] entity spawn name=%s", entity.name.c_str());
}

void NoesisLayer::OnEntityDespawn(std::uint32_t net_id)
{
    if (m_impl)
        m_impl->RemoveWorldEntity(net_id);

    LogFormat("[WORLD] entity despawn net_id=%u", net_id);
}

void NoesisLayer::OnEntityHealthUpdate(const client::net::EntityHealthInfo& health)
{
    if (!m_impl)
        return;

    const auto it = m_impl->entitiesByNetId.find(health.netId);
    if (it == m_impl->entitiesByNetId.end() || !it->second.is_valid())
        return;

    client::ecs::Health current;
    if (it->second.has<client::ecs::Health>())
        current = it->second.get<client::ecs::Health>();
    current.current = health.hpCurrent;
    current.max = health.hpMax <= 0.0f ? 1.0f : health.hpMax;
    current.displayed = std::clamp(current.displayed, 0.0f, current.max);
    it->second.set<client::ecs::Health>(current);
}

void NoesisLayer::OnEntityDeath(std::uint32_t net_id, std::uint32_t killer_net_id)
{
    (void)killer_net_id;
    if (m_impl)
        m_impl->RemoveWorldEntity(net_id);

    LogFormat("[WORLD] entity death net_id=%u killer=%u", net_id, killer_net_id);
}

void NoesisLayer::OnEntityTransforms(std::uint32_t server_tick,
                                     const std::vector<client::net::EntityTransform>& transforms)
{
    if (!m_impl)
        return;

    const double serverTimeSeconds = static_cast<double>(server_tick) * kServerTickSeconds;
    m_impl->latestServerTimeSeconds = serverTimeSeconds;
    m_impl->latestTransformReceiveLocalTimeSeconds = m_impl->currentTimeSeconds;

    for (const auto& transform : transforms)
    {
        const auto it = m_impl->entitiesByNetId.find(transform.netId);
        const char* name = it == m_impl->entitiesByNetId.end() ? "Player" : nullptr;
        if (it == m_impl->entitiesByNetId.end())
        {
            m_impl->UpsertWorldEntity(transform.netId,
                name,
                transform.position,
                transform.heading,
                transform.moveState,
                transform.netId == m_impl->ownNetId,
                0,
                1,
                1.0f,
                1.0f);
            continue;
        }

        if (it->second.has<client::ecs::LocalPlayerTag>())
            m_impl->ApplyLatestTransform(it->second, transform);
        else
            m_impl->BufferRemoteTransform(it->second, transform, serverTimeSeconds);
    }
}

void NoesisLayer::Destroy()
{
    if (!m_impl)
        return;

    m_impl->ReleaseCachedNoesisReferences();

    if (m_impl->view)
    {
        m_impl->view->GetRenderer()->Shutdown();
        m_impl->view.Reset();
    }
    if (m_impl->menuView)
    {
        m_impl->menuView->GetRenderer()->Shutdown();
        m_impl->menuView.Reset();
    }
    if (m_impl->editorView)
    {
        m_impl->editorView->GetRenderer()->Shutdown();
        m_impl->editorView.Reset();
    }

    m_impl->renderDevice.Reset();

    if (m_impl->guiInitialized)
    {
        Noesis::GUI::Shutdown();
        m_impl->guiInitialized = false;
    }

    m_impl.reset();
}
