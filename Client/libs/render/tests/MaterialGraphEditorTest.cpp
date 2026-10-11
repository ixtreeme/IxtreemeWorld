#define IMGUI_DEFINE_MATH_OPERATORS
#include "tools/material/MaterialGraphPanel.h"
#include "tools/material/MaterialGraphIO.h"
#include "tools/material/MaterialInstance.h"
#include "asset/AssetDatabase.h"
#include "IconsFontAwesome6.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor_internal.h>
#include <crude_json.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>

using namespace material_tool;
namespace ed = ax::NodeEditor;
namespace material_tool
{
struct MaterialGraphPanelTestAccess
{
    static ed::Detail::EditorContext* Context(MaterialGraphPanel& panel)
    { return reinterpret_cast<ed::Detail::EditorContext*>(panel.context_); }
    static ImVec2 Pin(MaterialGraphPanel& panel, std::uint32_t id)
    {
        auto* context = Context(panel);
        return context->ToScreen(context->FindPin(ed::PinId(id))->m_Bounds.GetCenter());
    }
    static void Select(MaterialGraphPanel& panel, std::uint32_t id)
    {
        ed::SetCurrentEditor(panel.context_); ed::SelectNode(ed::NodeId(id * 16)); ed::SetCurrentEditor(nullptr);
    }
    static ImVec2 NodeHeader(MaterialGraphPanel& panel, std::uint32_t id)
    {
        auto* context = Context(panel);
        auto bounds = context->FindNode(ed::NodeId(id * 16))->m_Bounds;
        return context->ToScreen({bounds.Min.x + 100, bounds.Min.y + 15});
    }
    static void LargeGraph(MaterialGraphPanel& panel)
    {
        panel.document_ = {};
        std::string reason;
        std::uint32_t previous = 0;
        for (int i = 0; i < 300; ++i)
        {
            const auto id = panel.document_.AddNode(NodeKind::Multiply, {static_cast<float>((i % 20) * 290), static_cast<float>((i / 20) * 240)});
            if (previous) panel.document_.Connect(PinId(previous, 2), PinId(id, 0), reason);
            previous = id;
        }
        panel.ResetContext(); panel.fitPending_ = true;
    }
    static float Zoom(MaterialGraphPanel& panel) { return Context(panel)->GetView().Scale; }
    static std::uint32_t Selected(MaterialGraphPanel& panel) { return panel.selected_; }
    static void Smart(MaterialGraphPanel& panel) {
        panel.document_=MaterialGraphDocument::WeatheredStone();panel.undo_.clear();panel.redo_.clear();panel.parameters_={};
        panel.selected_=panel.document_.layers.front().sourceNode;panel.ResetContext();panel.fitPending_=true;panel.previewDirty_=true;
        panel.propertyBefore_.reset();panel.dragBefore_.reset();
    }
    static void DisableLayer(MaterialGraphPanel& panel) {
        const auto before=panel.document_;std::string error;panel.document_.SetMaterialLayerEnabled(panel.document_.layers.back().id,false,error);panel.RecordUndo(before);
    }
    static void Detach(MaterialGraphPanel& panel) {const auto before=panel.document_;panel.document_.ConvertToCustomGraph();panel.RecordUndo(before);}
    static void Override(MaterialGraphPanel& panel,float value) {panel.parameters_.values["MossAmount"]={value,0,0,0};panel.previewDirty_=true;}
    static const ParameterOverrides& Overrides(const MaterialGraphPanel& panel) {return panel.parameters_;}
};
}
namespace
{
int failures = 0;
void Check(bool ok, const char* message) { std::cout << (ok ? "PASS " : "FAIL ") << message << '\n'; failures += !ok; }
void Model()
{
    auto d = MaterialGraphDocument::Example();
    Check(d.nodes.size() == 3 && d.links.size() == 2, "example graph has connected Color, Roughness and PBR Output");
    MaterialGraphDocument loaded; std::string error;
    Check(loaded.FromJson(d.ToJson(), error) && loaded == d, "graph round-trip preserves node values, layout, links and IDs");
    const auto intact = loaded;
    Check(!loaded.FromJson("{bad", error) && loaded == intact, "malformed JSON leaves current graph intact");
    auto json = crude_json::value::parse(d.ToJson()); json["version"] = 42.0;
    Check(!loaded.FromJson(json.dump(), error) && loaded == intact, "future file version is rejected without replacing the graph");
    json = crude_json::value::parse(d.ToJson()); json["nodes"][0]["value"] = "invalid";
    Check(!loaded.FromJson(json.dump(), error) && loaded == intact, "wrong parameter types are rejected safely");
    json = crude_json::value::parse(d.ToJson()); json["links"][0]["output"] = 123456.0;
    Check(!loaded.FromJson(json.dump(), error), "dangling link is rejected on load");
    json = crude_json::value::parse(d.ToJson()); json["nodes"][1]["id"] = json["nodes"][0]["id"];
    Check(!loaded.FromJson(json.dump(), error), "duplicate IDs are rejected on load");
    std::string reason;
    const auto before = d;
    Check(!d.Connect(PinId(1, 0), PinId(3, 1), reason) && d == before, "Color cannot connect to Scalar input");
    const auto alternate = d.AddNode(NodeKind::Color, {100, 400});
    Check(d.Connect(PinId(3, 0), PinId(alternate, 0), reason) && d.links.size() == 2, "reverse pin drag rewires one occupied input");
    d.RemoveNode(alternate);
    Check(d.links.size() == 1 && !d.FindNode(alternate), "deleting node removes its connections");
    Check(loaded.FromJson(d.ToJson(), error) && loaded.nextId == d.nextId, "saved graph does not reuse deleted node or link IDs");
    Check(!d.AddNode(NodeKind::PbrOutput, {}), "one surface output per graph");
    MaterialGraphDocument cycles;
    const auto a = cycles.AddNode(NodeKind::Multiply, {}), b = cycles.AddNode(NodeKind::Multiply, {}), c = cycles.AddNode(NodeKind::Multiply, {});
    cycles.Connect(PinId(a, 2), PinId(b, 0), reason); cycles.Connect(PinId(b, 2), PinId(c, 0), reason);
    const auto acyclic = cycles;
    Check(!cycles.Connect(PinId(c, 2), PinId(a, 0), reason) && cycles == acyclic, "multi-node cycle is rejected without losing existing connections");
    Check(!cycles.Connect(PinId(a, 2), PinId(a, 0), reason), "self link is rejected");
    Check(!cycles.Connect(PinId(a, 0), PinId(b, 0), reason), "two inputs cannot be connected");
    json = crude_json::value::parse(cycles.ToJson());
    crude_json::value link(crude_json::type_t::object);
    link["id"] = 500.0; link["output"] = static_cast<double>(PinId(c, 2)); link["input"] = static_cast<double>(PinId(a, 0));
    json["links"].push_back(link);
    Check(!loaded.FromJson(json.dump(), error), "saved cyclic graph is also rejected");
}
void Capture()
{
    auto* draw = ImGui::GetDrawData();
    const auto root = std::filesystem::path(IX_GRAPH_TEST_OUTPUT);
    std::ofstream output(root / "material_graph_draw.json");
    output << "{\"width\":1280,\"height\":800,\"lists\":[";
    for (int n = 0; n < draw->CmdListsCount; ++n)
    {
        const auto* list = draw->CmdLists[n]; if (n) output << ',';
        output << "{\"vertices\":[";
        for (int i=0;i<list->VtxBuffer.Size;++i) { const auto& v=list->VtxBuffer[i]; if(i)output<<','; output<<'['<<v.pos.x<<','<<v.pos.y<<','<<v.uv.x<<','<<v.uv.y<<','<<v.col<<']'; }
        output << "],\"indices\":[";
        for (int i=0;i<list->IdxBuffer.Size;++i) { if(i)output<<','; output<<list->IdxBuffer[i]; }
        output << "],\"commands\":[";
        for (int i=0;i<list->CmdBuffer.Size;++i) { const auto& c=list->CmdBuffer[i]; if(i)output<<','; output<<'['<<c.ElemCount<<','<<c.IdxOffset<<','<<c.VtxOffset<<','<<c.ClipRect.x<<','<<c.ClipRect.y<<','<<c.ClipRect.z<<','<<c.ClipRect.w<<']'; }
        output << "]}";
    }
    unsigned char* atlas; int w,h; ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas,&w,&h);
    output << "],\"atlas_width\":"<<w<<",\"atlas_height\":"<<h<<'}';
    std::ofstream texture(root / "material_graph_atlas.rgba",std::ios::binary);
    texture.write(reinterpret_cast<char*>(atlas),static_cast<std::streamsize>(w)*h*4);
}
void Editor(bool capture)
{
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = {1280, 800}; io.DeltaTime = 1.0f/60; io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels; int w, h; io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    {
        MaterialGraphPanel panel; bool open = true;
        const char* inspector = ICON_FA_CIRCLE_INFO "  Inspector###Inspector";
        const auto frame = [&] {
            ImGui::NewFrame();
            if (panel.OwnsKeyboard(true)) panel.HandleHotkeys();
            ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize({950, 760}); panel.RenderView(open);
            ImGui::SetNextWindowPos({960, 0}); ImGui::SetNextWindowSize({320, 760});
            ImGui::Begin(inspector); panel.RenderInspector(); ImGui::End(); ImGui::Render();
        };
        frame(); frame(); frame();
        Check(ImGui::GetDrawData()->TotalVtxCount > 1000 && panel.Document().nodes.size() == 3, "production panel renders nodes, pins and links with ImGui 1.92.8");
        const auto drag = [&](ImVec2 from, ImVec2 to, int button) {
            io.AddMousePosEvent(from.x, from.y); frame(); io.AddMouseButtonEvent(button, true); frame();
            io.AddMousePosEvent(to.x, to.y); frame(); frame(); io.AddMouseButtonEvent(button, false); frame(); frame();
        };
        // Drag the actual pin hit targets, including a rejected type mismatch.
        drag(MaterialGraphPanelTestAccess::Pin(panel, PinId(1, 0)), MaterialGraphPanelTestAccess::Pin(panel, PinId(3, 1)), 0);
        Check(panel.Document().links.size() == 2 && panel.Document().links[1].output == PinId(2, 0), "mouse drag cannot replace Roughness with a Color link");
        drag(MaterialGraphPanelTestAccess::Pin(panel, PinId(2, 0)), MaterialGraphPanelTestAccess::Pin(panel, PinId(3, 2)), 0);
        Check(panel.Document().links.size() == 3 && panel.CanUndo(), "mouse pin drag creates a typed connection and undo entry");
        panel.Undo(); frame(); frame(); Check(panel.Document().links.size() == 2 && panel.CanRedo(), "undo restores graph content and recreates node editor context");
        panel.Undo(true); frame(); frame(); Check(panel.Document().links.size() == 3, "redo restores the connection");
        const auto position = panel.Document().FindNode(1)->position;
        const auto header = MaterialGraphPanelTestAccess::NodeHeader(panel, 1);
        drag(header, {header.x+60, header.y+30}, 0);
        Check(panel.Document().FindNode(1)->position != position, "mouse drag moves a node in canvas coordinates");
        panel.Undo(); frame(); frame(); Check(panel.Document().FindNode(1)->position == position, "node move is one undoable edit");
        MaterialGraphPanelTestAccess::Select(panel, 1); frame(); frame();
        Check(MaterialGraphPanelTestAccess::Selected(panel) == 1, "node selection populates the Inspector");
        io.AddMousePosEvent(1140, 75); io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddInputCharactersUTF8("Forest Tint"); frame();
        io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
        Check(panel.Document().FindNode(1)->name == "Forest Tint", "Inspector name edit updates the selected graph node");
        panel.Undo(); frame(); frame(); Check(panel.Document().FindNode(1)->name == "Base Color", "Inspector edit is one undoable transaction");
        panel.Undo(true); frame(); frame(); MaterialGraphPanelTestAccess::Select(panel, 1); frame();
        if (capture) Capture();
        // Exercise the library's native selection/delete path instead of bypassing it.
        const auto selectHeader = MaterialGraphPanelTestAccess::NodeHeader(panel, 1);
        io.AddMousePosEvent(selectHeader.x, selectHeader.y); io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame();
        ImGui::SetWindowFocus(MaterialGraphPanel::WindowName);
        io.AddKeyEvent(ImGuiKey_Delete, true); frame(); io.AddKeyEvent(ImGuiKey_Delete, false); frame(); frame();
        Check(!panel.Document().FindNode(1) && panel.Document().links.size() == 2, "Delete key removes selected node and dependent links");
        panel.Undo(); frame(); frame(); Check(panel.Document().FindNode(1) != nullptr, "undo restores a deleted node and its links");
        const auto root = std::filesystem::temp_directory_path() / ("ixtreeme-material-graph-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root); const auto file = root / "draft.materialgraph";
        Check(panel.Save(file) && panel.Save(file), "save writes and safely replaces a graph file");
        MaterialGraphPanel reopened;
        Check(reopened.Load(file) && reopened.Document().nodes == panel.Document().nodes && reopened.Document().links == panel.Document().links, "opening saved graph restores parameters, connections and layout");
        std::ofstream(root / "bad.materialgraph") << "not JSON";
        const auto intact = reopened.Document();
        Check(!reopened.Load(root / "bad.materialgraph") && reopened.Document() == intact, "failed Open preserves current document");
        std::filesystem::remove(file); std::filesystem::remove(root / "bad.materialgraph"); std::filesystem::remove(root);
        const auto normalZoom = MaterialGraphPanelTestAccess::Zoom(panel);
        io.AddMousePosEvent(400, 350); frame(); io.AddMouseWheelEvent(0, 2); frame(); for(int i=0;i<30;++i)frame();
        Check(std::abs(MaterialGraphPanelTestAccess::Zoom(panel) - normalZoom) > .01f, "mouse wheel changes canvas zoom");
        const auto nodeScreen = MaterialGraphPanelTestAccess::NodeHeader(panel, 1);
        const auto beforePan = panel.Document();
        drag({800, 480}, {840, 510}, 2);
        const auto afterPan = MaterialGraphPanelTestAccess::NodeHeader(panel, 1);
        Check(std::abs(afterPan.x - nodeScreen.x) > 10 && panel.Document() == beforePan, "MMB pans the view without changing material data or node positions");
        const auto nodeCount = panel.Document().nodes.size();
        io.AddMousePosEvent(358, 37); io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
        Check(panel.OwnsKeyboard(true), "Add Node popup keeps keyboard input scoped to the graph");
        auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
        const ImVec2 search{popup->WorkRect.Min.x+35,popup->WorkRect.Min.y+8};
        io.AddMousePosEvent(search.x,search.y);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();
        io.AddInputCharactersUTF8("scalar");frame();frame();
        popup=ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
        const ImVec2 firstMenuItem{popup->WorkRect.Min.x+35,popup->WorkRect.Min.y+ImGui::GetFrameHeight()+ImGui::GetStyle().ItemSpacing.y+8};
        io.AddMousePosEvent(firstMenuItem.x,firstMenuItem.y);frame();
        io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();frame();
        Check(panel.Document().nodes.size() == nodeCount + 1, "Add Node popup creates an editable graph node");
        MaterialGraphPanelTestAccess::LargeGraph(panel); frame(); frame(); frame();
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; ++i) frame();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count()/30;
        Check(panel.Document().nodes.size() == 300 && panel.Document().links.size() == 299, "300-node connected graph remains stable while rendering");
        std::cout << "300-node graph CPU frame: " << ms << " ms (headless, no GPU submission)\n";
        MaterialGraphPanelTestAccess::Smart(panel);frame();frame();
        const auto smart=panel.Document();Check(smart.layers.size()==3,"production Inspector renders Weathered Stone Layer Stack");
        MaterialGraphPanelTestAccess::DisableLayer(panel);Check(!panel.Document().layers.back().enabled && panel.CanUndo(),"layer enable edit joins existing history");
        panel.Undo();Check(panel.Document()==smart,"Undo restores canonical graph and Layer Stack together");
        panel.Undo(true);Check(!panel.Document().layers.back().enabled,"Redo restores layer enablement and topology together");panel.Undo();
        MaterialGraphPanelTestAccess::Detach(panel);Check(panel.Document().layers.empty(),"explicit graph conversion removes managed ownership");
        panel.Undo();Check(panel.Document()==smart,"Undo restores the complete managed graph after conversion");frame();frame();
        const auto smartRoot=std::filesystem::temp_directory_path()/("ix-smart-editor-"+generateGuidV4().toString());std::filesystem::create_directories(smartRoot);
        AssetDatabase::Instance().scan(smartRoot);const auto definitionFile=smartRoot/"stone.materialgraph",instanceFile=smartRoot/"stone.ixmaterialinstance";
        Check(!panel.SaveInstance(instanceFile),"unsaved definition cannot produce a dangling instance reference");
        Check(panel.Save(definitionFile),"smart definition saves through production panel");MaterialGraphPanelTestAccess::Override(panel,.37f);
        Check(panel.SaveInstance(instanceFile),"production panel saves separate instance with definition GUID");
        const auto registeredGuid=AssetDatabase::Instance().resolvePath(definitionFile);
        AssetDatabase::Instance().scan(smartRoot);
        Check(registeredGuid && AssetDatabase::Instance().resolvePath(definitionFile)==registeredGuid,"graph GUID survives real asset database rescan and metadata parse");
        Check(detectAssetType(instanceFile)==AssetType::MaterialInstance,"instance is a recognized native asset type");
        const auto dependencies=AssetDatabase::Instance().loadDependencies(instanceFile);
        Check(registeredGuid && std::find(dependencies.begin(),dependencies.end(),*registeredGuid)!=dependencies.end(),"instance tracks the definition through native asset dependency metadata");
        const auto copiedDefinition=smartRoot/"copied.materialgraph";
        std::filesystem::copy_file(definitionFile,copiedDefinition);
        std::filesystem::copy_file(std::filesystem::path(definitionFile.wstring()+L".meta"),std::filesystem::path(copiedDefinition.wstring()+L".meta"));
        const auto copyGuid=AssetDatabase::Instance().getOrCreateGuid(copiedDefinition);
        Check(registeredGuid && copyGuid!=*registeredGuid && AssetDatabase::Instance().resolvePath(definitionFile)==registeredGuid,"copied metadata receives a new GUID without hijacking the original definition");
        Check(!panel.SaveInstance(definitionFile),"instance save cannot overwrite its material definition");
        MaterialGraphPanel restored;Check(restored.Load(definitionFile) && restored.LoadInstance(instanceFile),"production panel reloads definition and resolves its instance");
        const auto foundOverride=MaterialGraphPanelTestAccess::Overrides(restored).values.find("MossAmount");
        Check(foundOverride!=MaterialGraphPanelTestAccess::Overrides(restored).values.end() && foundOverride->second[0]==.37f,"saved instance restores exposed value without changing graph defaults");
        std::string instanceText,error;MaterialInstance wrong;LoadMaterialText(instanceFile,instanceText,error);wrong.FromJson(instanceText,error);wrong.definitionGuid=generateGuidV4().toString();
        const auto wrongFile=smartRoot/"wrong.ixmaterialinstance";SaveMaterialTextAtomic(wrongFile,wrong.ToJson(),error);const auto previousOverrides=MaterialGraphPanelTestAccess::Overrides(restored);
        Check(!restored.LoadInstance(wrongFile) && MaterialGraphPanelTestAccess::Overrides(restored)==previousOverrides,"mismatched instance fails without losing current overrides");
        std::filesystem::remove_all(smartRoot);
        open = false; frame(); Check(!open, "closed panel keeps document without drawing its canvas");
    }
    ImGui::DestroyContext();
}
}
int main(int argc, char** argv)
{
    Model(); Editor(argc > 1 && std::string(argv[1]) == "--capture");
    std::cout << "Material Graph failures: " << failures << '\n'; return failures ? 1 : 0;
}
