#pragma once

// apps/client's implementation of the script facade. It is the ONLY place script -> engine calls are
// realized, against the real Play-time state (entities, input, audio). RunGame wires the pointers/
// callbacks once and refreshes the per-frame scalars before each frame's script OnUpdate loop.

#include "ScriptApi.h"
#include "MapEditorTypes.h"  // MeshSceneEntity
#include "platform/tcp_stream.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ixaudio { class AudioEngine; }
struct MovementInputState;
class RmlUiLayer;

class ScriptApiImpl final : public ixscript::ScriptApi
{
public:
    // --- wired once by RunGame ---
    std::vector<MeshSceneEntity>* meshes = nullptr;
    std::function<void(MeshSceneEntity&)> syncMesh;                  // syncStaticMeshSpatialEntity
    std::function<void()> markDirty;                                 // SceneManager::MarkDirty
    ixaudio::AudioEngine* audio = nullptr;
    std::function<std::string(const std::string&)> resolveAudioClip;  // EditorImGui::AudioClipFilePath
    std::function<std::string(const std::string&)> resolveScriptSource; // .lua asset id -> UTF-8 source

    // Arrow keys + mouse buttons for scripts (NOT in MovementInputState, which the fly-camera/character
    // controller consume). Filled by the engine from input events each Play frame.
    struct ScriptInputState
    {
        bool up = false, down = false, left = false, right = false;
        bool mouseLeft = false, mouseRight = false;
        bool q = false, e = false, r = false, f = false;
        bool num[5] = {false, false, false, false, false};  // 1..5
    };

    // --- refreshed each frame before the OnUpdate loop ---
    double deltaSeconds = 0.0;
    double elapsedSeconds = 0.0;
    const MovementInputState* movement = nullptr;
    const ScriptInputState* input = nullptr;
    float mouseDx = 0.0f;
    float mouseDy = 0.0f;

    // ixscript::ScriptApi
    std::uint32_t FindEntityByName(const std::string& name) override;
    bool EntityExists(std::uint32_t id) override;
    std::string GetEntityName(std::uint32_t id) override;
    void GetPosition(std::uint32_t id, float out[3]) override;
    void SetPosition(std::uint32_t id, const float p[3]) override;
    void GetRotation(std::uint32_t id, float out[3]) override;
    void SetRotation(std::uint32_t id, const float r[3]) override;
    void GetScale(std::uint32_t id, float out[3]) override;
    void SetScale(std::uint32_t id, const float s[3]) override;
    bool IsKeyDown(ixscript::ScriptKey key) override;
    void GetMouseDelta(float& dx, float& dy) override;
    double GetDeltaTime() override;
    double GetElapsedTime() override;
    void PlayOneShot(const std::string& clipAssetId) override;
    std::string LoadScriptSource(const std::string& assetId) override;
    void Log(const std::string& msg) override;
    void LogError(const std::string& msg) override;
    std::uint32_t SpawnMesh(const std::string& meshAssetId, float x, float y, float z) override;
    std::uint32_t SpawnPrefab(const std::string& prefabAssetId, float x, float y, float z) override;
    void DestroyEntity(std::uint32_t id) override;
    ixscript::RaycastHit Raycast(float ox, float oy, float oz,
                                 float dx, float dy, float dz, float maxDist) override;
    void SetAnimatorFloat(std::uint32_t id, const std::string& name, float value) override;
    void SetAnimatorBool(std::uint32_t id, const std::string& name, bool value) override;
    void SetAnimatorTrigger(std::uint32_t id, const std::string& name) override;
    std::uint32_t NetConnect(const std::string& host, std::uint32_t port) override;
    int NetState(std::uint32_t handle) override;
    bool NetSend(std::uint32_t handle, const std::uint8_t* data, std::uint32_t size) override;
    std::uint32_t NetReceive(std::uint32_t handle, std::uint8_t* out, std::uint32_t capacity) override;
    void NetClose(std::uint32_t handle) override;
    std::uint32_t PromptText(const std::string& title, const std::string& label, bool secret) override;
    int PromptResult(std::uint32_t promptId, char* out, std::uint32_t capacity) override;
    void SetMaterial(std::uint32_t id, std::uint32_t slot, const std::string& materialAssetId) override;
    bool GetCharacterState(std::uint32_t id, ixscript::CharacterState& out) override;
    void SetCharacterAbilities(std::uint32_t id, bool canRun, bool canJump) override;
    std::uint32_t UiOpen(const std::string& documentPath) override;
    void UiClose(std::uint32_t document) override;
    void UiSetVisible(std::uint32_t document, bool visible) override;
    void UiSetText(std::uint32_t document, const std::string& elementId, const std::string& text) override;
    void UiSetProperty(std::uint32_t document, const std::string& elementId, const std::string& property,
                       const std::string& value) override;
    void UiSetClass(std::uint32_t document, const std::string& elementId, const std::string& className,
                    bool enabled) override;
    bool UiConsumeClick(std::uint32_t document, const std::string& elementId) override;
    void ParticlePlay(std::uint32_t id) override;
    void ParticleStop(std::uint32_t id) override;
    void ParticleRestart(std::uint32_t id) override;
    void ParticleEmit(std::uint32_t id, std::uint32_t count) override;

