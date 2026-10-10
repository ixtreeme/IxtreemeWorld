#include "EditorHelp.h"
#include "EditorHelpContent.h"
#include "UIHelpers.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace editor_help
{
namespace
{
std::string_view Trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
std::vector<std::string_view> Lines(std::string_view s)
{
    std::vector<std::string_view> lines;
    while (!s.empty())
    {
        const auto end = s.find('\n');
        lines.push_back(Trim(s.substr(0, end)));
        if (end == s.npos) break;
        s.remove_prefix(end + 1);
    }
    return lines;
}
// Inline emphasis/code with real word wrapping, retaining UTF-8 words. Draw-list
// text plus one layout item avoids the per-word ImGui item overhead.
void RichText(std::string_view text, float indent = 0, bool callout = false)
{
    const auto start = ImGui::GetCursorScreenPos();
    const float width = std::max(40.0f, ImGui::GetContentRegionAvail().x - indent);
    float x = 0, y = 0;
    const float lineHeight = std::ceil(std::max(ImGui::GetTextLineHeight(), 18.0f)) + 4;
    bool bold = false, code = false;
    auto* draw = ImGui::GetWindowDrawList();
    for (std::size_t at = 0; at < text.size();)
    {
        if (text.substr(at, 2) == "**") { bold = !bold; at += 2; continue; }
        if (text[at] == '`') { code = !code; ++at; continue; }
        if (text[at] == ' ') { x += ImGui::CalcTextSize(" ").x; ++at; continue; }
        std::size_t end = at + 1;
        while (end < text.size() && text[end] != ' ' && text[end] != '`' && text.substr(end, 2) != "**") ++end;
        auto* font = bold && UI::GetEditorFonts().bold ? UI::GetEditorFonts().bold : ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const float wordWidth = font->CalcTextSizeA(size, FLT_MAX, 0, text.data() + at, text.data() + end).x;
        if (x > 0 && x + wordWidth > width) { x = 0; y += lineHeight; }
        const auto pos = ImVec2(start.x + indent + x, start.y + y);
        const auto color = ImGui::GetColorU32(code || callout ? UI::Theme::AccentHovered : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (code) draw->AddRectFilled({pos.x - 2, pos.y - 1}, {pos.x + wordWidth + 2, pos.y + lineHeight - 3}, ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
        draw->AddText(font, size, pos, color, text.data() + at, text.data() + end);
        x += wordWidth; at = end;
    }
    ImGui::Dummy({width + indent, y + lineHeight});
}
void RenderMarkdown(std::string_view markdown)
{
    bool fenced = false;
    for (auto line : Lines(markdown))
    {
        if (line.starts_with("```")) { fenced = !fenced; continue; }
        if (line.empty()) { ImGui::Dummy({0, 7}); continue; }
        if (fenced) { RichText(line, 12, true); continue; }
        if (line.starts_with("#### "))
        {
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            if (UI::GetEditorFonts().bold) ImGui::PushFont(UI::GetEditorFonts().bold);
            RichText(line.substr(5));
            if (UI::GetEditorFonts().bold) ImGui::PopFont();
        }
        else if (line.starts_with("> ")) RichText(line.substr(2), 12, true);
        else if (line.starts_with("- ")) RichText(line, 12);
        else RichText(line);
    }
}
}

std::vector<Article> Parse(std::string_view markdown)
{
    std::vector<Article> result;
    std::string category, id;
    for (auto line : Lines(markdown))
    {
        if (line.starts_with("<!-- topic: ") && line.ends_with(" -->"))
            id = line.substr(12, line.size() - 16);
        else if (line.starts_with("## ")) category = line.substr(3);
        else if (line.starts_with("### "))
        {
            result.push_back({id, category, std::string(line.substr(4)), {}});
            id.clear();
        }
        else if (!result.empty()) { result.back().markdown.append(line); result.back().markdown += '\n'; }
    }
    for (auto& article : result) article.markdown = Trim(article.markdown);
    return result;
}

const std::vector<Article>& Articles(Language language)
{
    static const auto hu = Parse(content::hu), en = Parse(content::en);
    return language == Language::Hungarian ? hu : en;
}

std::string FoldSearch(std::string_view text)
{
    static constexpr std::pair<std::string_view, char> accents[] = {
        {"á",'a'}, {"Á",'a'}, {"é",'e'}, {"É",'e'}, {"í",'i'}, {"Í",'i'},
        {"ó",'o'}, {"Ó",'o'}, {"ö",'o'}, {"Ö",'o'}, {"ő",'o'}, {"Ő",'o'},
        {"ú",'u'}, {"Ú",'u'}, {"ü",'u'}, {"Ü",'u'}, {"ű",'u'}, {"Ű",'u'}};
    std::string result;
    for (std::size_t i = 0; i < text.size();)
    {
        bool replaced = false;
        for (const auto& [utf8, ascii] : accents)
            if (text.substr(i).starts_with(utf8)) { result += ascii; i += utf8.size(); replaced = true; break; }
        if (!replaced) result += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i++])));
    }
    return result;
}

