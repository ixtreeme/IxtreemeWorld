# Ixtreeme Engine — module map (Phases 1–3C)

The engine provides CAPABILITIES. The game provides POLICY. No game-specific
concept may leak into generic engine modules.

```
Core            — libs/debug, libs/math, libs/common, libs/platform (OS/window/process/time/paths).
                    libs/platform/VulkanDevice.* keeps device/queue/swapchain-handle
                    infrastructure + backend-synced migration shims; its frame loop
                    is deleted (Phase 3C). See docs/architecture/ixrhi-frame-lifecycle.md.
ECS             — Engine/ECS/EcsWorlds.h — EditorWorld vs ClientWorld seam (Flecs preserved,
                    no EnTT, no custom wrapper). No live flecs::world yet; SceneData vectors remain.
Graphics/IXRHI  — Engine/Graphics/IXRHI/ (contract + Buffer/Texture/Shader/Pipeline/
                    Binding/CommandList/Swapchain/Device/Sync/Types/Capabilities/Frame/
                    Query/RenderTarget/RenderPass). Owns the graphics frame contract.
Graphics/Vulkan — Engine/Graphics/Vulkan/ (IXVulkanDevice frame authority + swapchain,
                    targets, editor adapter, surface, conversions). Owns acquisition,
                    recording, submission, presentation, sync, timestamps.
Graphics/Renderer — libs/render/*Renderer.* — Static/SelectionOutline/WorldLabel/
                    OffscreenSceneRenderer are IXRHI-native; Skinned/Terrain/Water/
                    RmlUi remain native (inventoried shims).
Editor/Graphics — Editor/Graphics/ (EditorGraphicsBridge registry + provider
                    interface). Backend adapter lives in Engine/Graphics/Vulkan/.
World           — Engine/World/World.h — Terrain/Water/Spatial ownership + SpatialIndex LOCAL-ONLY policy.
Assets          — libs/asset (AssetDatabase GUID identity, AssetLibrary, AssetWatcher, ProjectManager,
                    MaterialAssetManager) — engine-owned. Asset Browser (editor_panels) is Editor-only.
Animation       — libs/animation (Ozz) + libs/render/SkinnedMeshRenderer sampling — unchanged.
Physics         — libs/physics (PhysicsWorld abstraction + Jolt backend). Character controller
                    (UpdateCharacterController in EngineApplication.cpp) is DEMO/editor-Play-specific,
                    NOT generic low-level physics — it stays in the app layer, documented in
                    docs/architecture/phase1-engine-application.md.
Audio           — libs/audio (miniaudio) — unchanged.
UI              — libs/render/RmlUiLayer.* (engine capability) vs Hierarchy/Inspector/Build Window (Editor)
                    vs future Auriga HUD (game project).
Input           — libs/platform/InputEvent.h + apps/client ViewportControls — unchanged.
Scripting       — libs/script (Lua/Native, IxModuleApi) + sdk/include — unchanged.
Networking      — NOT implemented. Seam = RuntimeSession::UpdateNetwork/SendMoveInput/SendAttackTarget
                    (stubs). No Boost.Asio in Phase 1.
Serialization   — libs/render/SceneManager (JSON + sidecars) + libs/prefab/PrefabDocument — unchanged.
Runtime         — Engine/Runtime/ClientRuntime.h (RESERVED ClientApplication/ClientWorld/Presentation/
                    GameModule) + live libs/render/RuntimeSession.* (stays put, see its header).
```

Dependency flow: Core <- ECS/Assets/Physics/Animation/Audio/UI <- World/Graphics/Runtime <- Editor.
Game projects depend on Engine/Runtime. Engine NEVER depends on a game project or Editor.
Frame flow: Application -> IXRHIDevice::BeginFrame -> IXRHIFrame -> renderers -> EndFrame.
