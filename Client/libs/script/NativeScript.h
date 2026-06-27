#pragma once

// The C++ "MonoBehaviour": derive your gameplay class from NativeScript, override the lifecycle
// hooks, and reach the engine through the convenience methods (or the raw `api` facade). Register it
// with IXSCRIPT_REGISTER(YourClass) and it shows up as a Script component you can attach to an entity.
//
//   class PlayerMover : public ixscript::NativeScript {
//       float speed = 5.0f;
//       void OnStart() override { speed = ParamFloat("speed", speed); }
//       void OnUpdate(float dt) override {
//           float p[3]; GetPosition(p);
//           if (IsKeyDown(ixscript::ScriptKey::W)) p[2] += speed * dt;
//           SetPosition(p);
//       }
//   };
//   IXSCRIPT_REGISTER(PlayerMover)

#include "ScriptApi.h"
#include "IxModuleApi.h"   // FieldBinder (the reflected-field seam) + IXTREEME_MODULE_API_VERSION

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

namespace ixscript
{

class NativeScript
{
public:
    virtual ~NativeScript() = default;

    // --- lifecycle hooks (override what you need) ---
    virtual void OnStart() {}
    virtual void OnUpdate(float dtSeconds) {}
    virtual void OnDestroy() {}
    virtual void OnCollision(std::uint32_t otherEntityId) {}

    // Declare serialized / inspector-visible fields (Unity-[SerializeField] style). Default: none, so
    // existing scripts are unaffected. The engine calls this in two modes via the binder it passes —
    // REFLECT (read in-class defaults to build the inspector schema) and APPLY (write the saved values
    // into the members before OnStart). Authors never write this by hand: IX_REFLECT(Class, fields...)
    // generates it. NOTE: the engine default-constructs a THROWAWAY instance to reflect the schema, so a
    // script's constructor must be cheap and side-effect-free (no api use — api/entityId aren't bound yet).
    virtual void DeclareFields(FieldBinder& f) { (void)f; }

    // Wires the engine context. Called once by the native backend right after construction — not for
    // user code to call. The parameters are COPIED (not pointed at): the source ScriptComponent lives
    // inside an entity in a std::vector the engine reallocates during Play, so a pointer would dangle.
    void BindContext(IScriptApi* a, std::uint32_t id, const std::map<std::string, std::string>& p)
    {
        api = a;
        entityId = id;
        m_params = p;
    }

protected:
    // --- MonoBehaviour-style convenience: operate on THIS entity via the engine facade ---
    void GetPosition(float out[3]) { api->GetPosition(entityId, out); }
    void SetPosition(const float p[3]) { api->SetPosition(entityId, p); }
    void GetRotation(float out[3]) { api->GetRotation(entityId, out); }     // Euler degrees
    void SetRotation(const float r[3]) { api->SetRotation(entityId, r); }
    void GetScale(float out[3]) { api->GetScale(entityId, out); }
    void SetScale(const float s[3]) { api->SetScale(entityId, s); }

    bool IsKeyDown(ScriptKey key) { return api->IsKeyDown(key); }
    void MouseDelta(float& dx, float& dy) { api->GetMouseDelta(dx, dy); }
    float DeltaTime() { return static_cast<float>(api->GetDeltaTime()); }
    float ElapsedTime() { return static_cast<float>(api->GetElapsedTime()); }

    std::uint32_t Find(const std::string& name) { return api->FindEntityByName(name); }
    void PlayOneShot(const std::string& clipAssetId) { api->PlayOneShot(clipAssetId); }

