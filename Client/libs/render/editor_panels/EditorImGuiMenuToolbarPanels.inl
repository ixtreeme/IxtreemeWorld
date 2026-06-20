// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

void EditorImGui::RenderMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;

    SceneManager& scenes = SceneManager::Instance();
    ProjectManager& projects = ProjectManager::Instance();
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Project..."))
            OpenProjectDialog(ProjectDialogMode::Create);
        if (ImGui::MenuItem("Open Project..."))
            OpenProjectDialog(ProjectDialogMode::Open);
        if (ImGui::MenuItem("Save Project", "Ctrl+S", false, projects.HasProject()))
            SaveProjectAndCurrentScene();
        if (ImGui::BeginMenu("Recent Projects", !projects.RecentProjects().empty()))
        {
            for (const auto& path : projects.RecentProjects())
            {
                const std::string label = path.parent_path().filename().string();
                if (ImGui::MenuItem(label.c_str()))
                    OpenProjectFromDialog(path);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FILE "  New Scene", "Ctrl+N"))
            scenes.NewScene();
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Scene...", "Ctrl+O"))
        {
#if defined(_WIN32)
            char file[MAX_PATH]{};
            OPENFILENAMEA ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrTitle = "Open Scene";
            ofn.lpstrFilter = "Scene Files (*.scene)\0*.scene\0All files (*.*)\0*.*\0\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn))
                scenes.LoadScene(file);
#endif
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save Scene As...", "Ctrl+Shift+S"))
            scenes.SaveSceneAs({});
        ImGui::Separator();
        if (ImGui::BeginMenu("Recent Scenes", !scenes.GetRecentScenes().empty()))
        {
            for (const std::string& path : scenes.GetRecentScenes())
            {
                const std::string label = std::filesystem::path(path).filename().string();
                if (ImGui::MenuItem(label.c_str()))
                    scenes.LoadScene(path);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
        ImGui::MenuItem("Undo", "Ctrl+Z", false, false);
        ImGui::MenuItem("Redo", "Ctrl+Y", false, false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        ImGui::MenuItem("Demo Window", nullptr, &m_showDemoWindow);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools"))
    {
        if (ImGui::MenuItem("Tree Generator..."))
        {
            if (!m_treeGeneratorPanel)
            {
                const std::filesystem::path presetDir = m_engineRoot.empty()
                    ? std::filesystem::path{}
                    : (InternalAssetRootFor(m_engineRoot) / "tree_presets");
                m_treeGeneratorPanel = std::make_unique<tree_tool::TreeGeneratorPanel>(presetDir);
            }
            m_treeGeneratorPanel->SetAssetLibrary(m_assetLibrary.get());
            m_treeGeneratorPanel->Show();
        }
        if (ImGui::BeginMenu("Debug"))
        {
            if (ImGui::MenuItem("Capture GPU Frame", "F11"))
                m_commands.captureGpuFrame = true;
            if (ImGui::MenuItem("Dump Frame Profile"))
                m_commands.dumpFrameProfile = true;
            if (ImGui::MenuItem("Dump Material Bindings", "F12"))
                m_commands.dumpMaterialState = true;
            ImGui::Separator();
            bool debugTogglesChanged = false;
            debugTogglesChanged = ImGui::MenuItem("Disable Shadow Pass", nullptr, &m_debugDisableShadowPass) || debugTogglesChanged;
            debugTogglesChanged = ImGui::MenuItem("Disable Water Reflection Pass", nullptr, &m_debugDisableWaterReflectionPass) || debugTogglesChanged;
            debugTogglesChanged = ImGui::MenuItem("Disable Asset Library Discovery", nullptr, &m_debugDisableAssetLibraryDiscovery) || debugTogglesChanged;
            debugTogglesChanged = ImGui::MenuItem("Disable Asset Watcher Poll", nullptr, &m_debugDisableAssetWatcherPoll) || debugTogglesChanged;
            debugTogglesChanged = ImGui::MenuItem("Disable Hierarchy Iteration", nullptr, &m_debugDisableHierarchyIteration) || debugTogglesChanged;
            if (debugTogglesChanged)
            {
                m_commands.debugPerfTogglesChanged = true;
                m_commands.disableShadowPass = m_debugDisableShadowPass;
                m_commands.disableWaterReflectionPass = m_debugDisableWaterReflectionPass;
                m_commands.disableAssetLibraryDiscovery = m_debugDisableAssetLibraryDiscovery;
                m_commands.disableAssetWatcherPoll = m_debugDisableAssetWatcherPoll;
                m_commands.disableHierarchyIteration = m_debugDisableHierarchyIteration;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        ImGui::MenuItem("About IxtreemeEngine", nullptr, false, false);
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

bool EditorImGui::SaveProjectAndCurrentScene(bool automatic)
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
    {
        m_projectStatus = automatic
            ? "Autosave failed: no project is active"
            : "Save project failed: no project is active";
        TraceError("[PROJECT] %s failed: no project is active", automatic ? "autosave" : "save");
        return false;
    }

    SceneManager& scenes = SceneManager::Instance();
    if (scenes.HasOpenScene())
    {
        if (!scenes.SaveScene())
        {
            m_projectStatus = automatic
                ? "Autosave failed: scene save failed"
                : "Save project failed: scene save failed";
            TraceError("[PROJECT] %s failed: current scene save failed", automatic ? "autosave" : "save");
            return false;
        }
    }

    std::string error;
    if (!projects.SaveProject(error))
    {
        m_projectStatus = automatic ? ("Autosave failed: " + error) : ("Save project failed: " + error);
        return false;
    }

    m_lastAutoSaveSeconds = ImGui::GetTime();
    UpdateAutoSaveWindowTitle(m_lastAutoSaveSeconds);
    if (automatic)
    {
        m_projectStatus = scenes.HasOpenScene() ? "Autosaved project and scene" : "Autosaved project";
        Tracen("[PROJECT] autosave with current scene OK");
    }
    else
    {
        m_projectStatus = scenes.HasOpenScene() ? "Project and scene saved" : "Project saved";
        Tracen("[PROJECT] save with current scene OK");
    }
    return true;
}

void EditorImGui::RunProjectAutoSave()
{
    const double now = ImGui::GetTime();
    if (!ProjectManager::Instance().HasProject())
    {
        m_lastAutoSaveSeconds = now;
        UpdateAutoSaveWindowTitle(now);
        return;
    }

    if (m_lastAutoSaveSeconds <= 0.0)
    {
        m_lastAutoSaveSeconds = now;
        UpdateAutoSaveWindowTitle(now);
        return;
    }

    if (now - m_lastAutoSaveSeconds >= kProjectAutoSaveIntervalSeconds)
    {
        SaveProjectAndCurrentScene(true);
        UpdateAutoSaveWindowTitle(ImGui::GetTime());
        return;
    }

    UpdateAutoSaveWindowTitle(now);
}

void EditorImGui::UpdateAutoSaveWindowTitle(double now)
{
    if (!ProjectManager::Instance().HasProject())
    {
        if (m_lastAutoSaveTitleRemainingSeconds != -1)
        {
            m_lastAutoSaveTitleRemainingSeconds = -1;
            SceneManager::Instance().SetWindowTitleSuffix({});
        }
        return;
    }

    if (m_lastAutoSaveSeconds <= 0.0)
        m_lastAutoSaveSeconds = now;

    const double elapsed = std::max(0.0, now - m_lastAutoSaveSeconds);
    const double remainingSecondsDouble = std::max(0.0, kProjectAutoSaveIntervalSeconds - elapsed);
    const int remainingSeconds = static_cast<int>(ixtreeme::math::Ceil(static_cast<float>(remainingSecondsDouble)));
    if (remainingSeconds == m_lastAutoSaveTitleRemainingSeconds)
        return;

    m_lastAutoSaveTitleRemainingSeconds = remainingSeconds;
    const int minutes = remainingSeconds / 60;
    const int seconds = remainingSeconds % 60;
    char suffix[64]{};
    std::snprintf(suffix, sizeof(suffix), "Autosave %02d:%02d", minutes, seconds);
    SceneManager::Instance().SetWindowTitleSuffix(suffix);
}

void EditorImGui::HandleEditorHotkeys()
{
    if (!m_editorModeActive)
        return;

    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        if (io.KeyShift)
            SceneManager::Instance().SaveSceneAs({});
        else
            SaveProjectAndCurrentScene();
        return;
    }

    if (io.WantCaptureKeyboard)
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false))
    {
        if (io.KeyShift)
        {
            if (m_playModeState.mode != EditorPlayMode::Edit)
                m_commands.exitPlayMode = true;
        }
        else if (m_playModeState.mode == EditorPlayMode::Edit)
        {
            if (SceneManager::Instance().HasOpenScene())
                m_commands.enterPlayMode = true;
            else
                Tracen("[EDIT-PLAY] F5 ignored: no open scene");
        }
        else
        {
            m_commands.exitPlayMode = true;
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F6, false))
    {
        if (m_playModeState.mode == EditorPlayMode::Play)
            m_commands.pausePlayMode = true;
        else if (m_playModeState.mode == EditorPlayMode::PlayPaused)
            m_commands.resumePlayMode = true;
    }

    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F12, false))
        m_commands.dumpMaterialState = true;
    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F11, false))
        m_commands.captureGpuFrame = true;

    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        if (m_waterBodyState.selected)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::WaterBody, m_waterBodyState.id))
                QueueHierarchyFocus(*entity);
        }
        else if (m_dynamicLightState.type == DynamicLightType::Point)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::PointLight, m_dynamicLightState.id))
                QueueHierarchyFocus(*entity);
        }
        else if (m_dynamicLightState.type == DynamicLightType::Spot)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::SpotLight, m_dynamicLightState.id))
                QueueHierarchyFocus(*entity);
        }
    }

    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F2, false))
    {
        if (m_waterBodyState.selected)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::WaterBody, m_waterBodyState.id))
                StartHierarchyRename(*entity);
        }
        else if (m_dynamicLightState.type == DynamicLightType::Point)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::PointLight, m_dynamicLightState.id))
                StartHierarchyRename(*entity);
        }
        else if (m_dynamicLightState.type == DynamicLightType::Spot)
        {
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::SpotLight, m_dynamicLightState.id))
                StartHierarchyRename(*entity);
        }
    }

    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        if (m_waterBodyState.selected)
        {
            m_commands.hierarchyDeleteEntity = true;
            m_commands.hierarchyEntityType = HierarchyEntityType::WaterBody;
            m_commands.hierarchyEntityId = m_waterBodyState.id;
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::WaterBody, m_waterBodyState.id))
                m_commands.hierarchyEntityHandle = entity->entity;
        }
        else if (m_dynamicLightState.type == DynamicLightType::Point)
        {
            m_commands.hierarchyDeleteEntity = true;
            m_commands.hierarchyEntityType = HierarchyEntityType::PointLight;
            m_commands.hierarchyEntityId = m_dynamicLightState.id;
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::PointLight, m_dynamicLightState.id))
                m_commands.hierarchyEntityHandle = entity->entity;
        }
        else if (m_dynamicLightState.type == DynamicLightType::Spot)
        {
            m_commands.hierarchyDeleteEntity = true;
            m_commands.hierarchyEntityType = HierarchyEntityType::SpotLight;
            m_commands.hierarchyEntityId = m_dynamicLightState.id;
            if (const HierarchySceneEntity* entity = FindHierarchyEntity(HierarchyEntityType::SpotLight, m_dynamicLightState.id))
                m_commands.hierarchyEntityHandle = entity->entity;
        }
    }

    if (io.KeyCtrl)
    {
        SceneManager& scenes = SceneManager::Instance();
        if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false))
            scenes.NewScene();
        if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O, false))
        {
#if defined(_WIN32)
            char file[MAX_PATH]{};
            OPENFILENAMEA ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrTitle = "Open Scene";
            ofn.lpstrFilter = "Scene Files (*.scene)\0*.scene\0All files (*.*)\0*.*\0\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn))
                scenes.LoadScene(file);
#endif
        }
    }
}

