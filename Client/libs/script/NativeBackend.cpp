#include "NativeBackend.h"
#include "NativeScript.h"
#include "Debug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ixscript
{
namespace
{

// className -> factory. std::function (not a bare function pointer) so it can also hold a CAPTURING
// wrapper around a loaded module's raw factory. A function-local static so the cross-TU
// IXSCRIPT_REGISTER initializers (which run before main) safely populate it regardless of static-init
// order. External (module) and in-engine registrations land in this SAME map.
using RegistryFactory = std::function<std::unique_ptr<NativeScript>()>;
std::map<std::string, RegistryFactory>& Registry()
{
    static std::map<std::string, RegistryFactory> registry;
    return registry;
}

// Names registered from loaded module DLLs (a subset of Registry()) — tracked so they can be purged
// before their DLL is unloaded, without disturbing the in-engine IXSCRIPT_REGISTER samples.
std::set<std::string>& ExternalNames()
{
    static std::set<std::string> names;
    return names;
}

// Engine-owned registrar handed to a loaded module's entry point. The module calls RegisterScript with
// a C-ABI raw factory; the engine copies the name into its own heap. Registrations are STAGED here, not
// committed to the global Registry() yet — because the module's API version is only known from the
// entry point's RETURN value (after all RegisterScript calls). The caller commits the staged entries to
// Registry() only if the version matches, so a mismatched module can't poison the registry.
class EngineModuleRegistrar : public IModuleRegistrar
{
public:
    void RegisterScript(const char* className, IxNativeScriptFactory factory) override
    {
        if (!className || !factory)
            return;
        staged.emplace_back(std::string(className),  // copy into engine heap (DLL owns the source buffer)
            [factory]() -> std::unique_ptr<NativeScript> {
                return std::unique_ptr<NativeScript>(factory());  // DLL news; engine owns (virtual dtor)
            });
    }
    std::uint32_t ApiVersion() const override { return IXTREEME_MODULE_API_VERSION; }

    std::vector<std::pair<std::string, RegistryFactory>> staged;
};

// Adapts a user NativeScript to the engine-facing ScriptInstance (just forwards the hooks).
class NativeScriptInstance : public ScriptInstance
{
public:
    explicit NativeScriptInstance(std::unique_ptr<NativeScript> script) : m_script(std::move(script)) {}
    void OnStart() override { m_script->OnStart(); }
    void OnUpdate(float dtSeconds) override { m_script->OnUpdate(dtSeconds); }
    void OnDestroy() override { m_script->OnDestroy(); }
    void OnCollision(std::uint32_t otherEntityId) override { m_script->OnCollision(otherEntityId); }

private:
    std::unique_ptr<NativeScript> m_script;
};

// Canonical string formatting for field values — MUST match the inspector's parse/format and the
// ApplyBinder's parse, so a reflected default, a hand-typed value, and a widget edit all round-trip.
std::string FormatFloat(float v)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}
std::string FormatVec(const float* v, int n)
{
    char buf[160];
    if (n == 4)
        std::snprintf(buf, sizeof(buf), "%g,%g,%g,%g", (double)v[0], (double)v[1], (double)v[2], (double)v[3]);
    else
        std::snprintf(buf, sizeof(buf), "%g,%g,%g", (double)v[0], (double)v[1], (double)v[2]);
    return buf;
}

// REFLECT mode: reads each member's in-class default and records {name, type, default-string} — the
// engine-side schema for the inspector. Runs on a throwaway default-constructed instance.
class ReflectBinder final : public FieldBinder
{
public:
    explicit ReflectBinder(std::vector<ScriptFieldDesc>& out) : m_out(out) {}
    void Float(const char* name, float& ref) override
    {
        m_out.push_back({name, ScriptFieldType::Float, FormatFloat(ref)});
    }
    void Int(const char* name, int& ref) override
    {
        m_out.push_back({name, ScriptFieldType::Int, std::to_string(ref)});
    }
    void Bool(const char* name, bool& ref) override
    {
        m_out.push_back({name, ScriptFieldType::Bool, ref ? "1" : "0"});
    }
    void Vec3(const char* name, float ref[3]) override
    {
        m_out.push_back({name, ScriptFieldType::Vec3, FormatVec(ref, 3)});
    }
    void Color(const char* name, float ref[4]) override
    {
        m_out.push_back({name, ScriptFieldType::Color, FormatVec(ref, 4)});
    }

private:
    std::vector<ScriptFieldDesc>& m_out;
};

// APPLY mode: writes the saved value from the parameters map into each member (before OnStart). An
// absent key leaves the member at its in-class default; malformed input degrades like ParamFloat's atof
// (never throws across the boundary). Comma-split fills the available vector components.
class ApplyBinder final : public FieldBinder
{
public:
    explicit ApplyBinder(const std::map<std::string, std::string>& params) : m_params(params) {}

    void Float(const char* name, float& ref) override
    {
        if (const std::string* s = Find(name))
            ref = std::strtof(s->c_str(), nullptr);
    }
    void Int(const char* name, int& ref) override
    {
        if (const std::string* s = Find(name))
            ref = std::atoi(s->c_str());
    }
    void Bool(const char* name, bool& ref) override
    {
        if (const std::string* s = Find(name))
            ref = (*s == "1" || *s == "true" || *s == "True");
    }
    void Vec3(const char* name, float ref[3]) override { ParseVec(name, ref, 3); }
    void Color(const char* name, float ref[4]) override { ParseVec(name, ref, 4); }

private:
    const std::string* Find(const char* name) const
    {
        const auto it = m_params.find(name);
        return it != m_params.end() ? &it->second : nullptr;
    }
    void ParseVec(const char* name, float* ref, int n) const
    {
        const std::string* s = Find(name);
        if (!s)
            return;
        const char* p = s->c_str();
        for (int i = 0; i < n && *p; ++i)
        {
            char* end = nullptr;
            ref[i] = std::strtof(p, &end);
            if (end == p)
                break;
            p = end;
            while (*p == ',' || *p == ' ')
                ++p;
        }
    }
    const std::map<std::string, std::string>& m_params;
};

} // namespace

