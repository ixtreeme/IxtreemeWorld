#include "NoesisLayer.h"
#include "VulkanDevice.h"
#include "Debug.h"
#include "asset/IAssetReader.h"
#include "network/ClientSession.h"
#if defined(__ANDROID__)
#include "SoftKeyboard.h"
#include <jni.h>
#endif

#include <NsApp/DelegateCommand.h>
#include <NsApp/NotifyPropertyChangedBase.h>
#include <NsCore/Ptr.h>
#include <NsCore/ReflectionImplement.h>
#include <NsCore/String.h>
#include <NsGui/CachedFontProvider.h>
#include <NsGui/Button.h>
#include <NsGui/CheckBox.h>
#include <NsGui/Enums.h>
#include <NsGui/FrameworkElement.h>
#include <NsGui/IntegrationAPI.h>
#include <NsGui/InputEnums.h>
#include <NsGui/IRenderer.h>
#include <NsGui/IView.h>
#include <NsGui/MemoryStream.h>
#include <NsGui/ObservableCollection.h>
#include <NsGui/PasswordBox.h>
#include <NsGui/RoutedEvent.h>
#include <NsGui/Stream.h>
#include <NsGui/TextBlock.h>
#include <NsGui/TextBox.h>
#include <NsGui/UIElementEvents.h>
#include <NsGui/Uri.h>
#include <NsGui/XamlProvider.h>
#include <NsCore/Delegate.h>
#include <NsCore/Nullable.h>
#include <NsRender/RenderDevice.h>
#include <NsRender/VKFactory.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
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
        : m_slot(info.slot)
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

    uint32_t GetSlot() const { return m_slot; }
    const char* GetName() const { return m_name.Str(); }
    const char* GetDetail() const { return m_detail.Str(); }
    uint32_t GetLevel() const { return m_level; }
    uint32_t GetClassId() const { return m_classId; }
    bool GetCanEnter() const { return !m_empty; }

private:
    uint32_t m_slot = 0;
    Noesis::String m_name;
    Noesis::String m_detail;
    uint32_t m_level = 0;
    uint32_t m_classId = 0;
    bool m_empty = true;

    NS_IMPLEMENT_INLINE_REFLECTION(LobbyCharacter, Noesis::BaseComponent, "Lobby.Character")
    {
        NsProp("Slot", &LobbyCharacter::GetSlot);
        NsProp("Name", &LobbyCharacter::GetName);
        NsProp("Detail", &LobbyCharacter::GetDetail);
        NsProp("Level", &LobbyCharacter::GetLevel);
        NsProp("ClassId", &LobbyCharacter::GetClassId);
        NsProp("CanEnter", &LobbyCharacter::GetCanEnter);
    }
};

class LobbyChannel final : public Noesis::BaseComponent
{
public:
    LobbyChannel(uint32_t channel, uint32_t port)
        : m_channel(channel)
        , m_port(port)
    {
        char label[64];
        std::snprintf(label, sizeof(label), "CH%u  (%u)", m_channel, m_port);
        m_label = label;
    }

    uint32_t GetChannel() const { return m_channel; }
    uint32_t GetPort() const { return m_port; }
    const char* GetLabel() const { return m_label.Str(); }

private:
    uint32_t m_channel = 1;
    uint32_t m_port = 30078; //ch99
    Noesis::String m_label;

    NS_IMPLEMENT_INLINE_REFLECTION(LobbyChannel, Noesis::BaseComponent, "Lobby.Channel")
    {
        NsProp("Channel", &LobbyChannel::GetChannel);
        NsProp("Port", &LobbyChannel::GetPort);
        NsProp("Label", &LobbyChannel::GetLabel);
    }
};

class LobbyViewModel final : public NoesisApp::NotifyPropertyChangedBase
{
public:
    LobbyViewModel()
    {
        m_characters = *new Noesis::ObservableCollection<LobbyCharacter>();
        m_channels = *new Noesis::ObservableCollection<LobbyChannel>();
        m_channels->Add(Noesis::MakePtr<LobbyChannel>(1, 30078)); //ch99
        m_channels->Add(Noesis::MakePtr<LobbyChannel>(2, 30058));
        m_channels->Add(Noesis::MakePtr<LobbyChannel>(3, 30062));
        m_channels->Add(Noesis::MakePtr<LobbyChannel>(4, 30066));
        m_channels->Add(Noesis::MakePtr<LobbyChannel>(5, 30070));
        m_channels->Add(Noesis::MakePtr<LobbyChannel>(6, 30074));
        m_selectedChannel = m_channels->Get(0);
        SetStatus("Fetching characters...");
        m_enterCommand.SetExecuteFunc(Noesis::MakeDelegate(this, &LobbyViewModel::Enter));
    }

