#include "MaterialGraphPanel.h"
#include "MaterialGraphIO.h"
#include "MaterialInstance.h"
#include "asset/AssetDatabase.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>
#include "UIHelpers.h"
#include "IconsFontAwesome6.h"
#include "asset/ExrImage.h"
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <fstream>
#include <windows.h>
#include <commdlg.h>

namespace material_tool
{
namespace ed = ax::NodeEditor;
namespace
{
ImVec4 PinColor(PinType type)
{
    switch(type) {
    case PinType::Color:return {0.38f,0.68f,0.95f,1};case PinType::Scalar:return {0.48f,0.82f,0.42f,1};
    case PinType::Vector2:return {.9f,.65f,.3f,1};case PinType::Vector3:return {.7f,.45f,.95f,1};
    case PinType::Texture:return {.9f,.45f,.45f,1};case PinType::Surface:return {.95f,.82f,.35f,1};
    }return {1,1,1,1};
}
std::filesystem::path ChooseGraphFile(bool save, const std::filesystem::path& current, bool instance = false, bool subgraph = false)
{
    wchar_t file[32768]{};
    const auto initial = current.wstring();
    if (initial.size() < std::size(file)) std::copy(initial.begin(), initial.end(), file);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    const auto* viewport = ImGui::GetMainViewport();
    dialog.hwndOwner = static_cast<HWND>(viewport->PlatformHandleRaw);
    dialog.lpstrFile = file; dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrFilter = instance ? L"Material Instance (*.ixmaterialinstance)\0*.ixmaterialinstance\0All Files\0*.*\0" :
        (subgraph && save ? L"Material Subgraph (*.ixmaterialsubgraph)\0*.ixmaterialsubgraph\0" : L"Material Documents\0*.materialgraph;*.ixsmartmaterial;*.ixmaterialsubgraph\0All Files\0*.*\0");
    dialog.lpstrDefExt = instance ? L"ixmaterialinstance" : (subgraph ? L"ixmaterialsubgraph" : L"materialgraph");
    dialog.lpstrTitle = save ? L"Save Material Graph" : L"Open Material Graph";
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) return std::filesystem::path(file);
    return {};
}
}
MaterialGraphPanel::MaterialGraphPanel() = default;
MaterialGraphPanel::~MaterialGraphPanel() { ResetContext(); if(provider_ && previewTexture_.IsValid())provider_->ReleasePreviewTexture(previewTexture_); }
void MaterialGraphPanel::SetServices(AssetLibrary* library,ixeditor::graphics::IEditorTextureProvider* provider) {
    if(provider_!=provider) {if(provider_ && previewTexture_.IsValid())provider_->ReleasePreviewTexture(previewTexture_);previewTexture_={};provider_=provider;previewDirty_=true;}
    if(library_!=library){library_=library;textures_.Clear();subgraphs_.Clear();forceCompile_=true;parameters_={};previewDirty_=true;}
    textures_.Configure(library_,path_.parent_path());
}
void MaterialGraphPanel::ResetContext()
{
    if (context_)
    {
        if (ed::GetCurrentEditor() == context_) ed::SetCurrentEditor(nullptr);
        ed::DestroyEditor(context_); context_ = nullptr;
    }
    layoutPending_ = true;
}
void MaterialGraphPanel::EnsureContext()
{
    if (context_) return;
    ed::Config config; config.SettingsFile = nullptr;
    config.NavigateButtonIndex = 2; config.ContextMenuButtonIndex = 1;
    config.CanvasSizeMode = ed::CanvasSizeMode::CenterOnly;
    context_ = ed::CreateEditor(&config);
    auto* previous = ed::GetCurrentEditor(); ed::SetCurrentEditor(context_);
    auto& style = ed::GetStyle();
    style.NodeRounding = 7; style.NodePadding = {16, 12, 16, 12}; style.NodeBorderWidth = 1;
    style.Colors[ed::StyleColor_Bg] = {0.09f, 0.10f, 0.12f, 1};
    style.Colors[ed::StyleColor_Grid] = {0.3f, 0.33f, 0.38f, 0.18f};
    style.Colors[ed::StyleColor_NodeBg] = {0.15f, 0.17f, 0.20f, 1};
    style.Colors[ed::StyleColor_NodeBorder] = {0.32f, 0.36f, 0.42f, 1};
    style.Colors[ed::StyleColor_SelNodeBorder] = {0.35f, 0.65f, 0.96f, 1};
    ed::SetCurrentEditor(previous);
}
bool MaterialGraphPanel::TakeInspectorRequest() { const bool result = requestInspector_; requestInspector_ = false; return result; }
bool MaterialGraphPanel::OwnsKeyboard(bool inspectorActive) const
{
    const auto* focused = ImGui::GetCurrentContext()->NavWindow;
    if (!focused) return false;
    const auto* root = focused->RootWindowPopupTree;
    return root == ImGui::FindWindowByName(WindowName) ||
        (inspectorActive && root == ImGui::FindWindowByName(ICON_FA_CIRCLE_INFO "  Inspector###Inspector"));
}
void MaterialGraphPanel::RecordUndo(const MaterialGraphDocument& before)
{
    if (before == document_) return;
    if (undo_.size() >= 64) undo_.erase(undo_.begin());
    undo_.push_back(before); redo_.clear();
}
void MaterialGraphPanel::Undo(bool redo)
{
    auto& source = redo ? redo_ : undo_; auto& target = redo ? undo_ : redo_;
    if (source.empty()) return;
    target.push_back(document_); document_ = std::move(source.back()); source.pop_back();
    dragBefore_.reset(); propertyBefore_.reset(); selected_ = 0;
    ResetContext(); status_ = redo ? "Edit restored" : "Edit undone";
}
void MaterialGraphPanel::HandleHotkeys()
{
    auto& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsAnyItemActive()) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveDialog(io.KeyShift);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Undo(io.KeyShift);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Undo(true);
}
void MaterialGraphPanel::AddNode(NodeKind kind)
{
    const auto before = document_;
    const auto id = document_.AddNode(kind, spawn_);
    if (!id) { status_ = "Only one PBR Output is allowed; graph size is limited"; return; }
    RecordUndo(before); selected_ = id; requestInspector_ = true;
    ed::SetNodePosition(ed::NodeId(id * 16), {spawn_[0], spawn_[1]});
    ed::SelectNode(ed::NodeId(id * 16)); spawn_[0] += 25; spawn_[1] += 25;
}
void MaterialGraphPanel::RenderNodeMenu()
{
    ImGui::SetNextItemWidth(260);ImGui::InputTextWithHint("##node_search","Search nodes...",nodeSearch_,sizeof(nodeSearch_));
    std::string filter=nodeSearch_;for(char& c:filter)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto item=[&](NodeKind kind){if(ImGui::MenuItem(NodeKindName(kind)))AddNode(kind);};
    if(!filter.empty()) {
        bool found=false;for(const auto kind:AllNodeKinds()) {std::string label=std::string(NodeKindName(kind))+" "+NodeCategory(kind);for(char& c:label)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if(label.find(filter)!=std::string::npos){item(kind);found=true;}}
        if(!found)ImGui::TextDisabled("No matching nodes");
    } else {
        for(const auto* category:{"Constants","Parameters","Coordinates","Textures","Math","Color","Masks","Materials","Subgraphs"})
            if(ImGui::BeginMenu(category)) {for(const auto kind:AllNodeKinds())if(std::string_view(NodeCategory(kind))==category)item(kind);ImGui::EndMenu();}
    }
}
void MaterialGraphPanel::RequestReplace(bool openFile)
{
    pendingOpen_ = openFile;
    if (document_ != saved_) { requestUnsaved_ = true; return; }
    if (openFile) OpenDialog();
    else
    {
        subgraphMode_=pendingPreset_==4; subgraphVersion_=1;
        document_ = subgraphMode_?MaterialSubgraph::Example().graph:(pendingPreset_==1?MaterialGraphDocument::WeatheredMetal():(pendingPreset_==2?MaterialGraphDocument::MossyRock():(pendingPreset_==3?MaterialGraphDocument::WeatheredStone():MaterialGraphDocument::Example()))); pendingPreset_=0; saved_ = MaterialGraphDocument::Example(); path_.clear();
        undo_.clear(); redo_.clear(); selected_ = 0; parameters_={}; previewDirty_=true; ResetContext(); fitPending_ = true; status_.clear();
        subgraphs_.Clear(); forceCompile_=true;
        if (subgraphMode_) { selected_=4; previewPin_=PinId(4,1); previewSettings_.channel=PreviewChannel::SelectedOutput; }
    }
}
void MaterialGraphPanel::RenderUnsavedPopup()
{
    if (requestUnsaved_) { ImGui::OpenPopup("Unsaved Material Graph"); requestUnsaved_ = false; }
    if (ImGui::BeginPopupModal("Unsaved Material Graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save changes before replacing this graph?");
        if (ImGui::Button("Save", {85, 0}))
        {
            SaveDialog();
            if (document_ == saved_) { ImGui::CloseCurrentPopup(); RequestReplace(pendingOpen_); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", {85, 0}))
        {
            ImGui::CloseCurrentPopup();
            // A cancelled Open dialog leaves the current document untouched.
            if (pendingOpen_) OpenDialog();
            else { saved_ = document_; RequestReplace(false); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {85, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
void MaterialGraphPanel::RenderToolbar()
{
    if (ImGui::Button("New")) {pendingPreset_=0;RequestReplace(false);}
    ImGui::SameLine(); if (ImGui::Button("Open...")) RequestReplace(true);
    ImGui::SameLine(); if (ImGui::Button("Save")) SaveDialog();
    ImGui::SameLine(); if (ImGui::Button("Save As...")) SaveDialog(true);
    ImGui::SameLine(); ImGui::BeginDisabled(!CanUndo()); if (ImGui::Button("Undo")) Undo(); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(!CanRedo()); if (ImGui::Button("Redo")) Undo(true); ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Add Node"))
    {
        const auto position = ed::ScreenToCanvas(ImGui::GetMousePos()); spawn_ = {position.x, position.y + 50};
        nodeSearch_[0]=0; ImGui::OpenPopup("Add Material Node");
    }
    if (ImGui::BeginPopup("Add Material Node")) { RenderNodeMenu(); ImGui::EndPopup(); }
    ImGui::SameLine(); if (ImGui::Button("Fit")) fitPending_ = true;
    ImGui::SameLine(); if(ImGui::Button("HLSL..."))ExportShader();
    ImGui::SameLine();if(ImGui::Button("Templates"))ImGui::OpenPopup("Material templates");
    if(ImGui::BeginPopup("Material templates")) {
        if(ImGui::MenuItem("Weathered metal (UV)")){pendingPreset_=1;RequestReplace(false);}
        if(ImGui::MenuItem("Mossy rock (world)")){pendingPreset_=2;RequestReplace(false);}
        if(ImGui::MenuItem("Smart Weathered Stone")){pendingPreset_=3;RequestReplace(false);}
        if(ImGui::MenuItem("New reusable subgraph")){pendingPreset_=4;RequestReplace(false);}
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("%s%s  |  %zu nodes, %zu links", path_.empty() ? "Untitled" : path_.filename().string().c_str(), document_ != saved_ ? " *" : "", document_.nodes.size(), document_.links.size());
}
void MaterialGraphPanel::DrawNode(const GraphNode& node)
{
    ed::BeginNode(ed::NodeId(node.id * 16)); ImGui::PushID(static_cast<int>(node.id));
    ImGui::TextColored(node.kind == NodeKind::PbrOutput ? ImVec4(0.94f, 0.72f, 0.32f, 1) : ImVec4(0.75f, 0.83f, 0.94f, 1), "%s", node.name.c_str());
    ImGui::TextDisabled("%s", NodeKindName(node.kind)); ImGui::Dummy({200, 5});
    if (node.region != GraphRegion::Custom) ImGui::TextDisabled("Managed connections");
    if (node.kind == NodeKind::Scalar || node.kind == NodeKind::ScalarParameter) { ImGui::Text("%.3f", node.value[0]); ImGui::Spacing(); }
    if (node.kind == NodeKind::Color || node.kind == NodeKind::ColorParameter)
    {
        const auto at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + 200, at.y + 25}, ImGui::ColorConvertFloat4ToU32({node.value[0], node.value[1], node.value[2], 1}), 3);
        ImGui::Dummy({200, 30});
    }
    const auto pins = NodePins(node);
    for (std::size_t slot = 0; slot < pins.size(); ++slot)
    {
        const auto& pin = pins[slot];
        ed::BeginPin(ed::PinId(PinId(node.id, slot)), pin.input ? ed::PinKind::Input : ed::PinKind::Output);
        ed::PinPivotAlignment({pin.input ? 0.0f : 1.0f, 0.5f});
        ImGui::BeginGroup();
        const float width = ImGui::CalcTextSize(pin.name).x;
        if (!pin.input) { ImGui::Dummy({std::max(0.0f, 182 - width), 0}); ImGui::SameLine(0, 0); }
        const auto dot = [&] {
            const auto at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled({at.x + 5, at.y + ImGui::GetTextLineHeight() * .5f}, 4, ImGui::ColorConvertFloat4ToU32(PinColor(pin.type)));
            ImGui::Dummy({10, ImGui::GetTextLineHeight()});
        };
        if (pin.input) { dot(); ImGui::SameLine(); }
        ImGui::TextUnformatted(pin.name);
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",PinTypeName(pin.type));
        if (!pin.input) { ImGui::SameLine(); dot(); }
        ImGui::EndGroup(); ed::EndPin();
    }
    ImGui::PopID(); ed::EndNode();
}
void MaterialGraphPanel::SyncPositions()
{
    for (auto& node : document_.nodes)
    {
        const auto at = ed::GetNodePosition(ed::NodeId(node.id * 16)); node.position = {at.x, at.y};
    }
}
void MaterialGraphPanel::RenderView(bool& open)
{
    if (!open) return;
    ImGui::SetNextWindowSize({900, 600}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WindowName, &open, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    EnsureContext(); auto* previous = ed::GetCurrentEditor(); ed::SetCurrentEditor(context_);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) requestInspector_ = true;
    RenderToolbar();
    // Toolbar undo/new/open may have replaced the context.
    EnsureContext(); ed::SetCurrentEditor(context_);
    const bool restoreSelection = layoutPending_ && document_.FindNode(selected_);
    if (layoutPending_)
    {
        for (const auto& node : document_.nodes) ed::SetNodePosition(ed::NodeId(node.id * 16), {node.position[0], node.position[1]});
        layoutPending_ = false;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Drag pins to connect  |  MMB: pan  |  Wheel: zoom  |  F: frame  |  Del: remove");
    const bool startDrag = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (startDrag) dragBefore_ = document_;
    ed::Begin("MaterialGraphCanvas", {0, std::max(100.0f, ImGui::GetContentRegionAvail().y - 45)});
    ed::EnableShortcuts(!ImGui::GetIO().WantTextInput);
    for (const auto& node : document_.nodes) DrawNode(node);
    if (restoreSelection) ed::SelectNode(ed::NodeId(selected_ * 16));
    for (const auto& link : document_.links)
        if (const auto pin = document_.FindPin(link.output)) ed::Link(ed::LinkId(link.id * 16 + 15), ed::PinId(link.output), ed::PinId(link.input), PinColor(pin->definition.type), 2);
    if (ed::BeginCreate())
    {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b)
        {
            std::string reason;
            if (document_.CanConnect(static_cast<std::uint32_t>(a.Get()), static_cast<std::uint32_t>(b.Get()), reason))
            {
                if (ed::AcceptNewItem(PinColor(PinType::Color), 2))
                {
                    const auto before = document_;
                    if (document_.Connect(static_cast<std::uint32_t>(a.Get()), static_cast<std::uint32_t>(b.Get()), reason)) { RecordUndo(before); status_ = "Connection created"; dragBefore_.reset(); }
                }
            }
            else { ed::RejectNewItem(ImVec4(0.93f, 0.36f, 0.34f, 1), 2); status_ = reason; }
        }
        ed::EndCreate();
    }
    if (ed::BeginDelete())
    {
        const auto before = document_; ed::LinkId link; ed::NodeId node;
        while (ed::QueryDeletedLink(&link)) {
            auto draft=document_;
            if (draft.RemoveLink(static_cast<std::uint32_t>(link.Get()) / 16)) { if (ed::AcceptDeletedItem()) document_=std::move(draft); }
            else { ed::RejectDeletedItem(); status_="Managed connections are read-only; use Layer Stack"; }
        }
        while (ed::QueryDeletedNode(&node)) {
            auto draft=document_;
            if (draft.RemoveNode(static_cast<std::uint32_t>(node.Get()) / 16)) { if (ed::AcceptDeletedItem()) document_=std::move(draft); }
            else { ed::RejectDeletedItem(); status_="This node belongs to or supplies the Layer Stack"; }
        }
        RecordUndo(before); ed::EndDelete();
    }
    if (ed::ShowBackgroundContextMenu())
    {
        const auto at = ed::ScreenToCanvas(ImGui::GetMousePos()); spawn_ = {at.x, at.y};
        ed::Suspend(); nodeSearch_[0]=0; ImGui::OpenPopup("Add Material Node"); ed::Resume();
    }
    ed::Suspend();
    if (ImGui::BeginPopup("Add Material Node")) { RenderNodeMenu(); ImGui::EndPopup(); }
    ed::Resume();
    if (fitPending_ && !document_.nodes.empty()) { ed::NavigateToContent(0); fitPending_ = false; }
    ed::End(); SyncPositions();
    // The library processes selection in End(), after all node hit targets exist.
    ed::NodeId selection;
    const auto selected = ed::GetSelectedNodes(&selection, 1) ? static_cast<std::uint32_t>(selection.Get()) / 16 : 0;
    if (selected != selected_) { selected_ = selected; requestInspector_ = true; }
    if (dragBefore_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { RecordUndo(*dragBefore_); dragBefore_.reset(); }
    ImGui::TextDisabled("Live preview and UV material baking are available in the Inspector.");
    if (!status_.empty()) ImGui::TextUnformatted(status_.c_str());
    RenderUnsavedPopup();
    ed::SetCurrentEditor(previous); ImGui::End();
}
void MaterialGraphPanel::RenderInspector()
{
    ImGui::TextUnformatted("Material Graph"); ImGui::Separator();
    if (document_.HasLayerStack()) RenderLayerStack();
    auto* node = document_.FindNode(selected_);
    if (node) {
    ImGui::TextDisabled("%s  |  Node %u", NodeKindName(node->kind), node->id);
    const bool protectedProperties = node->region != GraphRegion::Custom && node->kind != NodeKind::MaterialLayer && node->kind != NodeKind::ScalarParameter;
    if (protectedProperties) ImGui::TextWrapped("Managed properties: edit this node through Layer Stack or Convert to Custom Graph.");
    ImGui::BeginDisabled(protectedProperties);
    const auto edit = [&](auto draw) {
        const auto before = document_; const bool changed = draw();
        if (ImGui::IsItemActivated()) propertyBefore_ = before;
        if (changed && !propertyBefore_) propertyBefore_ = before;
        if (ImGui::IsItemDeactivatedAfterEdit() && propertyBefore_) { RecordUndo(*propertyBefore_); propertyBefore_.reset(); }
    };
    const auto immediate=[&](auto draw){const auto before=document_;if(draw())RecordUndo(before);};
    const auto number=[&](const char* label,float& value,float speed=.01f){edit([&]{return UI::Prop::DragFloat(label,&value,speed,-1000000.0f,1000000.0f,"%.3f");});};
    char name[256]; std::snprintf(name,sizeof(name),"%s",node->name.c_str());
    edit([&]{const bool changed=UI::Prop::InputText("Name",name,sizeof(name));if(changed)node->name=name;return changed;});
    using N=NodeKind;
    if(node->kind==N::ScalarParameter || node->kind==N::ColorParameter || node->kind==N::TextureParameter || node->kind==N::Vector2Parameter || node->kind==N::Vector3Parameter || node->kind==N::SubgraphInput || node->kind==N::SubgraphOutput) {
        char key[256];std::snprintf(key,sizeof(key),"%s",node->parameter.c_str());
        ImGui::BeginDisabled(node->region == GraphRegion::ManagedLayer);
        edit([&]{const bool changed=UI::Prop::InputText("Parameter",key,sizeof(key));if(changed)node->parameter=key;return changed;});
        ImGui::EndDisabled();
        ImGui::TextWrapped("Exposed by name. Instance overrides below leave the graph defaults intact.");
    }
    switch(node->kind) {
    case N::Subgraph:RenderSubgraphBinding(node->id);break;
    case N::SubgraphInput:case N::SubgraphOutput:{
        int type=static_cast<int>(node->interfaceType); const char* types[]={"Scalar","Color","Vector2","Vector3","Texture","Surface"};
        if (UI::Prop::Combo("Port type",&type,types,6)) {
            const auto before=document_; auto draft=document_; draft.FindNode(node->id)->interfaceType=static_cast<PinType>(type);
            MaterialGraphDocument checked;
            if (checked.FromJson(draft.ToJson(),status_)) { document_=std::move(draft); RecordUndo(before); node=document_.FindNode(selected_); }
        }
        if (node->kind==N::SubgraphInput) {
            if (node->interfaceType==PinType::Scalar) number("Default",node->value[0]);
            if (node->interfaceType==PinType::Color) edit([&]{return UI::Prop::ColorEdit4("Default (linear)",node->value.data());});
            if (node->interfaceType==PinType::Vector2) edit([&]{return UI::Prop::DragFloat2("Default XY",node->value.data(),.01f);});
            if (node->interfaceType==PinType::Vector3) edit([&]{return UI::Prop::DragFloat3("Default XYZ",node->value.data(),.01f);});
        }
        ImGui::TextWrapped("Stable declaration ID. Connected type changes are rejected; callers refresh interface changes explicitly. Texture/Surface inputs require a caller binding.");break;
    }
    case N::Scalar:case N::ScalarParameter:number("Value",node->value[0]);break;
    case N::Color:case N::ColorParameter:edit([&]{return UI::Prop::ColorEdit4("Color (linear)",node->value.data());});break;
    case N::Vector2:case N::Vector2Parameter:edit([&]{return UI::Prop::DragFloat2("XY",node->value.data(),.01f);});break;
    case N::Vector3:case N::Vector3Parameter:edit([&]{return UI::Prop::DragFloat3("XYZ",node->value.data(),.01f);});break;
    case N::TextureParameter:RenderTextureBinding(*node);break;
    case N::TextureSample:case N::Triplanar:
        RenderTextureBinding(*node);
        immediate([&]{return UI::Prop::Checkbox("sRGB RGB",&node->srgb);});
        ImGui::TextWrapped("RGB can decode sRGB. R/G/B/A and Normal outputs always read linear data. Connect Texture Parameter to override the local binding.");
        if(node->kind==N::Triplanar){number("Scale / meter",node->value[0]);number("Blend sharpness",node->value[1]);ImGui::TextWrapped("Color/data triplanar projection uses the geometric world normal. Triplanar tangent normal mapping requires a separate orientation transform.");}
        break;
    case N::TilingOffset:edit([&]{return UI::Prop::DragFloat2("Tiling",node->value.data(),.01f);});edit([&]{return UI::Prop::DragFloat2("Offset",node->value.data()+2,.01f);});break;
    case N::Noise:{number("Scale",node->value[0]);number("Octaves (1-8)",node->value[1],1);number("Persistence",node->value[2]);number("Seed",node->value[3],1);
        int space=node->settings[0]==0?0:1;const char* choices[]={"UV","World"};immediate([&]{if(UI::Prop::Combo("Default space",&space,choices,2)){node->settings[0]=static_cast<float>(space);return true;}return false;});break;}
    case N::HeightMask:number("Min height (m)",node->value[0]);number("Max height (m)",node->value[1]);ImGui::TextWrapped("Smooth mask from world position Y. Connected Position replaces the world input.");break;
    case N::SlopeMask:number("Min slope (deg)",node->value[0]);number("Max slope (deg)",node->value[1]);ImGui::TextWrapped("Uses the geometric normal, including undersides; upward is zero degrees.");break;
    case N::MaterialLayer:case N::PbrOutput:
        edit([&]{return UI::Prop::ColorEdit4("Base color",node->value.data());});number("Roughness",node->settings[0]);number("Metallic",node->settings[1]);number("AO",node->settings[2]);number("Height",node->settings[3]);
        ImGui::TextWrapped("Unconnected channels use these defaults. A connected Surface supplies all channels; explicit channel links override it.");break;
    case N::LayerBlend:case N::HeightBlend:number("Mask",node->value[0]);if(node->kind==N::HeightBlend)number("Transition width",node->value[1]);break;
    case N::NormalBlend:number("Detail strength",node->value[0]);ImGui::TextWrapped("Reoriented normal blending; both inputs must be tangent-space normals.");break;
    case N::NormalStrength:number("Strength",node->value[0]);ImGui::TextWrapped("Tangent-space normal: 0 is flat, 1 preserves it, values above 1 amplify XY.");break;
    case N::NormalUnpack:{edit([&]{return UI::Prop::ColorEdit4("Encoded normal (linear)",node->value.data());});bool flip=node->settings[0]==1;immediate([&]{if(UI::Prop::Checkbox("Flip green (DirectX/OpenGL)",&flip)){node->settings[0]=flip?1.0f:0.0f;return true;}return false;});break;}
    case N::NormalFlipY:case N::Normalize:case N::Length:case N::Cross:case N::Distance:case N::Reflect:edit([&]{return UI::Prop::DragFloat3("Unconnected vector",node->value.data(),.01f);});break;
    case N::MaskCombine:{number("A",node->value[0]);number("B",node->value[1]);int mode=static_cast<int>(node->settings[0]);const char* choices[]={"Multiply","Add","Minimum","Maximum","Subtract"};immediate([&]{if(UI::Prop::Combo("Operation",&mode,choices,5)){node->settings[0]=static_cast<float>(mode);return true;}return false;});break;}
    case N::Fresnel:number("Power",node->value[0]);ImGui::TextWrapped("World-space geometric normal and world-space direction toward the camera.");break;
    case N::Remap:number("Value",node->settings[0]);number("In Min",node->value[0]);number("In Max",node->value[1]);number("Out Min",node->value[2]);number("Out Max",node->value[3]);break;
    case N::VectorScale:edit([&]{return UI::Prop::DragFloat3("XYZ",node->value.data(),.01f);});number("Scale",node->settings[0]);break;
    case N::AO:case N::Curvature:case N::Cavity:ImGui::TextWrapped("Reads supplied geometry data. Use Texture Sample for imported baked masks. The preview data below is explicit test data; the engine does not yet bake geometry curvature or cavity.");break;
    default:
        for(size_t slot=0;slot<NodePins(node->kind).size();++slot)if(node->kind!=N::Blend && NodePins(node->kind)[slot].input && NodePins(node->kind)[slot].type==PinType::Scalar && slot<4) {
            ImGui::PushID(static_cast<int>(slot));number(NodePins(node->kind)[slot].name,node->value[slot]);ImGui::PopID();
        }
        if(node->kind==N::Blend)number("Factor",node->settings[0]);
        if(node->kind==N::ColorMultiply || node->kind==N::ColorAdd || node->kind==N::Blend || node->kind==N::SplitColor)edit([&]{return UI::Prop::ColorEdit4("Unconnected A",node->value.data());});
        if(node->kind==N::SplitVector || node->kind==N::Dot)edit([&]{return UI::Prop::DragFloat3("Unconnected A",node->value.data(),.01f);});
        break;
    }
    ImGui::EndDisabled();
    if(ImGui::CollapsingHeader("Connections"))for(const auto& link:document_.links)if(link.input/16==selected_ || link.output/16==selected_) {
        const auto from=document_.FindPin(link.output),to=document_.FindPin(link.input);ImGui::TextWrapped("%s.%s -> %s.%s",document_.FindNode(from->node)->name.c_str(),from->definition.name,document_.FindNode(to->node)->name.c_str(),to->definition.name);
    }
    } else ImGui::TextWrapped("Select a node in Material Graph to edit its parameters.");
    RenderInstanceParameters();RenderPreview();
}

bool MaterialGraphPanel::Load(const std::filesystem::path& path)
{
    std::string text;
    if (!LoadMaterialText(path, text, status_)) return false;
    MaterialGraphDocument draft;
    const bool subgraph = detectAssetType(path)==AssetType::MaterialSubgraph;
    std::uint32_t interfaceVersion=1;
    if (subgraph) {
        MaterialSubgraph asset; if (!asset.FromJson(text,status_)) return false;
        draft=std::move(asset.graph); interfaceVersion=asset.interfaceVersion;
    } else if (!draft.FromJson(text, status_)) return false;
    document_ = std::move(draft); saved_ = document_; path_ = path; undo_.clear(); redo_.clear();
    dragBefore_.reset(); propertyBefore_.reset(); selected_ = 0; ResetContext(); fitPending_ = true;
    subgraphMode_=subgraph; subgraphVersion_=interfaceVersion; subgraphs_.Clear(); forceCompile_=true;
    if (subgraph) for (const auto& node:document_.nodes) if (node.kind==NodeKind::SubgraphOutput) { selected_=node.id; previewPin_=PinId(node.id,1); previewSettings_.channel=PreviewChannel::SelectedOutput; break; }
    parameters_={};previewDirty_=true;textures_.Clear(); textures_.Configure(library_,path_.parent_path()); status_ = subgraph?"Subgraph opened":"Graph opened"; return true;
}
bool MaterialGraphPanel::Save(const std::filesystem::path& path)
{
    const bool subgraph = detectAssetType(path)==AssetType::MaterialSubgraph;
    if (subgraphMode_ && !subgraph) { status_="Save subgraph assets with the .ixmaterialsubgraph extension"; return false; }
    if (!subgraph && detectAssetType(path)!=AssetType::MaterialGraph) { status_="Use .materialgraph, .ixsmartmaterial or .ixmaterialsubgraph"; return false; }
    auto text = document_.ToJson();
    MaterialGraphDocument checked;
    if (!checked.FromJson(text, status_)) return false;
    if (subgraph) {
        MaterialSubgraph asset{subgraphVersion_,document_}; std::vector<MaterialGraphPort> ports;
        if (!asset.GetInterface(ports,status_)) return false; text=asset.ToJson();
    }
    if (!SaveMaterialTextAtomic(path, text, status_)) return false;
    auto& database=AssetDatabase::Instance(); const auto guid=database.getOrCreateGuid(path);
    std::vector<Guid> dependencies;
    for (const auto& node:document_.nodes) if (const auto dependency=Guid::fromString(node.assetGuid))
        if (std::find(dependencies.begin(),dependencies.end(),*dependency)==dependencies.end()) dependencies.push_back(*dependency);
    if (!IsMaterialAssetGuid(guid.toString()) || !database.writeDependencies(path,dependencies)) { status_="Definition saved, but asset identity/dependency metadata failed"; return false; }
    path_ = path; saved_ = document_; subgraphMode_=subgraph; textures_.Configure(library_,path_.parent_path()); previewDirty_=true; status_ = subgraph?"Subgraph saved":"Graph saved"; return true;
}
bool MaterialGraphPanel::CompileMaterial(MaterialGraphProgram& program, std::string& error, std::uint32_t previewPin)
{
    if (&program!=&program_) forceCompile_=true;
    subgraphs_.BeginCompilation();
    const bool result=program.Compile(document_,error,previewPin,subgraphs_.Resolver());
    subgraphs_.FinishCompilation(); return result;
}
bool MaterialGraphPanel::SaveInstance(const std::filesystem::path& path)
{
    if (path_.empty() || document_ != saved_) { status_="Save the material definition before saving an instance"; return false; }
    std::error_code sameFileError;
    if (std::filesystem::equivalent(path_,path,sameFileError)) { status_="An instance cannot replace its material definition"; return false; }
    MaterialGraphProgram program;
    if (!CompileMaterial(program,status_)) return false;
    MaterialInstance instance; instance.definitionGuid=AssetDatabase::Instance().getOrCreateGuid(path_).toString(); instance.overrides=parameters_;
    if (!instance.Validate(program,status_)) return false;
    if (!SaveMaterialTextAtomic(path,instance.ToJson(),status_)) return false;
    auto& database=AssetDatabase::Instance();database.getOrCreateGuid(path);
    std::vector<Guid> dependencies;
    if (const auto guid=Guid::fromString(instance.definitionGuid)) dependencies.push_back(*guid);
    for (const auto& [name,resource]:instance.overrides.textures) {
        (void)name;
        if (resource.starts_with("guid:")) if (const auto guid=Guid::fromString(resource.substr(5)))
            if (std::find(dependencies.begin(),dependencies.end(),*guid)==dependencies.end()) dependencies.push_back(*guid);
    }
    if (!database.writeDependencies(path,dependencies)) { status_="Instance saved, but asset dependency metadata could not be written"; return false; }
    status_="Material instance saved"; return true;
}
bool MaterialGraphPanel::LoadInstance(const std::filesystem::path& path)
{
    if (path_.empty()) { status_="Open the material definition before loading its instance"; return false; }
    std::string text; MaterialInstance instance; MaterialGraphProgram program;
    if (!LoadMaterialText(path,text,status_) || !instance.FromJson(text,status_) || !CompileMaterial(program,status_)) return false;
    if (instance.definitionGuid!=AssetDatabase::Instance().getOrCreateGuid(path_).toString()) { status_="This instance references a different material definition"; return false; }
    if (!instance.geometryBakeGuid.empty()) { status_="Geometry bake binding requires the geometry baker integration"; return false; }
    if (!instance.Validate(program,status_)) return false;
    parameters_=instance.overrides; textures_.Clear(); previewDirty_=true; status_="Material instance loaded"; return true;
}
void MaterialGraphPanel::SaveDialog(bool saveAs)
{
    const auto destination = saveAs || path_.empty() ? ChooseGraphFile(true, path_,false,subgraphMode_) : path_;
    if (!destination.empty()) Save(destination);
}
void MaterialGraphPanel::OpenDialog()
{
    const auto source = ChooseGraphFile(false, path_);
    if (!source.empty()) Load(source);
}
void MaterialGraphPanel::RenderTextureBinding(GraphNode& node)
{
    ImGui::PushID(static_cast<int>(node.id));
    std::string label=node.resource.empty()?"Drop texture here":std::filesystem::path(std::u8string(node.resource.begin(),node.resource.end())).filename().string();
    if(library_ && !node.assetGuid.empty())for(const auto& entry:library_->Entries())if(entry.guid==node.assetGuid){label=entry.displayName;break;}
    ImGui::Button(label.c_str(),{std::max(100.0f,ImGui::GetContentRegionAvail().x),32});
    if(ImGui::BeginDragDropTarget()) {
        if(const auto* payload=ImGui::AcceptDragDropPayload("ASSET_ID")) {
            std::string id(static_cast<const char*>(payload->Data),payload->DataSize);if(!id.empty() && id.back()==0)id.pop_back();
            const auto entry=library_?library_->FindById(id):std::optional<AssetLibrary::Entry>{};
            if(entry && entry->category==AssetLibrary::Category::Texture) {
                const auto before=document_;node.resource=library_->AssetRelativePath(*entry);node.assetGuid=entry->guid;node.assetId=entry->id;
                if(node.kind!=NodeKind::TextureParameter)node.srgb=entry->textureRole==AssetLibrary::TextureRole::Diffuse || entry->textureRole==AssetLibrary::TextureRole::Unknown;
                if(client::asset::IsExrPath(library_->AbsolutePath(*entry)))node.srgb=false;
                RecordUndo(before);textures_.Clear();previewDirty_=true;
            } else status_="This binding accepts textures only";
        }ImGui::EndDragDropTarget();
    }
    if(ImGui::SmallButton("Browse...")) {
        wchar_t file[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
        dialog.lpstrFile=file;dialog.nMaxFile=static_cast<DWORD>(std::size(file));dialog.lpstrFilter=L"Textures\0*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.exr\0All Files\0*.*\0";dialog.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
        if(GetOpenFileNameW(&dialog)) {
            const auto before=document_;const auto chosen=std::filesystem::path(file);
            if(library_) {AssetLibrary::Entry imported;std::string error;
                if(library_->Import(AssetLibrary::Category::Texture,chosen,imported,error)){node.resource=library_->AssetRelativePath(imported);node.assetGuid=imported.guid;node.assetId=imported.id;}else status_=error;
            }else {const auto utf8=chosen.generic_u8string();node.resource.assign(utf8.begin(),utf8.end());node.assetGuid.clear();node.assetId.clear();}
            if(client::asset::IsExrPath(chosen))node.srgb=false;RecordUndo(before);textures_.Clear();previewDirty_=true;
        }
    }ImGui::SameLine();if(ImGui::SmallButton("Clear")){const auto before=document_;node.resource.clear();node.assetGuid.clear();node.assetId.clear();RecordUndo(before);textures_.Clear();}
    ImGui::PopID();
}
void MaterialGraphPanel::RenderSubgraphBinding(std::uint32_t id)
{
    auto* node=document_.FindNode(id);
    if (!node) return;
    ImGui::TextWrapped("Referenced subgraph: %s",node->resource.empty()?"Unbound":node->resource.c_str());
    const bool browse=ImGui::Button("Bind subgraph..."); ImGui::SameLine(); const bool refresh=ImGui::Button("Refresh interface");
    if (browse || refresh) {
        std::string guid=node->assetGuid, resource=node->resource;
        if (browse) {
            wchar_t file[32768]{}; OPENFILENAMEW dialog{}; dialog.lStructSize=sizeof(dialog);
            dialog.hwndOwner=static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
            dialog.lpstrFile=file; dialog.nMaxFile=static_cast<DWORD>(std::size(file)); dialog.lpstrFilter=L"Material Subgraph\0*.ixmaterialsubgraph\0";
            dialog.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
            if (!GetOpenFileNameW(&dialog)) return;
            guid=AssetDatabase::Instance().getOrCreateGuid(file).toString(); resource=std::filesystem::path(file).filename().string();
        }
        const auto before=document_; MaterialSubgraph asset; subgraphs_.Clear(); forceCompile_=true;
        if (subgraphs_.Resolve(guid,asset,status_) && BindMaterialSubgraph(document_,id,guid,asset,status_)) {
            document_.FindNode(id)->resource=resource; RecordUndo(before); ResetContext(); previewDirty_=true; status_="Subgraph interface bound";
        }
        return;
    }
    ImGui::Text("Interface version: %u",node->interfaceVersion);
    for (auto& port:node->ports) if (port.input && port.type!=PinType::Texture && port.type!=PinType::Surface) {
        ImGui::PushID(static_cast<int>(port.id)); const auto before=document_; bool changed=false;
        if (port.type==PinType::Scalar) changed=UI::Prop::DragFloat(port.name.c_str(),port.defaultValue.data(),.01f,-1000000.0f,1000000.0f,"%.3f");
        if (port.type==PinType::Color) changed=UI::Prop::ColorEdit4(port.name.c_str(),port.defaultValue.data());
        if (port.type==PinType::Vector2) changed=UI::Prop::DragFloat2(port.name.c_str(),port.defaultValue.data(),.01f);
        if (port.type==PinType::Vector3) changed=UI::Prop::DragFloat3(port.name.c_str(),port.defaultValue.data(),.01f);
        if (ImGui::IsItemActivated()) propertyBefore_=before;
        if (changed && !propertyBefore_) propertyBefore_=before;
        if (ImGui::IsItemDeactivatedAfterEdit() && propertyBefore_) { RecordUndo(*propertyBefore_); propertyBefore_.reset(); }
        ImGui::PopID();
    }
    ImGui::TextWrapped("Unconnected numeric inputs use these defaults. Connected inputs take precedence. Published parameters appear below with a scope for this call.");
}
void MaterialGraphPanel::RenderLayerStack()
{
    if (!ImGui::CollapsingHeader("Layer Stack", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!document_.HasLayerStack()) { ImGui::TextWrapped("Custom graph. Choose Smart Weathered Stone from Templates to create a managed stack."); return; }
    if (ImGui::Button("Add Layer")) {
        const auto before=document_; document_.AddMaterialLayer("Fill Layer",status_); RecordUndo(before); return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Convert to Custom Graph")) {
        const auto before=document_; document_.ConvertToCustomGraph(); RecordUndo(before);
        status_="Layer ownership detached; Undo restores the stack"; return;
    }
    const auto layers=document_.layers;
    for (std::size_t index=0;index<layers.size();++index) {
        const auto layer=layers[index];
        auto& source=*document_.FindNode(layer.sourceNode); auto& opacity=*document_.FindNode(layer.opacityNode); auto& blend=*document_.FindNode(layer.blendNode);
        ImGui::PushID(static_cast<int>(layer.id));
        enum class Action { None, Enable, Move, Duplicate, Remove, Mask, Blend };
        Action action=Action::None; bool enabled=layer.enabled, heightBlend=blend.kind==NodeKind::HeightBlend;
        std::uint32_t maskPin=layer.maskPin; std::size_t moveIndex=index;
        if (ImGui::Checkbox("##enabled",&enabled)) action=Action::Enable;
        ImGui::SameLine();
        if (ImGui::Selectable(source.name.c_str(),selected_==source.id)) selected_=source.id;
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("MATERIAL_LAYER_ID",&layer.id,sizeof(layer.id)); ImGui::TextUnformatted(source.name.c_str()); ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload=ImGui::AcceptDragDropPayload("MATERIAL_LAYER_ID")) if (payload->DataSize==sizeof(std::uint32_t)) {
                const auto moved=*static_cast<const std::uint32_t*>(payload->Data);
                if (moved!=layer.id) {
                    const auto before=document_; document_.MoveMaterialLayer(moved,index,status_); RecordUndo(before);
                    ImGui::EndDragDropTarget(); ImGui::PopID(); return;
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Duplicate")) action=Action::Duplicate;
            if (ImGui::MenuItem("Remove")) action=Action::Remove;
            ImGui::EndPopup();
        }
        const auto beforeOpacity=document_;
        const bool changed=UI::Prop::SliderFloat("Opacity",&opacity.value[0],0.0f,1.0f);
        if (ImGui::IsItemActivated()) propertyBefore_=beforeOpacity;
        if (changed && !propertyBefore_) propertyBefore_=beforeOpacity;
        if (ImGui::IsItemDeactivatedAfterEdit() && propertyBefore_) { RecordUndo(*propertyBefore_); propertyBefore_.reset(); }
        int mode=heightBlend?1:0; const char* modes[]={"Normal","Height"};
        if (UI::Prop::Combo("Blend",&mode,modes,2)) { heightBlend=mode==1; action=Action::Blend; }
        if (ImGui::SmallButton("Up") && index>0) { moveIndex=index-1; action=Action::Move; }
        ImGui::SameLine(); if (ImGui::SmallButton("Down") && index+1<layers.size()) { moveIndex=index+1; action=Action::Move; }
        ImGui::SameLine(); if (layer.maskPin && ImGui::SmallButton("Preview mask")) { selected_=layer.maskPin/16; previewPin_=layer.maskPin; previewSettings_.channel=PreviewChannel::SelectedOutput; }
        const auto selectedMask=document_.FindPin(layer.maskPin);
        if (ImGui::BeginCombo("Mask",selectedMask?document_.FindNode(selectedMask->node)->name.c_str():"None")) {
            if (ImGui::Selectable("None",!layer.maskPin)) { maskPin=0; action=Action::Mask; }
            for (const auto& node:document_.nodes) {
                const auto pins=NodePins(node);
                for (std::size_t slot=0;slot<pins.size();++slot) if (!pins[slot].input && pins[slot].type==PinType::Scalar) {
                    const auto pin=PinId(node.id,slot);const auto label=node.name+" / "+pins[slot].name+"##"+std::to_string(pin);
                    if (ImGui::Selectable(label.c_str(),layer.maskPin==pin)) { maskPin=pin; action=Action::Mask; }
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::TreeNode("Channel overrides")) {
            const char* names[]={"Base Color","Roughness","Metallic","Emissive","Normal","AO","Height","Opacity"};
            for (unsigned k=0;k<8;++k) {
                auto bits=static_cast<unsigned>(blend.settings[2]); bool on=(bits&(1U<<k))!=0;
                const auto before=document_;
                if (ImGui::Checkbox(names[k],&on)) { blend.settings[2]=static_cast<float>(on?bits|(1U<<k):bits&~(1U<<k)); RecordUndo(before); }
            }
            ImGui::TreePop();
        }
        ImGui::Separator(); ImGui::PopID();
        if (action!=Action::None) {
            if (propertyBefore_) { RecordUndo(*propertyBefore_); propertyBefore_.reset(); }
            const auto before=document_;
            switch(action) {
            case Action::Enable:document_.SetMaterialLayerEnabled(layer.id,enabled,status_);break;
            case Action::Move:document_.MoveMaterialLayer(layer.id,moveIndex,status_);break;
            case Action::Duplicate:document_.DuplicateMaterialLayer(layer.id,status_);break;
            case Action::Remove:document_.RemoveMaterialLayer(layer.id,status_);selected_=0;break;
            case Action::Mask:document_.SetMaterialLayerMask(layer.id,maskPin,status_);break;
            case Action::Blend:document_.SetMaterialLayerHeightBlend(layer.id,heightBlend,status_);break;
            default:break;
            }
            RecordUndo(before); return;
        }
    }
}
void MaterialGraphPanel::RenderInstanceParameters()
{
    if(!ImGui::CollapsingHeader("Instance preview parameters"))return;
    ImGui::TextWrapped("Overrides affect preview and bake. Save them as an instance referencing the saved definition.");
    if (ImGui::Button("Save instance...")) { const auto path=ChooseGraphFile(true,{},true); if (!path.empty()) SaveInstance(path); }
    ImGui::SameLine(); if (ImGui::Button("Load instance...")) { const auto path=ChooseGraphFile(false,{},true); if (!path.empty()) LoadInstance(path); }
    if (ImGui::SmallButton("Reset all overrides")) { parameters_={}; textures_.Clear(); previewDirty_=true; }
    for(const auto& parameter:program_.Layout().parameters) {
        const auto& name=parameter.name; const auto type=parameter.type;
        ImGui::PushID(name.c_str());
        auto value=parameters_.values.contains(name)?parameters_.values.at(name):parameter.defaultValue;
        bool changed=false;
        if(type==PinType::Scalar)changed=UI::Prop::DragFloat(name.c_str(),value.data(),.01f,-1000000.0f,1000000.0f,"%.3f");
        if(type==PinType::Color)changed=UI::Prop::ColorEdit4(name.c_str(),value.data());
        if(type==PinType::Vector2)changed=UI::Prop::DragFloat2(name.c_str(),value.data(),.01f);
        if(type==PinType::Vector3)changed=UI::Prop::DragFloat3(name.c_str(),value.data(),.01f);
        if(changed){parameters_.values[name]=value;previewDirty_=true;}
        if(type==PinType::Texture) {
            std::string label="Default";if(parameters_.textures.contains(name))label=std::filesystem::path(std::u8string(parameters_.textures.at(name).begin(),parameters_.textures.at(name).end())).filename().string();
            ImGui::TextUnformatted(name.c_str());ImGui::Button(label.c_str(),{std::max(100.0f,ImGui::GetContentRegionAvail().x),26});
            if(ImGui::BeginDragDropTarget()) {
                if(const auto* payload=ImGui::AcceptDragDropPayload("ASSET_ID")) {std::string id(static_cast<const char*>(payload->Data),payload->DataSize);if(!id.empty() && id.back()==0)id.pop_back();const auto entry=library_?library_->FindById(id):std::optional<AssetLibrary::Entry>{};
                    if(entry && entry->category==AssetLibrary::Category::Texture){parameters_.textures[name]=entry->guid.empty()?"asset:"+entry->id:"guid:"+entry->guid;textures_.Clear();previewDirty_=true;}}
                ImGui::EndDragDropTarget();
            }
        }
        if(ImGui::SmallButton("Reset")){parameters_.values.erase(name);parameters_.textures.erase(name);previewDirty_=true;textures_.Clear();}ImGui::PopID();
    }
}
void MaterialGraphPanel::UpdatePreview()
{
    const auto revision=library_?library_->Revision():0;
    if(revision!=libraryRevision_){libraryRevision_=revision;textures_.Clear();previewDirty_=true;}
    const bool dependencyChanged=subgraphs_.Changed();
    if (dependencyChanged) { textures_.Clear(); previewDirty_=true; }
    if(!previewDirty_ && !forceCompile_ && SameEvaluationGraph(evaluated_,document_) && previewSettings_==renderedSettings_ && previewPin_==renderedPin_)return;
    // Coalesce continuous slider edits while keeping the final released value exact.
    if(ImGui::IsAnyItemActive() && ImGui::GetTime()-previewTime_<.1)return;
    previewTime_=ImGui::GetTime();
    const auto pin=previewSettings_.channel==PreviewChannel::SelectedOutput?previewPin_:0;
    const auto previousPin=renderedSettings_.channel==PreviewChannel::SelectedOutput?renderedPin_:0;
    const bool recompile = forceCompile_ || dependencyChanged || !SameEvaluationGraph(evaluated_,document_) || pin != previousPin || program_.Instructions().empty();
    evaluated_=document_;renderedSettings_=previewSettings_;renderedPin_=previewPin_;previewDirty_=false;
    if(previewSettings_.channel==PreviewChannel::SelectedOutput && !pin){compileError_="Select a numeric or surface output";return;}
    if (recompile) { CompileMaterial(program_,graphError_,pin); forceCompile_=false; }
    if (!graphError_.empty()) { compileError_=graphError_; return; }
    if (!program_.ValidateOverrides(parameters_, compileError_)) return;
    textures_.Configure(library_,path_.parent_path());
    textures_.BeginEvaluation();
    auto pixels=RenderMaterialPreview(program_,parameters_,[&](const auto& n,const auto& overridePath,float u,float v){return textures_.Sample(n,overridePath,u,v);},previewSettings_,256);
    if(!textures_.Error().empty()){compileError_=textures_.Error();return;}
    previewPixels_=std::move(pixels);
    if(provider_ && provider_->IsReady()) {
        const auto texture=provider_->UploadPreviewTexture(previewPixels_.data(),256,256,"Material Graph preview");
        if(texture.IsValid()){if(previewTexture_.IsValid())provider_->ReleasePreviewTexture(previewTexture_);previewTexture_=texture;}
    }
}
void MaterialGraphPanel::RenderPreview()
{
    if(!ImGui::CollapsingHeader("Material preview",ImGuiTreeNodeFlags_DefaultOpen))return;
    int channel=static_cast<int>(previewSettings_.channel);const char* channels[]={"Shaded","Base Color","Roughness","Metallic","Normal","AO","Height","Emissive","Selected output"};
    if(UI::Prop::Combo("Channel",&channel,channels,9))previewSettings_.channel=static_cast<PreviewChannel>(channel);
    UI::Prop::Checkbox("Sphere",&previewSettings_.sphere);
    UI::Prop::DragFloat("World size (m)",&previewSettings_.worldSize,.1f,.01f,10000.0f,"%.2f");
    UI::Prop::DragFloat("Height offset (m)",&previewSettings_.heightOffset,.1f,-10000.0f,10000.0f,"%.2f");
    if(previewSettings_.channel==PreviewChannel::SelectedOutput) {
        const auto* node=document_.FindNode(selected_);bool found=false;
        if(node) {
            const auto pins=NodePins(*node);
            for(size_t s=0;s<pins.size();++s)if(!pins[s].input && pins[s].type!=PinType::Texture){if(previewPin_/16!=selected_ || !document_.FindPin(previewPin_))previewPin_=PinId(selected_,s);found=true;break;}
            if(found && ImGui::BeginCombo("Output",document_.FindPin(previewPin_)->definition.name)) {
                for(size_t s=0;s<pins.size();++s)if(!pins[s].input && pins[s].type!=PinType::Texture)if(ImGui::Selectable(pins[s].name,previewPin_==PinId(selected_,s)))previewPin_=PinId(selected_,s);ImGui::EndCombo();
            }
        }
        if(!found){previewPin_=0;ImGui::TextDisabled("Select a node with a numeric/surface output");}
        if(const auto pin=document_.FindPin(previewPin_))previewSettings_.selectedType=pin->definition.type;
    }
    if(ImGui::SmallButton("Reload textures")){textures_.Clear();previewDirty_=true;}
    ImGui::SameLine(); if(ImGui::SmallButton("Reload subgraphs")){subgraphs_.Clear();forceCompile_=true;previewDirty_=true;}
    UpdatePreview();
    if(!previewPixels_.empty()) {
        const auto at=ImGui::GetCursorScreenPos();const float size=std::clamp(ImGui::GetContentRegionAvail().x,96.0f,256.0f);
        const auto texture=provider_?provider_->GetPreviewTexture(previewTexture_):nullptr;
        if(texture)ImGui::Image(reinterpret_cast<ImTextureID>(texture),{size,size});
        else { // Headless/unsupported backend fallback, same evaluated pixels.
            const float cell=size/32;auto* draw=ImGui::GetWindowDrawList();
            for(int y=0;y<32;++y)for(int x=0;x<32;++x){const size_t p=(static_cast<size_t>(y*8+4)*256+x*8+4)*4;draw->AddRectFilled({at.x+x*cell,at.y+y*cell},{at.x+(x+1)*cell+.1f,at.y+(y+1)*cell+.1f},IM_COL32(previewPixels_[p],previewPixels_[p+1],previewPixels_[p+2],255));}ImGui::Dummy({size,size});
        }
    }
    if(!compileError_.empty())ImGui::TextWrapped("%s\nKeeping the last valid preview.",compileError_.c_str());
    if(program_.NeedsSurfaceData()) {
        ImGui::TextWrapped("Missing Geometry Bake: using explicit preview test inputs, not mesh-baked data.");
        UI::Prop::SliderFloat("Preview AO",&previewSettings_.surfaceData.ao,0.0f,1.0f);
        UI::Prop::SliderFloat("Preview curvature",&previewSettings_.surfaceData.curvature,0.0f,1.0f);
        UI::Prop::SliderFloat("Preview cavity",&previewSettings_.surfaceData.cavity,0.0f,1.0f);
    }
    if(ImGui::CollapsingHeader("Bake UV material")) {
        UI::Prop::InputText("Material name",bakeName_,sizeof(bakeName_));const char* sizes[]={"256","512","1024"};UI::Prop::Combo("Resolution",&bakeResolution_,sizes,3);
        ImGui::BeginDisabled(!library_);if(ImGui::Button("Bake to Asset Browser"))BakeMaterial();ImGui::EndDisabled();
        ImGui::TextWrapped("Creates a material and textures in Materials/GraphBakes. UV graphs can be assigned through the existing material workflow. World-dependent rules require terrain or per-object baking.");
    }
    if(!status_.empty())ImGui::TextWrapped("%s",status_.c_str());
}
void MaterialGraphPanel::BakeMaterial()
{
    if(!library_)return;AssetLibrary::Entry result;std::string error;
    if(PublishBakedMaterial(*library_,document_,parameters_,256U<<bakeResolution_,bakeName_,result,error,path_.parent_path()))status_="Baked material: "+result.displayName+" in Materials/GraphBakes";
    else status_=error;
}
void MaterialGraphPanel::ExportShader()
{
    MaterialGraphProgram program;if(!CompileMaterial(program,status_))return;
    wchar_t file[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);dialog.lpstrFile=file;dialog.nMaxFile=static_cast<DWORD>(std::size(file));
    dialog.lpstrFilter=L"HLSL shader (*.hlsl)\0*.hlsl\0";dialog.lpstrDefExt=L"hlsl";dialog.Flags=OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT;
    if(!GetSaveFileNameW(&dialog))return;
    const auto path=std::filesystem::path(file);auto temporary=path;temporary+=L".tmp";std::ofstream output(temporary,std::ios::binary);output<<program.GenerateHlsl();output.close();
    if(!output || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))status_="Cannot save generated shader";
    else status_="Generated HLSL exported; bind textures and parameter constants as declared.";
}

}
