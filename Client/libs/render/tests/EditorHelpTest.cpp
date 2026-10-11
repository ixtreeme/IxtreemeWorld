#include "EditorHelp.h"
#include "UIHelpers.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

int main()
{
    using namespace editor_help;
    int failures = 0;
    const auto check = [&](bool value, const std::string& message) {
        if (!value) { ++failures; std::cerr << message << '\n'; }
    };
    // Inventory of current editor surfaces, independent of the document parser.
    const std::set<std::string> expected{
        "quick-start", "projects-scenes", "layout", "navigation-transform", "hierarchy", "play-cameras",
        "asset-browser", "model-import", "textures-exr", "materials", "prefabs", "model-export",
        "terrain-create", "terrain-sculpt", "terrain-paint", "terrain-mapping", "water-body", "water-material",
        "components", "lights", "camera", "colliders", "rigidbody", "joints", "character-controller",
        "animation-clips", "animator", "blend-trees", "particles", "audio", "scripts", "script-api",
        "script-queries", "game-ui", "network-native", "tree-generator", "lod-occlusion", "impostors",
        "layered-world", "server-export", "environment", "sky", "fog", "post-processing", "project-settings",
        "performance", "physics-debugger", "console-diagnostics", "build-game", "shortcuts", "troubleshooting"};
    const auto& hu = Articles(Language::Hungarian);
    const auto& en = Articles(Language::English);
    check(hu.size() == expected.size() && en.size() == expected.size(), "both translations cover every editor topic");
    for (const auto language : {Language::Hungarian, Language::English})
    {
        const auto& articles = Articles(language);
        std::set<std::string> ids, categories;
        for (const auto& article : articles)
        {
            check(ids.insert(article.id).second, "duplicate topic: " + article.id);
            categories.insert(article.category);
            check(!article.title.empty() && !article.category.empty(), "missing title/category: " + article.id);
            check(article.markdown.size() > 250, "missing tutorial body: " + article.id);
            for (const auto heading : language == Language::Hungarian
                    ? std::vector<std::string_view>{"#### Mire jó?", "#### Rövid tutorial", "#### Eredmény és gyakori hiba"}
                    : std::vector<std::string_view>{"#### Purpose", "#### Short tutorial", "#### Result and common mistake"})
                check(article.markdown.find(heading) != std::string::npos, "missing section: " + article.id);
            check(article.markdown.find("\n3. ") != std::string::npos, "tutorial needs actionable steps: " + article.id);
            check(article.markdown.find("<!-- topic:") == std::string::npos, "metadata leaked into body: " + article.id);
        }
        check(ids == expected, "topic inventory differs from editor feature inventory");
        check(categories.size() >= 8, "guide needs navigable categories");
    }
    for (std::size_t i = 0; i < std::min(hu.size(), en.size()); ++i)
        check(hu[i].id == en[i].id, "translated navigation order differs");

    const auto sample = Parse("# Intro\r\nignored\r\n## Assets\r\n<!-- topic: test -->\r\n### Test\r\n\r\n#### Details\r\nBody **bold** and `code`.\r\n## World\r\n<!-- topic: other -->\r\n### Other\r\nText\r\n");
    check(sample.size() == 2 && sample[0].id == "test" && sample[0].category == "Assets"
        && sample[1].category == "World" && sample[1].id == "other", "CRLF topic/category parsing");
    check(sample.size() == 2 && sample[0].markdown == "#### Details\nBody **bold** and `code`.", "body preserves emphasis/heading and removes metadata");
    check(Parse("# No topics\nintro").empty(), "intro alone is not an article");
    check(FoldSearch("ÁRVÍZTŰRŐ TÜKÖRFÚRÓGÉP") == "arvizturo tukorfurogep", "Hungarian accent-insensitive case folding");
    const auto matches = [&](Language language, std::string_view query, std::string_view id) {
        const auto found = Search(language, query);
        return std::any_of(found.begin(), found.end(), [&](auto index) { return Articles(language)[index].id == id; });
    };
    check(matches(Language::English, "ELSIMITAS", "terrain-paint"), "English view searches Hungarian text without accents");
    check(matches(Language::Hungarian, "OVERALL COVERAGE", "terrain-paint"), "Hungarian view searches English field names");
    check(matches(Language::Hungarian, "overlapsphere", "script-queries"), "full article body is searchable");
    check(Search(Language::English, "terrain painting") == Search(Language::English, " PAINTING\tTERRAIN "), "search terms ignore order and whitespace");
    check(Search(Language::Hungarian, "").size() == expected.size(), "cleared search restores all topics");
    check(Search(Language::English, "xx-nonexistent-topic-456").empty(), "unknown query yields no matches");

    Browser browser;
    check(!browser.IsOpen() && browser.GetLanguage() == Language::Hungarian, "default language is Hungarian and help stays closed");
    browser.Render(); // No ImGui context required while closed.
    check(browser.Select("terrain-paint"), "known topic selection");
    browser.SetLanguage(Language::English);
    check(browser.SelectedId() == "terrain-paint", "language switch retains topic identity");
    check(!browser.Select("missing") && browser.SelectedId() == "terrain-paint", "unknown topic cannot lose selection");

    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280, 900}; io.DeltaTime = 1.0f / 60;
    auto* regular = io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    UI::SetEditorFonts({regular, regular});
    browser.Open();
    const auto frame = [&](float windowWidth) {
        ImGui::NewFrame();
        const char* name = "Editor Guide / Súgó###EditorGuide";
        const bool exists = ImGui::FindWindowByName(name) != nullptr;
        ImGui::SetWindowSize(name, {windowWidth, 760}, ImGuiCond_Always);
        browser.Render(); ImGui::Render();
        if (exists) check(ImGui::FindWindowByName(name)->Size.x == windowWidth, "guide actually renders at requested narrow/full width");
        const auto* data = ImGui::GetDrawData();
        check(data && data->TotalVtxCount > 0, "open guide generates formatted UI");
        if (data)
            for (const auto* list : data->CmdLists)
                for (const auto& v : list->VtxBuffer)
                    check(std::isfinite(v.pos.x) && std::isfinite(v.pos.y), "invalid help layout coordinates");
    };
    frame(1080);
    check(browser.MatchingTopicCount() == expected.size(), "first English render populates the whole topic list");
    // Exercise every article in both languages and a narrow, resizable pane.
    for (const auto language : {Language::Hungarian, Language::English})
    {
        browser.SetLanguage(language);
        for (const auto& article : Articles(language))
        {
            browser.Select(article.id); frame(1080); frame(580);
        }
    }
    browser.SetSearch("xx-nonexistent-topic-456"); frame(580);
    check(browser.MatchingTopicCount() == 0, "empty UI search result is shown without stale topics");
    browser.Select("build-game"); browser.SetSearch("OVERALL COVERAGE"); frame(1080);
    check(browser.SelectedId() == "terrain-paint", "search reveals a matching article rather than an unrelated previous article");
    browser.SetSearch(std::string(190, 'x') + "ő"); frame(580);
    browser.SetSearch(""); frame(1080);
    check(browser.MatchingTopicCount() == expected.size(), "clearing UI search restores every topic");
    UI::SetEditorFonts({}); ImGui::DestroyContext();
    std::cout << "Editor help: " << expected.size() << " bilingual topics, failures: " << failures << '\n';
    return failures ? 1 : 0;
}
