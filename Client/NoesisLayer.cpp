#include "NoesisLayer.h"
#include "VulkanDevice.h"
#include "network/ClientSession.h"

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
#include <NsGui/ObservableCollection.h>
#include <NsGui/PasswordBox.h>
#include <NsGui/RoutedEvent.h>
#include <NsGui/Stream.h>
#include <NsGui/TextBlock.h>
#include <NsGui/TextBox.h>
#include <NsGui/Uri.h>
#include <NsGui/XamlProvider.h>
#include <NsCore/Delegate.h>
#include <NsCore/Nullable.h>
#include <NsRender/RenderDevice.h>
#include <NsRender/VKFactory.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <unordered_map>
#include <string>
#include <utility>
#include <windows.h>

namespace
{
void Log(const char* text)
{
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
    std::fprintf(stderr, "%s\n", text);
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

std::string ExecutableDirectory()
{
    char path[MAX_PATH]{};
    DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH)
        return ".";

    std::string result(path, length);
    size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? "." : result.substr(0, slash);
}

std::string JoinPath(const std::string& left, const std::string& right)
{
    if (left.empty())
        return right;

    char last = left.back();
    if (last == '\\' || last == '/')
        return left + right;

    return left + "\\" + right;
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

class FileXamlProvider final : public Noesis::XamlProvider
{
public:
    explicit FileXamlProvider(std::string root) : m_root(std::move(root)) {}

private:
    Noesis::Ptr<Noesis::Stream> LoadXaml(const Noesis::Uri& uri) override
    {
        Noesis::FixedString<512> path;
        uri.GetPath(path);

        std::string filename = JoinPath(JoinPath(m_root, "xaml"), path.Str());
        return Noesis::OpenFileStream(filename.c_str());
    }

    std::string m_root;
};

class FileFontProvider final : public Noesis::CachedFontProvider
{
public:
    explicit FileFontProvider(std::string root) : m_root(std::move(root)) {}

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

        std::string fullPath = m_root;
        if (path.Str()[0] != '\0')
            fullPath = JoinPath(fullPath, path.Str());

        fullPath = JoinPath(fullPath, filename);
        return Noesis::OpenFileStream(fullPath.c_str());
    }

    std::string m_root;
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

void AppendUtf8(std::string& out, uint32_t codepoint)
{
    if (codepoint <= 0x7Fu)
    {
        out.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FFu)
    {
        out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
    else if (codepoint <= 0xFFFFu)
    {
        out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
}

std::string Cp1250ToUtf8(const char* src)
{
    static constexpr uint32_t kCp1250High[128] =
    {
        0x20AC, 0x0081, 0x201A, 0x0083, 0x201E, 0x2026, 0x2020, 0x2021,
        0x0088, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x0098, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
        0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
        0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
        0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
        0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
        0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
        0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
        0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
        0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
        0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
        0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
        0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
        0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
    };

    std::string out;
    if (!src)
        return out;

    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(src); *p != 0; ++p)
    {
        const uint32_t codepoint = *p < 0x80 ? *p : kCp1250High[*p - 0x80];
        AppendUtf8(out, codepoint);
    }
    return out;
}

int QuestPanelTimeSeconds()
{
    return static_cast<int>(std::time(nullptr));
}

class QuestListItem final : public Noesis::BaseComponent
{
public:
    explicit QuestListItem(const TQuestInfo& quest)
        : m_index(quest.index)
        , m_title(quest.title[0] ? Cp1250ToUtf8(quest.title).c_str() : "Quest")
    {
        char line[128]{};
        if (quest.hasCounter && quest.counterName[0])
        {
            const std::string counterName = Cp1250ToUtf8(quest.counterName);
            m_counterText = counterName.c_str();
            m_counterText += ": ";
            std::snprintf(line, sizeof(line), "%d", quest.counterValue);
            m_counterText += line;
        }
        else if (quest.hasCounter)
        {
            std::snprintf(line, sizeof(line), "Counter: %d", quest.counterValue);
            m_counterText = line;
        }

        if (quest.hasClock && quest.clockName[0])
        {
            const std::string clockName = Cp1250ToUtf8(quest.clockName);
            m_clockText = clockName.c_str();
            m_clockText += ": ";
            std::snprintf(line, sizeof(line), "%d", quest.clockValue);
            m_clockText += line;
        }
        else if (quest.hasClock)
        {
            std::snprintf(line, sizeof(line), "Time: %d", quest.clockValue);
            m_clockText = line;
        }
    }

    uint32_t GetIndex() const { return m_index; }
    const char* GetTitle() const { return m_title.Str(); }
    const char* GetCounterText() const { return m_counterText.Str(); }
    const char* GetClockText() const { return m_clockText.Str(); }

private:
    uint32_t m_index = 0;
    Noesis::String m_title;
    Noesis::String m_counterText;
    Noesis::String m_clockText;

    NS_IMPLEMENT_INLINE_REFLECTION(QuestListItem, Noesis::BaseComponent, "Quest.Item")
    {
        NsProp("Index", &QuestListItem::GetIndex);
        NsProp("Title", &QuestListItem::GetTitle);
        NsProp("CounterText", &QuestListItem::GetCounterText);
        NsProp("ClockText", &QuestListItem::GetClockText);
    }
};

class QuestPanelViewModel final : public NoesisApp::NotifyPropertyChangedBase
{
public:
    QuestPanelViewModel()
    {
        m_quests = *new Noesis::ObservableCollection<QuestListItem>();
    }

    Noesis::ObservableCollection<QuestListItem>* GetQuests() const { return m_quests; }
    QuestListItem* GetSelectedQuest() const { return m_selectedQuest; }
    void SetSelectedQuest(QuestListItem* value)
    {
        if (m_selectedQuest == value)
            return;

        m_selectedQuest = value;
        m_selectedIndex = value ? static_cast<uint16_t>(value->GetIndex()) : 0;
        m_hasSelection = value != nullptr;
        RefreshDetail();
        OnPropertyChanged("SelectedQuest");
    }
    const char* GetDetailTitle() const { return m_detailTitle.Str(); }
    const char* GetDetailProgress() const { return m_detailProgress.Str(); }
    const char* GetDetailTimeRemaining() const { return m_detailTimeRemaining.Str(); }
    const char* GetDetailIconFile() const { return m_detailIconFile.Str(); }

    void SetQuests(const std::unordered_map<uint16_t, TQuestInfo>& quests)
    {
        m_rawQuests = quests;
        m_quests->Clear();
        std::vector<uint16_t> indices;
        indices.reserve(quests.size());
        for (const auto& item : quests)
            indices.push_back(item.first);
        std::sort(indices.begin(), indices.end());
        for (uint16_t index : indices)
            m_quests->Add(Noesis::MakePtr<QuestListItem>(quests.at(index)));

        if (!m_hasSelection || quests.find(m_selectedIndex) == quests.end())
        {
            m_hasSelection = !indices.empty();
            m_selectedIndex = m_hasSelection ? indices.front() : 0;
        }

        m_selectedQuest = nullptr;
        for (uint32_t i = 0; i < m_quests->Count(); ++i)
        {
            QuestListItem* item = m_quests->Get(i);
            if (item && item->GetIndex() == m_selectedIndex)
            {
                m_selectedQuest = item;
                break;
            }
        }

        RefreshDetail();
        OnPropertyChanged("Quests");
        OnPropertyChanged("SelectedQuest");
    }

    void RefreshDetail()
    {
        const auto it = m_rawQuests.find(m_selectedIndex);
        if (!m_hasSelection || it == m_rawQuests.end())
        {
            m_detailTitle = "";
            m_detailProgress = "";
            m_detailTimeRemaining = "";
            m_detailIconFile = "";
            NotifyDetailChanged();
            return;
        }

        const TQuestInfo& quest = it->second;
        m_detailTitle = quest.title[0] ? Cp1250ToUtf8(quest.title).c_str() : "Quest";

        char line[128]{};
        if (quest.hasCounter && quest.counterName[0])
        {
            const std::string counterName = Cp1250ToUtf8(quest.counterName);
            std::snprintf(line, sizeof(line), "%s: %d", counterName.c_str(), quest.counterValue);
            m_detailProgress = line;
        }
        else if (quest.hasCounter)
        {
            std::snprintf(line, sizeof(line), "Progress: %d", quest.counterValue);
            m_detailProgress = line;
        }
        else
        {
            m_detailProgress = "";
        }

        if (quest.hasClock && (quest.clockValue > 0 || quest.clockName[0]))
        {
            const int remaining = std::max(0, quest.startTime + quest.clockValue - QuestPanelTimeSeconds());
            const int minutes = remaining / 60;
            const int seconds = remaining % 60;
            if (minutes > 0)
                std::snprintf(line, sizeof(line), "Time Remaining: %d:%02d", minutes, seconds);
            else
                std::snprintf(line, sizeof(line), "Time Remaining: %ds", seconds);
            m_detailTimeRemaining = line;
        }
        else
        {
            m_detailTimeRemaining = "";
        }

        m_detailIconFile = quest.iconFile[0] ? Cp1250ToUtf8(quest.iconFile).c_str() : "";
        NotifyDetailChanged();
    }

private:
    Noesis::Ptr<Noesis::ObservableCollection<QuestListItem>> m_quests;
    QuestListItem* m_selectedQuest = nullptr;
    uint16_t m_selectedIndex = 0;
    bool m_hasSelection = false;
    std::unordered_map<uint16_t, TQuestInfo> m_rawQuests;
    Noesis::String m_detailTitle;
    Noesis::String m_detailProgress;
    Noesis::String m_detailTimeRemaining;
    Noesis::String m_detailIconFile;

    void NotifyDetailChanged()
    {
        OnPropertyChanged("DetailTitle");
        OnPropertyChanged("DetailProgress");
        OnPropertyChanged("DetailTimeRemaining");
        OnPropertyChanged("DetailIconFile");
    }

    NS_IMPLEMENT_INLINE_REFLECTION(QuestPanelViewModel, NoesisApp::NotifyPropertyChangedBase, "Quest.Panel")
    {
        NsProp("Quests", &QuestPanelViewModel::GetQuests);
        NsProp("SelectedQuest", &QuestPanelViewModel::GetSelectedQuest, &QuestPanelViewModel::SetSelectedQuest);
        NsProp("DetailTitle", &QuestPanelViewModel::GetDetailTitle);
        NsProp("DetailProgress", &QuestPanelViewModel::GetDetailProgress);
        NsProp("DetailTimeRemaining", &QuestPanelViewModel::GetDetailTimeRemaining);
        NsProp("DetailIconFile", &QuestPanelViewModel::GetDetailIconFile);
    }
};

class CharacterPanelViewModel final : public NoesisApp::NotifyPropertyChangedBase
{
public:
    CharacterPanelViewModel()
    {
        m_vitCommand.SetCanExecuteFunc(Noesis::MakeDelegate(this, &CharacterPanelViewModel::CanExecuteStatUp));
        m_vitCommand.SetExecuteFunc(Noesis::MakeDelegate(this, &CharacterPanelViewModel::StatUpVit));
        m_intCommand.SetCanExecuteFunc(Noesis::MakeDelegate(this, &CharacterPanelViewModel::CanExecuteStatUp));
        m_intCommand.SetExecuteFunc(Noesis::MakeDelegate(this, &CharacterPanelViewModel::StatUpInt));
        m_strCommand.SetCanExecuteFunc(Noesis::MakeDelegate(this, &CharacterPanelViewModel::CanExecuteStatUp));
        m_strCommand.SetExecuteFunc(Noesis::MakeDelegate(this, &CharacterPanelViewModel::StatUpStr));
        m_dexCommand.SetCanExecuteFunc(Noesis::MakeDelegate(this, &CharacterPanelViewModel::CanExecuteStatUp));
        m_dexCommand.SetExecuteFunc(Noesis::MakeDelegate(this, &CharacterPanelViewModel::StatUpDex));
        Refresh();
    }

    void SetPoints(const std::array<int64_t, POINT_MAX_NUM>& points)
    {
        m_points = points;
        Refresh();
        NotifyAllChanged();
    }

    const char* GetLevel() const { return m_level.Str(); }
    const char* GetExp() const { return m_exp.Str(); }
    const char* GetNextExp() const { return m_nextExp.Str(); }
    const char* GetHp() const { return m_hp.Str(); }
    const char* GetSp() const { return m_sp.Str(); }
    const char* GetVit() const { return m_vit.Str(); }
    const char* GetInt() const { return m_int.Str(); }
    const char* GetStr() const { return m_str.Str(); }
    const char* GetDex() const { return m_dex.Str(); }
    const char* GetStatPoints() const { return m_statPoints.Str(); }
    const char* GetAttackDamage() const { return m_attackDamage.Str(); }
    const char* GetDefence() const { return m_defence.Str(); }
    const char* GetMovingSpeed() const { return m_movingSpeed.Str(); }
    const char* GetAttackRate() const { return m_attackRate.Str(); }
    const char* GetCastingSpeed() const { return m_castingSpeed.Str(); }
    const char* GetMagicDamage() const { return m_magicDamage.Str(); }
    const char* GetMagicDefence() const { return m_magicDefence.Str(); }
    const char* GetEvading() const { return m_evading.Str(); }
    bool GetCanStatUp() const { return Point(POINT_STAT) > 0; }
    const NoesisApp::DelegateCommand* GetVitCommand() const { return &m_vitCommand; }
    const NoesisApp::DelegateCommand* GetIntCommand() const { return &m_intCommand; }
    const NoesisApp::DelegateCommand* GetStrCommand() const { return &m_strCommand; }
    const NoesisApp::DelegateCommand* GetDexCommand() const { return &m_dexCommand; }

private:
    std::array<int64_t, POINT_MAX_NUM> m_points{};
    Noesis::String m_level;
    Noesis::String m_exp;
    Noesis::String m_nextExp;
    Noesis::String m_hp;
    Noesis::String m_sp;
    Noesis::String m_vit;
    Noesis::String m_int;
    Noesis::String m_str;
    Noesis::String m_dex;
    Noesis::String m_statPoints;
    Noesis::String m_attackDamage;
    Noesis::String m_defence;
    Noesis::String m_movingSpeed;
    Noesis::String m_attackRate;
    Noesis::String m_castingSpeed;
    Noesis::String m_magicDamage;
    Noesis::String m_magicDefence;
    Noesis::String m_evading;
    NoesisApp::DelegateCommand m_vitCommand;
    NoesisApp::DelegateCommand m_intCommand;
    NoesisApp::DelegateCommand m_strCommand;
    NoesisApp::DelegateCommand m_dexCommand;

    int64_t Point(uint32_t index) const
    {
        return index < m_points.size() ? m_points[index] : 0;
    }

    static Noesis::String Number(int64_t value)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
        return buffer;
    }

    static Noesis::String Pair(int64_t current, int64_t maximum)
    {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%lld / %lld",
            static_cast<long long>(current),
            static_cast<long long>(maximum));
        return buffer;
    }

    static Noesis::String Range(int64_t minimum, int64_t maximum)
    {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%lld - %lld",
            static_cast<long long>(minimum),
            static_cast<long long>(maximum));
        return buffer;
    }

