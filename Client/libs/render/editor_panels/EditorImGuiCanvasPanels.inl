// Included inside the editor implementation. Runtime layout lives in UiCanvas.cpp.
void EditorImGui::EnsureUiCanvasState()
{
    auto& scenes = SceneManager::Instance();
    if (m_uiKnownRevision == scenes.UiRevision()) return;
    m_uiCanvases = scenes.GetCurrentScene().uiCanvases;
    m_uiKnownRevision = scenes.UiRevision();
    m_uiUndo.clear(); m_uiRedo.clear(); m_uiDragBefore.clear(); m_uiDragHandle = -1;
    if (!SelectedUiCanvas()) { m_uiSelectedCanvas = m_uiCanvases.empty() ? 0 : m_uiCanvases.front().id; m_uiSelectedElement = 0; }
}

UiCanvas* EditorImGui::SelectedUiCanvas()
{
    const auto it = std::find_if(m_uiCanvases.begin(), m_uiCanvases.end(), [&](const auto& c) { return c.id == m_uiSelectedCanvas; });
    return it == m_uiCanvases.end() ? nullptr : &*it;
}

void EditorImGui::CommitUiCanvasEdit(const std::vector<UiCanvas>& before, bool mergeWidget)
{
    for (auto& c : m_uiCanvases) SanitizeUiCanvas(c);
    if (before == m_uiCanvases) return;
    const auto widget = mergeWidget ? ImGui::GetActiveID() : 0;
    if (!widget || widget != m_uiLastEditWidget)
    {
        m_uiUndo.push_back(before);
        if (m_uiUndo.size() > 100) m_uiUndo.erase(m_uiUndo.begin());
    }
    m_uiLastEditWidget = widget; m_uiRedo.clear();
    SceneManager::Instance().SetUiCanvases(m_uiCanvases);
    m_uiKnownRevision = SceneManager::Instance().UiRevision();
}

void EditorImGui::UiCanvasUndo(bool redo)
{
    auto& from = redo ? m_uiRedo : m_uiUndo;
    auto& to = redo ? m_uiUndo : m_uiRedo;
    if (from.empty()) return;
    to.push_back(m_uiCanvases); m_uiCanvases = std::move(from.back()); from.pop_back();
    SceneManager::Instance().SetUiCanvases(m_uiCanvases);
    m_uiKnownRevision = SceneManager::Instance().UiRevision(); m_uiLastEditWidget = 0;
}

void EditorImGui::CreateUiCanvas(bool starterHud)
{
    if (!CanUseEditorTools() || !SceneManager::Instance().HasOpenScene()) return;
    EnsureUiCanvasState(); const auto before = m_uiCanvases;
    UiCanvas canvas;
    for (const auto& c : m_uiCanvases) canvas.id = std::max(canvas.id, c.id);
    ++canvas.id; canvas.name = "Canvas " + std::to_string(canvas.id);
    if (starterHud)
    {
        std::string name = "HUD";
        for (unsigned suffix = 2; std::any_of(m_uiCanvases.begin(), m_uiCanvases.end(), [&](const auto& c) { return c.name == name; }); ++suffix)
            name = "HUD " + std::to_string(suffix);
        canvas = MakeUiHudCanvas(canvas.id, name);
    }
    m_uiSelectedCanvas = canvas.id; m_uiSelectedElement = 0;
    m_uiCanvases.push_back(std::move(canvas)); CommitUiCanvasEdit(before);
    m_uiSelectionActive = m_inspectorPanelOpen = true;
    SetSceneView2D(true);
    m_pendingViewFocusWindow = EditorWindow::SceneView;
}

