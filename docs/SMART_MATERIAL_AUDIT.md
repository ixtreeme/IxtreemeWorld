# Smart Material repository audit

Audit date: 2026-10-11. This records the starting working tree, including the user's
uncommitted Material Graph implementation. C++20 is required by `Client/CMakeLists.txt`.
No `AGENTS.md` was found in the workspace. Existing changes must be retained.

## Architecture and responsibilities

| Subsystem | Starting status | Evidence and responsibility |
| --- | --- | --- |
| `MaterialGraphDocument` | Implemented; needs extension | UI-independent nodes, typed pins, links, shared 24-bit ID allocator, v1/v2 JSON, transactional load and whole-graph cycle validation. |
| `MaterialGraphProgram` | Implemented CPU/export; runtime integration missing | Validates a document, finds reachable nodes, emits topological `GraphInstruction` IR; CPU evaluation and HLSL consume the same instructions. Dead node elimination exists. Constant folding/CSE/variant cache do not. |
| `MaterialGraphPanel` | Implemented editor | ImGui Node Editor canvas, Inspector, snapshot Undo/Redo, atomic Windows save, presets, preview and UV publish. Exposed parameter overrides are session-only. |
| `RenderMaterialPreview` | Implemented CPU | Sphere/plane contexts, channel output, simple PBR shading, tangent-frame transform and RGBA8 upload. It is not GPU graph evaluation. |
| `GraphTextureCache` | Implemented, partially integrated | GUID/library-ID/path resolution, PNG-family and EXR decoding, bilinear repeat, bounded memory, explicit missing-texture error. Library revision/manual reload invalidate it; file-content dependency invalidation is incomplete. |
| `GenerateHlsl` | Implemented export | HLSL context/surface/helpers, parameter cbuffer and texture declarations. No runtime compiler/reflection/resource-binding owner consumes this source. |
| `PublishBakedMaterial` | Implemented UV path | CPU evaluates UV-only graph; writes color/normal/AO/roughness/metallic/height/emissive, editable source and real `.material` via the existing manager. World/geometry dependence is rejected before export. |
| Existing graph tests | Implemented; baseline needs rebuilding | Compiler tests execute CPU graphs and invoke actual DXC for DXIL/SPIR-V when configured; editor tests simulate ImGui mouse/key events; baking tests create/read real assets. None measures GPU output parity. The old build initially contained no registered graph tests. |
| `AssetLibrary` / `AssetDatabase` | Implemented existing asset path | Manifest entries, `.meta` GUIDs, move tracking, revision, GUID resolution and `.meta` dependency read/write. Smart material/subgraph/generator/instance types are absent. |
| `MaterialAssetManager` | Implemented traditional materials | GUID-backed texture references, material revision, ORM packing, scalar strengths. Height is retained but the mesh shader does not sample it. |
| Mesh import/render data | Partially suitable for geometry baking | Imported positions/normals/UV reach `StaticMeshRenderer::Vertex`; it has no tangent or vertex-color field. `StaticMesh.hlsl` derives a tangent frame from derivatives. Explicit persistent bake mesh ownership is absent. |
| Object material overrides | Partial instance foundation | `MeshSceneEntity::MaterialOverride` stores per-slot color/PBR/UV strengths; it does not reference a graph definition or generator parameters. |
| IXRHI/Vulkan | Implemented abstraction and backend | `IXRHIDevice` creates SPIR-V shaders, buffers, textures, binding groups and pipelines. Generic renderer interfaces are IXRHI-based. Editor registry uses weak resources and opaque handles. |
| Shader compiler/cache | Build compiler implemented; dynamic graph path missing | CMake locates DXC and builds fixed HLSL variants. `IXRHIShaderDesc` consumes SPIR-V, has no reflection. Renderer owns fixed pipelines; no dynamic material variant cache or graph failure-recovery path was found. |
| PBR runtime | Implemented traditional material path | Static mesh reads diffuse/normal/ORM, instance buffer values and alpha/shadow variants; terrain has eight splat-driven layers. These do not call `EvaluateMaterialGraph`. |
| Geometry baker | Missing | AO/curvature/cavity graph nodes read explicitly supplied context scalars. Preview sliders are test inputs; neither those nor the constant defaults are mesh baking. |
| Layer stack / smart generators / subgraphs | Missing | Material Layer/Layer Blend/Height Blend nodes exist, but there is no ownership metadata, canonical layer list or reusable generator/subgraph asset. |

## Material data flow

Document -> validation/type and cycle checks -> reachable topological instructions ->
CPU evaluation -> preview RGBA8 or UV bake -> traditional material asset -> fixed
runtime shader. The separate HLSL export branch ends at source/DXC validation.
It does not currently reach runtime draw submission.

Preview parameters are name-keyed maps; HLSL uses node-ID-named `float4` cbuffer
fields. There is no public layout/packing contract connecting these representations.
The document/compiler/images library is `IXMaterialGraph` and has no ImGui/Vulkan
dependency; asset publish/cache and the panel are editor translation units.

## Geometry and texture semantics

World position is in world units; slope uses geometric world normal and +Y up.
Material normal outputs are tangent-space. RGB decoding is explicit, raw texture
channels and normal output bypass sRGB. EXR indicates scene-linear RGB. Triplanar
supports color/data only; it does not transform sampled tangent normals.

UV baking currently has no mesh UV rasterizer, island padding, overlap diagnostic,
topology check, ray-based AO/thickness, mesh cache or cancellation. Curvature and
cavity must become separate mesh-derived data, never guesses from constant AO.

## Risks and implementation order

1. Preserve v1/v2 files and node numeric enum values. Add versions explicitly and
   make malformed/future versions fail transactionally.
2. Extend the existing document as the canonical material definition, never add a
   second evaluator. Stable ownership must make managed topology edits explicit.
3. Publish a parameter/resource layout from the existing IR; instances share an
   immutable definition/program and store only overrides. Keep dynamic data out
   of shader identity; validate missing/wrong-type parameters.
4. Add foundation nodes in both CPU and HLSL and test actual DXC compilation.
5. Add managed layers and Weathered Stone as ordinary graph nodes with metadata;
   preserve custom nodes, serialize ownership, support undoable detachment.
6. Introduce an editor-independent geometry baker and explicit required-map
   diagnostics before claiming geometric Weathered Stone acceptance.
7. Add generator/subgraph assets and register smart assets with the existing GUID
   pipeline; track dependencies and migration.
8. Add runtime shader compilation/reflection and IXRHI resource/pipeline lifetime
   ownership, then actual GPU readback parity and failure recovery tests.
9. Add asynchronous/GPU baking and measured profiling after correctness.

The smallest full vertical slice is Stone Base + cavity/AO Dirt + slope/noise Moss,
one graph/layer document, saved instance overrides, real selected-mesh geometry
contexts and UV rasterization. Its CPU part can precede GPU runtime integration,
but it must not be called a completed GPU Smart Material framework.

Follow-up 2026-10-12: the canonical core/layers/instances and GUID-backed subgraph
expansion have since been implemented. This table remains the starting-state
audit. Current capabilities, verification and outstanding geometry/runtime work
are recorded in `SMART_MATERIAL_IMPLEMENTATION.md` and `SMART_MATERIAL_TESTS.md`.