    void Refresh()
    {
        m_level = Number(Point(POINT_LEVEL));
        m_exp = Number(Point(POINT_EXP));
        m_nextExp = Number(Point(POINT_NEXT_EXP));
        m_hp = Pair(Point(POINT_HP), Point(POINT_MAX_HP));
        m_sp = Pair(Point(POINT_SP), Point(POINT_MAX_SP));
        m_vit = Number(Point(POINT_HT));
        m_int = Number(Point(POINT_IQ));
        m_str = Number(Point(POINT_ST));
        m_dex = Number(Point(POINT_DX));
        m_statPoints = Number(Point(POINT_STAT));
        m_attackDamage = Range(Point(POINT_WEAPON_MIN), Point(POINT_WEAPON_MAX));
        m_defence = Number(Point(POINT_DEF_GRADE));
        m_movingSpeed = Number(Point(POINT_MOV_SPEED));
        m_attackRate = Number(Point(POINT_ATT_SPEED));
        m_castingSpeed = Number(Point(POINT_CASTING_SPEED));
        m_magicDamage = Number(Point(POINT_MAGIC_ATT_GRADE));
        m_magicDefence = Number(Point(POINT_MAGIC_DEF_GRADE));
        m_evading = Number(Point(POINT_DODGE));
    }

    void NotifyAllChanged()
    {
        OnPropertyChanged("Level");
        OnPropertyChanged("Exp");
        OnPropertyChanged("NextExp");
        OnPropertyChanged("Hp");
        OnPropertyChanged("Sp");
        OnPropertyChanged("Vit");
        OnPropertyChanged("Int");
        OnPropertyChanged("Str");
        OnPropertyChanged("Dex");
        OnPropertyChanged("StatPoints");
        OnPropertyChanged("AttackDamage");
        OnPropertyChanged("Defence");
        OnPropertyChanged("MovingSpeed");
        OnPropertyChanged("AttackRate");
        OnPropertyChanged("CastingSpeed");
        OnPropertyChanged("MagicDamage");
        OnPropertyChanged("MagicDefence");
        OnPropertyChanged("Evading");
        OnPropertyChanged("CanStatUp");
    }

