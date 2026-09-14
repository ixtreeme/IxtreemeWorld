# Graphics/Renderer — live renderers (Phase 1: preserved, behavior unchanged)

Renderers that will depend on IXRHI in Phase 2 (exact audited names):

- StaticMeshRenderer (libs/render/StaticMeshRenderer.* + libs/mesh_system/*, shaders/StaticMesh.hlsl)
- SkinnedMeshRenderer (libs/render/SkinnedMeshRenderer.* — Ozz sampling — shaders/SkinnedMesh.hlsl + SkinnedMeshSkin.hlsl CSMain)
- TerrainRenderer (libs/render/TerrainRenderer.*, shaders/Terrain.hlsl)
- Water rendering (libs/render/WaterBodyIO.* + TerrainRenderer water passes, shaders/Water.hlsl)
- SelectionOutlineRenderer (libs/render/SelectionOutlineRenderer.* + libs/selection_system/*, shaders/SelectionOutline.hlsl)
- WorldLabelRenderer (libs/render/WorldLabelRenderer.*, shaders/WorldLabel.hlsl)
- OffscreenSceneRenderer (libs/render/OffscreenSceneRenderer.* — scene color/depth for refraction + Scene/Game view)
- Shadow rendering (shaders/ShadowDepth.hlsl, inside StaticMesh/Terrain passes)
- CubeRenderer (libs/render/CubeRenderer.*, shaders/Cube.hlsl — debug/smoke)
- Composite (shaders/Composite.hlsl), RmlUi (libs/render/RmlUiLayer.*, shaders/RmlUi.hlsl)

Phase 1: only relocation-safe dependency cleanup (none required — no moves).
Phase 2: see docs/architecture/phase2-ixrhi-scope.md for the per-class migration inventory.
