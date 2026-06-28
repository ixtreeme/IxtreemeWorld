// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

static std::vector<std::string> PhysicsMatrixToProjectRows(const PhysicsLayerMatrix& matrix)
{
    constexpr std::size_t kLayerCount = static_cast<std::size_t>(ixtreeme::physics::PhysicsLayer::Count);
    std::vector<std::string> rows;
    rows.reserve(kLayerCount);
    for (std::size_t row = 0; row < kLayerCount; ++row)
    {
        std::string encoded;
        encoded.reserve(kLayerCount);
        for (std::size_t col = 0; col < kLayerCount; ++col)
            encoded.push_back(matrix[row][col] ? '1' : '0');
        rows.push_back(std::move(encoded));
    }
    return rows;
}

static bool ProjectRowsToPhysicsMatrix(const std::vector<std::string>& rows, PhysicsLayerMatrix& matrix)
{
    constexpr std::size_t kLayerCount = static_cast<std::size_t>(ixtreeme::physics::PhysicsLayer::Count);
    if (rows.size() != kLayerCount)
        return false;

    PhysicsLayerMatrix parsed{};
    for (std::size_t row = 0; row < kLayerCount; ++row)
    {
        if (rows[row].size() != kLayerCount)
            return false;
        for (std::size_t col = 0; col < kLayerCount; ++col)
        {
            const char value = rows[row][col];
            if (value != '0' && value != '1')
                return false;
            parsed[row][col] = value == '1';
        }
    }

    matrix = parsed;
    return true;
}

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
        ImGui::MenuItem(ICON_FA_PERSON_RUNNING " Animator", nullptr, &m_animatorPanelOpen);
        ImGui::MenuItem("Demo Window", nullptr, &m_showDemoWindow);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Audio"))
    {
        ImGui::TextDisabled("Bus Volumes");
        bool volChanged = false;
        ImGui::SetNextItemWidth(160.0f);
        volChanged |= ImGui::SliderFloat("Master", &m_audioVolume[0], 0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        volChanged |= ImGui::SliderFloat("Music", &m_audioVolume[1], 0.0f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(160.0f);
        volChanged |= ImGui::SliderFloat("SFX", &m_audioVolume[2], 0.0f, 1.0f, "%.2f");
        if (volChanged)
        {
            m_commands.audioVolumesChanged = true;
            m_commands.audioVolume[0] = m_audioVolume[0];
            m_commands.audioVolume[1] = m_audioVolume[1];
            m_commands.audioVolume[2] = m_audioVolume[2];
        }
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
            ImGui::Separator();
            debugTogglesChanged = ImGui::MenuItem("Show Physics Colliders", nullptr, &m_debugShowPhysicsColliders) || debugTogglesChanged;
            debugTogglesChanged = ImGui::MenuItem("Show Physics Contacts", nullptr, &m_debugShowPhysicsContacts) || debugTogglesChanged;
            debugTogglesChanged = ImGui::MenuItem("Show Physics Body Centers", nullptr, &m_debugShowPhysicsBodyCenters) || debugTogglesChanged;
            if (debugTogglesChanged)
            {
                m_commands.debugPerfTogglesChanged = true;
                m_commands.disableShadowPass = m_debugDisableShadowPass;
                m_commands.disableWaterReflectionPass = m_debugDisableWaterReflectionPass;
                m_commands.disableAssetLibraryDiscovery = m_debugDisableAssetLibraryDiscovery;
                m_commands.disableAssetWatcherPoll = m_debugDisableAssetWatcherPoll;
                m_commands.disableHierarchyIteration = m_debugDisableHierarchyIteration;
                m_commands.showPhysicsColliders = m_debugShowPhysicsColliders;
                m_commands.showPhysicsContacts = m_debugShowPhysicsContacts;
                m_commands.showPhysicsBodyCenters = m_debugShowPhysicsBodyCenters;
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

void EditorImGui::LoadProjectPhysicsSettings()
{
    ProjectManager& projects = ProjectManager::Instance();
    constexpr std::size_t kLayerCount = static_cast<std::size_t>(ixtreeme::physics::PhysicsLayer::Count);
    PhysicsLayerMatrix matrix{};
    for (std::size_t row = 0; row < kLayerCount; ++row)
    {
        for (std::size_t col = 0; col < kLayerCount; ++col)
        {
            matrix[row][col] = ixtreeme::physics::DefaultLayerCollision(
                static_cast<ixtreeme::physics::PhysicsLayer>(row),
                static_cast<ixtreeme::physics::PhysicsLayer>(col));
        }
    }

    if (projects.HasProject())
    {
        const std::vector<std::string>& rows = projects.CurrentProject().physicsCollisionMatrixRows;
        if (!rows.empty())
        {
            if (ProjectRowsToPhysicsMatrix(rows, matrix))
                Tracen("[PHYSICS-LAYER] project matrix loaded");
            else
                TraceError("[PHYSICS-LAYER] project matrix invalid, using defaults");
        }
    }

    m_physicsLayerMatrix = matrix;
    m_commands.physicsLayerMatrixChanged = true;
    m_commands.physicsLayerMatrix = m_physicsLayerMatrix;
}

void EditorImGui::StoreProjectPhysicsSettings()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return;
    projects.SetPhysicsCollisionMatrixRows(PhysicsMatrixToProjectRows(m_physicsLayerMatrix));
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

    StoreProjectPhysicsSettings();

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

EditorImGui::ScriptFileChanges EditorImGui::PollScriptFileChanges()
{
    ScriptFileChanges changes;
    const double now = ImGui::GetTime();
    if (!ProjectManager::Instance().HasProject() || !m_assetLibrary)
    {
        m_lastScriptPollSeconds = now;
        return changes;
    }
    const bool firstPass = (m_lastScriptPollSeconds <= 0.0);
    if (!firstPass && now - m_lastScriptPollSeconds < kScriptFilePollIntervalSeconds)
        return changes;  // throttle
    m_lastScriptPollSeconds = now;

    std::error_code ec;

    // (a) .lua Script assets — report each changed asset id (hot-reloaded live). Native .cpp Script
    // assets are ALSO in this category now, but they're compiled (handled by the .cpp poll below), not
    // hot-reloaded — so skip them here or we'd fire a bogus Lua reload on a C++ edit.
    for (const AssetLibrary::Entry& e : m_assetLibrary->EntriesFor(AssetLibrary::Category::Script))
    {
        if (e.filename.size() < 4 ||
            e.filename.compare(e.filename.size() - 4, 4, ".lua") != 0)
            continue;
        const std::filesystem::path p = m_assetLibrary->AbsolutePath(e);
        const std::filesystem::file_time_type mt = std::filesystem::last_write_time(p, ec);
        if (ec)
            continue;  // mid-write/locked — catch it next tick
        const auto it = m_luaMtimes.find(e.id);
        if (it == m_luaMtimes.end())
            m_luaMtimes[e.id] = mt;
        else if (it->second != mt)
        {
            it->second = mt;
            if (!firstPass)
                changes.changedLua.push_back(e.id);
        }
    }

    // (b) native C++ sources (the asset scripts folder, *.cpp,*.h,*.hpp) — a single "something changed"
    // flag that triggers an auto-build. Headers aren't assets, and brand-new .cpp may not be registered
    // yet, so scan the directory directly rather than rely on the manifest.
    const std::filesystem::path scriptsDir = ProjectScriptSourceDir();
    std::unordered_map<std::string, std::filesystem::file_time_type> seen;
    if (std::filesystem::is_directory(scriptsDir, ec))
    {
        for (const std::filesystem::directory_entry& de : std::filesystem::recursive_directory_iterator(
                 scriptsDir, std::filesystem::directory_options::skip_permission_denied, ec))
        {
            if (!de.is_regular_file(ec))
                continue;
            // Skip any CMake build tree that might sit under the scripts dir (its churn isn't a source edit).
            const std::string full = de.path().generic_string();
            if (full.find("/build/") != std::string::npos)
                continue;
            std::string ext = de.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".cxx" && ext != ".cc")
                continue;
            const std::filesystem::file_time_type mt = std::filesystem::last_write_time(de.path(), ec);
            if (ec)
                continue;
            seen[full] = mt;
            const auto it = m_cppMtimes.find(full);
            if (it != m_cppMtimes.end() && it->second != mt && !firstPass)
                changes.changedCpp = true;
        }
    }
    if (!firstPass && seen.size() != m_cppMtimes.size())
        changes.changedCpp = true;  // an add or delete
    m_cppMtimes.swap(seen);

    return changes;
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
        ImGui::Dummy(ImVec2(16.0f, 0.0f));
        ImGui::SameLine();
        {
            // Build the project's native C++ game scripts (<ProjectRoot>/Scripts) into the module DLL.
            const bool canBuild = isEdit && ProjectManager::Instance().HasProject() && !IsBuildRunning();
            if (!canBuild)
                ImGui::BeginDisabled();
            const char* buildLabel = IsBuildRunning() ? "Building..." : "Build";
            if (UI::IconButton(ICON_FA_HAMMER, buildLabel, ImVec2(120.0f, 32.0f)))
                m_commands.buildGameScripts = true;
            if (!canBuild)
            {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(IsBuildRunning() ? "Build in progress..."
                        : !isEdit ? "Stop Play to build" : "Open a project to build C++ scripts");
            }
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

void EditorImGui::RenderBuildOutputPanel()
{
    if (!m_buildOutputPanelOpen)
        return;
    if (ImGui::Begin("Build Output", &m_buildOutputPanelOpen))
    {
        if (m_buildState == ScriptBuildState::Running)
            ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.30f, 1.0f), "Building game scripts...");
        else if (m_buildState == ScriptBuildState::Done && m_buildSucceeded)
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.35f, 1.0f), "Build succeeded — module reloaded.");
        else if (m_buildState == ScriptBuildState::Done)
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Build FAILED — see the log below.");
        ImGui::Separator();
        ImGui::BeginChild("##buildlog", ImVec2(0, 0), ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(m_buildLog.empty() ? "(no output)" : m_buildLog.c_str());
        ImGui::EndChild();
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
            ImGui::TextUnformatted("Editor Camera");
            ImGui::Text("Eye: %.1f, %.1f, %.1f",
                scene.editorCamera.eye[0],
                scene.editorCamera.eye[1],
                scene.editorCamera.eye[2]);
            const CameraEntity* mainCamera = nullptr;
            for (const CameraEntity& cam : scene.cameras)
            {
                if (cam.id == scene.mainCameraId)
                {
                    mainCamera = &cam;
                    break;
                }
            }
            if (mainCamera)
                ImGui::Text("Main Camera FOV: %.1f  Near/Far: %.2f / %.1f",
                    mainCamera->fovDegrees,
                    mainCamera->nearPlane,
                    mainCamera->farPlane);
            else
                ImGui::TextDisabled("No main camera");

            ImGui::Separator();
            ImGui::TextUnformatted("Environment");
            ImGui::Text("Sun: %.1f / %.1f  Intensity: %.2f",
                scene.lighting.directional.elevationDegrees,
                scene.lighting.directional.azimuthDegrees,
                scene.lighting.directional.intensity);
            ImGui::Text("Ambient: %.2f", scene.lighting.ambient.intensity);

            ImGui::Separator();
            ImGui::TextUnformatted("Physics");
            PhysicsSceneSettings physicsSettings = scene.physics;
            bool physicsSettingsChanged = false;
            physicsSettingsChanged |= ImGui::DragFloat3("Gravity", physicsSettings.gravity, 0.05f, -1000.0f, 1000.0f, "%.2f");
            physicsSettingsChanged |= ImGui::DragFloat("Fixed Timestep", &physicsSettings.fixedDeltaSeconds, 0.001f, 0.001f, 0.1f, "%.4f s");
            int maxSubsteps = static_cast<int>(physicsSettings.maxSubsteps);
            if (ImGui::SliderInt("Max Substeps", &maxSubsteps, 1, 16))
            {
                physicsSettings.maxSubsteps = static_cast<std::uint32_t>(std::clamp(maxSubsteps, 1, 16));
                physicsSettingsChanged = true;
            }
            if (physicsSettingsChanged)
                scenes.SetPhysicsSettings(physicsSettings);
            ImGui::TextDisabled("Default Earth gravity: 0.00, -9.81, 0.00");
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
    m_assetInspectorSelectionActive = false;
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

