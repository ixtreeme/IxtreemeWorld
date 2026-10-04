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
    // Scene/project switches would prompt to save (and then restore over) the Play-mode copy.
    const bool editing = CanUseEditorTools();
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Project...", nullptr, false, editing))
            OpenProjectDialog(ProjectDialogMode::Create);
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Project...", nullptr, false, editing))
            OpenProjectDialog(ProjectDialogMode::Open);
        if (ImGui::BeginMenu("      Recent Projects", editing && !projects.RecentProjects().empty()))
        {
            for (const auto& path : projects.RecentProjects())
            {
                const std::string label = path.parent_path().filename().string();
                if (ImGui::MenuItem(label.c_str()))
                    OpenProjectFromDialog(path);
                UI::ItemTooltip(path.parent_path().generic_string().c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FILE "  New Scene", "Ctrl+N", false, editing))
            scenes.NewScene();
        // The project's scenes are listed right here: switching scene needs no OS file dialog.
        if (ImGui::BeginMenu(ICON_FA_GLOBE "  Open Scene", editing && projects.HasProject()))
        {
            const std::vector<AssetLibrary::Entry> sceneAssets = QuerySceneAssets();
            if (sceneAssets.empty())
                ImGui::TextDisabled("The project has no scene yet");
            const std::string currentScene = CachedComparablePath(scenes.GetCurrentScenePath());
            for (const AssetLibrary::Entry& scene : sceneAssets)
            {
                std::error_code ec;
                const std::filesystem::path relative = std::filesystem::relative(scene.originalPath, projects.ProjectRoot(), ec);
                const std::string label = ec ? scene.displayName : relative.generic_string();
                const bool current = !currentScene.empty() && CachedComparablePath(scene.originalPath) == currentScene;
                if (ImGui::MenuItem(label.c_str(), nullptr, current))
                    scenes.LoadScene(scene.originalPath);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("      Recent Scenes", editing && !scenes.GetRecentScenes().empty()))
        {
            for (const std::string& path : scenes.GetRecentScenes())
            {
                const std::string label = std::filesystem::path(path).filename().string();
                if (ImGui::MenuItem(label.c_str()))
                    scenes.LoadScene(path);
                UI::ItemTooltip(path.c_str());
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save", "Ctrl+S", false, projects.HasProject() || scenes.HasOpenScene()))
            SaveProjectAndCurrentScene();
        UI::ItemTooltip("Save the project and the open scene");
        if (ImGui::MenuItem("      Save Scene As...", "Ctrl+Shift+S", false, scenes.HasOpenScene()))
        {
            if (!RefuseSaveDuringPlay())
                scenes.SaveSceneAs({});
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_HAMMER "  Build Game...", nullptr, false,
                editing && projects.HasProject() && !IsGameBuildRunning() && !IsBuildRunning()))
            OpenBuildGameDialog();
        UI::ItemTooltip("Package the project as a game that runs without the editor");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        const HierarchySceneEntity* selected = SelectedHierarchyEntity();
        const bool canEditSelection = editing && selected != nullptr;
        if (ImGui::MenuItem(ICON_FA_BULLSEYE "  Frame Selection", "F", false, selected != nullptr))
            QueueHierarchyFocus(*selected);
        if (ImGui::MenuItem(ICON_FA_PEN "  Rename", nullptr, false, canEditSelection))
            StartHierarchyRename(*selected);
        if (ImGui::MenuItem(ICON_FA_COPY "  Duplicate", nullptr, false, canEditSelection))
        {
            m_commands.hierarchyDuplicateEntity = true;
            m_commands.hierarchyEntityType = selected->type;
            m_commands.hierarchyEntityId = selected->objectId;
            m_commands.hierarchyEntityHandle = selected->entity;
        }
        if (ImGui::MenuItem(ICON_FA_TRASH "  Delete", "Del", false, canEditSelection))
        {
            m_commands.hierarchyDeleteEntity = true;
            m_commands.hierarchyEntityType = selected->type;
            m_commands.hierarchyEntityId = selected->objectId;
            m_commands.hierarchyEntityHandle = selected->entity;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_GEAR "  Project Settings..."))
            m_projectSettingsOpen = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create"))
    {
        RenderHierarchyCreateMenuItems();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        // Opens the panel and brings it to the front (it may be hidden behind another tab); the
        // tick shows which panels are open, the tab's x closes one.
        const auto panelItem = [this](const char* label, bool& open, const char* window) {
            if (ImGui::MenuItem(label, nullptr, open))
            {
                open = true;
                m_pendingViewFocusWindow = window;
            }
        };
        panelItem(ICON_FA_LIST_TREE "  Hierarchy", m_hierarchyPanelOpen, EditorWindow::Hierarchy);
        panelItem(ICON_FA_CIRCLE_INFO "  Inspector", m_inspectorPanelOpen, EditorWindow::Inspector);
        panelItem(ICON_FA_SLIDERS "  Scene Settings", m_sceneSettingsPanelOpen, EditorWindow::SceneSettings);
        panelItem(ICON_FA_FOLDER_OPEN "  Asset Browser", m_assetBrowserPanelOpen, EditorWindow::AssetBrowser);
        panelItem(ICON_FA_CODE "  Scripts", m_scriptsPanelOpen, EditorWindow::Scripts);
        panelItem(ICON_FA_TERMINAL "  Console", m_consolePanelOpen, EditorWindow::Console);
        panelItem(ICON_FA_HAMMER "  Build Output", m_buildOutputPanelOpen, EditorWindow::BuildOutput);
        panelItem(ICON_FA_PERSON_RUNNING "  Animator", m_animatorPanelOpen, EditorWindow::Animator);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CUBES "  Scene View"))
            m_pendingViewFocusWindow = EditorWindow::SceneView;
        if (ImGui::MenuItem(ICON_FA_GAMEPAD "  Game View"))
            m_pendingViewFocusWindow = EditorWindow::Game;
        ImGui::Separator();
        panelItem(ICON_FA_GAUGE_HIGH "  Statistics", m_statisticsWindowOpen, EditorWindow::Statistics);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_ARROWS_ROTATE "  Reset Layout"))
            ResetEditorLayout();
        UI::ItemTooltip("Put every panel back where it starts");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools"))
    {
        if (ImGui::MenuItem(ICON_FA_TREE "  Tree Generator..."))
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
        if (ImGui::MenuItem(ICON_FA_LAYER_GROUP "  Layered World...", nullptr, m_layeredWorldOpen))
            m_layeredWorldOpen = true;
        if (ImGui::MenuItem(ICON_FA_BUG "  Physics Debugger...", nullptr, m_physicsDebuggerOpen))
            m_physicsDebuggerOpen = true;
        ImGui::Separator();
        if (ImGui::BeginMenu(ICON_FA_WRENCH "  Engine Diagnostics"))
        {
            if (ImGui::MenuItem("Capture GPU Frame", "F11"))
                m_commands.captureGpuFrame = true;
            if (ImGui::MenuItem("Dump Frame Profile"))
                m_commands.dumpFrameProfile = true;
            if (ImGui::MenuItem("Dump Material Bindings", "F12"))
                m_commands.dumpMaterialState = true;
            ImGui::Separator();
            bool togglesChanged = false;
            togglesChanged |= ImGui::MenuItem("Disable Shadow Pass", nullptr, &m_debugDisableShadowPass);
            togglesChanged |= ImGui::MenuItem("Disable Water Reflection Pass", nullptr, &m_debugDisableWaterReflectionPass);
            togglesChanged |= ImGui::MenuItem("Disable Asset Library Discovery", nullptr, &m_debugDisableAssetLibraryDiscovery);
            togglesChanged |= ImGui::MenuItem("Disable Asset Watcher Poll", nullptr, &m_debugDisableAssetWatcherPoll);
            togglesChanged |= ImGui::MenuItem("Disable Hierarchy Iteration", nullptr, &m_debugDisableHierarchyIteration);
            if (togglesChanged)
                QueueDebugToggleCommands();
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo Window", nullptr, &m_showDemoWindow);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem(ICON_FA_KEYBOARD "  Keyboard Shortcuts"))
            m_shortcutsWindowOpen = true;
        if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO "  About IxtreemeEngine"))
            m_openAboutPopup = true;
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
    if (!CanUseEditorTools())
    {
        // Play runs on a throwaway copy of the scene (restored on Stop): never write it out. The
        // autosave timer is left as is, so the autosave happens right after Play stops.
        if (!automatic)
            RefuseSaveDuringPlay();
        return false;
    }
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

