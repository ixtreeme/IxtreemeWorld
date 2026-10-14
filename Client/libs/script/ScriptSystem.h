#pragma once

// The scripting subsystem facade the engine talks to. Owns the backends (AngelScript + Lua for
// projects, legacy Native C++ internally) and routes ScriptComponent -> ScriptInstance by backend
// kind. The engine's Play loop creates one instance per scripted entity and drives its lifecycle.
// libs/script depends on NOTHING engine-specific; the engine-API bindings arrive through the
// injected ScriptApi facade.

#include "IScriptBackend.h"
#include "ScriptComponent.h"

#include <memory>
#include <string>
#include <vector>

namespace ixscript
{

class ScriptApi;

class ScriptSystem
{
public:
    explicit ScriptSystem(ScriptApi& api);
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    // Creates a live instance for a component (routes by backend). Returns nullptr if disabled or the
    // script can't be resolved — the caller simply skips that entity.
    std::unique_ptr<ScriptInstance> CreateInstance(std::uint32_t entityId, const ScriptComponent& comp);

    // Hot-reload: drop the cached source for a .lua asset so the next CreateInstance recompiles it.
    void InvalidateLuaSource(const std::string& assetId);

    // Hot-reload: mark a .as asset's compiled module stale so the next CreateInstance rebuilds it.
    void InvalidateAngelScriptSource(const std::string& assetId);

    // Registered native class names (engine-internal; not offered to projects).
    static std::vector<std::string> NativeClassNames();

    // Chunk-1 VM proof (kept; the Lua backend builds on this).
    static bool RunLuaString(const std::string& code);

    // Same smoke test for the AngelScript VM (logs "[SCRIPT] angelscript ok").
    static bool RunAngelScriptString(const std::string& code);

private:
    ScriptApi& m_api;
    std::unique_ptr<IScriptBackend> m_native;
    std::unique_ptr<IScriptBackend> m_lua;
    std::unique_ptr<IScriptBackend> m_angelScript;
};

} // namespace ixscript
