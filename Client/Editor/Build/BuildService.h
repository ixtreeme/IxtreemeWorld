#pragma once

// Editor/Build — game-script Build pipeline service boundary (Phase 1).
//
// Long-term direction: Editor -> Build -> BuildPipeline -> playable client.
// The Editor's Build button must survive; CMake stays an internal detail.
//
// Phase 1 scope (behavior parity, no redesign):
// - This service owns ONLY the cmake configure+build worker thread that used to be
//   inline in apps/client/src/EngineApplication.cpp (RunGame frame loop).
// - UI stays in Editor (EditorImGui + editor_panels/*.inl): the Build button,
//   ScriptBuildState, build log panel, project scaffold (EnsureProjectScriptsScaffold),
//   module load/unload (LoadProjectGameModules/UnloadGameModules).
// - Dependency direction: Editor UI / EngineApplication -> BuildService -> platform::RunProcess.
//   BuildService depends on NOTHING editor-specific (no EditorImGui, no ProjectManager,
//   no SceneManager, no Vulkan). It takes plain filesystem paths.
// - Remaining debt (Phase 2): move EnsureProjectScriptsScaffold + Load/UnloadGameModules
//   behind this boundary too, then expand toward ProjectValidator/ModuleResolver/
//   AssetCooker/ShaderCooker/WorldCooker/RuntimePackager/BuildManifestGenerator.

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace ixeditor::build
{

struct GameScriptBuildResult
{
    bool ok = false;
    std::string log;
};

class GameScriptBuildService
{
public:
    GameScriptBuildService() = default;
    ~GameScriptBuildService();

    GameScriptBuildService(const GameScriptBuildService&) = delete;
    GameScriptBuildService& operator=(const GameScriptBuildService&) = delete;

    // True while a build is in flight OR a completed result is waiting to be
    // taken. Matches the old `buildInFlight` flag exactly: a new RequestBuild is
    // refused until the pending result is drained via TryTakeResult.
    bool IsRunning() const;

    // Starts `cmake -S <scriptsDir> -B <buildDir> -A x64` then
    // `cmake --build <buildDir> --config <buildConfig>` on a worker thread.
    // Returns false (no-op) when a build is already running. Matches the exact
    // cmake argv previously embedded in EngineApplication.cpp.
    bool RequestBuild(const std::filesystem::path& scriptsDir,
                      const std::filesystem::path& buildDir,
                      const std::string& buildConfig);

    // Main-thread poll: when the worker has finished, joins it and moves the
    // result out. Returns true exactly once per completed build.
    bool TryTakeResult(GameScriptBuildResult& out);

    // Blocks until any in-flight build finishes. Must be called before the
    // owning scope (RunGame locals referenced by the worker) unwinds.
    void Shutdown();

    // Build config matching the running engine binary (CRT/IDL parity: a Release
    // engine is /MT, a Debug engine /MTd — the module MUST match).
    static std::string BuildConfigForCurrentBinary();
    static std::filesystem::path ScriptsDirFor(const std::filesystem::path& projectRoot);
    static std::filesystem::path BuildDirFor(const std::filesystem::path& projectRoot);

private:
    std::jthread m_thread;
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_ok{false};
    std::mutex m_logMutex;
    std::string m_logShared;
    bool m_inFlight = false;
};

} // namespace ixeditor::build