    // --- spawn / destroy (deferred — the new entity appears next frame) ---
    std::uint32_t SpawnMesh(const std::string& meshAssetId, float x, float y, float z)
    {
        return api->SpawnMesh(meshAssetId, x, y, z);
    }
    std::uint32_t SpawnMesh(const std::string& meshAssetId, const float pos[3])
    {
        return api->SpawnMesh(meshAssetId, pos[0], pos[1], pos[2]);
    }
    std::uint32_t SpawnPrefab(const std::string& prefabAssetId, float x, float y, float z)
    {
        return api->SpawnPrefab(prefabAssetId, x, y, z);
    }
    void DestroyEntity(std::uint32_t id) { api->DestroyEntity(id); }
    void DestroySelf() { api->DestroyEntity(entityId); }

    // --- physics raycast ---
    RaycastHit Raycast(float ox, float oy, float oz, float dx, float dy, float dz, float maxDist)
    {
        return api->Raycast(ox, oy, oz, dx, dy, dz, maxDist);
    }

    // --- animator parameters (operate on THIS entity's animator) ---
    void SetAnimatorFloat(const std::string& name, float v) { api->SetAnimatorFloat(entityId, name, v); }
    void SetAnimatorBool(const std::string& name, bool v) { api->SetAnimatorBool(entityId, name, v); }
    void SetAnimatorTrigger(const std::string& name) { api->SetAnimatorTrigger(entityId, name); }

    void Log(const std::string& msg) { api->Log(msg); }
    void LogError(const std::string& msg) { api->LogError(msg); }

    // --- exposed parameters (string KV set in the inspector) ---
    std::string Param(const std::string& key, const std::string& def = {}) const
    {
        const auto it = m_params.find(key);
        return it != m_params.end() ? it->second : def;
    }
    float ParamFloat(const std::string& key, float def = 0.0f) const
    {
        const auto it = m_params.find(key);
        return it != m_params.end() ? static_cast<float>(std::atof(it->second.c_str())) : def;
    }

    IScriptApi* api = nullptr;      // the boundary-safe engine facade (heap-safe across a /MT module DLL)
    std::uint32_t entityId = 0;     // this script's own entity (like Unity's gameObject)

private:
    std::map<std::string, std::string> m_params;  // owned copy — safe across entity-vector reallocation
};

// Factory + registration. The registry storage lives in NativeBackend.cpp (engine) or, for a game
// module DLL, in IxModuleRegistry.inl — this header is just the seam the macro calls.
using NativeScriptFactory = std::unique_ptr<NativeScript> (*)();  // engine-internal (owns via unique_ptr)
using NativeScriptFactoryRaw = NativeScript* (*)();               // C-ABI seam (raw new; engine wraps it)
bool RegisterNativeScript(const std::string& className, NativeScriptFactory factory);

#if defined(IXTREEME_GAME_MODULE)
// In a game-module DLL: registration is recorded into a DLL-local list (defined in IxModuleRegistry.inl)
// and drained to the engine through the C-ABI registrar at load time. Declared here so the macro
// resolves even before that .inl is included.
namespace detail
{
bool ModuleRegisterScript(const char* className, NativeScriptFactoryRaw factory);
}
#endif

} // namespace ixscript

// Registers a native script class by name. Place at file scope in the .cpp that defines the class.
// The SAME macro serves both worlds: an in-engine build registers into the engine's static registry;
// a module build (IXTREEME_GAME_MODULE) records into the DLL-local list with a RAW factory.
#if defined(IXTREEME_GAME_MODULE)
#define IXSCRIPT_REGISTER(Class)                                                        \
    namespace                                                                           \
    {                                                                                   \
        const bool g_ixscript_registered_##Class = ::ixscript::detail::ModuleRegisterScript( \
            #Class, []() -> ::ixscript::NativeScript* { return new Class(); });         \
    }
#else
#define IXSCRIPT_REGISTER(Class)                                                        \
    namespace                                                                           \
    {                                                                                   \
        const bool g_ixscript_registered_##Class = ::ixscript::RegisterNativeScript(    \
            #Class, []() -> std::unique_ptr<::ixscript::NativeScript> {                 \
                return std::make_unique<Class>();                                       \
            });                                                                         \
    }
