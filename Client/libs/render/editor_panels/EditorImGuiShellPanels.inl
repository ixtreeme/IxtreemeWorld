// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// The editor shell: the fixed toolbar under the menu bar, the status bar, the Console, and the
// utility windows opened from the menus (Statistics, Physics Debugger, Project Settings, ...).

namespace
{
std::string ConsoleClockNow()
{
    try
    {
        const std::chrono::zoned_time now{std::chrono::current_zone(),
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())};
        return std::format("{:%H:%M:%S}", now);
    }
    catch (const std::exception&)
    {
        return std::format("{:%H:%M:%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    }
}

// Status texts are plain sentences set all over the editor: their wording tells the severity.
bool ContainsAnyCaseInsensitive(const std::string& text, std::initializer_list<const char*> needles)
{
    const std::string lower = ToLowerAscii(text);
    return std::any_of(needles.begin(), needles.end(), [&](const char* needle) {
        return lower.find(needle) != std::string::npos;
    });
}

// A window opened from a menu appears in the middle of the editor the first time, at a useful size.
void SetNextUtilityWindowPlacement(const ImVec2& size)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
}
}

void EditorImGui::PushConsoleMessage(ConsoleSeverity severity, std::string text)
{
    if (text.empty())
        return;
    ConsoleMessage message;
    message.time = ImGui::GetTime();
    message.clock = ConsoleClockNow();
    message.severity = severity;
    message.text = std::move(text);
    m_statusBarMessage = message;
    m_consoleMessages.push_back(std::move(message));
    constexpr std::size_t kMaxConsoleMessages = 500;
    if (m_consoleMessages.size() > kMaxConsoleMessages)
        m_consoleMessages.erase(m_consoleMessages.begin(),
            m_consoleMessages.begin() + static_cast<std::ptrdiff_t>(m_consoleMessages.size() - kMaxConsoleMessages));
    m_consoleScrollToBottom = true;
}

void EditorImGui::TrackStatusMessages()
{
    const auto severityOf = [](const std::string& text) {
        if (ContainsAnyCaseInsensitive(text, {"fail", "error", "invalid", "cannot", "can't", "unable", "missing"}))
            return ConsoleSeverity::Error;
        if (ContainsAnyCaseInsensitive(text, {"stop play", "blocked", "refused", "warning", "before"}))
            return ConsoleSeverity::Warning;
        return ConsoleSeverity::Info;
    };
    const auto track = [&](const std::string& current, std::string& lastSeen) {
        if (current == lastSeen)
            return;
        lastSeen = current;
        PushConsoleMessage(severityOf(current), current);
    };
    track(m_projectStatus, m_lastSeenProjectStatus);
    track(m_assetStatus, m_lastSeenAssetStatus);
    track(m_layerAuthoringStatus, m_lastSeenLayerStatus);

    if (m_buildState != m_lastSeenBuildState)
    {
        m_lastSeenBuildState = m_buildState;
        if (m_buildState == ScriptBuildState::Running)
            PushConsoleMessage(ConsoleSeverity::Info, "Compiling the C++ game scripts...");
        else if (m_buildState == ScriptBuildState::Done && m_buildSucceeded)
            PushConsoleMessage(ConsoleSeverity::Info, "C++ scripts compiled and reloaded.");
        else if (m_buildState == ScriptBuildState::Done)
            PushConsoleMessage(ConsoleSeverity::Error, "C++ script compilation failed: see Build Output.");
    }
    if (m_gameBuildState != m_lastSeenGameBuildState)
    {
        m_lastSeenGameBuildState = m_gameBuildState;
        if (m_gameBuildState == ScriptBuildState::Running)
            PushConsoleMessage(ConsoleSeverity::Info, "Building the game...");
        else if (m_gameBuildState == ScriptBuildState::Done && m_gameBuildSucceeded)
            PushConsoleMessage(ConsoleSeverity::Info, "Game built: " + m_gameBuildExecutable.generic_string());
        else if (m_gameBuildState == ScriptBuildState::Done)
            PushConsoleMessage(ConsoleSeverity::Error, "Game build failed: see Build Output.");
    }
}

