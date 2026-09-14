# EngineApplication decomposition (Phase 1)

`apps/client/src/EngineApplication.cpp` (~11k lines) is the highest-priority
architecture problem. Phase 1 extracts ONE coherent subsystem (the Build worker)
and documents the rest. No blind per-function class split — behavior parity first.

## Extracted in Phase 1

- Game-script cmake worker (~70 lines incl. thread state) -> `Editor/Build/BuildService.*`
  (`ixeditor::build::GameScriptBuildService`, lib `IXEngineEditorBuild`).
  - `EngineApplication.cpp` no longer includes `platform/process.h` / calls `RunProcess`.
  - Frame loop only calls `RequestBuild()` / `TryTakeResult()` / `IsRunning()` / `Shutdown()`.
  - cmake argv, log merging, missing-cmake message, CRT-config parity: byte-identical behavior.

## Categorized responsibilities (still inside EngineApplication.cpp, by design in Phase 1)

- APPLICATION LIFECYCLE — `RunIxtreemeEngine`/`RunGame`, `ResolveEngineAssetRoot`,
  `StartupSceneFromConfig`, `LoadRuntimeScene`, shutdown joins.
- EDITOR — `EditorImGui` frame/panels, `SceneManager` snapshot/pending, play-mode state machine,
  `EnsureProjectScriptsScaffold` / `LoadProjectGameModules` / `UnloadGameModules` triggers.
- RUNTIME — `RuntimeSession` (empty stub) start/tick/stop, `RuntimeUiAdapter` (null), RmlUi routing.
- PHYSICS — `PhysicsWorld` step/kinematic sync, collider debug-draw builders, raycast/overlap queries.
- INPUT — `ViewportControls` fly camera + `MovementInputState` routing (editor vs Play).
- SCENE — `BuildSceneSnapshot`/`ApplySceneData` (`EditorSceneRuntime`), hierarchy entities,
  `rebuildEditorPhysicsWorld`, water/light/camera editor states.
- RENDER — `VulkanDevice`, all `*Renderer.Create/SetMainRenderPass/RecreatePipeline`, asset library init.
- ANIMATION — `ixanim::AnimatorRuntime` per-entity maps, Ozz sampling via `SkinnedMeshRenderer`.
- SELECTION — `SelectionSystem` outline lines, gizmo apply, picking.
- CHARACTER CONTROLLER — `UpdateCharacterController` + `CharacterRuntimeState`.
  DECISION: this is DEMO/editor-Play-specific (WASD + camera-relative + Jolt kinematic sync for
  Play testing), NOT generic low-level physics. It stays in the app layer. A future generic
  kinematic controller (if Auriga needs one) belongs in `Engine/Physics/` or `Engine/Runtime/`,
  designed from game requirements — not promoted from this demo code.
- HIERARCHY — raw `ecs_*` C-API world (`editorHierarchyWorld`, scene root, note component).
  NOTE: this is the Flecs **C API**, not `flecs::world` C++. EditorWorld vs ClientWorld separation
  (`Engine/ECS/EcsWorlds.h`) starts here: this world is EditorWorld. ClientWorld does not exist yet.
- TOOLS — terrain sculpt raycast/brush, water sculpt, tree generator wiring, GPU capture toggles.

## Factories preserved (runtime seams, unchanged)

- `CreateRuntimeSession()` -> `CreateEmptyRuntimeSession()` (libs/render/RuntimeSession.*).
- `CreateRuntimeUiAdapter()` -> null adapter. Future net seam: replace these factories;
  do NOT branch MMO logic inside EngineApplication.

## Next extractions (Phase 2+, in order)

1. `EditorSceneRuntime` already exists — grow it until `Build/ApplySceneSnapshot` leaves RunGame.
2. Character controller -> `apps/client` subsystem file (still app-layer, not Engine/Physics).
3. Physics debug-draw builders -> Editor debug overlay (they pull editor selection state).
4. Boot/path helpers (`ExecutableDirectory`/`FindClientRootNear`/`ResolveEngineAssetRoot`) ->
   `Engine/Core/Filesystem` once a Core lib exists (today Core is header-boundary only).