void EditorImGui::RenderUiCanvasHierarchy(bool tableRows)
{
    EnsureUiCanvasState();
    const auto row = [&]() { if (tableRows) { ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); } };
    std::uint32_t reparentCanvas = 0, reparentElement = 0, reparentTarget = 0;
    for (auto& c : m_uiCanvases)
    {
        ImGui::PushID(static_cast<int>(c.id)); row();
        const bool selected = m_uiSelectionActive && m_uiSelectedCanvas == c.id;
        const bool open = ImGui::TreeNodeEx((std::string(ICON_FA_WINDOW_MAXIMIZE) + "  " + c.name).c_str(),
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth |
            (selected && !m_uiSelectedElement ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        { m_uiSelectedCanvas = c.id; m_uiSelectedElement = 0; m_uiSelectionActive = m_inspectorPanelOpen = true; }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        { SetSceneView2D(true); m_pendingViewFocusWindow = EditorWindow::SceneView; }
        if (CanUseEditorTools() && ImGui::BeginDragDropTarget())
        {
            if (const auto* p = ImGui::AcceptDragDropPayload("IX_UI_ELEMENT"))
                if (p->DataSize == sizeof(std::uint32_t) * 2)
                { const auto* ids = static_cast<const std::uint32_t*>(p->Data); if (ids[0] == c.id) { reparentCanvas = c.id; reparentElement = ids[1]; } }
            ImGui::EndDragDropTarget();
        }
        const auto nodes = [&](auto&& self, std::uint32_t parent) -> void {
            for (auto& e : c.elements) if (e.parentId == parent)
            {
                row(); ImGui::PushID(static_cast<int>(e.id));
                const bool children = std::any_of(c.elements.begin(), c.elements.end(), [&](const auto& n) { return n.parentId == e.id; });
                const bool expanded = ImGui::TreeNodeEx(e.name.c_str(), ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth |
                    ImGuiTreeNodeFlags_DefaultOpen | (!children ? ImGuiTreeNodeFlags_Leaf : 0) |
                    (selected && m_uiSelectedElement == e.id ? ImGuiTreeNodeFlags_Selected : 0));
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                { m_uiSelectedCanvas = c.id; m_uiSelectedElement = e.id; m_uiSelectionActive = m_inspectorPanelOpen = true; }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                { SetSceneView2D(true); FitUiCanvasSelection(); m_pendingViewFocusWindow = EditorWindow::SceneView; }
                if (CanUseEditorTools() && ImGui::BeginDragDropSource())
                {
                    const std::uint32_t ids[] = {c.id, e.id}; ImGui::SetDragDropPayload("IX_UI_ELEMENT", ids, sizeof(ids));
                    ImGui::TextUnformatted(e.name.c_str()); ImGui::EndDragDropSource();
                }
                if (CanUseEditorTools() && ImGui::BeginDragDropTarget())
                {
                    if (const auto* p = ImGui::AcceptDragDropPayload("IX_UI_ELEMENT"))
                        if (p->DataSize == sizeof(std::uint32_t) * 2)
                        { const auto* ids = static_cast<const std::uint32_t*>(p->Data); if (ids[0] == c.id) { reparentCanvas = c.id; reparentElement = ids[1]; reparentTarget = e.id; } }
                    ImGui::EndDragDropTarget();
                }
                if (expanded) { self(self, e.id); ImGui::TreePop(); }
                ImGui::PopID();
            }
        };
        if (open) { nodes(nodes, 0); ImGui::TreePop(); }
        ImGui::PopID();
    }
    if (reparentCanvas)
    {
        const auto before = m_uiCanvases;
        for (auto& c : m_uiCanvases) if (c.id == reparentCanvas)
            ReparentUiElement(c, reparentElement, reparentTarget, m_uiPreviewSize[0], m_uiPreviewSize[1]);
        CommitUiCanvasEdit(before);
    }
}

void EditorImGui::RenderUiCanvasInspector()
{
    EnsureUiCanvasState(); auto* c = SelectedUiCanvas(); if (!c) return;
    ImGui::PushID("CanvasInspector"); ImGui::BeginDisabled(!CanUseEditorTools());
    ImGui::PushItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x * 0.45f));
    auto before = m_uiCanvases;
    auto* e = FindUiElement(*c, m_uiSelectedElement);
    const auto text = [](const char* label, std::string& value, bool multiline = false) {
        std::vector<char> buffer(std::max<std::size_t>(4096, value.size() + 256), 0); std::copy(value.begin(), value.end(), buffer.begin());
        const bool changed = multiline ? ImGui::InputTextMultiline(label, buffer.data(), buffer.size(), {-1, 80}) : ImGui::InputText(label, buffer.data(), buffer.size());
        if (changed) value = buffer.data(); return changed;
    };
    ImGui::TextUnformatted(e ? UiElementTypeName(e->type) : "Canvas / Screen Space Overlay");
    ImGui::Separator();
    if (ImGui::Button("Add UI element")) ImGui::OpenPopup("UiAddElement");
    if (ImGui::BeginPopup("UiAddElement"))
    {
        for (int type = 0; type < 8; ++type)
            if (ImGui::MenuItem(UiElementTypeName(static_cast<UiElementType>(type)))) AddUiCanvasElement(static_cast<UiElementType>(type));
        ImGui::EndPopup();
    }
    c = SelectedUiCanvas(); e = c ? FindUiElement(*c, m_uiSelectedElement) : nullptr;
    ImGui::SameLine(); if (ImGui::Button("Edit in 2D")) { SetSceneView2D(true); m_pendingViewFocusWindow = EditorWindow::SceneView; }
    if (ImGui::CollapsingHeader("Canvas tools", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::DragFloat2("Preview resolution", m_uiPreviewSize, 1, 64, 8192, "%.0f");
        if (ImGui::Button("1920 x 1080")) { m_uiPreviewSize[0] = 1920; m_uiPreviewSize[1] = 1080; }
        ImGui::SameLine(); if (ImGui::Button("Portrait")) { m_uiPreviewSize[0] = 1080; m_uiPreviewSize[1] = 1920; }
        ImGui::Checkbox("Snap 8 px", &m_uiSnap);
        const char* previewStates[]={"Normal","Hover","Pressed","Disabled"};
        ImGui::Combo("Control preview",&m_uiPreviewState,previewStates,4);
        if (ImGui::Button("Fit Canvas")) { m_uiZoom = 1; m_uiPan[0] = m_uiPan[1] = 0; }
        ImGui::SameLine(); if (ImGui::Button("Frame selection")) FitUiCanvasSelection();
        ImGui::BeginDisabled(m_uiUndo.empty()); if (ImGui::Button("Undo")) UiCanvasUndo(false); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(m_uiRedo.empty()); if (ImGui::Button("Redo")) UiCanvasUndo(true); ImGui::EndDisabled();
        c = SelectedUiCanvas(); e = c ? FindUiElement(*c, m_uiSelectedElement) : nullptr;
        if (!c) { ImGui::PopItemWidth(); ImGui::EndDisabled(); ImGui::PopID(); return; }
        ImGui::TextWrapped("Move and resize in Scene View > 2D. Wheel: zoom, middle mouse: pan. UI elements are in the Hierarchy.");
    }
    before = m_uiCanvases;
    if (!e)
    {
        text("Name", c->name); ImGui::Checkbox("Enabled", &c->enabled);
        ImGui::DragFloat2("Reference resolution", c->referenceResolution, 1, 1, 16384, "%.0f");
        const char* modes[] = {"Constant pixel size", "Scale with screen size"}; int mode = static_cast<int>(c->scaleMode);
        if (ImGui::Combo("Canvas Scaler", &mode, modes, 2)) c->scaleMode = static_cast<UiCanvas::ScaleMode>(mode);
        if (c->scaleMode == UiCanvas::ScaleMode::ScaleWithScreenSize)
            ImGui::SliderFloat("Match width / height", &c->matchWidthOrHeight, 0, 1);
        ImGui::DragInt("Sorting order", &c->sortingOrder, 1, -10000, 10000);
        ImGui::TextWrapped("Higher sorting orders render above other canvases. The Game view and packaged game use the same layout.");
        ImGui::Separator(); ImGui::TextWrapped("Scripts: UiOpen(\"canvas:%s\") returns this canvas's document. Elements use their Script ID.", c->name.c_str());
        if (ImGui::Button("Delete Canvas")) { const auto id = c->id; std::erase_if(m_uiCanvases, [id](const auto& n) { return n.id == id; }); m_uiSelectedCanvas = 0; }
    }
    else
    {
        text("Name", e->name); text("Script ID", e->scriptId); ImGui::Checkbox("Enabled", &e->enabled);
        if (ImGui::CollapsingHeader("Rect Transform", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Button("Anchor presets")) ImGui::OpenPopup("anchors");
            if (ImGui::BeginPopup("anchors"))
            {
                const auto layout = BuildUiCanvasLayout(*c, m_uiPreviewSize[0], m_uiPreviewSize[1]);
                UiLayoutRect bounds, parent{0, 0, 0, m_uiPreviewSize[0], m_uiPreviewSize[1]};
                for (const auto& r : layout) { if (r.id == e->id) bounds = r; if (e->parentId && r.id == e->parentId) parent = r; }
                for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x)
                {
                    if (x) ImGui::SameLine(); ImGui::PushID(y * 3 + x);
                    const char* names[] = {"Top left", "Top", "Top right", "Left", "Centre", "Right", "Bottom left", "Bottom", "Bottom right"};
                    if (ImGui::Button(names[y * 3 + x], {82, 26}))
                    {
                        e->rect.anchorMin[0] = e->rect.anchorMax[0] = x * 0.5f; e->rect.anchorMin[1] = e->rect.anchorMax[1] = y * 0.5f;
                        SetUiRectFromBounds(e->rect, bounds, parent, UiCanvasScale(*c, m_uiPreviewSize[0], m_uiPreviewSize[1])); ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopID();
                }
                if (ImGui::Button("Stretch to parent", {-1, 26}))
                { e->rect.anchorMin[0] = e->rect.anchorMin[1] = 0; e->rect.anchorMax[0] = e->rect.anchorMax[1] = 1; e->rect.position[0] = e->rect.position[1] = e->rect.sizeDelta[0] = e->rect.sizeDelta[1] = 0; ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
            ImGui::DragFloat2("Position", e->rect.position, 1); ImGui::DragFloat2("Size delta", e->rect.sizeDelta, 1);
            ImGui::DragFloat2("Anchor min", e->rect.anchorMin, 0.01f, 0, 1); ImGui::DragFloat2("Anchor max", e->rect.anchorMax, 0.01f, 0, 1);
            ImGui::DragFloat2("Pivot", e->rect.pivot, 0.01f, 0, 1);
            const char* parentName = "Canvas root"; if (const auto* p = FindUiElement(*c, e->parentId)) parentName = p->name.c_str();
            if (ImGui::BeginCombo("Parent", parentName))
            {
                if (ImGui::Selectable("Canvas root", !e->parentId)) ReparentUiElement(*c, e->id, 0, m_uiPreviewSize[0], m_uiPreviewSize[1]);
                for (const auto& p : c->elements) if (p.id != e->id)
                { ImGui::PushID(static_cast<int>(p.id)); if (ImGui::Selectable(p.name.c_str(), p.id == e->parentId)) ReparentUiElement(*c, e->id, p.id, m_uiPreviewSize[0], m_uiPreviewSize[1]); ImGui::PopID(); }
                ImGui::EndCombo();
            }
            ImGui::TextWrapped("Y grows downward. Stretched elements use Size delta as offsets from their anchor span.");
        }
        if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::ColorEdit4("Color", e->color); ImGui::DragFloat("Corner radius", &e->cornerRadius, 0.5f, 0, 1000);
            if (e->type == UiElementType::Text || e->type == UiElementType::Button || e->type == UiElementType::InputField)
            {
                text("Text", e->text, true); ImGui::ColorEdit4("Text color", e->textColor);
                ImGui::DragFloat("Font size", &e->fontSize, 1, 1, 512);
                const char* align[] = {"Left", "Centre", "Right"}; ImGui::Combo("Alignment", &e->textAlignment, align, 3);
            }
            if (ImGui::CollapsingHeader("Images / Skin", ImGuiTreeNodeFlags_DefaultOpen))
            {
                const auto imageSlot = [&](const char* label, std::string& path) {
                    ImGui::PushID(label);
                    text(label, path);
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const auto* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
                            if (const auto asset = m_assetLibrary ? m_assetLibrary->FindById(std::string(static_cast<const char*>(payload->Data), payload->DataSize)) : std::optional<AssetLibrary::Entry>{}; asset && asset->category == AssetLibrary::Category::Texture)
                                path = m_assetLibrary->AssetRelativePath(*asset);
                        ImGui::EndDragDropTarget();
                    }
                    if (ImGui::Button("Choose texture")) ImGui::OpenPopup("TexturePicker");
                    ImGui::SameLine(); if (ImGui::SmallButton("Clear")) path.clear();
                    if (ImGui::BeginPopup("TexturePicker"))
                    {
                        if (m_assetLibrary)
                            for (const auto& asset : m_assetLibrary->Entries()) if (asset.category == AssetLibrary::Category::Texture)
                            {
                                const auto relative=m_assetLibrary->AssetRelativePath(asset);
                                ImGui::PushID(asset.id.c_str());
                                if (ImGui::Selectable(asset.displayName.c_str(),path==relative)) path=relative;
                                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",relative.c_str());
                                ImGui::PopID();
                            }
                        ImGui::EndPopup();
                    }
                    if (!path.empty() && m_assetLibrary && m_textureProvider)
                        for (const auto& asset : m_assetLibrary->Entries()) if (asset.category==AssetLibrary::Category::Texture && m_assetLibrary->AssetRelativePath(asset)==path)
                        {
                            if (auto* preview=GetAssetPreviewTexture(asset); preview && preview->handle.IsValid())
                                if (auto* texture=m_textureProvider->GetPreviewTexture(preview->handle))
                                {
                                    const float fit=std::min(64.0f/std::max(1u,preview->width),48.0f/std::max(1u,preview->height));
                                    ImGui::Image(reinterpret_cast<ImTextureID>(texture),{preview->width*fit,preview->height*fit});
                                }
                            break;
                        }
                    ImGui::PopID();
                };
                imageSlot(e->type==UiElementType::Slider ? "Track image" : e->type==UiElementType::Image ? "Source image" : "Background image",e->imagePath);
                if (e->type==UiElementType::Button || e->type==UiElementType::Toggle || e->type==UiElementType::InputField || e->type==UiElementType::Slider)
                {
                    imageSlot(e->type==UiElementType::Slider ? "Handle hover image" : "Hover image",e->hoverImagePath);
                    imageSlot(e->type==UiElementType::Slider ? "Handle pressed image" : "Pressed image",e->pressedImagePath);
                    imageSlot(e->type==UiElementType::Slider ? "Handle disabled image" : "Disabled image",e->disabledImagePath);
                }
                if (e->type==UiElementType::Toggle) imageSlot("Checked image",e->checkedImagePath);
                if (e->type==UiElementType::Slider || e->type==UiElementType::ProgressBar) imageSlot("Fill image",e->fillImagePath);
                if (e->type==UiElementType::Slider)
                {
                    imageSlot("Handle image",e->handleImagePath);
                    ImGui::DragFloat("Track height",&e->sliderTrackHeight,1,1,1024);
                    ImGui::DragFloat2("Handle size",e->sliderHandleSize,1,1,1024);
                    ImGui::ColorEdit4("Handle color",e->handleColor);
                }
                ImGui::ColorEdit4("Image tint",e->imageTint);
                if (e->type==UiElementType::Toggle || e->type==UiElementType::Slider || e->type==UiElementType::ProgressBar) ImGui::ColorEdit4("Fill color",e->fillColor);
                ImGui::TextWrapped("Drop textures from the Asset Browser onto the image fields, or use Choose texture. Empty layers use colors. PNG alpha is supported.");
            }
            if (e->type == UiElementType::Slider || e->type == UiElementType::ProgressBar) ImGui::SliderFloat("Value", &e->value, 0, 1);
            if (e->type == UiElementType::Toggle) ImGui::Checkbox("Checked", &e->checked);
            ImGui::Checkbox("Clip children", &e->clipChildren);
        }
        ImGui::Checkbox("Interactable", &e->interactable); ImGui::Checkbox("Raycast target", &e->raycastTarget);
        ImGui::TextWrapped("Enable Raycast target for elements that should receive mouse input. Decorative elements let clicks reach the game.");
        if (ImGui::Button("Duplicate")) m_uiSelectedElement = DuplicateUiElement(*c, e->id);
        ImGui::SameLine(); if (ImGui::Button("Delete")) { DeleteUiElement(*c, m_uiSelectedElement); m_uiSelectedElement = 0; }
        if (auto* selected = FindUiElement(*c, m_uiSelectedElement))
        {
            auto it = std::find_if(c->elements.begin(), c->elements.end(), [&](const auto& n) { return n.id == selected->id; });
            if (ImGui::Button("Move backward") && it != c->elements.begin()) std::iter_swap(it, it - 1);
            // Re-find after swapping: the selected element may have moved.
            it = std::find_if(c->elements.begin(), c->elements.end(), [&](const auto& n) { return n.id == m_uiSelectedElement; });
            ImGui::SameLine(); if (ImGui::Button("Move forward") && it + 1 != c->elements.end()) std::iter_swap(it, it + 1);
        }
    }
    CommitUiCanvasEdit(before, true); ImGui::PopItemWidth(); ImGui::EndDisabled(); ImGui::PopID();
}

