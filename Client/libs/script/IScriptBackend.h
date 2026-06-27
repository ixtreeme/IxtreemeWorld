#pragma once

// A backend (Native C++ / Lua / ...) creates live per-entity ScriptInstances and is the only thing
// the engine's Play loop talks to per language. ScriptSystem owns the backends and routes by
// ScriptComponent::backend.

#include "ScriptComponent.h"

#include <cstdint>
#include <memory>

namespace ixscript
{

class ScriptApi;

// One running script on one entity. The engine drives the lifecycle from the Play loop.
class ScriptInstance
{
public:
    virtual ~ScriptInstance() = default;
    virtual void OnStart() = 0;
    virtual void OnUpdate(float dtSeconds) = 0;
    virtual void OnDestroy() = 0;
    virtual void OnCollision(std::uint32_t otherEntityId) = 0;
};

class IScriptBackend
{
public:
    virtual ~IScriptBackend() = default;
    // Creates an instance for `comp` bound to `entityId` + `api`. Returns nullptr if the script can't
    // be resolved (unknown native class / unloadable Lua) — the caller skips it (never crashes).
    virtual std::unique_ptr<ScriptInstance> CreateInstance(std::uint32_t entityId,
                                                           const ScriptComponent& comp,
                                                           ScriptApi& api) = 0;
};

} // namespace ixscript
