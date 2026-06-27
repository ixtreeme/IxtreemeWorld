#pragma once

// The language-agnostic SEAM between scripts and the engine. libs/script defines this pure-virtual
// facade; apps/client implements it (ScriptApiImpl) against the real engine state (entities, input,
// audio, ...). BOTH backends (native C++ and Lua) call the engine ONLY through here — so a script
// can never reach past this surface, and a 3rd backend (C#) would reuse the exact same API.
//
// Rotations are Euler angles in DEGREES at this boundary (script-friendly); the impl converts to/from
// the radians the engine stores. Entity ids are the editor MeshSceneEntity ids (0 = none/invalid).

#include <cstdint>
#include <string>
#include <vector>

namespace ixscript
{

// Engine-neutral input keys (NO Win32 VK_* — the engine is cross-platform). The impl maps these to
// the movement/input state.
enum class ScriptKey
{
    W, A, S, D,
    Space, Shift, Ctrl,
    Up, Down, Left, Right,
    MouseLeft, MouseRight
};

struct RaycastHit
{
    std::uint32_t entityId = 0;  // 0 if the hit body isn't an entity
    float point[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 0.0f, 0.0f};
    float distance = 0.0f;
    bool hit = false;
};

// The SDK-facing facade — the EXACT surface a native game-module DLL may call. Every method is
// heap-safe across the engine's static-CRT (/MT) DLL boundary: scalars, raw float[3] buffers the
// caller owns, and STL params passed by const-ref that the engine only READS (never frees). No method
// here returns an STL object by value (that would cross-allocate/free between heaps). This is what
// NativeScript::api points at, so a module physically cannot reach a boundary-unsafe call.
class IScriptApi
{
public:
    virtual ~IScriptApi() = default;

    // --- entity ---
    virtual std::uint32_t FindEntityByName(const std::string& name) = 0;  // 0 = not found
    virtual bool EntityExists(std::uint32_t id) = 0;

    // --- transform (rotation in Euler DEGREES) ---
    virtual void GetPosition(std::uint32_t id, float out[3]) = 0;
    virtual void SetPosition(std::uint32_t id, const float p[3]) = 0;
    virtual void GetRotation(std::uint32_t id, float out[3]) = 0;
    virtual void SetRotation(std::uint32_t id, const float r[3]) = 0;
    virtual void GetScale(std::uint32_t id, float out[3]) = 0;
    virtual void SetScale(std::uint32_t id, const float s[3]) = 0;

    // --- input / time ---
    virtual bool IsKeyDown(ScriptKey key) = 0;
    virtual void GetMouseDelta(float& dx, float& dy) = 0;
    virtual double GetDeltaTime() = 0;     // seconds since last frame
    virtual double GetElapsedTime() = 0;   // seconds since Play started

    // --- audio ---
    virtual void PlayOneShot(const std::string& clipAssetId) = 0;

    // --- log ---
    virtual void Log(const std::string& msg) = 0;
    virtual void LogError(const std::string& msg) = 0;

    // --- spawn / destroy (DEFERRED: applied after the per-frame script OnUpdate loop). Spawn returns a
    //     real entity id immediately (the entity is a "ghost" until the deferred apply this frame). ---
    virtual std::uint32_t SpawnMesh(const std::string& meshAssetId, float x, float y, float z) = 0;
    virtual std::uint32_t SpawnPrefab(const std::string& prefabAssetId, float x, float y, float z) = 0;
    virtual void DestroyEntity(std::uint32_t id) = 0;

    // --- physics query (synchronous). RaycastHit is POD, returned BY VALUE (/MT-safe). ---
    virtual RaycastHit Raycast(float ox, float oy, float oz,
                               float dx, float dy, float dz, float maxDist) = 0;

    // --- animator parameters (no-op if the entity has no bound animator) ---
    virtual void SetAnimatorFloat(std::uint32_t id, const std::string& name, float value) = 0;
    virtual void SetAnimatorBool(std::uint32_t id, const std::string& name, bool value) = 0;
    virtual void SetAnimatorTrigger(std::uint32_t id, const std::string& name) = 0;

    // (NEVER add an STL-by-value return here — use a caller-owned char* buffer for strings to keep the
    //  /MT module boundary safe. By-value RaycastHit is fine: it is POD, no heap.)
};

// The full engine-internal facade: adds the methods that return an STL object by value (heap-unsafe
// across a /MT DLL boundary, so deliberately NOT on IScriptApi). Only the engine and the in-process Lua
// backend call these — never a native module DLL. ScriptApiImpl implements this; the Lua backend holds
// a ScriptApi* (not IScriptApi*) so it can reach GetEntityName / LoadScriptSource.
class ScriptApi : public IScriptApi
{
public:
    virtual std::string GetEntityName(std::uint32_t id) = 0;

    // UTF-8 source text of a .lua asset, or "" if unresolvable. apps/client reads the file through the
    // engine's stream abstraction; libs/script never touches the filesystem or the asset DB.
    virtual std::string LoadScriptSource(const std::string& assetId) = 0;
};

} // namespace ixscript