void EditorImGui::SetSceneView2D(bool enabled)
{
    if (m_sceneView2D == enabled) return;
    if (!m_uiDragBefore.empty())
    {
        const auto before = std::move(m_uiDragBefore); m_uiDragBefore.clear();
        CommitUiCanvasEdit(before); m_uiDragHandle = -1;
    }
    m_sceneView2D = enabled; m_sceneViewVisible = !enabled;
    m_sceneViewKeyboardFocus = false; m_sceneGizmoInputActive = false;
    m_viewportInputDiagnostics = {};
    if (enabled)
    {
        EnsureUiCanvasState();
        if (SelectedUiCanvas()) m_uiSelectionActive = m_inspectorPanelOpen = true;
    }
}

void EditorImGui::AddUiCanvasElement(UiElementType type)
{
    if (!CanUseEditorTools() || !SceneManager::Instance().HasOpenScene()) return;
    EnsureUiCanvasState();
    if (!SelectedUiCanvas()) CreateUiCanvas();
    auto* canvas = SelectedUiCanvas(); if (!canvas) return;
    const auto before = m_uiCanvases;
    m_uiSelectedElement = AddUiElement(*canvas, type, m_uiSelectionActive ? m_uiSelectedElement : 0).id;
    m_uiSelectionActive = m_inspectorPanelOpen = true;
    CommitUiCanvasEdit(before); SetSceneView2D(true);
}

