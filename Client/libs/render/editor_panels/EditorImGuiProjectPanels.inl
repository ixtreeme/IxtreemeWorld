// This file is included from EditorImGui.cpp inside the editor-enabled implementation block.
// Keep shared anonymous-namespace helpers in EditorImGui.cpp until this panel group is fully decoupled.

void EditorImGui::RenderProjectBrowser(bool pickProjectFile)
{
    if (m_projectBrowserPath.empty())
        NavigateProjectBrowser(InitialProjectBrowserPath(m_engineRoot));

    const std::string currentPath = m_projectBrowserPath.string();
    ImGui::TextUnformatted("Browse Path");
    ImGui::SetNextItemWidth(-56.0f);
    if (ImGui::InputText("##ProjectBrowsePath",
            m_projectBrowsePathBuffer,
            sizeof(m_projectBrowsePathBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue))
    {
        if (NavigateProjectBrowser(m_projectBrowsePathBuffer, !pickProjectFile) && !pickProjectFile)
            CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
    }
    ImGui::SameLine();
    if (ImGui::Button("Go", ImVec2(44.0f, 0.0f)))
    {
        if (NavigateProjectBrowser(m_projectBrowsePathBuffer, !pickProjectFile) && !pickProjectFile)
            CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
    }

#if defined(_WIN32)
    ImGui::TextUnformatted("Drives");
    bool firstDrive = true;
    for (char drive = 'A'; drive <= 'Z'; ++drive)
    {
        const std::string root = std::string(1, drive) + ":\\";
        std::error_code driveEc;
        if (!std::filesystem::exists(root, driveEc))
            continue;
        if (!firstDrive)
            ImGui::SameLine();
        firstDrive = false;
        if (ImGui::Button((std::string(1, drive) + ":").c_str()))
            NavigateProjectBrowser(root);
    }
#endif

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##ProjectBrowseFilter", "Filter folders/projects...", m_projectBrowseFilterBuffer, sizeof(m_projectBrowseFilterBuffer));
    ImGui::TextDisabled("%s", currentPath.c_str());
    if (ImGui::Button("Up"))
    {
        const std::filesystem::path parent = m_projectBrowserPath.parent_path();
        if (!parent.empty())
            NavigateProjectBrowser(parent);
    }
    ImGui::SameLine();
    if (!pickProjectFile && ImGui::Button("Use This Folder"))
    {
        CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
        m_projectCreateBrowserVisible = false;
    }

    ImGui::Separator();
    if (ImGui::BeginChild(pickProjectFile ? "OpenProjectBrowser" : "CreateProjectBrowser", ImVec2(0.0f, 220.0f), true))
    {
        std::vector<std::filesystem::directory_entry> directories;
        std::vector<std::filesystem::directory_entry> manifests;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(m_projectBrowserPath, ec))
        {
            if (entry.is_directory(ec))
                directories.push_back(entry);
            else if (pickProjectFile && entry.path().extension() == ".ixproj")
                manifests.push_back(entry);
        }
        std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
            return a.path().filename().string() < b.path().filename().string();
        });
        std::sort(manifests.begin(), manifests.end(), [](const auto& a, const auto& b) {
            return a.path().filename().string() < b.path().filename().string();
        });

        for (const auto& entry : directories)
        {
            const std::string filename = entry.path().filename().string();
            if (!ContainsCaseInsensitive(filename, m_projectBrowseFilterBuffer))
                continue;
            const std::string label = "[Folder] " + filename;
            if (ImGui::Selectable(label.c_str()))
            NavigateProjectBrowser(entry.path());
        }
        if (pickProjectFile)
        {
            for (const auto& entry : manifests)
            {
                const std::string filename = entry.path().filename().string();
                if (!ContainsCaseInsensitive(filename, m_projectBrowseFilterBuffer))
                    continue;
                const std::string label = "[Project] " + filename;
                if (ImGui::Selectable(label.c_str()))
                    CopyToBuffer(m_projectOpenPathBuffer, sizeof(m_projectOpenPathBuffer), entry.path().string());
            }
        }
    }
    ImGui::EndChild();
}

