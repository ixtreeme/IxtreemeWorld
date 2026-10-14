#pragma once

// The AngelScript scripting backend: runs .as ASSETS at runtime so a game built on the engine BINARY
// (no engine source) can drive entities. One asIScriptEngine for the whole Play session; each .as
// asset compiles to ONE module that must define `class Script`, and every scripted entity gets its
// own instance of that class (per-entity state lives in class MEMBERS — module globals are shared
// between the entities using the same asset). Every hook is execute-isolated: a script error logs
// once, marks that instance dead, and never crashes or spams the frame.
//
// Parameters + the entity id are auto-filled into same-named class members before OnStart (uint
// `id`/`entityId` for the entity; int/uint/float/double/bool/string members for the component's
// parameters). `Self()` and `Param("name")` cover dynamic access.
//
// The bound global functions mirror the Lua surface 1:1 (Find, GetPosition, SetPosition, IsKeyDown,
// Raycast, Spawn*, Ui*, Net*, ...) with AngelScript-idiomatic shapes: float3 RETURN values for
// positions/rotations/scales, and OUT params for the multi-value calls (MouseDelta(float2 &out),
// Raycast(..., RaycastHit &out), GetCharacterState(..., CharacterState &out), PromptResult(id, text)).
//
// Engine capabilities arrive exclusively through the ScriptApi facade; this file never sees the
// filesystem or the asset DB (.as SOURCE TEXT comes from ScriptApi::LoadScriptSource).

#include "IScriptBackend.h"

#include <memory>
#include <string>

namespace ixscript
{

class ScriptApi;

class AngelScriptBackend final : public IScriptBackend
{
public:
    AngelScriptBackend();
    ~AngelScriptBackend() override;
    AngelScriptBackend(const AngelScriptBackend&) = delete;
    AngelScriptBackend& operator=(const AngelScriptBackend&) = delete;

    std::unique_ptr<ScriptInstance> CreateInstance(std::uint32_t entityId,
                                                   const ScriptComponent& comp,
                                                   ScriptApi& api) override;

    // Hot-reload: mark a .as asset's compiled module stale so the next CreateInstance rebuilds it
    // from fresh source. The old module stays alive until the backend is torn down (any still-live
    // instance's script object must remain valid); the caller recreates the live instances.
    void InvalidateSource(const std::string& assetId);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;  // owns the single asIScriptEngine
};

} // namespace ixscript
