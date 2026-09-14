#pragma once

// Runtime/Client seam (Phase 1: boundary reservation only).
//
// Current truth: libs/render/RuntimeSession.h defines the RuntimeSession interface
// (UpdateNetwork/SendMoveInput/SendAttackTarget/GetWorldEntities/WorldRenderEntity)
// with an EmptyRuntimeSession stub. It historically mixes editor-adjacent concepts
// (IsMapEditorOpen/GetMapEditorSettings/InitializeAssetLibrary/ImportDroppedFiles)
// with future MMO client concepts (network identity, interpolation, replication).
//
// Phase 1 decision (documented, not yet moved for build safety):
// - RuntimeSession STAYS functional where it is (libs/render/) in this phase.
// - Its long-term home is Engine/Runtime/ (this directory). A physical move happens
//   after EditorImGui stops calling editor-specific RuntimeSession methods.
// - Generic Engine Core must NEVER depend on MMO concepts (NetworkIdentity, guilds,
//   parties, combat). Those belong in the game project (Projects/Auriga/) or a
//   future MMO extension outside generic Engine.
// - Future client runtime: Runtime/ClientApplication + Runtime/ClientWorld +
//   Runtime/Presentation + Runtime/GameModule (stubs below, implemented post-IXRHI).
//
// Game code depending on Engine/Runtime is allowed. Engine depending on a game
// project is FORBIDDEN.

namespace ixengine::runtime
{

// Forward seam for the future client application object. The editor's
// EngineApplication (apps/client/) remains the only application in Phase 1.
class ClientApplication;

// Per-client game world (flecs::world in Phase 2+). Separate from EditorWorld.
class ClientWorld;

// Presentation layer (camera, HUD routing, world labels) driven by ClientWorld.
class Presentation;

// GameModule: engine-owned scaffold (sdk/include + IxModuleApi.h) compiled by the
// Editor Build pipeline into a project DLL. See Editor/Build/BuildService.
class GameModule;

} // namespace ixengine::runtime