void EditorImGui::RenderEditorToolbar()
{
    if (!m_editorModeActive)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(760.0f, 58.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Editor Toolbar", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar))
    {
        const bool isEdit = m_playModeState.mode == EditorPlayMode::Edit;
        const bool isPlay = m_playModeState.mode == EditorPlayMode::Play;
        const bool isPaused = m_playModeState.mode == EditorPlayMode::PlayPaused;

        if (isEdit)
        {
            const bool canPlay = SceneManager::Instance().HasOpenScene();
            if (!canPlay)
                ImGui::BeginDisabled();
            if (UI::IconButton(ICON_FA_PLAY, "Play", ImVec2(96.0f, 32.0f)))
                m_commands.enterPlayMode = true;
            if (!canPlay)
            {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Open a scene to Play");
            }
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
            if (UI::IconButton(ICON_FA_STOP, "Stop", ImVec2(96.0f, 32.0f)))
                m_commands.exitPlayMode = true;
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (isPlay)
            {
                if (UI::IconButton(ICON_FA_PAUSE, "Pause", ImVec2(104.0f, 32.0f)))
                    m_commands.pausePlayMode = true;
            }
            else if (isPaused)
            {
                if (UI::IconButton(ICON_FA_PLAY, "Resume", ImVec2(112.0f, 32.0f)))
                    m_commands.resumePlayMode = true;
            }
        }

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(16.0f, 0.0f));
        ImGui::SameLine();

        const char* modeText = isEdit ? "EDIT MODE" : isPlay ? "PLAY MODE" : "PAUSED";
        const ImVec4 modeColor = isEdit
            ? ImVec4(0.72f, 0.72f, 0.72f, 1.0f)
            : isPlay ? ImVec4(0.35f, 0.90f, 0.35f, 1.0f) : ImVec4(0.95f, 0.74f, 0.30f, 1.0f);
        ImGui::TextColored(modeColor, "%s", modeText);
        if (!isEdit)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%.1fs, frame %d)", m_playModeState.elapsedSeconds, m_playModeState.frameCount);
        }

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(24.0f, 0.0f));
        ImGui::SameLine();
        RenderGizmoControls();

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(18.0f, 0.0f));
        ImGui::SameLine();
        ImGui::TextDisabled("FPS %.0f | %.2f ms | CPU %.0f%%",
            m_engineStats.fps,
            m_engineStats.averageFrameMs > 0.0 ? m_engineStats.averageFrameMs : m_engineStats.frameMs,
            m_engineStats.processCpuPercent);
    }
    ImGui::End();
}