bool EditorImGui::RefuseSaveDuringPlay()
{
    if (CanUseEditorTools())
        return false;
    m_projectStatus = "Stop Play before saving: Play-mode changes are never saved";
    Tracen("[PROJECT] save refused during Play");
    return true;
}

void EditorImGui::RunProjectAutoSave()
{
    const double now = ImGui::GetTime();
    if (!CanUseEditorTools())
    {
        UpdateAutoSaveWindowTitle(now);
        return; // deferred until Play stops (see SaveProjectAndCurrentScene)
    }
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

    // (b) native C++ sources — every .cpp Script asset (wherever it sits in the asset folder) plus the
    // headers beside them: a single "something changed" flag that triggers an auto-build. A brand-new
    // .cpp counts once the library registered it (the file watcher refreshes the library), so the
    // whole asset folder is never walked here.
    const std::filesystem::path scriptsDir = ProjectScriptSourceDir();
    std::unordered_map<std::string, std::filesystem::file_time_type> seen;
    std::set<std::filesystem::path> sourceDirs;
    const auto stampSource = [&](const std::filesystem::path& path) {
        const std::filesystem::file_time_type mt = std::filesystem::last_write_time(path, ec);
        if (ec)
            return;
        const std::string full = path.generic_string();
        seen[full] = mt;
        const auto it = m_cppMtimes.find(full);
        if (it != m_cppMtimes.end() && it->second != mt && !firstPass)
            changes.changedCpp = true;
    };
    for (const AssetLibrary::Entry& e : m_assetLibrary->EntriesFor(AssetLibrary::Category::Script))
    {
        const std::filesystem::path p = m_assetLibrary->AbsolutePath(e);
        if (!IsNativeScriptSource(scriptsDir, p))
            continue;
        stampSource(p);
        sourceDirs.insert(p.parent_path());
    }
    for (const std::filesystem::path& dir : sourceDirs)
    {
        for (const std::filesystem::directory_entry& de : std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec))
        {
            std::error_code entryEc;
            if (de.is_regular_file(entryEc) && de.path().extension() != ".cpp" && IsNativeScriptSource(scriptsDir, de.path()))
                stampSource(de.path());
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
        {
            if (!RefuseSaveDuringPlay())
                SceneManager::Instance().SaveSceneAs({});
        }
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
        if (const HierarchySceneEntity* selected = SelectedHierarchyEntity())
            QueueHierarchyFocus(*selected);
        else if (m_waterBodyState.selected)
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

    // W / E / R pick the Move / Rotate / Scale tool, unless the right mouse button is flying the
    // camera (W is "forward" then).
    if (CanUseEditorTools() && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        const MapEditorGizmoOperation previous = m_gizmoOperation;
        if (ImGui::IsKeyPressed(ImGuiKey_W, false))
            m_gizmoOperation = MapEditorGizmoOperation::Translate;
        else if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            m_gizmoOperation = MapEditorGizmoOperation::Rotate;
        else if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            m_gizmoOperation = MapEditorGizmoOperation::Scale;
        if (m_gizmoOperation != previous)
        {
            m_commands.gizmoSettingsChanged = true;
            m_commands.gizmoOperation = m_gizmoOperation;
            m_commands.gizmoSnapEnabled = m_gizmoSnapEnabled;
            m_commands.gizmoSnapValue = m_gizmoSnapValue;
        }
    }

    if (io.KeyCtrl && !io.KeyShift && CanUseEditorTools() && ImGui::IsKeyPressed(ImGuiKey_N, false))
        SceneManager::Instance().NewScene();
}

void EditorImGui::RenderBuildOutputPanel()
{
    if (!m_buildOutputPanelOpen)
        return;
    DockBesideIfUnplaced(EditorWindow::BuildOutput, EditorWindow::AssetBrowser);
    if (ImGui::Begin(EditorWindow::BuildOutput, &m_buildOutputPanelOpen))
    {
        if (m_buildState == ScriptBuildState::Running)
            ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.30f, 1.0f), "Building game scripts...");
        else if (m_buildState == ScriptBuildState::Done && m_buildSucceeded)
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.35f, 1.0f), "Build succeeded — module reloaded.");
        else if (m_buildState == ScriptBuildState::Done)
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Build FAILED — see the log below.");

        // "Build Game" status, with the finished game one click away.
        if (m_gameBuildState == ScriptBuildState::Running)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.30f, 1.0f),
                "Building the game... (the first time this also compiles the runtime player: several minutes)");
        }
        else if (m_gameBuildState == ScriptBuildState::Done && m_gameBuildSucceeded)
        {
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.35f, 1.0f), "Game built: %s",
                m_gameBuildExecutable.generic_string().c_str());
            ImGui::SameLine();
            std::string openError;
            if (ImGui::SmallButton("Run Game") && !platform::OpenInDefaultApp(m_gameBuildExecutable, &openError))
                m_projectStatus = "Run failed: " + openError;
            ImGui::SameLine();
            if (ImGui::SmallButton("Open Folder") &&
                !platform::OpenInDefaultApp(m_gameBuildExecutable.parent_path(), &openError))
                m_projectStatus = "Open folder failed: " + openError;
        }
        else if (m_gameBuildState == ScriptBuildState::Done)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Game build FAILED — see the log below.");
        }
        ImGui::Separator();
        ImGui::BeginChild("##buildlog", ImVec2(0, 0), ImGuiChildFlags_Borders,
            ImGuiWindowFlags_HorizontalScrollbar);
        if (m_gameBuildState == ScriptBuildState::Done)
        {
            ImGui::TextUnformatted(m_gameBuildLog.empty() ? "(no output)" : m_gameBuildLog.c_str());
            if (!m_buildLog.empty())
                ImGui::Separator();
        }
        if (m_gameBuildState != ScriptBuildState::Done || !m_buildLog.empty())
            ImGui::TextUnformatted(m_buildLog.empty() ? "(no output)" : m_buildLog.c_str());
        ImGui::EndChild();
    }
    ImGui::End();
}

