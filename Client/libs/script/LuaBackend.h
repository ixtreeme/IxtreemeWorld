#pragma once

// The Lua scripting backend: runs .lua ASSETS at runtime so a game built on the engine BINARY (no
// engine source) can drive entities. One sol::state for the whole Play session; each scripted entity
// gets its own isolated sol::environment whose globals fall through (via __index) to one shared,
// read-only engine-API table bound from the ScriptApi facade. Every hook is pcall-isolated — a script
// error logs once, marks that instance dead, and never crashes or spams the frame.
//
// libs/script stays engine-agnostic: the .lua SOURCE TEXT is pulled through ScriptApi::LoadScriptSource
// (implemented in apps/client); this file never sees the filesystem or the asset DB.

#include "IScriptBackend.h"

#include <memory>
#include <string>
#include <unordered_map>

// sol2 is heavy; keep it out of this header. The sol::state lives behind a PIMPL in the .cpp.
namespace ixscript
{

class ScriptApi;

class LuaBackend final : public IScriptBackend
{
public:
    LuaBackend();
    ~LuaBackend() override;
    LuaBackend(const LuaBackend&) = delete;
    LuaBackend& operator=(const LuaBackend&) = delete;

    std::unique_ptr<ScriptInstance> CreateInstance(std::uint32_t entityId,
                                                   const ScriptComponent& comp,
                                                   ScriptApi& api) override;

    // Hot-reload: drop the cached source for a .lua asset so the next CreateInstance re-reads + recompiles
    // it. The caller recreates the live instances bound to that asset.
    void InvalidateSource(const std::string& assetId);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  // owns the single sol::state + the shared engine-API table
};

} // namespace ixscript