void EditorImGui::RenderGizmoControls()
{
    auto publish = [this]() {
        m_commands.gizmoSettingsChanged = true;
        m_commands.gizmoOperation = m_gizmoOperation;
        m_commands.gizmoSnapEnabled = m_gizmoSnapEnabled;
        m_commands.gizmoSnapValue = m_gizmoSnapValue;
    };
    const float buttonSize = ImGui::GetFrameHeight() + 4.0f;
    auto operationButton = [&](const char* icon, const char* tooltip, MapEditorGizmoOperation operation) {
        if (UI::ToggleIconButton(icon, m_gizmoOperation == operation, tooltip, buttonSize))
        {
            m_gizmoOperation = operation;
            publish();
            Tracenf("[EDITOR-GIZMO] Gizmo operation changed: %d", static_cast<int>(operation));
        }
        ImGui::SameLine(0.0f, 2.0f);
    };
    operationButton(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "Move (W)", MapEditorGizmoOperation::Translate);
    operationButton(ICON_FA_ROTATE, "Rotate (E)", MapEditorGizmoOperation::Rotate);
    operationButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Scale (R)", MapEditorGizmoOperation::Scale);

    ImGui::SameLine(0.0f, 10.0f);
    bool snapChanged = false;
    if (UI::ToggleIconButton(ICON_FA_MAGNET, m_gizmoSnapEnabled,
            m_gizmoSnapEnabled ? "Snapping on: moves, rotations and scales step by the value beside"
                               : "Snapping off: click to step moves, rotations and scales",
            buttonSize))
    {
        m_gizmoSnapEnabled = !m_gizmoSnapEnabled;
        snapChanged = true;
    }
    if (m_gizmoSnapEnabled)
    {
        ImGui::SameLine(0.0f, 4.0f);
        const char* labels[] = {"0.1", "0.5", "1.0", "5.0"};
        ImGui::SetNextItemWidth(64.0f);
        snapChanged = ImGui::Combo("##GizmoSnap", &m_gizmoSnapIndex, labels, IM_ARRAYSIZE(labels)) || snapChanged;
        UI::ItemTooltip("Snap step");
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

void EditorImGui::RenderMainToolbar()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float padding = 6.0f;
    const float buttonSize = ImGui::GetFrameHeight() + 4.0f;
    const float height = buttonSize + padding * 2.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus;

    // While Play runs the toolbar turns green-tinted: changes made then are thrown away on Stop,
    // so the mode must be obvious at a glance.
    const bool playing = m_playModeState.mode != EditorPlayMode::Edit;
    const ImVec4 barColor = playing ? ImVec4(0.13f, 0.24f, 0.16f, 1.0f) : style.Colors[ImGuiCol_MenuBarBg];
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, padding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, barColor);
    const bool open = ImGui::BeginViewportSideBar("##MainToolbar", viewport, ImGuiDir_Up, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open)
    {
        ImGui::End();
        return;
    }

    const bool isEdit = m_playModeState.mode == EditorPlayMode::Edit;
    const bool isPlay = m_playModeState.mode == EditorPlayMode::Play;
    const bool isPaused = m_playModeState.mode == EditorPlayMode::PlayPaused;
    const float windowWidth = ImGui::GetWindowWidth();

    // Left: how the selection is moved in the Scene View.
    RenderGizmoControls();

    // Centre: Play / Pause / Stop, with the current mode under the user's eyes.
    {
        const float playGroupWidth = buttonSize * 2.0f + 2.0f;
        ImGui::SameLine((windowWidth - playGroupWidth) * 0.5f);
        const bool canPlay = SceneManager::Instance().HasOpenScene();
        if (!canPlay && isEdit)
            ImGui::BeginDisabled();
        if (UI::ToggleIconButton(isEdit ? ICON_FA_PLAY : ICON_FA_STOP,
                !isEdit,
                isEdit ? (canPlay ? "Play the scene (F5)" : "Open a scene to Play")
                       : "Stop: back to editing; Play-mode changes are discarded (Shift+F5)",
                buttonSize))
        {
            if (isEdit)
                m_commands.enterPlayMode = true;
            else
                m_commands.exitPlayMode = true;
        }
        if (!canPlay && isEdit)
            ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 2.0f);
        if (isEdit)
            ImGui::BeginDisabled();
        if (UI::ToggleIconButton(isPaused ? ICON_FA_PLAY : ICON_FA_PAUSE,
                isPaused,
                isPaused ? "Resume (F6)" : "Pause (F6)",
                buttonSize))
        {
            if (isPlay)
                m_commands.pausePlayMode = true;
            else if (isPaused)
                m_commands.resumePlayMode = true;
        }
        if (isEdit)
            ImGui::EndDisabled();
        if (!isEdit)
        {
            ImGui::SameLine(0.0f, 10.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(isPlay ? UI::Theme::PlayMode : UI::Theme::PausedMode,
                "%s  %.1fs",
                isPlay ? "PLAYING" : "PAUSED",
                m_playModeState.elapsedSeconds);
        }
    }

    // Right: compile the project's LEGACY C++ game scripts. Only shown while the project actually has
    // .cpp Script assets — AngelScript/Lua scripts are hot-reloaded and need no build step.
    if (ProjectHasNativeScriptSources())
    {
        const char* label = IsBuildRunning() ? ICON_FA_HAMMER "  Compiling..." : ICON_FA_HAMMER "  Compile Scripts";
        const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, windowWidth - width - 10.0f));
        const bool canBuild = isEdit && ProjectManager::Instance().HasProject() && !IsBuildRunning();
        if (!canBuild)
            ImGui::BeginDisabled();
        if (ImGui::Button(label, ImVec2(width, buttonSize)))
            m_commands.buildGameScripts = true;
        if (!canBuild)
            ImGui::EndDisabled();
        UI::ItemTooltip(IsBuildRunning() ? "Compiling the legacy C++ game scripts..."
            : !isEdit                    ? "Stop Play to compile the legacy C++ game scripts"
            : !ProjectManager::Instance().HasProject()
                ? "Open a project to compile its legacy C++ game scripts"
                : "Compile the project's legacy C++ game scripts and reload them (the result is in Build Output)");
    }
    ImGui::End();
}

