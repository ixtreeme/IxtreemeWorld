// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

void EditorImGui::RenderGizmoControls()
{
    auto publish = [this]() {
        m_commands.gizmoSettingsChanged = true;
        m_commands.gizmoOperation = m_gizmoOperation;
        m_commands.gizmoSnapEnabled = m_gizmoSnapEnabled;
        m_commands.gizmoSnapValue = m_gizmoSnapValue;
    };

    ImGui::TextUnformatted("Gizmo");
    ImGui::SameLine();

    auto operationButton = [&](const char* label, const char* tooltip, MapEditorGizmoOperation operation, const char* trace) {
        const bool active = m_gizmoOperation == operation;
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.48f, 0.86f, 1.0f));
        if (ImGui::SmallButton(label))
        {
            m_gizmoOperation = operation;
            publish();
            Tracen(trace);
        }
        if (active)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
    };

    operationButton("W", "Object Gizmo", MapEditorGizmoOperation::Translate, "[EDITOR-GIZMO] Gizmo operation changed: object");
    operationButton("E", "Rotate", MapEditorGizmoOperation::Rotate, "[EDITOR-GIZMO] Gizmo operation changed: rotate");
    operationButton("R", "Scale", MapEditorGizmoOperation::Scale, "[EDITOR-GIZMO] Gizmo operation changed: scale");

    bool snapChanged = ImGui::Checkbox("Snap", &m_gizmoSnapEnabled);
    if (m_gizmoSnapEnabled)
    {
        ImGui::SameLine();
        const char* labels[] = {"0.1", "0.5", "1.0", "5.0"};
        ImGui::SetNextItemWidth(72.0f);
        snapChanged = ImGui::Combo("##GizmoSnap", &m_gizmoSnapIndex, labels, IM_ARRAYSIZE(labels)) || snapChanged;
        constexpr float values[] = {0.1f, 0.5f, 1.0f, 5.0f};
        m_gizmoSnapIndex = std::clamp(m_gizmoSnapIndex, 0, 3);
        m_gizmoSnapValue = values[m_gizmoSnapIndex];
    }
    if (snapChanged)
    {
        publish();
        Tracenf("[EDITOR-GIZMO] Snapping: enabled=%d value=%.2f",
            m_gizmoSnapEnabled ? 1 : 0,
            m_gizmoSnapValue);
    }
}

