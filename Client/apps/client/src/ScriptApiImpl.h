#pragma once

// apps/client's implementation of the script facade. It is the ONLY place script -> engine calls are
// realized, against the real Play-time state (entities, input, audio). RunGame wires the pointers/
// callbacks once and refreshes the per-frame scalars before each frame's script OnUpdate loop.

#include "ScriptApi.h"
#include "MapEditorTypes.h"  // MeshSceneEntity

#include <functional>
#include <string>
#include <vector>

namespace ixaudio { class AudioEngine; }
struct MovementInputState;

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

    // --- deferred spawn/destroy queue (drained by the engine after the script OnUpdate loop) ---
    enum class DeferredKind { SpawnMesh, SpawnPrefab, Destroy };
    struct DeferredOp
    {
        DeferredKind kind;
        std::string assetId;
        float pos[3] = {0.0f, 0.0f, 0.0f};
        std::uint32_t id = 0;  // pre-allocated for spawns; target for destroy
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
};
