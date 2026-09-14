#pragma once

// ECS ownership boundary (Phase 1).
//
// Current truth (audited): the engine links Flecs (flecs::flecs_static) but the
// only `#include <flecs.h>` is apps/client/src/EngineApplication.cpp and there is
// NO live flecs::world yet. Scene state is the custom vector-based SceneData
// (libs/render/SceneManager.h: SceneData with cameras/lights/terrain/water/meshes).
//
// Preserved decisions:
// - Keep Flecs. Do NOT introduce EnTT. Do NOT build a custom ECS wrapper framework.
// - Common ECS helpers / component registration utilities may live here later.
//
// Critical rule established in this phase:
//   EditorWorld  (flecs::world) — Selected/Locked/Hidden/Dirty/Gizmo/EditorOnly
//   ClientWorld  (flecs::world) — NetworkIdentity/Transform/RenderTransform/
//                                 Interpolation/Appearance/Health/AnimationState/
//                                 Targetable/AudioEmitter
// These are NOT the same world. Nothing in this phase may assume
// "editor hierarchy world == game runtime world".
//
// This header is intentionally dependency-free (no flecs include) so Core and
// low-level modules can reference the seam without pulling in the ECS runtime.

namespace ixengine::ecs
{

// Opaque tags marking which world a system operates on. Phase 2 will replace the
// using-aliases below with real flecs::world owners (one per world).
struct EditorWorldTag
{
};
struct ClientWorldTag
{
};

} // namespace ixengine::ecs