void EditorImGui::RenderWaterMaterialHeader()
{
    if (!m_assetLibrary)
        return;

    auto entry = m_assetLibrary->FindById(m_waterMaterialEditor.materialId);
    const std::string title = entry ? entry->displayName : m_waterMaterialEditor.materialId;
    UI::SectionHeader(ICON_FA_PALETTE " Water Material");
    ImGui::Text("Editing: %s%s", title.c_str(), m_waterMaterialEditor.dirty ? " *" : "");
    ImGui::InputText("Name", m_waterMaterialEditor.name, sizeof(m_waterMaterialEditor.name));

    if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Save"))
        SaveWaterMaterialEditor();
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_PLUS, "New Material"))
        ImGui::OpenPopup("NewWaterMaterialPopup");
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_TRASH, "Delete"))
        ImGui::OpenPopup("DeleteWaterMaterialConfirm");

    if (ImGui::BeginPopup("NewWaterMaterialPopup"))
    {
        ImGui::InputText("Name", m_waterMaterialEditor.newName, sizeof(m_waterMaterialEditor.newName));
        if (UI::IconButton(ICON_FA_CHECK, "Create"))
        {
            AssetLibrary::Entry created{};
            if (CreateWaterMaterialAsset(m_waterMaterialEditor.newName, created))
                OpenWaterMaterialEditor(created.id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (UI::IconButton(ICON_FA_XMARK, "Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("DeleteWaterMaterialConfirm"))
    {
        const auto usageIt = m_waterMaterialUsageCounts.find(m_waterMaterialEditor.materialId);
        const std::uint32_t users = usageIt != m_waterMaterialUsageCounts.end() ? usageIt->second : 0u;
        ImGui::Text("Delete '%s'?", title.c_str());
        if (users > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%u water bodies will use the default material.", users);
        if (UI::IconButton(ICON_FA_TRASH, "Yes, delete"))
        {
            DeleteWaterMaterialEditor();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (UI::IconButton(ICON_FA_XMARK, "Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Spacing();
    if (ImGui::BeginListBox("Water Materials", ImVec2(-1.0f, 110.0f)))
    {
        for (const AssetLibrary::Entry& material : m_assetLibrary->Entries())
        {
            if (material.category != AssetLibrary::Category::WaterMaterial)
                continue;
            const bool selected = material.id == m_waterMaterialEditor.materialId;
            if (ImGui::Selectable(material.displayName.c_str(), selected))
                OpenWaterMaterialEditor(material.id);
        }
        ImGui::EndListBox();
    }
}

void EditorImGui::RenderWaterMaterialColorsSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Colors", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    WaterConfig& config = material.config;
    if (ImGui::Checkbox("Water Enabled", &config.enabled)) MarkWaterMaterialChanged("enabled");
    if (ImGui::ColorEdit4("Base Color", config.baseColor)) MarkWaterMaterialChanged("base_color");
    if (ImGui::ColorEdit3("Deep Color", config.deepColor)) MarkWaterMaterialChanged("deep_color");
    if (ImGui::ColorEdit3("Shallow Color", config.shallowColor)) MarkWaterMaterialChanged("shallow_color");
    if (ImGui::SliderFloat("Color Depth Min", &config.depthColorMin, 0.0f, 50.0f, "%.2f m")) MarkWaterMaterialChanged("depth_color_min");
    if (ImGui::SliderFloat("Color Depth Max", &config.depthColorMax, 0.01f, 50.0f, "%.2f m")) MarkWaterMaterialChanged("depth_color_max");
    config.depthColorMax = std::max(config.depthColorMax, config.depthColorMin + 0.01f);
    if (ImGui::SliderFloat("Fade Distance", &config.depthFadeDistance, 0.01f, 50.0f, "%.2f m")) MarkWaterMaterialChanged("depth_fade_distance");
}

void EditorImGui::RenderWaterMaterialWaveSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Wave", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    WaterConfig& config = material.config;
    if (ImGui::SliderFloat("Wave Scale Small", &config.waveScaleSmall, 0.001f, 0.12f, "%.3f")) MarkWaterMaterialChanged("wave_scale_small");
    if (ImGui::SliderFloat("Wave Scale Large", &config.waveScaleLarge, 0.001f, 0.08f, "%.3f")) MarkWaterMaterialChanged("wave_scale_large");
    if (ImGui::SliderFloat("Wave Speed Small", &config.waveSpeedSmall, 0.0f, 0.5f, "%.3f")) MarkWaterMaterialChanged("wave_speed_small");
    if (ImGui::SliderFloat("Wave Speed Large", &config.waveSpeedLarge, 0.0f, 0.5f, "%.3f")) MarkWaterMaterialChanged("wave_speed_large");
    if (ImGui::SliderFloat("Normal Strength", &config.normalStrength, 0.0f, 2.0f, "%.2f")) MarkWaterMaterialChanged("normal_strength");
    if (ImGui::SliderFloat("Fresnel Power", &config.fresnelPower, 1.0f, 10.0f, "%.2f")) MarkWaterMaterialChanged("fresnel_power");
    if (ImGui::SliderFloat("Fresnel Min", &config.fresnelMin, 0.0f, 0.5f, "%.3f")) MarkWaterMaterialChanged("fresnel_min");
}

void EditorImGui::RenderWaterMaterialFoamSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Foam"))
        return;
    WaterConfig& config = material.config;
    if (ImGui::Checkbox("Foam Enabled", &config.foamEnabled)) MarkWaterMaterialChanged("foam_enabled");
    if (!config.foamEnabled)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Foam Distance", &config.foamDistance, 0.02f, 1.5f, "%.3f m")) MarkWaterMaterialChanged("foam_distance");
    if (ImGui::SliderFloat("Foam Softness", &config.foamSoftness, 0.0f, 0.5f, "%.3f")) MarkWaterMaterialChanged("foam_softness");
    if (ImGui::SliderFloat("Foam Intensity", &config.foamIntensity, 0.0f, 2.0f, "%.2f")) MarkWaterMaterialChanged("foam_intensity");
    if (ImGui::SliderFloat("Foam Scroll Speed", &config.foamScrollSpeed, -1.0f, 1.0f, "%.3f")) MarkWaterMaterialChanged("foam_scroll_speed");
    if (ImGui::SliderFloat("Foam Scale", &config.foamScale, 0.1f, 2.0f, "%.2f")) MarkWaterMaterialChanged("foam_scale");
    if (ImGui::SliderFloat("Terrain Foam Thickness", &config.foamTerrainThickness, 0.0f, 1.0f, "%.3f m")) MarkWaterMaterialChanged("foam_terrain_thickness");
    if (!config.foamEnabled)
        ImGui::EndDisabled();
}

void EditorImGui::RenderWaterMaterialCausticSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Caustic"))
        return;
    WaterConfig& config = material.config;
    const char* modes[] = {"Off", "Animated", "Procedural"};
    int mode = static_cast<int>(config.causticMode);
    if (ImGui::Combo("Caustic Mode", &mode, modes, IM_ARRAYSIZE(modes)))
    {
        config.causticMode = static_cast<WaterConfig::CausticMode>(std::clamp(mode, 0, 2));
        MarkWaterMaterialChanged("caustic_mode");
    }
    if (config.causticMode == WaterConfig::CausticMode::Off)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Caustic Intensity", &config.causticIntensity, 0.0f, 3.0f, "%.2f")) MarkWaterMaterialChanged("caustic_intensity");
    if (ImGui::SliderFloat("Caustic Scale", &config.causticScale, 0.1f, 2.0f, "%.2f m")) MarkWaterMaterialChanged("caustic_scale");
    if (ImGui::SliderFloat("Caustic Speed", &config.causticSpeed, 0.0f, 2.0f, "%.2f")) MarkWaterMaterialChanged("caustic_speed");
    if (ImGui::SliderFloat("Caustic Max Depth", &config.causticMaxDepth, 1.0f, 30.0f, "%.1f m")) MarkWaterMaterialChanged("caustic_max_depth");
    if (config.causticMode == WaterConfig::CausticMode::Off)
        ImGui::EndDisabled();
}

void EditorImGui::RenderWaterMaterialReflectionSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Reflection"))
        return;
    WaterConfig& config = material.config;
    if (ImGui::Checkbox("Reflection Enabled", &config.reflectionEnabled)) MarkWaterMaterialChanged("reflection_enabled");
    if (!config.reflectionEnabled)
        ImGui::BeginDisabled();
    if (ImGui::ColorEdit3("Reflection Color", config.reflectionColor)) MarkWaterMaterialChanged("reflection_color");
    const char* qualities[] = {"Low", "Medium", "High"};
    int quality = static_cast<int>(config.reflectionQuality);
    if (ImGui::Combo("Reflection Quality", &quality, qualities, IM_ARRAYSIZE(qualities)))
    {
        config.reflectionQuality = static_cast<WaterConfig::ReflectionQuality>(std::clamp(quality, 0, 2));
        MarkWaterMaterialChanged("reflection_quality");
    }
    if (ImGui::SliderFloat("Distortion Strength", &config.reflectionDistortionStrength, 0.0f, 0.2f, "%.3f")) MarkWaterMaterialChanged("reflection_distortion_strength");
    if (!config.reflectionEnabled)
        ImGui::EndDisabled();
}

void EditorImGui::RenderWaterMaterialRefractionSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Refraction"))
        return;
    WaterConfig& config = material.config;
    if (ImGui::Checkbox("Refraction Enabled", &config.refractionEnabled)) MarkWaterMaterialChanged("refraction_enabled");
    if (!config.refractionEnabled)
        ImGui::BeginDisabled();
    if (ImGui::SliderFloat("Refraction Strength", &config.refractionStrength, 0.0f, 0.1f, "%.3f")) MarkWaterMaterialChanged("refraction_strength");
    if (ImGui::SliderFloat("Depth Multiplier", &config.refractionDepthStrength, 0.0f, 2.0f, "%.2f")) MarkWaterMaterialChanged("refraction_depth_strength");
    if (!config.refractionEnabled)
        ImGui::EndDisabled();
}

void EditorImGui::RenderWaterMaterialEdgeFadeSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Edge Fade", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    WaterConfig& config = material.config;
    if (ImGui::SliderFloat("Edge Fade Distance", &config.edgeFadeDistance, 0.0f, 3.0f, "%.2f m")) MarkWaterMaterialChanged("edge_fade_distance");
    const char* curves[] = {"Linear", "Smooth", "Exponential"};
    int curve = static_cast<int>(config.edgeFadeCurve);
    if (ImGui::Combo("Edge Fade Curve", &curve, curves, IM_ARRAYSIZE(curves)))
    {
        config.edgeFadeCurve = static_cast<WaterConfig::EdgeFadeCurve>(std::clamp(curve, 0, 2));
        MarkWaterMaterialChanged("edge_fade_curve");
    }
}

void EditorImGui::RenderWaterTextureSlot(const char* label, std::string& texturePath, bool& changed)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    std::string display = texturePath.empty() ? std::string("empty") : texturePath;
    if (m_assetLibrary)
    {
        for (const AssetLibrary::Entry& entry : m_assetLibrary->Entries())
        {
            if (entry.category == AssetLibrary::Category::Texture && m_assetLibrary->AssetRelativePath(entry) == texturePath)
            {
                display = entry.displayName;
                break;
            }
        }
    }

    ImGui::Button("##texture_slot", ImVec2(70.0f, 70.0f));
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
            if (entry && entry->category == AssetLibrary::Category::Texture)
            {
                texturePath = m_assetLibrary->AssetRelativePath(*entry);
                changed = true;
                Tracenf("[EDITOR-IMGUI-4] Texture slot assigned: material_id=%s slot=%s asset_id=%s",
                    m_waterMaterialEditor.materialId.c_str(),
                    label,
                    entry->id.c_str());
            }
            else
            {
                m_assetStatus = "Texture slot accepts texture assets only";
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", display.c_str());
    if (!texturePath.empty() && ImGui::SmallButton("Clear"))
    {
        texturePath.clear();
        changed = true;
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

void EditorImGui::RenderWaterMaterialTexturesSection(WaterMaterialData& material)
{
    if (!ImGui::CollapsingHeader("Textures"))
        return;

    bool changed = false;
    RenderWaterTextureSlot("Normal Map A", material.normalMapA, changed);
    RenderWaterTextureSlot("Normal Map B", material.normalMapB, changed);
    RenderWaterTextureSlot("Diffuse Map", material.diffuseMap, changed);
    if (changed)
        MarkWaterMaterialChanged("texture_slot");

    if (ImGui::SliderFloat2("Scroll Speed A", material.scrollSpeedA, -1.0f, 1.0f, "%.3f")) MarkWaterMaterialChanged("scroll_speed_a");
    if (ImGui::SliderFloat2("Scroll Speed B", material.scrollSpeedB, -1.0f, 1.0f, "%.3f")) MarkWaterMaterialChanged("scroll_speed_b");
    if (ImGui::SliderFloat("Normal Tiling", &material.normalTiling, 0.1f, 20.0f, "%.2f")) MarkWaterMaterialChanged("normal_tiling");
}

void EditorImGui::RenderWaterMaterialEditor()
{
    if (!m_editorModeActive || !m_waterMaterialEditor.windowOpen)
        return;

    if (ImGui::Begin("Water Material Editor", &m_waterMaterialEditor.windowOpen))
    {
        const bool toolsEnabled = CanUseEditorTools();
        if (!toolsEnabled)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.30f, 1.0f), "Read-only during Play Mode");
            ImGui::BeginDisabled();
        }
        if (m_waterMaterialEditor.materialId.empty())
        {
            ImGui::TextDisabled("No water material loaded.");
            if (!toolsEnabled)
                ImGui::EndDisabled();
            ImGui::End();
            return;
        }

        RenderWaterMaterialHeader();
        ImGui::Separator();
        WaterMaterialData& material = m_waterMaterialEditor.draft;
        RenderWaterMaterialColorsSection(material);
        RenderWaterMaterialWaveSection(material);
        RenderWaterMaterialFoamSection(material);
        RenderWaterMaterialCausticSection(material);
        RenderWaterMaterialReflectionSection(material);
        RenderWaterMaterialRefractionSection(material);
        RenderWaterMaterialEdgeFadeSection(material);
        RenderWaterMaterialTexturesSection(material);
        if (!toolsEnabled)
            ImGui::EndDisabled();
    }
    ImGui::End();
}

void EditorImGui::RenderPbrMaterialHeader()
{
    UI::SectionHeader(ICON_FA_PALETTE " PBR Material");
    ImGui::Text("Editing: %s%s", m_pbrMaterialEditor.name, m_pbrMaterialEditor.dirty ? " *" : "");
    ImGui::InputText("Name", m_pbrMaterialEditor.name, sizeof(m_pbrMaterialEditor.name));
    if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Save"))
        SavePbrMaterialEditor();
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_PLUS, "New Material"))
        CreatePbrMaterialAsset();
}