void EditorImGui::FitUiCanvasSelection()
{
    const auto* canvas = SelectedUiCanvas(); if (!canvas || m_uiViewportSize[0] <= 48 || m_uiViewportSize[1] <= 48) return;
    UiLayoutRect bounds{0,0,0,m_uiPreviewSize[0],m_uiPreviewSize[1]};
    for (const auto& r : BuildUiCanvasLayout(*canvas, m_uiPreviewSize[0], m_uiPreviewSize[1]))
        if (r.id == m_uiSelectedElement) bounds = r;
    const float base = std::max(0.001f, std::min((m_uiViewportSize[0]-30)/m_uiPreviewSize[0],(m_uiViewportSize[1]-30)/m_uiPreviewSize[1]));
    const float fit = std::min((m_uiViewportSize[0]-48)/std::max(1.0f,bounds.width),(m_uiViewportSize[1]-48)/std::max(1.0f,bounds.height));
    m_uiZoom = std::clamp(fit/base,0.2f,8.0f);
    m_uiPan[0] = (m_uiPreviewSize[0]*.5f-bounds.x-bounds.width*.5f)*base*m_uiZoom;
    m_uiPan[1] = (m_uiPreviewSize[1]*.5f-bounds.y-bounds.height*.5f)*base*m_uiZoom;
}