void EditorImGui::RenderSceneSettingsPanel()
{
    if (ImGui::Begin("Scene Settings"))
    {
        SceneManager& scenes = SceneManager::Instance();
        if (!scenes.HasOpenScene())
        {
            ImGui::TextUnformatted("No scene open");
            ImGui::TextWrapped("Create or open a scene before editing scene metadata.");
        }
        else
        {
            const SceneData& scene = scenes.GetCurrentScene();

            char nameBuffer[128]{};
            std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", scene.name.c_str());
            if (ImGui::InputText("Scene Name", nameBuffer, sizeof(nameBuffer)))
                scenes.SetSceneName(nameBuffer);

            ImGui::Separator();
            ImGui::TextUnformatted("Camera");
            ImGui::Text("Position: %.1f, %.1f, %.1f",
                scene.cameraPosition[0],
                scene.cameraPosition[1],
                scene.cameraPosition[2]);
            ImGui::Text("FOV: %.1f  Near/Far: %.2f / %.1f",
                scene.cameraFov,
                scene.cameraNear,
                scene.cameraFar);

            ImGui::Separator();
            ImGui::TextUnformatted("Environment");
            ImGui::Text("Sun: %.1f / %.1f  Intensity: %.2f",
                scene.lighting.directional.elevationDegrees,
                scene.lighting.directional.azimuthDegrees,
                scene.lighting.directional.intensity);
            ImGui::Text("Ambient: %.2f", scene.lighting.ambient.intensity);
        }
    }
    ImGui::End();
}

