#include "LuaBackend.h"

#include "ScriptApi.h"
#include "Debug.h"

#include <sol/sol.hpp>

#include <tuple>
#include <utility>

namespace ixscript
{
namespace
{

// String -> ScriptKey, the single source of truth for the Lua key names. Case-sensitive; an unknown
// name yields false from IsKeyDown rather than an error. Mirrors the ScriptKey enum in ScriptApi.h.
bool ParseKey(const std::string& name, ScriptKey& out)
{
    if (name == "W") { out = ScriptKey::W; return true; }
    if (name == "A") { out = ScriptKey::A; return true; }
    if (name == "S") { out = ScriptKey::S; return true; }
    if (name == "D") { out = ScriptKey::D; return true; }
    if (name == "Space") { out = ScriptKey::Space; return true; }
    if (name == "Shift") { out = ScriptKey::Shift; return true; }
    if (name == "Ctrl") { out = ScriptKey::Ctrl; return true; }
    if (name == "Up") { out = ScriptKey::Up; return true; }
    if (name == "Down") { out = ScriptKey::Down; return true; }
    if (name == "Left") { out = ScriptKey::Left; return true; }
    if (name == "Right") { out = ScriptKey::Right; return true; }
    if (name == "MouseLeft") { out = ScriptKey::MouseLeft; return true; }
    if (name == "MouseRight") { out = ScriptKey::MouseRight; return true; }
    return false;
}

// Reads a hook function out of a script's environment; returns an invalid function if absent (so an
// optional hook like OnCollision is simply skipped, matching a native script not overriding it).
sol::protected_function GrabHook(sol::environment& env, const char* name)
{
    sol::object o = env[name];
    if (o.get_type() == sol::type::function)
        return o.as<sol::protected_function>();
    return sol::protected_function();
}

// One running .lua on one entity. Owns its private environment (keeps the env, its hooks, `self`, and
// the script's locals alive), the cached hooks, and the self/params table. Every hook is pcall-wrapped;
// the first error logs once and trips the dead flag so the instance stops being driven.
class LuaScriptInstance final : public ScriptInstance
{
public:
    LuaScriptInstance(std::uint32_t entityId,
                      sol::environment env,
                      sol::table self,
                      sol::protected_function onStart,
                      sol::protected_function onUpdate,
                      sol::protected_function onDestroy,
                      sol::protected_function onCollision)
        : m_entityId(entityId)
        , m_env(std::move(env))
        , m_self(std::move(self))
        , m_onStart(std::move(onStart))
        , m_onUpdate(std::move(onUpdate))
        , m_onDestroy(std::move(onDestroy))
        , m_onCollision(std::move(onCollision))
    {
    }

    void OnStart() override
    {
        Call(m_onStart, "OnStart", [&] { return m_onStart(m_self); });
    }
    void OnUpdate(float dtSeconds) override
    {
        Call(m_onUpdate, "OnUpdate", [&] { return m_onUpdate(m_self, dtSeconds); });
    }
    void OnDestroy() override
    {
        Call(m_onDestroy, "OnDestroy", [&] { return m_onDestroy(m_self); });
    }
    void OnCollision(std::uint32_t otherEntityId) override
    {
        Call(m_onCollision, "OnCollision", [&] { return m_onCollision(m_self, otherEntityId); });
    }

private:
    template <typename Invoke>
    void Call(sol::protected_function& fn, const char* name, Invoke&& invoke)
    {
        if (m_dead || !fn.valid())
            return;
        const sol::protected_function_result result = invoke();
        if (!result.valid())
        {
            const sol::error err = result;
            TraceError("[SCRIPT][lua] entity=%u %s error: %s", m_entityId, name, err.what());
            m_dead = true;  // one bad call retires this instance for the session; never spams the frame
        }
    }

    std::uint32_t m_entityId = 0;
    sol::environment m_env;  // MUST outlive the cached hooks — keeps the script's env + locals alive
    sol::table m_self;
    sol::protected_function m_onStart;
    sol::protected_function m_onUpdate;
    sol::protected_function m_onDestroy;
    sol::protected_function m_onCollision;
    bool m_dead = false;
};

} // namespace

// The single sol::state for the whole Play session + the shared, read-only engine-API table. Per-entity
// environments fall through to m_engineApi (via __index), and m_engineApi falls through to the safe
// globals (math/string/table/tonumber/...). Source text is cached per asset id.
struct LuaBackend::Impl
{
    sol::state lua;
    sol::table engineApi;
    bool apiBound = false;
    std::unordered_map<std::string, std::string> sourceCache;