void EditorImGui::RenderProjectModal()
{
    ProjectManager& projects = ProjectManager::Instance();
    if (m_projectDialogMode == ProjectDialogMode::None)
        return;

    if (m_projectPopupNeedsOpen)
    {
        ImGui::OpenPopup("Project");
        m_projectPopupNeedsOpen = false;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Project", nullptr, ImGuiWindowFlags_NoCollapse))
        return;

    if (m_projectDialogMode == ProjectDialogMode::NoProject)
    {
        ImGui::TextUnformatted("No Project");
        ImGui::TextDisabled("Create or open a project to bind the Asset Browser and scene workspace.");
        ImGui::Separator();
        if (ImGui::Button("Create New Project", ImVec2(180.0f, 0.0f)))
            m_projectDialogMode = ProjectDialogMode::Create;
        ImGui::SameLine();
        if (ImGui::Button("Open Project", ImVec2(180.0f, 0.0f)))
            m_projectDialogMode = ProjectDialogMode::Open;

        ImGui::Separator();
        ImGui::TextUnformatted("Recent Projects");
        if (projects.RecentProjects().empty())
        {
            ImGui::TextDisabled("No recent projects.");
        }
        else
        {
            for (const auto& path : projects.RecentProjects())
            {
                const std::string label = path.parent_path().filename().string() + "##" + path.string();
                if (ImGui::Selectable(label.c_str()))
                    OpenProjectFromDialog(path);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", path.string().c_str());
            }
        }
    }
    else if (m_projectDialogMode == ProjectDialogMode::Create)
    {
        ImGui::TextUnformatted("Create New Project");
        ImGui::TextUnformatted("Parent Folder");
        const float browseButtonWidth = 96.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - browseButtonWidth - spacing));
        ImGui::InputText("##ProjectParentFolder", m_projectParentBuffer, sizeof(m_projectParentBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(96.0f, 0.0f)))
        {
            m_projectCreateBrowserVisible = !m_projectCreateBrowserVisible;
            if (NavigateProjectBrowser(m_projectParentBuffer, true))
                CopyToBuffer(m_projectParentBuffer, sizeof(m_projectParentBuffer), m_projectBrowserPath.string());
        }
        ImGui::TextUnformatted("Project Name");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##ProjectName", m_projectNameBuffer, sizeof(m_projectNameBuffer));

        const std::filesystem::path targetProjectPath = std::filesystem::path(m_projectParentBuffer) / m_projectNameBuffer;
        ImGui::TextDisabled("Target: %s", targetProjectPath.string().c_str());
        if (ImGui::Button("Create Project", ImVec2(150.0f, 0.0f)))
            CreateProjectFromDialog();
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(120.0f, 0.0f)))
            m_projectDialogMode = projects.HasProject() ? ProjectDialogMode::None : ProjectDialogMode::NoProject;

        if (m_projectCreateBrowserVisible)
            RenderProjectBrowser(false);
    }
    else if (m_projectDialogMode == ProjectDialogMode::Open)
    {
        ImGui::TextUnformatted("Open Project");
        ImGui::InputText("project.ixproj", m_projectOpenPathBuffer, sizeof(m_projectOpenPathBuffer));
        RenderProjectBrowser(true);
        ImGui::Separator();
        if (ImGui::Button("Open", ImVec2(120.0f, 0.0f)))
            OpenProjectFromDialog(m_projectOpenPathBuffer);
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(120.0f, 0.0f)))
            m_projectDialogMode = projects.HasProject() ? ProjectDialogMode::None : ProjectDialogMode::NoProject;
    }

    if (!m_projectStatus.empty())
    {
        ImGui::Separator();
        ImGui::TextDisabled("%s", m_projectStatus.c_str());
    }

    ImGui::EndPopup();
}

