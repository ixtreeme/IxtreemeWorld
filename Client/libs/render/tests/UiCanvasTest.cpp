#include "UiCanvas.h"
#include "SceneManager.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <unordered_set>
#if defined(IXTREEME_WITH_EDITOR)
#include "EditorImGui.h"
#include <imgui.h>
#include <imgui_internal.h>
#endif
#if defined(_WIN32)
#include "IXVulkanDevice.h"
#include "VulkanDevice.h"
#include "NativeWindow_Win32.h"
#include "OffscreenSceneRenderer.h"
#include "RmlUiLayer.h"
#include "asset/FileAssetReader.h"
#include "IXRHIShader.h"
#endif

namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; failures += !ok; }
bool Near(float a, float b) { return std::abs(a - b) < 0.02f; }
UiCanvas Fixture()
{
    UiCanvas c; c.id = 3; c.name = "HUD"; c.referenceResolution[0] = 640; c.referenceResolution[1] = 360;
    auto& p = AddUiElement(c, UiElementType::Panel); p.scriptId = "panel"; p.rect.anchorMin[0] = p.rect.anchorMax[0] = 0;
    p.rect.anchorMin[1] = p.rect.anchorMax[1] = 0; p.rect.pivot[0] = p.rect.pivot[1] = 0;
    p.rect.position[0] = 40; p.rect.position[1] = 60; p.rect.sizeDelta[0] = 200; p.rect.sizeDelta[1] = 100;
    p.color[0] = p.color[3] = 1; p.color[1] = p.color[2] = 0; p.cornerRadius = 0;
    auto& b = AddUiElement(c, UiElementType::Button); b.scriptId = "button"; b.rect.position[0] = 80; b.rect.position[1] = -70;
    b.rect.sizeDelta[0] = 100; b.rect.sizeDelta[1] = 40; b.text = "Play < & >";
    return c;
}
void Model()
{
    auto c = Fixture(); auto layout = BuildUiCanvasLayout(c, 640, 360);
    Check(Near(layout[0].x, 40) && Near(layout[0].y, 60) && Near(layout[0].width, 200), "fixed top-left anchor at reference resolution");
    auto half = BuildUiCanvasLayout(c, 320, 180);
    Check(Near(half[0].x, 20) && Near(half[0].width, 100), "Canvas Scaler preserves proportions");
    c.matchWidthOrHeight = 0; Check(Near(UiCanvasScale(c, 1280, 360), 2), "width matching");
    c.matchWidthOrHeight = 1; Check(Near(UiCanvasScale(c, 1280, 360), 1), "height matching");
    c.matchWidthOrHeight = .5f;
    auto& child = AddUiElement(c, UiElementType::Text, 1); child.scriptId = "label"; child.text = "Arviz & <script>";
    child.rect.anchorMin[0] = child.rect.anchorMin[1] = 0; child.rect.anchorMax[0] = child.rect.anchorMax[1] = 1;
    child.rect.sizeDelta[0] = -20; child.rect.sizeDelta[1] = -10;
    layout = BuildUiCanvasLayout(c, 640, 360);
    Check(Near(layout[2].x, 50) && Near(layout[2].y, 65) && Near(layout[2].width, 180), "nested stretch anchors and padding");
    Check(HitTestUiCanvas(c,layout,55,70,640,360) == child.id, "design picking chooses the visible child above its parent");
    child.rect.position[0] = 220; c.elements[0].clipChildren = true;
    layout = BuildUiCanvasLayout(c,640,360);
    Check(HitTestUiCanvas(c,layout,280,70,640,360) == 0, "design picking respects parent clipping");
    child.rect.position[0] = 0; c.elements[0].clipChildren = false; layout = BuildUiCanvasLayout(c,640,360);
    const auto original = layout[2]; const auto childId = child.id;
    Check(ReparentUiElement(c, childId, 0, 640, 360), "reparent to Canvas root");
    layout = BuildUiCanvasLayout(c, 640, 360);
    Check(Near(layout[2].x, original.x) && Near(layout[2].width, original.width), "reparent preserves visual bounds");
    ReparentUiElement(c, childId, 1, 640, 360);
    Check(!ReparentUiElement(c, 1, childId, 640, 360), "hierarchy rejects descendant cycles");
    c.elements[0].enabled = false; Check(!BuildUiCanvasLayout(c, 640, 360)[2].visible, "parent activation propagates"); c.elements[0].enabled = true;
    const auto copy = DuplicateUiElement(c, 1);
    Check(copy && c.elements.size() == 5 && c.elements.back().parentId == copy, "duplicate includes descendants with remapped ids");
    std::unordered_set<std::string> names; for (const auto& e : c.elements) names.insert(e.scriptId);
    Check(names.size() == c.elements.size(), "duplicated Script IDs remain unique");
    DeleteUiElement(c, copy); Check(c.elements.size() == 3, "delete removes only the chosen subtree");
    const auto parts=BuildUiSliderLayout(c.elements[1],{0,10,20,200,50},1);
    Check(Near(parts.track.y,39) && Near(parts.track.height,12) && Near(parts.handle.x,98) && Near(parts.fill.width,88), "slider preview shares centered track and movable handle bounds");
    auto& skin=c.elements[1]; skin.imagePath="normal.png"; skin.hoverImagePath="hover.png"; skin.pressedImagePath="pressed.png"; skin.disabledImagePath="disabled.png";
    Check(UiElementPreviewImage(skin,1)=="hover.png" && UiElementPreviewImage(skin,2)=="pressed.png" && UiElementPreviewImage(skin,3)=="disabled.png", "authoring preview selects each control state image");
    skin.interactable=false; Check(UiElementPreviewImage(skin,1)=="disabled.png", "disabled preview takes precedence over hover"); skin.interactable=true;
    const auto rml = BuildUiCanvasRml(c, 640, 360);
    Check(rml.find("&lt;script&gt;") != std::string::npos && rml.find("Play &lt; &amp; &gt;") != std::string::npos, "RML escapes editable text");
    c.elements[0].parentId = childId; c.elements[2].parentId = 1; c.elements[1].scriptId = "label";
    c.elements[0].fontSize = std::numeric_limits<float>::quiet_NaN(); SanitizeUiCanvas(c);
    Check(std::isfinite(c.elements[0].fontSize) && c.elements[0].parentId == 0 && c.elements[1].scriptId != c.elements[2].scriptId, "malformed input is sanitized");
}