bool EditorImGui::HierarchyPassesSearch(const std::string& name) const
{
    return ContainsCaseInsensitive(name, m_hierarchySearchBuffer);
}

const HierarchySceneEntity* EditorImGui::FindHierarchyEntity(std::uint64_t entity) const
{
    auto it = std::find_if(m_hierarchyEntities.begin(), m_hierarchyEntities.end(),
        [entity](const HierarchySceneEntity& candidate) {
            return candidate.entity == entity;
        });
    return it == m_hierarchyEntities.end() ? nullptr : &*it;
}

const HierarchySceneEntity* EditorImGui::FindHierarchyEntity(HierarchyEntityType type, std::uint32_t objectId) const
{
    auto it = std::find_if(m_hierarchyEntities.begin(), m_hierarchyEntities.end(),
        [type, objectId](const HierarchySceneEntity& candidate) {
            return candidate.type == type && candidate.objectId == objectId;
        });
    return it == m_hierarchyEntities.end() ? nullptr : &*it;
}

bool EditorImGui::HierarchySubtreePassesSearch(std::uint64_t entity) const
{
    const HierarchySceneEntity* node = FindHierarchyEntity(entity);
    if (!node)
        return false;
    if (HierarchyPassesSearch(node->name))
        return true;
    for (const HierarchySceneEntity& child : m_hierarchyEntities)
    {
        if (child.parent == entity && HierarchySubtreePassesSearch(child.entity))
            return true;
    }
    return false;
}

void EditorImGui::QueueHierarchySelection(const HierarchySceneEntity& entity)
{
    m_commands.hierarchySelectEntity = true;
    m_commands.hierarchyEntityType = entity.type;
    m_commands.hierarchyEntityId = entity.objectId;
    m_commands.hierarchyEntityHandle = entity.entity;
    m_selectedHierarchyEntity = entity.entity;
    Tracenf("[HIERARCHY] Selected entity: flecs=%llu object=%u type=%d",
        static_cast<unsigned long long>(entity.entity),
        entity.objectId,
        static_cast<int>(entity.type));
}

void EditorImGui::QueueHierarchyFocus(const HierarchySceneEntity& entity)
{
    m_commands.hierarchyFocusEntity = true;
    m_commands.hierarchyEntityType = entity.type;
    m_commands.hierarchyEntityId = entity.objectId;
    m_commands.hierarchyEntityHandle = entity.entity;
    Tracenf("[HIERARCHY] Focus requested: flecs=%llu object=%u type=%d",
        static_cast<unsigned long long>(entity.entity),
        entity.objectId,
        static_cast<int>(entity.type));
}

void EditorImGui::StartHierarchyRename(const HierarchySceneEntity& entity)
{
    m_hierarchyRenamingEntity = entity.entity;
    std::snprintf(m_hierarchyRenameBuffer, sizeof(m_hierarchyRenameBuffer), "%s", entity.name.c_str());
}