    Impl()
    {
        // Sandbox: base/math/string/table only — no io/os/package (no filesystem or OS reach). Engine
        // capabilities arrive exclusively through the bound engine-API table.
        lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
        // base still ships dofile/loadfile (filesystem reach) — remove them to keep the sandbox closed.
        lua.globals()["dofile"] = sol::nil;
        lua.globals()["loadfile"] = sol::nil;
        engineApi = lua.create_table();
        // engineApi falls through to the safe globals so scripts still see tonumber/math/string/etc.
        // __metatable=false hides the metatable from getmetatable() so a script can't reflect its way
        // up the __index chain and write into the shared engineApi/globals (poisoning other instances).
        sol::table meta = lua.create_table();
        meta["__index"] = lua.globals();
        meta["__metatable"] = false;
        engineApi[sol::metatable_key] = meta;
    }

    void BindEngineApi(ScriptApi& api)
    {
        if (apiBound)
            return;
        apiBound = true;
        ScriptApi* a = &api;  // valid for the session: ScriptApi outlives the ScriptSystem/LuaBackend
        sol::table& t = engineApi;

        t.set_function("Find", [a](const std::string& name) { return a->FindEntityByName(name); });
        t.set_function("EntityExists", [a](std::uint32_t id) { return a->EntityExists(id); });
        t.set_function("GetName", [a](std::uint32_t id) { return a->GetEntityName(id); });

        t.set_function("GetPosition", [a](std::uint32_t id) {
            float o[3]; a->GetPosition(id, o); return std::make_tuple(o[0], o[1], o[2]);
        });
        t.set_function("SetPosition", [a](std::uint32_t id, float x, float y, float z) {
            const float p[3] = {x, y, z}; a->SetPosition(id, p);
        });
        t.set_function("GetRotation", [a](std::uint32_t id) {
            float o[3]; a->GetRotation(id, o); return std::make_tuple(o[0], o[1], o[2]);
        });
        t.set_function("SetRotation", [a](std::uint32_t id, float x, float y, float z) {
            const float r[3] = {x, y, z}; a->SetRotation(id, r);
        });
        t.set_function("GetScale", [a](std::uint32_t id) {
            float o[3]; a->GetScale(id, o); return std::make_tuple(o[0], o[1], o[2]);
        });
        t.set_function("SetScale", [a](std::uint32_t id, float x, float y, float z) {
            const float s[3] = {x, y, z}; a->SetScale(id, s);
        });

        t.set_function("IsKeyDown", [a](const std::string& key) {
            ScriptKey k; return ParseKey(key, k) ? a->IsKeyDown(k) : false;
        });
        t.set_function("MouseDelta", [a]() {
            float dx = 0.0f, dy = 0.0f; a->GetMouseDelta(dx, dy); return std::make_tuple(dx, dy);
        });
        t.set_function("DeltaTime", [a]() { return a->GetDeltaTime(); });
        t.set_function("ElapsedTime", [a]() { return a->GetElapsedTime(); });

        t.set_function("PlayOneShot", [a](const std::string& clip) { a->PlayOneShot(clip); });
        t.set_function("Log", [a](const std::string& msg) { a->Log(msg); });
        t.set_function("LogError", [a](const std::string& msg) { a->LogError(msg); });

        // --- rich API: spawn/destroy, raycast, animator params ---
        t.set_function("SpawnMesh", [a](const std::string& id, float x, float y, float z) {
            return a->SpawnMesh(id, x, y, z);
        });
        t.set_function("SpawnPrefab", [a](const std::string& id, float x, float y, float z) {
            return a->SpawnPrefab(id, x, y, z);
        });
        t.set_function("DestroyEntity", [a](std::uint32_t id) { a->DestroyEntity(id); });
        // Raycast(ox,oy,oz, dx,dy,dz, maxDist) -> hit, entityId, px,py,pz, nx,ny,nz, distance
        t.set_function("Raycast", [a](float ox, float oy, float oz, float dx, float dy, float dz, float maxDist) {
            const RaycastHit h = a->Raycast(ox, oy, oz, dx, dy, dz, maxDist);
            return std::make_tuple(h.hit, h.entityId,
                h.point[0], h.point[1], h.point[2], h.normal[0], h.normal[1], h.normal[2], h.distance);
        });
        t.set_function("SetAnimatorFloat", [a](std::uint32_t id, const std::string& n, float v) {
            a->SetAnimatorFloat(id, n, v);
        });
        t.set_function("SetAnimatorBool", [a](std::uint32_t id, const std::string& n, bool v) {
            a->SetAnimatorBool(id, n, v);
        });
        t.set_function("SetAnimatorTrigger", [a](std::uint32_t id, const std::string& n) {
            a->SetAnimatorTrigger(id, n);
        });

        // Ergonomic Key.* table: names map to the same strings IsKeyDown accepts (Key.W == "W").
        sol::table keys = lua.create_table();
        for (const char* name : {"W", "A", "S", "D", "Space", "Shift", "Ctrl",
                                 "Up", "Down", "Left", "Right", "MouseLeft", "MouseRight"})
            keys[name] = name;
        t["Key"] = keys;
    }

