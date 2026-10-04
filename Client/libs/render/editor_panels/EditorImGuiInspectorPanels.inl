// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

// One axis of a vector row: the colored axis letter, then the value filling `width`.
bool EditorImGui::RenderAxisFloat(const char* axis,
                                  float& value,
                                  float r,
                                  float g,
                                  float b,
                                  float speed,
                                  float minValue,
                                  float maxValue,
                                  const char* format)
{
    ImGui::PushID(axis);
    const float width = ImGui::CalcItemWidth();
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(r, g, b, 1.0f), "%s", axis);
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::SetNextItemWidth(std::max(20.0f, width - ImGui::CalcTextSize(axis).x - 3.0f));
    const bool changed = ImGui::DragFloat("##value", &value, speed, minValue, maxValue, format);
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

bool EditorImGui::RenderTransformComponent(float* position, float* rotation, float* scale, bool meshScale)
{
    bool changed = false;
    // Position, rotation and scale as one row each: X / Y / Z side by side, like every 3D editor.
    const auto vectorRow = [&](const char* label, const char* tooltip, float* values, float speed, float minValue, float maxValue,
                               const char* format = "%.2f") {
        UI::Property(label, [&](const char*) {
            ImGui::PushID(label);
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            const float fieldWidth = std::max(30.0f, (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f);
            const char* axes[] = {"X", "Y", "Z"};
            const ImVec4 colors[] = {UI::Theme::AxisX, UI::Theme::AxisY, UI::Theme::AxisZ};
            for (int i = 0; i < 3; ++i)
            {
                if (i > 0)
                    ImGui::SameLine(0.0f, spacing);
                ImGui::PushItemWidth(fieldWidth);
                changed |= RenderAxisFloat(axes[i], values[i], colors[i].x, colors[i].y, colors[i].z, speed, minValue, maxValue, format);
                ImGui::PopItemWidth();
            }
            ImGui::PopID();
            return false;
        });
        UI::ItemTooltip(tooltip);
    };
    if (ImGui::CollapsingHeader(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT "  Transform", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
    {
        vectorRow("Position", "Position in metres", position, 0.1f, -500.0f, 500.0f);
        if (rotation)
            vectorRow("Rotation", "Rotation in degrees around X (pitch), Y (yaw) and Z (roll)", rotation, 0.5f, 0.0f, 0.0f);
        // A model scale gets three decimals and a 0.001 floor: models authored in centimetres
        // (Mixamo FBX) need 0.01, which "%.2f" with a 0.1 drag floor could neither show nor reach.
        if (scale && meshScale)
            vectorRow("Scale", "Scale along X, Y and Z (1 = as imported; 0.01 turns centimetres into metres)",
                scale, 0.01f, 0.001f, 1000.0f, "%.3f");
        else if (scale)
            vectorRow("Scale", "Size along X, Y and Z", scale, 0.1f, 0.1f, 200.0f);
    }
    return changed;
}

void EditorImGui::RenderInspectorTitle(const char* icon, const char* typeName, std::uint32_t objectId)
{
    const ImVec2 start = ImGui::GetCursorPos();
    if (UI::GetEditorFonts().bold)
        ImGui::PushFont(UI::GetEditorFonts().bold);
    ImGui::Text("%s  %s", icon, typeName);
    if (UI::GetEditorFonts().bold)
        ImGui::PopFont();

    // Engine ids and file paths help when debugging, not when building a scene: hidden by default.
    const float buttonSize = ImGui::GetFrameHeight();
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - buttonSize, start.y));
    if (UI::ToggleIconButton(ICON_FA_BUG, m_inspectorShowDebugInfo,
            m_inspectorShowDebugInfo ? "Hide engine ids and asset paths" : "Show engine ids and asset paths",
            buttonSize))
        m_inspectorShowDebugInfo = !m_inspectorShowDebugInfo;
    if (m_inspectorShowDebugInfo)
        ImGui::TextDisabled("entity %llu, object %u", static_cast<unsigned long long>(m_selectedHierarchyEntity), objectId);
    ImGui::Separator();
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
        Tracenf("[INSPECTOR-COMP] registered=%zu categories=[Core, Rendering, Physics, Lighting, Editor]",
            InspectorComponentRegistry().size());
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
        if (id == "physics.rigidbody")
            return hasMesh && m_meshRendererState.hasRigidbody;
        if (id == "physics.box_collider" ||
            id == "physics.sphere_collider" ||
            id == "physics.capsule_collider" ||
            id == "physics.trigger_box" ||
            id == "physics.trigger_sphere" ||
            id == "physics.trigger_capsule")
        {
            return hasMesh && m_meshRendererState.hasCollider;
        }
        if (id == "physics.fixed_joint")
            return hasMesh && m_meshRendererState.hasFixedJoint;
        if (id == "physics.hinge_joint")
            return hasMesh && m_meshRendererState.hasHingeJoint;
        if (id == "physics.character_controller")
            return hasMesh && m_meshRendererState.hasCharacterController;
        if (id == "audio.audio_source")
            return hasMesh && m_meshRendererState.hasAudioSource;
        if (id == "audio.audio_listener")
            return hasMesh && m_meshRendererState.hasAudioListener;
        if (id == "scripting.script")
            return hasMesh && m_meshRendererState.hasScript;
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
        UI::Prop::InputTextWithHint("##componentSearch", "Search components...", m_componentSearchBuffer, sizeof(m_componentSearchBuffer));
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
                (definition.addableToMesh && hasMesh) ||
                (!definition.addableToMesh && definition.legacyType != EditorComponentType::None);
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
        const bool open = ImGui::CollapsingHeader(isLod ? "LOD Group" : "Note", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        if (UI::HeaderMenuButton("Component options"))
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
                if (UI::Prop::Checkbox("Override on this entity", &overrideEnabled))
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
                if (UI::Prop::SliderInt("Levels", &levelCount, 1, static_cast<int>(LodConfig::MaxLevels)))
                {
                    displayConfig.levelCount = static_cast<std::uint32_t>(levelCount);
                    lodConfigChanged = true;
                    changed = true;
                }
                lodCommitRequested = lodCommitRequested || ImGui::IsItemDeactivatedAfterEdit();
                float hysteresis = displayConfig.hysteresisMeters;
                if (UI::Prop::DragFloat("Hysteresis (m)", &hysteresis, 0.25f, 0.0f, 100.0f, "%.2f"))
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
                    if (UI::Prop::SliderFloat(ratioLabel.c_str(), &ratioPercent, 1.0f, 100.0f, "%.1f"))
                    {
                        displayConfig.targetRatios[level] = std::clamp(ratioPercent / 100.0f, 0.001f, 1.0f);
                        lodConfigChanged = true;
                        changed = true;
                    }
                    lodCommitRequested = lodCommitRequested || ImGui::IsItemDeactivatedAfterEdit();
                    float distance = displayConfig.distances[level];
                    std::string distanceLabel = "Distance (m)##distance" + std::to_string(level);
                    if (UI::Prop::DragFloat(distanceLabel.c_str(), &distance, 1.0f, 0.0f, 100000.0f, "%.1f"))
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

void EditorImGui::RenderPrefabOverrideControls(const std::string& assetId,
                                               const PrefabInstanceState& instance,
                                               const std::vector<std::string>& overrides)
{
    const std::string effectiveAssetId = !instance.assetId.empty() ? instance.assetId : assetId;
    std::string assetLabel = effectiveAssetId.empty() ? std::string("<missing>") : effectiveAssetId;
    if (m_assetLibrary)
    {
        if (const auto entry = m_assetLibrary->FindById(effectiveAssetId))
            assetLabel = entry->displayName.empty() ? entry->filename : entry->displayName;
    }

    ImGui::TextUnformatted("Linked Prefab Instance");
    ImGui::TextDisabled("Asset: %s", assetLabel.c_str());
    ImGui::TextDisabled("Asset ID: %s", effectiveAssetId.c_str());
    ImGui::TextDisabled("Local ID: %u", instance.localId == 0 ? 1u : instance.localId);
    ImGui::TextDisabled("Overrides: %zu", overrides.size());
    ImGui::Separator();

    if (overrides.empty())
    {
        ImGui::TextDisabled("No instance overrides.");
    }
    else
    {
        for (const std::string& overrideName : overrides)
        {
            ImGui::PushID(overrideName.c_str());
            ImGui::BulletText("%s", overrideName.c_str());
            if (overrideName != "Prefab asset missing")
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Revert"))
                {
                    m_commands.revertSelectedPrefabOverride = true;
                    m_commands.selectedPrefabOverrideName = overrideName;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Apply"))
                {
                    m_commands.applySelectedPrefabOverrideToAsset = true;
                    m_commands.selectedPrefabOverrideName = overrideName;
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::Separator();
    if (UI::IconButton(ICON_FA_ROTATE, "Refresh Selected Instance", ImVec2(-1.0f, 0.0f)))
        m_commands.refreshSelectedPrefabInstance = true;
    if (UI::IconButton(ICON_FA_ROTATE, "Revert All Overrides", ImVec2(-1.0f, 0.0f)))
        m_commands.revertSelectedPrefabInstance = true;
    if (UI::IconButton(ICON_FA_FLOPPY_DISK, "Apply All Overrides to Prefab", ImVec2(-1.0f, 0.0f)))
        m_commands.applySelectedPrefabToAsset = true;
    if (UI::IconButton(ICON_FA_ROTATE, "Refresh All Prefab Instances", ImVec2(-1.0f, 0.0f)))
        m_commands.refreshAllPrefabInstances = true;
    if (UI::IconButton(ICON_FA_LAYER_GROUP, "Unpack Prefab Instance", ImVec2(-1.0f, 0.0f)))
        m_commands.unpackSelectedPrefabInstance = true;
}

void EditorImGui::RenderSelectedWaterBodyInspector()
{
    if (!m_waterBodyState.selected)
        return;

    RenderInspectorTitle(ICON_FA_DROPLET, "Water Body", m_waterBodyState.id);

    char nameBuffer[96]{};
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_waterBodyState.name.c_str());
    if (UI::Prop::InputText("Name", nameBuffer, sizeof(nameBuffer)))
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

    if (!ImGui::CollapsingHeader(ICON_FA_WATER " Water Body", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
        return;
    const std::string materialLabel = m_waterBodyState.materialName.empty()
        ? (m_waterBodyState.materialId.empty() ? std::string("Inline Water") : m_waterBodyState.materialId)
        : m_waterBodyState.materialName;
    UI::AssetField("Material", ICON_FA_WATER, materialLabel, "The water material. Drop a water material from the Asset Browser here to change it.");
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

    if (ImGui::CollapsingHeader(ICON_FA_PAINTBRUSH "  Shape", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
    {
        if (!CanUseEditorTools())
            ImGui::BeginDisabled();
        RenderWaterSculptTool();
        if (!CanUseEditorTools())
            ImGui::EndDisabled();
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
    RenderInspectorTitle(isSpot ? ICON_FA_BULLSEYE : ICON_FA_LIGHTBULB, isSpot ? "Spot Light" : "Point Light", m_dynamicLightState.id);

    if (isPoint)
    {
        PointLight& point = m_dynamicLightState.point;
        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", PointLightDisplayName(point).c_str());
        if (UI::Prop::InputText("Name", nameBuffer, sizeof(nameBuffer)))
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

        if (!ImGui::CollapsingHeader(ICON_FA_LIGHTBULB " Point Light", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
            return;
        bool changed = false;
        changed |= UI::Prop::Checkbox("Enabled", &point.enabled);
        changed |= UI::Prop::ColorEdit3("Color", &point.r);
        changed |= UI::Prop::SliderFloat("Intensity", &point.intensity, 0.0f, 10.0f, "%.2f");
        changed |= UI::Prop::SliderFloat("Radius", &point.radius, 0.5f, 100.0f, "%.1f m");
        if (changed)
            MarkSelectedLightChanged();

        if (!point.prefabAssetId.empty() &&
            ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Prefab", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
        {
            RenderPrefabOverrideControls(point.prefabAssetId, point.prefabInstance, m_dynamicLightState.prefabOverrides);
        }
    }
    else if (isSpot)
    {
        SpotLight& spot = m_dynamicLightState.spot;
        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", SpotLightDisplayName(spot).c_str());
        if (UI::Prop::InputText("Name", nameBuffer, sizeof(nameBuffer)))
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

        if (!ImGui::CollapsingHeader(ICON_FA_BULLSEYE " Spot Light", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
            return;
        bool changed = false;
        changed |= UI::Prop::Checkbox("Enabled", &spot.enabled);
        changed |= UI::Prop::ColorEdit3("Color", &spot.r);
        changed |= UI::Prop::SliderFloat("Intensity", &spot.intensity, 0.0f, 10.0f, "%.2f");
        changed |= UI::Prop::SliderFloat("Radius", &spot.radius, 0.5f, 100.0f, "%.1f m");
        float pitch = spot.rotation[0] * 57.2957795f;
        float yaw = spot.rotation[1] * 57.2957795f;
        if (UI::Prop::DragFloat("Pitch", &pitch, 0.5f, 0.0f, 0.0f, "%.1f deg"))
        {
            spot.rotation[0] = pitch / 57.2957795f;
            changed = true;
        }
        if (UI::Prop::DragFloat("Yaw", &yaw, 0.5f, 0.0f, 0.0f, "%.1f deg"))
        {
            spot.rotation[1] = yaw / 57.2957795f;
            changed = true;
        }
        changed |= UI::Prop::SliderFloat("Inner Cone", &spot.innerConeDegrees, 1.0f, 89.0f, "%.1f deg");
        changed |= UI::Prop::SliderFloat("Outer Cone", &spot.outerConeDegrees, 1.0f, 90.0f, "%.1f deg");
        spot.outerConeDegrees = std::max(spot.outerConeDegrees, spot.innerConeDegrees);
        if (changed)
            MarkSelectedLightChanged();

        if (!spot.prefabAssetId.empty() &&
            ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Prefab", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
        {
            RenderPrefabOverrideControls(spot.prefabAssetId, spot.prefabInstance, m_dynamicLightState.prefabOverrides);
        }
    }

    ImGui::Separator();
    RenderAddComponentMenu();
    ImGui::Separator();
    if (UI::IconButton(ICON_FA_TRASH, "Delete Light", ImVec2(-1.0f, 0.0f)))
        m_commands.deleteSelectedLight = true;
}

bool EditorImGui::RenderSelectedMeshPhysicsComponents()
{
    bool changed = false;
    auto componentMenu = [&](const char* popupId, const char* componentId) {
        if (UI::HeaderMenuButton("Component options"))
            ImGui::OpenPopup(popupId);
        if (ImGui::BeginPopup(popupId))
        {
            if (ImGui::MenuItem("Remove Component"))
            {
                m_commands.removeComponentFromSelectedEntity = true;
                m_commands.removeComponentTypeId = componentId;
            }
            ImGui::EndPopup();
        }
    };

    if (m_meshRendererState.hasRigidbody)
    {
        ImGui::PushID("physics.rigidbody");
        const bool open = ImGui::CollapsingHeader(ICON_FA_CUBE " Rigidbody", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        componentMenu("RigidbodyComponentMenu", "physics.rigidbody");
        if (open)
        {
            auto& body = m_meshRendererState.rigidbody;
            const char* bodyTypes[] = {"Static", "Dynamic", "Kinematic"};
            int bodyTypeIndex = body.bodyType == ixtreeme::physics::BodyType::Static ? 0 :
                (body.bodyType == ixtreeme::physics::BodyType::Kinematic ? 2 : 1);
            if (UI::Prop::Combo("Body Type", &bodyTypeIndex, bodyTypes, IM_ARRAYSIZE(bodyTypes)))
            {
                body.bodyType = bodyTypeIndex == 0 ? ixtreeme::physics::BodyType::Static :
                    (bodyTypeIndex == 2 ? ixtreeme::physics::BodyType::Kinematic : ixtreeme::physics::BodyType::Dynamic);
                changed = true;
            }
            changed |= UI::Prop::Checkbox("Enabled", &body.enabled);
            changed |= UI::Prop::Checkbox("Use Gravity", &body.useGravity);
            changed |= UI::Prop::Checkbox("Allow Sleeping", &body.allowSleeping);
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Sleeping lets inactive bodies stop simulating until they are touched or moved.");
                ImGui::EndTooltip();
            }
            changed |= UI::Prop::Checkbox("Continuous Collision", &body.continuousCollision);
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Uses linear-cast motion quality for fast dynamic bodies to reduce tunneling.");
                ImGui::EndTooltip();
            }
            changed |= UI::Prop::DragFloat("Mass", &body.mass, 0.05f, 0.001f, 100000.0f, "%.3f");
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Dynamic body mass in kilograms. Static and kinematic bodies ignore mass.");
                ImGui::EndTooltip();
            }
            changed |= UI::Prop::DragFloat("Linear Damping", &body.linearDamping, 0.01f, 0.0f, 100.0f, "%.3f");
            changed |= UI::Prop::DragFloat("Angular Damping", &body.angularDamping, 0.01f, 0.0f, 100.0f, "%.3f");
            const auto axisToggles = [&](const char* label, const char* id, bool* axes) {
                UI::Property(label, [&](const char*) {
                    ImGui::PushID(id);
                    changed |= ImGui::Checkbox("X", &axes[0]);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("Y", &axes[1]);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("Z", &axes[2]);
                    ImGui::PopID();
                    return false;
                });
            };
            axisToggles("Freeze Position", "freeze_pos", body.freezePosition);
            axisToggles("Freeze Rotation", "freeze_rot", body.freezeRotation);
            ixtreeme::physics::Sanitize(body);

            // Live values and test pushes only matter while Play runs: folded away by default.
            if (ImGui::TreeNodeEx("Live physics (Play mode)", ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                if (m_meshRendererState.physicsRuntimeValid)
                {
                    const float* linear = m_meshRendererState.physicsRuntimeLinearVelocity;
                    const float* angular = m_meshRendererState.physicsRuntimeAngularVelocity;
                    const float speed = std::sqrt(
                        linear[0] * linear[0] +
                        linear[1] * linear[1] +
                        linear[2] * linear[2]);
                    ImGui::TextDisabled("Body: %llu  %s",
                        static_cast<unsigned long long>(m_meshRendererState.physicsRuntimeBodyId),
                        m_meshRendererState.physicsRuntimeActive ? "active" : "sleeping");
                    ImGui::Text("Linear:  %.3f, %.3f, %.3f  | speed %.3f m/s",
                        linear[0],
                        linear[1],
                        linear[2],
                        speed);
                    ImGui::Text("Angular: %.3f, %.3f, %.3f rad/s",
                        angular[0],
                        angular[1],
                        angular[2]);
                }
                else
                {
                    ImGui::TextDisabled("Runtime body unavailable. Enter Play mode to inspect live physics.");
                }
    
                ImGui::SeparatorText("Runtime Test");
                UI::Prop::DragFloat3("Linear Velocity", m_physicsTestLinearVelocity, 0.1f, -1000.0f, 1000.0f, "%.2f");
                if (ImGui::Button("Set Velocity"))
                {
                    m_commands.physicsSetLinearVelocityForSelected = true;
                    m_commands.physicsRuntimeEntityId = m_meshRendererState.id;
                    std::copy(std::begin(m_physicsTestLinearVelocity), std::end(m_physicsTestLinearVelocity), std::begin(m_commands.physicsLinearVelocity));
                }
                UI::Prop::DragFloat3("Force", m_physicsTestForce, 0.5f, -100000.0f, 100000.0f, "%.2f");
                if (ImGui::Button("Apply Force"))
                {
                    m_commands.physicsApplyForceToSelected = true;
                    m_commands.physicsRuntimeEntityId = m_meshRendererState.id;
                    std::copy(std::begin(m_physicsTestForce), std::end(m_physicsTestForce), std::begin(m_commands.physicsForce));
                }
                UI::Prop::DragFloat3("Impulse", m_physicsTestImpulse, 0.1f, -10000.0f, 10000.0f, "%.2f");
                if (ImGui::Button("Apply Impulse"))
                {
                    m_commands.physicsApplyImpulseToSelected = true;
                    m_commands.physicsRuntimeEntityId = m_meshRendererState.id;
                    std::copy(std::begin(m_physicsTestImpulse), std::end(m_physicsTestImpulse), std::begin(m_commands.physicsImpulse));
                }
                UI::Prop::DragFloat3("Angular Impulse", m_physicsTestAngularImpulse, 0.1f, -10000.0f, 10000.0f, "%.2f");
                if (ImGui::Button("Apply Angular Impulse"))
                {
                    m_commands.physicsApplyAngularImpulseToSelected = true;
                    m_commands.physicsRuntimeEntityId = m_meshRendererState.id;
                    std::copy(std::begin(m_physicsTestAngularImpulse), std::end(m_physicsTestAngularImpulse), std::begin(m_commands.physicsAngularImpulse));
                }
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }

    if (m_meshRendererState.hasFixedJoint)
    {
        ImGui::PushID("physics.fixed_joint");
        const bool open = ImGui::CollapsingHeader(ICON_FA_GEAR " Fixed Joint", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        componentMenu("FixedJointComponentMenu", "physics.fixed_joint");
        if (open)
        {
            auto& joint = m_meshRendererState.fixedJoint;
            ImGui::TextDisabled("Add this to one body only; Connected Entity is the other body.");
            changed |= UI::Prop::Checkbox("Enabled", &joint.enabled);
            int connectedEntity = static_cast<int>(joint.connectedEntityId);
            if (UI::Prop::InputInt("Connected Entity ID", &connectedEntity))
            {
                joint.connectedEntityId = static_cast<std::uint32_t>(std::max(0, connectedEntity));
                changed = true;
            }
            if (joint.connectedEntityId == 0)
                ImGui::TextDisabled("Pick another mesh entity with a Rigidbody before entering Play.");
            else
                ImGui::TextDisabled("Connects this Rigidbody to entity %u in Play mode.", joint.connectedEntityId);
        }
        ImGui::PopID();
    }

    if (m_meshRendererState.hasHingeJoint)
    {
        ImGui::PushID("physics.hinge_joint");
        const bool open = ImGui::CollapsingHeader(ICON_FA_GEAR " Hinge Joint", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        componentMenu("HingeJointComponentMenu", "physics.hinge_joint");
        if (open)
        {
            auto& joint = m_meshRendererState.hingeJoint;
            ImGui::TextDisabled("Add this to one body only; Connected Entity is the other body.");
            changed |= UI::Prop::Checkbox("Enabled", &joint.enabled);
            int connectedEntity = static_cast<int>(joint.connectedEntityId);
            if (UI::Prop::InputInt("Connected Entity ID", &connectedEntity))
            {
                joint.connectedEntityId = static_cast<std::uint32_t>(std::max(0, connectedEntity));
                changed = true;
            }
            changed |= UI::Prop::DragFloat3("Anchor", joint.anchor, 0.05f, -10000.0f, 10000.0f, "%.2f");
            changed |= UI::Prop::DragFloat3("Axis", joint.axis, 0.02f, -1.0f, 1.0f, "%.2f");
            changed |= UI::Prop::Checkbox("Limits", &joint.limitsEnabled);
            if (joint.limitsEnabled)
            {
                changed |= UI::Prop::DragFloat("Min Angle", &joint.minAngleDegrees, 0.5f, -180.0f, 0.0f, "%.1f deg");
                changed |= UI::Prop::DragFloat("Max Angle", &joint.maxAngleDegrees, 0.5f, 0.0f, 180.0f, "%.1f deg");
            }
            changed |= UI::Prop::DragFloat("Friction Torque", &joint.frictionTorque, 0.1f, 0.0f, 100000.0f, "%.2f");
            joint.frictionTorque = std::max(0.0f, joint.frictionTorque);
            if (joint.connectedEntityId == 0)
                ImGui::TextDisabled("Pick another mesh entity with a Rigidbody before entering Play.");
            else
                ImGui::TextDisabled("Allows rotation around the axis between this body and entity %u.", joint.connectedEntityId);
        }
        ImGui::PopID();
    }

    if (m_meshRendererState.skinned)
    {
        ImGui::PushID("animation.debug_clip");
        if (ImGui::CollapsingHeader(ICON_FA_PERSON_RUNNING " Animation (debug)", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
        {
            std::vector<AssetLibrary::Entry> clips;
            if (m_assetLibrary)
                clips = m_assetLibrary->EntriesFor(AssetLibrary::Category::AnimationClip);

            std::vector<const char*> labels;
            labels.reserve(clips.size() + 1);
            labels.push_back("(none)");
            int currentIndex = 0;
            for (std::size_t i = 0; i < clips.size(); ++i)
            {
                labels.push_back(clips[i].displayName.c_str());
                if (clips[i].id == m_meshRendererState.debugAnimationClipId)
                    currentIndex = static_cast<int>(i) + 1;
            }
            if (UI::Prop::Combo("Clip", &currentIndex, labels.data(), static_cast<int>(labels.size())))
            {
                m_meshRendererState.debugAnimationClipId =
                    (currentIndex <= 0) ? std::string() : clips[static_cast<std::size_t>(currentIndex - 1)].id;
                changed = true;
            }
            ImGui::TextDisabled("Debug single clip (Stage 3). The Animator Controller below");
            ImGui::TextDisabled("overrides it when assigned.");

            ImGui::Separator();
            // Stage-4 Animator Controller (state machine; takes priority over the debug clip).
            std::vector<AssetLibrary::Entry> controllers;
            if (m_assetLibrary)
                controllers = m_assetLibrary->EntriesFor(AssetLibrary::Category::AnimatorController);
            std::vector<const char*> ctrlLabels;
            ctrlLabels.reserve(controllers.size() + 1);
            ctrlLabels.push_back("(none)");
            int ctrlIndex = 0;
            for (std::size_t i = 0; i < controllers.size(); ++i)
            {
                ctrlLabels.push_back(controllers[i].displayName.c_str());
                if (controllers[i].id == m_meshRendererState.animatorControllerId)
                    ctrlIndex = static_cast<int>(i) + 1;
            }
            if (UI::Prop::Combo("Controller", &ctrlIndex, ctrlLabels.data(), static_cast<int>(ctrlLabels.size())))
            {
                m_meshRendererState.animatorControllerId =
                    (ctrlIndex <= 0) ? std::string() : controllers[static_cast<std::size_t>(ctrlIndex - 1)].id;
                changed = true;
            }
            if (ImGui::Button("Create Locomotion Controller") && m_assetLibrary)
            {
                AssetLibrary::ImportOptions opts;
                opts.displayName = "Locomotion";
                opts.subpath = m_assetSubpath;  // the folder the asset browser shows
                AssetLibrary::Entry created;
                std::string createErr;
                if (m_assetLibrary->CreateAnimatorController(opts, created, createErr))
                {
                    m_meshRendererState.animatorControllerId = created.id;
                    changed = true;
                    m_assetStatus = "Created controller " + created.displayName;
                }
                else
                {
                    m_assetStatus = "Create controller failed: " + createErr;
                }
            }
            if (!m_meshRendererState.animatorControllerId.empty())
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_PERSON_RUNNING " Open in Animator"))
                {
                    m_animatorPanelOpen = true;
                    m_pendingViewFocusWindow = EditorWindow::Animator;
                }
            }
            ImGui::TextDisabled("Drives idle/walk/run by Speed (auto-bound from movement in Play).");
            ImGui::TextDisabled("Empty states auto-fill with this character's <name>_anim_<i> clips.");
        }
        ImGui::PopID();
    }

    if (m_meshRendererState.hasCharacterController)
    {
        ImGui::PushID("physics.character_controller");
        const bool open = ImGui::CollapsingHeader(ICON_FA_PERSON_RUNNING " Character Controller", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        componentMenu("CharacterControllerComponentMenu", "physics.character_controller");
        if (open)
        {
            auto& cc = m_meshRendererState.characterController;
            changed |= UI::Prop::Checkbox("Enabled", &cc.enabled);
            ImGui::TextDisabled("Tags this entity as the player. WASD + Space drive it in Play.");

            const char* cameraModes[] = {"First Person", "Third Person", "Top Down"};
            int cameraModeIndex = std::clamp(static_cast<int>(cc.cameraMode), 0, 2);
            if (UI::Prop::Combo("Camera Mode", &cameraModeIndex, cameraModes, IM_ARRAYSIZE(cameraModes)))
            {
                cc.cameraMode = static_cast<ixtreeme::physics::CameraMode>(cameraModeIndex);
                changed = true;
            }

            ImGui::SeparatorText("Movement");
            changed |= UI::Prop::DragFloat("Walk Speed", &cc.walkSpeed, 0.1f, 0.1f, 100.0f, "%.1f m/s");
            changed |= UI::Prop::DragFloat("Run Speed", &cc.runSpeed, 0.1f, 0.1f, 100.0f, "%.1f m/s");
            changed |= UI::Prop::DragFloat("Jump Height", &cc.jumpHeight, 0.05f, 0.0f, 50.0f, "%.2f m");
            changed |= UI::Prop::DragFloat("Gravity Scale", &cc.gravityScale, 0.05f, 0.0f, 10.0f, "%.2f");
            changed |= UI::Prop::DragFloat("Slope Limit", &cc.slopeLimitDegrees, 0.5f, 1.0f, 89.0f, "%.0f deg");
            changed |= UI::Prop::DragFloat("Step Height", &cc.stepHeight, 0.01f, 0.0f, 5.0f, "%.2f m");

            ImGui::SeparatorText("Capsule");
            changed |= UI::Prop::DragFloat("Capsule Radius", &cc.capsuleRadius, 0.01f, 0.05f, 5.0f, "%.2f m");
            changed |= UI::Prop::DragFloat("Capsule Height", &cc.capsuleHeight, 0.05f, 0.2f, 10.0f, "%.2f m");

            ImGui::SeparatorText("Camera");
            changed |= UI::Prop::DragFloat("Mouse Sensitivity", &cc.mouseSensitivity, 0.01f, 0.01f, 2.0f, "%.2f");
            if (cc.cameraMode == ixtreeme::physics::CameraMode::FirstPerson)
            {
                changed |= UI::Prop::DragFloat("Eye Height", &cc.eyeHeight, 0.02f, 0.1f, 10.0f, "%.2f m");
            }
            else if (cc.cameraMode == ixtreeme::physics::CameraMode::ThirdPerson)
            {
                changed |= UI::Prop::DragFloat("Distance", &cc.thirdPersonDistance, 0.1f, 0.5f, 50.0f, "%.1f m");
                changed |= UI::Prop::DragFloat("Height", &cc.thirdPersonHeight, 0.05f, 0.0f, 50.0f, "%.2f m");
                changed |= UI::Prop::DragFloat("Pitch", &cc.thirdPersonPitchDegrees, 0.5f, -89.0f, 89.0f, "%.0f deg");
            }
            else
            {
                changed |= UI::Prop::DragFloat("Camera Height", &cc.topDownHeight, 0.2f, 1.0f, 200.0f, "%.1f m");
                changed |= UI::Prop::DragFloat("Camera Pitch", &cc.topDownPitchDegrees, 0.5f, 10.0f, 89.0f, "%.0f deg");
            }
            ixtreeme::physics::Sanitize(cc);
            ImGui::TextDisabled("Right-drag in the Game view to look around.");
        }
        ImGui::PopID();
    }

    if (m_meshRendererState.hasAudioSource)
    {
        ImGui::PushID("audio.audio_source");
        const bool open = ImGui::CollapsingHeader("Audio Source", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        componentMenu("AudioSourceComponentMenu", "audio.audio_source");
        if (open)
        {
            auto& a = m_meshRendererState.audioSource;

            if (m_assetLibrary)
            {
                const std::vector<AssetLibrary::Entry> clips =
                    m_assetLibrary->EntriesFor(AssetLibrary::Category::Audio);
                std::string preview = "(no clip)";
                for (const AssetLibrary::Entry& e : clips)
                    if (e.id == a.clipAssetId) { preview = e.displayName; break; }
                if (UI::Prop::BeginCombo("Clip", preview.c_str()))
                {
                    if (ImGui::Selectable("(no clip)", a.clipAssetId.empty())) { a.clipAssetId.clear(); changed = true; }
                    for (const AssetLibrary::Entry& e : clips)
                    {
                        const bool sel = (e.id == a.clipAssetId);
                        if (ImGui::Selectable(e.displayName.c_str(), sel)) { a.clipAssetId = e.id; changed = true; }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            changed |= UI::Prop::Checkbox("Enabled", &a.enabled);
            changed |= UI::Prop::Checkbox("Play On Start", &a.playOnStart);
            ImGui::SameLine();
            changed |= UI::Prop::Checkbox("Loop", &a.loop);
            ImGui::SameLine();
            changed |= UI::Prop::Checkbox("3D", &a.is3d);

            changed |= UI::Prop::SliderFloat("Volume", &a.volume, 0.0f, 1.0f, "%.2f");
            changed |= UI::Prop::SliderFloat("Pitch", &a.pitch, 0.5f, 2.0f, "%.2f");

            const char* buses[] = {"Master", "Music", "SFX"};
            int busIdx = std::clamp(static_cast<int>(a.bus), 0, 2);
            if (UI::Prop::Combo("Bus", &busIdx, buses, IM_ARRAYSIZE(buses)))
            {
                a.bus = static_cast<ixaudio::AudioBus>(busIdx);
                changed = true;
            }

            if (a.is3d)
            {
                ImGui::SeparatorText("3D");
                changed |= UI::Prop::DragFloat("Min Distance", &a.minDistance, 0.1f, 0.01f, 1000.0f, "%.2f m");
                changed |= UI::Prop::DragFloat("Max Distance", &a.maxDistance, 0.5f, 0.1f, 5000.0f, "%.1f m");
            }
            ixaudio::Sanitize(a);
            ImGui::TextDisabled("Plays in Play mode (Play On Start). Stops on Stop.");
        }
        ImGui::PopID();
    }

    if (m_meshRendererState.hasAudioListener)
    {
        ImGui::PushID("audio.audio_listener");
        const bool open = ImGui::CollapsingHeader("Audio Listener", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        componentMenu("AudioListenerComponentMenu", "audio.audio_listener");
        if (open)
        {
            changed |= UI::Prop::Checkbox("Enabled", &m_meshRendererState.audioListener.enabled);
            ImGui::TextDisabled("Puts the 3D \"ears\" at this entity. Without one, the camera listens.");
        }
        ImGui::PopID();
    }

    if (m_meshRendererState.hasScript)
    {
        ImGui::PushID("scripting.script");
        const bool open = ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        componentMenu("ScriptComponentMenu", "scripting.script");
        if (open)
        {
            auto& sc = m_meshRendererState.script;
            changed |= UI::Prop::Checkbox("Enabled", &sc.enabled);

            const char* backends[] = {"Native (C++)", "Lua"};
            int backendIdx = (sc.backend == ixscript::ScriptBackendType::Lua) ? 1 : 0;
            if (UI::Prop::Combo("Backend", &backendIdx, backends, IM_ARRAYSIZE(backends)))
            {
                sc.backend = (backendIdx == 1) ? ixscript::ScriptBackendType::Lua
                                               : ixscript::ScriptBackendType::Native;
                changed = true;
            }

            if (sc.backend == ixscript::ScriptBackendType::Native)
            {
                const std::vector<std::string> classes = ixscript::NativeBackend::RegisteredNames();
                const std::string preview = sc.nativeClassName.empty() ? "(class)" : sc.nativeClassName;
                if (UI::Prop::BeginCombo("Class", preview.c_str()))
                {
                    for (const std::string& cls : classes)
                        if (ImGui::Selectable(cls.c_str(), cls == sc.nativeClassName))
                        {
                            sc.nativeClassName = cls;
                            changed = true;
                        }
                    ImGui::EndCombo();
                }
                if (classes.empty())
                    ImGui::TextDisabled("No native scripts registered (IXSCRIPT_REGISTER).");
            }
            else if (m_assetLibrary)  // Lua: pick a .lua asset from the project library
            {
                const std::vector<AssetLibrary::Entry> scripts =
                    m_assetLibrary->EntriesFor(AssetLibrary::Category::Script);
                std::string preview = sc.scriptAssetId.empty() ? "(no script)" : sc.scriptAssetId;
                for (const AssetLibrary::Entry& e : scripts)
                    if (e.id == sc.scriptAssetId) { preview = e.displayName; break; }
                // Label must differ from the "Script" CollapsingHeader above (same PushID scope → ID clash).
                if (UI::Prop::BeginCombo("Lua Script", preview.c_str()))
                {
                    if (ImGui::Selectable("(no script)", sc.scriptAssetId.empty())) { sc.scriptAssetId.clear(); changed = true; }
                    for (const AssetLibrary::Entry& e : scripts)
                    {
                        const bool sel = (e.id == sc.scriptAssetId);
                        if (ImGui::Selectable(e.displayName.c_str(), sel)) { sc.scriptAssetId = e.id; changed = true; }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (scripts.empty())
                    ImGui::TextDisabled("No .lua scripts in the project. Drop a .lua into the asset browser.");
            }

            // Reflected fields (Unity-[SerializeField] style): if the chosen native class declares fields
            // via IX_REFLECT, show typed widgets bound to parameters[name]; an unedited field uses its
            // code default (no key stored). Lua and schema-less native classes fall back to the manual
            // key/value table below.
            std::vector<ixscript::ScriptFieldDesc> fieldSchema;
            if (sc.backend == ixscript::ScriptBackendType::Native && !sc.nativeClassName.empty())
                fieldSchema = ixscript::NativeBackend::DescribeFields(sc.nativeClassName);

            if (!fieldSchema.empty())
            {
                ImGui::SeparatorText("Script Fields");
                // Parse up to n comma-separated floats (no sscanf — C4996; matches the runtime ApplyBinder).
                auto parseFloats = [](const std::string& s, float* out, int n) {
                    const char* p = s.c_str();
                    for (int i = 0; i < n && *p; ++i)
                    {
                        char* end = nullptr;
                        out[i] = std::strtof(p, &end);
                        if (end == p)
                            break;
                        p = end;
                        while (*p == ',' || *p == ' ')
                            ++p;
                    }
                };
                for (const ixscript::ScriptFieldDesc& fd : fieldSchema)
                {
                    ImGui::PushID(fd.name.c_str());
                    const auto existing = sc.parameters.find(fd.name);
                    const std::string current =
                        (existing != sc.parameters.end()) ? existing->second : fd.defaultValue;
                    char buf[160];
                    switch (fd.type)
                    {
                    case ixscript::ScriptFieldType::Float:
                    {
                        float v = std::strtof(current.c_str(), nullptr);
                        if (UI::Prop::DragFloat(fd.name.c_str(), &v, 0.1f, 0.0f, 0.0f, "%.3f"))
                        {
                            std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
                            sc.parameters[fd.name] = buf;
                            changed = true;
                        }
                        break;
                    }
                    case ixscript::ScriptFieldType::Int:
                    {
                        int v = std::atoi(current.c_str());
                        if (UI::Prop::DragInt(fd.name.c_str(), &v, 0.1f))
                        {
                            sc.parameters[fd.name] = std::to_string(v);
                            changed = true;
                        }
                        break;
                    }
                    case ixscript::ScriptFieldType::Bool:
                    {
                        bool v = (current == "1" || current == "true" || current == "True");
                        if (UI::Prop::Checkbox(fd.name.c_str(), &v))
                        {
                            sc.parameters[fd.name] = v ? "1" : "0";
                            changed = true;
                        }
                        break;
                    }
                    case ixscript::ScriptFieldType::Vec3:
                    {
                        float v[3] = {0.0f, 0.0f, 0.0f};
                        parseFloats(current, v, 3);
                        if (UI::Prop::DragFloat3(fd.name.c_str(), v, 0.1f, 0.0f, 0.0f, "%.3f"))
                        {
                            std::snprintf(buf, sizeof(buf), "%g,%g,%g",
                                static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]));
                            sc.parameters[fd.name] = buf;
                            changed = true;
                        }
                        break;
                    }
                    case ixscript::ScriptFieldType::Color:
                    {
                        float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        parseFloats(current, v, 4);
                        if (UI::Prop::ColorEdit4(fd.name.c_str(), v))
                        {
                            std::snprintf(buf, sizeof(buf), "%g,%g,%g,%g", static_cast<double>(v[0]),
                                static_cast<double>(v[1]), static_cast<double>(v[2]), static_cast<double>(v[3]));
                            sc.parameters[fd.name] = buf;
                            changed = true;
                        }
                        break;
                    }
                    }
                    ImGui::PopID();
                }
                ImGui::TextDisabled("Fields declared in C++ via IX_REFLECT (defaults from code).");
            }
            else
            {
                // Exposed string parameters (key/value), read by the script via Param/ParamFloat or
                // (Lua) self.params. Fallback for Lua + native classes that declare no IX_REFLECT fields.
                ImGui::SeparatorText("Parameters");
                std::string removeKey;
                for (auto& kv : sc.parameters)
                {
                    ImGui::PushID(kv.first.c_str());
                    ImGui::TextUnformatted(kv.first.c_str());
                    ImGui::SameLine(140.0f);
                    char valueBuf[128];
                    std::snprintf(valueBuf, sizeof(valueBuf), "%s", kv.second.c_str());
                    ImGui::SetNextItemWidth(150.0f);
                    if (UI::Prop::InputText("##val", valueBuf, sizeof(valueBuf)))
                    {
                        kv.second = valueBuf;
                        changed = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X"))
                        removeKey = kv.first;
                    ImGui::PopID();
                }
                if (!removeKey.empty())
                {
                    sc.parameters.erase(removeKey);
                    changed = true;
                }
                static char s_newParamKey[64] = {};
                ImGui::SetNextItemWidth(120.0f);
                UI::Prop::InputText("##newparam", s_newParamKey, sizeof(s_newParamKey));
                ImGui::SameLine();
                if (ImGui::SmallButton("+ Add Param") && s_newParamKey[0] != '\0')
                {
                    sc.parameters[s_newParamKey] = "0";
                    s_newParamKey[0] = '\0';
                    changed = true;
                }
            }
        }
        ImGui::PopID();
    }

    if (m_meshRendererState.hasCollider)
    {
        ImGui::PushID("physics.collider");
        const bool open = ImGui::CollapsingHeader(ICON_FA_CUBE " Collider", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        componentMenu("ColliderComponentMenu", "physics.collider");
        if (open)
        {
            auto& collider = m_meshRendererState.collider;
            auto requestColliderFit = [&]() {
                m_commands.fitSelectedColliderToMesh = true;
                m_commands.selectedMeshEntityChanged = true;
                m_commands.selectedMeshEntity = m_meshRendererState;
            };
            const char* shapes[] = {"Box", "Sphere", "Capsule", "Mesh", "Convex Hull"};
            int shapeIndex = collider.shape == ixtreeme::physics::ColliderShape::Sphere ? 1 :
                (collider.shape == ixtreeme::physics::ColliderShape::Capsule ? 2 :
                (collider.shape == ixtreeme::physics::ColliderShape::Mesh ? 3 :
                (collider.shape == ixtreeme::physics::ColliderShape::ConvexHull ? 4 : 0)));
            if (UI::Prop::Combo("Shape", &shapeIndex, shapes, IM_ARRAYSIZE(shapes)))
            {
                collider.shape = shapeIndex == 1 ? ixtreeme::physics::ColliderShape::Sphere :
                    (shapeIndex == 2 ? ixtreeme::physics::ColliderShape::Capsule :
                    (shapeIndex == 3 ? ixtreeme::physics::ColliderShape::Mesh :
                    (shapeIndex == 4 ? ixtreeme::physics::ColliderShape::ConvexHull : ixtreeme::physics::ColliderShape::Box)));
                changed = true;
            }
            changed |= UI::Prop::Checkbox("Enabled", &collider.enabled);
            changed |= UI::Prop::Checkbox("Is Trigger", &collider.trigger);
            UI::Prop::Checkbox("Edit Collider In Scene", &m_editSelectedColliderInScene);
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Shows the Scene View gizmo at the collider center. Translate moves the collider offset, not the object.");
                ImGui::EndTooltip();
            }
            const char* layerNames[] = {
                ixtreeme::physics::DisplayName(ixtreeme::physics::PhysicsLayer::Default),
                ixtreeme::physics::DisplayName(ixtreeme::physics::PhysicsLayer::StaticWorld),
                ixtreeme::physics::DisplayName(ixtreeme::physics::PhysicsLayer::DynamicObject),
                ixtreeme::physics::DisplayName(ixtreeme::physics::PhysicsLayer::Player),
                ixtreeme::physics::DisplayName(ixtreeme::physics::PhysicsLayer::Trigger),
                ixtreeme::physics::DisplayName(ixtreeme::physics::PhysicsLayer::Projectile),
                ixtreeme::physics::DisplayName(ixtreeme::physics::PhysicsLayer::Foliage),
                ixtreeme::physics::DisplayName(ixtreeme::physics::PhysicsLayer::NoCollision),
            };
            int layerIndex = static_cast<int>(ixtreeme::physics::PhysicsLayerIndex(collider.layer));
            layerIndex = std::clamp(layerIndex, 0, static_cast<int>(ixtreeme::physics::PhysicsLayerCount()) - 1);
            if (UI::Prop::Combo("Physics Layer", &layerIndex, layerNames, IM_ARRAYSIZE(layerNames)))
            {
                collider.layer = static_cast<ixtreeme::physics::PhysicsLayer>(layerIndex);
                changed = true;
            }
            if (m_assetLibrary)
            {
                std::vector<AssetLibrary::Entry> physicsMaterials =
                    m_assetLibrary->EntriesFor(AssetLibrary::Category::PhysicsMaterial);
                int selectedPhysicsMaterial = 0;
                std::vector<std::string> physicsMaterialNames;
                physicsMaterialNames.reserve(physicsMaterials.size() + 1);
                physicsMaterialNames.push_back("None");
                for (std::size_t i = 0; i < physicsMaterials.size(); ++i)
                {
                    physicsMaterialNames.push_back(physicsMaterials[i].displayName.empty()
                        ? physicsMaterials[i].id
                        : physicsMaterials[i].displayName);
                    if (physicsMaterials[i].id == collider.materialAssetId)
                        selectedPhysicsMaterial = static_cast<int>(i + 1);
                }
                const auto comboPreview = [&]() -> const char* {
                    if (selectedPhysicsMaterial < 0 ||
                        selectedPhysicsMaterial >= static_cast<int>(physicsMaterialNames.size()))
                    {
                        return "None";
                    }
                    return physicsMaterialNames[static_cast<std::size_t>(selectedPhysicsMaterial)].c_str();
                };
                if (UI::Prop::BeginCombo("Physics Material", comboPreview()))
                {
                    if (ImGui::Selectable("None", selectedPhysicsMaterial == 0))
                    {
                        collider.materialAssetId.clear();
                        changed = true;
                    }
                    for (std::size_t i = 0; i < physicsMaterials.size(); ++i)
                    {
                        const bool selectedMaterial = selectedPhysicsMaterial == static_cast<int>(i + 1);
                        if (ImGui::Selectable(physicsMaterialNames[i + 1].c_str(), selectedMaterial))
                        {
                            collider.materialAssetId = physicsMaterials[i].id;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (!collider.materialAssetId.empty())
                {
                    const auto material = m_assetLibrary->FindById(collider.materialAssetId);
                    if (material && material->category == AssetLibrary::Category::PhysicsMaterial)
                    {
                        ImGui::TextDisabled("Material: friction %.2f, bounce %.2f, density %.2f",
                            material->physicsMaterial.friction,
                            material->physicsMaterial.restitution,
                            material->physicsMaterial.density);
                        ImGui::TextDisabled("Combine: friction %s, bounce %s",
                            ixtreeme::physics::DisplayName(material->physicsMaterial.frictionCombine),
                            ixtreeme::physics::DisplayName(material->physicsMaterial.restitutionCombine));
                    }
                    else
                    {
                        ImGui::TextDisabled("Material: missing (%s)", collider.materialAssetId.c_str());
                    }
                }
            }
            ImGui::TextDisabled("Fit Collider");
            if (ImGui::Button("Box", ImVec2(92.0f, 0.0f)))
            {
                collider.shape = ixtreeme::physics::ColliderShape::Box;
                requestColliderFit();
            }
            ImGui::SameLine();
            if (ImGui::Button("Sphere", ImVec2(92.0f, 0.0f)))
            {
                collider.shape = ixtreeme::physics::ColliderShape::Sphere;
                requestColliderFit();
            }
            ImGui::SameLine();
            if (ImGui::Button("Capsule", ImVec2(92.0f, 0.0f)))
            {
                collider.shape = ixtreeme::physics::ColliderShape::Capsule;
                requestColliderFit();
            }
            ImGui::SameLine();
            if (ImGui::Button("Mesh", ImVec2(92.0f, 0.0f)))
            {
                collider.shape = ixtreeme::physics::ColliderShape::Mesh;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Hull", ImVec2(72.0f, 0.0f)))
            {
                collider.shape = ixtreeme::physics::ColliderShape::ConvexHull;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Center", ImVec2(64.0f, 0.0f)))
            {
                collider.center[0] = 0.0f;
                collider.center[1] = 0.0f;
                collider.center[2] = 0.0f;
                changed = true;
            }
            changed |= UI::Prop::DragFloat3("Center", collider.center, 0.05f, -1000.0f, 1000.0f, "%.2f");
            if (collider.shape == ixtreeme::physics::ColliderShape::Box)
                changed |= UI::Prop::DragFloat3("Size", collider.size, 0.05f, 0.001f, 10000.0f, "%.2f");
            else if (collider.shape == ixtreeme::physics::ColliderShape::Mesh)
                ImGui::TextDisabled("Mesh collider uses the render mesh triangles. Static/Kinematic recommended.");
            else if (collider.shape == ixtreeme::physics::ColliderShape::ConvexHull)
                ImGui::TextDisabled("Convex Hull uses the render mesh shape and is suitable for dynamic bodies.");
            else
                changed |= UI::Prop::DragFloat("Radius", &collider.radius, 0.025f, 0.001f, 10000.0f, "%.2f");
            if (collider.shape == ixtreeme::physics::ColliderShape::Capsule)
                changed |= UI::Prop::DragFloat("Height", &collider.height, 0.05f, 0.001f, 10000.0f, "%.2f");
            changed |= UI::Prop::SliderFloat("Friction", &collider.friction, 0.0f, 4.0f, "%.2f");
            changed |= UI::Prop::SliderFloat("Bounciness", &collider.restitution, 0.0f, 1.0f, "%.2f");
            ImGui::TextDisabled("Physics Presets");
            auto applyPhysicsPreset = [&](const char* name,
                float friction,
                float restitution,
                bool trigger,
                ixtreeme::physics::PhysicsLayer layer,
                float mass,
                float linearDamping,
                float angularDamping) {
                collider.friction = friction;
                collider.restitution = restitution;
                collider.trigger = trigger;
                collider.layer = layer;
                if (m_meshRendererState.hasRigidbody)
                {
                    auto& body = m_meshRendererState.rigidbody;
                    body.mass = mass;
                    body.linearDamping = linearDamping;
                    body.angularDamping = angularDamping;
                    if (trigger)
                        body.useGravity = false;
                    ixtreeme::physics::Sanitize(body);
                }
                changed = true;
                Tracenf("[PHYSICS] preset entity=%u name=%s friction=%.2f bounce=%.2f trigger=%d",
                    m_meshRendererState.id,
                    name,
                    friction,
                    restitution,
                    trigger ? 1 : 0);
            };
            if (ImGui::Button("Default", ImVec2(78.0f, 0.0f)))
                applyPhysicsPreset("Default", 0.60f, 0.00f, false, ixtreeme::physics::PhysicsLayer::Default, 1.0f, 0.05f, 0.05f);
            ImGui::SameLine();
            if (ImGui::Button("Bouncy", ImVec2(78.0f, 0.0f)))
                applyPhysicsPreset("Bouncy", 0.40f, 0.85f, false, ixtreeme::physics::PhysicsLayer::DynamicObject, 1.0f, 0.02f, 0.02f);
            ImGui::SameLine();
            if (ImGui::Button("Ice", ImVec2(78.0f, 0.0f)))
                applyPhysicsPreset("Ice", 0.02f, 0.02f, false, ixtreeme::physics::PhysicsLayer::DynamicObject, 1.0f, 0.0f, 0.0f);
            if (ImGui::Button("Heavy", ImVec2(78.0f, 0.0f)))
                applyPhysicsPreset("Heavy", 0.90f, 0.05f, false, ixtreeme::physics::PhysicsLayer::DynamicObject, 20.0f, 0.10f, 0.10f);
            ImGui::SameLine();
            if (ImGui::Button("Trigger", ImVec2(78.0f, 0.0f)))
                applyPhysicsPreset("Trigger", 0.60f, 0.00f, true, ixtreeme::physics::PhysicsLayer::Trigger, 1.0f, 0.05f, 0.05f);
            ixtreeme::physics::Sanitize(collider);
        }
        ImGui::PopID();
    }

    if (changed)
    {
        Tracenf("[PHYSICS] inspector entity=%u rigidbody=%d collider=%d",
            m_meshRendererState.id,
            m_meshRendererState.hasRigidbody ? 1 : 0,
            m_meshRendererState.hasCollider ? 1 : 0);
    }
    return changed;
}

void EditorImGui::RenderSelectedCameraInspector()
{
    if (!m_cameraEditorState.selected)
        return;

    CameraEntity& camera = m_cameraEditorState.camera;
    RenderInspectorTitle(ICON_FA_VIDEO, "Camera", camera.id);

    char nameBuffer[96]{};
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", camera.name.c_str());
    if (UI::Prop::InputText("Name", nameBuffer, sizeof(nameBuffer)))
    {
        camera.name = nameBuffer;
        MarkSelectedCameraChanged();
    }

    float position[3] = {camera.position[0], camera.position[1], camera.position[2]};
    float rotation[3] = {camera.rotation[0], camera.rotation[1], camera.rotation[2]};
    if (RenderTransformComponent(position, rotation, nullptr))
    {
        camera.position[0] = position[0];
        camera.position[1] = position[1];
        camera.position[2] = position[2];
        camera.rotation[0] = rotation[0];
        camera.rotation[1] = rotation[1];
        camera.rotation[2] = rotation[2];
        MarkSelectedCameraChanged();
    }

    if (ImGui::CollapsingHeader(ICON_FA_EYE " Camera", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
    {
        bool changed = false;
        changed |= UI::Prop::SliderFloat("Field of View", &camera.fovDegrees, 10.0f, 120.0f, "%.1f deg");
        changed |= UI::Prop::DragFloat("Near Plane", &camera.nearPlane, 0.01f, 0.01f, 100.0f, "%.2f");
        changed |= UI::Prop::DragFloat("Far Plane", &camera.farPlane, 1.0f, 1.0f, 100000.0f, "%.1f");
        if (changed)
        {
            camera.fovDegrees = std::clamp(camera.fovDegrees, 1.0f, 179.0f);
            camera.nearPlane = std::max(0.001f, camera.nearPlane);
            camera.farPlane = std::max(camera.nearPlane + 0.001f, camera.farPlane);
            MarkSelectedCameraChanged();
        }

        ImGui::Spacing();
        if (m_cameraEditorState.isMain)
        {
            ImGui::TextDisabled(ICON_FA_CHECK " Main Camera (drives the Game view)");
        }
        else if (UI::IconButton(ICON_FA_CHECK, "Set as Main Camera", ImVec2(-1.0f, 0.0f)))
        {
            m_commands.setMainCameraRequested = true;
            m_commands.setMainCameraId = camera.id;
        }
    }
}

void EditorImGui::RenderSelectedMeshRendererInspector()
{
    if (!m_meshRendererState.selected)
        return;

    RenderInspectorTitle(ICON_FA_CUBE, m_meshRendererState.skinned ? "Animated Mesh" : "Mesh", m_meshRendererState.id);

    char nameBuffer[96]{};
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", m_meshRendererState.name.c_str());
    if (UI::Prop::InputText("Name", nameBuffer, sizeof(nameBuffer)))
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
    if (RenderTransformComponent(position, rotation, scale, /*meshScale=*/true))
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

    if (ImGui::CollapsingHeader(ICON_FA_CUBE "  Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
    {
        std::string meshLabel = m_meshRendererState.meshDisplayName.empty()
            ? (m_meshRendererState.meshAssetId.empty() ? std::string("No model assigned") : m_meshRendererState.meshAssetId)
            : m_meshRendererState.meshDisplayName;
        // Built-in primitives by name ("Cube (built-in)"), not by their internal id.
        constexpr std::string_view kPrimitivePrefix = "builtin://primitive/";
        if (meshLabel.starts_with(kPrimitivePrefix) && meshLabel.size() > kPrimitivePrefix.size())
        {
            std::string primitive = meshLabel.substr(kPrimitivePrefix.size());
            primitive[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(primitive[0])));
            meshLabel = primitive + " (built-in)";
        }
        UI::AssetField("Model", ICON_FA_CUBE, meshLabel, "The model this entity shows. Drop a model from the Asset Browser here to change it.");
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
            {
                const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                AssignAssetToSelectedMeshRenderer(assetId);
            }
            ImGui::EndDragDropTarget();
        }
        if (m_inspectorShowDebugInfo)
        {
            ImGui::TextDisabled("Asset ID: %s", m_meshRendererState.meshAssetId.empty() ? "<none>" : m_meshRendererState.meshAssetId.c_str());
            ImGui::TextDisabled("Path: %s", m_meshRendererState.meshAssetPath.empty() ? "<none>" : m_meshRendererState.meshAssetPath.c_str());
            ImGui::TextDisabled("Render path: %s", m_meshRendererState.skinned ? "SkinnedMeshRenderer" : "StaticMeshRenderer");
        }
    }

    if (!m_meshRendererState.prefabAssetId.empty() &&
        ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Prefab", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
    {
        RenderPrefabOverrideControls(m_meshRendererState.prefabAssetId,
            m_meshRendererState.prefabInstance,
            m_meshRendererState.prefabOverrides);
    }

    if (RenderSelectedMeshPhysicsComponents())
        MarkSelectedMeshRendererChanged();

    if (ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Layer generation", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
    {
        bool changed = UI::Prop::Checkbox("Include collision surfaces", &m_meshRendererState.layerAuthoring.enabled);
        ImGui::TextDisabled("Bounds and floor heights are derived from collision geometry.");
        ImGui::BeginDisabled(!m_meshRendererState.layerAuthoring.enabled);
        struct TagOption { const char* name; mx::map::VolumeTag tag; };
        constexpr TagOption tagOptions[] = {
            {"Ground", mx::map::VolumeTagGround},
            {"Building", mx::map::VolumeTagBuilding},
            {"Bridge", mx::map::VolumeTagBridge},
            {"Water", mx::map::VolumeTagWater},
            {"Underwater", mx::map::VolumeTagUnderwater},
            {"Dungeon", mx::map::VolumeTagDungeon},
            {"Interior", mx::map::VolumeTagInterior},
            {"Connector", mx::map::VolumeTagConnector},
            {"Road", mx::map::VolumeTagRoad},
            {"Stairs", mx::map::VolumeTagStairs},
            {"Lift", mx::map::VolumeTagLift},
            {"Dock", mx::map::VolumeTagDock},
        };
        for (const TagOption& option : tagOptions)
        {
            bool selected = mx::map::HasVolumeTag(m_meshRendererState.layerAuthoring.tags, option.tag);
            if (UI::Prop::Checkbox(option.name, &selected))
            {
                if (selected)
                    m_meshRendererState.layerAuthoring.tags |= option.tag;
                else
                    m_meshRendererState.layerAuthoring.tags &= ~static_cast<std::uint32_t>(option.tag);
                changed = true;
            }
        }
        ImGui::EndDisabled();
        if (!m_meshRendererState.hasCollider || !m_meshRendererState.collider.enabled)
            ImGui::TextDisabled("An enabled static collider is required for export.");
        if ((m_meshRendererState.layerAuthoring.tags & ~mx::map::kKnownVolumeTags) != 0)
            ImGui::TextUnformatted("Invalid layer tags: generation will reject this source.");
        if (changed)
            MarkSelectedMeshRendererChanged();
    }

    if (ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Material Slots", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
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
        ImGui::CollapsingHeader(ICON_FA_PALETTE " Material", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap))
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
        changed |= UI::Prop::Checkbox("Override Active", &material.enabled);
        changed |= UI::Prop::ColorEdit4("BaseColor Tint", material.baseColor, ImGuiColorEditFlags_Float);
        changed |= UI::Prop::SliderFloat("Metallic", &material.metallic, 0.0f, 1.0f, "%.2f");
        changed |= UI::Prop::SliderFloat("Roughness", &material.roughness, 0.0f, 1.0f, "%.2f");
        changed |= UI::Prop::SliderFloat("Normal Strength", &material.normalStrength, 0.0f, 4.0f, "%.2f");
        changed |= UI::Prop::SliderFloat("AO Strength", &material.aoStrength, 0.0f, 2.0f, "%.2f");
        changed |= UI::Prop::ColorEdit3("Emissive", material.emissive, ImGuiColorEditFlags_Float);
        changed |= UI::Prop::SliderFloat("Emissive Intensity", &material.emissiveIntensity, 0.0f, 20.0f, "%.2f");
        changed |= UI::Prop::DragFloat2("UV Tiling", material.uvTiling, 0.01f, 0.01f, 64.0f, "%.2f");
        changed |= UI::Prop::DragFloat2("UV Offset", material.uvOffset, 0.01f, -1000.0f, 1000.0f, "%.2f");

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
    ImGui::SeparatorText("Sun");
    UI::Prop::Checkbox("Enabled", &m_lightingState.directional.enabled);
    UI::Prop::Checkbox("Casts shadows", &m_lightingState.sunShadowsEnabled);
    UI::Prop::ColorEdit3("Color", &m_lightingState.directional.r);
    UI::Prop::SliderFloat("Intensity", &m_lightingState.directional.intensity, 0.0f, 5.0f, "%.2f");
    UI::Prop::SliderFloat("Height in sky", &m_lightingState.directional.elevationDegrees, -90.0f, 90.0f, "%.1f deg");
    UI::ItemTooltip("Elevation above the horizon: 90 is straight overhead, below 0 is night");
    UI::Prop::SliderFloat("Direction", &m_lightingState.directional.azimuthDegrees, 0.0f, 360.0f, "%.1f deg");
    UI::ItemTooltip("Compass direction the sunlight comes from");

    ImGui::SeparatorText("Ambient light");
    ImGui::BeginDisabled(m_skySettings.ambientFromSky);
    UI::Prop::ColorEdit3("Color##ambient", &m_lightingState.ambient.r);
    ImGui::EndDisabled();
    if (m_skySettings.ambientFromSky)
        UI::ItemTooltip("Taken from the sky (Sky > Ambient from sky)");
    UI::Prop::SliderFloat("Intensity##ambient", &m_lightingState.ambient.intensity, 0.0f, 3.0f, "%.2f");

    ImGui::SeparatorText("Time of day preset");
    UI::Property("Hour", [&](const char* id) {
        const float buttonWidth = ImGui::CalcTextSize("Apply").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(std::max(40.0f, ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x));
        UI::Prop::SliderFloat(id, &m_timeOfDayHours, 0.0f, 24.0f, "%.1f h");
        ImGui::SameLine();
        if (ImGui::Button("Apply"))
            ApplyTimeOfDayPreset(m_timeOfDayHours);
        return false;
    });
    UI::ItemTooltip("Sets the sun and ambient light for that hour");
}

namespace
{
// File-name words that mark each cube face, in SkySettings face order (+X, -X, +Y, -Y, +Z, -Z).
// Dropping one face fills the others from same-named siblings: sky_right.png -> sky_left.png, ...
const std::array<std::vector<std::string>, SkySettings::kCubeFaces> kSkyFaceWords = {{
    {"right", "px", "posx", "rt"},
    {"left", "nx", "negx", "lf"},
    {"up", "top", "py", "posy"},
    {"down", "bottom", "ny", "negy", "dn"},
    {"front", "pz", "posz", "ft"},
    {"back", "nz", "negz", "bk"},
}};

bool IsWordBoundary(const std::string& text, std::size_t index)
{
    return index == 0 || index >= text.size() ||
        !std::isalnum(static_cast<unsigned char>(text[index - 1])) ||
        !std::isalnum(static_cast<unsigned char>(text[index]));
}

// Finds the face word in a file stem: a whole word (between separators) or a trailing suffix.
std::optional<std::pair<std::size_t, std::size_t>> FindSkyFaceWord(const std::string& stem, std::size_t face)
{
    std::string lower = stem;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const std::string& word : kSkyFaceWords[face])
    {
        for (std::size_t at = lower.rfind(word); at != std::string::npos; at = at == 0 ? std::string::npos : lower.rfind(word, at - 1))
        {
            const std::size_t end = at + word.size();
            if (IsWordBoundary(lower, at) && (end == lower.size() || !std::isalpha(static_cast<unsigned char>(lower[end]))))
                return std::make_pair(at, word.size());
        }
    }
    return std::nullopt;
}

// The other face's word in the same letter case as the one it replaces.
std::string MatchCase(const std::string& word, const std::string& like)
{
    std::string out = word;
    const bool upper = std::all_of(like.begin(), like.end(),
        [](unsigned char c) { return !std::isalpha(c) || std::isupper(c); });
    const bool capitalized = !upper && !like.empty() && std::isupper(static_cast<unsigned char>(like[0]));
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        if (upper || (capitalized && i == 0))
            out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    }
    return out;
}
} // namespace

void EditorImGui::RenderSkyPanel()
{
    SkySettings& sky = m_skySettings;
    bool changed = false;

    const char* modes[] = {"Color", "Procedural", "Cube map (6 images)", "Panorama (360 image)"};
    int mode = static_cast<int>(sky.mode);
    if (UI::Prop::Combo("Type", &mode, modes, IM_ARRAYSIZE(modes)))
    {
        sky.mode = static_cast<SkySettings::Mode>(std::clamp(mode, 0, 3));
        changed = true;
    }
    UI::ItemTooltip("What the scene shows behind everything (the water reflects it too)");

    // An image slot: shows the file, takes a texture dropped from the Asset Browser, X clears it.
    // Returns the project-relative path of a newly dropped texture (empty when nothing was dropped).
    const auto imageSlot = [&](const char* label, std::string& path, const char* tooltip) {
        std::string dropped;
        const std::string name = path.empty() ? std::string("None (drop an image here)")
                                              : std::filesystem::path(path).filename().string();
        ImGui::PushID(label);
        UI::Property(label, [&](const char*) {
            const float clearWidth = path.empty() ? 0.0f : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            const std::string text = std::string(ICON_FA_IMAGE "  ") + name + "##image";
            ImGui::Button(text.c_str(), ImVec2(std::max(40.0f, ImGui::GetContentRegionAvail().x - clearWidth), 0.0f));
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            UI::ItemTooltip(path.empty() ? tooltip : path.c_str());
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
                {
                    const std::string assetId(static_cast<const char*>(payload->Data), payload->DataSize);
                    auto entry = m_assetLibrary ? m_assetLibrary->FindById(assetId) : std::optional<AssetLibrary::Entry>{};
                    if (entry && entry->category == AssetLibrary::Category::Texture)
                    {
                        dropped = m_assetLibrary->AssetRelativePath(*entry);
                        path = dropped;
                        changed = true;
                    }
                    else
                    {
                        m_assetStatus = "The sky takes image assets only (PNG, JPG, TGA or HDR)";
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (!path.empty())
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_XMARK "##clear", ImVec2(ImGui::GetFrameHeight(), 0.0f)))
                {
                    path.clear();
                    changed = true;
                }
                UI::ItemTooltip("Remove the image");
            }
            return false;
        });
        ImGui::PopID();
        return dropped;
    };

    switch (sky.mode)
    {
    case SkySettings::Mode::Color:
        changed |= UI::Prop::ColorEdit3("Color##sky", sky.color);
        break;
    case SkySettings::Mode::Procedural:
        changed |= UI::Prop::ColorEdit3("Zenith", sky.zenithColor);
        UI::ItemTooltip("The sky straight overhead");
        changed |= UI::Prop::ColorEdit3("Horizon", sky.horizonColor);
        UI::ItemTooltip("The sky at the horizon; turns warm on the sun's side at sunrise and sunset");
        changed |= UI::Prop::ColorEdit3("Ground", sky.groundColor);
        UI::ItemTooltip("Below the horizon, where no terrain covers it");
        changed |= UI::Prop::SliderFloat("Sun size", &sky.sunSizeDegrees, 0.0f, 10.0f, "%.1f deg");
        UI::ItemTooltip("Angular size of the sun disc (the real sun is about 0.5); 0 hides it.\n"
                        "The sun's place and colour come from the Sun above.");
        changed |= UI::Prop::SliderFloat("Sun glow", &sky.sunGlow, 0.0f, 2.0f, "%.2f");
        break;
    case SkySettings::Mode::Cubemap:
    {
        static const char* faceLabels[SkySettings::kCubeFaces] = {
            "Right (+X)", "Left (-X)", "Up (+Y)", "Down (-Y)", "Front (+Z)", "Back (-Z)"};
        for (std::size_t face = 0; face < SkySettings::kCubeFaces; ++face)
        {
            const std::string dropped = imageSlot(faceLabels[face], sky.cubeFacePaths[face],
                "Drop one face image: faces named like it (right / left / up / down / front / back,\n"
                "px / nx / py / ny / pz / nz) in the same folder fill the empty slots");
            if (dropped.empty())
                continue;
            // Fill the other empty faces from same-named siblings of the dropped image.
            const std::filesystem::path droppedPath(dropped);
            const std::string stem = droppedPath.stem().string();
            const auto found = FindSkyFaceWord(stem, face);
            if (!found)
                continue;
            const std::filesystem::path projectRoot = ProjectManager::Instance().HasProject()
                ? ProjectManager::Instance().ProjectRoot() : std::filesystem::path{};
            int filled = 0;
            for (std::size_t other = 0; other < SkySettings::kCubeFaces; ++other)
            {
                if (other == face || !sky.cubeFacePaths[other].empty())
                    continue;
                const std::string original = stem.substr(found->first, found->second);
                for (const std::string& word : kSkyFaceWords[other])
                {
                    const std::string candidateStem = stem.substr(0, found->first) + MatchCase(word, original) +
                        stem.substr(found->first + found->second);
                    const std::filesystem::path candidate =
                        droppedPath.parent_path() / (candidateStem + droppedPath.extension().string());
                    std::error_code ec;
                    if (std::filesystem::exists(projectRoot / candidate, ec))
                    {
                        sky.cubeFacePaths[other] = candidate.generic_string();
                        ++filled;
                        break;
                    }
                }
            }
            if (filled > 0)
                m_assetStatus = "Sky: filled " + std::to_string(filled) + " more face(s) from the same folder";
        }
        break;
    }
    case SkySettings::Mode::Panorama:
        imageSlot("Panorama", sky.panoramaPath,
            "An equirectangular 360 x 180 degree image (2:1), PNG, JPG or HDR.\n"
            "Drop it here from the Asset Browser.");
        break;
    }

    if (sky.mode != SkySettings::Mode::Color)
    {
        changed |= UI::Prop::SliderFloat("Exposure", &sky.exposure, 0.0f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        UI::ItemTooltip("Brightness multiplier (HDR images often need less than 1)");
        if (sky.mode != SkySettings::Mode::Procedural)
        {
            changed |= UI::Prop::SliderFloat("Rotation", &sky.rotationDegrees, 0.0f, 360.0f, "%.0f deg");
            UI::ItemTooltip("Turns the image around the vertical axis");
        }
        changed |= UI::Prop::ColorEdit3("Tint", sky.tint);
    }
    changed |= UI::Prop::Checkbox("Ambient from sky", &sky.ambientFromSky);
    UI::ItemTooltip("The ambient light takes the sky's average colour (its intensity stays above)");

    if (!m_skyStatus.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::Warning);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  %s", m_skyStatus.c_str());
        ImGui::PopStyleColor();
    }

    if (changed)
        SceneManager::Instance().MarkDirty();
}

void EditorImGui::RenderGodRaysPanel()
{
    SkySettings& sky = m_skySettings;
    bool changed = UI::Prop::Checkbox("Enabled##god_rays", &sky.godRays);
    UI::ItemTooltip("Light shafts from the Sun (its colour and direction: Environment > Sun)");
    ImGui::BeginDisabled(!sky.godRays);
    const char* techniques[] = {"Screen space", "Volumetric", "Both"};
    int technique = std::clamp(static_cast<int>(sky.godRayTechnique), 0, 2);
    if (UI::Prop::Combo("Technique##god_rays", &technique, techniques, IM_ARRAYSIZE(techniques)))
    {
        sky.godRayTechnique = technique;
        changed = true;
    }
    UI::ItemTooltip("Screen space: shafts around the sun while it is on the screen, from everything in\n"
                    "front of it (terrain, trees, characters).\n"
                    "Volumetric: light scattered in the air, with the terrain's sun shadow cutting it\n"
                    "into shafts - also seen from the side and with the sun off the screen.\n"
                    "Both: the two added together.");

    if (sky.godRayTechnique != 1)
    {
        ImGui::SeparatorText("Screen space");
        changed |= UI::Prop::SliderFloat("Intensity##god_rays", &sky.godRayIntensity, 0.0f, 2.0f, "%.2f");
        changed |= UI::Prop::SliderFloat("Length##god_rays", &sky.godRayLength, 0.05f, 1.0f, "%.2f");
        UI::ItemTooltip("How far across the screen the shafts reach");
        changed |= UI::Prop::SliderFloat("Falloff##god_rays", &sky.godRayFalloff, 0.85f, 1.0f, "%.3f");
        UI::ItemTooltip("How quickly the shafts fade along their length (closer to 1: they fade later)");
        const char* qualities[] = {"Low (32 samples)", "Medium (64 samples)", "High (96 samples)"};
        int quality = std::clamp(static_cast<int>(sky.godRayQuality), 0, 2);
        if (UI::Prop::Combo("Quality##god_rays", &quality, qualities, IM_ARRAYSIZE(qualities)))
        {
            sky.godRayQuality = quality;
            changed = true;
        }
    }
    if (sky.godRayTechnique != 0)
    {
        ImGui::SeparatorText("Volumetric");
        changed |= UI::Prop::SliderFloat("Intensity##volumetric", &sky.volumetricIntensity, 0.0f, 4.0f, "%.2f");
        changed |= UI::Prop::SliderFloat("Haze##volumetric", &sky.volumetricDensity, 0.0f, 0.05f, "%.4f",
            ImGuiSliderFlags_Logarithmic);
        UI::ItemTooltip("How much the air scatters the sunlight: more haze, brighter and denser shafts");
        changed |= UI::Prop::SliderFloat("Forward scattering##volumetric", &sky.volumetricAnisotropy, 0.0f, 0.95f, "%.2f");
        UI::ItemTooltip("0: the haze glows the same in every direction; near 1: mostly when looking towards the sun");
        changed |= UI::Prop::SliderFloat("Distance##volumetric", &sky.volumetricDistance, 5.0f, 200.0f, "%.0f m");
        UI::ItemTooltip("How far from the camera the light in the air is gathered (the sun shadow reaches 200 m)");
        const char* steps[] = {"Low (16 steps)", "Medium (32 steps)", "High (64 steps)"};
        int quality = std::clamp(static_cast<int>(sky.volumetricQuality), 0, 2);
        if (UI::Prop::Combo("Quality##volumetric", &quality, steps, IM_ARRAYSIZE(steps)))
        {
            sky.volumetricQuality = quality;
            changed = true;
        }
        ImGui::TextDisabled("Shafts come from the sun shadow (Environment > Sun > Casts shadows).");
    }
    ImGui::EndDisabled();
    if (changed)
        SceneManager::Instance().MarkDirty();
}

void EditorImGui::RenderToneMappingPanel()
{
    SkySettings& sky = m_skySettings;
    bool changed = false;
    const char* modes[] = {"None", "Neutral", "Filmic"};
    int mode = std::clamp(static_cast<int>(sky.toneMapping), 0, 2);
    if (UI::Prop::Combo("Curve##tone_mapping", &mode, modes, IM_ARRAYSIZE(modes)))
    {
        sky.toneMapping = mode;
        changed = true;
    }
    UI::ItemTooltip("How light brighter than white reaches the screen.\n"
                    "None: clipped at white.\n"
                    "Neutral: colours up to 0.8 exactly as set, brighter ones roll off smoothly to white.\n"
                    "Filmic: an ACES-like film curve - more contrast, brighter midtones.");
    changed |= UI::Prop::SliderFloat("Exposure##tone_mapping", &sky.exposureEv, -4.0f, 4.0f, "%+.1f EV");
    UI::ItemTooltip("Brightness of the whole image in stops: +1 doubles the light, -1 halves it");
    if (changed)
        SceneManager::Instance().MarkDirty();
}

void EditorImGui::OpenCreateTerrainDialog()
{
    if (m_terrainState.exists)
        m_replaceTerrainConfirmOpen = true;
    else
        m_createTerrainModalOpen = true;
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
        UI::Prop::InputFloat("Width (m)", &m_createTerrainWidthMeters, 10.0f, 100.0f, "%.1f");
        ImGui::SetNextItemWidth(180.0f);
        UI::Prop::InputFloat("Depth (m)", &m_createTerrainDepthMeters, 10.0f, 100.0f, "%.1f");
        ImGui::SetNextItemWidth(180.0f);
        UI::Prop::InputFloat("Cell size (m/cell)", &m_createTerrainCellSizeMeters, 0.25f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(180.0f);
        UI::Prop::InputInt("Chunk size (cells/chunk)", &m_createTerrainChunkSizeCells, 16, 32);

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

namespace
{
// A row of mutually exclusive choices drawn as joined buttons; returns the clicked index or -1.
int SegmentedChoice(const char* id, const char* const* labels, int count, int selected, float width = -1.0f)
{
    ImGui::PushID(id);
    const float available = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float spacing = 2.0f;
    const float buttonWidth = std::max(40.0f, (available - spacing * static_cast<float>(count - 1)) / static_cast<float>(count));
    int clicked = -1;
    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
            ImGui::SameLine(0.0f, spacing);
        const bool active = i == selected;
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, UI::Theme::Accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI::Theme::AccentHovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI::Theme::AccentActive);
        }
        if (ImGui::Button(labels[i], ImVec2(buttonWidth, 0.0f)))
            clicked = i;
        if (active)
            ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
    return clicked;
}

// The one-line hint under a tool: what to do in the Scene View now.
void ToolHint(bool active, const char* activeText, const char* idleText)
{
    if (active)
        ImGui::TextColored(UI::Theme::AccentHovered, ICON_FA_CIRCLE_INFO "  %s", activeText);
    else
        ImGui::TextDisabled(ICON_FA_CIRCLE_INFO "  %s", idleText);
}
}

void EditorImGui::RenderWaterSculptTool()
{
    const bool active = m_editorSettings.toolMode == MapEditorToolMode::WaterSculpt;
    const char* modes[] = {ICON_FA_PLUS "  Add water", ICON_FA_ERASER "  Remove water"};
    const int current = active ? (m_editorSettings.waterSculptAdd ? 0 : 1) : -1;
    const int clicked = SegmentedChoice("water_sculpt_mode", modes, 2, current);
    if (clicked >= 0)
    {
        if (clicked == current)
        {
            SetToolMode(MapEditorToolMode::None);
        }
        else
        {
            m_editorSettings.waterSculptAdd = clicked == 0;
            SetToolMode(MapEditorToolMode::WaterSculpt);
            Tracenf("[EDITOR-IMGUI-5] Water sculpt brush mode: %s", m_editorSettings.waterSculptAdd ? "add" : "remove");
        }
    }
    UI::Prop::SliderFloat("Brush radius", &m_editorSettings.waterSculptRadiusMeters, 0.5f, 20.0f, "%.1f m");
    ToolHint(active,
        "Paint in the Scene View to change the water's shape. Click the tool again to stop.",
        "Pick Add or Remove, then paint in the Scene View.");
}

void EditorImGui::RenderTerrainSculptTool()
{
    const bool active = m_editorSettings.toolMode == MapEditorToolMode::Heightmap;
    const char* tools[] = {"Raise", "Lower", "Smooth", "Flatten"};
    const MapEditorTool toolValues[] = {MapEditorTool::Raise, MapEditorTool::Lower, MapEditorTool::Smooth, MapEditorTool::Flatten};
    int current = -1;
    for (int i = 0; i < 4; ++i)
    {
        if (active && m_editorSettings.tool == toolValues[i])
            current = i;
    }
    const int clicked = SegmentedChoice("terrain_sculpt_tool", tools, 4, current);
    if (clicked >= 0)
    {
        if (clicked == current)
        {
            SetToolMode(MapEditorToolMode::None);
        }
        else
        {
            m_editorSettings.tool = toolValues[clicked];
            SetToolMode(MapEditorToolMode::Heightmap);
        }
    }
    ImGui::Spacing();
    UI::Prop::SliderFloat("Brush radius", &m_editorSettings.brushRadiusMeters, 1.0f, 100.0f, "%.1f m");
    UI::Prop::SliderFloat("Strength", &m_editorSettings.brushStrength, 0.1f, 5.0f, "%.2f");
    UI::Prop::SliderFloat("Falloff", &m_editorSettings.brushFalloff, 0.0f, 1.0f, "%.2f");
    UI::ItemTooltip("0: hard edge, 1: soft edge");
    if (m_editorSettings.tool == MapEditorTool::Flatten)
        UI::Prop::SliderFloat("Target height", &m_editorSettings.flattenTargetY, -50.0f, 100.0f, "%.2f m");
    ToolHint(active,
        "Hold the left mouse button in the Scene View to sculpt. Click the tool again to stop.",
        "Pick a tool, then sculpt in the Scene View.");
}

void EditorImGui::RenderSplatLayerSlot(std::uint32_t slotIndex)
{
    if (slotIndex >= m_paletteSlots.size())
        return;

    MapEditorPaletteSlot& slot = m_paletteSlots[slotIndex];
    ImGui::PushID(static_cast<int>(slotIndex));
    const bool selected = m_editorSettings.textureSlot == slotIndex;
    const float slotWidth = std::max(52.0f, (ImGui::GetContentRegionAvail().x - 3.0f * ImGui::GetStyle().ItemSpacing.x) / 4.0f);
    ImGui::BeginGroup();
    if (selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, UI::Theme::Accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI::Theme::AccentHovered);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI::Theme::AccentActive);
    }
    const std::string label = std::to_string(slotIndex + 1u) + "##splat_slot";
    if (ImGui::Button(label.c_str(), ImVec2(slotWidth, 36.0f)))
    {
        m_editorSettings.textureSlot = slotIndex;
        m_editorSettings.tool = MapEditorTool::Paint;
        SetToolMode(MapEditorToolMode::SplatPaint);
    }
    if (selected)
        ImGui::PopStyleColor(3);

    if (ImGui::BeginDragDropTarget())
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
                    m_assetStatus = "Terrain layer " + std::to_string(slotIndex + 1u) + " texture: " + entry->displayName;
                    Tracenf("[EDITOR-IMGUI-5] Splat layer texture changed: layer=%u asset_id=%s",
                        slotIndex,
                        entry->id.c_str());
                }
            }
            else
            {
                m_assetStatus = "Terrain layers accept texture or material assets";
            }
        }
        ImGui::EndDragDropTarget();
    }

    const std::string name = slot.displayName.empty()
        ? (slot.texturePath.empty() ? std::string("(empty)") : std::filesystem::path(slot.texturePath).stem().string())
        : slot.displayName;
    UI::ItemTooltip(("Layer " + std::to_string(slotIndex + 1u) + ": " + name +
        "\nClick to paint with it; drop a texture or material here to change it").c_str());
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + slotWidth);
    ImGui::TextDisabled("%s", name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::PopID();
}

void EditorImGui::RenderTerrainPaintTool()
{
    const bool active = m_editorSettings.toolMode == MapEditorToolMode::SplatPaint;
    for (std::uint32_t i = 0; i < m_paletteSlots.size(); ++i)
    {
        RenderSplatLayerSlot(i);
        if (i % 4 != 3)
            ImGui::SameLine();
    }
    ToolHint(active,
        "Hold the left mouse button in the Scene View to paint the highlighted layer.",
        "Click a layer to paint with it. Drop textures from the Asset Browser onto the layers.");
    if (active && ImGui::Button("Stop painting"))
        SetToolMode(MapEditorToolMode::None);

    ImGui::SeparatorText("Brush");
    int paintMode = m_editorSettings.paintMode == MapEditorPaintMode::Mix ? 1 : 0;
    const char* modes[] = {"Replace", "Mix"};
    if (UI::Prop::Combo("Blend", &paintMode, modes, IM_ARRAYSIZE(modes)))
        m_editorSettings.paintMode = paintMode == 1 ? MapEditorPaintMode::Mix : MapEditorPaintMode::Replace;
    UI::Prop::SliderFloat("Brush radius", &m_editorSettings.brushRadiusMeters, 1.0f, 100.0f, "%.1f m");
    UI::Prop::SliderFloat("Strength", &m_editorSettings.brushStrength, 0.1f, 1.0f, "%.2f");
    UI::Prop::SliderFloat("Falloff", &m_editorSettings.brushFalloff, 0.0f, 1.0f, "%.2f");

    const std::uint32_t selectedSlot = std::min<std::uint32_t>(m_editorSettings.textureSlot, 7u);
    MapEditorPaletteSlot& materialSlot = m_paletteSlots[selectedSlot];
    const std::string layerTitle = "Layer " + std::to_string(selectedSlot + 1u) + ": " +
        (materialSlot.displayName.empty() ? std::string("(empty)") : materialSlot.displayName);
    ImGui::SeparatorText(layerTitle.c_str());
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
    if (UI::Prop::SliderFloat("Tiling", &tiling, 0.05f, 32.0f, "%.2f"))
    {
        materialSlot.tilingScaleX = tiling;
        materialSlot.tilingScaleY = tiling;
        commitMaterialParams();
    }
    if (UI::Prop::ColorEdit3("Tint", materialSlot.colorTint))
        commitMaterialParams();
    if (UI::Prop::SliderFloat("Normal strength", &materialSlot.normalStrength, 0.0f, 3.0f, "%.2f"))
        commitMaterialParams();
    if (UI::Prop::SliderFloat("Roughness", &materialSlot.roughnessStrength, 0.0f, 2.0f, "%.2f"))
        commitMaterialParams();
    if (UI::Prop::SliderFloat("Metallic", &materialSlot.metallicStrength, 0.0f, 1.0f, "%.2f"))
        commitMaterialParams();
    if (UI::Prop::SliderFloat("AO strength", &materialSlot.aoStrength, 0.0f, 1.0f, "%.2f"))
        commitMaterialParams();
    if (UI::Prop::SliderFloat2("UV offset", materialSlot.uvOffset, -10.0f, 10.0f, "%.3f"))
        commitMaterialParams();
    if (UI::Prop::SliderFloat("UV rotation", &materialSlot.uvRotationDegrees, -180.0f, 180.0f, "%.1f deg"))
        commitMaterialParams();
}
