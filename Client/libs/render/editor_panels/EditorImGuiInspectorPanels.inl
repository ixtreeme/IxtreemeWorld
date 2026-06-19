// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

bool EditorImGui::RenderAxisFloat(const char* axis,
                                  float& value,
                                  float r,
                                  float g,
                                  float b,
                                  float speed,
                                  float minValue,
                                  float maxValue)
{
    ImGui::PushID(axis);
    ImGui::TextColored(ImVec4(r, g, b, 1.0f), "%s", axis);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::DragFloat("##value", &value, speed, minValue, maxValue, "%.2f");
    ImGui::PopID();
    return changed;
}

bool EditorImGui::RenderTransformComponent(float* position, float* rotation, float* scale)
{
    bool changed = false;
    if (ImGui::CollapsingHeader(ICON_FA_CUBE " Transform", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Position");
        changed |= RenderAxisFloat("X", position[0], 0.86f, 0.25f, 0.25f, 0.1f, -500.0f, 500.0f);
        changed |= RenderAxisFloat("Y", position[1], 0.30f, 0.78f, 0.34f, 0.1f, -100.0f, 200.0f);
        changed |= RenderAxisFloat("Z", position[2], 0.28f, 0.45f, 0.92f, 0.1f, -500.0f, 500.0f);

        if (rotation)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Rotation");
            changed |= RenderAxisFloat("Pitch", rotation[0], 0.86f, 0.25f, 0.25f, 0.5f, 0.0f, 0.0f);
            changed |= RenderAxisFloat("Yaw", rotation[1], 0.30f, 0.78f, 0.34f, 0.5f, 0.0f, 0.0f);
            changed |= RenderAxisFloat("Roll", rotation[2], 0.28f, 0.45f, 0.92f, 0.5f, 0.0f, 0.0f);
        }

        if (scale)
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Scale");
            changed |= RenderAxisFloat("W", scale[0], 0.86f, 0.25f, 0.25f, 0.1f, 0.1f, 200.0f);
            changed |= RenderAxisFloat("H", scale[1], 0.30f, 0.78f, 0.34f, 0.1f, 0.1f, 200.0f);
            changed |= RenderAxisFloat("D", scale[2], 0.28f, 0.45f, 0.92f, 0.1f, 0.1f, 200.0f);
        }
    }
    return changed;
}

void EditorImGui::RenderAddComponentMenu()
{
    const bool hasWater = m_waterBodyState.selected;
    const bool hasPoint = m_dynamicLightState.type == DynamicLightType::Point;
    const bool hasSpot = m_dynamicLightState.type == DynamicLightType::Spot;
    const bool hasMesh = m_meshRendererState.selected;
    const bool hasSelection = hasWater || hasPoint || hasSpot || hasMesh;
    if (!m_componentRegistryLogged)
    {
        m_componentRegistryLogged = true;
        Tracen("[INSPECTOR-COMP] registered=7 categories=[Core, Rendering, Lighting, Editor]");
    }

    auto hasAttachedComponent = [&](const char* id) {
        return std::any_of(m_meshRendererState.editorComponents.begin(),
            m_meshRendererState.editorComponents.end(),
            [id](const EditorAttachedComponent& component) { return component.type == id; });
    };
    auto selectionHasComponent = [&](const InspectorComponentDefinition& definition) {
        const std::string id = definition.id;
        if (id == "builtin.transform")
            return hasSelection;
        if (id == "builtin.mesh_renderer")
            return hasMesh;
        if (id == "builtin.water_body")
            return hasWater;
        if (id == "builtin.point_light")
            return hasPoint;
        if (id == "builtin.spot_light")
            return hasSpot;
        return hasMesh && hasAttachedComponent(definition.id);
    };

    if (!hasSelection)
    {
        ImGui::BeginDisabled();
        UI::IconButton(ICON_FA_PLUS, "Add Component", ImVec2(-1.0f, 0.0f));
        ImGui::EndDisabled();
        return;
    }

    if (UI::IconButton(ICON_FA_PLUS, "Add Component", ImVec2(-1.0f, 0.0f)))
    {
        m_componentSearchBuffer[0] = '\0';
        ImGui::OpenPopup("AddComponentPopup");
    }

    if (ImGui::BeginPopup("AddComponentPopup"))
    {
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##componentSearch", "Search components...", m_componentSearchBuffer, sizeof(m_componentSearchBuffer));
        ImGui::Separator();

        const std::string filter = ToLowerAscii(m_componentSearchBuffer);
        std::string currentCategory;
        for (const InspectorComponentDefinition& definition : InspectorComponentRegistry())
        {
            if (!filter.empty() &&
                ToLowerAscii(definition.displayName).find(filter) == std::string::npos &&
                ToLowerAscii(definition.category).find(filter) == std::string::npos)
            {
                continue;
            }

            if (currentCategory != definition.category)
            {
                currentCategory = definition.category;
                ImGui::TextDisabled("%s", currentCategory.c_str());
            }

            const bool alreadyPresent = selectionHasComponent(definition);
            const bool canAddToSelection =
                definition.legacyType != EditorComponentType::None ||
                (definition.addableToMesh && hasMesh);
            const bool disabled = alreadyPresent || !canAddToSelection;
            if (disabled)
                ImGui::BeginDisabled();
            if (ImGui::MenuItem(definition.displayName, nullptr, false, !disabled))
            {
                m_commands.addComponentToSelectedEntity = true;
                m_commands.addComponentType = definition.legacyType;
                m_commands.addComponentTypeId = definition.id;
                Tracenf("[INSPECTOR-COMP] add requested component=%s", definition.displayName);
                ImGui::CloseCurrentPopup();
            }
            if (disabled)
                ImGui::EndDisabled();
        }

        ImGui::EndPopup();
    }
}

