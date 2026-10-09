// Editor/Build service implementation. See BuildService.h for the boundary contract.
// Behavior parity: identical cmake argv, identical log merging, identical failure
// strings as the code previously inline in EngineApplication.cpp.

#include "BuildService.h"

#include "platform/process.h"

#include <mutex>
#include <utility>

namespace ixeditor::build
{

GameScriptBuildService::~GameScriptBuildService()
{
    Shutdown();
}

bool GameScriptBuildService::IsRunning() const
{
    return m_inFlight;
}

bool GameScriptBuildService::RequestBuild(const std::filesystem::path& scriptsDir,
                                          const std::filesystem::path& buildDir,
                                          const std::string& buildConfig)
{
    if (m_inFlight)
        return false;

    if (m_thread.joinable())
        m_thread.join();

    m_inFlight = true;
    m_finished.store(false);
    m_ok.store(false);
    {
        std::lock_guard<std::mutex> lk(m_logMutex);
        m_logShared.clear();
    }

    m_thread = std::jthread(
        [this, scriptsDir, buildDir, buildConfig] {
            platform::ProcessResult cfg = platform::RunProcess(
                {"cmake", "-S", scriptsDir.string(), "-B", buildDir.string(), "-A", "x64"});
            std::string log = cfg.output;
            bool ok = cfg.launched && cfg.exitCode == 0;
            if (!cfg.launched)
                log += "\n[BUILD] cmake not found on PATH — install CMake or add it to PATH.";
            else if (ok)
            {
                platform::ProcessResult bld = platform::RunProcess(
                    {"cmake", "--build", buildDir.string(), "--config", buildConfig});
                log += "\n" + bld.output;
                ok = bld.launched && bld.exitCode == 0;
            }
            {
                std::lock_guard<std::mutex> lk(m_logMutex);
                m_logShared = std::move(log);
            }
            m_ok.store(ok);
            m_finished.store(true);
        });
    return true;
}

bool GameScriptBuildService::TryTakeResult(GameScriptBuildResult& out)
{
    if (!m_inFlight || !m_finished.load())
        return false;

    m_inFlight = false;
    if (m_thread.joinable())
        m_thread.join(); // worker has finished; join returns immediately

    {
        std::lock_guard<std::mutex> lk(m_logMutex);
        out.log = std::move(m_logShared);
    }
    out.ok = m_ok.load();
    return true;
}

void GameScriptBuildService::Shutdown()
{
    // Joining here guarantees the worker (which may reference the owner's locals
    // via captured paths only — no dangling refs by design) cannot outlive us.
    // m_inFlight flag is left as-is; owner is unwinding.
    if (m_thread.joinable())
        m_thread.join();
    m_inFlight = false;
}

std::string GameScriptBuildService::BuildConfigForCurrentBinary()
{
#if defined(NDEBUG)
    return "Release";
#else
    return "Debug";
#endif
}

std::filesystem::path GameScriptBuildService::ScriptsDirFor(const std::filesystem::path& projectRoot)
{
    return projectRoot / "Scripts";
}

std::filesystem::path GameScriptBuildService::BuildDirFor(const std::filesystem::path& projectRoot)
{
    return projectRoot / "Scripts" / "build";
}

} // namespace ixeditor::build
