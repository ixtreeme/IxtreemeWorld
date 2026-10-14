// Editor/Build — "Build Game" packager. See GamePackager.h for the package layout and contract.

#include "GamePackager.h"

#include "platform/process.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>
#include <utility>
#include <vector>

namespace ixeditor::build
{
namespace
{
#if defined(_WIN32)
constexpr const char* kRuntimeExecutable = "IxtreemeEngine.exe";
constexpr const char* kExecutableExtension = ".exe";
constexpr const char* kLibraryExtension = ".dll";
#else
constexpr const char* kRuntimeExecutable = "IxtreemeEngine";
constexpr const char* kExecutableExtension = "";
constexpr const char* kLibraryExtension = ".so";
#endif
constexpr const char* kGameFolder = "Game";

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// Lower-case generic text of a path, for prefix tests between paths spelled differently.
std::string PathKey(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path normal = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        normal = path;
    std::string key = Lower(normal.lexically_normal().generic_string());
    while (key.size() > 1 && key.back() == '/')
        key.pop_back();
    return key;
}

bool IsSameOrInside(const std::filesystem::path& path, const std::filesystem::path& folder)
{
    const std::string pathKey = PathKey(path);
    const std::string folderKey = PathKey(folder);
    return pathKey == folderKey || pathKey.rfind(folderKey + "/", 0) == 0;
}

// "KEY:TYPE=value" from a CMakeCache.txt.
std::string CacheValue(const std::filesystem::path& cacheFile, const std::string& key)
{
    std::ifstream cache(cacheFile);
    std::string line;
    while (std::getline(cache, line))
    {
        if (line.rfind(key + ":", 0) != 0)
            continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            return {};
        std::string value = line.substr(equals + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.pop_back();
        return value;
    }
    return {};
}

// The interesting part of a long tool output: its error lines plus the last lines.
std::string SummarizeOutput(const std::string& output, size_t tailLines = 40)
{
    std::vector<std::string> lines;
    std::istringstream stream(output);
    for (std::string line; std::getline(stream, line);)
    {
        while (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
    }
    std::string summary;
    const size_t tailStart = lines.size() > tailLines ? lines.size() - tailLines : 0;
    for (size_t i = 0; i < tailStart; ++i)
    {
        const std::string lower = Lower(lines[i]);
        if (lower.find(" error ") != std::string::npos || lower.find("error:") != std::string::npos ||
            lower.find("cmake error") != std::string::npos)
            summary += lines[i] + "\n";
    }
    if (tailStart > 0)
        summary += "  ...\n";
    for (size_t i = tailStart; i < lines.size(); ++i)
        summary += lines[i] + "\n";
    return summary;
}

bool RunLogged(const std::vector<std::string>& argv, std::string& log)
{
    std::string command;
    for (const std::string& arg : argv)
        command += (command.empty() ? "" : " ") + (arg.find(' ') != std::string::npos ? "\"" + arg + "\"" : arg);
    log += "> " + command + "\n";
    const platform::ProcessResult result = platform::RunProcess(argv);
    if (!result.launched)
    {
        log += "[BUILD-GAME] could not start " + argv.front() + " (is it installed and on PATH?)\n";
        return false;
    }
    log += SummarizeOutput(result.output);
    return result.exitCode == 0;
}

// The editor-less runtime player: prebuilt next to the editor, else built from the engine sources.
bool ResolveRuntime(const GamePackageRequest& request, std::filesystem::path& executable, std::string& log)
{
    std::error_code ec;
    if (!request.prebuiltRuntimeDir.empty())
    {
        const std::filesystem::path prebuilt = request.prebuiltRuntimeDir / kRuntimeExecutable;
        if (std::filesystem::exists(prebuilt, ec))
        {
            executable = prebuilt;
            log += "[BUILD-GAME] runtime player: " + prebuilt.generic_string() + "\n";
            return true;
        }
    }
    if (request.engineSourceDir.empty() || !std::filesystem::exists(request.engineSourceDir / "CMakeLists.txt", ec))
    {
        log += "[BUILD-GAME] no runtime player: neither a prebuilt one (" +
            (request.prebuiltRuntimeDir / kRuntimeExecutable).generic_string() +
            ") nor the engine sources to build it from were found.\n";
        return false;
    }

    // Built from the engine sources (kept up to date: an unchanged engine builds in seconds).
    const std::filesystem::path buildDir = request.engineSourceDir / "build-runtime";
    if (!std::filesystem::exists(buildDir / "CMakeCache.txt", ec))
    {
        log += "[BUILD-GAME] configuring the runtime player build (first time only)...\n";
        std::vector<std::string> configure = {
            "cmake", "-S", request.engineSourceDir.string(), "-B", buildDir.string(), "-DIXTREEME_WITH_EDITOR=OFF"};
        // Same generator and vcpkg setup as the editor's own build.
        const std::filesystem::path editorCache = request.editorBuildDir / "CMakeCache.txt";
        if (const std::string generator = CacheValue(editorCache, "CMAKE_GENERATOR"); !generator.empty())
            configure.insert(configure.end(), {"-G", generator});
        if (const std::string platformName = CacheValue(editorCache, "CMAKE_GENERATOR_PLATFORM"); !platformName.empty())
            configure.insert(configure.end(), {"-A", platformName});
        if (const std::string toolchain = CacheValue(editorCache, "CMAKE_TOOLCHAIN_FILE"); !toolchain.empty())
            configure.push_back("-DCMAKE_TOOLCHAIN_FILE=" + toolchain);
        if (const std::string triplet = CacheValue(editorCache, "VCPKG_TARGET_TRIPLET"); !triplet.empty())
            configure.push_back("-DVCPKG_TARGET_TRIPLET=" + triplet);
        if (!RunLogged(configure, log))
        {
            log += "[BUILD-GAME] configuring the runtime player failed.\n";
            return false;
        }
    }
    log += "[BUILD-GAME] building the runtime player (the first build takes several minutes)...\n";
    if (!RunLogged({"cmake", "--build", buildDir.string(), "--config", request.buildConfig,
                    "--target", "IxtreemeEngine", "--parallel"}, log))
    {
        log += "[BUILD-GAME] building the runtime player failed.\n";
        return false;
    }
    // Multi-config generators put it under the config folder, single-config ones don't.
    for (const std::filesystem::path& candidate : {
             buildDir / "apps" / "client" / request.buildConfig / kRuntimeExecutable,
             buildDir / "apps" / "client" / kRuntimeExecutable})
    {
        if (std::filesystem::exists(candidate, ec))
        {
            executable = candidate;
            log += "[BUILD-GAME] runtime player: " + candidate.generic_string() + "\n";
            return true;
        }
    }
    log += "[BUILD-GAME] the runtime player build finished but its executable was not found under " +
        buildDir.generic_string() + "\n";
    return false;
}

struct CopyStats
{
    std::uint64_t files = 0;
    std::uint64_t bytes = 0;
};

// Copies a folder tree; skip(relativePath, isDirectory) leaves out files and whole folders.
bool CopyTree(const std::filesystem::path& from,
              const std::filesystem::path& to,
              const std::function<bool(const std::filesystem::path&, bool)>& skip,
              CopyStats& stats,
              std::string& log)
{
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    if (ec)
    {
        log += "[BUILD-GAME] cannot create " + to.generic_string() + ": " + ec.message() + "\n";
        return false;
    }
    std::filesystem::recursive_directory_iterator it(from, std::filesystem::directory_options::skip_permission_denied, ec);
    for (const std::filesystem::recursive_directory_iterator end; !ec && it != end; it.increment(ec))
    {
        const std::filesystem::path relative = it->path().lexically_relative(from);
        std::error_code entryEc;
        const bool isDirectory = it->is_directory(entryEc);
        if (skip && skip(relative, isDirectory))
        {
            if (isDirectory)
                it.disable_recursion_pending();
            continue;
        }
        const std::filesystem::path target = to / relative;
        if (isDirectory)
        {
            std::filesystem::create_directories(target, entryEc);
        }
        else if (it->is_regular_file(entryEc))
        {
            std::filesystem::create_directories(target.parent_path(), entryEc);
            std::filesystem::copy_file(it->path(), target, std::filesystem::copy_options::overwrite_existing, entryEc);
            if (!entryEc)
            {
                ++stats.files;
                stats.bytes += it->file_size(entryEc);
            }
        }
        if (entryEc)
        {
            log += "[BUILD-GAME] copy failed: " + it->path().generic_string() + ": " + entryEc.message() + "\n";
            return false;
        }
    }
    if (ec)
    {
        log += "[BUILD-GAME] cannot read " + from.generic_string() + ": " + ec.message() + "\n";
        return false;
    }
    return true;
}

// Rewrites the "startup_scene" value of a project.ixproj.
bool SetStartupScene(const std::filesystem::path& manifest, const std::string& scene, std::string& log)
{
    std::string text;
    {
        std::ifstream in(manifest, std::ios::binary);
        if (!in)
        {
            log += "[BUILD-GAME] cannot read " + manifest.generic_string() + "\n";
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::string escaped;
    for (char c : scene)
    {
        if (c == '"' || c == '\\')
            escaped.push_back('\\');
        escaped.push_back(c == '\\' ? '/' : c);
    }
    const std::string key = "\"startup_scene\"";
    const size_t keyPos = text.find(key);
    const size_t colon = keyPos == std::string::npos ? std::string::npos : text.find(':', keyPos + key.size());
    const size_t open = colon == std::string::npos ? std::string::npos : text.find('"', colon + 1);
    size_t close = open == std::string::npos ? std::string::npos : open + 1;
    while (close != std::string::npos && close < text.size() && text[close] != '"')
        close += text[close] == '\\' ? 2 : 1;
    if (close == std::string::npos || close >= text.size())
    {
        log += "[BUILD-GAME] project.ixproj has no startup_scene to set\n";
        return false;
    }
    text.replace(open + 1, close - open - 1, escaped);
    std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

GamePackageResult Package(const GamePackageRequest& request)
{
    GamePackageResult result;
    std::string& log = result.log;
    std::error_code ec;
    const std::string gameName = SanitizeGameName(request.gameName);
    log += "[BUILD-GAME] '" + gameName + "' -> " + request.outputDir.generic_string() + "\n";

    if (request.projectRoot.empty() || !std::filesystem::exists(request.projectRoot / "project.ixproj", ec))
    {
        log += "[BUILD-GAME] no project.ixproj in " + request.projectRoot.generic_string() + "\n";
        return result;
    }
    if (request.outputDir.empty() || IsSameOrInside(request.projectRoot, request.outputDir))
    {
        log += "[BUILD-GAME] the output folder must not be (or contain) the project folder\n";
        return result;
    }

    std::filesystem::path runtime;
    if (!ResolveRuntime(request, runtime, log))
        return result;
    const std::filesystem::path engineAssets = runtime.parent_path() / "assets";
    if (!std::filesystem::exists(engineAssets / "shaders", ec))
    {
        log += "[BUILD-GAME] the runtime player has no assets/shaders next to it: " + engineAssets.generic_string() + "\n";
        return result;
    }

    // Replace a previous package; never touch a folder that is not one.
    if (std::filesystem::exists(request.outputDir, ec) && !std::filesystem::is_empty(request.outputDir, ec))
    {
        if (!std::filesystem::exists(request.outputDir / GamePackageService::kPackageMarker, ec))
        {
            log += "[BUILD-GAME] the output folder is not empty and is not a previous game build: " +
                request.outputDir.generic_string() + " (choose an empty or new folder)\n";
            return result;
        }
        std::filesystem::remove_all(request.outputDir, ec);
        if (ec)
        {
            log += "[BUILD-GAME] cannot clear the previous build (is the game still running?): " + ec.message() + "\n";
            return result;
        }
    }
    std::filesystem::create_directories(request.outputDir, ec);
    if (ec)
    {
        log += "[BUILD-GAME] cannot create the output folder: " + ec.message() + "\n";
        return result;
    }

    // 1. The runtime player as <GameName>.exe, with the libraries beside it (e.g. the Vulkan loader).
    const std::filesystem::path executable = request.outputDir / (gameName + kExecutableExtension);
    std::filesystem::copy_file(runtime, executable, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
    {
        log += "[BUILD-GAME] cannot copy the runtime player: " + ec.message() + "\n";
        return result;
    }
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(runtime.parent_path(), ec))
    {
        std::error_code entryEc;
        if (entry.is_regular_file(entryEc) && Lower(entry.path().extension().string()) == kLibraryExtension)
            std::filesystem::copy_file(entry.path(), request.outputDir / entry.path().filename(),
                std::filesystem::copy_options::overwrite_existing, entryEc);
    }

    // 2. The engine's runtime assets.
    CopyStats engineStats;
    if (!CopyTree(engineAssets, request.outputDir / "assets", {}, engineStats, log))
        return result;

    // 3. The project, without what only the editor and the build need.
    const std::filesystem::path gameRoot = request.outputDir / kGameFolder;
    const std::filesystem::path assetRootRelative = request.assetRoot.empty()
        ? std::filesystem::path("Assets")
        : request.assetRoot.lexically_relative(request.projectRoot);
    const std::string thumbnailsKey = Lower((assetRootRelative / "thumbnails").generic_string());
    std::vector<std::string> skipped;
    const auto skipProjectEntry = [&](const std::filesystem::path& relative, bool isDirectory) {
        const std::string relativeKey = Lower(relative.generic_string());
        const std::string name = Lower(relative.filename().generic_string());
        bool skip = false;
        if (name.empty() || name.front() == '.')
            skip = true;                                         // hidden (.git, .vs, ...)
        else if (isDirectory && (name == "build" || name == "builds"))
            skip = true;                                         // build trees, earlier game builds
        else if (isDirectory && relativeKey == "scripts")
            skip = true;                                         // the C++ script build project
        else if (isDirectory && relativeKey == thumbnailsKey)
            skip = true;                                         // editor previews
        else if (name.find(".autosave-") != std::string::npos)
            skip = true;                                         // editor autosave backups
        else if (name.ends_with(".server-worlds"))
            skip = true;                                         // world exports for the game server
        else if (!isDirectory && name.ends_with(".log"))
            skip = true;
        else if (!isDirectory && !relative.has_parent_path() &&
                 (name.ends_with(".exe") || name.ends_with(".dll") || name.ends_with(".so") ||
                  name == "editor_layout.ini" || name == "editor_recent_projects.txt" || name == "imgui.ini"))
            skip = true;                                         // an engine/editor copy kept in the project folder (game modules live in Binaries/)
        else if (IsSameOrInside(request.projectRoot / relative, request.outputDir))
            skip = true;                                         // this build itself
        if (skip && relative.has_parent_path() == false)
            skipped.push_back(relative.generic_string());
        return skip;
    };
    CopyStats projectStats;
    if (!CopyTree(request.projectRoot, gameRoot, skipProjectEntry, projectStats, log))
        return result;
    if (!skipped.empty())
    {
        log += "[BUILD-GAME] left out (editor/build only):";
        for (const std::string& name : skipped)
            log += " " + name;
        log += "\n";
    }
    if (!std::filesystem::exists(gameRoot / "Binaries", ec))
        log += "[BUILD-GAME] note: the project has no Binaries/ (no C++ game module) — fine for Lua-only games\n";

    // 4. The scene the game starts in, and the boot pointer the runtime reads.
    if (!request.startupScene.empty())
    {
        if (!std::filesystem::exists(gameRoot / request.startupScene, ec))
        {
            log += "[BUILD-GAME] the startup scene is not in the package: " + request.startupScene + "\n";
            return result;
        }
        if (!SetStartupScene(gameRoot / "project.ixproj", request.startupScene, log))
            return result;
        log += "[BUILD-GAME] startup scene: " + request.startupScene + "\n";
    }
    {
        std::ofstream boot(request.outputDir / GamePackageService::kPackageMarker, std::ios::binary | std::ios::trunc);
        boot << kGameFolder << "\n";
        if (!boot)
        {
            log += "[BUILD-GAME] cannot write " + std::string(GamePackageService::kPackageMarker) + "\n";
            return result;
        }
    }

    const std::uint64_t totalBytes = engineStats.bytes + projectStats.bytes;
    std::ostringstream summary;
    summary << "[BUILD-GAME] done: " << gameName << kExecutableExtension << " + " << (engineStats.files + projectStats.files)
            << " files (" << (totalBytes + 512 * 1024) / (1024 * 1024) << " MB) in " << request.outputDir.generic_string() << "\n";
    log += summary.str();
    result.executable = executable;
    result.ok = true;
    return result;
}
} // namespace

std::string SanitizeGameName(const std::string& name)
{
    std::string result;
    for (char c : name)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        result.push_back(std::isalnum(uc) || c == ' ' || c == '-' || c == '_' || c == '.' ? c : '_');
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
        result.pop_back();
    while (!result.empty() && result.front() == ' ')
        result.erase(result.begin());
    return result.empty() ? std::string("Game") : result;
}

GamePackageService::~GamePackageService()
{
    Shutdown();
}

bool GamePackageService::IsRunning() const
{
    return m_inFlight;
}

bool GamePackageService::RequestPackage(GamePackageRequest request)
{
    if (m_inFlight)
        return false;
    if (m_thread.joinable())
        m_thread.join();
    m_inFlight = true;
    m_finished.store(false);
    m_thread = std::jthread([this, request = std::move(request)] {
        GamePackageResult result = Package(request);
        {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_result = std::move(result);
        }
        m_finished.store(true);
    });
    return true;
}

bool GamePackageService::TryTakeResult(GamePackageResult& out)
{
    if (!m_inFlight || !m_finished.load())
        return false;
    m_inFlight = false;
    if (m_thread.joinable())
        m_thread.join();
    std::lock_guard<std::mutex> lock(m_resultMutex);
    out = std::move(m_result);
    m_result = {};
    return true;
}

void GamePackageService::Shutdown()
{
    if (m_thread.joinable())
        m_thread.join();
    m_inFlight = false;
}

} // namespace ixeditor::build
