#include "MaterialGraphPanel.h"
#include "MaterialGraphIO.h"
#include "asset/MaterialAssetManager.h"
#include "MaterialInstance.h"
#include "GeometryAtlasAssets.h"
#include "asset/AssetDatabase.h"
#include "platform/file_dialog.h"
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
#include <utility>

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
std::filesystem::path BrowseFile(bool save, std::vector<platform::FileDialogFilter> filters, const char* defaultExtension = "",
    const std::filesystem::path& current = {}, const char* title = "")
{
    platform::FileDialogOptions options;
    options.save = save; options.title = title; options.filters = std::move(filters);
    options.defaultExtension = defaultExtension; options.initialPath = current;
    options.ownerWindow = ImGui::GetMainViewport()->PlatformHandleRaw;
    return platform::ShowFileDialog(options).value_or(std::filesystem::path{});
}
std::filesystem::path ChooseGraphFile(bool save, const std::filesystem::path& current, bool instance = false, bool subgraph = false, bool generator = false)
{
    std::vector<platform::FileDialogFilter> filters;
    if (generator) filters = {{"Smart Generator (*.ixsmartgenerator)", "*.ixsmartgenerator"}};
    else if (instance) filters = {{"Material Instance (*.ixmaterialinstance)", "*.ixmaterialinstance"}, {"All Files", "*.*"}};
    else if (subgraph && save) filters = {{"Material Subgraph (*.ixmaterialsubgraph)", "*.ixmaterialsubgraph"}};
    else filters = {{"Material Documents", "*.materialgraph;*.ixsmartmaterial;*.ixmaterialsubgraph;*.ixsmartgenerator"}, {"All Files", "*.*"}};
    return BrowseFile(save, std::move(filters), generator ? "ixsmartgenerator" : (instance ? "ixmaterialinstance" : (subgraph ? "ixmaterialsubgraph" : "materialgraph")),
        current, save ? "Save Material Graph" : "Open Material Graph");
}
}
MaterialGraphPanel::MaterialGraphPanel() = default;
MaterialGraphPanel::~MaterialGraphPanel() { ResetContext(); if(provider_ && previewTexture_.IsValid())provider_->ReleasePreviewTexture(previewTexture_); }
void MaterialGraphPanel::SetServices(AssetLibrary* library,ixeditor::graphics::IEditorTextureProvider* provider) {
    if(provider_!=provider) {if(provider_ && previewTexture_.IsValid())provider_->ReleasePreviewTexture(previewTexture_);previewTexture_={};provider_=provider;previewDirty_=true;}
    if(library_!=library){if(applyRequest_){geometryJob_.Cancel();uvAtlasJob_.Cancel();}DiscardApplyRequest();materialAssignment_.reset();UnlinkLiveMaterial();library_=library;textures_.Clear();subgraphs_.Clear();forceCompile_=true;parameters_={};previewDirty_=true;}
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
    dragBefore_.reset(); propertyBefore_.reset();if(!document_.FindNode(selected_))selected_=0;
    if(previewSettings_.channel==PreviewChannel::SelectedOutput){
        const auto pin=document_.FindPin(previewPin_);
        if(pin && !pin->definition.input && pin->definition.type!=PinType::Texture)selected_=pin->node;
        else {previewSettings_.channel=PreviewChannel::Shaded;previewPin_=0;}
    }
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
void MaterialGraphPanel::RequestOpen(const std::filesystem::path& path)
{
    if (path.empty()) return;
    pendingPath_ = path;
    RequestReplace(true);
}
void MaterialGraphPanel::RequestReplace(bool openFile)
{
    pendingOpen_ = openFile;
    if (!openFile) pendingPath_.clear();
    if (document_ != saved_) { requestUnsaved_ = true; return; }
    if (openFile) OpenDialog();
    else
    {
        const bool generator=pendingPreset_>=10 && pendingPreset_<10+static_cast<int>(SmartGeneratorPresets().size());
        SmartGenerator preset;
        if(generator && !SmartGenerator::Create(SmartGeneratorPresets()[pendingPreset_-10].kind,preset,status_))return;
        generatorMode_=generator;generator_=std::move(preset);subgraphMode_=generator || pendingPreset_==4; subgraphVersion_=1;
        document_ = generator?generator_.definition.graph:(subgraphMode_?MaterialSubgraph::Example().graph:(pendingPreset_==1?MaterialGraphDocument::WeatheredMetal():(pendingPreset_==2?MaterialGraphDocument::MossyRock():(pendingPreset_==3?MaterialGraphDocument::WeatheredStone():MaterialGraphDocument::Example())))); pendingPreset_=0; saved_ = MaterialGraphDocument::Example(); path_.clear();
        undo_.clear(); redo_.clear(); selected_ = 0; parameters_={}; previewDirty_=true; ResetContext(); fitPending_ = true; status_.clear();
        UnlinkLiveMaterial();
        subgraphs_.Clear(); forceCompile_=true;
        previewPin_=0;
        if (subgraphMode_) {for(const auto& node:document_.nodes)if(node.kind==NodeKind::SubgraphOutput){selected_=node.id;previewPin_=PinId(node.id,1);previewSettings_.channel=PreviewChannel::SelectedOutput;break;}}
        else if(previewSettings_.channel==PreviewChannel::SelectedOutput)previewSettings_.channel=PreviewChannel::Shaded;
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
        if (ImGui::Button("Cancel", {85, 0})) { pendingPath_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}
void MaterialGraphPanel::RenderToolbar()
{
    if (ImGui::Button("New")) {pendingPreset_=0;RequestReplace(false);}
    ImGui::SameLine(); if (ImGui::Button("Open...")) { pendingPath_.clear(); RequestReplace(true); }
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
        if(ImGui::BeginMenu("Smart Generators")){int index=0;for(const auto& preset:SmartGeneratorPresets()){if(ImGui::MenuItem(preset.name)){pendingPreset_=10+index;RequestReplace(false);}++index;}ImGui::EndMenu();}
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
    PollGeometryBake();
    UpdateLiveMaterial();
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
    RenderSmartGenerators();RenderInstanceParameters();RenderPreview();
}

bool MaterialGraphPanel::Load(const std::filesystem::path& path)
{
    std::string text;
    if (!LoadMaterialText(path, text, status_)) return false;
    MaterialGraphDocument draft;
    const auto type=detectAssetType(path);const bool generator=type==AssetType::SmartGenerator;
    const bool subgraph = generator || type==AssetType::MaterialSubgraph;
    SmartGenerator generatorAsset;
    std::uint32_t interfaceVersion=1;
    if (subgraph) {
        MaterialSubgraph asset;
        if(generator){if(!generatorAsset.FromJson(text,status_))return false;asset=generatorAsset.definition;}
        else if (!asset.FromJson(text,status_)) return false;
        draft=std::move(asset.graph); interfaceVersion=asset.interfaceVersion;
    } else if (!draft.FromJson(text, status_)) return false;
    document_ = std::move(draft); saved_ = document_; path_ = path; undo_.clear(); redo_.clear();
    UnlinkLiveMaterial();
    dragBefore_.reset(); propertyBefore_.reset(); selected_ = 0; ResetContext(); fitPending_ = true;
    subgraphMode_=subgraph; subgraphVersion_=interfaceVersion; subgraphs_.Clear(); forceCompile_=true;
    generatorMode_=generator;generator_=std::move(generatorAsset);
    previewPin_=0;
    if (subgraph) {for (const auto& node:document_.nodes) if (node.kind==NodeKind::SubgraphOutput) { selected_=node.id; previewPin_=PinId(node.id,1); previewSettings_.channel=PreviewChannel::SelectedOutput; break; }}
    else if(previewSettings_.channel==PreviewChannel::SelectedOutput)previewSettings_.channel=PreviewChannel::Shaded;
    parameters_={};previewDirty_=true;textures_.Clear(); textures_.Configure(library_,path_.parent_path()); status_ = subgraph?"Subgraph opened":"Graph opened"; return true;
}
bool MaterialGraphPanel::Save(const std::filesystem::path& path)
{
    const auto type=detectAssetType(path);const bool generator=type==AssetType::SmartGenerator;
    const bool subgraph = generator || type==AssetType::MaterialSubgraph;
    if(generatorMode_ && !generator){status_="Save generator assets with the .ixsmartgenerator extension to preserve metadata";return false;}
    if (subgraphMode_ && !subgraph) { status_="Save subgraph assets with .ixmaterialsubgraph or .ixsmartgenerator"; return false; }
    if (!subgraph && type!=AssetType::MaterialGraph) { status_="Use .materialgraph, .ixsmartmaterial, .ixmaterialsubgraph or .ixsmartgenerator"; return false; }
    auto text = document_.ToJson();
    MaterialGraphDocument checked;
    if (!checked.FromJson(text, status_)) return false;
    if (subgraph) {
        MaterialSubgraph asset{subgraphVersion_,document_}; std::vector<MaterialGraphPort> ports;
        if (!asset.GetInterface(ports,status_)) return false; text=asset.ToJson();
        if(generator){auto preset=generator_;preset.definition=std::move(asset);if(!preset.Validate(status_))return false;text=preset.ToJson();}
    }
    if (!SaveMaterialTextAtomic(path, text, status_)) return false;
    auto& database=AssetDatabase::Instance(); const auto guid=database.getOrCreateGuid(path);
    std::vector<Guid> dependencies;
    for (const auto& node:document_.nodes) if (const auto dependency=Guid::fromString(node.assetGuid))
        if (std::find(dependencies.begin(),dependencies.end(),*dependency)==dependencies.end()) dependencies.push_back(*dependency);
    if (!IsMaterialAssetGuid(guid.toString()) || !database.writeDependencies(path,dependencies)) { status_="Definition saved, but asset identity/dependency metadata failed"; return false; }
    path_ = path; saved_ = document_; subgraphMode_=subgraph;generatorMode_=generator; textures_.Configure(library_,path_.parent_path()); previewDirty_=true; status_ = generator?"Smart Generator saved":(subgraph?"Subgraph saved":"Graph saved");
    RefreshSavedAsset(); return true;
}
void MaterialGraphPanel::RefreshSavedAsset()
{
    if (!library_) return;
    std::string error;
    if (!library_->Refresh(error)) status_ += "; Asset Browser refresh failed: " + error;
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
    if(geometryPreview_) {
        geometry_.Refresh();
        if(!geometry_.Bake() || geometry_.BakeGuid().empty()){status_="Save the valid Geometry Bake cache before saving a mesh-bound instance";return false;}
        instance.geometryBakeGuid=geometry_.BakeGuid();
    }
    if (!instance.Validate(program,status_)) return false;
    if (!SaveMaterialTextAtomic(path,instance.ToJson(),status_)) return false;
    auto& database=AssetDatabase::Instance();database.getOrCreateGuid(path);
    std::vector<Guid> dependencies;
    if (const auto guid=Guid::fromString(instance.definitionGuid)) dependencies.push_back(*guid);
    if (const auto guid=Guid::fromString(instance.geometryBakeGuid)) dependencies.push_back(*guid);
    for (const auto& [name,resource]:instance.overrides.textures) {
        (void)name;
        if (resource.starts_with("guid:")) if (const auto guid=Guid::fromString(resource.substr(5)))
            if (std::find(dependencies.begin(),dependencies.end(),*guid)==dependencies.end()) dependencies.push_back(*guid);
    }
    if (!database.writeDependencies(path,dependencies)) { status_="Instance saved, but asset dependency metadata could not be written"; return false; }
    status_="Material instance saved"; RefreshSavedAsset(); return true;
}
bool MaterialGraphPanel::LoadInstance(const std::filesystem::path& path)
{
    if (path_.empty()) { status_="Open the material definition before loading its instance"; return false; }
    std::string text; MaterialInstance instance; MaterialGraphProgram program;
    if (!LoadMaterialText(path,text,status_) || !instance.FromJson(text,status_) || !CompileMaterial(program,status_)) return false;
    if (instance.definitionGuid!=AssetDatabase::Instance().getOrCreateGuid(path_).toString()) { status_="This instance references a different material definition"; return false; }
    if (!instance.Validate(program,status_)) return false;
    if(geometryJob_.Running()){status_="Finish or cancel the geometry bake before loading an instance";return false;}
    GraphGeometryCache geometry;
    if(!instance.geometryBakeGuid.empty() && !geometry.Bind(instance.geometryBakeGuid,status_))return false;
    geometry_=std::move(geometry);geometryPreview_=!instance.geometryBakeGuid.empty();
    if(geometry_.Bake())geometrySettings_=geometry_.Bake()->settings;
    parameters_=instance.overrides; textures_.Clear(); previewDirty_=true; status_="Material instance loaded"; return true;
}
void MaterialGraphPanel::SaveDialog(bool saveAs)
{
    const auto destination = saveAs || path_.empty() ? ChooseGraphFile(true, path_,false,subgraphMode_,generatorMode_) : path_;
    if (!destination.empty()) Save(destination);
}
void MaterialGraphPanel::OpenDialog()
{
    const auto source = pendingPath_.empty() ? ChooseGraphFile(false, path_) : std::exchange(pendingPath_, {});
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
        const auto chosen=BrowseFile(false,{{"Textures","*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.exr"},{"All Files","*.*"}});
        if(!chosen.empty()) {
            const auto before=document_;
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
            const auto file=BrowseFile(false,{{"Subgraph or Smart Generator","*.ixmaterialsubgraph;*.ixsmartgenerator"}});
            if (file.empty()) return;
            guid=AssetDatabase::Instance().getOrCreateGuid(file).toString(); resource=file.filename().string();
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
        ImGui::SameLine();if(ImGui::SmallButton("Add generator...")){const auto path=ChooseGraphFile(false,{},false,false,true);if(!path.empty()){AddGeneratorAsset(path,layer.id);ImGui::PopID();return;}}
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
bool MaterialGraphPanel::AddGeneratorAsset(const std::filesystem::path& path,std::uint32_t layer) {
    SmartGenerator generator;if(!LoadSmartGeneratorAsset(path,generator,status_))return false;
    const auto guid=AssetDatabase::Instance().getOrCreateGuid(path).toString();const auto before=document_;
    const auto call=AddSmartGenerator(document_,guid,generator,layer,status_,spawn_);if(!call)return false;
    auto& node=*document_.FindNode(call);const auto utf8=path.filename().generic_u8string();node.resource.assign(utf8.begin(),utf8.end());
    const auto output=std::find_if(node.ports.begin(),node.ports.end(),[](const auto& p){return !p.input;});
    const auto pin=PinId(call,static_cast<size_t>(output-node.ports.begin()));
    RecordUndo(before);ResetContext();selected_=call;previewPin_=pin;previewSettings_.channel=PreviewChannel::SelectedOutput;
    subgraphs_.Clear();forceCompile_=previewDirty_=true;fitPending_=true;requestInspector_=true;status_="Smart Generator added";return true;
}
void MaterialGraphPanel::RenderSmartGenerators() {
    if(!ImGui::CollapsingHeader("Smart Generators"))return;
    if(generatorMode_){ImGui::TextUnformatted(generator_.name.c_str());ImGui::TextWrapped("%s",generator_.description.c_str());ImGui::TextWrapped("Edit the canonical graph and save its .ixsmartgenerator preset. Mask is the scalar output; parameters become scoped instance overrides in callers.");}
    if(ImGui::Button("Add generator preset...")){const auto path=ChooseGraphFile(false,{},false,false,true);if(!path.empty())AddGeneratorAsset(path);}
    ImGui::SameLine();if(ImGui::Button("Create generator preset"))ImGui::OpenPopup("Create smart generator");
    if(ImGui::BeginPopup("Create smart generator")){int index=0;for(const auto& preset:SmartGeneratorPresets()){if(ImGui::MenuItem(preset.name)){pendingPreset_=10+index;RequestReplace(false);}++index;}ImGui::EndPopup();}
    for(const auto& node:document_.nodes)if(node.kind==NodeKind::Subgraph){
        const auto guid=Guid::fromString(node.assetGuid);const auto path=guid?AssetDatabase::Instance().resolveGuid(*guid):std::optional<std::filesystem::path>{};
        if(!path || detectAssetType(*path)!=AssetType::SmartGenerator)continue;
        ImGui::PushID(static_cast<int>(node.id));if(ImGui::Selectable(node.name.c_str(),selected_==node.id)){
            selected_=node.id;const auto output=std::find_if(node.ports.begin(),node.ports.end(),[](const auto& p){return !p.input;});
            if(output!=node.ports.end()){previewPin_=PinId(node.id,static_cast<size_t>(output-node.ports.begin()));previewSettings_.channel=PreviewChannel::SelectedOutput;}
        }ImGui::PopID();
    }
    ImGui::TextWrapped("Use Add generator on a Layer Stack row to assign its mask in one undoable edit. NoiseMask modulates seeded breakup; geometry inputs come from the same preview/bake context. A valid mesh bake supplies real AO, curvature and cavity.");
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
    PollGeometryBake();
    UpdateLiveMaterial();
    if(geometry_.Refresh())previewDirty_=true;
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
    if(previewSettings_.channel>=PreviewChannel::GeometryAO && !geometryPreview_){compileError_="Geometry map channels require Mesh UV atlas preview and a valid Geometry Bake";return;}
    if(geometryPreview_ && !geometry_.Bake()){compileError_=geometry_.Error().empty()?"Missing Geometry Bake: bake the selected mesh to render its UV atlas":geometry_.Error();return;}
    textures_.Configure(library_,path_.parent_path());
    textures_.BeginEvaluation();
    auto pixels=RenderMaterialPreview(program_,parameters_,[&](const auto& n,const auto& overridePath,float u,float v){return textures_.Sample(n,overridePath,u,v);},previewSettings_,256,geometryPreview_?geometry_.Bake():nullptr);
    if(!textures_.Error().empty()){compileError_=textures_.Error();return;}
    previewPixels_=std::move(pixels);
    if(provider_ && provider_->IsReady()) {
        const auto texture=provider_->UploadPreviewTexture(previewPixels_.data(),256,256,"Material Graph preview");
        if(texture.IsValid()){if(previewTexture_.IsValid())provider_->ReleasePreviewTexture(previewTexture_);previewTexture_=texture;}
    }
}
void MaterialGraphPanel::SetApplyTarget(const MeshRendererEditorState& mesh, const std::filesystem::path& path, bool allowed)
{
    applyTarget_ = mesh; applyModelPath_ = path; applyAllowed_ = allowed;
}
bool MaterialGraphPanel::BakeAndApply()
{
    if (!library_ || !applyAllowed_ || !applyTarget_.selected || applyTarget_.skinned || applyModelPath_.empty())
    { status_ = "Select an imported static model in Edit mode before Bake & Apply."; return false; }
    if (subgraphMode_) { status_ = "Open a full smart material with a PBR Output; generator recipes supply layer masks."; return false; }
    if (geometryJob_.Running() || uvAtlasJob_.Running() || applyRequest_ || materialAssignment_)
    { status_ = "Finish or cancel the current Bake & Apply first."; return false; }
    // A cancelled atlas job can finish before it is polled; its result must not reach this request.
    { GeometryMesh stale; std::string staleError; uvAtlasJob_.TakeResult(stale,staleError); }
    MaterialGraphProgram checked;
    if (!CompileMaterial(checked,status_) || !checked.ValidateOverrides(parameters_,status_)) return false;
    GraphGeometryCache selected;std::uint64_t sourceHash=0;
    if (!selected.SelectMaterialSlot(applyModelPath_,applyTarget_.selectedMaterialSlot,status_,&sourceHash)) return false;
    if (!sourceHash) { status_ = "Static source model is unavailable or changed"; return false; }
    const auto settings = SelectedMeshBakeSettings(applyTarget_,128U << applyResolution_);
    if (!geometryJob_.Start(*selected.Mesh(),settings)) { status_ = "Geometry bake could not start."; return false; }
    ApplyRequest request;
    request.target = CaptureSmartMaterialTarget(applyTarget_);
    request.target.assetRoot = library_->LibraryRoot().lexically_normal().generic_string();
    request.target.sourceModelGuid=selected.MeshGuid();request.target.sourceModelHash=sourceHash;request.submeshes=selected.MeshIndices();
    request.document = document_; request.parameters = parameters_; request.graphFolder = path_.parent_path();
    request.name = applyTarget_.name.empty() ? "SmartMaterial" : applyTarget_.name + " SmartMaterial";
    applyRequest_ = std::move(request); geometry_ = std::move(selected); geometrySettings_ = settings;
    status_ = "Bake & Apply: baking model geometry..."; return true;
}
std::optional<SmartMaterialAssignment> MaterialGraphPanel::TakeMaterialAssignment()
{
    return std::exchange(materialAssignment_,std::nullopt);
}
void MaterialGraphPanel::CancelBakeAndApply()
{
    if (applyRequest_) { geometryJob_.Cancel(); uvAtlasJob_.Cancel(); DiscardApplyRequest(); status_="Bake & Apply cancelled; model material kept."; }
}
void MaterialGraphPanel::DiscardApplyRequest()
{
    if (!applyRequest_) return;
    const auto model=applyRequest_->target.replacementModelGuid,source=applyRequest_->target.sourceModelGuid;
    const auto slot=applyRequest_->target.slot;
    applyRequest_.reset();
    if (model.empty() || !library_) return;
    DiscardGeometryAtlasAsset(*library_,model);
    if (geometry_.MeshGuid()!=model) return;
    // The preview was following the removed model; return it to the source it was made from.
    std::string ignored;const auto guid=Guid::fromString(source);
    const auto path=guid?AssetDatabase::Instance().resolveGuid(*guid):std::nullopt;
    if (!path || !geometry_.SelectMaterialSlot(*path,slot,ignored)) geometry_.Clear();
    previewDirty_=true;
}
void MaterialGraphPanel::UnlinkLiveMaterial()
{
    std::string bytes,error;
    if(live_ && live_->unsaved && !(EncodeGraphShaderPackage(live_->data,bytes,error) && SaveMaterialTextAtomic(live_->package,bytes,error)))
        status_="Cannot save live material values: "+error;
    live_.reset();liveJobGuid_.clear(); // a compilation still running belongs to no material now
}
void MaterialGraphPanel::LinkLiveMaterial(const AssetLibrary::Entry& material, const MaterialGraphDocument& document,
                                          const ParameterOverrides& parameters, const std::filesystem::path& graphFolder)
{
    UnlinkLiveMaterial();
    const auto guid=Guid::fromString(material.guid);
    const auto* asset=guid?MaterialAssetManager::Instance().getOrLoad(*guid):nullptr;
    if(!asset || asset->graphShader.empty())return;
    LiveMaterial link;
    link.guid=material.guid;link.name=material.displayName;link.document=document;link.textures=parameters.textures;link.graphFolder=graphFolder;
    link.package=asset->path.parent_path()/std::filesystem::path(std::u8string(asset->graphShader.begin(),asset->graphShader.end()));
    std::string bytes,error;MaterialGraphProgram program;
    subgraphs_.BeginCompilation();const bool compiled=program.Compile(document,error,0,subgraphs_.Resolver());subgraphs_.FinishCompilation();
    if(!compiled || !LoadMaterialText(link.package,bytes,error) || !DecodeGraphShaderPackage(bytes,link.data,error))return;
    link.parameters=program.Layout().parameterBufferBytes/sizeof(Float4);
    if(link.parameters>link.data.uniforms.size())return;
    for(const auto& texture:link.data.textures)if(texture.binding==kGraphGeometryBinding)link.geometryMap=texture.guid;
    link.checkedDocument=document_;link.checkedParameters=parameters_;link.recheck=true;
    GraphLiveValues::Instance().Clear(link.guid);
    live_=std::move(link);
}
namespace {
// The same graph apart from numeric parameter values: its compiled shader still fits.
bool SameShaderStructure(MaterialGraphDocument a,MaterialGraphDocument b)
{
    for(auto* document:{&a,&b})for(auto& node:document->nodes)
        if(node.kind==NodeKind::ScalarParameter || node.kind==NodeKind::ColorParameter || node.kind==NodeKind::Vector2Parameter || node.kind==NodeKind::Vector3Parameter)node.value={};
    return SameEvaluationGraph(a,b);
}
}
void MaterialGraphPanel::UpdateLiveMaterial(bool persistNow)
{
    if(!live_)return;
    auto& link=*live_;
    const auto now=std::chrono::steady_clock::now();
    GraphShaderPackage recompiled;std::string compileError;
    if(liveJob_.TakeResult(recompiled,compileError) && liveJobGuid_==link.guid) {
        std::string bytes;
        if(!compileError.empty() || recompiled.opaqueSpirv.empty()) link.notice=compileError.empty()?"the shader compiler gave no output":compileError;
        else if(!EncodeGraphShaderPackage(recompiled,bytes,compileError) || !SaveMaterialTextAtomic(link.package,bytes,compileError)) link.notice="cannot save the shader: "+compileError;
        else {
            // The renderer reads the package again when the material revision moves; its uniforms hold
            // the values the graph was compiled with, so earlier live values are dropped.
            link.document=std::move(link.compiling);link.textures=link.compilingParameters.textures;
            link.data=std::move(recompiled);link.parameters=link.compilingSlots;link.unsaved=false;link.recheck=true;link.notice.clear();
            link.failed={};link.failedParameters={};
            GraphLiveValues::Instance().Clear(link.guid);
            MaterialAssetManager::Instance().markChanged();
            status_="Live shader updated: "+link.name;
        }
        if(!link.notice.empty()){link.failed=link.compiling;link.failedParameters=link.compilingParameters;status_=link.name+": "+link.notice+". The scene keeps the previous shader.";}
        link.compiling={};link.compilingParameters={};
    }
    if(link.recheck || link.checkedDocument!=document_ || link.checkedParameters!=parameters_) {
        link.recheck=false;
        link.checkedDocument=document_;link.checkedParameters=parameters_;
        link.structureChanged=!SameShaderStructure(document_,link.document) || parameters_.textures!=link.textures;
        if(link.structureChanged)link.edited=now; // each edit restarts the pause before compiling
        MaterialGraphProgram program;std::string error;std::vector<Float4> parameters;
        bool compiled=false;
        if(!link.structureChanged){subgraphs_.BeginCompilation();compiled=program.Compile(document_,error,0,subgraphs_.Resolver());subgraphs_.FinishCompilation();}
        if(compiled && program.BuildParameterBuffer(parameters_,parameters,error) &&
           parameters.size()==link.parameters && !std::equal(parameters.begin(),parameters.end(),link.data.uniforms.begin())) {
            std::copy(parameters.begin(),parameters.end(),link.data.uniforms.begin());
            GraphLiveValues::Instance().Set(link.guid,link.data.uniforms);
            link.unsaved=true;link.changed=std::chrono::steady_clock::now();
        }
    }
    // Saved once the values settle, so a dragged slider writes the file once, not every frame.
    if(link.unsaved && (persistNow || now-link.changed>std::chrono::milliseconds(400))) {
        std::string bytes,error;
        if(EncodeGraphShaderPackage(link.data,bytes,error) && SaveMaterialTextAtomic(link.package,bytes,error))link.unsaved=false;
        else status_="Cannot save live material values: "+error;
    }
    // A changed graph is compiled once the edits pause; a graph that failed waits for the next edit.
    const bool failedAgain=document_==link.failed && parameters_==link.failedParameters;
    if(!link.structureChanged || failedAgain || liveJob_.Running() || !library_ ||
       (!persistNow && now-link.edited<std::chrono::milliseconds(500)))return;
    MaterialGraphDocument source;MaterialGraphProgram program;GraphShaderPackage package;std::string notice;
    if(!EffectiveGraph(*library_,document_,parameters_,source,notice) ||
       !PrepareGraphShader(*library_,source,nullptr,link.geometryMap,link.graphFolder,link.package.parent_path(),program,package,notice)) {
        static constexpr std::string_view prefix="Runtime shader not built: ";
        if(notice.starts_with(prefix))notice.erase(0,prefix.size());
        link.notice=notice;link.failed=document_;link.failedParameters=parameters_;
        status_=link.name+": "+notice+". The scene keeps the previous shader.";return;
    }
    link.compilingSlots=program.Layout().parameterBufferBytes/sizeof(Float4);
    if(!liveJob_.Start(std::move(program),std::move(package)))return;
    liveJobGuid_=link.guid;link.compiling=document_;link.compilingParameters=parameters_;link.notice.clear();
}
void MaterialGraphPanel::RenderApplyMaterial()
{
    ImGui::SeparatorText("Apply to model");
    if(live_) {
        if(liveJob_.Running() && liveJobGuid_==live_->guid)ImGui::TextWrapped("Live in the scene: %s. Compiling the changed graph...",live_->name.c_str());
        else if(live_->structureChanged && !live_->notice.empty())
            ImGui::TextColored(ImVec4(1,.75f,.3f,1),"%s: the changed graph cannot run live (%s). The scene keeps the previous shader.",live_->name.c_str(),live_->notice.c_str());
        else if(live_->structureChanged)ImGui::TextWrapped("Live in the scene: %s. The changed graph compiles when the edits pause.",live_->name.c_str());
        else ImGui::TextWrapped("Live in the scene: %s. Parameter and graph changes show in the scene.",live_->name.c_str());
    }
    if (applyTarget_.selected) ImGui::Text("%s / Material Slot %u",applyTarget_.name.c_str(),applyTarget_.selectedMaterialSlot+1);
    else ImGui::TextDisabled("Select a model in Hierarchy.");
    if (applyTarget_.selected && (applyTarget_.skinned || applyModelPath_.empty())) ImGui::TextDisabled("An imported static model is required.");
    const char* sizes[] = {"128","256","512"};
    UI::Prop::Combo("Apply resolution",&applyResolution_,sizes,3);
    ImGui::BeginDisabled(!applyAllowed_ || !applyTarget_.selected || applyTarget_.skinned || applyModelPath_.empty() || subgraphMode_ || geometryJob_.Running() || uvAtlasJob_.Running());
    if (ImGui::Button("Bake & Apply to selected model")) BakeAndApply();
    ImGui::EndDisabled();
    if (subgraphMode_) ImGui::TextWrapped("Generator recipes are layer masks. Open a full smart material to apply its appearance.");
    if (applyRequest_) {
        if(uvAtlasJob_.Running())ImGui::TextUnformatted("Preparing automatic UV atlas...");
        ImGui::ProgressBar(uvAtlasJob_.Running()?uvAtlasJob_.Progress():geometryJob_.Progress());
        if (ImGui::Button("Cancel Bake & Apply")) CancelBakeAndApply();
    }
    ImGui::TextWrapped("Bakes geometry and textures, saves the material, and assigns this slot automatically. Moving the model later requires rebaking world-dependent effects.");
}
void MaterialGraphPanel::RenderPreview()
{
    if(!ImGui::CollapsingHeader("Material preview",ImGuiTreeNodeFlags_DefaultOpen))return;
    int channel=static_cast<int>(previewSettings_.channel);const char* channels[]={"Shaded","Base Color","Roughness","Metallic","Normal","AO","Height","Emissive","Selected output","Geometry AO","Curvature","Cavity","Thickness / ray distance","Object Normal","World Normal","Object Position (bounds)","World Position (bounds)"};
    if(UI::Prop::Combo("Channel",&channel,channels,17)){previewSettings_.channel=static_cast<PreviewChannel>(channel);if(previewSettings_.channel>=PreviewChannel::GeometryAO)geometryPreview_=true;}
    RenderApplyMaterial();
    RenderGeometryBake();
    // Where the material is evaluated: an analytic sphere or plane, or the selected mesh's UV atlas
    // (which Bake & Apply and a geometry bake switch to).
    int shape=geometryPreview_?2:(previewSettings_.sphere?0:1);const char* shapes[]={"Sphere","Plane","Mesh UV atlas"};
    if(UI::Prop::Combo("Preview shape",&shape,shapes,geometryPreview_ || !geometry_.MeshGuid().empty()?3:2)) {
        geometryPreview_=shape==2;if(shape<2)previewSettings_.sphere=shape==0;previewDirty_=true;
        if(!geometryPreview_ && previewSettings_.channel>=PreviewChannel::GeometryAO)previewSettings_.channel=PreviewChannel::Shaded;
    }
    ImGui::BeginDisabled(geometryPreview_);
    UI::Prop::DragFloat("World size (m)",&previewSettings_.worldSize,.1f,.01f,10000.0f,"%.2f");
    UI::Prop::DragFloat("Height offset (m)",&previewSettings_.heightOffset,.1f,-10000.0f,10000.0f,"%.2f");
    ImGui::EndDisabled();
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
    if(geometryPreview_ && !previewPixels_.empty())
        ImGui::TextWrapped("Mesh UV atlas of %s: the material laid out flat as the model's UVs map it, not a 3D view.",geometry_.Name().empty()?"the baked mesh":geometry_.Name().c_str());
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
    if(program_.NeedsSurfaceData() && !geometryPreview_) {
        ImGui::TextWrapped(geometry_.Bake()?"Analytic preview uses explicit test inputs. Enable Mesh UV atlas preview to use the baked geometry.":"Missing Geometry Bake: using explicit preview test inputs, not mesh-baked data.");
        UI::Prop::SliderFloat("Preview AO",&previewSettings_.surfaceData.ao,0.0f,1.0f);
        UI::Prop::SliderFloat("Preview curvature",&previewSettings_.surfaceData.curvature,0.0f,1.0f);
        UI::Prop::SliderFloat("Preview cavity",&previewSettings_.surfaceData.cavity,0.0f,1.0f);
    }
    if(ImGui::CollapsingHeader("Bake UV material")) {
        UI::Prop::InputText("Material name",bakeName_,sizeof(bakeName_));const char* sizes[]={"256","512","1024"};UI::Prop::Combo("Resolution",&bakeResolution_,sizes,3);
        ImGui::BeginDisabled(!library_);if(ImGui::Button("Bake to Asset Browser"))BakeMaterial();ImGui::EndDisabled();
        ImGui::TextWrapped(geometryPreview_?"Creates textures for this mesh UV atlas and baked world transform. Reuse requires matching geometry, UVs and placement. Resolution follows Geometry Bake.":"Creates a material and textures in Materials/GraphBakes. World-dependent rules require per-object geometry baking.");
    }
    if(!status_.empty())ImGui::TextWrapped("%s",status_.c_str());
}
void MaterialGraphPanel::BakeMaterial()
{
    if(!library_)return;AssetLibrary::Entry result;std::string error;
    geometry_.Refresh();
    if(geometryPreview_ && (!geometry_.Bake() || geometry_.BakeGuid().empty())){status_="Save a valid Geometry Bake cache before publishing a mesh material";return;}
    const auto* geometry=geometryPreview_?geometry_.Bake():nullptr;
    std::string runtimeNotice;
    if(PublishBakedMaterial(*library_,document_,parameters_,geometry?geometry->settings.resolution:256U<<bakeResolution_,bakeName_,result,error,path_.parent_path(),geometry,geometryPreview_?geometry_.BakeGuid():"",nullptr,&runtimeNotice)) {
        status_="Baked material: "+result.displayName+" in Materials/GraphBakes"+(runtimeNotice.empty()?std::string(" (live graph shader)"):". "+runtimeNotice);
        if(runtimeNotice.empty())LinkLiveMaterial(result,document_,parameters_,path_.parent_path());
    }
    else status_=error;
}
bool MaterialGraphPanel::SelectGeometryMesh(const std::filesystem::path& path,unsigned index) {
    if(geometryJob_.Running() || uvAtlasJob_.Running()){status_="Finish or cancel the active geometry bake first";return false;}
    if(!geometry_.Select(path,index,status_))return false;
    geometryPreview_=true;previewDirty_=true;status_="Mesh selected; start Geometry Bake";return true;
}
bool MaterialGraphPanel::StartGeometryBake() {
    if(uvAtlasJob_.Running() || applyRequest_){status_="Finish or cancel Bake & Apply first";return false;}
    geometry_.Refresh();const auto* mesh=geometry_.Mesh();
    if(!mesh){status_=geometry_.Error().empty()?"Select a static mesh first":geometry_.Error();return false;}
    if(!geometryJob_.Start(*mesh,geometrySettings_)){status_="A geometry bake is already running";return false;}
    status_="Geometry bake running";return true;
}
bool MaterialGraphPanel::SaveGeometryBake(const std::filesystem::path& path) {
    if(!geometry_.Save(path,status_))return false;status_="Geometry Bake cache saved";RefreshSavedAsset();return true;
}
bool MaterialGraphPanel::BindGeometryBake(const std::string& guid) {
    if(geometryJob_.Running()){status_="Finish or cancel the active geometry bake first";return false;}
    if(!geometry_.Bind(guid,status_))return false;
    geometrySettings_=geometry_.Bake()->settings;geometryPreview_=true;previewDirty_=true;status_="Geometry Bake bound";return true;
}
void MaterialGraphPanel::PollGeometryBake() {
    GeometryMesh atlas;std::string atlasError;
    if(uvAtlasJob_.TakeResult(atlas,atlasError)) {
        if(!applyRequest_)return;
        if(!atlasError.empty()){status_=atlasError;DiscardApplyRequest();return;}
        AssetLibrary::Entry model;
        if(!library_ || !CreateGeometryAtlasAsset(*library_,applyRequest_->target.sourceModelGuid,applyRequest_->target.sourceModelHash,
            applyRequest_->submeshes,atlas,model,status_))
        {DiscardApplyRequest();return;}
        applyRequest_->target.replacementModelGuid=model.guid;applyRequest_->target.replacementModelPath=library_->AssetRelativePath(model);
        if(!geometry_.SelectMaterialSlot(library_->AbsolutePath(model),applyRequest_->target.slot,status_)){DiscardApplyRequest();return;}
        if(!geometryJob_.Start(*geometry_.Mesh(),geometrySettings_)){status_="Cannot start automatic atlas bake";DiscardApplyRequest();return;}
        status_="Automatic UV model ready; baking material geometry...";return;
    }
    GeometryBake completed;std::string error;auto failure=GeometryBakeFailure::None;
    if(!geometryJob_.TakeResult(completed,error,&failure))return;
    if(!error.empty()){
        if(applyRequest_ && !applyRequest_->atlasAttempted && failure==GeometryBakeFailure::UvLayout) {
            geometry_.Refresh(true);applyRequest_->atlasAttempted=true;
            if(geometry_.Mesh() && uvAtlasJob_.Start(*geometry_.Mesh(),geometrySettings_.resolution)) {status_="Preparing automatic UV atlas for this model...";return;}
        }
        status_=error;DiscardApplyRequest();return;
    }
    if(!geometry_.Accept(std::move(completed),status_)){DiscardApplyRequest();return;}
    if (applyRequest_) {
        AssetLibrary::Entry material;
        std::string runtimeNotice;
        if (!library_ || !PublishBakedMaterial(*library_,applyRequest_->document,applyRequest_->parameters,geometry_.Bake()->settings.resolution,
            applyRequest_->name,material,status_,applyRequest_->graphFolder,geometry_.Bake(),{},&geometry_,&runtimeNotice)) { DiscardApplyRequest(); return; }
        auto request=std::move(*applyRequest_);applyRequest_.reset();
        geometry_.Refresh(true);
        if (!geometry_.Bake()) { status_="Model changed during material baking; output remains in Asset Browser. Bake & Apply again."; return; }
        request.target.materialGuid=material.guid;materialAssignment_=std::move(request.target);
        if(runtimeNotice.empty())LinkLiveMaterial(material,request.document,request.parameters,request.graphFolder);
        geometryPreview_=true;previewDirty_=true;
        status_=runtimeNotice.empty()?"Baked material with live graph shader ready; assigning it to the model.":"Baked material ready; assigning it to the model. "+runtimeNotice;return;
    }
    geometryPreview_=true;previewDirty_=true;status_="Geometry Bake complete; save the cache for instances and material export";
}
void MaterialGraphPanel::RenderGeometryBake() {
    PollGeometryBake();
    UpdateLiveMaterial();
    if(!ImGui::CollapsingHeader("Geometry Bake"))return;
    const bool running=geometryJob_.Running() || uvAtlasJob_.Running();
    ImGui::BeginDisabled(running);
    if(ImGui::Button("Select mesh...")) {
        const auto file=BrowseFile(false,{{"Static models","*.fbx;*.obj;*.gltf;*.glb"}});
        if(!file.empty())SelectGeometryMesh(file);
    }
    ImGui::SameLine();if(ImGui::Button("Open bake...")) {
        const auto file=BrowseFile(false,{{"Geometry Bake","*.ixgeometrybake"}});
        if(!file.empty())BindGeometryBake(AssetDatabase::Instance().getOrCreateGuid(file).toString());
    }
    if(!geometry_.MeshGuid().empty()) {
        ImGui::TextWrapped("%s (submesh %u of %u)",geometry_.Name().c_str(),geometry_.MeshIndex()+1,geometry_.MeshCount());
        int index=static_cast<int>(geometry_.MeshIndex());if(ImGui::SliderInt("Submesh",&index,0,static_cast<int>(geometry_.MeshCount())-1))SelectGeometryMesh(geometry_.MeshPath(),static_cast<unsigned>(index));
        if(ImGui::Checkbox("Mesh UV atlas preview",&geometryPreview_))previewDirty_=true;
        if(ImGui::SmallButton("Reload geometry dependencies")){geometry_.Refresh(true);previewDirty_=true;}
    }
    int resolution=geometrySettings_.resolution==512?2:geometrySettings_.resolution==256?1:0;const char* sizes[]={"128","256","512"};
    if(UI::Prop::Combo("Bake resolution",&resolution,sizes,3))geometrySettings_.resolution=128U<<resolution;
    int samples=static_cast<int>(geometrySettings_.aoSamples);if(ImGui::SliderInt("AO rays",&samples,1,128))geometrySettings_.aoSamples=static_cast<unsigned>(samples);
    int padding=static_cast<int>(geometrySettings_.padding);if(ImGui::SliderInt("UV padding",&padding,0,32))geometrySettings_.padding=static_cast<unsigned>(padding);
    UI::Prop::DragFloat("Ray distance (m)",&geometrySettings_.rayDistance,.1f,.001f,10000.0f,"%.3f");
    UI::Prop::DragFloat("Ray bias (m)",&geometrySettings_.rayBias,.0001f,.000001f,1.0f,"%.6f");
    float translation[]={geometrySettings_.objectToWorld[3],geometrySettings_.objectToWorld[7],geometrySettings_.objectToWorld[11]};
    if(ImGui::DragFloat3("World translation",translation,.1f)){geometrySettings_.objectToWorld[3]=translation[0];geometrySettings_.objectToWorld[7]=translation[1];geometrySettings_.objectToWorld[11]=translation[2];}
    if(ImGui::Button("Bake geometry"))StartGeometryBake();
    ImGui::SameLine();ImGui::BeginDisabled(!geometry_.Bake());
    if(ImGui::Button("Save bake...")) {
        const auto file=BrowseFile(true,{{"Geometry Bake","*.ixgeometrybake"}},"ixgeometrybake");
        if(!file.empty())SaveGeometryBake(file);
    }
    ImGui::EndDisabled();ImGui::EndDisabled();
    if(running){ImGui::ProgressBar(geometryJob_.Progress());if(ImGui::Button("Cancel geometry bake"))geometryJob_.Cancel();}
    if(const auto* bake=geometry_.Bake()) {
        ImGui::Text("%u covered texels, %u open boundary edges",bake->coveredTexels,bake->boundaryEdges);
        if(geometrySettings_!=bake->settings)ImGui::TextWrapped("Settings changed. Preview uses the previous successful bake until you rebake.");
    }
    if(!geometry_.Error().empty())ImGui::TextWrapped("%s",geometry_.Error().c_str());
    ImGui::TextWrapped("Static submesh in object space. Preview shows its UV atlas with baked world positions/normals. Curvature is 0.5 on flat faces; cavity is concavity. Open surfaces have zero thickness when no exit ray hits.");
}
void MaterialGraphPanel::ExportShader()
{
    MaterialGraphProgram program;if(!CompileMaterial(program,status_))return;
    const auto path=BrowseFile(true,{{"HLSL shader (*.hlsl)","*.hlsl"}},"hlsl");
    if(path.empty())return;
    std::string error;
    if(!SaveMaterialTextAtomic(path,program.GenerateHlsl(),error))status_=error.empty()?"Cannot save generated shader":error;
    else status_="Generated HLSL exported; bind textures and parameter constants as declared.";
}

}