    ixscript::RaycastHit RaycastFiltered(float ox, float oy, float oz, float dx, float dy, float dz,
        float maxDistance, const ixscript::QueryFilter* filter) override;
    std::uint32_t OverlapSphere(float x, float y, float z, float radius,
        const ixscript::QueryFilter* filter, ixscript::SphereOverlapHit* output,
        std::uint32_t capacity, std::uint32_t* truncated) override;
    void SetEntityEnabled(std::uint32_t id, bool enabled) override;
    std::function<ixscript::RaycastHit(const float*, const float*, float, const ixscript::QueryFilter&)> raycastFiltered;
    std::function<std::vector<ixscript::SphereOverlapHit>(const float*, float, const ixscript::QueryFilter&)> overlapSphere;

    // --- game UI + character controller, wired once by RunGame ---
    RmlUiLayer* gameUi = nullptr;
    // A document path relative to the project's asset folder -> the file to load ("" = not found).
    std::function<std::string(const std::string&)> resolveUiDocument;
    std::function<bool(std::uint32_t, ixscript::CharacterState&)> getCharacterState;
    std::function<void(std::uint32_t, bool, bool)> setCharacterAbilities;
    // Particle System actions (wired once by RunGame; no-ops when unset).
    std::function<void(std::uint32_t)> particlePlay;
    std::function<void(std::uint32_t)> particleStop;
    std::function<void(std::uint32_t)> particleRestart;
    std::function<void(std::uint32_t, std::uint32_t)> particleEmit;

    // --- script text prompts, drawn by the host UI (the editor) ---
    struct Prompt
    {
        std::string title;
        std::string label;
        bool secret = false;
        int status = 0;  // 0 open, 1 submitted, -1 cancelled
        std::string text;
    };
    // True only while a host draws prompts (the editor); otherwise PromptText answers 0.
    bool promptsAvailable = false;
    const std::map<std::uint32_t, Prompt>& Prompts() const { return m_prompts; }
    void SubmitPrompt(std::uint32_t id, const std::string& text);
    void CancelPrompt(std::uint32_t id);
    // Play stop: closes every script stream and forgets (wipes) every prompt.
    void ResetTransportAndPrompts();

    // --- deferred spawn/destroy queue (drained by the engine after the script OnUpdate loop) ---
    enum class DeferredKind { SpawnMesh, SpawnPrefab, Destroy, SetMaterial, SetEnabled };
    struct DeferredOp
    {
        DeferredKind kind;
        std::string assetId;
        float pos[3] = {0.0f, 0.0f, 0.0f};
        std::uint32_t id = 0;  // pre-allocated for spawns; target for destroy
        bool enabled = true;
        std::uint32_t slot = 0;  // SetMaterial: material slot (assetId = the material)
    };
    std::vector<DeferredOp> deferredOps;

    // --- wired once by RunGame (the rich API needs more engine reach) ---
    std::uint32_t* nextEntityId = nullptr;  // shared entity-id allocator (pre-alloc for spawn)
    enum class AnimatorParamType { Float, Bool, Trigger };
    std::function<void(std::uint32_t, const std::string&, AnimatorParamType, float, bool)> setAnimatorParam;
    // origin[3], dir[3](normalized), maxDist -> writes outId/outPoint[3]/outNormal[3]/outDist, returns hit.
    std::function<bool(const float[3], const float[3], float, std::uint32_t&, float[3], float[3], float&)> raycast;

private:
    MeshSceneEntity* Find(std::uint32_t id);
    std::map<std::uint32_t, std::unique_ptr<platform::TcpStream>> m_streams;
    std::uint32_t m_nextStream = 1;
    std::map<std::uint32_t, Prompt> m_prompts;
    std::uint32_t m_nextPrompt = 1;
};