void EditorImGui::OpenBuildGameDialog()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (!projects.HasProject())
        return;
    const std::string name = projects.CurrentProject().name.empty() ? "Game" : projects.CurrentProject().name;
    CopyToBuffer(m_buildGameName, sizeof(m_buildGameName), name);
    // Default output: <Project>/Build/<Name> (a "build" folder: the asset database and the package
    // copy both skip it, so the game never ends up inside itself).
    std::string folderName;
    for (char c : name)
        folderName.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_');
    const std::string defaultOutput = (projects.ProjectRoot() / "Build" / folderName).string();
    if (m_buildGameOutputDir[0] == '\0' ||
        std::string(m_buildGameOutputDir).rfind(projects.ProjectRoot().string(), 0) != 0)
        CopyToBuffer(m_buildGameOutputDir, sizeof(m_buildGameOutputDir), defaultOutput);

    // Start in the project's startup scene, else the open scene.
    m_buildGameStartupScene = projects.CurrentProject().startupScene;
    if (m_buildGameStartupScene.empty())
    {
        const std::string current = SceneManager::Instance().GetCurrentScenePath();
        std::error_code ec;
        const std::filesystem::path relative = std::filesystem::relative(current, projects.ProjectRoot(), ec);
        if (!current.empty() && !ec)
            m_buildGameStartupScene = relative.generic_string();
    }
    // Compile the C++ game scripts first when the project has any.
    m_buildGameCompileScripts = false;
    if (m_assetLibrary)
    {
        for (const AssetLibrary::Entry& entry : m_assetLibrary->EntriesFor(AssetLibrary::Category::Script))
            m_buildGameCompileScripts = m_buildGameCompileScripts || ToLowerAscii(entry.filename).ends_with(".cpp");
    }
    m_openBuildGamePopup = true;
}

