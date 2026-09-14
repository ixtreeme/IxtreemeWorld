# Ixtreeme Engine — module map (Phase 1)

The engine provides CAPABILITIES. The game provides POLICY. No game-specific
concept may leak into generic engine modules.

```
Core            — libs/debug, libs/math, libs/common, libs/platform (OS/window/process/time/paths)
                    Engine/Core/Core.h documents the rule. EXCEPTION (Phase 2 moves it):
                    libs/platform/VulkanDevice.* is really Graphics/Vulkan.
ECS             — Engine/ECS/EcsWorlds.h — EditorWorld vs ClientWorld seam (Flecs preserved,
                    no EnTT, no custom wrapper). No live flecs::world yet; SceneData vectors remain.
Graphics/IXRHI  — Engine/Graphics/IXRHI/ (IXRHI.h contract + IXRHIBuffer/Texture/
                    Shader/Pipeline/Binding/CommandList/Swapchain/Device/Sync/Types/Capabilities).
Graphics/Vulkan — Engine/Graphics/Vulkan/IXVulkan.h (RESERVED) + live libs/platform/VulkanDevice.*.
Graphics/Renderer — live libs/render/*Renderer.* (Static/Skinned/Terrain/Water/Selection/WorldLabel/
                    Offscreen/Cube) — behavior unchanged in Phase 1.
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
                    GameModule) + live libs/render/RuntimeSession.* (stays put in Phase 1, see its header).
```

Dependency flow: Core <- ECS/Assets/Physics/Animation/Audio/UI <- World/Graphics/Runtime <- Editor.
Game projects depend on Engine/Runtime. Engine NEVER depends on a game project or Editor.