class TestRenderer : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return ++next; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& size, const Rml::String& source) override { loadedSources.push_back(source); size = {4,4}; return ++next; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return ++next; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
    std::uintptr_t next = 0;
    std::vector<std::string> loadedSources;
};
class TestSystem : public Rml::SystemInterface
{
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    { if (type == Rml::Log::LT_WARNING || type == Rml::Log::LT_ERROR) { ++warnings; std::cerr << message << '\n'; } return true; }
    int warnings = 0;
};
void RmlLayout()
{
    TestRenderer renderer; TestSystem system;
    Rml::SetRenderInterface(&renderer); Rml::SetSystemInterface(&system);
    Check(Rml::Initialise(), "RmlUi initialises");
    Check(Rml::LoadFontFace(std::string(IX_UI_TEST_ASSETS_ROOT) + "/assets/fonts/Roboto-Regular.ttf"), "runtime font loads");
    auto* context = Rml::CreateContext("canvas-layout-test", {640,360});
    auto canvas = Fixture();
    auto& child = AddUiElement(canvas, UiElementType::Text, 1); child.scriptId = "child"; child.text = "Nested";
    auto& field = AddUiElement(canvas, UiElementType::InputField); field.scriptId = "name"; field.text = "Initial";
    auto& toggle = AddUiElement(canvas, UiElementType::Toggle); toggle.scriptId = "toggle"; toggle.checked = true;
    auto& slider = AddUiElement(canvas, UiElementType::Slider); slider.scriptId = "volume"; slider.value = .35f;
    auto& bar = AddUiElement(canvas, UiElementType::ProgressBar); bar.scriptId = "hp"; bar.value = .7f;
    for (auto& element : canvas.elements)
    {
        element.imagePath="skin assets/normal.png"; element.hoverImagePath="skin assets/hover.png";
        element.pressedImagePath="skin assets/pressed.png"; element.disabledImagePath="skin assets/disabled.png";
        element.checkedImagePath="skin assets/checked.png"; element.fillImagePath="skin assets/fill.png"; element.handleImagePath="skin assets/handle.png";
    }
    auto* document = context->LoadDocumentFromMemory(BuildUiCanvasRml(canvas, 640, 360,[](const auto& path) { return "resolved/"+path; }));
    Check(document != nullptr, "generated Canvas RML loads");
    if (document)
    {
        document->Show(); context->Update();
        auto* label = document->GetElementById("button__label");
        Check(label && label->GetBox().GetSize(Rml::BoxArea::Content).x > 20 && label->GetBox().GetSize(Rml::BoxArea::Content).y > 10,
            "button label has visible text layout inside the flex container");
        for (const auto& r : BuildUiCanvasLayout(canvas, 640, 360))
        {
            const auto* e = FindUiElement(canvas, r.id); auto* node = document->GetElementById(e->scriptId);
            Check(node && Near(node->GetAbsoluteOffset(Rml::BoxArea::Border).x, r.x) && Near(node->GetBox().GetSize(Rml::BoxArea::Border).x, r.width), "native RmlUi and editor agree on geometry");
        }
        auto* input = dynamic_cast<Rml::ElementFormControlInput*>(document->GetElementById("name__control"));
        auto* range = dynamic_cast<Rml::ElementFormControlInput*>(document->GetElementById("volume__control"));
        Check(input && input->GetValue() == "Initial" && range && Near(std::stof(range->GetValue()), .35f), "native input and slider values");
        auto* checkedControl=document->GetElementById("toggle__control");
        Check(checkedControl->HasAttribute("checked"), "native toggle starts checked");
        Check(checkedControl->GetProperty("decorator") && checkedControl->GetProperty("decorator")->ToString().find("checked.png")!=std::string::npos, "checked control uses its bound image");
        context->ProcessMouseMove(380, 105, 0); context->Update();
        Check(context->GetHoverElement() && context->GetHoverElement()->GetId() == "button", "button receives pointer events");
        auto* skinnedButton=document->GetElementById("button");
        Check(skinnedButton->GetProperty("decorator") && skinnedButton->GetProperty("decorator")->ToString().find("hover.png")!=std::string::npos, "native hover selects the bound button image");
        context->ProcessMouseButtonDown(0,0); context->Update();
        Check(skinnedButton->GetProperty("decorator") && skinnedButton->GetProperty("decorator")->ToString().find("pressed.png")!=std::string::npos, "native press selects the bound button image");
        context->ProcessMouseButtonUp(0,0); context->Update();
        context->ProcessMouseMove(45, 65, 0); context->Update();
        Check(context->GetHoverElement() != document->GetElementById("panel"), "decorative panel passes pointer through");
        context->Render();
        Check(std::find(renderer.loadedSources.begin(),renderer.loadedSources.end(),"resolved/skin assets/normal.png")!=renderer.loadedSources.end(), "all image slots resolve asset paths containing spaces");
        document->Close(); context->Update();
    }
    Check(system.warnings == 0, "generated control styles have no RmlUi warnings");
    Rml::RemoveContext("canvas-layout-test"); Rml::Shutdown(); Rml::SetRenderInterface(nullptr); Rml::SetSystemInterface(nullptr);
}

