#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ProjectData
{
    std::string name;
    std::string engineVersion = "0.1";
    std::string createdAt;
    std::string modifiedAt;
    std::string assetRoot = "Assets";
    std::string scenesDir = "Scenes";
    std::string startupScene;
    std::vector<std::string> recentScenes;
    std::filesystem::path manifestPath;
    std::filesystem::path rootPath;
};

class ProjectManager
{
public:
    static ProjectManager& Instance();

    bool HasProject() const { return m_hasProject; }
    const ProjectData& CurrentProject() const { return m_project; }
    const std::vector<std::filesystem::path>& RecentProjects() const { return m_recentProjects; }

    bool CreateProject(const std::filesystem::path& parentDirectory,
                       const std::string& projectName,
                       std::string& error);
    bool OpenProject(const std::filesystem::path& manifestPath, std::string& error);
    bool SaveProject(std::string& error);

    void SetRecentScenes(const std::vector<std::string>& recentScenes);
    std::filesystem::path ProjectRoot() const;
    std::filesystem::path AssetRootPath() const;
    std::filesystem::path ScenesPath() const;
    std::filesystem::path ManifestPath() const;

private:
    ProjectManager() = default;

    void AddRecentProject(const std::filesystem::path& manifestPath);
    bool WriteManifest(std::string& error);
    bool ReadManifest(const std::filesystem::path& manifestPath, std::string& error);

    ProjectData m_project;
    bool m_hasProject = false;
    std::vector<std::filesystem::path> m_recentProjects;
};