void EditorImGui::RenderStatusBar()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight() + 4.0f;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg]);
    const bool open = ImGui::BeginViewportSideBar("##StatusBar", viewport, ImGuiDir_Down, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open)
    {
        ImGui::End();
        return;
    }

    ImGui::AlignTextToFramePadding();
    // Left: the latest message; a click opens the Console with the full history.
    if (m_statusBarMessage && ImGui::GetTime() - m_statusBarMessage->time < 30.0)
    {
        const ConsoleMessage& message = *m_statusBarMessage;
        const ImVec4 color = message.severity == ConsoleSeverity::Error ? UI::Theme::Error
            : message.severity == ConsoleSeverity::Warning               ? UI::Theme::Warning
                                                                         : ImGui::GetStyle().Colors[ImGuiCol_Text];
        const char* icon = message.severity == ConsoleSeverity::Error ? ICON_FA_CIRCLE_EXCLAMATION
            : message.severity == ConsoleSeverity::Warning               ? ICON_FA_TRIANGLE_EXCLAMATION
                                                                         : ICON_FA_CIRCLE_INFO;
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        const std::string text = std::string(icon) + "  " + message.text;
        if (ImGui::Selectable(text.c_str(), false, ImGuiSelectableFlags_None,
                ImVec2(std::max(0.0f, ImGui::GetContentRegionAvail().x * 0.6f), 0.0f)))
        {
            m_consolePanelOpen = true;
            m_pendingViewFocusWindow = EditorWindow::Console;
        }
        ImGui::PopStyleColor();
        UI::ItemTooltip("Show all messages in the Console");
    }
    else
    {
        ImGui::TextDisabled(CanUseEditorTools() ? "Ready" : "Playing: changes made now are discarded on Stop");
    }

    // Right: what is open, whether it is saved, and how fast the editor runs.
    std::string right;
    ProjectManager& projects = ProjectManager::Instance();
    SceneManager& scenes = SceneManager::Instance();
    if (projects.HasProject())
        right += std::string(ICON_FA_FOLDER_OPEN " ") + projects.CurrentProject().name + "    ";
    if (scenes.HasOpenScene())
    {
        const std::string scenePath = scenes.GetCurrentScenePath();
        std::string sceneName = scenePath.empty() ? scenes.GetCurrentScene().name
                                                  : std::filesystem::path(scenePath).stem().string();
        if (sceneName.empty())
            sceneName = "Untitled";
        right += std::string(ICON_FA_GLOBE " ") + sceneName + (scenes.IsDirty() ? " (unsaved)" : "") + "    ";
    }
    if (projects.HasProject() && CanUseEditorTools() && m_lastAutoSaveSeconds > 0.0)
    {
        const int remaining = static_cast<int>(std::max(0.0,
            kProjectAutoSaveIntervalSeconds - (ImGui::GetTime() - m_lastAutoSaveSeconds)));
        right += std::format("{} Autosave {:02}:{:02}    ", ICON_FA_CLOCK, remaining / 60, remaining % 60);
    }
    right += std::format("{:.0f} FPS  {:.2f} ms",
        m_engineStats.fps,
        m_engineStats.averageFrameMs > 0.0 ? m_engineStats.averageFrameMs : m_engineStats.frameMs);
    const float rightWidth = ImGui::CalcTextSize(right.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - rightWidth - 12.0f));
    ImGui::TextDisabled("%s", right.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Ctrl+S saves the project and the scene. View > Statistics shows the frame breakdown.");
    ImGui::End();
}