void Persistence()
{
    namespace fs = std::filesystem;
    const auto parent = fs::temp_directory_path(); const auto root = parent / ("ixw_canvas_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root); auto& manager = SceneManager::Instance(); manager.NewScene();
    auto canvas = Fixture(); canvas.elements[1].text = "Unicode \xc3\xa1 & \"quote\"\nnew line";
    for (int type = 0; type < 8; ++type) AddUiElement(canvas, static_cast<UiElementType>(type), 1);
    for (auto& element : canvas.elements)
    {
        element.imagePath="textures/ui/panel.png"; element.hoverImagePath="textures/ui/hover.png"; element.pressedImagePath="textures/ui/pressed.png";
        element.disabledImagePath="textures/ui/disabled.png"; element.checkedImagePath="textures/ui/checked.png";
        element.fillImagePath="textures/ui/fill.png"; element.handleImagePath="textures/ui/handle.png";
        element.imageTint[0]=.4f; element.fillColor[1]=.6f; element.handleColor[2]=.2f;
        element.sliderTrackHeight=8; element.sliderHandleSize[0]=18; element.sliderHandleSize[1]=26;
    }
    manager.SetUiCanvases({canvas}); const auto revision = manager.UiRevision(); const auto snapshot = manager.GetCurrentScene();
    manager.SetCurrentSceneSnapshot(snapshot); Check(manager.UiRevision() == revision, "routine snapshots preserve UI revision");
    const bool saved = manager.SaveSceneAs((root / "canvas.scene").string()); manager.CloseScene();
    const bool loaded = saved && manager.LoadScene((root / "canvas.scene").string());
    Check(loaded && manager.GetCurrentScene().uiCanvases == snapshot.uiCanvases, "all Canvas properties round-trip through scene JSON");
    manager.CloseScene(); { std::ofstream file(root / "legacy.scene"); file << "{\"version\":1,\"entities\":[]}"; }
    Check(manager.LoadScene((root / "legacy.scene").string()) && manager.GetCurrentScene().uiCanvases.empty(), "legacy scenes need no Canvas fields");
    manager.CloseScene();
    { std::ofstream file(root / "legacy_ui.scene"); file << R"({"version":1,"entities":[],"ui_canvases":[{"id":1,"elements":[{"id":1,"type":"Image","image_path":"ui/legacy.png","color":[0.5,0.4,0.3,0.8]}]}]})"; }
    const bool legacyLoaded=manager.LoadScene((root / "legacy_ui.scene").string());
    const auto& legacy=manager.GetCurrentScene().uiCanvases;
    Check(legacyLoaded && legacy.size()==1 && legacy[0].elements.size()==1 && legacy[0].elements[0].imageTint[0]==1 && legacy[0].elements[0].hoverImagePath.empty() && Near(legacy[0].elements[0].color[0],.5f), "older UI scenes retain their image tint and use empty optional skin slots");
    manager.CloseScene(); if (root.parent_path() == parent && root.filename().string().starts_with("ixw_canvas_test_")) fs::remove_all(root);
}
}