void EditorImGui::RenderBuildGamePopup()
{
    if (m_openBuildGamePopup)
    {
        ImGui::OpenPopup("Build Game");
        m_openBuildGamePopup = false;
    }
    if (!ImGui::BeginPopupModal("Build Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ProjectManager& projects = ProjectManager::Instance();
    ImGui::TextUnformatted("Package this project as a game that runs without the editor.");
    ImGui::Separator();
    ImGui::SetNextItemWidth(360.0f);
    ImGui::InputText("Game name", m_buildGameName, sizeof(m_buildGameName));
    ImGui::SetNextItemWidth(560.0f);
    ImGui::InputText("Output folder", m_buildGameOutputDir, sizeof(m_buildGameOutputDir));

    ImGui::SetNextItemWidth(360.0f);
    const std::string preview = m_buildGameStartupScene.empty() ? std::string("(choose a scene)") : m_buildGameStartupScene;
    if (ImGui::BeginCombo("Startup scene", preview.c_str()))
    {
        for (const AssetLibrary::Entry& scene : QuerySceneAssets())
        {
            std::error_code ec;
            const std::filesystem::path relative = std::filesystem::relative(scene.originalPath, projects.ProjectRoot(), ec);
            const std::string label = relative.generic_string();
            if (ec || label.empty() || label.rfind("..", 0) == 0)
                continue;  // the game can only start in a scene that is part of the project
            if (ImGui::Selectable(label.c_str(), label == m_buildGameStartupScene))
                m_buildGameStartupScene = label;
        }
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Compile the C++ game scripts first", &m_buildGameCompileScripts);
    ImGui::Checkbox("Run the game when it is built", &m_buildGameRunWhenDone);
    ImGui::TextDisabled("The folder gets <Game name>.exe, the engine runtime files and the project (Game/).");
    ImGui::TextDisabled("The project and the open scene are saved first. The first build also compiles the");
    ImGui::TextDisabled("editor-less runtime player, which takes several minutes.");

    const bool canBuild = m_buildGameName[0] != '\0' && m_buildGameOutputDir[0] != '\0' &&
        !m_buildGameStartupScene.empty() && CanUseEditorTools() && !IsGameBuildRunning() && !IsBuildRunning();
    ImGui::Separator();
    if (!canBuild)
        ImGui::BeginDisabled();
    if (ImGui::Button("Build", ImVec2(120.0f, 0.0f)))
    {
        // The package is made from what is on disk: save first.
        if (SaveProjectAndCurrentScene(false))
        {
            m_commands.buildGame = true;
            m_commands.buildGameName = m_buildGameName;
            m_commands.buildGameOutputDir = m_buildGameOutputDir;
            m_commands.buildGameStartupScene = m_buildGameStartupScene;
            m_commands.buildGameCompileScripts = m_buildGameCompileScripts;
            m_commands.buildGameRunWhenDone = m_buildGameRunWhenDone;
            m_gameBuildState = ScriptBuildState::Running;  // until EngineApplication reports back
            m_buildOutputPanelOpen = true;
            ImGui::CloseCurrentPopup();
        }
    }
    if (!canBuild)
        ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    if (!m_projectStatus.empty())
        ImGui::TextDisabled("%s", m_projectStatus.c_str());
    ImGui::EndPopup();
}

void EditorImGui::RenderSceneSettingsPanel()
{
    if (!m_sceneSettingsPanelOpen)
        return;
    DockBesideIfUnplaced(EditorWindow::SceneSettings, EditorWindow::Inspector);
    if (ImGui::Begin(EditorWindow::SceneSettings, &m_sceneSettingsPanelOpen))
    {
        SceneManager& scenes = SceneManager::Instance();
        if (!scenes.HasOpenScene())
        {
            ImGui::TextDisabled("No scene is open");
            ImGui::TextWrapped("Create or open a scene from the File menu.");
        }
        else
        {
            const SceneData& scene = scenes.GetCurrentScene();
            if (ImGui::CollapsingHeader(ICON_FA_GLOBE "  Scene", ImGuiTreeNodeFlags_DefaultOpen))
            {
                char nameBuffer[128]{};
                std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", scene.name.c_str());
                if (UI::Prop::InputText("Name", nameBuffer, sizeof(nameBuffer)))
                    scenes.SetSceneName(nameBuffer);
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
                    UI::Prop::Text("Main camera", "%s  (FOV %.0f)", mainCamera->name.c_str(), mainCamera->fovDegrees);
                else
                    UI::Prop::Text("Main camera", "none: the Game view and the built game need one");
            }

            if (ImGui::CollapsingHeader(ICON_FA_SUN "  Environment", ImGuiTreeNodeFlags_DefaultOpen))
                RenderLightingPanel();

            if (ImGui::CollapsingHeader(ICON_FA_CLOUD_SUN "  Sky", ImGuiTreeNodeFlags_DefaultOpen))
                RenderSkyPanel();

            if (ImGui::CollapsingHeader(ICON_FA_SUN "  God rays", ImGuiTreeNodeFlags_DefaultOpen))
                RenderGodRaysPanel();

            if (ImGui::CollapsingHeader(ICON_FA_CAMERA "  Tone mapping", ImGuiTreeNodeFlags_DefaultOpen))
                RenderToneMappingPanel();

            if (ImGui::CollapsingHeader(ICON_FA_CUBES "  Physics", ImGuiTreeNodeFlags_DefaultOpen))
            {
                PhysicsSceneSettings physicsSettings = scene.physics;
                bool physicsSettingsChanged = false;
                physicsSettingsChanged |= UI::Prop::DragFloat3("Gravity", physicsSettings.gravity, 0.05f, -1000.0f, 1000.0f, "%.2f");
                UI::ItemTooltip("Earth gravity is 0, -9.81, 0 (m/s^2)");
                physicsSettingsChanged |= UI::Prop::DragFloat("Fixed timestep", &physicsSettings.fixedDeltaSeconds, 0.001f, 0.001f, 0.1f, "%.4f s");
                int maxSubsteps = static_cast<int>(physicsSettings.maxSubsteps);
                if (UI::Prop::SliderInt("Max substeps", &maxSubsteps, 1, 16))
                {
                    physicsSettings.maxSubsteps = static_cast<std::uint32_t>(std::clamp(maxSubsteps, 1, 16));
                    physicsSettingsChanged = true;
                }
                if (physicsSettingsChanged)
                    scenes.SetPhysicsSettings(physicsSettings);
                ImGui::TextDisabled("Which layers collide: Edit > Project Settings.");
            }
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

const HierarchySceneEntity* EditorImGui::SelectedHierarchyEntity() const
{
    for (const HierarchySceneEntity& entity : m_hierarchyEntities)
    {
        if (entity.selected)
            return &entity;
    }
    return FindHierarchyEntity(m_selectedHierarchyEntity);
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