void EditorImGui::HandleUiCanvasHotkeys()
{
    if (!m_uiSelectionActive || !CanUseEditorTools() || ImGui::GetIO().WantTextInput || IsTextInputActive()) return;
    auto* focus = ImGui::GetCurrentContext()->NavWindow;
    auto* scene = ImGui::FindWindowByName(EditorWindow::SceneView);
    const bool ownsKeys = focus && (focus->Name == std::string(EditorWindow::SceneView) ||
        (scene && ImGui::IsWindowChildOf(focus,scene,false,false)) || focus->Name == std::string(EditorWindow::Hierarchy) ||
        focus->Name == std::string(EditorWindow::Inspector));
    if (!ownsKeys || !ImGui::GetCurrentContext()->OpenPopupStack.empty()) return;
    EnsureUiCanvasState();
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z,false)) UiCanvasUndo(ImGui::GetIO().KeyShift);
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y,false)) UiCanvasUndo(true);
    auto* canvas = SelectedUiCanvas(); if (!canvas) return;
    if (m_uiSelectedElement && ImGui::IsKeyPressed(ImGuiKey_Delete,false))
    { const auto before=m_uiCanvases; DeleteUiElement(*canvas,m_uiSelectedElement); m_uiSelectedElement=0; CommitUiCanvasEdit(before); }
    if (m_uiSelectedElement && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D,false))
    { const auto before=m_uiCanvases; m_uiSelectedElement=DuplicateUiElement(*canvas,m_uiSelectedElement); CommitUiCanvasEdit(before); }
    if (m_sceneView2D && !ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F,false)) FitUiCanvasSelection();
}