void EditorImGui::RenderPbrTextureSlot(const char* label, std::string& textureId, bool& changed)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    std::string display = "(empty)";
    if (m_assetLibrary && !textureId.empty())
    {
        auto entry = m_assetLibrary->FindById(textureId);
        if (entry)
            display = entry->displayName;
    }

    ImGui::Button("##pbr_texture_slot", ImVec2(70.0f, 70.0f));
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
            if (entry && entry->category == AssetLibrary::Category::Texture)
            {
                textureId = entry->id;
                changed = true;
                Tracenf("[EDITOR-IMGUI-4] Texture slot assigned: material_id=%s slot=%s asset_id=%s",
                    m_pbrMaterialEditor.materialId.c_str(),
                    label,
                    entry->id.c_str());
            }
            else
            {
                m_assetStatus = "PBR texture slot accepts texture assets only";
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", display.c_str());
    if (!textureId.empty() && ImGui::SmallButton("Clear"))
    {
        textureId.clear();
        changed = true;
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

void EditorImGui::RenderPbrMaterialEditor()
{
    if (!m_editorModeActive || !m_pbrMaterialEditor.windowOpen)
        return;

    if (ImGui::Begin("PBR Material Editor", &m_pbrMaterialEditor.windowOpen))
    {
        const bool toolsEnabled = CanUseEditorTools();
        if (!toolsEnabled)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.30f, 1.0f), "Read-only during Play Mode");
            ImGui::BeginDisabled();
        }
        RenderPbrMaterialHeader();
        ImGui::Separator();
        AssetLibrary::MaterialData& material = m_pbrMaterialEditor.draft;
        int shadingModeIndex = ToLowerAscii(material.shadingMode) == "unlit" ? 1 : 0;
        const char* shadingModes[] = {"Lit", "Unlit"};
        if (ImGui::Combo("Shading Mode", &shadingModeIndex, shadingModes, IM_ARRAYSIZE(shadingModes)))
        {
            material.shadingMode = shadingModeIndex == 1 ? "unlit" : "lit";
            MarkPbrMaterialChanged("shading_mode");
            Tracenf("[MATERIAL] editor shadingMode=%s material_id=%s",
                shadingModeIndex == 1 ? "Unlit" : "Lit",
                m_pbrMaterialEditor.materialId.c_str());
        }
        const bool unlitMode = shadingModeIndex == 1;
        if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool changed = false;
            RenderPbrTextureSlot("Diffuse / Albedo", material.diffuseTextureId, changed);
            if (unlitMode)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
            RenderPbrTextureSlot("Normal Map", material.normalTextureId, changed);
            RenderPbrTextureSlot("AO", material.aoTextureId, changed);
            RenderPbrTextureSlot("Roughness", material.roughnessTextureId, changed);
            RenderPbrTextureSlot("Metallic", material.metallicTextureId, changed);
            if (unlitMode)
                ImGui::PopStyleVar();
            RenderPbrTextureSlot("Height", material.heightTextureId, changed);
            if (changed)
                MarkPbrMaterialChanged("texture_slot");
        }
        if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::SliderFloat("Tiling X", &material.tilingScaleX, 0.01f, 32.0f, "%.2f")) MarkPbrMaterialChanged("tiling_x");
            if (ImGui::SliderFloat("Tiling Y", &material.tilingScaleY, 0.01f, 32.0f, "%.2f")) MarkPbrMaterialChanged("tiling_y");
            if (ImGui::ColorEdit3("Tint", material.colorTint)) MarkPbrMaterialChanged("color_tint");
            if (unlitMode)
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
            if (ImGui::SliderFloat("Normal Strength", &material.normalStrength, 0.0f, 2.0f, "%.2f")) MarkPbrMaterialChanged("normal_strength");
            if (ImGui::SliderFloat("AO Strength", &material.aoStrength, 0.0f, 2.0f, "%.2f")) MarkPbrMaterialChanged("ao_strength");
            if (ImGui::SliderFloat("Roughness Strength", &material.roughnessStrength, 0.0f, 2.0f, "%.2f")) MarkPbrMaterialChanged("roughness_strength");
            if (ImGui::SliderFloat("Metallic Strength", &material.metallicStrength, 0.0f, 2.0f, "%.2f")) MarkPbrMaterialChanged("metallic_strength");
            if (unlitMode)
                ImGui::PopStyleVar();
        }
        if (ImGui::CollapsingHeader("Transparency", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int alphaModeIndex = ToLowerAscii(material.alphaMode) == "mask" ? 1 :
                (ToLowerAscii(material.alphaMode) == "blend" ? 2 : 0);
            const char* alphaModes[] = {"OPAQUE", "MASK", "BLEND"};
            if (ImGui::Combo("Alpha Mode", &alphaModeIndex, alphaModes, IM_ARRAYSIZE(alphaModes)))
            {
                material.alphaMode = alphaModeIndex == 1 ? "mask" : (alphaModeIndex == 2 ? "blend" : "opaque");
                MarkPbrMaterialChanged("alpha_mode");
            }
            if (alphaModeIndex == 1)
            {
                if (ImGui::SliderFloat("Alpha Cutoff", &material.alphaCutoff, 0.0f, 1.0f, "%.3f"))
                    MarkPbrMaterialChanged("alpha_cutoff");
            }
        }
        if (!toolsEnabled)
            ImGui::EndDisabled();
    }
    ImGui::End();
}

