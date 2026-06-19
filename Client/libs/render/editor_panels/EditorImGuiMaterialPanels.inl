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

void EditorImGui::RenderInspector()
{
    if (ImGui::Begin("Inspector"))
    {
        if (m_terrainState.selected)
            RenderSelectedTerrainInspector();
        else if (m_waterBodyState.selected)
            RenderSelectedWaterBodyInspector();
        else if (m_dynamicLightState.type != DynamicLightType::None)
            RenderSelectedLightInspector();
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