bool RegisterNativeScript(const std::string& className, NativeScriptFactory factory)
{
    Registry()[className] = factory;
    return true;
}

std::unique_ptr<ScriptInstance> NativeBackend::CreateInstance(std::uint32_t entityId,
                                                              const ScriptComponent& comp,
                                                              ScriptApi& api)
{
    const auto it = Registry().find(comp.nativeClassName);
    if (it == Registry().end() || !it->second)
        return nullptr;  // unknown class name — caller skips this entity
    std::unique_ptr<NativeScript> script = it->second();
    if (!script)
        return nullptr;
    script->BindContext(&api, entityId, comp.parameters);  // parameters are copied into the script
    // Apply serialized IX_REFLECT field values into the members BEFORE OnStart (the Play loop fires
    // OnStart on the returned wrapper). A schema-less script has an empty DeclareFields → no-op.
    ApplyBinder apply(comp.parameters);
    script->DeclareFields(apply);
    return std::make_unique<NativeScriptInstance>(std::move(script));
}

std::vector<std::string> NativeBackend::RegisteredNames()
{
    std::vector<std::string> names;
    names.reserve(Registry().size());
    for (const auto& entry : Registry())
        names.push_back(entry.first);
    return names;
}

namespace
{
// className -> reflected field schema. Built once per class on first request; cleared on module reload.
std::map<std::string, std::vector<ScriptFieldDesc>>& SchemaCache()
{
    static std::map<std::string, std::vector<ScriptFieldDesc>> cache;
    return cache;
}
} // namespace

std::vector<ScriptFieldDesc> NativeBackend::DescribeFields(const std::string& className)
{
    if (const auto it = SchemaCache().find(className); it != SchemaCache().end())
        return it->second;

    std::vector<ScriptFieldDesc> schema;
    const auto it = Registry().find(className);
    if (it != Registry().end() && it->second)
    {
        // Throwaway default-constructed probe — NEVER BindContext/OnStart. DeclareFields(reflect) only
        // reads the members' in-class defaults. Destroyed here via the virtual dtor (DLL heap if module).
        std::unique_ptr<NativeScript> probe = it->second();
        if (probe)
        {
            ReflectBinder reflect(schema);
            probe->DeclareFields(reflect);
        }
    }
    SchemaCache()[className] = schema;  // cache (incl. empty: a schema-less class won't be re-probed)
    return schema;
}

ModuleLoadResult InvokeGameModule(IxModuleEntryFn entry)
{
    ModuleLoadResult result;
    if (!entry)
        return result;
    EngineModuleRegistrar registrar;
    result.moduleVersion = entry(&registrar);  // module stages its registrations into `registrar`
    result.versionOk = (result.moduleVersion == IXTREEME_MODULE_API_VERSION);
    if (!result.versionOk)
    {
        // Version mismatch ⇒ the module's vtable/ABI may not match ours; its staged factories are NOT
        // committed to the registry (nothing was added globally). Reject loudly.
        TraceError("[SCRIPT] game module API version mismatch: module=%u engine=%u (module rejected)",
            result.moduleVersion, IXTREEME_MODULE_API_VERSION);
        return result;
    }
    for (auto& [name, factory] : registrar.staged)
    {
        // Never let a module shadow a BUILT-IN class: it would overwrite the engine's factory and then
        // be purged by ClearExternalNativeScripts(), deleting the built-in for the session. (A built-in
        // is a name in Registry() that isn't tracked as external.)
        if (Registry().count(name) != 0 && ExternalNames().count(name) == 0)
        {
            TraceError("[SCRIPT] module class '%s' collides with a built-in script; skipped", name.c_str());
            continue;
        }
        Registry()[name] = std::move(factory);
        ExternalNames().insert(name);
        ++result.registeredCount;
    }
    return result;
}

void ClearExternalNativeScripts()
{
    for (const std::string& name : ExternalNames())
        Registry().erase(name);
    ExternalNames().clear();
    SchemaCache().clear();  // module factories are going away; cached schemas (and built-ins) must rebuild
}

} // namespace ixscript