    std::string Source(ScriptApi& api, const std::string& assetId)
    {
        const auto it = sourceCache.find(assetId);
        if (it != sourceCache.end())
            return it->second;
        std::string text = api.LoadScriptSource(assetId);
        if (!text.empty())  // don't cache a failed resolve — a later asset fix should be picked up
            sourceCache.emplace(assetId, text);
        return text;
    }
};

LuaBackend::LuaBackend() : m_impl(std::make_unique<Impl>()) {}
LuaBackend::~LuaBackend() = default;

void LuaBackend::InvalidateSource(const std::string& assetId)
{
    m_impl->sourceCache.erase(assetId);  // next CreateInstance re-reads the file + recompiles
}

std::unique_ptr<ScriptInstance> LuaBackend::CreateInstance(std::uint32_t entityId,
                                                           const ScriptComponent& comp,
                                                           ScriptApi& api)
{
    if (comp.scriptAssetId.empty())
    {
        api.LogError("Lua script component has no script asset assigned (entity skipped)");
        return nullptr;
    }
    m_impl->BindEngineApi(api);

    const std::string source = m_impl->Source(api, comp.scriptAssetId);
    if (source.empty())
    {
        api.LogError("Lua script source '" + comp.scriptAssetId + "' could not be loaded (entity skipped)");
        return nullptr;
    }

    // Private environment for THIS entity: a fresh _ENV whose global lookups fall through to the shared
    // engine API (and then the safe stdlib). Global writes (defining OnUpdate, top-level state) land in
    // this env only, so two entities running the same .lua never share locals.
    sol::environment env(m_impl->lua, sol::create, m_impl->engineApi);

    sol::load_result loaded = m_impl->lua.load(source, "@" + comp.scriptAssetId);
    if (!loaded.valid())
    {
        const sol::error err = loaded;
        api.LogError("Lua compile error in '" + comp.scriptAssetId + "': " + err.what());
        return nullptr;
    }
    sol::protected_function chunk = loaded;
    sol::set_environment(env, chunk);
    const sol::protected_function_result ran = chunk();
    if (!ran.valid())
    {
        const sol::error err = ran;
        api.LogError("Lua load-time error in '" + comp.scriptAssetId + "': " + err.what());
        return nullptr;
    }

    // self: { id = <entity>, params = { <string,string> ... } }, copied by value (the ScriptComponent
    // lives in an entity vector the engine reallocates during Play).
    sol::table self = m_impl->lua.create_table();
    self["id"] = entityId;
    sol::table params = m_impl->lua.create_table();
    for (const auto& [key, value] : comp.parameters)
        params[key] = value;
    self["params"] = params;

    // Grab the hooks BEFORE moving env (argument evaluation order is unspecified, so reading a
    // moved-from env in the constructor call would yield invalid functions).
    sol::protected_function onStart = GrabHook(env, "OnStart");
    sol::protected_function onUpdate = GrabHook(env, "OnUpdate");
    sol::protected_function onDestroy = GrabHook(env, "OnDestroy");
    sol::protected_function onCollision = GrabHook(env, "OnCollision");

    return std::make_unique<LuaScriptInstance>(
        entityId,
        std::move(env),
        std::move(self),
        std::move(onStart),
        std::move(onUpdate),
        std::move(onDestroy),
        std::move(onCollision));
}

} // namespace ixscript