    bool CanExecuteStatUp(Noesis::BaseComponent*) const
    {
        return GetCanStatUp();
    }

    void SendStatUp(uint8_t pointIndex)
    {
        (void)pointIndex;
    }

    void StatUpVit(Noesis::BaseComponent*) { SendStatUp(POINT_HT); }
    void StatUpInt(Noesis::BaseComponent*) { SendStatUp(POINT_IQ); }
    void StatUpStr(Noesis::BaseComponent*) { SendStatUp(POINT_ST); }
    void StatUpDex(Noesis::BaseComponent*) { SendStatUp(POINT_DX); }

    NS_IMPLEMENT_INLINE_REFLECTION(CharacterPanelViewModel, NoesisApp::NotifyPropertyChangedBase, "Character.Panel")
    {
        NsProp("Level", &CharacterPanelViewModel::GetLevel);
        NsProp("Exp", &CharacterPanelViewModel::GetExp);
        NsProp("NextExp", &CharacterPanelViewModel::GetNextExp);
        NsProp("Hp", &CharacterPanelViewModel::GetHp);
        NsProp("Sp", &CharacterPanelViewModel::GetSp);
        NsProp("Vit", &CharacterPanelViewModel::GetVit);
        NsProp("Int", &CharacterPanelViewModel::GetInt);
        NsProp("Str", &CharacterPanelViewModel::GetStr);
        NsProp("Dex", &CharacterPanelViewModel::GetDex);
        NsProp("StatPoints", &CharacterPanelViewModel::GetStatPoints);
        NsProp("AttackDamage", &CharacterPanelViewModel::GetAttackDamage);
        NsProp("Defence", &CharacterPanelViewModel::GetDefence);
        NsProp("MovingSpeed", &CharacterPanelViewModel::GetMovingSpeed);
        NsProp("AttackRate", &CharacterPanelViewModel::GetAttackRate);
        NsProp("CastingSpeed", &CharacterPanelViewModel::GetCastingSpeed);
        NsProp("MagicDamage", &CharacterPanelViewModel::GetMagicDamage);
        NsProp("MagicDefence", &CharacterPanelViewModel::GetMagicDefence);
        NsProp("Evading", &CharacterPanelViewModel::GetEvading);
        NsProp("CanStatUp", &CharacterPanelViewModel::GetCanStatUp);
        NsProp("VitCommand", &CharacterPanelViewModel::GetVitCommand);
        NsProp("IntCommand", &CharacterPanelViewModel::GetIntCommand);
        NsProp("StrCommand", &CharacterPanelViewModel::GetStrCommand);
        NsProp("DexCommand", &CharacterPanelViewModel::GetDexCommand);
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

    void SetEnterWorldCallback(std::function<void(const TWorldEnterInfo&)> callback)
    {
        m_enterWorldCallback = std::move(callback);
    }

    void SetWorldEntityCallbacks(std::function<void(const TWorldEntityInfo&)> upsertCallback,
        std::function<void(uint32_t)> removeCallback)
    {
        m_worldEntityUpsertCallback = std::move(upsertCallback);
        m_worldEntityRemoveCallback = std::move(removeCallback);
    }

    void SetQuestCallbacks(std::function<void(const TQuestInfo&)> updateCallback,
        std::function<void(uint16_t)> removeCallback)
    {
        m_questUpdateCallback = std::move(updateCallback);
        m_questRemoveCallback = std::move(removeCallback);
    }

    void SetPointsCallback(std::function<void()> callback)
    {
        m_pointsUpdateCallback = std::move(callback);
    }

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

        for (uint32_t slot = static_cast<uint32_t>(characters.size()); slot < PLAYER_PER_ACCOUNT4; ++slot)
        {
            m_characters->Add(Noesis::MakePtr<LobbyCharacter>(slot));
        }

        SetStatus("Select a character and channel");
        OnPropertyChanged("Characters");
        OnPropertyChanged("SelectedCharacter");
        OnPropertyChanged("CanEnter");
    }