#if defined(IXTREEME_WITH_EDITOR)
struct EditorCanvasTestAccess
{
    static void Create(EditorImGui& editor) { editor.CreateUiCanvas(); }
    static void Draw(EditorImGui& editor, bool overlay = false)
    {
        editor.m_editorModeActive = true;
        editor.HandleUiCanvasHotkeys();
        ImGui::SetNextWindowPos({235,35},ImGuiCond_Always); ImGui::SetNextWindowSize({715,730},ImGuiCond_Always);
        editor.RenderSceneViewDropTarget();
        ImGui::SetNextWindowPos({0,35},ImGuiCond_Always); ImGui::SetNextWindowSize({230,730},ImGuiCond_Always);
        if (ImGui::Begin("Hierarchy###Hierarchy")) editor.RenderUiCanvasHierarchy(); ImGui::End();
        ImGui::SetNextWindowPos({955,35},ImGuiCond_Always); ImGui::SetNextWindowSize({325,730},ImGuiCond_Always);
        editor.RenderInspector();
        if (overlay)
        {
            ImGui::SetNextWindowPos({420,170},ImGuiCond_Always); ImGui::SetNextWindowSize({220,210},ImGuiCond_Always);
            ImGui::Begin("Floating window regression"); ImGui::TextUnformatted("Clicks stay in this window."); ImGui::End();
        }
    }
    static void Mode(EditorImGui& editor,bool twoD) { editor.SetSceneView2D(twoD); }
    static void TextureSource(EditorImGui& editor)
    {
        ImGui::SetNextWindowPos({0,500},ImGuiCond_Always); ImGui::SetNextWindowSize({230,160},ImGuiCond_Always);
        ImGui::Begin("Texture source regression");
        AssetLibrary::Entry entry; entry.id="ui-skin-texture-test"; entry.displayName="UI texture"; entry.category=AssetLibrary::Category::Texture;
        editor.RenderAssetTile(entry,64); ImGui::End();
    }
    static bool UiSelected(const EditorImGui& editor) { return editor.m_uiSelectionActive && !editor.m_assetInspectorSelectionActive; }
    static ImVec2 ModeButton(bool twoD)
    {
        const auto* scene=ImGui::FindWindowByName("Scene View###SceneView");
        return {scene->DC.CursorStartPos.x+(twoD?71.0f:21.0f),scene->DC.CursorStartPos.y+9.0f};
    }
    static UiCanvas* Canvas(EditorImGui& editor) { return editor.SelectedUiCanvas(); }
    static void Commit(EditorImGui& editor, const std::vector<UiCanvas>& before) { editor.CommitUiCanvasEdit(before); }
    static void Undo(EditorImGui& editor, bool redo) { editor.UiCanvasUndo(redo); }
    static void Sync(EditorImGui& editor) { editor.EnsureUiCanvasState(); }
    static void TestKeyboardMapping(EditorImGui& editor)
    {
        editor.m_gameViewVisible = editor.m_gameViewRect.valid = true;
        editor.m_gameViewRect.size[0] = 640; editor.m_gameViewRect.size[1] = 360;
        InputEvent input, mapped; input.type = InputEvent::Char; input.codepoint = 'A';
        editor.m_gameViewFocused = true;
        Check(editor.MapInputToGameView(input,mapped) && mapped.codepoint == 'A', "focused Game view receives Canvas text without mouse coordinates");
        editor.m_gameViewFocused = false;
        Check(!editor.MapInputToGameView(input,mapped), "editor fields retain keyboard input outside the Game view");
    }
    static ImVec2 Position(EditorImGui& editor, bool corner = false)
    {
        const ImVec2 start(editor.m_uiViewportOrigin[0],editor.m_uiViewportOrigin[1]);
        const ImVec2 size(editor.m_uiViewportSize[0],editor.m_uiViewportSize[1]);
        const float factor = std::min((size.x-30)/editor.m_uiPreviewSize[0],(size.y-30)/editor.m_uiPreviewSize[1])*editor.m_uiZoom;
        const auto c = editor.SelectedUiCanvas(); if (!c) return {};
        const auto rect = BuildUiCanvasLayout(*c,editor.m_uiPreviewSize[0],editor.m_uiPreviewSize[1])[0];
        return {start.x+(size.x-editor.m_uiPreviewSize[0]*factor)*.5f+editor.m_uiPan[0]+(rect.x+rect.width*(corner?1:.5f))*factor,
            start.y+(size.y-editor.m_uiPreviewSize[1]*factor)*.5f+editor.m_uiPan[1]+(rect.y+rect.height*(corner?1:.5f))*factor};
    }
    static void Capture()
    {
        auto* draw = ImGui::GetDrawData();
        std::ofstream output(std::filesystem::path(IX_UI_TEST_SHADER_ROOT)/"ui_editor_draw.json");
        output << "{\"width\":1280,\"height\":800,\"lists\":[";
        for (int n = 0; n < draw->CmdListsCount; ++n)
        {
            const auto* list = draw->CmdLists[n]; if (n) output << ',';
            output << "{\"vertices\":[";
            for (int i=0;i<list->VtxBuffer.Size;++i) { const auto& v=list->VtxBuffer[i]; if(i)output<<','; output<<'['<<v.pos.x<<','<<v.pos.y<<','<<v.uv.x<<','<<v.uv.y<<','<<v.col<<']'; }
            output << "],\"indices\":[";
            for (int i=0;i<list->IdxBuffer.Size;++i) { if(i)output<<','; output<<list->IdxBuffer[i]; }
            output << "],\"commands\":[";
            for (int i=0;i<list->CmdBuffer.Size;++i)
            { const auto& c=list->CmdBuffer[i]; if(i)output<<','; output<<'['<<c.ElemCount<<','<<c.IdxOffset<<','<<c.VtxOffset<<','<<c.ClipRect.x<<','<<c.ClipRect.y<<','<<c.ClipRect.z<<','<<c.ClipRect.w<<']'; }
            output << "]}";
        }
        unsigned char* atlas; int w,h; ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas,&w,&h);
        output << "],\"atlas_width\":"<<w<<",\"atlas_height\":"<<h<<'}';
        std::ofstream texture(std::filesystem::path(IX_UI_TEST_SHADER_ROOT)/"ui_editor_atlas.rgba",std::ios::binary);
        texture.write(reinterpret_cast<char*>(atlas),static_cast<std::streamsize>(w)*h*4);
    }
};
namespace {
void Editor()
{
    auto& manager = SceneManager::Instance(); manager.NewScene();
    ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1280,800}; io.DeltaTime = 1.0f / 60;
    io.ConfigInputTrickleEventQueue = false;
    unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    {
        EditorImGui editor; ImGui::NewFrame(); EditorCanvasTestAccess::Create(editor); EditorCanvasTestAccess::Draw(editor); ImGui::Render();
        Check(manager.GetCurrentScene().uiCanvases.size() == 1, "Create UI Canvas persists a scene canvas");
        Check(editor.IsSceneView2D() && !editor.IsSceneViewVisible(), "Create Canvas opens Scene View 2D and skips the 3D render");
        Check(ImGui::FindWindowByName("UI Editor###UiCanvas") == nullptr && ImGui::FindWindowByName("Inspector###Inspector"), "UI authoring uses Scene View and the existing Inspector without another editor window");
        const auto before = manager.GetCurrentScene().uiCanvases;
        AddUiElement(*EditorCanvasTestAccess::Canvas(editor), UiElementType::Button);
        EditorCanvasTestAccess::Commit(editor, before);
        EditorCanvasTestAccess::Undo(editor, false); Check(manager.GetCurrentScene().uiCanvases[0].elements.empty(), "UI edit undo");
        EditorCanvasTestAccess::Undo(editor, true); Check(manager.GetCurrentScene().uiCanvases[0].elements.size() == 1, "UI edit redo");
        ImGui::NewFrame(); EditorCanvasTestAccess::Draw(editor); ImGui::Render();
        Check(ImGui::GetDrawData()->TotalVtxCount > 100, "Canvas editor builds a visual preview");
        const auto frame = [&] { ImGui::NewFrame(); EditorCanvasTestAccess::Draw(editor); ImGui::Render(); };
        auto pointer = EditorCanvasTestAccess::Position(editor); io.AddMousePosEvent(pointer.x,pointer.y); io.AddMouseButtonEvent(0,true); frame();
        io.AddMousePosEvent(pointer.x+18,pointer.y+12); frame(); io.AddMouseButtonEvent(0,false); frame();
        const auto moved = EditorCanvasTestAccess::Canvas(editor)->elements[0].rect.position[0];
        Check(moved > 8, "production canvas mouse drag moves the selected element");
        EditorCanvasTestAccess::Undo(editor,false); Check(EditorCanvasTestAccess::Canvas(editor)->elements[0].rect.position[0] == 0, "one undo restores the entire drag gesture");
        pointer = EditorCanvasTestAccess::Position(editor,true); io.AddMousePosEvent(pointer.x,pointer.y); io.AddMouseButtonEvent(0,true); frame();
        io.AddMousePosEvent(pointer.x+20,pointer.y+15); frame(); io.AddMouseButtonEvent(0,false); frame();
        Check(EditorCanvasTestAccess::Canvas(editor)->elements[0].rect.sizeDelta[0] > 200, "production canvas resize handle changes size");
        InputEvent worldClick; worldClick.type=InputEvent::MouseDown; worldClick.x=static_cast<int>(pointer.x); worldClick.y=static_cast<int>(pointer.y);
        Check(!editor.IsSceneViewInputTarget(worldClick), "2D Canvas pointer never enters 3D picking or camera controls");
        const auto edited=manager.GetCurrentScene().uiCanvases;
        auto mode=EditorCanvasTestAccess::ModeButton(false); io.AddMousePosEvent(mode.x,mode.y); io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame();
        Check(!editor.IsSceneView2D() && editor.IsSceneViewVisible() && manager.GetCurrentScene().uiCanvases==edited, "Scene View 3D toolbar restores the viewport and preserves UI layout");
        worldClick.x=550; worldClick.y=250;
        Check(editor.IsSceneViewInputTarget(worldClick), "visible unobstructed 3D viewport accepts world input");
        const auto overlayFrame=[&] { ImGui::NewFrame(); EditorCanvasTestAccess::Draw(editor,true); ImGui::Render(); };
        io.AddMousePosEvent(280,500); overlayFrame(); overlayFrame();
        Check(!editor.IsSceneViewInputTarget(worldClick), "floating window blocks clicks at event coordinates even with stale Scene hover");
        worldClick.type=InputEvent::MouseWheel; Check(!editor.IsSceneViewInputTarget(worldClick), "floating window blocks viewport zoom behind it");
        worldClick.type=InputEvent::MouseMove; Check(!editor.IsSceneViewInputTarget(worldClick), "floating window blocks scene mouse-look behind it");
        frame(); frame();
        mode=EditorCanvasTestAccess::ModeButton(true); io.AddMousePosEvent(mode.x,mode.y); io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame();
        Check(editor.IsSceneView2D() && manager.GetCurrentScene().uiCanvases==edited, "Scene View 2D toolbar returns to the same Canvas without moving the 3D camera");
        const auto textureFrame=[&] { ImGui::NewFrame(); EditorCanvasTestAccess::Draw(editor); EditorCanvasTestAccess::TextureSource(editor); ImGui::Render(); };
        textureFrame(); textureFrame();
        const auto* source=ImGui::FindWindowByName("Texture source regression");
        io.AddMousePosEvent(source->DC.CursorStartPos.x+32,source->DC.CursorStartPos.y+32); io.AddMouseButtonEvent(0,true); textureFrame();
        io.AddMouseButtonEvent(0,false); textureFrame();
        Check(EditorCanvasTestAccess::UiSelected(editor), "choosing a texture in 2D retains the Canvas Inspector and its image drop targets");
        EditorCanvasTestAccess::TestKeyboardMapping(editor);
        manager.SetUiCanvases({MakeUiHudCanvas(5)}); EditorCanvasTestAccess::Sync(editor); frame(); EditorCanvasTestAccess::Capture();
        manager.CloseScene(); manager.NewScene(); EditorCanvasTestAccess::Sync(editor);
        Check(!EditorCanvasTestAccess::Canvas(editor), "switching scenes clears editor UI selection and undo state");
    }
    ImGui::DestroyContext(); manager.CloseScene();
}
}
#endif

