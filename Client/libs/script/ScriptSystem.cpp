#include "ScriptSystem.h"

#include "NativeBackend.h"
#include "LuaBackend.h"
#include "Debug.h"

#include <sol/sol.hpp>

namespace ixscript
{

ScriptSystem::ScriptSystem(ScriptApi& api)
    : m_api(api)
    , m_native(std::make_unique<NativeBackend>())
    , m_lua(std::make_unique<LuaBackend>())
{
}

ScriptSystem::~ScriptSystem() = default;

std::unique_ptr<ScriptInstance> ScriptSystem::CreateInstance(std::uint32_t entityId, const ScriptComponent& comp)
{
    if (!comp.enabled)
        return nullptr;
    switch (comp.backend)
    {
    case ScriptBackendType::Native:
        return m_native->CreateInstance(entityId, comp, m_api);
    case ScriptBackendType::Lua:
        return m_lua->CreateInstance(entityId, comp, m_api);
    case ScriptBackendType::None:
        return nullptr;
    }
    return nullptr;
}

std::vector<std::string> ScriptSystem::NativeClassNames()
{
    return NativeBackend::RegisteredNames();
}

void ScriptSystem::InvalidateLuaSource(const std::string& assetId)
{
    static_cast<LuaBackend*>(m_lua.get())->InvalidateSource(assetId);  // m_lua is always a LuaBackend
}

bool ScriptSystem::RunLuaString(const std::string& code)
{
    sol::state lua;
    // Sandbox: base/math/string/table only — no io/os/package (scripts can't touch the filesystem or
    // the OS). Engine capabilities arrive through the ScriptApi facade, not Lua's stdlib.
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);

    const sol::protected_function_result result = lua.safe_script(code, sol::script_pass_on_error);
    if (!result.valid())
    {
        const sol::error err = result;
        TraceError("[SCRIPT][lua] error: %s", err.what());
        return false;
    }
    Tracen("[SCRIPT] lua ok");
    return true;
}

} // namespace ixscript
