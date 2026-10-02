#pragma once

// Editor/Build — "Build Game": turns an editor project into a game folder that runs without the
// editor (the RuntimePackager of the BuildService roadmap).
//
// Package layout (the runtime finds everything relative to its own folder):
//   <Output>/<GameName>.exe     the editor-less engine runtime (IXTREEME_WITH_EDITOR=OFF build)
//   <Output>/assets/...         the engine's own runtime assets (shaders, fonts, internal)
//   <Output>/game.txt           names the game project folder below ("Game")
//   <Output>/Game/...           the project: project.ixproj, Assets/, scenes, Binaries/ (game modules)
// The project sits in a subfolder because its "Assets" folder and the engine's "assets" folder would
// be the same folder on a case-insensitive file system.
//
// Like GameScriptBuildService this depends on nothing editor-specific: plain paths in, a log out.
// All work (an optional runtime build with cmake, then the copy) runs on one worker thread.

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace ixeditor::build
{

struct GamePackageRequest
{
    // Runtime player: a prebuilt one shipped next to the editor (prebuiltRuntimeDir/IxtreemeEngine.exe)
    // wins; otherwise it is built from the engine sources (engineSourceDir, configured like the
    // editor's own build in editorBuildDir) into engineSourceDir/build-runtime.
    std::filesystem::path prebuiltRuntimeDir;
    std::filesystem::path engineSourceDir;
    std::filesystem::path editorBuildDir;
    std::string buildConfig = "Release";

    std::filesystem::path projectRoot;
    std::filesystem::path assetRoot;      // the project's asset folder (its thumbnails are skipped)
    std::filesystem::path outputDir;      // the game folder to (re)create
    std::string gameName;                 // <GameName>.exe
    std::string startupScene;             // project-relative scene the game starts in ("" = keep)
};

struct GamePackageResult
{
    bool ok = false;
    std::string log;
    std::filesystem::path executable;     // the packaged <GameName>.exe (when ok)
};

class GamePackageService
{
public:
    GamePackageService() = default;
    ~GamePackageService();

    GamePackageService(const GamePackageService&) = delete;
    GamePackageService& operator=(const GamePackageService&) = delete;

    // True while packaging runs OR a finished result waits to be taken (as GameScriptBuildService).
    bool IsRunning() const;
    bool RequestPackage(GamePackageRequest request);
    // Main-thread poll: true exactly once per finished package.
    bool TryTakeResult(GamePackageResult& out);
    void Shutdown();

    // The marker file a package carries: an existing output folder is only replaced when it has it
    // (so a typo in the output path can never wipe an unrelated folder).
    static constexpr const char* kPackageMarker = "game.txt";

private:
    std::jthread m_thread;
    std::atomic<bool> m_finished{false};
    std::mutex m_resultMutex;
    GamePackageResult m_result;
    bool m_inFlight = false;
};

// Makes a name usable as a file name (the packaged executable's).
std::string SanitizeGameName(const std::string& name);

} // namespace ixeditor::build