#endif

// ---- IX_REFLECT: declare serialized / inspector-visible fields ------------------------------------
// Place inside the class body, listing the POD members to expose:
//     float speed = 120.0f; bool active = true;
//     IX_REFLECT(MyScript, speed, active)
// It generates DeclareFields(FieldBinder&), binding each member by name. f.Auto deduces the type from
// the member (float/int/bool/float[3]=vec3/float[4]=color). The in-class default is the inspector
// default; the saved value is written into the member before OnStart. Up to 16 fields.
// IX_EXPAND forces an extra rescan so MSVC's traditional preprocessor re-splits __VA_ARGS__ instead of
// passing it as one token. It must wrap EVERY recursive step (not just the count pick), or 3+ fields
// expand wrong (the nested __VA_ARGS__ stays glued). No-op under /Zc:preprocessor and conformant CPPs.
#define IX_EXPAND(X) X
#define IX_FE_1(WHAT, X)       WHAT(X)
#define IX_FE_2(WHAT, X, ...)  WHAT(X) IX_EXPAND(IX_FE_1(WHAT, __VA_ARGS__))
#define IX_FE_3(WHAT, X, ...)  WHAT(X) IX_EXPAND(IX_FE_2(WHAT, __VA_ARGS__))
#define IX_FE_4(WHAT, X, ...)  WHAT(X) IX_EXPAND(IX_FE_3(WHAT, __VA_ARGS__))
#define IX_FE_5(WHAT, X, ...)  WHAT(X) IX_EXPAND(IX_FE_4(WHAT, __VA_ARGS__))
#define IX_FE_6(WHAT, X, ...)  WHAT(X) IX_EXPAND(IX_FE_5(WHAT, __VA_ARGS__))
#define IX_FE_7(WHAT, X, ...)  WHAT(X) IX_EXPAND(IX_FE_6(WHAT, __VA_ARGS__))
#define IX_FE_8(WHAT, X, ...)  WHAT(X) IX_EXPAND(IX_FE_7(WHAT, __VA_ARGS__))
#define IX_FE_9(WHAT, X, ...)  WHAT(X) IX_EXPAND(IX_FE_8(WHAT, __VA_ARGS__))
#define IX_FE_10(WHAT, X, ...) WHAT(X) IX_EXPAND(IX_FE_9(WHAT, __VA_ARGS__))
#define IX_FE_11(WHAT, X, ...) WHAT(X) IX_EXPAND(IX_FE_10(WHAT, __VA_ARGS__))
#define IX_FE_12(WHAT, X, ...) WHAT(X) IX_EXPAND(IX_FE_11(WHAT, __VA_ARGS__))
#define IX_FE_13(WHAT, X, ...) WHAT(X) IX_EXPAND(IX_FE_12(WHAT, __VA_ARGS__))
#define IX_FE_14(WHAT, X, ...) WHAT(X) IX_EXPAND(IX_FE_13(WHAT, __VA_ARGS__))
#define IX_FE_15(WHAT, X, ...) WHAT(X) IX_EXPAND(IX_FE_14(WHAT, __VA_ARGS__))
#define IX_FE_16(WHAT, X, ...) WHAT(X) IX_EXPAND(IX_FE_15(WHAT, __VA_ARGS__))
#define IX_FE_GET(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16, N, ...) IX_FE_##N
#define IX_FOR_EACH(WHAT, ...) \
    IX_EXPAND(IX_FE_GET(__VA_ARGS__,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)(WHAT, __VA_ARGS__))

#define IX_REFLECT_ONE(field) f.Auto(#field, field);  // f.Auto deduces the member's type
#define IX_REFLECT(Class, ...)                                  \
    void DeclareFields(::ixscript::FieldBinder& f) override      \
    {                                                           \
        IX_FOR_EACH(IX_REFLECT_ONE, __VA_ARGS__)                \
    }
