#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace editor_help
{
enum class Language { Hungarian, English };
struct Article
{
    std::string id, category, title, markdown;
};
// These APIs have no ImGui dependency; content validation/search tests use the
// same embedded documents that the editor reads, without filesystem/network IO.
std::vector<Article> Parse(std::string_view markdown);
const std::vector<Article>& Articles(Language language);
std::string FoldSearch(std::string_view text);
std::vector<std::size_t> Search(Language language, std::string_view query);

class Browser
{
public:
    void Open() { m_open = true; }
    bool IsOpen() const { return m_open; }
    void Render();
    void SetLanguage(Language language);
    Language GetLanguage() const { return m_language; }
    void SetSearch(std::string_view query);
    std::size_t MatchingTopicCount() const { return m_results.size(); }
    bool Select(std::string_view id);
    std::string_view SelectedId() const;
private:
    bool m_open = false, m_scrollToTop = true, m_searchInitialized = false;
    Language m_language = Language::Hungarian;
    std::size_t m_selected = 0;
    char m_search[192]{};
    std::string m_lastQuery;
    Language m_lastLanguage = Language::English;
    std::vector<std::size_t> m_results;
};
} // namespace editor_help
