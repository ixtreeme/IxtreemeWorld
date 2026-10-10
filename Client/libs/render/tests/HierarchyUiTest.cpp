#include "EditorImGui.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cstring>
#include <iostream>

struct EditorHierarchyTestAccess
{
    static int Draw(EditorImGui& editor)
    {
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({480, 260});
        ImGui::Begin("Hierarchy test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        int drawn = 0;
        if (ImGui::BeginTable("HierarchyEntityTree", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed, 25);
            const auto& rows = editor.m_hierarchyView.Rows();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()), ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2);
            if (editor.m_hierarchyScrollToSelection || editor.m_hierarchyRenamingEntity)
                for (std::size_t i = 0; i < rows.size(); ++i)
                    if (rows[i].entity == editor.m_selectedHierarchyEntity || rows[i].entity == editor.m_hierarchyRenamingEntity)
                        clipper.IncludeItemByIndex(static_cast<int>(i));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                { editor.RenderHierarchyEntityNode(rows[i].entity, rows[i].depth); ++drawn; }
            ImGui::EndTable();
            editor.m_hierarchyScrollToSelection = false;
        }
        ImGui::End();
        return drawn;
    }
    static MapEditorCommands Commands(EditorImGui& editor) { auto commands = editor.m_commands; editor.m_commands = {}; return commands; }
    static void Search(EditorImGui& editor, const char* query) { editor.m_hierarchyView.SetSearch(query); }
    static void Rename(EditorImGui& editor, std::uint64_t id) { editor.StartHierarchyRename(*editor.m_hierarchyView.Find(id)); }
    static float Scroll() { return ImGui::FindWindowByName("Hierarchy test")->Scroll.y; }
    static bool IsVisible(EditorImGui& editor, std::uint64_t id) {
        const auto& rows = editor.m_hierarchyView.Rows();
        return std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.entity == id; });
    }
};

int main()
{
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::cerr << message << '\n'; } };
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {640, 480}; io.DeltaTime = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    {
        EditorImGui editor;
        std::vector<HierarchySceneEntity> entities;
        for (std::uint64_t i = 0; i < 10000; ++i)
            entities.push_back({100 + i, 1, HierarchyEntityType::MeshEntity, static_cast<std::uint32_t>(i + 1), "Object " + std::to_string(i)});
        editor.SetHierarchySceneState(1, "Scene", entities);
        const auto frame = [&] { ImGui::NewFrame(); const int count = EditorHierarchyTestAccess::Draw(editor); ImGui::Render(); return count; };
        frame(); check(frame() < 20, "10000 objects draw only the viewport rows");
        io.AddMousePosEvent(100, 40); io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
        auto commands = EditorHierarchyTestAccess::Commands(editor);
        check(commands.hierarchySelectEntity && commands.hierarchyEntityId == 1, "click selects correct clipped row");
        // Drag the first row onto the second: exercise the production source/target payload path.
        io.AddMousePosEvent(100, 40); io.AddMouseButtonEvent(0, true); frame();
        io.AddMousePosEvent(130, 68); frame(); frame();
        io.AddMouseButtonEvent(0, false); frame();
        commands = EditorHierarchyTestAccess::Commands(editor);
        check(commands.hierarchyReparentEntity && commands.hierarchyEntityId == 1 && commands.hierarchyParentId == 2,
            "drag-and-drop preserves source and parent identity");
        EditorHierarchyTestAccess::Rename(editor, 100); frame(); frame();
        io.AddInputCharactersUTF8("Renamed"); frame();
        io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
        commands = EditorHierarchyTestAccess::Commands(editor);
        check(commands.hierarchyRenameEntity && commands.hierarchyRenameValue == "Renamed", "rename commits text to the correct entity");
        EditorHierarchyTestAccess::Search(editor, "Object 9999");
        check(frame() == 1, "search clips to the matching object");
        io.AddMousePosEvent(100, 40); io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
        commands = EditorHierarchyTestAccess::Commands(editor);
        check(commands.hierarchySelectEntity && commands.hierarchyEntityId == 10000, "filtered row preserves entity identity");
        EditorHierarchyTestAccess::Search(editor, "");
        editor.SetHierarchySelection(100); frame(); frame();
        editor.SetHierarchySelection(10099); frame(); frame();
        check(EditorHierarchyTestAccess::Scroll() > 100000, "selection reveals an offscreen clipped row");
        EditorHierarchyTestAccess::Rename(editor, 100); frame(); frame(); frame();
        check(EditorHierarchyTestAccess::Scroll() < 100, "rename reveals an offscreen clipped row");
        io.AddInputCharactersUTF8("Top renamed"); frame();
        io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
        commands = EditorHierarchyTestAccess::Commands(editor);
        check(commands.hierarchyRenameEntity && commands.hierarchyEntityId == 1 && commands.hierarchyRenameValue == "Top renamed",
            "offscreen rename preserves focus and entity identity");
        entities.back().selected = true;
        entities.back().parent = entities.front().entity;
        editor.SetHierarchySceneState(1, "Scene", entities);
        check(EditorHierarchyTestAccess::IsVisible(editor, entities.back().entity),
            "reparenting the selected object reveals its new parent path");
    }
    ImGui::DestroyContext();
    std::cout << "Hierarchy UI failures: " << failures << '\n';
    return failures ? 1 : 0;
}