void EditorImGui::RenderSceneViewToolbar()
{
    const auto modeButton = [&](const char* label,bool active,bool twoD) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label,{42,0})) SetSceneView2D(twoD);
        if (active) ImGui::PopStyleColor();
    };
    modeButton("3D",!m_sceneView2D,false); ImGui::SameLine(); modeButton("2D",m_sceneView2D,true);
    ImGui::SameLine(); ImGui::TextDisabled(m_sceneView2D ? "Canvas UI  |  Properties: Inspector" : "Scene");
    ImGui::Separator();
}

void EditorImGui::RenderUiCanvasSceneView()
{
    EnsureUiCanvasState(); const bool editing = CanUseEditorTools();
    ImGui::BeginChild("CanvasViewport",{0,-ImGui::GetTextLineHeightWithSpacing()},false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto stageOrigin = ImGui::GetCursorScreenPos(), stageSize = ImGui::GetContentRegionAvail();
    ImGui::GetWindowDrawList()->AddRectFilled(stageOrigin,{stageOrigin.x+stageSize.x,stageOrigin.y+stageSize.y},IM_COL32(17,20,26,255));
    auto* canvas = SelectedUiCanvas();
    if (canvas)
    {
        const auto origin = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
        m_uiViewportOrigin[0] = origin.x; m_uiViewportOrigin[1] = origin.y;
        m_uiViewportSize[0] = size.x; m_uiViewportSize[1] = size.y;
        const float factor = std::max(0.001f, std::min((size.x - 30) / m_uiPreviewSize[0], (size.y - 30) / m_uiPreviewSize[1])) * m_uiZoom;
        const ImVec2 top(origin.x + (size.x - m_uiPreviewSize[0] * factor) * 0.5f + m_uiPan[0], origin.y + (size.y - m_uiPreviewSize[1] * factor) * 0.5f + m_uiPan[1]);
        const auto screen = [&](float x, float y) { return ImVec2(top.x + x * factor, top.y + y * factor); };
        auto* dl = ImGui::GetWindowDrawList(); const auto end = screen(m_uiPreviewSize[0], m_uiPreviewSize[1]);
        dl->AddRectFilled(top, end, IM_COL32(25, 29, 36, 255)); dl->AddRect(top, end, IM_COL32(100, 110, 126, 255));
        const auto layout = BuildUiCanvasLayout(*canvas, m_uiPreviewSize[0], m_uiPreviewSize[1]);
        const float scale = UiCanvasScale(*canvas, m_uiPreviewSize[0], m_uiPreviewSize[1]);
        UiLayoutRect selected, parent{0, 0, 0, m_uiPreviewSize[0], m_uiPreviewSize[1]};
        auto* element = FindUiElement(*canvas, m_uiSelectedElement);
        for (const auto& r : layout) { if (r.id == m_uiSelectedElement) selected = r; if (element && element->parentId && r.id == element->parentId) parent = r; }
        const auto paint = [&](auto&& self, std::uint32_t parentId) -> void {
            for (std::size_t i = 0; i < canvas->elements.size(); ++i) if (canvas->elements[i].parentId == parentId)
            {
                const auto& e = canvas->elements[i]; const auto& r = layout[i]; if (!r.visible) continue;
                const auto a = screen(r.x, r.y), b = screen(r.x + r.width, r.y + r.height);
                const int state=e.id==m_uiSelectedElement ? m_uiPreviewState : 0;
                float tint[4]; for (int channel=0;channel<4;++channel) tint[channel]=e.imageTint[channel]*(e.type==UiElementType::Image ? e.color[channel] : 1.0f);
                const auto paintLayer=[&](const std::string& path,const UiLayoutRect& bounds,const float* fallback) {
                    const auto first=screen(bounds.x,bounds.y),last=screen(bounds.x+bounds.width,bounds.y+bounds.height);
                    if (!path.empty() && m_assetLibrary && m_textureProvider)
                        for (const auto& asset : m_assetLibrary->Entries()) if (asset.category==AssetLibrary::Category::Texture && m_assetLibrary->AssetRelativePath(asset)==path)
                        {
                            if (auto* preview=GetAssetPreviewTexture(asset); preview && preview->handle.IsValid())
                                if (auto* texture=m_textureProvider->GetPreviewTexture(preview->handle))
                                { dl->AddImage(reinterpret_cast<ImTextureID>(texture),first,last,{0,0},{1,1},ImGui::ColorConvertFloat4ToU32({tint[0],tint[1],tint[2],tint[3]})); return; }
                            break;
                        }
                    if (fallback) dl->AddRectFilled(first,last,ImGui::ColorConvertFloat4ToU32({fallback[0],fallback[1],fallback[2],fallback[3]}),e.cornerRadius*factor*scale);
                    else dl->AddRect(first,last,IM_COL32(145,160,185,255));
                };
                if (e.type==UiElementType::Slider)
                {
                    const auto parts=BuildUiSliderLayout(e,r,scale);
                    paintLayer(e.imagePath,parts.track,e.color);
                    paintLayer(e.fillImagePath,parts.fill,e.fillColor);
                    const auto& handle=(!e.interactable || state==3) && !e.disabledImagePath.empty() ? e.disabledImagePath :
                        state==2 && !e.pressedImagePath.empty() ? e.pressedImagePath : (state==1 || state==2) && !e.hoverImagePath.empty() ? e.hoverImagePath : e.handleImagePath;
                    paintLayer(handle,parts.handle,e.handleColor);
                }
                else
                {
                    paintLayer(UiElementPreviewImage(e,state),r,e.type==UiElementType::Image ? nullptr :
                        e.type==UiElementType::Toggle && e.checked ? e.fillColor : e.color);
                    if (e.type==UiElementType::ProgressBar)
                        paintLayer(e.fillImagePath,{0,r.x,r.y,r.width*e.value,r.height},e.fillColor);
                }
                if (!e.text.empty() && (e.type == UiElementType::Text || e.type == UiElementType::Button || e.type == UiElementType::InputField))
                {
                    const float fontSize = std::max(1.0f, e.fontSize * factor * scale);
                    auto* font = ImGui::GetFont(); const float wrapWidth = std::max(1.0f, b.x - a.x);
                    const auto textSize = font->CalcTextSizeA(fontSize, FLT_MAX, wrapWidth, e.text.c_str());
                    const float textX = e.textAlignment == 0 ? a.x : e.textAlignment == 2 ? b.x - textSize.x : (a.x + b.x - textSize.x) * 0.5f;
                    dl->PushClipRect(a, b, true); dl->AddText(font, fontSize, {textX, (a.y + b.y - textSize.y) * 0.5f},
                        ImGui::ColorConvertFloat4ToU32({e.textColor[0], e.textColor[1], e.textColor[2], e.textColor[3]}), e.text.c_str(), nullptr, wrapWidth); dl->PopClipRect();
                }
                if (e.clipChildren) dl->PushClipRect(a, b, true);
                self(self, e.id); if (e.clipChildren) dl->PopClipRect();
            }
        };
        dl->PushClipRect(top, end, true); paint(paint, 0); dl->PopClipRect();
        ImGui::SetCursorScreenPos(origin); ImGui::InvisibleButton("Canvas interaction", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered(); const auto mouse = ImGui::GetIO().MousePos;
        const float mx = (mouse.x - top.x) / factor, my = (mouse.y - top.y) / factor;
        const auto handles = [&](const UiLayoutRect& r) { return std::array<ImVec2, 8>{screen(r.x,r.y),screen(r.x+r.width*.5f,r.y),screen(r.x+r.width,r.y),screen(r.x+r.width,r.y+r.height*.5f),screen(r.x+r.width,r.y+r.height),screen(r.x+r.width*.5f,r.y+r.height),screen(r.x,r.y+r.height),screen(r.x,r.y+r.height*.5f)}; };
        if (element)
        {
            dl->AddRect(screen(selected.x,selected.y), screen(selected.x+selected.width,selected.y+selected.height), IM_COL32(72,190,255,255), 0.0f, ImDrawFlags_None, 2.0f);
            for (const auto& h : handles(selected)) dl->AddRectFilled({h.x-4,h.y-4},{h.x+4,h.y+4}, IM_COL32(72,190,255,255));
            dl->AddCircleFilled(screen(selected.x+selected.width*element->rect.pivot[0],selected.y+selected.height*element->rect.pivot[1]), 3, IM_COL32(255,180,70,255));
            for (int corner = 0; corner < 4; ++corner)
                dl->AddCircle(screen(parent.x + parent.width * (corner & 1 ? element->rect.anchorMax[0] : element->rect.anchorMin[0]),
                    parent.y + parent.height * (corner & 2 ? element->rect.anchorMax[1] : element->rect.anchorMin[1])), 4, IM_COL32(255,180,70,255));
        }
        if (hovered && ImGui::GetIO().MouseWheel != 0) m_uiZoom = std::clamp(m_uiZoom * std::pow(1.15f, ImGui::GetIO().MouseWheel), 0.2f, 8.0f);
        if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) { m_uiPan[0] += ImGui::GetIO().MouseDelta.x; m_uiPan[1] += ImGui::GetIO().MouseDelta.y; }
        if (editing && hovered && ImGui::IsMouseClicked(0))
        {
            m_uiDragHandle = -1;
            if (element) { int n = 0; for (const auto& h : handles(selected)) { if (std::abs(mouse.x-h.x) <= 7 && std::abs(mouse.y-h.y) <= 7) m_uiDragHandle = n; ++n; } }
            if (m_uiDragHandle < 0)
            {
                m_uiSelectedElement = HitTestUiCanvas(*canvas, layout, mx, my, m_uiPreviewSize[0], m_uiPreviewSize[1]);
                for (const auto& r : layout) if (r.id == m_uiSelectedElement) selected = r;
                m_uiDragHandle = 8;
            }
            m_uiSelectionActive = true;
            if (m_uiSelectedElement) { m_uiDragBefore = m_uiCanvases; m_uiDragRect = selected; m_uiDragMouse[0] = mx; m_uiDragMouse[1] = my; }
        }
        element = FindUiElement(*canvas, m_uiSelectedElement);
        if (editing && element && !m_uiDragBefore.empty() && ImGui::IsMouseDown(0))
        {
            auto bounds = m_uiDragRect; float dx = mx - m_uiDragMouse[0], dy = my - m_uiDragMouse[1];
            if (m_uiSnap && !ImGui::GetIO().KeyAlt) { dx = std::round(dx / (8 * scale)) * 8 * scale; dy = std::round(dy / (8 * scale)) * 8 * scale; }
            if (m_uiDragHandle == 8) { bounds.x += dx; bounds.y += dy; }
            else
            {
                if (m_uiDragHandle == 0 || m_uiDragHandle == 6 || m_uiDragHandle == 7) { dx = std::min(dx, bounds.width - 4 * scale); bounds.x += dx; bounds.width -= dx; }
                if (m_uiDragHandle == 2 || m_uiDragHandle == 3 || m_uiDragHandle == 4) bounds.width = std::max(4 * scale, bounds.width + dx);
                if (m_uiDragHandle == 0 || m_uiDragHandle == 1 || m_uiDragHandle == 2) { dy = std::min(dy, bounds.height - 4 * scale); bounds.y += dy; bounds.height -= dy; }
                if (m_uiDragHandle == 4 || m_uiDragHandle == 5 || m_uiDragHandle == 6) bounds.height = std::max(4 * scale, bounds.height + dy);
            }
            parent = {0,0,0,m_uiPreviewSize[0],m_uiPreviewSize[1]}; for (const auto& r : layout) if (element->parentId && r.id == element->parentId) parent = r;
            SetUiRectFromBounds(element->rect, bounds, parent, scale);
            SceneManager::Instance().SetUiCanvases(m_uiCanvases); m_uiKnownRevision = SceneManager::Instance().UiRevision();
        }
        if (!m_uiDragBefore.empty() && !ImGui::IsMouseDown(0))
        { const auto before = std::move(m_uiDragBefore); m_uiDragBefore.clear(); CommitUiCanvasEdit(before); m_uiDragHandle = -1; }
    }
    else
    {
        ImGui::TextWrapped("Create a Canvas, then add UI elements from Create > UI or the Inspector.");
        ImGui::BeginDisabled(!editing || !SceneManager::Instance().HasOpenScene());
        if (ImGui::Button("Create Canvas")) CreateUiCanvas();
        ImGui::SameLine(); if (ImGui::Button("HUD template")) CreateUiCanvas(true);
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Drag: move  |  Handles: resize  |  Wheel: zoom  |  Middle: pan  |  F: frame");
}