#if defined(_WIN32)
namespace {
void Gpu()
{
    NativeWindow_Win32 window;
    if (!window.Create(GetModuleHandle(nullptr), "Canvas GPU regression", 64, 64)) { Check(false, "GPU window"); return; }
    ShowWindow(window.GetHwnd(), SW_HIDE); VulkanDevice native;
    if (!native.Create(window, 64, 64)) { Check(false, "GPU device"); window.Destroy(); return; }
    auto rhi = ixvulkan::CreateDevice(native);
    [&] {
        client::asset::FileAssetReader assets(IX_UI_TEST_ASSETS_ROOT), shaders(IX_UI_TEST_SHADER_ROOT);
        using S = ixrhi::IXRHIShaderStage; using B = ixrhi::IXRHIBindingType; using U = ixrhi::IXRHIBufferUsage;
        auto bytes = shaders.ReadAll("ui_canvas_test.spv"); if (!bytes) { Check(false, "readback shader"); return; }
        ixrhi::IXRHIShaderDesc shader; shader.stage = S::Compute; shader.entryPoint = "CSMain";
        auto words = reinterpret_cast<const std::uint32_t*>(bytes->data()); shader.spirv.assign(words, words + bytes->size() / 4);
        auto cs = rhi->CreateShader(shader);
        auto layout = rhi->CreateBindGroupLayout({{0,B::StorageBuffer,S::Compute},{1,B::SampledTexture,S::Compute}});
        auto bindings = rhi->CreateBindGroup(*layout, 1); auto sampler = rhi->CreateSampler({});
        ixrhi::IXRHIComputePipelineDesc pipeline; pipeline.computeShader = cs; pipeline.bindGroupLayouts = {layout.get()};
        auto compute = rhi->CreateComputePipeline(pipeline);
        const unsigned bufferBytes = 640 * 360 * 4 * sizeof(float);
        ixrhi::IXRHIBufferDesc buffer; buffer.sizeBytes = bufferBytes; buffer.usage = U::Storage | U::TransferSrc;
        auto output = rhi->CreateBuffer(buffer, nullptr, 0);
        buffer.usage = U::TransferDst; buffer.cpuAccess = ixrhi::IXRHICpuAccess::Write;
        auto host = rhi->CreateBuffer(buffer, nullptr, 0); bindings->UpdateBuffer(0, 0, output, 0, bufferBytes);
        OffscreenSceneRenderer view; RmlUiLayer ui;
        Check(view.Create(*rhi, assets, 640, 360, OffscreenSceneRenderer::kSceneColorFormat, rhi->GetMainSwapchain().DepthFormat(), "Canvas.Game"), "native Game render target");
        Check(ui.Create(*rhi, assets, 640, 360), "native RmlUi renderer"); ui.HideAll();
        ui.SetTargetPass(view.GetDisplayPass()); Check(ui.RecreatePipeline(*rhi), "native UI pipeline");
        auto canvas = Fixture();
        const auto imagePath = std::filesystem::path(IX_UI_TEST_SHADER_ROOT) / "ui_canvas_alpha.tga";
        {
            std::ofstream image(imagePath, std::ios::binary); unsigned char header[18]{};
            header[2] = 2; header[12] = header[14] = 4; header[16] = 32; header[17] = 0x28;
            image.write(reinterpret_cast<char*>(header), sizeof(header));
            const unsigned char pixel[] = {0,255,0,128}; for (int i = 0; i < 16; ++i) image.write(reinterpret_cast<const char*>(pixel), 4);
        }
        auto& image = AddUiElement(canvas, UiElementType::Image, 1); image.scriptId = "icon"; image.imagePath = imagePath.generic_string();
        image.rect.anchorMin[0] = image.rect.anchorMax[0] = image.rect.anchorMin[1] = image.rect.anchorMax[1] = 0;
        image.rect.pivot[0] = image.rect.pivot[1] = 0; image.rect.position[0] = image.rect.position[1] = 20;
        image.rect.sizeDelta[0] = image.rect.sizeDelta[1] = 40;
        auto& field = AddUiElement(canvas, UiElementType::InputField); field.scriptId = "name"; field.text = "Initial"; field.rect.position[1] = 110;
        auto& toggle = AddUiElement(canvas, UiElementType::Toggle); toggle.scriptId = "toggle"; toggle.rect.position[0] = 220;
        auto& slider = AddUiElement(canvas, UiElementType::Slider); slider.scriptId = "volume"; slider.rect.position[1] = 50;
        auto& bar = AddUiElement(canvas, UiElementType::ProgressBar); bar.scriptId = "hp"; bar.rect.position[0] = -150; bar.rect.position[1] = 100;
        std::vector<std::filesystem::path> skinPaths;
        const auto solidTexture=[&](const char* name,unsigned char red,unsigned char green,unsigned char blue) {
            const auto path=std::filesystem::path(IX_UI_TEST_SHADER_ROOT)/(std::string("ui skin ")+name+".tga"); skinPaths.push_back(path);
            std::ofstream file(path,std::ios::binary); unsigned char header[18]{}; header[2]=2; header[12]=header[14]=4; header[16]=32; header[17]=0x28;
            file.write(reinterpret_cast<char*>(header),18); const unsigned char rgba[]={blue,green,red,255};
            for(int pixel=0;pixel<16;++pixel) file.write(reinterpret_cast<const char*>(rgba),4);
            return path.generic_string();
        };
        const auto normal=solidTexture("normal",255,0,0), hover=solidTexture("hover",0,255,0), pressed=solidTexture("pressed",0,0,255);
        const auto disabled=solidTexture("disabled",255,255,0), checked=solidTexture("checked",255,0,255), fill=solidTexture("fill",0,255,255), thumb=solidTexture("handle",255,255,255);
        const auto place=[&](UiElement& e,float x,float y,float w,float h) {
            e.rect.anchorMin[0]=e.rect.anchorMax[0]=e.rect.anchorMin[1]=e.rect.anchorMax[1]=e.rect.pivot[0]=e.rect.pivot[1]=0;
            e.rect.position[0]=x; e.rect.position[1]=y; e.rect.sizeDelta[0]=w; e.rect.sizeDelta[1]=h; e.cornerRadius=0; e.text.clear();
        };
        auto& skinPanel=AddUiElement(canvas,UiElementType::Panel); skinPanel.scriptId="skin_panel"; place(skinPanel,480,10,60,35); skinPanel.imagePath=normal;
        auto& skinButton=AddUiElement(canvas,UiElementType::Button); skinButton.scriptId="skin_button"; place(skinButton,480,55,60,35);
        skinButton.imagePath=normal; skinButton.hoverImagePath=hover; skinButton.pressedImagePath=pressed;
        auto& disabledButton=AddUiElement(canvas,UiElementType::Button); disabledButton.scriptId="skin_disabled"; place(disabledButton,480,100,60,35);
        disabledButton.imagePath=normal; disabledButton.disabledImagePath=disabled; disabledButton.interactable=false;
        for(auto& e:canvas.elements)
        {
            if(e.scriptId=="name") e.imagePath=normal;
            if(e.scriptId=="toggle") { e.imagePath=normal; e.checkedImagePath=checked; }
            if(e.scriptId=="volume") { e.imagePath=normal; e.fillImagePath=fill; e.handleImagePath=thumb; e.hoverImagePath=hover; e.pressedImagePath=pressed; }
            if(e.scriptId=="hp") { e.imagePath=normal; e.fillImagePath=fill; }
        }
        ui.SyncSceneCanvases({canvas}, 1, IX_UI_TEST_ASSETS_ROOT); ui.Update();
        const auto handle = ui.FindSceneCanvas("HUD"); Check(handle != 0, "scene Canvas creates a script-addressable document");
        Check(ui.SetGameElementText(handle, "button", "Play"), "script updates Canvas button label");
        Check(ui.SetGameElementText(handle, "name", "Changed") && ui.GetGameElementValue(handle, "name") == "Changed", "script updates native input value");
        Check(ui.SetGameElementProperty(handle, "volume", "value", "0.73") && Near(std::stof(ui.GetGameElementValue(handle, "volume")), .73f), "script updates native slider");
        Check(ui.SetGameElementProperty(handle, "toggle", "checked", "true") && ui.IsGameElementChecked(handle, "toggle"), "script updates native checkbox");
        Check(ui.SetGameElementProperty(handle, "hp", "value", "0.8") && Near(std::stof(ui.GetGameElementValue(handle, "hp")), .8f), "script updates native progress bar");
        InputEvent input; input.x = 380; input.y = 105; ui.OnInput(input); ui.Update();
        input.type = InputEvent::MouseDown; ui.OnInput(input); input.type = InputEvent::MouseUp; ui.OnInput(input); ui.Update();
        Check(ui.ConsumeGameElementClick(handle, "button") && !ui.ConsumeGameElementClick(handle, "button"), "native button click reaches script once");
        const auto click = [&](int x,int y) {
            InputEvent e; e.x=x; e.y=y; ui.OnInput(e); ui.Update();
            e.type=InputEvent::MouseDown; ui.OnInput(e); e.type=InputEvent::MouseUp; ui.OnInput(e); ui.Update();
        };
        click(540,180); Check(!ui.IsGameElementChecked(handle,"toggle") && ui.ConsumeGameElementClick(handle,"toggle"), "mouse click toggles native checkbox and reports the wrapper Script ID");
        click(540,180); Check(ui.IsGameElementChecked(handle,"toggle"), "second checkbox click restores its state");
        click(270,225); Check(std::stof(ui.GetGameElementValue(handle,"volume")) < .5f, "mouse click changes native slider value");
        click(310,285); Check(ui.HasGameUiTextFocus(), "native input field takes keyboard focus");
        InputEvent key; key.type=InputEvent::KeyDown; key.key=Key_End; Check(ui.OnInput(key), "focused UI input consumes keys before player movement");
        key.type=InputEvent::Char; key.codepoint='!'; Check(ui.OnInput(key), "focused UI input consumes typed text"); ui.Update();
        Check(ui.GetGameElementValue(handle,"name") == "Changed!", "typed text updates the native input value");
        click(380,105); Check(!ui.HasGameUiTextFocus(), "leaving a text field releases gameplay keyboard input");
        click(500,110); Check(!ui.ConsumeGameElementClick(handle,"skin_disabled"), "disabled skinned button never emits clicks");
        using State = ixrhi::IXRHIBufferState; State outputState = State::Undefined, hostState = State::Undefined;
        for (unsigned fixture = 0; fixture < 6; ++fixture)
        {
            const unsigned width = fixture < 2 ? 640 : 320, height = fixture < 2 ? 360 : 180;
            if (fixture == 2)
            {
                rhi->WaitIdle(); Check(view.Recreate(*rhi,width,height,view.DisplayFormat(),view.DepthFormat()), "Game viewport resize");
                ui.SetTargetPass(view.GetDisplayPass()); ui.RecreatePipeline(*rhi); ui.Resize(width,height); ui.Update();
                Check(ui.FindSceneCanvas("HUD") == handle && ui.GetGameElementValue(handle,"name") == "Changed!" && ui.IsGameElementChecked(handle,"toggle"), "resize preserves handles and runtime form state");
            }
            UiElement runtimeSlider; for(const auto& e:canvas.elements) if(e.scriptId=="volume") runtimeSlider=e;
            runtimeSlider.value=std::stof(ui.GetGameElementValue(handle,"volume"));
            const unsigned divisor=fixture<2 ? 1 : 2;
            const auto sliderBounds=BuildUiSliderLayout(runtimeSlider,{0,220.0f/divisor,205.0f/divisor,200.0f/divisor,50.0f/divisor},1.0f/divisor);
            if(fixture==1 || fixture==2) { InputEvent e; e.x=500/divisor; e.y=70/divisor; ui.OnInput(e); ui.Update(); }
            if(fixture==3) { InputEvent e; e.type=InputEvent::MouseDown; e.x=500/divisor; e.y=70/divisor; ui.OnInput(e); ui.Update(); }
            if(fixture==4)
            {
                InputEvent e; e.type=InputEvent::MouseUp; e.x=250; e.y=35; ui.OnInput(e);
                e.type=InputEvent::MouseMove; e.x=static_cast<int>(sliderBounds.handle.x+sliderBounds.handle.width*.5f); e.y=static_cast<int>(sliderBounds.handle.y+sliderBounds.handle.height*.5f); ui.OnInput(e); ui.Update();
            }
            if(fixture==5) { InputEvent e; e.type=InputEvent::MouseDown; e.x=static_cast<int>(sliderBounds.handle.x+sliderBounds.handle.width*.5f); e.y=static_cast<int>(sliderBounds.handle.y+sliderBounds.handle.height*.5f); ui.OnInput(e); ui.Update(); }
            auto frame = rhi->BeginFrame(); if (!frame) { Check(false,"frame"); break; } auto& cmd = *frame.info.commandList;
            view.BeginMainPass(cmd, frame.info); view.EndMainPass(cmd); view.BeginDisplayPass(cmd,frame.info,{});
            ui.Render(cmd,frame.info); view.EndDisplayPass(cmd);
            bindings->UpdateTexture(0,1,view.GetDisplayTexture(),sampler);
            cmd.TransitionBuffer(*output,outputState,State::ShaderWrite); cmd.SetComputePipeline(*compute); cmd.BindGroup(0,*bindings,0);
            cmd.Dispatch((width+7)/8,(height+7)/8,1); cmd.TransitionBuffer(*output,State::ShaderWrite,State::TransferSrc); outputState = State::TransferSrc;
            cmd.TransitionBuffer(*host,hostState,State::TransferDst); cmd.CopyBuffer(*output,*host,bufferBytes);
            cmd.TransitionBuffer(*host,State::TransferDst,State::HostRead); hostState = State::HostRead;
            rhi->GetMainRenderTarget()->Begin(cmd); rhi->GetMainRenderTarget()->End(cmd); rhi->EndFrame(frame); rhi->WaitIdle();
            std::vector<float> values(bufferBytes / sizeof(float)); host->Read(0,values.data(),bufferBytes);
            const auto pixel = [&](unsigned x,unsigned y,unsigned c) { return values[(y*width+x)*4+c]; };
            const unsigned div = fixture < 2 ? 1 : 2;
            Check(pixel(45/div,65/div,0) > .95f && pixel(45/div,65/div,1) < .02f, "GPU paints Canvas at anchored rectangle");
            unsigned textPixels = 0;
            for (unsigned y=90/div; y<130/div; ++y) for (unsigned x=350/div; x<450/div; ++x)
                textPixels += pixel(x,y,0) > .75f && pixel(x,y,1) > .75f && pixel(x,y,2) > .75f;
            Check(textPixels > 15, "GPU renders the centered button text at each viewport size");
            std::cout << "alpha image=" << pixel(70/div,90/div,0) << ',' << pixel(70/div,90/div,1) << '\n';
            Check(std::abs(pixel(70/div,90/div,0)-.498f) < .035f && std::abs(pixel(70/div,90/div,1)-.502f) < .035f, "GPU image decode and premultiplied alpha are correct");
            const auto rgb=[&](unsigned x,unsigned y,float red,float green,float blue) { return std::abs(pixel(x,y,0)-red)<.04f && std::abs(pixel(x,y,1)-green)<.04f && std::abs(pixel(x,y,2)-blue)<.04f; };
            Check(rgb(490/div,20/div,1,0,0), "GPU renders panel background texture without the fallback color tint");
            Check(rgb(490/div,110/div,1,1,0), "GPU renders the disabled button image");
            Check(rgb(400/div,290/div,1,0,0), "GPU renders the Input Field background image");
            Check(rgb(535/div,175/div,1,0,1), "GPU renders the checked Toggle image");
            Check(rgb(170/div,285/div,0,1,1) && rgb(240/div,285/div,1,0,0), "GPU renders separate Progress Bar fill and background images");
            const unsigned trackY=static_cast<unsigned>(sliderBounds.track.y+sliderBounds.track.height*.5f);
            Check(rgb(230/div,trackY,0,1,1) && rgb(400/div,trackY,1,0,0), "GPU renders separate Slider fill and track images");
            Check(pixel(230/div,250/div,1)<.1f, "Slider fill stays within the centered track and cannot cover controls below it");
            const bool buttonHover=fixture==1 || fixture==2, buttonPressed=fixture==3;
            Check(rgb(490/div,70/div,buttonHover || buttonPressed ? 0.0f : 1.0f,buttonHover ? 1.0f : 0.0f,buttonPressed ? 1.0f : 0.0f), "GPU button switches normal, hover and pressed textures at both viewport sizes");
            const unsigned hx=static_cast<unsigned>(sliderBounds.handle.x+sliderBounds.handle.width*.5f),hy=static_cast<unsigned>(sliderBounds.handle.y+sliderBounds.handle.height*.5f);
            Check(rgb(hx,hy,fixture>=4 ? 0.0f : 1.0f,fixture==5 ? 0.0f : 1.0f,fixture==4 ? 0.0f : 1.0f), "GPU Slider handle texture follows native hover and press");
            if (!fixture) { std::ofstream capture(std::filesystem::path(IX_UI_TEST_SHADER_ROOT)/"ui_canvas_gpu_preview.rgba32f",std::ios::binary); capture.write(reinterpret_cast<const char*>(values.data()),bufferBytes); }
        }
        InputEvent release; release.type=InputEvent::MouseUp; ui.OnInput(release);
        ui.SetGameDocumentVisible(handle,false); Check(!ui.HasVisibleGameDocuments(), "script can hide the scene Canvas");
        ui.SyncSceneCanvases({},2,IX_UI_TEST_ASSETS_ROOT); Check(!ui.FindSceneCanvas("HUD"), "scene switch removes stale Canvas documents");
        ui.Destroy(); view.Destroy(); std::error_code ec; std::filesystem::remove(imagePath,ec); for(const auto& path:skinPaths) std::filesystem::remove(path,ec);
    }();
    rhi->Shutdown(); rhi.reset(); native.Destroy(); window.Destroy();
}
}
#endif

int main(int argc, char** argv)
{
    std::cout << std::unitbuf; Model(); RmlLayout(); Persistence();
#if defined(IXTREEME_WITH_EDITOR)
    Editor();
#endif
#if defined(_WIN32)
    if (argc > 1 && std::string(argv[1]) == "--gpu") Gpu();
#endif
    return failures ? 1 : 0;
}