    void OnGameServerConnectFailure()
    {
        SetStatus("Game server connection failed");
    }

    void OnCharacterSelectAck()
    {
        SetStatus("Entering world...");
    }

    void OnEnterWorld(const TWorldEnterInfo& info)
    {
        LogFormat("[WORLD] OnEnterWorld vid=%u pos=(%d,%d,%d)",
            info.vid,
            info.x,
            info.y,
            info.z);
        if (m_enterWorldCallback)
            m_enterWorldCallback(info);
        SetStatus("");
    }

    void OnWorldCharacterUpsert(const TWorldEntityInfo& entity)
    {
        if (m_worldEntityUpsertCallback)
            m_worldEntityUpsertCallback(entity);
    }

    void OnWorldCharacterRemove(uint32_t vid)
    {
        if (m_worldEntityRemoveCallback)
            m_worldEntityRemoveCallback(vid);
    }

    void OnQuestUpdate(const TQuestInfo& quest)
    {
        if (m_questUpdateCallback)
            m_questUpdateCallback(quest);
    }

    void OnQuestRemove(uint16_t index)
    {
        if (m_questRemoveCallback)
            m_questRemoveCallback(index);
    }

    void OnPointsUpdate()
    {
        if (m_pointsUpdateCallback)
            m_pointsUpdateCallback();
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
    std::function<void(const TWorldEnterInfo&)> m_enterWorldCallback;
    std::function<void(const TWorldEntityInfo&)> m_worldEntityUpsertCallback;
    std::function<void(uint32_t)> m_worldEntityRemoveCallback;
    std::function<void(const TQuestInfo&)> m_questUpdateCallback;
    std::function<void(uint16_t)> m_questRemoveCallback;
    std::function<void()> m_pointsUpdateCallback;

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

struct NoesisLayer::Impl
{
    bool CreateView(Noesis::FrameworkElement* root, uint32_t width, uint32_t height)
    {
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

    bool CreateQuestPanelView(Noesis::FrameworkElement* root, uint32_t width, uint32_t height)
    {
        if (questView)
        {
            questView->GetRenderer()->Shutdown();
            questView.Reset();
        }

        questView = Noesis::GUI::CreateView(root);
        questView->SetSize(width, height);
        questView->SetFlags(Noesis::RenderFlags_PPAA | Noesis::RenderFlags_LCD);
        questView->GetRenderer()->Init(renderDevice);
        return true;
    }

    bool CreateCharacterPanelView(Noesis::FrameworkElement* root, uint32_t width, uint32_t height)
    {
        if (characterView)
        {
            characterView->GetRenderer()->Shutdown();
            characterView.Reset();
        }

        characterView = Noesis::GUI::CreateView(root);
        characterView->SetSize(width, height);
        characterView->SetFlags(Noesis::RenderFlags_PPAA | Noesis::RenderFlags_LCD);
        characterView->GetRenderer()->Init(renderDevice);
        return true;
    }

    bool LoadLoginView(uint32_t width, uint32_t height)
    {
        lobbyActive = false;
        worldActive = false;
        inGameMenuOpen = false;
        questPanelOpen = false;
        characterPanelOpen = false;
        worldInfo = {};
        worldEntities.clear();
        quests.clear();
        points.fill(0);
        lobbyViewModel.Reset();

        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("Login.xaml");
        usernameBox = root->FindName<Noesis::TextBox>("UsernameBox");
        passwordBox = root->FindName<Noesis::PasswordBox>("PasswordBox");
        rememberBox = root->FindName<Noesis::CheckBox>("RememberBox");
        statusText = root->FindName<Noesis::TextBlock>("StatusText");
        if (Noesis::Button* loginButton = root->FindName<Noesis::Button>("LoginButton"))
            loginButton->Click() += Noesis::MakeDelegate(this, &Impl::OnLoginClicked);

        return CreateView(root, width, height);
    }

    bool LoadInGameMenuView(uint32_t width, uint32_t height)
    {
        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("InGameMenu.xaml");
        if (Noesis::Button* button = root->FindName<Noesis::Button>("BtnCharacterSwitch"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnCharacterSwitchClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("BtnLogout"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnLogoutClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("BtnQuit"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnQuitClicked);
        if (Noesis::Button* button = root->FindName<Noesis::Button>("BtnClose"))
            button->Click() += Noesis::MakeDelegate(this, &Impl::OnCloseMenuClicked);
        return CreateMenuView(root, width, height);
    }

    bool LoadQuestPanelView(uint32_t width, uint32_t height)
    {
        questPanelViewModel = Noesis::MakePtr<QuestPanelViewModel>();
        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("QuestPanel.xaml");
        root->SetDataContext(questPanelViewModel);
        return CreateQuestPanelView(root, width, height);
    }

    bool LoadCharacterPanelView(uint32_t width, uint32_t height)
    {
        characterPanelViewModel = Noesis::MakePtr<CharacterPanelViewModel>();
        characterPanelViewModel->SetPoints(points);
        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("CharacterPanel.xaml");
        root->SetDataContext(characterPanelViewModel);
        return CreateCharacterPanelView(root, width, height);
    }

    bool LoadLobbyView(uint32_t width, uint32_t height)
    {
        inGameMenuOpen = false;
        questPanelOpen = false;
        characterPanelOpen = false;
        usernameBox = nullptr;
        passwordBox = nullptr;
        rememberBox = nullptr;
        statusText = nullptr;

        Noesis::Ptr<Noesis::FrameworkElement> root =
            Noesis::GUI::LoadXaml<Noesis::FrameworkElement>("Lobby.xaml");
        lobbyViewModel = Noesis::MakePtr<LobbyViewModel>();
        lobbyViewModel->SetEnterWorldCallback([this](const TWorldEnterInfo& info)
        {
            worldInfo = info;
            worldActive = true;
            lobbyActive = false;
            worldEntities.clear();
            LogFormat("[WORLD] world-active true vid=%u spawn=(%d,%d,%d)",
                info.vid,
                info.x,
                info.y,
                info.z);
        });
        lobbyViewModel->SetWorldEntityCallbacks(
            [this](const TWorldEntityInfo& entity)
            {
                for (TWorldEntityInfo& existing : worldEntities)
                {
                    if (existing.vid == entity.vid)
                    {
                        existing = entity;
                        LogFormat("[WORLD-ENTITY] visible count=%zu updated vid=%u type=%u race=%u hasPos=%u name=%s pos=(%d,%d,%d)",
                            worldEntities.size(),
                            entity.vid,
                            entity.type,
                            entity.race,
                            entity.hasPosition ? 1 : 0,
                            entity.name,
                            entity.x,
                            entity.y,
                            entity.z);
                        return;
                    }
                }

                worldEntities.push_back(entity);
                LogFormat("[WORLD-ENTITY] visible count=%zu added vid=%u type=%u race=%u hasPos=%u name=%s pos=(%d,%d,%d)",
                    worldEntities.size(),
                    entity.vid,
                    entity.type,
                    entity.race,
                    entity.hasPosition ? 1 : 0,
                    entity.name,
                    entity.x,
                    entity.y,
                    entity.z);
            },
            [this](uint32_t vid)
            {
                for (auto it = worldEntities.begin(); it != worldEntities.end(); ++it)
                {
                    if (it->vid == vid)
                    {
                        worldEntities.erase(it);
                        LogFormat("[WORLD-ENTITY] visible count=%zu removed vid=%u",
                            worldEntities.size(),
                            vid);
                        return;
                    }
                }
            });
        lobbyViewModel->SetQuestCallbacks(
            [this](const TQuestInfo& quest)
            {
                quests[quest.index] = quest;
                RefreshQuestPanel();
            },
            [this](uint16_t index)
            {
                quests.erase(index);
                RefreshQuestPanel();
            });
        lobbyViewModel->SetPointsCallback([this]()
        {
            RefreshCharacterPanel();
        });
        root->SetDataContext(lobbyViewModel);
        const bool created = CreateView(root, width, height);
        lobbyActive = created;
        return created;
    }

    void ToggleInGameMenu()
    {
        if (!worldActive)
            return;

        inGameMenuOpen = !inGameMenuOpen;
        LogFormat("[MENU] in-game menu %s", inGameMenuOpen ? "open" : "closed");
    }

    void CloseInGameMenu()
    {
        inGameMenuOpen = false;
    }

    void ToggleQuestPanel()
    {
        if (!worldActive)
            return;

        questPanelOpen = !questPanelOpen;
        if (questPanelOpen)
        {
            RefreshQuestPanel();
        }
        LogFormat("[QUEST] panel %s count=%zu", questPanelOpen ? "open" : "closed", quests.size());
    }

    void RefreshQuestPanel()
    {
        if (questPanelViewModel)
            questPanelViewModel->SetQuests(quests);
    }

    void ToggleCharacterPanel()
    {
        if (!worldActive)
            return;

        characterPanelOpen = !characterPanelOpen;
        if (characterPanelOpen)
        {
            RefreshCharacterPanel();
        }
        LogFormat("[CHARACTER] panel %s level=%lld hp=%lld/%lld",
            characterPanelOpen ? "open" : "closed",
            static_cast<long long>(points[POINT_LEVEL]),
            static_cast<long long>(points[POINT_HP]),
            static_cast<long long>(points[POINT_MAX_HP]));
    }

    void RefreshCharacterPanel()
    {
        if (characterPanelViewModel)
            characterPanelViewModel->SetPoints(points);
    }

    void OnCharacterSwitchClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        Log("[MENU] character switch");
        CloseInGameMenu();
        questPanelOpen = false;
        characterPanelOpen = false;
        worldActive = false;
        worldInfo = {};
        worldEntities.clear();
        quests.clear();
        points.fill(0);
        LoadLobbyView(currentWidth, currentHeight);
        if (clientSession && clientSession->IsAuthenticated())
            clientSession->SendCharacterListRequest();
    }

    void OnLogoutClicked(Noesis::BaseComponent*, const Noesis::RoutedEventArgs&)
    {
        Log("[MENU] logout");
        CloseInGameMenu();
        questPanelOpen = false;
        characterPanelOpen = false;
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
    Noesis::Ptr<Noesis::IView> questView;
    Noesis::Ptr<Noesis::IView> characterView;
    Noesis::Ptr<LobbyViewModel> lobbyViewModel;
    Noesis::Ptr<QuestPanelViewModel> questPanelViewModel;
    Noesis::Ptr<CharacterPanelViewModel> characterPanelViewModel;
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
    bool worldActive = false;
    bool inGameMenuOpen = false;
    bool questPanelOpen = false;
    bool characterPanelOpen = false;
    std::function<void()> quitCallback;
    TWorldEnterInfo worldInfo{};
    std::vector<TWorldEntityInfo> worldEntities;
    std::unordered_map<uint16_t, TQuestInfo> quests;
    std::array<int64_t, POINT_MAX_NUM> points{};
};

NoesisLayer::NoesisLayer() = default;
NoesisLayer::~NoesisLayer() { Destroy(); }

bool NoesisLayer::Create(VulkanDevice& device, uint32_t width, uint32_t height)
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

    const std::string assetRoot = ExecutableDirectory();
    std::string assetLog = "Noesis asset root: " + assetRoot;
    Log(assetLog.c_str());
    Noesis::GUI::SetXamlProvider(Noesis::MakePtr<FileXamlProvider>(assetRoot));
    Noesis::GUI::SetFontProvider(Noesis::MakePtr<FileFontProvider>(assetRoot));

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
    const bool quests = menu ? m_impl->LoadQuestPanelView(width, height) : false;
    const bool character = quests ? m_impl->LoadCharacterPanelView(width, height) : false;
    return login && menu && quests && character;
}

void NoesisLayer::Update(double timeSeconds)
{
    if (m_impl && m_impl->view)
        m_impl->view->Update(timeSeconds);
    if (m_impl && m_impl->questView && m_impl->questPanelOpen)
    {
        if (m_impl->questPanelViewModel)
            m_impl->questPanelViewModel->RefreshDetail();
        m_impl->questView->Update(timeSeconds);
    }
    if (m_impl && m_impl->characterView && m_impl->characterPanelOpen)
        m_impl->characterView->Update(timeSeconds);
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
    if (m_impl->questView && m_impl->questPanelOpen)
    {
        m_impl->questView->GetRenderer()->UpdateRenderTree();
        m_impl->questView->GetRenderer()->RenderOffscreen();
    }
    if (m_impl->characterView && m_impl->characterPanelOpen)
    {
        m_impl->characterView->GetRenderer()->UpdateRenderTree();
        m_impl->characterView->GetRenderer()->RenderOffscreen();
    }
}

void NoesisLayer::RenderOnscreen(VulkanDevice& device)
{
    if (!m_impl || !device.IsFrameActive())
        return;

    if (m_impl->worldActive && !m_impl->questPanelOpen && !m_impl->characterPanelOpen && !m_impl->inGameMenuOpen)
        return;

    NoesisApp::VKFactory::RecordingInfo info{};
    info.commandBuffer = device.GetCommandBuffer();
    info.frameNumber = device.GetFrameNumber() + 2;
    info.safeFrameNumber = device.GetSafeFrameNumber();
    NoesisApp::VKFactory::SetCommandBuffer(m_impl->renderDevice, info);
    NoesisApp::VKFactory::SetRenderPass(m_impl->renderDevice, device.GetRenderPass(), VK_SAMPLE_COUNT_1_BIT);
    if (m_impl->worldActive)
    {
        if (m_impl->questPanelOpen && m_impl->questView)
            m_impl->questView->GetRenderer()->Render();
        if (m_impl->characterPanelOpen && m_impl->characterView)
            m_impl->characterView->GetRenderer()->Render();
        if (m_impl->inGameMenuOpen && m_impl->menuView)
            m_impl->menuView->GetRenderer()->Render();
    }
    else if (m_impl->view)
    {
        m_impl->view->GetRenderer()->Render();
    }
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
    if (m_impl && m_impl->questView)
        m_impl->questView->SetSize(width, height);
    if (m_impl && m_impl->characterView)
        m_impl->characterView->SetSize(width, height);
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

bool NoesisLayer::IsWorldActive() const
{
    return m_impl && m_impl->worldActive;
}

bool NoesisLayer::IsInGameMenuOpen() const
{
    return m_impl && m_impl->inGameMenuOpen;
}

bool NoesisLayer::IsQuestPanelOpen() const
{
    return m_impl && m_impl->questPanelOpen;
}

bool NoesisLayer::IsCharacterPanelOpen() const
{
    return m_impl && m_impl->characterPanelOpen;
}

void NoesisLayer::ToggleInGameMenu()
{
    if (m_impl)
        m_impl->ToggleInGameMenu();
}

void NoesisLayer::ToggleQuestPanel()
{
    if (m_impl)
        m_impl->ToggleQuestPanel();
}

void NoesisLayer::ToggleCharacterPanel()
{
    if (m_impl)
        m_impl->ToggleCharacterPanel();
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

const TWorldEnterInfo& NoesisLayer::GetWorldEnterInfo() const
{
    static const TWorldEnterInfo empty{};
    return m_impl ? m_impl->worldInfo : empty;
}

const std::vector<TWorldEntityInfo>& NoesisLayer::GetWorldEntities() const
{
    static const std::vector<TWorldEntityInfo> empty;
    return m_impl ? m_impl->worldEntities : empty;
}

bool NoesisLayer::OnInput(const InputEvent& event)
{
    if (!m_impl || !m_impl->view)
        return false;

    Noesis::IView* targetView = m_impl->view.GetPtr();
    if (m_impl->inGameMenuOpen && m_impl->menuView)
        targetView = m_impl->menuView.GetPtr();

    if (!m_impl->inGameMenuOpen)
    {
        if (m_impl->characterPanelOpen && m_impl->characterView)
        {
            switch (event.type)
            {
            case InputEvent::MouseMove:
                if (m_impl->characterView->MouseMove(event.x, event.y))
                    return true;
                break;
            case InputEvent::MouseDown:
                if (m_impl->characterView->MouseButtonDown(event.x, event.y, ToNoesisMouseButton(event.button)))
                    return true;
                break;
            case InputEvent::MouseUp:
                if (m_impl->characterView->MouseButtonUp(event.x, event.y, ToNoesisMouseButton(event.button)))
                    return true;
                break;
            case InputEvent::MouseWheel:
                if (m_impl->characterView->MouseWheel(event.x, event.y, event.wheelDelta))
                    return true;
                break;
            default:
                break;
            }
        }
        if (m_impl->questPanelOpen && m_impl->questView)
            targetView = m_impl->questView.GetPtr();
        else if (m_impl->characterPanelOpen && m_impl->worldActive)
            return false;
    }

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
    if (m_impl->questView)
    {
        m_impl->questView->GetRenderer()->Shutdown();
        m_impl->questView.Reset();
    }
    if (m_impl->characterView)
    {
        m_impl->characterView->GetRenderer()->Shutdown();
        m_impl->characterView.Reset();
    }

    m_impl->renderDevice.Reset();

    if (m_impl->guiInitialized)
    {
        Noesis::GUI::Shutdown();
        m_impl->guiInitialized = false;
    }

    m_impl.reset();
}