std::vector<std::size_t> Search(Language language, std::string_view query)
{
    static const auto keys = [] {
        std::vector<std::string> result;
        const auto& hu = Articles(Language::Hungarian); const auto& en = Articles(Language::English);
        for (std::size_t i = 0; i < hu.size(); ++i)
        {
            std::string text = hu[i].title + " " + hu[i].category + " " + hu[i].markdown;
            const auto other = std::find_if(en.begin(), en.end(), [&](const auto& a) { return a.id == hu[i].id; });
            if (other != en.end()) text += " " + other->title + " " + other->category + " " + other->markdown;
            result.push_back(FoldSearch(text));
        }
        return result;
    }();
    std::vector<std::size_t> result;
    // Whitespace-separated terms all have to match; their order is irrelevant.
    const auto folded = FoldSearch(query);
    std::vector<std::string_view> terms;
    for (std::size_t at = 0; at < folded.size();)
    {
        while (at < folded.size() && std::isspace(static_cast<unsigned char>(folded[at]))) ++at;
        const auto start = at;
        while (at < folded.size() && !std::isspace(static_cast<unsigned char>(folded[at]))) ++at;
        if (at > start) terms.emplace_back(folded.data() + start, at - start);
    }
    const auto& current = Articles(language); const auto& hu = Articles(Language::Hungarian);
    for (std::size_t i = 0; i < current.size(); ++i)
    {
        const auto key = std::find_if(hu.begin(), hu.end(), [&](const auto& a) { return a.id == current[i].id; });
        if (key == hu.end()) continue;
        const auto& text = keys[static_cast<std::size_t>(key - hu.begin())];
        if (std::all_of(terms.begin(), terms.end(), [&](auto term) { return text.find(term) != text.npos; })) result.push_back(i);
    }
    return result;
}

bool Browser::Select(std::string_view id)
{
    const auto& articles = Articles(m_language);
    const auto found = std::find_if(articles.begin(), articles.end(), [&](const auto& a) { return a.id == id; });
    if (found == articles.end()) return false;
    m_selected = static_cast<std::size_t>(found - articles.begin()); m_scrollToTop = true;
    return true;
}
std::string_view Browser::SelectedId() const
{
    const auto& articles = Articles(m_language);
    return m_selected < articles.size() ? std::string_view(articles[m_selected].id) : std::string_view();
}

void Browser::SetLanguage(Language language)
{
    const std::string selected(SelectedId());
    m_language = language;
    Select(selected);
    m_scrollToTop = true;
}
void Browser::SetSearch(std::string_view query)
{
    auto length = std::min(query.size(), sizeof(m_search) - 1);
    // Never truncate in the middle of a UTF-8 codepoint.
    while (length < query.size() && length > 0 && (static_cast<unsigned char>(query[length]) & 0xc0) == 0x80) --length;
    if (length) std::memcpy(m_search, query.data(), length);
    m_search[length] = '\0';
}

