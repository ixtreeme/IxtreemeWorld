#pragma once

// The native C++ backend: resolves a ScriptComponent's nativeClassName against the IXSCRIPT_REGISTER
// registry and wraps the user's NativeScript in a ScriptInstance the Play loop drives.

#include "IScriptBackend.h"
#include "IxModuleApi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ixscript
{

// Reflected-field types the inspector can edit (the POD member types IX_REFLECT supports).
enum class ScriptFieldType : std::uint8_t { Float, Int, Bool, Vec3, Color };

// Engine-side description of ONE IX_REFLECT'd field, built by DescribeFields by running a throwaway
// default-constructed instance's DeclareFields in REFLECT mode. Pure engine-heap POD; the DLL never
// sees it. `defaultValue` is the in-class default captured from the probe, ALREADY string-formatted to
// match how ScriptComponent.parameters stores it (so the inspector can seed parameters[name] directly).
struct ScriptFieldDesc
{
    std::string name;                                  // member name == parameters[] key
    ScriptFieldType type = ScriptFieldType::Float;
    std::string defaultValue;                          // e.g. "120", "1"/"0", "1,0,0", "1,1,1,1"
};

class NativeBackend : public IScriptBackend
{
public:
    std::unique_ptr<ScriptInstance> CreateInstance(std::uint32_t entityId,
                                                   const ScriptComponent& comp,
                                                   ScriptApi& api) override;

    // Class names registered via IXSCRIPT_REGISTER (in-engine OR loaded module) — for the class picker.
    static std::vector<std::string> RegisteredNames();

    // Reflected schema for a native class: name+type+default of each IX_REFLECT'd field. Built ONCE per
    // class by running a THROWAWAY default-constructed instance's DeclareFields in REFLECT mode (never
    // BindContext, never OnStart), then cached. The cache is cleared by ClearExternalNativeScripts on
    // module reload. Returns empty for an unknown class, Lua, or a native class with no IX_REFLECT
    // fields — empty == "the inspector should use the manual key/value fallback".
    static std::vector<ScriptFieldDesc> DescribeFields(const std::string& className);
};

// Result of invoking a loaded game module's entry point.
struct ModuleLoadResult
{
    bool versionOk = false;        // module's API version matched the engine's
    std::uint32_t moduleVersion = 0;
    int registeredCount = 0;       // classes the module registered (only valid if versionOk)
};

// Calls a loaded module's resolved entry point with an engine-owned registrar, version-checks the
// result, and drains its class registrations into the SAME registry the in-engine samples use. The
// engine (render layer) resolves the symbol via the platform loader and passes the function here, so
// the registry coupling stays inside libs/script. A null entry yields versionOk=false.
ModuleLoadResult InvokeGameModule(IxModuleEntryFn entry);

// Removes every class registered from a loaded module (NOT the in-engine IXSCRIPT_REGISTER samples),
// so the registry holds no factory pointing into a DLL that's about to be unloaded. Call this BEFORE
// CloseLibrary on the module handles (e.g. when switching projects). Must not run mid-Play.
void ClearExternalNativeScripts();

} // namespace ixscript