bool EditorImGui::RenderAttachedEditorComponents(std::vector<EditorAttachedComponent>& components)
{
    bool changed = false;
    for (EditorAttachedComponent& component : components)
    {
        if (component.type != kEditorNoteComponentId && component.type != kLodComponentId)
            continue;

        ImGui::PushID(component.type.c_str());
        const bool isLod = component.type == kLodComponentId;
        const bool open = ImGui::CollapsingHeader(isLod ? "LOD Group" : "Note", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine();
        if (ImGui::SmallButton("..."))
            ImGui::OpenPopup("ComponentMenu");
        if (ImGui::BeginPopup("ComponentMenu"))
        {
            if (ImGui::MenuItem("Remove Component"))
            {
                m_commands.removeComponentFromSelectedEntity = true;
                m_commands.removeComponentTypeId = component.type;
            }
            ImGui::EndPopup();
        }
        if (open)
        {
            if (isLod)
            {
                LodComponent& lod = m_meshRendererState.lod;
                lod.enabled = true;
                bool lodConfigChanged = false;
                bool lodCommitRequested = false;
                const std::optional<LodConfig> assetDefault = FindModelLodDefault(m_meshRendererState.meshAssetId);
                const char* source = lod.overrideAssetDefault
                    ? "Entity override"
                    : (assetDefault ? "Asset default" : "Engine default");
                ImGui::TextDisabled("Source: %s", source);
                bool overrideEnabled = lod.overrideAssetDefault;
                if (ImGui::Checkbox("Override on this entity", &overrideEnabled))
                {
                    lod.overrideAssetDefault = overrideEnabled;
                    if (overrideEnabled && assetDefault)
                        lod.config = *assetDefault;
                    lodConfigChanged = true;
                    changed = true;
                }

                LodConfig displayConfig = lod.overrideAssetDefault
                    ? lod.config
                    : (assetDefault ? *assetDefault : LodConfig{});
                const bool editEnabled = lod.overrideAssetDefault || !assetDefault;
                if (!editEnabled)
                    ImGui::BeginDisabled();

                int levelCount = static_cast<int>(std::clamp(displayConfig.levelCount, 1u, LodConfig::MaxLevels));
                if (ImGui::SliderInt("Levels", &levelCount, 1, static_cast<int>(LodConfig::MaxLevels)))
                {
                    displayConfig.levelCount = static_cast<std::uint32_t>(levelCount);
                    lodConfigChanged = true;
                    changed = true;
                }
                lodCommitRequested = lodCommitRequested || ImGui::IsItemDeactivatedAfterEdit();
                float hysteresis = displayConfig.hysteresisMeters;
                if (ImGui::DragFloat("Hysteresis (m)", &hysteresis, 0.25f, 0.0f, 100.0f, "%.2f"))
                {
                    displayConfig.hysteresisMeters = std::max(0.0f, hysteresis);
                    lodConfigChanged = true;
                    changed = true;
                }
                lodCommitRequested = lodCommitRequested || ImGui::IsItemDeactivatedAfterEdit();
                for (std::uint32_t level = 1; level < static_cast<std::uint32_t>(levelCount); ++level)
                {
                    ImGui::Separator();
                    ImGui::Text("LOD%u", level);
                    float ratioPercent = std::clamp(displayConfig.targetRatios[level], 0.001f, 1.0f) * 100.0f;
                    std::string ratioLabel = "Target %##ratio" + std::to_string(level);
                    if (ImGui::SliderFloat(ratioLabel.c_str(), &ratioPercent, 1.0f, 100.0f, "%.1f"))
                    {
                        displayConfig.targetRatios[level] = std::clamp(ratioPercent / 100.0f, 0.001f, 1.0f);
                        lodConfigChanged = true;
                        changed = true;
                    }
                    lodCommitRequested = lodCommitRequested || ImGui::IsItemDeactivatedAfterEdit();
                    float distance = displayConfig.distances[level];
                    std::string distanceLabel = "Distance (m)##distance" + std::to_string(level);
                    if (ImGui::DragFloat(distanceLabel.c_str(), &distance, 1.0f, 0.0f, 100000.0f, "%.1f"))
                    {
                        displayConfig.distances[level] = std::max(0.0f, distance);
                        lodConfigChanged = true;
                        changed = true;
                    }
                    lodCommitRequested = lodCommitRequested || ImGui::IsItemDeactivatedAfterEdit();
                }
                if (!editEnabled)
                    ImGui::EndDisabled();

                if (lodConfigChanged && editEnabled)
                {
                    displayConfig.levelCount = std::clamp(displayConfig.levelCount, 1u, LodConfig::MaxLevels);
                    displayConfig.targetRatios[0] = 1.0f;
                    displayConfig.distances[0] = 0.0f;
                    lod.config = displayConfig;
                    if (LodLogsEnabled())
                    {
                        Tracenf("[LOD] ui edit entity=%u levels=%u override=%d",
                            m_meshRendererState.id,
                            lod.config.levelCount,
                            lod.overrideAssetDefault ? 1 : 0);
                    }
                }
                if (lodCommitRequested && editEnabled)
                {
                    m_commands.lodQualityCommitRequested = true;
                    m_commands.lodQualityCommitEntityId = m_meshRendererState.id;
                    m_commands.lodQualityCommitConfig = lod.config;
                }
                if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Set as asset default", ImVec2(-1.0f, 0.0f)))
                {
                    const LodConfig defaultConfig = lod.overrideAssetDefault ? lod.config : displayConfig;
                    if (SaveModelLodDefault(m_meshRendererState.meshAssetId, defaultConfig))
                    {
                        lod.overrideAssetDefault = false;
                        lod.config = defaultConfig;
                        changed = true;
                    }
                }
            }
            else
            {
                char noteBuffer[512]{};
                CopyToBuffer(noteBuffer, sizeof(noteBuffer), component.note);
                if (ImGui::InputTextMultiline("##note", noteBuffer, sizeof(noteBuffer), ImVec2(-1.0f, 96.0f)))
                {
                    component.note = noteBuffer;
                    changed = true;
                }
            }
        }
        ImGui::PopID();
    }
    return changed;
}

void EditorImGui::RenderSelectedWaterBodyInspector()
{
    if (!m_waterBodyState.selected)
        return;

    UI::SectionHeader(ICON_FA_CUBE " Entity");
    ImGui::TextDisabled("flecs=%llu  object=%u",
        static_cast<unsigned long long>(m_selectedHierarchyEntity),
        m_waterBodyState.id);

    char nameBuffer[96]{};
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_waterBodyState.name.c_str());
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
    {
        m_waterBodyState.name = nameBuffer[0] != '\0' ? nameBuffer : ("Water_" + std::to_string(m_waterBodyState.id));
        MarkSelectedWaterBodyChanged();
    }

    float position[3] = {m_waterBodyState.center[0], m_waterBodyState.config.waterLevelY, m_waterBodyState.center[2]};
    float scale[3] = {m_waterBodyState.width, 1.0f, m_waterBodyState.depth};
    if (RenderTransformComponent(position, nullptr, scale))
    {
        m_waterBodyState.center[0] = position[0];
        m_waterBodyState.config.waterLevelY = position[1];
        m_waterBodyState.center[1] = position[1];
        m_waterBodyState.center[2] = position[2];
        m_waterBodyState.width = scale[0];
        m_waterBodyState.depth = scale[2];
        MarkSelectedWaterBodyChanged();
    }

    if (!ImGui::CollapsingHeader(ICON_FA_WATER " Water Body", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    const std::string materialLabel = m_waterBodyState.materialName.empty()
        ? (m_waterBodyState.materialId.empty() ? std::string("Inline Water") : m_waterBodyState.materialId)
        : m_waterBodyState.materialName;
    ImGui::TextUnformatted("Material");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.46f, 1.0f));
    ImGui::Button(materialLabel.c_str(), ImVec2(-1.0f, 42.0f));
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            AssignAssetToSelectedWaterBody(assetId);
        }
        ImGui::EndDragDropTarget();
    }

    if (UI::IconButton(ICON_FA_PALETTE, "Edit Material"))
    {
        m_commands.selectedWaterBodyChanged = true;
        m_commands.selectedWaterBody = m_waterBodyState;
        m_commands.openSelectedWaterMaterialEditor = true;
        OpenWaterMaterialEditor(m_waterBodyState.materialId);
    }
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_FOLDER_OPEN, "Change Material"))
        ImGui::OpenPopup("ChangeWaterMaterial");

    if (ImGui::BeginPopup("ChangeWaterMaterial"))
    {
        ImGui::TextUnformatted("Select water material:");
        ImGui::Separator();
        for (const auto& material : m_waterMaterials)
        {
            const bool selected = material.first == m_waterBodyState.materialId;
            if (ImGui::Selectable(material.first.c_str(), selected))
            {
                const std::string oldId = m_waterBodyState.materialId;
                m_waterBodyState.materialId = material.first;
                m_waterBodyState.materialName = material.first;
                Tracenf("[EDITOR-IMGUI-2] Material change: body_id=%u from=%s to=%s",
                    m_waterBodyState.id,
                    oldId.c_str(),
                    m_waterBodyState.materialId.c_str());
                MarkSelectedWaterBodyChanged();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    RenderAddComponentMenu();
    ImGui::Separator();
    if (UI::IconButton(ICON_FA_TRASH, "Delete Water Body", ImVec2(-1.0f, 0.0f)))
        m_commands.deleteSelectedWaterBody = true;
}

void EditorImGui::RenderSelectedLightInspector()
{
    if (m_dynamicLightState.type == DynamicLightType::None)
        return;

    const bool isPoint = m_dynamicLightState.type == DynamicLightType::Point;
    const bool isSpot = m_dynamicLightState.type == DynamicLightType::Spot;
    UI::SectionHeader(ICON_FA_CUBE " Entity");
    ImGui::TextDisabled("flecs=%llu  object=%u",
        static_cast<unsigned long long>(m_selectedHierarchyEntity),
        m_dynamicLightState.id);

    if (isPoint)
    {
        PointLight& point = m_dynamicLightState.point;
        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", PointLightDisplayName(point).c_str());
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
        {
            point.name = nameBuffer;
            MarkSelectedLightChanged();
        }
        float position[3] = {point.position[0], point.position[1], point.position[2]};
        float scale[3] = {point.radius, point.radius, point.radius};
        if (RenderTransformComponent(position, nullptr, scale))
        {
            point.position[0] = position[0];
            point.position[1] = position[1];
            point.position[2] = position[2];
            point.radius = std::clamp(scale[0], 0.5f, 100.0f);
            MarkSelectedLightChanged();
        }

        if (!ImGui::CollapsingHeader(ICON_FA_LIGHTBULB " Point Light", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        bool changed = false;
        changed |= ImGui::Checkbox("Enabled", &point.enabled);
        changed |= ImGui::ColorEdit3("Color", &point.r);
        changed |= ImGui::SliderFloat("Intensity", &point.intensity, 0.0f, 10.0f, "%.2f");
        changed |= ImGui::SliderFloat("Radius", &point.radius, 0.5f, 100.0f, "%.1f m");
        if (changed)
            MarkSelectedLightChanged();
    }
    else if (isSpot)
    {
        SpotLight& spot = m_dynamicLightState.spot;
        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", SpotLightDisplayName(spot).c_str());
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
        {
            spot.name = nameBuffer;
            MarkSelectedLightChanged();
        }
        float position[3] = {spot.position[0], spot.position[1], spot.position[2]};
        float rotation[3] = {spot.rotation[0] * 57.2957795f, spot.rotation[1] * 57.2957795f, spot.rotation[2] * 57.2957795f};
        float scale[3] = {spot.radius, spot.radius, spot.radius};
        if (RenderTransformComponent(position, rotation, scale))
        {
            spot.position[0] = position[0];
            spot.position[1] = position[1];
            spot.position[2] = position[2];
            spot.rotation[0] = rotation[0] / 57.2957795f;
            spot.rotation[1] = rotation[1] / 57.2957795f;
            spot.rotation[2] = rotation[2] / 57.2957795f;
            spot.radius = std::clamp(scale[0], 0.5f, 100.0f);
            MarkSelectedLightChanged();
        }

        if (!ImGui::CollapsingHeader(ICON_FA_BULLSEYE " Spot Light", ImGuiTreeNodeFlags_DefaultOpen))
            return;
        bool changed = false;
        changed |= ImGui::Checkbox("Enabled", &spot.enabled);
        changed |= ImGui::ColorEdit3("Color", &spot.r);
        changed |= ImGui::SliderFloat("Intensity", &spot.intensity, 0.0f, 10.0f, "%.2f");
        changed |= ImGui::SliderFloat("Radius", &spot.radius, 0.5f, 100.0f, "%.1f m");
        float pitch = spot.rotation[0] * 57.2957795f;
        float yaw = spot.rotation[1] * 57.2957795f;
        if (ImGui::DragFloat("Pitch", &pitch, 0.5f, 0.0f, 0.0f, "%.1f deg"))
        {
            spot.rotation[0] = pitch / 57.2957795f;
            changed = true;
        }
        if (ImGui::DragFloat("Yaw", &yaw, 0.5f, 0.0f, 0.0f, "%.1f deg"))
        {
            spot.rotation[1] = yaw / 57.2957795f;
            changed = true;
        }
        changed |= ImGui::SliderFloat("Inner Cone", &spot.innerConeDegrees, 1.0f, 89.0f, "%.1f deg");
        changed |= ImGui::SliderFloat("Outer Cone", &spot.outerConeDegrees, 1.0f, 90.0f, "%.1f deg");
        spot.outerConeDegrees = std::max(spot.outerConeDegrees, spot.innerConeDegrees);
        if (changed)
            MarkSelectedLightChanged();
    }

    ImGui::Separator();
    RenderAddComponentMenu();
    ImGui::Separator();
    if (UI::IconButton(ICON_FA_TRASH, "Delete Light", ImVec2(-1.0f, 0.0f)))
        m_commands.deleteSelectedLight = true;
}

void EditorImGui::RenderSelectedMeshRendererInspector()
{
    if (!m_meshRendererState.selected)
        return;

    UI::SectionHeader(ICON_FA_CUBE " Entity");
    ImGui::TextDisabled("flecs=%llu  object=%u",
        static_cast<unsigned long long>(m_selectedHierarchyEntity),
        m_meshRendererState.id);

    char nameBuffer[96]{};
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_meshRendererState.name.c_str());
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
    {
        m_meshRendererState.name = nameBuffer[0] != '\0' ? nameBuffer : ("Mesh Entity " + std::to_string(m_meshRendererState.id));
        MarkSelectedMeshRendererChanged();
    }

    float position[3] = {m_meshRendererState.position[0], m_meshRendererState.position[1], m_meshRendererState.position[2]};
    float rotation[3] = {
        m_meshRendererState.rotation[0] * 57.2957795f,
        m_meshRendererState.rotation[1] * 57.2957795f,
        m_meshRendererState.rotation[2] * 57.2957795f};
    float scale[3] = {m_meshRendererState.scale[0], m_meshRendererState.scale[1], m_meshRendererState.scale[2]};
    if (RenderTransformComponent(position, rotation, scale))
    {
        m_meshRendererState.position[0] = position[0];
        m_meshRendererState.position[1] = position[1];
        m_meshRendererState.position[2] = position[2];
        m_meshRendererState.rotation[0] = rotation[0] / 57.2957795f;
        m_meshRendererState.rotation[1] = rotation[1] / 57.2957795f;
        m_meshRendererState.rotation[2] = rotation[2] / 57.2957795f;
        m_meshRendererState.scale[0] = std::max(scale[0], 0.001f);
        m_meshRendererState.scale[1] = std::max(scale[1], 0.001f);
        m_meshRendererState.scale[2] = std::max(scale[2], 0.001f);
        MarkSelectedMeshRendererChanged();
    }

    if (ImGui::CollapsingHeader(ICON_FA_CUBE " MeshRenderer", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const std::string meshLabel = m_meshRendererState.meshDisplayName.empty()
            ? (m_meshRendererState.meshAssetId.empty() ? std::string("No model assigned") : m_meshRendererState.meshAssetId)
            : m_meshRendererState.meshDisplayName;
        ImGui::TextUnformatted("Mesh");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.34f, 0.64f, 1.0f));
        ImGui::Button(meshLabel.c_str(), ImVec2(-1.0f, 42.0f));
        ImGui::PopStyleColor();
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
            {
                const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                AssignAssetToSelectedMeshRenderer(assetId);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::TextDisabled("Asset ID: %s", m_meshRendererState.meshAssetId.empty() ? "<none>" : m_meshRendererState.meshAssetId.c_str());
        ImGui::TextDisabled("Path: %s", m_meshRendererState.meshAssetPath.empty() ? "<none>" : m_meshRendererState.meshAssetPath.c_str());
        ImGui::TextDisabled("Render path: %s", m_meshRendererState.skinned ? "SkinnedMeshRenderer" : "StaticMeshRenderer");
    }

    if (ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Material Slots", ImGuiTreeNodeFlags_DefaultOpen))
    {
        m_meshRendererState.materialSlotCount = std::max<std::uint32_t>(
            1u,
            std::max(m_meshRendererState.materialSlotCount,
                static_cast<std::uint32_t>(m_meshRendererState.materialSlots.size())));
        if (m_meshRendererState.materialSlots.size() < m_meshRendererState.materialSlotCount)
            m_meshRendererState.materialSlots.resize(m_meshRendererState.materialSlotCount);

        auto materialLabelForGuid = [](const std::string& guidText) {
            if (guidText.empty())
                return std::string("(Drop Material here)");
            const std::optional<Guid> guid = Guid::fromString(guidText);
            if (!guid)
                return std::string("Invalid material GUID");
            const std::optional<std::filesystem::path> path = AssetDatabase::Instance().resolveGuid(*guid);
            if (!path)
                return std::string("Missing Material");
            return path->filename().generic_string();
        };

        for (std::uint32_t slot = 0; slot < m_meshRendererState.materialSlotCount; ++slot)
        {
            ImGui::PushID(static_cast<int>(slot));
            ImGui::Text("[%u] Submesh %u", slot, slot);
            ImGui::SameLine();
            if (UI::IconButton(ICON_FA_XMARK, "Clear Material Slot", ImVec2(28.0f, 0.0f)))
            {
                m_meshRendererState.materialSlots[slot].clear();
                MarkSelectedMeshRendererChanged();
                Tracenf("[MATERIAL-SLOTS] clear entity=%u slot=%u",
                    m_meshRendererState.id,
                    slot);
            }
            const std::string label = materialLabelForGuid(m_meshRendererState.materialSlots[slot]);
            const bool emptySlot = m_meshRendererState.materialSlots[slot].empty();
            ImGui::PushStyleColor(ImGuiCol_Button,
                emptySlot ? ImVec4(0.22f, 0.22f, 0.25f, 1.0f) : ImVec4(0.34f, 0.42f, 0.34f, 1.0f));
            ImGui::Button(label.c_str(), ImVec2(-1.0f, 42.0f));
            ImGui::PopStyleColor();
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
                {
                    const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                    const auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
                    if (entry && entry->category == AssetLibrary::Category::Material)
                    {
                        std::filesystem::path materialPath =
                            entry->originalPath.empty() ? m_assetLibrary->AbsolutePath(*entry) : std::filesystem::path(entry->originalPath);
                        if (materialPath.is_relative())
                            materialPath = m_assetLibrary->AbsolutePath(*entry);
                        const Guid guid = AssetDatabase::Instance().getOrCreateGuid(materialPath);
                        m_meshRendererState.materialSlots[slot] = guid.toString();
                        m_assetStatus = "Material slot <- " + entry->displayName;
                        MarkSelectedMeshRendererChanged();
                        Tracenf("[MATERIAL-SLOTS] assign entity=%u slot=%u material=%s guid=%s",
                            m_meshRendererState.id,
                            slot,
                            entry->displayName.c_str(),
                            guid.toString().c_str());
                    }
                    else
                    {
                        m_assetStatus = "Material Slots accept Material assets";
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (!m_meshRendererState.materialSlots[slot].empty())
                ImGui::TextDisabled("guid: %s", m_meshRendererState.materialSlots[slot].c_str());
            ImGui::Spacing();
            ImGui::PopID();
        }
    }

    if (!m_meshRendererState.skinned &&
        ImGui::CollapsingHeader(ICON_FA_PALETTE " Material", ImGuiTreeNodeFlags_DefaultOpen))
    {
        m_meshRendererState.materialSlotCount = std::max<std::uint32_t>(1u, m_meshRendererState.materialSlotCount);
        m_meshRendererState.selectedMaterialSlot = std::min(m_meshRendererState.selectedMaterialSlot,
            m_meshRendererState.materialSlotCount - 1u);
        if (m_meshRendererState.materialSlotCount > 1)
        {
            ImGui::TextDisabled("Material Slot");
            ImGui::SameLine();
            for (std::uint32_t slot = 0; slot < m_meshRendererState.materialSlotCount; ++slot)
            {
                ImGui::PushID(static_cast<int>(slot));
                if (slot > 0)
                    ImGui::SameLine();
                const bool selected = slot == m_meshRendererState.selectedMaterialSlot;
                if (selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::SmallButton(std::to_string(slot).c_str()))
                    m_meshRendererState.selectedMaterialSlot = slot;
                if (selected)
                    ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }

        auto findOverride = [&]() -> MeshSceneEntity::MaterialOverride& {
            const std::uint32_t slot = m_meshRendererState.selectedMaterialSlot;
            auto it = std::find_if(m_meshRendererState.materialOverrides.begin(),
                m_meshRendererState.materialOverrides.end(),
                [slot](const MeshSceneEntity::MaterialOverride& material) { return material.slot == slot; });
            if (it == m_meshRendererState.materialOverrides.end())
            {
                MeshSceneEntity::MaterialOverride material{};
                material.slot = slot;
                m_meshRendererState.materialOverrides.push_back(material);
                return m_meshRendererState.materialOverrides.back();
            }
            return *it;
        };

        MeshSceneEntity::MaterialOverride& material = findOverride();
        bool changed = false;
        changed |= ImGui::Checkbox("Override Active", &material.enabled);
        changed |= ImGui::ColorEdit4("BaseColor Tint", material.baseColor, ImGuiColorEditFlags_Float);
        changed |= ImGui::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Normal Strength", &material.normalStrength, 0.0f, 4.0f, "%.2f");
        changed |= ImGui::SliderFloat("AO Strength", &material.aoStrength, 0.0f, 2.0f, "%.2f");
        changed |= ImGui::ColorEdit3("Emissive", material.emissive, ImGuiColorEditFlags_Float);
        changed |= ImGui::SliderFloat("Emissive Intensity", &material.emissiveIntensity, 0.0f, 20.0f, "%.2f");
        changed |= ImGui::DragFloat2("UV Tiling", material.uvTiling, 0.01f, 0.01f, 64.0f, "%.2f");
        changed |= ImGui::DragFloat2("UV Offset", material.uvOffset, 0.01f, -1000.0f, 1000.0f, "%.2f");

        material.baseColor[0] = std::clamp(material.baseColor[0], 0.0f, 8.0f);
        material.baseColor[1] = std::clamp(material.baseColor[1], 0.0f, 8.0f);
        material.baseColor[2] = std::clamp(material.baseColor[2], 0.0f, 8.0f);
        material.baseColor[3] = std::clamp(material.baseColor[3], 0.0f, 1.0f);
        material.metallic = std::clamp(material.metallic, 0.0f, 1.0f);
        material.roughness = std::clamp(material.roughness, 0.0f, 1.0f);
        material.normalStrength = std::clamp(material.normalStrength, 0.0f, 4.0f);
        material.aoStrength = std::clamp(material.aoStrength, 0.0f, 2.0f);
        material.emissiveIntensity = std::clamp(material.emissiveIntensity, 0.0f, 20.0f);
        material.uvTiling[0] = std::clamp(material.uvTiling[0], 0.01f, 64.0f);
        material.uvTiling[1] = std::clamp(material.uvTiling[1], 0.01f, 64.0f);

        if (changed)
        {
            material.enabled = true;
            Tracenf("[MMAT] entity=%u slot=%u baseColor=(%.3f,%.3f,%.3f,%.3f) metallic=%.3f roughness=%.3f normal=%.3f ao=%.3f emissive=(%.3f,%.3f,%.3f,%.3f) uvTiling=(%.3f,%.3f) uvOffset=(%.3f,%.3f) (changed)",
                m_meshRendererState.id,
                material.slot,
                material.baseColor[0], material.baseColor[1], material.baseColor[2], material.baseColor[3],
                material.metallic,
                material.roughness,
                material.normalStrength,
                material.aoStrength,
                material.emissive[0], material.emissive[1], material.emissive[2], material.emissiveIntensity,
                material.uvTiling[0], material.uvTiling[1],
                material.uvOffset[0], material.uvOffset[1]);
            Tracen("[MMAT] override buffer updated (live, no reload, no remesh)");
            MarkSelectedMeshRendererChanged();
        }
    }

    ImGui::Separator();
    if (RenderAttachedEditorComponents(m_meshRendererState.editorComponents))
        MarkSelectedMeshRendererChanged();
    RenderAddComponentMenu();
    ImGui::Separator();
    if (UI::IconButton(ICON_FA_TRASH, "Delete Mesh Entity", ImVec2(-1.0f, 0.0f)))
        m_commands.deleteSelectedMeshEntity = true;
}

void EditorImGui::ApplyTimeOfDayPreset(float hour)
{
    if (hour < 6.0f)
    {
        m_lightingState.directional.azimuthDegrees = 180.0f;
        m_lightingState.directional.elevationDegrees = 4.0f;
        m_lightingState.directional.intensity = 0.2f;
        m_lightingState.ambient.intensity = 0.35f;
    }
    else if (hour < 10.0f)
    {
        m_lightingState.directional.azimuthDegrees = 90.0f;
        m_lightingState.directional.elevationDegrees = 18.0f;
        m_lightingState.directional.intensity = 0.9f;
        m_lightingState.ambient.intensity = 0.6f;
    }
    else if (hour < 17.0f)
    {
        m_lightingState.directional.azimuthDegrees = 180.0f;
        m_lightingState.directional.elevationDegrees = 75.0f;
        m_lightingState.directional.intensity = 1.2f;
        m_lightingState.ambient.intensity = 0.8f;
    }
    else
    {
        m_lightingState.directional.azimuthDegrees = 270.0f;
        m_lightingState.directional.elevationDegrees = 10.0f;
        m_lightingState.directional.intensity = 0.7f;
        m_lightingState.ambient.intensity = 0.7f;
    }
}

void EditorImGui::RenderLightingPanel()
{
    UI::SectionHeader(ICON_FA_LIGHTBULB " Directional Light");
    ImGui::Checkbox("Sun Enabled", &m_lightingState.directional.enabled);
    ImGui::Checkbox("Sun Shadows", &m_lightingState.sunShadowsEnabled);
    ImGui::ColorEdit3("Sun Color", &m_lightingState.directional.r);
    ImGui::SliderFloat("Sun Intensity", &m_lightingState.directional.intensity, 0.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("Sun Angle X", &m_lightingState.directional.elevationDegrees, -90.0f, 90.0f, "%.1f deg");
    ImGui::SliderFloat("Sun Angle Y", &m_lightingState.directional.azimuthDegrees, 0.0f, 360.0f, "%.1f deg");

    ImGui::Spacing();
    UI::SectionHeader(ICON_FA_GEAR " Ambient Light");
    ImGui::ColorEdit3("Ambient Color", &m_lightingState.ambient.r);
    ImGui::SliderFloat("Ambient Intensity", &m_lightingState.ambient.intensity, 0.0f, 3.0f, "%.2f");

    ImGui::Spacing();
    UI::SectionHeader(ICON_FA_GEAR " Time of Day");
    ImGui::SliderFloat("Hour", &m_timeOfDayHours, 0.0f, 24.0f, "%.1f h");
    if (UI::IconButton(ICON_FA_CHECK, "Apply Preset"))
        ApplyTimeOfDayPreset(m_timeOfDayHours);
}

void EditorImGui::RenderDynamicLightsPanel()
{
    const uint32_t total = m_lightingState.numPointLights + m_lightingState.numSpotLights;
    ImGui::Text("Total: %u / %u", total, kMaxDynamicPointLights + kMaxDynamicSpotLights);

    if (UI::IconButton(ICON_FA_LIGHTBULB, "Point Light"))
    {
        m_commands.addPointLight = true;
        Tracen("[EDITOR-IMGUI-2] Light spawn: type=point id=pending");
    }
    ImGui::SameLine();
    if (UI::IconButton(ICON_FA_LIGHTBULB, "Spot Light"))
    {
        m_commands.addSpotLight = true;
        Tracen("[EDITOR-IMGUI-2] Light spawn: type=spot id=pending");
    }
}

void EditorImGui::RenderWorldPanel()
{
    if (ImGui::Begin(ICON_FA_GLOBE " World"))
    {
        if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen))
            RenderPerformancePanel();
        if (ImGui::CollapsingHeader("Objects", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto queuePrimitive = [&](const char* type) {
                m_commands.addPrimitiveEntity = true;
                m_commands.primitiveType = type;
                Tracenf("[PRIMITIVE] UI create queued type=%s", type);
            };
            if (UI::IconButton(ICON_FA_CUBE, "Cube"))
                queuePrimitive("cube");
            ImGui::SameLine();
            if (UI::IconButton(ICON_FA_CUBE, "Sphere"))
                queuePrimitive("sphere");
            ImGui::SameLine();
            if (UI::IconButton(ICON_FA_CUBE, "Capsule"))
                queuePrimitive("capsule");
        }
        if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (UI::IconButton(ICON_FA_MOUNTAIN, "Create Terrain"))
            {
                if (m_terrainState.exists)
                    m_replaceTerrainConfirmOpen = true;
                else
                    m_createTerrainModalOpen = true;
            }
            if (m_terrainState.exists)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%.0fm x %.0fm, %.2fm/cell",
                    m_terrainState.widthMeters,
                    m_terrainState.depthMeters,
                    m_terrainState.cellSizeMeters);
            }
        }
        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
            RenderLightingPanel();
        if (ImGui::CollapsingHeader("Dynamic Lights", ImGuiTreeNodeFlags_DefaultOpen))
            RenderDynamicLightsPanel();
    }
    ImGui::End();
    RenderCreateTerrainModal();
}

void EditorImGui::RenderPerformancePanel()
{
    auto queueRenderResolution = [&](bool native, std::uint32_t width, std::uint32_t height) {
        m_commands.renderResolutionChanged = true;
        m_commands.renderResolutionUseNative = native;
        m_commands.renderResolutionWidth = width;
        m_commands.renderResolutionHeight = height;
        Tracenf("[RENDER-RES] request mode=%s size=%ux%u",
            native ? "native" : "fixed",
            width,
            height);
    };

    const double frameMs = m_engineStats.averageFrameMs > 0.0 ? m_engineStats.averageFrameMs : m_engineStats.frameMs;
    ImGui::Text("FPS: %.1f", m_engineStats.fps);
    ImGui::Text("Frame: %.2f ms  (min %.2f / max %.2f)",
        frameMs,
        m_engineStats.minFrameMs,
        m_engineStats.maxFrameMs);
    ImGui::Text("Swapchain: %u x %u",
        m_engineStats.swapchainWidth,
        m_engineStats.swapchainHeight);
    ImGui::Text("Render Target: %u x %u",
        m_engineStats.renderWidth,
        m_engineStats.renderHeight);

    const char* modes[] = {
        "Native / Window",
        "3840 x 2160",
        "2560 x 1440",
        "1920 x 1080",
        "1600 x 900",
        "1280 x 720",
        "Custom"
    };
    if (ImGui::Combo("Render Resolution", &m_renderResolutionMode, modes, IM_ARRAYSIZE(modes)))
    {
        switch (m_renderResolutionMode)
        {
        case 0: queueRenderResolution(true, 0, 0); break;
        case 1: queueRenderResolution(false, 3840, 2160); break;
        case 2: queueRenderResolution(false, 2560, 1440); break;
        case 3: queueRenderResolution(false, 1920, 1080); break;
        case 4: queueRenderResolution(false, 1600, 900); break;
        case 5: queueRenderResolution(false, 1280, 720); break;
        default: break;
        }
    }
    if (m_renderResolutionMode == 6)
    {
        ImGui::InputInt("Width", &m_customRenderResolutionWidth, 16, 128);
        ImGui::InputInt("Height", &m_customRenderResolutionHeight, 16, 128);
        m_customRenderResolutionWidth = std::clamp(m_customRenderResolutionWidth, 320, 7680);
        m_customRenderResolutionHeight = std::clamp(m_customRenderResolutionHeight, 180, 4320);
        if (UI::IconButton(ICON_FA_CHECK, "Apply Resolution"))
        {
            queueRenderResolution(false,
                static_cast<std::uint32_t>(m_customRenderResolutionWidth),
                static_cast<std::uint32_t>(m_customRenderResolutionHeight));
        }
    }

    const float budgetFraction = static_cast<float>(std::clamp(m_engineStats.frameBudgetPercent / 100.0, 0.0, 1.0));
    const std::string budgetLabel =
        std::to_string(static_cast<int>(std::round(m_engineStats.frameBudgetPercent))) + "%";
    ImGui::TextUnformatted("Frame Budget @60 FPS");
    ImGui::ProgressBar(budgetFraction, ImVec2(-1.0f, 0.0f), budgetLabel.c_str());

    const float cpuFraction = static_cast<float>(std::clamp(m_engineStats.processCpuPercent / 100.0, 0.0, 1.0));
    const std::string cpuLabel =
        std::to_string(static_cast<int>(std::round(m_engineStats.processCpuPercent))) + "%";
    ImGui::TextUnformatted("Process CPU");
    ImGui::ProgressBar(cpuFraction, ImVec2(-1.0f, 0.0f), cpuLabel.c_str());

    ImGui::TextDisabled("Frame #%llu | scene entities=%zu | static submitted=%zu drawcalls=%zu",
        static_cast<unsigned long long>(m_engineStats.frameNumber),
        m_engineStats.sceneEntityCount,
        m_engineStats.staticMeshSubmitted,
        m_engineStats.staticMeshDrawCalls);
}

void EditorImGui::RenderCreateTerrainModal()
{
    if (m_replaceTerrainConfirmOpen)
    {
        ImGui::OpenPopup("Replace Terrain?");
        m_replaceTerrainConfirmOpen = false;
    }
    if (ImGui::BeginPopupModal("Replace Terrain?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("The current scene already has a terrain. Creating a new terrain will replace it.");
        ImGui::Separator();
        if (ImGui::Button("Replace", ImVec2(120.0f, 0.0f)))
        {
            m_createTerrainModalOpen = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_createTerrainModalOpen)
    {
        ImGui::OpenPopup("Create Terrain");
        m_createTerrainModalOpen = false;
    }
    if (ImGui::BeginPopupModal("Create Terrain", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputFloat("Width (m)", &m_createTerrainWidthMeters, 10.0f, 100.0f, "%.1f");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputFloat("Depth (m)", &m_createTerrainDepthMeters, 10.0f, 100.0f, "%.1f");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputFloat("Cell size (m/cell)", &m_createTerrainCellSizeMeters, 0.25f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("Chunk size (cells/chunk)", &m_createTerrainChunkSizeCells, 16, 32);

        m_createTerrainWidthMeters = std::max(1.0f, m_createTerrainWidthMeters);
        m_createTerrainDepthMeters = std::max(1.0f, m_createTerrainDepthMeters);
        m_createTerrainCellSizeMeters = std::max(0.01f, m_createTerrainCellSizeMeters);
        m_createTerrainChunkSizeCells = std::clamp(m_createTerrainChunkSizeCells, 32, 256);
        const std::uint32_t cellsX = std::max(1u,
            static_cast<std::uint32_t>(std::lround(m_createTerrainWidthMeters / m_createTerrainCellSizeMeters)));
        const std::uint32_t cellsZ = std::max(1u,
            static_cast<std::uint32_t>(std::lround(m_createTerrainDepthMeters / m_createTerrainCellSizeMeters)));
        const std::uint32_t chunkSize = static_cast<std::uint32_t>(m_createTerrainChunkSizeCells);
        const std::uint32_t chunksX = (cellsX + chunkSize - 1u) / chunkSize;
        const std::uint32_t chunksZ = (cellsZ + chunkSize - 1u) / chunkSize;
        const std::uint64_t vertexCount =
            static_cast<std::uint64_t>(cellsX + 1u) * static_cast<std::uint64_t>(cellsZ + 1u);
        ImGui::Text("Cells: %u x %u", cellsX, cellsZ);
        ImGui::Text("Chunk grid: %u x %u", chunksX, chunksZ);
        ImGui::Text("Vertices: %llu", static_cast<unsigned long long>(vertexCount));
        if (vertexCount > 4000000ull)
            ImGui::TextColored(ImVec4(0.95f, 0.58f, 0.22f, 1.0f), "Large terrain: this may be heavy to edit/render.");

        ImGui::Separator();
        if (ImGui::Button("Create", ImVec2(120.0f, 0.0f)))
        {
            TerrainSceneData terrain{};
            terrain.exists = true;
            terrain.name = "Terrain";
            terrain.cellSizeMeters = m_createTerrainCellSizeMeters;
            terrain.cellsX = cellsX;
            terrain.cellsZ = cellsZ;
            terrain.chunkSizeCells = chunkSize;
            terrain.widthMeters = static_cast<float>(cellsX) * terrain.cellSizeMeters;
            terrain.depthMeters = static_cast<float>(cellsZ) * terrain.cellSizeMeters;
            m_commands.createTerrain = true;
            m_commands.terrainCreate = terrain;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorImGui::RenderWaterSculptToolPanel()
{
    if (!m_editorModeActive || !m_waterSculptToolOpen)
        return;

    if (ImGui::Begin("Water Sculpt Tool", &m_waterSculptToolOpen))
    {
        if (!m_waterBodyState.selected)
        {
            ImGui::TextWrapped("Select a water body to sculpt its shape.");
        }

        bool active = m_editorSettings.toolMode == MapEditorToolMode::WaterSculpt;
        if (ImGui::Checkbox("Sculpt Mode Active", &active))
            SetToolMode(active ? MapEditorToolMode::WaterSculpt : MapEditorToolMode::None);

        if (!m_waterBodyState.selected)
            ImGui::BeginDisabled();
        const char* modes[] = {"Add Water", "Remove Water"};
        int mode = m_editorSettings.waterSculptAdd ? 0 : 1;
        if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes)))
        {
            m_editorSettings.waterSculptAdd = mode == 0;
            Tracenf("[EDITOR-IMGUI-5] Water sculpt brush mode: %s",
                m_editorSettings.waterSculptAdd ? "add" : "remove");
        }
        ImGui::SliderFloat("Radius", &m_editorSettings.waterSculptRadiusMeters, 0.5f, 20.0f, "%.1f m");
        if (!m_waterBodyState.selected)
            ImGui::EndDisabled();
    }
    ImGui::End();
}

void EditorImGui::RenderHeightmapToolPanel()
{
    if (!m_editorModeActive || !m_heightmapToolOpen)
        return;

    if (ImGui::Begin("Heightmap Tool", &m_heightmapToolOpen))
    {
        bool active = m_editorSettings.toolMode == MapEditorToolMode::Heightmap;
        if (ImGui::Checkbox("Heightmap Editing Active", &active))
            SetToolMode(active ? MapEditorToolMode::Heightmap : MapEditorToolMode::None);

        auto modeRadio = [this](const char* label, MapEditorTool tool) {
            if (ImGui::RadioButton(label, m_editorSettings.tool == tool))
            {
                m_editorSettings.tool = tool;
                if (m_editorSettings.toolMode != MapEditorToolMode::Heightmap)
                    SetToolMode(MapEditorToolMode::Heightmap);
            }
        };
        modeRadio("Raise", MapEditorTool::Raise);
        ImGui::SameLine();
        modeRadio("Lower", MapEditorTool::Lower);
        modeRadio("Smooth", MapEditorTool::Smooth);
        ImGui::SameLine();
        modeRadio("Flatten", MapEditorTool::Flatten);

        ImGui::Separator();
        ImGui::SliderFloat("Radius", &m_editorSettings.brushRadiusMeters, 1.0f, 100.0f, "%.1f m");
        ImGui::SliderFloat("Strength", &m_editorSettings.brushStrength, 0.1f, 5.0f, "%.2f");
        ImGui::SliderFloat("Falloff", &m_editorSettings.brushFalloff, 0.0f, 1.0f, "%.2f");
        if (m_editorSettings.tool == MapEditorTool::Flatten)
            ImGui::SliderFloat("Target Height", &m_editorSettings.flattenTargetY, -50.0f, 100.0f, "%.2f m");
    }
    ImGui::End();
}

void EditorImGui::RenderSplatLayerSlot(std::uint32_t slotIndex)
{
    if (slotIndex >= m_paletteSlots.size())
        return;

    MapEditorPaletteSlot& slot = m_paletteSlots[slotIndex];
    ImGui::PushID(static_cast<int>(slotIndex));
    const bool selected = m_editorSettings.textureSlot == slotIndex;
    const ImVec4 selectedColor = ImVec4(0.20f, 0.34f, 0.56f, 1.0f);
    const ImVec4 selectedHover = ImVec4(0.24f, 0.42f, 0.68f, 1.0f);
    const ImVec4 idleColor = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? selectedColor : idleColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? selectedHover : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, selected ? selectedHover : ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const std::string label = std::to_string(slotIndex) + "##splat_slot";
    if (ImGui::Button(label.c_str(), ImVec2(48.0f, 38.0f)))
    {
        m_editorSettings.textureSlot = slotIndex;
        m_editorSettings.tool = MapEditorTool::Paint;
        if (m_editorSettings.toolMode != MapEditorToolMode::SplatPaint)
            SetToolMode(MapEditorToolMode::SplatPaint);
    }
    ImGui::PopStyleColor(3);

    if (m_assetFilter != AssetBrowserFilter::Scene && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
        {
            const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
            auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
            if (entry && (entry->category == AssetLibrary::Category::Texture ||
                entry->category == AssetLibrary::Category::Material))
            {
                MapEditorPaletteSlot next = BuildPaletteSlotFromAsset(slotIndex, *entry);
                if (!next.texturePath.empty())
                {
                    slot = next;
                    m_commands.paletteSlotChanged = true;
                    m_commands.paletteSlot = slotIndex;
                    m_commands.paletteAssetId = next.assetId;
                    m_commands.paletteTexturePath = next.texturePath;
                    m_commands.paletteSlotData = next;
                    m_assetStatus = "Splat layer texture <- " + entry->displayName;
                    Tracenf("[EDITOR-IMGUI-5] Splat layer texture changed: layer=%u asset_id=%s",
                        slotIndex,
                        entry->id.c_str());
                }
            }
            else
            {
                m_assetStatus = "Splat slots accept texture or material assets";
            }
        }
        ImGui::EndDragDropTarget();
    }

    const std::string name = slot.displayName.empty()
        ? (slot.texturePath.empty() ? std::string("empty") : slot.texturePath)
        : slot.displayName;
    ImGui::TextWrapped("%s", name.c_str());
    ImGui::PopID();
}

void EditorImGui::RenderSplatPaintToolPanel()
{
    if (!m_editorModeActive || !m_splatPaintToolOpen)
        return;

    if (ImGui::Begin("Splat Paint Tool", &m_splatPaintToolOpen))
    {
        bool active = m_editorSettings.toolMode == MapEditorToolMode::SplatPaint;
        if (ImGui::Checkbox("Splat Paint Active", &active))
            SetToolMode(active ? MapEditorToolMode::SplatPaint : MapEditorToolMode::None);

        ImGui::TextUnformatted("Active Layer");
        for (std::uint32_t i = 0; i < m_paletteSlots.size(); ++i)
        {
            ImGui::BeginGroup();
            RenderSplatLayerSlot(i);
            ImGui::EndGroup();
            if (i % 4 != 3)
                ImGui::SameLine();
        }

        ImGui::Separator();
        m_editorSettings.tool = MapEditorTool::Paint;
        int paintMode = m_editorSettings.paintMode == MapEditorPaintMode::Mix ? 1 : 0;
        const char* modes[] = {"Replace", "Mix"};
        if (ImGui::Combo("Paint Mode", &paintMode, modes, IM_ARRAYSIZE(modes)))
            m_editorSettings.paintMode = paintMode == 1 ? MapEditorPaintMode::Mix : MapEditorPaintMode::Replace;
        ImGui::SliderFloat("Radius", &m_editorSettings.brushRadiusMeters, 1.0f, 100.0f, "%.1f m");
        ImGui::SliderFloat("Strength", &m_editorSettings.brushStrength, 0.1f, 1.0f, "%.2f");
        ImGui::SliderFloat("Falloff", &m_editorSettings.brushFalloff, 0.0f, 1.0f, "%.2f");

        ImGui::Separator();
        UI::SectionHeader(ICON_FA_PALETTE " Terrain Material");
        if (m_terrainState.exists)
        {
            bool triplanarEnabled = m_terrainState.triplanarEnabled;
            float triplanarSharpness = std::clamp(m_terrainState.triplanarSharpness, 1.0f, 16.0f);
            float triplanarSlopeThreshold = std::clamp(m_terrainState.triplanarSlopeThreshold, 0.0f, 1.0f);
            float triplanarSlopeTransition = std::clamp(m_terrainState.triplanarSlopeTransition, 0.001f, 1.0f);
            bool triplanarChanged = false;
            triplanarChanged |= ImGui::Checkbox("Triplanar Mapping", &triplanarEnabled);
            triplanarChanged |= ImGui::SliderFloat("Triplanar Sharpness", &triplanarSharpness, 1.0f, 16.0f, "%.2f");
            triplanarChanged |= ImGui::SliderFloat("Slope Threshold", &triplanarSlopeThreshold, 0.0f, 1.0f, "%.3f");
            triplanarChanged |= ImGui::SliderFloat("Slope Transition", &triplanarSlopeTransition, 0.001f, 1.0f, "%.3f");
            if (triplanarChanged)
            {
                m_terrainState.triplanarEnabled = triplanarEnabled;
                m_terrainState.triplanarSharpness = triplanarSharpness;
                m_terrainState.triplanarSlopeThreshold = triplanarSlopeThreshold;
                m_terrainState.triplanarSlopeTransition = triplanarSlopeTransition;
                m_commands.terrainTriplanarChanged = true;
                m_commands.terrainTriplanarEnabled = triplanarEnabled;
                m_commands.terrainTriplanarSharpness = triplanarSharpness;
                m_commands.terrainTriplanarSlopeThreshold = triplanarSlopeThreshold;
                m_commands.terrainTriplanarSlopeTransition = triplanarSlopeTransition;
                SceneManager::Instance().MarkDirty();
            }
        }
        else
        {
            ImGui::TextDisabled("Create a terrain to edit triplanar sampling.");
        }
        ImGui::Separator();
        const std::uint32_t selectedSlot = std::min<std::uint32_t>(m_editorSettings.textureSlot, 7u);
        MapEditorPaletteSlot& materialSlot = m_paletteSlots[selectedSlot];
        ImGui::Text("Layer %u: %s",
            selectedSlot,
            materialSlot.displayName.empty() ? "empty" : materialSlot.displayName.c_str());

        auto commitMaterialParams = [&]() {
            materialSlot.slot = selectedSlot;
            materialSlot.tilingScaleX = std::clamp(materialSlot.tilingScaleX, 0.01f, 64.0f);
            materialSlot.tilingScaleY = std::clamp(materialSlot.tilingScaleY, 0.01f, 64.0f);
            materialSlot.normalStrength = std::clamp(materialSlot.normalStrength, 0.0f, 4.0f);
            materialSlot.roughnessStrength = std::clamp(materialSlot.roughnessStrength, 0.0f, 4.0f);
            m_commands.paletteSlotParamsChanged = true;
            m_commands.paletteSlot = selectedSlot;
            m_commands.paletteSlotData = materialSlot;
            SceneManager::Instance().MarkDirty();
        };

        float tiling = (materialSlot.tilingScaleX + materialSlot.tilingScaleY) * 0.5f;
        if (ImGui::SliderFloat("Tiling", &tiling, 0.05f, 32.0f, "%.2f"))
        {
            materialSlot.tilingScaleX = tiling;
            materialSlot.tilingScaleY = tiling;
            commitMaterialParams();
        }
        if (ImGui::ColorEdit3("Tint", materialSlot.colorTint))
            commitMaterialParams();
        if (ImGui::SliderFloat("Normal Strength", &materialSlot.normalStrength, 0.0f, 3.0f, "%.2f"))
            commitMaterialParams();
        if (ImGui::SliderFloat("Roughness", &materialSlot.roughnessStrength, 0.0f, 2.0f, "%.2f"))
            commitMaterialParams();
        if (ImGui::SliderFloat("Metallic", &materialSlot.metallicStrength, 0.0f, 1.0f, "%.2f"))
            commitMaterialParams();
        if (ImGui::SliderFloat("AO Strength", &materialSlot.aoStrength, 0.0f, 1.0f, "%.2f"))
            commitMaterialParams();
        if (ImGui::SliderFloat2("UV Offset", materialSlot.uvOffset, -10.0f, 10.0f, "%.3f"))
            commitMaterialParams();
        if (ImGui::SliderFloat("UV Rotation", &materialSlot.uvRotationDegrees, -180.0f, 180.0f, "%.1f deg"))
            commitMaterialParams();
    }
    ImGui::End();
}

