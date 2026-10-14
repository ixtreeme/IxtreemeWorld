#pragma once

// Plain-old-data for the Script component on an entity (NO VM / no engine deps) — so MapEditorTypes /
// SceneManager / PrefabDocument can include it cheaply, exactly like AudioComponents.h. The live
// per-entity ScriptInstance (the running script) lives in the Play loop, not here.

#include <cstdint>
#include <map>
#include <string>

namespace ixscript
{

enum class ScriptBackendType : std::uint8_t
{
    None = 0,
    Native,       // a C++ class registered via IXSCRIPT_REGISTER (engine-internal, not project-facing)
    Lua,          // a .lua script asset
    AngelScript   // a .as script asset (the project-facing "native-like" backend)
};

inline const char* BackendName(ScriptBackendType b)
{
    switch (b)
    {
    case ScriptBackendType::Native: return "Native";
    case ScriptBackendType::Lua: return "Lua";
    case ScriptBackendType::AngelScript: return "AngelScript";
    case ScriptBackendType::None: return "None";
    }
    return "Native";
}

inline ScriptBackendType ParseBackend(const std::string& s)
{
    if (s == "Native") return ScriptBackendType::Native;
    if (s == "Lua") return ScriptBackendType::Lua;
    if (s == "AngelScript") return ScriptBackendType::AngelScript;
    return ScriptBackendType::None;
}

struct ScriptComponent
{
    // AngelScript is the project-facing default (C++ native scripts are engine-internal only).
    ScriptBackendType backend = ScriptBackendType::AngelScript;
    std::string scriptAssetId;     // Lua / AngelScript backend: the .lua / .as script asset id
    std::string nativeClassName;   // Native backend: the IXSCRIPT_REGISTER name (engine-internal)
    bool enabled = true;
    std::map<std::string, std::string> parameters;  // string key/value, exposed to the script (ordered)
};

} // namespace ixscript