void EditorImGui::RenderSelectedTerrainInspector()
{
    if (!m_terrainState.selected)
        return;

    UI::SectionHeader(ICON_FA_MOUNTAIN " Terrain");
    ImGui::TextDisabled("flecs=%llu  object=1",
        static_cast<unsigned long long>(m_selectedHierarchyEntity));
    ImGui::Text("Name: %s", m_terrainState.name.c_str());
    ImGui::Separator();
    ImGui::Text("Size: %.2f m x %.2f m",
        m_terrainState.widthMeters,
        m_terrainState.depthMeters);
    ImGui::Text("Cell size: %.2f m/cell", m_terrainState.cellSizeMeters);
    ImGui::Text("Cells: %u x %u", m_terrainState.cellsX, m_terrainState.cellsZ);
    const std::uint64_t verts =
        static_cast<std::uint64_t>(m_terrainState.cellsX + 1u) *
        static_cast<std::uint64_t>(m_terrainState.cellsZ + 1u);
    ImGui::Text("Vertices: %llu", static_cast<unsigned long long>(verts));
}

bool EditorImGui::RenderSelectedPhysicsMaterialAssetInspector()
{
    if (!m_assetInspectorSelectionActive || !m_assetLibrary || m_selectedAssetId.empty())
        return false;

    const auto entry = m_assetLibrary->FindById(m_selectedAssetId);
    if (!entry || entry->category != AssetLibrary::Category::PhysicsMaterial)
        return false;

    AssetLibrary::PhysicsMaterialData material = entry->physicsMaterial;
    bool changed = false;

    UI::SectionHeader(ICON_FA_GEAR " Physics Material Asset");
    ImGui::TextDisabled("Asset ID: %s", entry->id.c_str());
    ImGui::TextDisabled("File: %s", m_assetLibrary->AbsolutePath(*entry).generic_string().c_str());
    ImGui::Separator();

    changed |= ImGui::SliderFloat("Friction", &material.friction, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Bounciness", &material.restitution, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::DragFloat("Density", &material.density, 0.05f, 0.001f, 1000.0f, "%.3f");
    changed |= ImGui::DragFloat("Linear Damping", &material.linearDamping, 0.01f, 0.0f, 100.0f, "%.3f");
    changed |= ImGui::DragFloat("Angular Damping", &material.angularDamping, 0.01f, 0.0f, 100.0f, "%.3f");
    auto combineCombo = [](const char* label, ixtreeme::physics::PhysicsMaterialCombineMode& mode) {
        const char* modes[] = {"Average", "Minimum", "Maximum", "Multiply"};
        int index = mode == ixtreeme::physics::PhysicsMaterialCombineMode::Minimum ? 1 :
            (mode == ixtreeme::physics::PhysicsMaterialCombineMode::Maximum ? 2 :
            (mode == ixtreeme::physics::PhysicsMaterialCombineMode::Multiply ? 3 : 0));
        if (!ImGui::Combo(label, &index, modes, IM_ARRAYSIZE(modes)))
            return false;
        mode = index == 1 ? ixtreeme::physics::PhysicsMaterialCombineMode::Minimum :
            (index == 2 ? ixtreeme::physics::PhysicsMaterialCombineMode::Maximum :
            (index == 3 ? ixtreeme::physics::PhysicsMaterialCombineMode::Multiply :
                ixtreeme::physics::PhysicsMaterialCombineMode::Average));
        return true;
    };
    changed |= combineCombo("Friction Combine", material.frictionCombine);
    changed |= combineCombo("Bounce Combine", material.restitutionCombine);

    if (changed)
    {
        AssetLibrary::Entry updated{};
        std::string error;
        if (m_assetLibrary->UpdatePhysicsMaterial(entry->id, material, updated, error))
        {
            m_assetStatus = "Physics material saved: " + updated.displayName;
            Tracenf("[PHYSICS-MAT] inspector updated id=%s friction=%.2f bounce=%.2f density=%.3f damping=(%.3f,%.3f) combine=(%s,%s)",
                updated.id.c_str(),
                updated.physicsMaterial.friction,
                updated.physicsMaterial.restitution,
                updated.physicsMaterial.density,
                updated.physicsMaterial.linearDamping,
                updated.physicsMaterial.angularDamping,
                ixtreeme::physics::ToString(updated.physicsMaterial.frictionCombine),
                ixtreeme::physics::ToString(updated.physicsMaterial.restitutionCombine));
        }
        else
        {
            m_assetStatus = "Physics material save failed: " + error;
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.28f, 1.0f), "%s", m_assetStatus.c_str());
        }
    }

    ImGui::TextWrapped("Assigned colliders use these values when the Play physics world is built. Combine modes control how this material mixes with the other collider in a contact.");
    return true;
}