void EditorImGui::RenderConsolePanel()
{
    if (!m_consolePanelOpen)
        return;
    DockBesideIfUnplaced(EditorWindow::Console, EditorWindow::AssetBrowser);
    if (!ImGui::Begin(EditorWindow::Console, &m_consolePanelOpen))
    {
        ImGui::End();
        return;
    }
    if (UI::IconButton(ICON_FA_TRASH, "Clear"))
    {
        m_consoleMessages.clear();
        m_statusBarMessage.reset();
    }
    ImGui::SameLine();
    std::size_t counts[3] = {0, 0, 0};
    for (const ConsoleMessage& message : m_consoleMessages)
        ++counts[static_cast<int>(message.severity)];
    const auto filterToggle = [&](const char* icon, const char* name, std::size_t count, bool& shown, const ImVec4& color) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, shown ? color : UI::Theme::TextMuted);
        const std::string label = std::format("{} {}##{}", icon, count, name);
        if (ImGui::Button(label.c_str()))
            shown = !shown;
        ImGui::PopStyleColor();
        UI::ItemTooltip(shown ? std::format("Hide {}", name).c_str() : std::format("Show {}", name).c_str());
    };
    filterToggle(ICON_FA_CIRCLE_INFO, "messages", counts[0], m_consoleShowInfo, ImGui::GetStyle().Colors[ImGuiCol_Text]);
    filterToggle(ICON_FA_TRIANGLE_EXCLAMATION, "warnings", counts[1], m_consoleShowWarnings, UI::Theme::Warning);
    filterToggle(ICON_FA_CIRCLE_EXCLAMATION, "errors", counts[2], m_consoleShowErrors, UI::Theme::Error);
    ImGui::Separator();

    if (ImGui::BeginChild("##console_log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
    {
        if (m_consoleMessages.empty())
            ImGui::TextDisabled("No messages yet. Saves, imports, builds and errors are listed here.");
        for (const ConsoleMessage& message : m_consoleMessages)
        {
            const bool shown = message.severity == ConsoleSeverity::Error ? m_consoleShowErrors
                : message.severity == ConsoleSeverity::Warning               ? m_consoleShowWarnings
                                                                             : m_consoleShowInfo;
            if (!shown)
                continue;
            ImGui::TextDisabled("%s", message.clock.c_str());
            ImGui::SameLine();
            if (message.severity == ConsoleSeverity::Error)
                ImGui::TextColored(UI::Theme::Error, ICON_FA_CIRCLE_EXCLAMATION "  %s", message.text.c_str());
            else if (message.severity == ConsoleSeverity::Warning)
                ImGui::TextColored(UI::Theme::Warning, ICON_FA_TRIANGLE_EXCLAMATION "  %s", message.text.c_str());
            else
                ImGui::Text(ICON_FA_CIRCLE_INFO "  %s", message.text.c_str());
        }
        if (m_consoleScrollToBottom)
        {
            ImGui::SetScrollHereY(1.0f);
            m_consoleScrollToBottom = false;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void EditorImGui::RenderStatisticsWindow()
{
    if (!m_statisticsWindowOpen)
        return;
    SetNextUtilityWindowPlacement(ImVec2(460.0f, 520.0f));
    if (!ImGui::Begin(EditorWindow::Statistics, &m_statisticsWindowOpen))
    {
        ImGui::End();
        return;
    }
    const double frameMs = m_engineStats.averageFrameMs > 0.0 ? m_engineStats.averageFrameMs : m_engineStats.frameMs;
    UI::SectionHeader("Frame");
    UI::Prop::Text("FPS", "%.1f", m_engineStats.fps);
    UI::Prop::Text("Frame time", "%.2f ms  (min %.2f / max %.2f)", frameMs, m_engineStats.minFrameMs, m_engineStats.maxFrameMs);
    UI::Prop::Text("Window", "%u x %u", m_engineStats.swapchainWidth, m_engineStats.swapchainHeight);
    UI::Prop::Text("Render target", "%u x %u", m_engineStats.renderWidth, m_engineStats.renderHeight);
    UI::Prop::Text("Present", "%s", m_engineStats.presentUncapped ? "uncapped (no vsync)" : "vsync (limited to the monitor rate)");

    const float budgetFraction = static_cast<float>(std::clamp(m_engineStats.frameBudgetPercent / 100.0, 0.0, 1.0));
    const std::string budgetLabel = std::format("{:.0f}%", m_engineStats.frameBudgetPercent);
    UI::Property("Budget @60 FPS", [&](const char*) {
        ImGui::ProgressBar(budgetFraction, ImVec2(-FLT_MIN, 0.0f), budgetLabel.c_str());
        return false;
    });
    const float cpuFraction = static_cast<float>(std::clamp(m_engineStats.processCpuPercent / 100.0, 0.0, 1.0));
    const std::string cpuLabel = std::format("{:.0f}%", m_engineStats.processCpuPercent);
    UI::Property("Process CPU", [&](const char*) {
        ImGui::ProgressBar(cpuFraction, ImVec2(-FLT_MIN, 0.0f), cpuLabel.c_str());
        return false;
    });

    UI::SectionHeader("CPU time per frame");
    UI::Prop::Text("Total", "%.2f ms", m_engineStats.cpuTotalMs);
    UI::Prop::Text("Scene render", "%.2f ms", m_engineStats.cpuSceneRenderMs);
    UI::Prop::Text("Editor UI", "%.2f ms", m_engineStats.cpuEditorUiMs);
    UI::Prop::Text("Submit / present", "%.2f ms", m_engineStats.cpuSubmitPresentMs);
    UI::Prop::Text("ECS update", "%.2f ms", m_engineStats.cpuEcsUpdateMs);
    UI::Prop::Text("Asset watcher", "%.2f ms", m_engineStats.cpuAssetWatcherMs);

    UI::SectionHeader("Scene");
    UI::Prop::Text("Entities", "%zu", m_engineStats.sceneEntityCount);
    UI::Prop::Text("Static meshes", "%zu submitted, %zu draw calls", m_engineStats.staticMeshSubmitted, m_engineStats.staticMeshDrawCalls);
    UI::Prop::Text("Frame #", "%llu", static_cast<unsigned long long>(m_engineStats.frameNumber));

    if (ImGui::CollapsingHeader("Math library"))
    {
        UI::Prop::Text("Backend", "%s", ixtreeme::math::simd::ActiveBackendName());
        static ixtreeme::math::diagnostics::SelfCheckResult mathSelfCheck = ixtreeme::math::diagnostics::RunSelfCheck();
        UI::Prop::Text("Self-check", "%s  (max error %.6f, %u checks)",
            mathSelfCheck.passed ? "OK" : "FAILED",
            mathSelfCheck.maxAbsError,
            mathSelfCheck.checks);
        static ixtreeme::math::diagnostics::BenchmarkResult mathBenchmark{};
        static bool mathBenchmarkReady = false;
        if (ImGui::Button("Run Math Benchmark"))
        {
            mathBenchmark = ixtreeme::math::diagnostics::RunBenchmark(75000);
            mathBenchmarkReady = true;
            Tracenf("[MATH-BENCH] backend=%s iters=%u mat4=%.3fms rowMat4=%.3fms vec4=%.3fms frustumAabb=%.3fms checksum=%.3f",
                mathBenchmark.backend.c_str(),
                mathBenchmark.iterations,
                mathBenchmark.columnMajorMat4Ms,
                mathBenchmark.rowMajorMat4Ms,
                mathBenchmark.transformVec4Ms,
                mathBenchmark.frustumAabbMs,
                mathBenchmark.checksum);
        }
        if (mathBenchmarkReady)
        {
            ImGui::TextDisabled("mat4 %.2f ms | row %.2f ms | vec4 %.2f ms | frustum %.2f ms",
                mathBenchmark.columnMajorMat4Ms,
                mathBenchmark.rowMajorMat4Ms,
                mathBenchmark.transformVec4Ms,
                mathBenchmark.frustumAabbMs);
        }
    }
    ImGui::End();
}

void EditorImGui::QueueDebugToggleCommands()
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

void EditorImGui::RenderPhysicsDebuggerWindow()
{
    if (!m_physicsDebuggerOpen)
        return;
    SetNextUtilityWindowPlacement(ImVec2(520.0f, 560.0f));
    if (!ImGui::Begin(EditorWindow::PhysicsDebugger, &m_physicsDebuggerOpen))
    {
        ImGui::End();
        return;
    }

    UI::SectionHeader("Show in the Scene View");
    bool togglesChanged = false;
    togglesChanged |= UI::Prop::Checkbox("Colliders", &m_debugShowPhysicsColliders);
    togglesChanged |= UI::Prop::Checkbox("Contacts", &m_debugShowPhysicsContacts);
    togglesChanged |= UI::Prop::Checkbox("Body centers", &m_debugShowPhysicsBodyCenters);
    if (togglesChanged)
        QueueDebugToggleCommands();

    constexpr std::size_t kLayerCount = static_cast<std::size_t>(ixtreeme::physics::PhysicsLayer::Count);
    const auto layerMaskEditor = [&](const char* id, std::uint32_t& mask) {
        ImGui::PushID(id);
        for (std::size_t layerIndex = 0; layerIndex < kLayerCount; ++layerIndex)
        {
            const auto layer = static_cast<ixtreeme::physics::PhysicsLayer>(layerIndex);
            bool enabled = (mask & ixtreeme::physics::PhysicsLayerMask(layer)) != 0u;
            ImGui::PushID(static_cast<int>(layerIndex));
            if (layerIndex % 4u != 0u)
                ImGui::SameLine(static_cast<float>(layerIndex % 4u) * 110.0f + ImGui::GetStyle().WindowPadding.x);
            if (ImGui::Checkbox(ixtreeme::physics::DisplayName(layer), &enabled))
            {
                if (enabled)
                    mask |= ixtreeme::physics::PhysicsLayerMask(layer);
                else
                    mask &= ~ixtreeme::physics::PhysicsLayerMask(layer);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    };

    UI::SectionHeader("Raycast from the camera");
    UI::Prop::DragFloat("Distance", &m_physicsRaycastDistance, 5.0f, 0.1f, 5000.0f, "%.1f m");
    m_physicsRaycastDistance = std::clamp(m_physicsRaycastDistance, 0.1f, 5000.0f);
    UI::Prop::Checkbox("Hit triggers", &m_physicsRaycastHitTriggers);
    ImGui::TextDisabled("Layers");
    layerMaskEditor("raycast_layers", m_physicsRaycastLayerMask);
    if (UI::IconButton(ICON_FA_LOCATION_CROSSHAIRS, "Raycast"))
    {
        m_commands.physicsRaycastFromCamera = true;
        m_commands.physicsRaycastLayerMask = m_physicsRaycastLayerMask;
        m_commands.physicsRaycastHitTriggers = m_physicsRaycastHitTriggers;
        m_commands.physicsRaycastDistance = m_physicsRaycastDistance;
    }

    UI::SectionHeader("Overlap in front of the camera");
    UI::Prop::DragFloat("Distance##overlap", &m_physicsOverlapDistance, 1.0f, 0.0f, 5000.0f, "%.1f m");
    UI::Prop::DragFloat("Sphere radius", &m_physicsOverlapRadius, 0.1f, 0.05f, 100.0f, "%.2f m");
    UI::Prop::DragFloat3("Box half extents", m_physicsOverlapBoxHalfExtents, 0.1f, 0.05f, 100.0f, "%.2f m");
    UI::Prop::DragFloat("Capsule radius", &m_physicsOverlapCapsuleRadius, 0.05f, 0.05f, 100.0f, "%.2f m");
    UI::Prop::DragFloat("Capsule height", &m_physicsOverlapCapsuleHeight, 0.1f, 0.1f, 200.0f, "%.2f m");
    m_physicsOverlapDistance = std::clamp(m_physicsOverlapDistance, 0.0f, 5000.0f);
    m_physicsOverlapRadius = std::clamp(m_physicsOverlapRadius, 0.05f, 100.0f);
    for (float& extent : m_physicsOverlapBoxHalfExtents)
        extent = std::clamp(extent, 0.05f, 100.0f);
    m_physicsOverlapCapsuleRadius = std::clamp(m_physicsOverlapCapsuleRadius, 0.05f, 100.0f);
    m_physicsOverlapCapsuleHeight = std::clamp(m_physicsOverlapCapsuleHeight, m_physicsOverlapCapsuleRadius * 2.0f, 200.0f);
    UI::Prop::Checkbox("Hit triggers##overlap", &m_physicsOverlapHitTriggers);
    ImGui::TextDisabled("Layers");
    layerMaskEditor("overlap_layers", m_physicsOverlapLayerMask);
    if (ImGui::Button("Sphere"))
    {
        m_commands.physicsOverlapSphereFromCamera = true;
        m_commands.physicsOverlapLayerMask = m_physicsOverlapLayerMask;
        m_commands.physicsOverlapHitTriggers = m_physicsOverlapHitTriggers;
        m_commands.physicsOverlapDistance = m_physicsOverlapDistance;
        m_commands.physicsOverlapRadius = m_physicsOverlapRadius;
    }
    ImGui::SameLine();
    if (ImGui::Button("Box"))
    {
        m_commands.physicsOverlapBoxFromCamera = true;
        m_commands.physicsOverlapLayerMask = m_physicsOverlapLayerMask;
        m_commands.physicsOverlapHitTriggers = m_physicsOverlapHitTriggers;
        m_commands.physicsOverlapDistance = m_physicsOverlapDistance;
        std::copy(std::begin(m_physicsOverlapBoxHalfExtents), std::end(m_physicsOverlapBoxHalfExtents), std::begin(m_commands.physicsOverlapBoxHalfExtents));
    }
    ImGui::SameLine();
    if (ImGui::Button("Capsule"))
    {
        m_commands.physicsOverlapCapsuleFromCamera = true;
        m_commands.physicsOverlapLayerMask = m_physicsOverlapLayerMask;
        m_commands.physicsOverlapHitTriggers = m_physicsOverlapHitTriggers;
        m_commands.physicsOverlapDistance = m_physicsOverlapDistance;
        m_commands.physicsOverlapCapsuleRadius = m_physicsOverlapCapsuleRadius;
        m_commands.physicsOverlapCapsuleHeight = m_physicsOverlapCapsuleHeight;
    }
    ImGui::TextDisabled("The results are written to the log.");

    UI::SectionHeader("Collision and trigger events");
    if (m_physicsEvents.empty())
    {
        ImGui::TextDisabled("No events yet: they appear while Play runs and bodies touch.");
    }
    else if (ImGui::BeginTable("PhysicsEventsTable", 5,
                 ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                 ImVec2(0.0f, 200.0f)))
    {
        ImGui::TableSetupColumn("Event");
        ImGui::TableSetupColumn("A");
        ImGui::TableSetupColumn("B");
        ImGui::TableSetupColumn("Point");
        ImGui::TableSetupColumn("Depth");
        ImGui::TableHeadersRow();
        const std::size_t maxRows = std::min<std::size_t>(m_physicsEvents.size(), 48u);
        for (std::size_t row = 0; row < maxRows; ++row)
        {
            const PhysicsEventEditorState& event = m_physicsEvents[m_physicsEvents.size() - 1u - row];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s %s", event.kind.c_str(), event.phase.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("#%u %s", event.entityA, event.nameA.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("#%u %s", event.entityB, event.nameB.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.2f, %.2f, %.2f", event.point[0], event.point[1], event.point[2]);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.3f", event.penetrationDepth);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void EditorImGui::RenderProjectSettingsWindow()
{
    if (!m_projectSettingsOpen)
        return;
    SetNextUtilityWindowPlacement(ImVec2(620.0f, 560.0f));
    if (!ImGui::Begin(EditorWindow::ProjectSettings, &m_projectSettingsOpen))
    {
        ImGui::End();
        return;
    }
    if (!ProjectManager::Instance().HasProject())
        ImGui::TextColored(UI::Theme::Warning, "No project is open: the physics layers are not saved.");

    if (ImGui::CollapsingHeader(ICON_FA_CUBES "  Physics layers", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Which layers collide with each other. Tick a box to let the two layers collide.");
        constexpr std::size_t kLayerCount = static_cast<std::size_t>(ixtreeme::physics::PhysicsLayer::Count);
        bool matrixChanged = false;
        const char* shortNames[] = {"Def", "World", "Dyn", "Player", "Trig", "Proj", "Foliage", "None"};
        if (ImGui::BeginTable("PhysicsLayerMatrix", static_cast<int>(kLayerCount) + 1,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("");
            for (std::size_t i = 0; i < kLayerCount; ++i)
                ImGui::TableSetupColumn(shortNames[i]);
            ImGui::TableHeadersRow();
            for (std::size_t row = 0; row < kLayerCount; ++row)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(ixtreeme::physics::DisplayName(static_cast<ixtreeme::physics::PhysicsLayer>(row)));
                for (std::size_t col = 0; col < kLayerCount; ++col)
                {
                    ImGui::TableSetColumnIndex(static_cast<int>(col) + 1);
                    ImGui::PushID(static_cast<int>(row * kLayerCount + col));
                    bool value = m_physicsLayerMatrix[row][col];
                    if (ImGui::Checkbox("##collides", &value))
                    {
                        m_physicsLayerMatrix[row][col] = value;
                        m_physicsLayerMatrix[col][row] = value;
                        matrixChanged = true;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Reset to defaults"))
        {
            for (std::size_t a = 0; a < kLayerCount; ++a)
            {
                for (std::size_t b = 0; b < kLayerCount; ++b)
                {
                    m_physicsLayerMatrix[a][b] = ixtreeme::physics::DefaultLayerCollision(
                        static_cast<ixtreeme::physics::PhysicsLayer>(a),
                        static_cast<ixtreeme::physics::PhysicsLayer>(b));
                }
            }
            matrixChanged = true;
        }
        if (matrixChanged)
        {
            m_commands.physicsLayerMatrixChanged = true;
            m_commands.physicsLayerMatrix = m_physicsLayerMatrix;
            StoreProjectPhysicsSettings();
            Tracen("[PHYSICS-LAYER] matrix edited");
        }
    }

    if (ImGui::CollapsingHeader(ICON_FA_VOLUME_HIGH "  Audio", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool volumeChanged = false;
        volumeChanged |= UI::Prop::SliderFloat("Master volume", &m_audioVolume[0], 0.0f, 1.0f, "%.2f");
        volumeChanged |= UI::Prop::SliderFloat("Music volume", &m_audioVolume[1], 0.0f, 1.0f, "%.2f");
        volumeChanged |= UI::Prop::SliderFloat("Effects volume", &m_audioVolume[2], 0.0f, 1.0f, "%.2f");
        if (volumeChanged)
        {
            m_commands.audioVolumesChanged = true;
            m_commands.audioVolume[0] = m_audioVolume[0];
            m_commands.audioVolume[1] = m_audioVolume[1];
            m_commands.audioVolume[2] = m_audioVolume[2];
        }
    }

    if (ImGui::CollapsingHeader(ICON_FA_IMAGE "  Rendering", ImGuiTreeNodeFlags_DefaultOpen))
    {
        auto queueRenderResolution = [&](bool native, std::uint32_t width, std::uint32_t height) {
            m_commands.renderResolutionChanged = true;
            m_commands.renderResolutionUseNative = native;
            m_commands.renderResolutionWidth = width;
            m_commands.renderResolutionHeight = height;
            Tracenf("[RENDER-RES] request mode=%s size=%ux%u", native ? "native" : "fixed", width, height);
        };
        const char* modes[] = {"View size", "3840 x 2160", "2560 x 1440", "1920 x 1080", "1600 x 900", "1280 x 720", "Custom"};
        const bool resolutionChanged = UI::Prop::Combo("Render resolution", &m_renderResolutionMode, modes, IM_ARRAYSIZE(modes));
        UI::ItemTooltip("View size: the Scene View and Game panels render at their own size on screen.\n"
                        "A fixed size renders that many pixels and scales the image into the panel.");
        if (resolutionChanged)
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
            UI::Prop::InputInt("Width", &m_customRenderResolutionWidth, 16, 128);
            UI::Prop::InputInt("Height", &m_customRenderResolutionHeight, 16, 128);
            m_customRenderResolutionWidth = std::clamp(m_customRenderResolutionWidth, 320, 7680);
            m_customRenderResolutionHeight = std::clamp(m_customRenderResolutionHeight, 180, 4320);
            if (UI::IconButton(ICON_FA_CHECK, "Apply Resolution"))
            {
                queueRenderResolution(false,
                    static_cast<std::uint32_t>(m_customRenderResolutionWidth),
                    static_cast<std::uint32_t>(m_customRenderResolutionHeight));
            }
        }
    }
    ImGui::End();
}

void EditorImGui::RenderLayeredWorldWindow()
{
    if (!m_layeredWorldOpen)
        return;
    SetNextUtilityWindowPlacement(ImVec2(520.0f, 460.0f));
    if (!ImGui::Begin(EditorWindow::LayeredWorld, &m_layeredWorldOpen))
    {
        ImGui::End();
        return;
    }
    const bool editing = CanUseEditorTools();
    if (!editing)
        ImGui::TextColored(UI::Theme::Warning, "Stop Play to work on the layered world.");

    UI::SectionHeader("Layers");
    ImGui::TextWrapped("Walkable layers (floors, bridges, caves) are generated from the scene's collision.");
    if (!editing)
        ImGui::BeginDisabled();
    if (UI::IconButton(ICON_FA_LAYER_GROUP, "Generate layers from collision"))
        m_commands.generateLayers = true;
    ImGui::SameLine();
    if (ImGui::Button("Export layer metadata"))
        m_commands.exportLayers = true;
    if (!editing)
        ImGui::EndDisabled();
    UI::Prop::Checkbox("Show layer volumes", &m_showLayerVolumes);

    UI::SectionHeader("Server world export");
    UI::Prop::InputText("World ID", m_serverWorldId, sizeof(m_serverWorldId));
    UI::Prop::InputFloat2("Player spawn X/Z (m)", m_serverWorldSpawn);
    UI::Prop::InputScalar("Spawn volume", ImGuiDataType_U32, &m_serverWorldSpawnVolume);
    ImGui::TextDisabled("Volume 0 spawns on the terrain, any other on that generated layer volume\n"
                        "(the baked capsule must fit there; blocked or outside positions are rejected).");
    if (!editing)
        ImGui::BeginDisabled();
    if (UI::IconButton(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE, "Export strict server world"))
    {
        m_commands.exportServerWorld = true;
        m_commands.serverWorldId = m_serverWorldId;
        m_commands.serverWorldSpawnX = m_serverWorldSpawn[0];
        m_commands.serverWorldSpawnZ = m_serverWorldSpawn[1];
        m_commands.serverWorldSpawnVolume = m_serverWorldSpawnVolume;
    }
    if (!editing)
        ImGui::EndDisabled();

    UI::SectionHeader("Support probe (offline)");
    UI::Prop::InputScalar("Support volume", ImGuiDataType_U32, &m_layerGroundVolume);
    UI::Prop::InputFloat2("Support point X/Z (m)", m_layerGroundPoint);
    if (!editing)
        ImGui::BeginDisabled();
    if (ImGui::Button("Place support probe"))
        m_commands.placeLayerGround = true;
    ImGui::SameLine();
    if (ImGui::Button("Move support probe"))
        m_commands.moveLayerGround = true;
    if (!editing)
        ImGui::EndDisabled();
    if (m_commands.placeLayerGround || m_commands.moveLayerGround)
    {
        m_commands.layerGroundVolume = m_layerGroundVolume;
        m_commands.layerGroundX = m_layerGroundPoint[0];
        m_commands.layerGroundZ = m_layerGroundPoint[1];
    }
    if (!m_layerAuthoringStatus.empty())
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_layerAuthoringStatus.c_str());
    }
    ImGui::End();
}

void EditorImGui::RenderShortcutsWindow()
{
    if (!m_shortcutsWindowOpen)
        return;
    SetNextUtilityWindowPlacement(ImVec2(440.0f, 520.0f));
    if (!ImGui::Begin(EditorWindow::Shortcuts, &m_shortcutsWindowOpen))
    {
        ImGui::End();
        return;
    }
    struct Shortcut
    {
        const char* keys;
        const char* action;
    };
    const auto section = [](const char* title, std::initializer_list<Shortcut> shortcuts) {
        UI::SectionHeader(title);
        if (ImGui::BeginTable(title, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            for (const Shortcut& shortcut : shortcuts)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(UI::Theme::AccentHovered, "%s", shortcut.keys);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(shortcut.action);
            }
            ImGui::EndTable();
        }
    };
    section("Files", {
        {"Ctrl+S", "Save the project and the open scene"},
        {"Ctrl+Shift+S", "Save the scene under a new name"},
        {"Ctrl+N", "New scene"},
    });
    section("Play", {
        {"F5", "Play / Stop"},
        {"Shift+F5", "Stop"},
        {"F6", "Pause / Resume"},
    });
    section("Scene View", {
        {"Right mouse + W A S D", "Fly the camera"},
        {"W / E / R", "Move / Rotate / Scale tool"},
        {"F", "Frame the selection"},
        {"Delete", "Delete the selection"},
    });
    section("Debug", {
        {"F11", "Capture a GPU frame"},
        {"F12", "Dump material bindings to the log"},
    });
    ImGui::End();
}

void EditorImGui::RenderAboutPopup()
{
    if (m_openAboutPopup)
    {
        ImGui::OpenPopup("About IxtreemeEngine");
        m_openAboutPopup = false;
    }
    if (!ImGui::BeginPopupModal("About IxtreemeEngine", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (UI::GetEditorFonts().bold)
        ImGui::PushFont(UI::GetEditorFonts().bold);
    ImGui::TextUnformatted("IxtreemeEngine Editor");
    if (UI::GetEditorFonts().bold)
        ImGui::PopFont();
    ImGui::TextDisabled("UI: Dear ImGui %s", IMGUI_VERSION);
    ImGui::Separator();
    ImGui::TextUnformatted("Help > Keyboard Shortcuts lists the editor's keys.");
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void EditorImGui::ResetEditorLayout()
{
    m_hierarchyPanelOpen = true;
    m_inspectorPanelOpen = true;
    m_sceneSettingsPanelOpen = true;
    m_assetBrowserPanelOpen = true;
    m_scriptsPanelOpen = true;
    m_consolePanelOpen = true;
    m_animatorPanelOpen = true;
    m_applyDefaultDockLayout = true;
    m_defaultDockLayoutBuilt = false;
    Tracen("[EDITOR-LAYOUT] Layout reset requested");
}