    Noesis::ObservableCollection<LobbyCharacter>* GetCharacters() const { return m_characters; }
    Noesis::ObservableCollection<LobbyChannel>* GetChannels() const { return m_channels; }
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
    LobbyChannel* GetSelectedChannel() const { return m_selectedChannel; }
    void SetSelectedChannel(LobbyChannel* value)
    {
        if (m_selectedChannel != value)
        {
            m_selectedChannel = value;
            OnPropertyChanged("SelectedChannel");
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

        SetStatus("Select a character and channel");
        OnPropertyChanged("Characters");
        OnPropertyChanged("SelectedCharacter");
        OnPropertyChanged("CanEnter");
    }

private:
    void Enter(Noesis::BaseComponent*)
    {
        if (!m_selectedCharacter || !m_selectedChannel || !m_selectedCharacter->GetCanEnter())
        {
            SetStatus("Select a character");
            return;
        }

        SetStatus("World entry is not available yet");
    }

private:
    Noesis::Ptr<Noesis::ObservableCollection<LobbyCharacter>> m_characters;
    Noesis::Ptr<Noesis::ObservableCollection<LobbyChannel>> m_channels;
    LobbyCharacter* m_selectedCharacter = nullptr;
    LobbyChannel* m_selectedChannel = nullptr;
    NoesisApp::DelegateCommand m_enterCommand;
    Noesis::String m_status;
    uint32_t m_characterCount = 0;
    uint32_t m_filledCharacterCount = 0;

    NS_IMPLEMENT_INLINE_REFLECTION(LobbyViewModel, NoesisApp::NotifyPropertyChangedBase, "Lobby.ViewModel")
    {
        NsProp("Characters", &LobbyViewModel::GetCharacters);
        NsProp("Channels", &LobbyViewModel::GetChannels);
        NsProp("SelectedCharacter", &LobbyViewModel::GetSelectedCharacter, &LobbyViewModel::SetSelectedCharacter);
        NsProp("SelectedChannel", &LobbyViewModel::GetSelectedChannel, &LobbyViewModel::SetSelectedChannel);
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

        lobbyViewModel = Noesis::MakePtr<LobbyViewModel>();
        root->SetDataContext(lobbyViewModel);
        const bool created = CreateView(root, width, height);
        lobbyActive = created;
        return created;
    }

    void ToggleInGameMenu()
    {
        inGameMenuOpen = !inGameMenuOpen;
        LogFormat("[MENU] in-game menu %s", inGameMenuOpen ? "open" : "closed");
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

    void OnLoginClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
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

    Noesis::Ptr<Noesis::RenderDevice> renderDevice;
    Noesis::Ptr<Noesis::IView> view;
    Noesis::Ptr<Noesis::IView> menuView;
    Noesis::Ptr<LobbyViewModel> lobbyViewModel;
    Noesis::TextBox* usernameBox = nullptr;
    Noesis::PasswordBox* passwordBox = nullptr;
    Noesis::CheckBox* rememberBox = nullptr;
    Noesis::TextBlock* statusText = nullptr;
    std::string loginName;
    std::string loginPassword;
    client::net::ClientSession* clientSession = nullptr;
    uint32_t currentWidth = 0;
    uint32_t currentHeight = 0;
    bool guiInitialized = false;
    bool lobbyActive = false;
    bool inGameMenuOpen = false;
    std::function<void()> quitCallback;
};

NoesisLayer::NoesisLayer() = default;
NoesisLayer::~NoesisLayer() { Destroy(); }

bool NoesisLayer::Create(VulkanDevice& device, client::asset::IAssetReader& assets, uint32_t width,
    uint32_t height)
{
    Destroy();
    m_impl = std::make_unique<Impl>();

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
    return login && menu;
}

void NoesisLayer::Update(double timeSeconds)
{
#if defined(__ANDROID__)
    DrainPendingTextInput();
#endif
    if (m_impl && m_impl->view)
        m_impl->view->Update(timeSeconds);
    if (m_impl && m_impl->menuView && m_impl->inGameMenuOpen)
        m_impl->menuView->Update(timeSeconds);
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

bool NoesisLayer::IsInGameMenuOpen() const
{
    return m_impl && m_impl->inGameMenuOpen;
}

void NoesisLayer::ToggleInGameMenu()
{
    if (m_impl)
        m_impl->ToggleInGameMenu();
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

    Noesis::IView* targetView = m_impl->view.GetPtr();
    if (m_impl->inGameMenuOpen && m_impl->menuView)
        targetView = m_impl->menuView.GetPtr();

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
        Noesis::Key key = ToNoesisKey(event.key);
        return key != Noesis::Key_None ? targetView->KeyDown(key) : false;
    }
    case InputEvent::KeyUp:
    {
        Noesis::Key key = ToNoesisKey(event.key);
        return key != Noesis::Key_None ? targetView->KeyUp(key) : false;
    }
    case InputEvent::Char:
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
        m_impl->SetStatus(reason.empty() ? "Connection failed" : reason.c_str());
}

void NoesisLayer::OnDisconnected()
{
    if (m_impl)
        m_impl->SetStatus("Disconnected");
}

void NoesisLayer::OnHandshakeAccepted()
{
    if (!m_impl)
        return;

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

void NoesisLayer::Destroy()
{
    if (!m_impl)
        return;

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

    m_impl->renderDevice.Reset();

    if (m_impl->guiInitialized)
    {
        Noesis::GUI::Shutdown();
        m_impl->guiInitialized = false;
    }

    m_impl.reset();
}
