#pragma once

// The language-agnostic SEAM between scripts and the engine. libs/script defines this pure-virtual
// facade; apps/client implements it (ScriptApiImpl) against the real engine state (entities, input,
// audio, ...). ALL backends (AngelScript, Lua, legacy native C++) call the engine ONLY through here —
// so a script can never reach past this surface.
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
    MouseLeft, MouseRight,
    // v6: action keys (appended, so the values above keep their numbers)
    Q, E, R, F,
    Num1, Num2, Num3, Num4, Num5
};

// String -> ScriptKey, the single source of truth for the script-facing key names. Shared by the
// backends so Lua and AngelScript accept the exact same strings. Case-sensitive; an unknown name
// yields false (the caller answers false from IsKeyDown rather than erroring).
inline bool ParseKeyName(const std::string& name, ScriptKey& out)
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
    if (name == "Q") { out = ScriptKey::Q; return true; }
    if (name == "E") { out = ScriptKey::E; return true; }
    if (name == "R") { out = ScriptKey::R; return true; }
    if (name == "F") { out = ScriptKey::F; return true; }
    if (name == "Num1") { out = ScriptKey::Num1; return true; }
    if (name == "Num2") { out = ScriptKey::Num2; return true; }
    if (name == "Num3") { out = ScriptKey::Num3; return true; }
    if (name == "Num4") { out = ScriptKey::Num4; return true; }
    if (name == "Num5") { out = ScriptKey::Num5; return true; }
    return false;
}

// What a player CharacterController did in its last simulation step (v6). POD: safe by value across
// the /MT module boundary.
struct CharacterState
{
    bool grounded = false;
    bool moving = false;       // horizontal movement input this step
    bool running = false;      // moving at run speed (Shift held and running allowed)
    bool jumped = false;       // a jump started this step
    float planarSpeed = 0.0f;  // m/s
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

    // --- generic TCP transport (v4). A byte stream only: no framing, no protocol -- a game's network
    //     protocol lives in its scripts. Non-blocking; the engine polls the stream on every call.
    //     Every stream is closed when Play stops. ---
    // Starts a connect to host:port (DNS name or IP literal). Returns a handle, 0 if it cannot start.
    virtual std::uint32_t NetConnect(const std::string& host, std::uint32_t port) = 0;
    // 0 = connecting, 1 = connected, 2 = closed by the peer / NetClose, 3 = failed or unknown handle.
    virtual int NetState(std::uint32_t handle) = 0;
    // Queues bytes (caller-owned buffer, read only). False once the stream is closed/failed.
    virtual bool NetSend(std::uint32_t handle, const std::uint8_t* data, std::uint32_t size) = 0;
    // Moves up to `capacity` received bytes into the caller's buffer; returns how many.
    virtual std::uint32_t NetReceive(std::uint32_t handle, std::uint8_t* out, std::uint32_t capacity) = 0;
    virtual void NetClose(std::uint32_t handle) = 0;

    // --- text prompt (v4): an engine-drawn dialog asking the player for one line of text (`secret`
    //     masks the input, e.g. a password). Returns a prompt id (0 = prompts unavailable). ---
    virtual std::uint32_t PromptText(const std::string& title, const std::string& label, bool secret) = 0;
    // 0 = still open, 1 = submitted (UTF-8 copied into `out`, NUL-terminated, truncated to capacity-1;
    // the engine then forgets the text), -1 = cancelled / unknown id.
    virtual int PromptResult(std::uint32_t promptId, char* out, std::uint32_t capacity) = 0;

    // --- rendering (v5) ---
    // Assigns a material asset (its GUID as stored in a scene's "materials" slots, or its asset id) to
    // material slot `slot` of the entity's mesh. DEFERRED like spawn/destroy, so it also applies to an
    // entity spawned earlier in the same frame. An unknown entity or material is a logged no-op.
    virtual void SetMaterial(std::uint32_t id, std::uint32_t slot, const std::string& materialAssetId) = 0;

    // --- character controller (v6) ---
    // The last step of the entity's player CharacterController; false when it has none (or Play is off).
    virtual bool GetCharacterState(std::uint32_t id, CharacterState& out) = 0;
    // What the controller may do from now on: run (Shift) and jump. Both are allowed when Play starts.
    virtual void SetCharacterAbilities(std::uint32_t id, bool canRun, bool canJump) = 0;

    // --- game UI (v6): RmlUi documents (.rml + .rcss) from the project's asset folder ---
    // Opens and shows a document (path relative to the asset folder, e.g. "ui/hud.rml") over the game
    // (the Game view in the editor). Returns its handle, 0 if it cannot be loaded. Every document closes
    // when Play stops.
    virtual std::uint32_t UiOpen(const std::string& documentPath) = 0;
    virtual void UiClose(std::uint32_t document) = 0;
    virtual void UiSetVisible(std::uint32_t document, bool visible) = 0;
    // An element by its id attribute: its text, one RCSS property ("width", "62%") or a class on/off.
    virtual void UiSetText(std::uint32_t document, const std::string& elementId, const std::string& text) = 0;
    virtual void UiSetProperty(std::uint32_t document, const std::string& elementId, const std::string& property,
                               const std::string& value) = 0;
    virtual void UiSetClass(std::uint32_t document, const std::string& elementId, const std::string& className,
                            bool enabled) = 0;
    // True once per click on the element (or anything inside it) since the last call.
    virtual bool UiConsumeClick(std::uint32_t document, const std::string& elementId) = 0;

    // --- particles (v7): the entity's Particle System component ---
    // Starts/resumes emission (creates the emitter's simulator if it has not started). No-op when
    // the entity has no Particle System component. Live particles from before a Stop keep going
    // until their lifetimes end.
    virtual void ParticlePlay(std::uint32_t id) = 0;
    // Stops emitting; the live particles finish. The emitter stays stopped (a later ParticlePlay
    // resumes it without clearing).
    virtual void ParticleStop(std::uint32_t id) = 0;
    // Clears the live particles and restarts emission (with the component's burst).
    virtual void ParticleRestart(std::uint32_t id) = 0;
    // Spawns `count` particles immediately, even while emission is stopped.
    virtual void ParticleEmit(std::uint32_t id, std::uint32_t count) = 0;

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