void Browser::Render()
{
    if (!m_open) return;
    ImGui::SetNextWindowSize({1080, 760}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({580, 380}, {FLT_MAX, FLT_MAX});
    if (!ImGui::Begin("Editor Guide / Súgó###EditorGuide", &m_open)) { ImGui::End(); return; }
    ImGui::SetNextItemWidth(140);
    int language = m_language == Language::Hungarian ? 0 : 1;
    if (ImGui::Combo("##language", &language, "Magyar\0English\0"))
        SetLanguage(language ? Language::English : Language::Hungarian);
    const bool hu = m_language == Language::Hungarian;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", hu ? "Használati útmutató és rövid tutorialok" : "User guide and short tutorials");
    ImGui::SetNextItemWidth(-80);
    ImGui::InputTextWithHint("##help_search", hu ? "Keresés minden témában..." : "Search all topics...", m_search, sizeof(m_search));
    ImGui::SameLine();
    if (ImGui::Button(hu ? "Törlés" : "Clear", {72, 0})) m_search[0] = '\0';
    if (!m_searchInitialized || m_lastQuery != m_search || m_lastLanguage != m_language)
    {
        m_results = Search(m_language, m_search); m_lastQuery = m_search; m_lastLanguage = m_language;
        m_searchInitialized = true;
        if (!m_results.empty() && std::find(m_results.begin(), m_results.end(), m_selected) == m_results.end())
        {
            m_selected = m_results.front(); m_scrollToTop = true;
        }
    }
    ImGui::Separator();
    const auto& articles = Articles(m_language);
    if (articles.empty()) { ImGui::TextUnformatted("No help content."); ImGui::End(); return; }
    m_selected = std::min(m_selected, articles.size() - 1);
    const float sidebar = std::clamp(ImGui::GetContentRegionAvail().x * 0.29f, 190.0f, 310.0f);
    ImGui::BeginChild("HelpTopics", {sidebar, 0}, ImGuiChildFlags_Borders);
    ImGui::TextDisabled(hu ? "%zu / %zu téma" : "%zu / %zu topics", m_results.size(), articles.size());
    if (m_results.empty()) ImGui::TextWrapped("%s", hu ? "Nincs találat. Próbálj rövidebb vagy másik kifejezést." : "No matches. Try a shorter or different term.");
    std::string category;
    bool categoryOpen = true;
    for (auto index : m_results)
    {
        const auto& article = articles[index];
        if (category != article.category)
        {
            category = article.category;
            if (m_search[0]) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            categoryOpen = ImGui::CollapsingHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            UI::ItemTooltip(category.c_str());
        }
        if (!categoryOpen) continue;
        ImGui::PushID(article.id.c_str());
        const float height = std::max(ImGui::GetFrameHeight(), ImGui::CalcTextSize(article.title.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x - 16).y + 10);
        if (ImGui::Selectable("##topic", m_selected == index, 0, {0, height})) Select(article.id);
        const auto min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {min.x + 6, min.y + 5},
            ImGui::GetColorU32(ImGuiCol_Text), article.title.c_str(), nullptr, ImGui::GetContentRegionAvail().x - 16);
        ImGui::PopID();
    }
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("HelpArticle", {0, 0}, ImGuiChildFlags_Borders);
    if (m_scrollToTop) { ImGui::SetScrollY(0); m_scrollToTop = false; }
    const auto& article = articles[m_selected];
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", article.category.c_str());
    ImGui::PopStyleColor();
    if (UI::GetEditorFonts().bold) ImGui::PushFont(UI::GetEditorFonts().bold);
    RichText(article.title);
    if (UI::GetEditorFonts().bold) ImGui::PopFont();
    if (ImGui::Button(hu ? "Leírás másolása" : "Copy article"))
    {
        const std::string copy = "# " + article.title + "\n\n" + article.markdown;
        ImGui::SetClipboardText(copy.c_str());
    }
    const char* offline = hu ? "Offline • magyar / angol" : "Offline • Hungarian / English";
    if (ImGui::GetContentRegionAvail().x > ImGui::GetItemRectSize().x + ImGui::CalcTextSize(offline).x + 12) ImGui::SameLine();
    ImGui::TextDisabled("%s", offline);
    ImGui::Spacing(); RenderMarkdown(article.markdown); ImGui::Spacing(); ImGui::Separator();
    ImGui::BeginDisabled(m_selected == 0);
    if (ImGui::Button(hu ? "Előző téma" : "Previous topic")) { --m_selected; m_scrollToTop = true; }
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled(m_selected + 1 == articles.size());
    if (ImGui::Button(hu ? "Következő téma" : "Next topic")) { ++m_selected; m_scrollToTop = true; }
    ImGui::EndDisabled();
    ImGui::EndChild(); ImGui::End();
}
} // namespace editor_help
