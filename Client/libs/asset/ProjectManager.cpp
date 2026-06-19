#include "ProjectManager.h"

#include "Common.h"
#include "Debug.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>

namespace
{
using ixtreeme::common::EscapeJson;
using ixtreeme::common::JsonStringValue;
using ixtreeme::common::TimestampUtc;

bool JsonArrayBody(const std::string& text, const std::string& key, std::string& out)
{
    const std::string needle = "\"" + key + "\"";
    const size_t keyPos = text.find(needle);
    if (keyPos == std::string::npos)
        return false;
    const size_t arrayBegin = text.find('[', keyPos + needle.size());
    if (arrayBegin == std::string::npos)
        return false;

    int depth = 0;
    bool inString = false;
    bool escaping = false;
    for (size_t i = arrayBegin; i < text.size(); ++i)
    {
        const char c = text[i];
        if (inString)
        {
            if (escaping)
                escaping = false;
            else if (c == '\\')
                escaping = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
        {
            inString = true;
            continue;
        }
        if (c == '[')
            ++depth;
        else if (c == ']')
        {
            --depth;
            if (depth == 0)
            {
                out = text.substr(arrayBegin + 1, i - arrayBegin - 1);
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> JsonStringArrayValue(const std::string& object, const std::string& key)
{
    std::vector<std::string> values;
    std::string body;
    if (!JsonArrayBody(object, key, body))
        return values;

    bool inString = false;
    bool escaping = false;
    std::string value;
    for (char c : body)
    {
        if (!inString)
        {
            if (c == '"')
            {
                inString = true;
                value.clear();
            }
            continue;
        }
        if (escaping)
        {
            value += c;
            escaping = false;
            continue;
        }
        if (c == '\\')
        {
            escaping = true;
            continue;
        }
        if (c == '"')
        {
            values.push_back(value);
            inString = false;
            continue;
        }
        value += c;
    }
    return values;
}

bool IsValidProjectName(const std::string& name)
{
    if (name.empty())
        return false;
    constexpr const char* invalid = "<>:\"/\\|?*";
    return name.find_first_of(invalid) == std::string::npos;
}
}

ProjectManager& ProjectManager::Instance()
{
    static ProjectManager instance;
    return instance;
}

bool ProjectManager::CreateProject(const std::filesystem::path& parentDirectory,
                                   const std::string& projectName,
                                   std::string& error)
{
    if (!IsValidProjectName(projectName))
    {
        error = "invalid project name";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::absolute(parentDirectory / projectName, ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }
    if (std::filesystem::exists(root / "project.ixproj"))
    {
        error = "project.ixproj already exists";
        return false;
    }

    std::filesystem::create_directories(root / "Assets", ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }
    std::filesystem::create_directories(root / "Scenes", ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }

    m_project = {};
    m_project.name = projectName;
    m_project.createdAt = TimestampUtc();
    m_project.modifiedAt = m_project.createdAt;
    m_project.rootPath = root;
    m_project.manifestPath = root / "project.ixproj";
    m_hasProject = true;

    if (!WriteManifest(error))
    {
        m_hasProject = false;
        return false;
    }

    AddRecentProject(m_project.manifestPath);
    Tracenf("[PROJECT] create OK: %s", m_project.manifestPath.generic_string().c_str());
    return true;
}

bool ProjectManager::OpenProject(const std::filesystem::path& manifestPath, std::string& error)
{
    if (!ReadManifest(manifestPath, error))
        return false;

    std::error_code ec;
    std::filesystem::create_directories(AssetRootPath(), ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }
    std::filesystem::create_directories(ScenesPath(), ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }

    m_hasProject = true;
    AddRecentProject(m_project.manifestPath);
    Tracenf("[PROJECT] open OK: %s", m_project.manifestPath.generic_string().c_str());
    return true;
}

bool ProjectManager::SaveProject(std::string& error)
{
    if (!m_hasProject)
    {
        error = "no project is active";
        return false;
    }
    m_project.modifiedAt = TimestampUtc();
    const bool ok = WriteManifest(error);
    if (ok)
        Tracenf("[PROJECT] save OK: %s", m_project.manifestPath.generic_string().c_str());
    return ok;
}

void ProjectManager::SetRecentScenes(const std::vector<std::string>& recentScenes)
{
    if (!m_hasProject)
        return;
    m_project.recentScenes = recentScenes;
    if (!m_project.recentScenes.empty())
        m_project.startupScene = m_project.recentScenes.front();
    std::string ignored;
    SaveProject(ignored);
}

std::filesystem::path ProjectManager::ProjectRoot() const
{
    return m_project.rootPath;
}

std::filesystem::path ProjectManager::AssetRootPath() const
{
    return m_project.rootPath / m_project.assetRoot;
}

std::filesystem::path ProjectManager::ScenesPath() const
{
    return m_project.rootPath / m_project.scenesDir;
}

std::filesystem::path ProjectManager::ManifestPath() const
{
    return m_project.manifestPath;
}

void ProjectManager::AddRecentProject(const std::filesystem::path& manifestPath)
{
    std::error_code ec;
    const std::filesystem::path absolutePath = std::filesystem::absolute(manifestPath, ec);
    const std::filesystem::path normalized = ec ? manifestPath : absolutePath;
    m_recentProjects.erase(std::remove(m_recentProjects.begin(), m_recentProjects.end(), normalized), m_recentProjects.end());
    m_recentProjects.insert(m_recentProjects.begin(), normalized);
    if (m_recentProjects.size() > 8)
        m_recentProjects.resize(8);
}

bool ProjectManager::WriteManifest(std::string& error)
{
    if (m_project.manifestPath.empty())
    {
        error = "missing project manifest path";
        return false;
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"version\": 1,\n";
    json << "  \"name\": \"" << EscapeJson(m_project.name) << "\",\n";
    json << "  \"engine_version\": \"" << EscapeJson(m_project.engineVersion) << "\",\n";
    json << "  \"created_at\": \"" << EscapeJson(m_project.createdAt) << "\",\n";
    json << "  \"modified_at\": \"" << EscapeJson(m_project.modifiedAt) << "\",\n";
    json << "  \"asset_root\": \"" << EscapeJson(m_project.assetRoot) << "\",\n";
    json << "  \"scenes_dir\": \"" << EscapeJson(m_project.scenesDir) << "\",\n";
    json << "  \"startup_scene\": \"" << EscapeJson(m_project.startupScene) << "\",\n";
    json << "  \"recent_scenes\": [";
    for (size_t i = 0; i < m_project.recentScenes.size(); ++i)
    {
        if (i > 0)
            json << ", ";
        json << "\"" << EscapeJson(m_project.recentScenes[i]) << "\"";
    }
    json << "]\n";
    json << "}\n";

    std::error_code ec;
    std::filesystem::create_directories(m_project.manifestPath.parent_path(), ec);
    if (ec)
    {
        error = ec.message();
        return false;
    }

    std::ofstream file(m_project.manifestPath, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        error = "failed to open project manifest";
        return false;
    }
    file << json.str();
    if (!file)
    {
        error = "failed to write project manifest";
        return false;
    }
    return true;
}

bool ProjectManager::ReadManifest(const std::filesystem::path& manifestPath, std::string& error)
{
    std::error_code ec;
    const std::filesystem::path absolutePath = std::filesystem::absolute(manifestPath, ec);
    const std::filesystem::path resolvedPath = ec ? manifestPath : absolutePath;
    std::ifstream file(resolvedPath, std::ios::binary);
    if (!file)
    {
        error = "project.ixproj not found";
        return false;
    }

    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ProjectData loaded;
    loaded.manifestPath = resolvedPath;
    loaded.rootPath = resolvedPath.parent_path();
    loaded.name = JsonStringValue(text, "name");
    loaded.engineVersion = JsonStringValue(text, "engine_version");
    loaded.createdAt = JsonStringValue(text, "created_at");
    loaded.modifiedAt = JsonStringValue(text, "modified_at");
    loaded.assetRoot = JsonStringValue(text, "asset_root");
    loaded.scenesDir = JsonStringValue(text, "scenes_dir");
    loaded.startupScene = JsonStringValue(text, "startup_scene");
    loaded.recentScenes = JsonStringArrayValue(text, "recent_scenes");

    if (loaded.name.empty())
        loaded.name = loaded.rootPath.filename().string();
    if (loaded.engineVersion.empty())
        loaded.engineVersion = "0.1";
    if (loaded.createdAt.empty())
        loaded.createdAt = TimestampUtc();
    if (loaded.modifiedAt.empty())
        loaded.modifiedAt = loaded.createdAt;
    if (loaded.assetRoot.empty())
        loaded.assetRoot = "Assets";
    if (loaded.scenesDir.empty())
        loaded.scenesDir = "Scenes";

    m_project = std::move(loaded);
    return true;
}