bool EditorImGui::RenderSelectedPrefabAssetInspector()
{
    if (!m_assetInspectorSelectionActive || !m_assetLibrary || m_selectedAssetId.empty())
        return false;

    const auto entry = m_assetLibrary->FindById(m_selectedAssetId);
    if (!entry || entry->category != AssetLibrary::Category::Prefab)
        return false;

    const std::filesystem::path path = entry->originalPath.empty()
        ? m_assetLibrary->AbsolutePath(*entry)
        : std::filesystem::path(entry->originalPath);

    std::string prefabName;
    std::vector<PrefabInspectorEntity> entities;
    std::string error;
    const bool loaded = ReadPrefabAssetForInspector(path, prefabName, entities, error);
    if (prefabName.empty())
        prefabName = entry->displayName.empty() ? entry->filename : entry->displayName;
    auto resetEditState = [&]() {
        m_prefabInspectorEditAssetId = entry->id;
        CopyToBuffer(m_prefabInspectorNameBuffer, sizeof(m_prefabInspectorNameBuffer), prefabName);
        m_prefabInspectorEditRows.clear();
        m_prefabInspectorEditRows.reserve(entities.size());
        for (const PrefabInspectorEntity& entity : entities)
        {
            PrefabInspectorEditRow row{};
            row.localId = entity.localId;
            row.parentLocalId = entity.parentLocalId;
            row.type = entity.type;
            row.lightType = entity.lightType;
            row.meshAssetId = entity.meshAssetId;
            row.meshAssetPath = entity.meshAssetPath;
            row.prefabAssetId = entity.prefabAssetId;
            row.materialSlotBuffers.clear();
            row.materialSlotBuffers.reserve(entity.materialSlots.size());
            for (const std::string& materialSlot : entity.materialSlots)
            {
                std::array<char, 128> buffer{};
                CopyToBuffer(buffer.data(), buffer.size(), materialSlot);
                row.materialSlotBuffers.push_back(buffer);
            }
            CopyToBuffer(row.name, sizeof(row.name), entity.name);
            row.transformValid = entity.transformValid;
            std::copy(std::begin(entity.position), std::end(entity.position), std::begin(row.position));
            std::copy(std::begin(entity.rotation), std::end(entity.rotation), std::begin(row.rotation));
            std::copy(std::begin(entity.scale), std::end(entity.scale), std::begin(row.scale));
            row.lightValid = entity.lightValid;
            std::copy(std::begin(entity.color), std::end(entity.color), std::begin(row.color));
            row.intensity = entity.intensity;
            row.radius = entity.radius;
            row.innerConeDegrees = entity.innerConeDegrees;
            row.outerConeDegrees = entity.outerConeDegrees;
            row.enabled = entity.enabled;
            m_prefabInspectorEditRows.push_back(std::move(row));
        }
        m_prefabInspectorDirty = false;
    };
    if (loaded &&
        (m_prefabInspectorEditAssetId != entry->id ||
            (!m_prefabInspectorDirty && m_prefabInspectorEditRows.size() != entities.size())))
    {
        resetEditState();
    }

    UI::SectionHeader(ICON_FA_LAYER_GROUP " Prefab Asset");
    ImGui::TextDisabled("Asset ID: %s", entry->id.c_str());
    ImGui::TextDisabled("File: %s", path.generic_string().c_str());
    ImGui::Separator();

    if (!loaded)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.28f, 1.0f), "Failed to read prefab: %s", error.c_str());
        return true;
    }

    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("Prefab Name", m_prefabInspectorNameBuffer, sizeof(m_prefabInspectorNameBuffer)))
        m_prefabInspectorDirty = true;

    ImGui::Text("Entities: %zu", entities.size());
    if (UI::IconButton(ICON_FA_CUBE, "Instantiate Prefab", ImVec2(-1.0f, 0.0f)))
    {
        m_commands.addPrefabInstance = true;
        m_commands.prefabAssetId = entry->id;
        m_assetStatus = "Prefab instance queued: " + entry->displayName;
        Tracenf("[PREFAB] Inspector instantiate queued: asset_id=%s", entry->id.c_str());
    }
    auto nextPrefabLocalId = [&]() {
        std::uint32_t nextId = 1;
        for (const PrefabInspectorEditRow& row : m_prefabInspectorEditRows)
            nextId = std::max(nextId, row.localId + 1u);
        return nextId;
    };
    auto addPrefabLightRow = [&](const char* lightType, const char* displayName) {
        PrefabInspectorEditRow row{};
        row.localId = nextPrefabLocalId();
        row.type = "dynamic_light";
        row.lightType = lightType;
        row.transformValid = true;
        row.lightValid = true;
        CopyToBuffer(row.name, sizeof(row.name), displayName);
        row.position[1] = 2.0f;
        row.intensity = std::strcmp(lightType, "spot") == 0 ? 5.0f : 3.0f;
        row.radius = std::strcmp(lightType, "spot") == 0 ? 20.0f : 10.0f;
        row.rotation[0] = std::strcmp(lightType, "spot") == 0 ? -1.5708f : 0.0f;
        m_prefabInspectorEditRows.push_back(row);
        m_prefabInspectorDirty = true;
    };
    if (UI::IconButton(ICON_FA_LIGHTBULB, "Add Point Light", ImVec2(-1.0f, 0.0f)))
        addPrefabLightRow("point", "Point Light");
    if (UI::IconButton(ICON_FA_BULLSEYE, "Add Spot Light", ImVec2(-1.0f, 0.0f)))
        addPrefabLightRow("spot", "Spot Light");
    if (m_prefabInspectorDirty)
        ImGui::TextColored(ImVec4(0.96f, 0.77f, 0.28f, 1.0f), "Unsaved prefab edits");

    auto findRowByLocalId = [&](std::uint32_t localId) -> PrefabInspectorEditRow* {
        auto it = std::find_if(m_prefabInspectorEditRows.begin(), m_prefabInspectorEditRows.end(),
            [&](const PrefabInspectorEditRow& row) { return row.localId == localId; });
        return it == m_prefabInspectorEditRows.end() ? nullptr : &*it;
    };
    auto wouldCreateParentCycle = [&](std::uint32_t localId, std::uint32_t parentLocalId) {
        std::set<std::uint32_t> visited;
        std::uint32_t cursor = parentLocalId;
        while (cursor != 0)
        {
            if (cursor == localId || !visited.insert(cursor).second)
                return true;
            PrefabInspectorEditRow* parent = findRowByLocalId(cursor);
            if (!parent)
                return false;
            cursor = parent->parentLocalId;
        }
        return false;
    };
    std::vector<std::string> validationErrors;
    std::set<std::uint32_t> seenLocalIds;
    for (const PrefabInspectorEditRow& row : m_prefabInspectorEditRows)
    {
        if (row.localId == 0 || !seenLocalIds.insert(row.localId).second)
            validationErrors.push_back("Duplicate or missing local id in prefab hierarchy.");
        if (row.parentLocalId != 0 && !findRowByLocalId(row.parentLocalId))
            validationErrors.push_back("Entity " + std::to_string(row.localId) + " has a missing parent.");
        if (wouldCreateParentCycle(row.localId, row.parentLocalId))
            validationErrors.push_back("Entity " + std::to_string(row.localId) + " would create a parent cycle.");
        if (row.type == "mesh_entity" && row.meshAssetId.empty() && row.meshAssetPath.empty())
            validationErrors.push_back("Mesh entity " + std::to_string(row.localId) + " has no mesh asset.");
    }
    if (!validationErrors.empty())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.28f, 1.0f), "Prefab validation failed:");
        for (const std::string& errorText : validationErrors)
            ImGui::BulletText("%s", errorText.c_str());
    }
    const bool canSave = m_prefabInspectorDirty && m_prefabInspectorNameBuffer[0] != '\0' && validationErrors.empty();
    if (!canSave)
        ImGui::BeginDisabled();
    if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Save Prefab Asset", ImVec2(-1.0f, 0.0f)))
    {
        m_commands.savePrefabAssetEdit = true;
        m_commands.editPrefabAssetId = entry->id;
        m_commands.editPrefabName = m_prefabInspectorNameBuffer;
        m_commands.editPrefabEntityNames.clear();
        for (const PrefabInspectorEditRow& row : m_prefabInspectorEditRows)
        {
            PrefabAssetEntityNameEdit edit{};
            edit.localId = row.localId;
            edit.parentLocalId = row.parentLocalId;
            edit.type = row.type;
            edit.lightType = row.lightType;
            edit.name = row.name;
            edit.meshAssetId = row.meshAssetId;
            edit.meshAssetPath = row.meshAssetPath;
            edit.prefabAssetId = row.prefabAssetId;
            edit.materialSlots.clear();
            edit.materialSlots.reserve(row.materialSlotBuffers.size());
            for (const auto& materialSlot : row.materialSlotBuffers)
            {
                std::string value = materialSlot.data();
                if (!value.empty())
                    edit.materialSlots.push_back(std::move(value));
            }
            edit.transformValid = row.transformValid;
            std::copy(std::begin(row.position), std::end(row.position), std::begin(edit.position));
            std::copy(std::begin(row.rotation), std::end(row.rotation), std::begin(edit.rotation));
            std::copy(std::begin(row.scale), std::end(row.scale), std::begin(edit.scale));
            edit.lightValid = row.lightValid;
            std::copy(std::begin(row.color), std::end(row.color), std::begin(edit.color));
            edit.intensity = row.intensity;
            edit.radius = row.radius;
            edit.innerConeDegrees = row.innerConeDegrees;
            edit.outerConeDegrees = row.outerConeDegrees;
            edit.enabled = row.enabled;
            m_commands.editPrefabEntityNames.push_back(std::move(edit));
        }
        m_assetStatus = "Prefab asset save queued: " + entry->displayName;
        m_prefabInspectorDirty = false;
    }
    if (!canSave)
        ImGui::EndDisabled();
    if (UI::IconButton(ICON_FA_ROTATE, "Reload Prefab Asset", ImVec2(-1.0f, 0.0f)))
        resetEditState();

    ImGui::Separator();
    if (entities.empty())
    {
        ImGui::TextDisabled("This prefab has no scene entities.");
        return true;
    }

    if (ImGui::BeginTable("PrefabAssetEntities", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        ImGui::TableSetupColumn("Parent", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableHeadersRow();
        std::uint32_t deleteLocalId = 0;
        for (PrefabInspectorEditRow& entity : m_prefabInspectorEditRows)
        {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(entity.localId));
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", entity.localId);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##name", entity.name, sizeof(entity.name)))
                m_prefabInspectorDirty = true;
            if (entity.parentLocalId == 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("root");
            }
            if (entity.transformValid)
            {
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat3("Position", entity.position, 0.05f, 0.0f, 0.0f, "%.2f"))
                    m_prefabInspectorDirty = true;
                if (entity.type == "mesh_entity")
                {
                    float rotationDegrees[3] = {
                        Degrees(entity.rotation[0]),
                        Degrees(entity.rotation[1]),
                        Degrees(entity.rotation[2])};
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::DragFloat3("Rotation", rotationDegrees, 0.5f, 0.0f, 0.0f, "%.1f deg"))
                    {
                        entity.rotation[0] = Radians(rotationDegrees[0]);
                        entity.rotation[1] = Radians(rotationDegrees[1]);
                        entity.rotation[2] = Radians(rotationDegrees[2]);
                        m_prefabInspectorDirty = true;
                    }
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::DragFloat3("Scale", entity.scale, 0.02f, 0.001f, 0.0f, "%.3f"))
                    {
                        entity.scale[0] = std::max(entity.scale[0], 0.001f);
                        entity.scale[1] = std::max(entity.scale[1], 0.001f);
                        entity.scale[2] = std::max(entity.scale[2], 0.001f);
                        m_prefabInspectorDirty = true;
                    }
                    if (!entity.meshAssetId.empty())
                        ImGui::TextDisabled("Mesh asset: %s", entity.meshAssetId.c_str());
                    if (!entity.meshAssetPath.empty())
                        ImGui::TextDisabled("Mesh path: %s", entity.meshAssetPath.c_str());
                    if (!entity.prefabAssetId.empty())
                        ImGui::TextDisabled("Nested prefab: %s", entity.prefabAssetId.c_str());
                    if (ImGui::TreeNodeEx("Material Slots", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        for (std::size_t slotIndex = 0; slotIndex < entity.materialSlotBuffers.size(); ++slotIndex)
                        {
                            ImGui::PushID(static_cast<int>(slotIndex));
                            ImGui::SetNextItemWidth(-1.0f);
                            const std::string label = "Slot " + std::to_string(slotIndex);
                            if (ImGui::InputText(label.c_str(),
                                    entity.materialSlotBuffers[slotIndex].data(),
                                    entity.materialSlotBuffers[slotIndex].size()))
                            {
                                m_prefabInspectorDirty = true;
                            }
                            if (ImGui::BeginDragDropTarget())
                            {
                                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
                                {
                                    const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                                    auto materialEntry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
                                    if (materialEntry && materialEntry->category == AssetLibrary::Category::Material)
                                    {
                                        CopyToBuffer(entity.materialSlotBuffers[slotIndex].data(),
                                            entity.materialSlotBuffers[slotIndex].size(),
                                            materialEntry->id);
                                        m_prefabInspectorDirty = true;
                                    }
                                    else
                                    {
                                        m_assetStatus = "Prefab material slots accept PBR material assets only";
                                    }
                                }
                                ImGui::EndDragDropTarget();
                            }
                            ImGui::PopID();
                        }
                        if (ImGui::SmallButton("Add Material Slot"))
                        {
                            entity.materialSlotBuffers.push_back({});
                            m_prefabInspectorDirty = true;
                        }
                        ImGui::SameLine();
                        if (!entity.materialSlotBuffers.empty() && ImGui::SmallButton("Remove Last Slot"))
                        {
                            entity.materialSlotBuffers.pop_back();
                            m_prefabInspectorDirty = true;
                        }
                        ImGui::TreePop();
                    }
                }
                else if (entity.lightType == "spot")
                {
                    float rotationDegrees[3] = {
                        Degrees(entity.rotation[0]),
                        Degrees(entity.rotation[1]),
                        Degrees(entity.rotation[2])};
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::DragFloat3("Rotation", rotationDegrees, 0.5f, 0.0f, 0.0f, "%.1f deg"))
                    {
                        entity.rotation[0] = Radians(rotationDegrees[0]);
                        entity.rotation[1] = Radians(rotationDegrees[1]);
                        entity.rotation[2] = Radians(rotationDegrees[2]);
                        m_prefabInspectorDirty = true;
                    }
                }
            }
            if (entity.lightValid)
            {
                if (ImGui::Checkbox("Enabled", &entity.enabled))
                    m_prefabInspectorDirty = true;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::ColorEdit3("Color", entity.color))
                    m_prefabInspectorDirty = true;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat("Intensity", &entity.intensity, 0.05f, 0.0f, 100.0f, "%.2f"))
                    m_prefabInspectorDirty = true;
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::DragFloat("Radius", &entity.radius, 0.1f, 0.1f, 1000.0f, "%.2f"))
                    m_prefabInspectorDirty = true;
                if (entity.lightType == "spot")
                {
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::DragFloat("Inner Cone", &entity.innerConeDegrees, 0.25f, 1.0f, 89.0f, "%.1f deg"))
                        m_prefabInspectorDirty = true;
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::DragFloat("Outer Cone", &entity.outerConeDegrees, 0.25f, 1.0f, 90.0f, "%.1f deg"))
                        m_prefabInspectorDirty = true;
                    entity.outerConeDegrees = std::max(entity.outerConeDegrees, entity.innerConeDegrees);
                }
            }
            ImGui::TableSetColumnIndex(2);
            const std::string typeLabel = entity.type == "dynamic_light" && !entity.lightType.empty()
                ? entity.lightType + " light"
                : (entity.type.empty() ? std::string("entity") : entity.type);
            ImGui::TextDisabled("%s", typeLabel.c_str());
            if (m_prefabInspectorEditRows.size() > 1)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete"))
                    deleteLocalId = entity.localId;
            }
            ImGui::TableSetColumnIndex(3);
            const std::string parentPreview = entity.parentLocalId == 0
                ? std::string("root")
                : ("#" + std::to_string(entity.parentLocalId));
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##parent", parentPreview.c_str()))
            {
                if (ImGui::Selectable("root", entity.parentLocalId == 0))
                {
                    entity.parentLocalId = 0;
                    m_prefabInspectorDirty = true;
                }
                for (const PrefabInspectorEditRow& candidate : m_prefabInspectorEditRows)
                {
                    if (candidate.localId == entity.localId)
                        continue;
                    const bool createsCycle = wouldCreateParentCycle(entity.localId, candidate.localId);
                    if (createsCycle)
                        ImGui::BeginDisabled();
                    const std::string candidateLabel = "#" + std::to_string(candidate.localId) + " " + candidate.name;
                    if (ImGui::Selectable(candidateLabel.c_str(), entity.parentLocalId == candidate.localId) && !createsCycle)
                    {
                        entity.parentLocalId = candidate.localId;
                        m_prefabInspectorDirty = true;
                    }
                    if (createsCycle)
                        ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        if (deleteLocalId != 0)
        {
            m_prefabInspectorEditRows.erase(
                std::remove_if(m_prefabInspectorEditRows.begin(),
                    m_prefabInspectorEditRows.end(),
                    [&](const PrefabInspectorEditRow& row) { return row.localId == deleteLocalId; }),
                m_prefabInspectorEditRows.end());
            for (PrefabInspectorEditRow& row : m_prefabInspectorEditRows)
            {
                if (row.parentLocalId == deleteLocalId)
                    row.parentLocalId = 0;
            }
            m_prefabInspectorDirty = true;
        }
        ImGui::EndTable();
    }

    return true;
}

void EditorImGui::RenderInspector()
{
    if (ImGui::Begin("Inspector"))
    {
        if (RenderSelectedPhysicsMaterialAssetInspector())
        {
        }
        else if (RenderSelectedPrefabAssetInspector())
        {
        }
        else if (m_terrainState.selected)
            RenderSelectedTerrainInspector();
        else if (m_waterBodyState.selected)
            RenderSelectedWaterBodyInspector();
        else if (m_dynamicLightState.type != DynamicLightType::None)
            RenderSelectedLightInspector();
        else if (m_cameraEditorState.selected)
            RenderSelectedCameraInspector();
        else if (m_meshRendererState.selected)
            RenderSelectedMeshRendererInspector();
        else
        {
            ImGui::TextUnformatted("Nothing selected");
            ImGui::TextWrapped("Select an entity in the Hierarchy or 3D viewport to edit its components.");
        }
    }
    ImGui::End();

    if (!m_logInspectorRendered)
    {
        m_logInspectorRendered = true;
        Tracenf("[EDITOR-IMGUI-2] Inspector active: selected_water_body=%u selected_light=%u",
            m_waterBodyState.selected ? m_waterBodyState.id : 0u,
            m_dynamicLightState.id);
    }
}

