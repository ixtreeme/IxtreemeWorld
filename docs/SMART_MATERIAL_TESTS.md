# Smart Material validation

All tests use real production classes. Final validation date: 2026-10-12.

## Coverage and its limits

| Test | What is actually executed |
| --- | --- |
| `SmartMaterialCoreTest` | Canonical layers and ID ownership; opacity/channel blending/order/enable/duplication/removal; v3 round-trip and malformed/desynchronized data; explicit conversion; instance JSON/shared program/packing/error recovery; real atomic IO; 22 new node reference/error cases; Weathered Stone preview and geometry-dependent rejection. |
| `MaterialSubgraphTest` | Graph v4/wrapper v1, all six typed passthrough reference values, defaults, scoped/repeated/nested parameters, preview, UV bake, layer masks, deterministic source, missing assets/resolver, recursion, schema refresh, depth/call/node/ID bounds; actual expanded DXIL/SPIR-V compilation. |
| `MaterialGraphCompilerTest` | Existing CPU arithmetic, textures, HDR/sRGB, normals, layers, cycles/validation, 2048-node traversal, v1/v2 compatibility; all 69 node kinds and three presets compile using actual DXC to DXIL and SPIR-V when configured. Missing DXC is printed as SKIP rather than asserted as GPU validation. |
| `GeometryBakerTest` | Actual plane/convex and inward boxes/concave corner; barycentric maps, seam topology, AO/thickness references, independent curvature/cavity, nonuniform/reflected positions/normals/thickness, padding/source UV, deterministic seed, malformed/overlap cases, cancellation, portable corruption-safe codec and real shared-evaluator/material/raw map preview values; worker completion/cancel. |
| `SmartGeneratorTest` | Six editable preset wrappers/reference values, geometry/context needs, seed determinism/variation, amount/contrast/growth/height controls, gravity rotation anisotropy, zero sizes/directions and large allowed inputs, transactional errors, real geometry mask preview/material bake, canonical layers, repeated call scopes/shared instances; actual DXIL/SPIR-V for every expanded recipe. |
| `MaterialGraphEditorTest` | Production panel/headless ImGui and existing synthetic mouse/key workflow; stack/history; graph/instance/subgraph files, GUID rescan/moves, dependency metadata, changed/malformed/fixed subgraph preview, standalone output, scoped instances; full 256-asset cache and dependency-set replacement. Real OBJ selection/async geometry job/atlas preview/cache save, geometry-bound instance/dependencies and invalid cache preview/reload. New binding dialogs and layer drag-and-drop pixel interactions are not independently automated. |
| `MaterialGraphBakingTest` | Actual UV images, native material/ORM and manifest reload; texture imports/moves/renames, bilinear filtering and native GUID lookup with/without a library; missing GUID cannot select a stale path; subgraph authored/effective source snapshots and scoped overrides; geometry-dependent UV export refusal. Actual Assimp OBJ/geometry cache/native source dependency, mesh Weathered Stone images/material/exact cache snapshot, cache/model moves, corruption/recovery and stale source rejection before publication. |

The native baking test also covers existing Assets/Smart files, all six supported
smart extensions, case-insensitive discovery, folder/search queries, manifest
persistence, typed/direct imports with immediate independent GUIDs, and generator
rename/move resolution. The editor test verifies immediate browser visibility after
definition/instance/generator/geometry saves and guarded generator open requests.
The full editor build checks the actual browser double-click/context-menu route;
browser tile pointer interactions are not independently automated.

Asset Browser regression validation on 2026-10-12: full `IxtreemeEngine` Release
build succeeded; the eight-test editor/material selection below passed in 12.07 s.

Quick application tests run the production asynchronous panel pipeline with a real
OBJ, generator graph and scoped instance override, including edits during baking,
automatic publication without a saved input cache, renderer-matching world
positions under rotation/nonuniform scale, and cancellation without publication.
Native assignment checks reject changed models, poses, slots/overrides and different
entity IDs, and preserve unrelated slots/entity metadata. Unsaved geometry also
rejects changed source content before publication. GUI button clicks and an actual
GPU scene capture are not independently automated.

Quick application validation on 2026-10-12: full Release editor build succeeded,
the eight-test selection passed in 14.74 s, and `IXEngineRender` also built with
`IXTREEME_WITH_EDITOR=OFF` (the assignment/transform helpers require no ImGui).

UV repair regression coverage includes tiny exporter roundoff at both atlas borders,
real invalid/non-finite UV rejection, overlapping box UVs, generated seam topology,
atlas cancellation, the production asynchronous fallback, and source edits before
assignment. Reported-model regression modes copy the supplied input into an isolated
temporary project and run both submesh publication and the one-action panel path:

```powershell
build/client-gui-check/libs/render/Release/MaterialGraphBakingTest.exe --model-under-test D:/AkitaOnline/Assets/BlackwaterHollow/Models/LanternPost.glb
build/client-gui-check/libs/render/Release/MaterialGraphEditorTest.exe --model-under-test D:/AkitaOnline/Assets/BlackwaterHollow/Models/LanternPost.glb
```

These do not modify the supplied original or its metadata. The native mode compares
UV-to-surface positions/normals on unrelated submeshes; raw exporter buffer ordering
may differ. The pinned MIT xatlas source and license are included under
`Client/third_party/xatlas` for offline builds.

No test currently reads graph shading back from the GPU. DXC compilation checks
syntax/types/backends, **not CPU/GPU numerical parity or SPIR-V reflection**.
Earlier core geometry-context test values are explicitly supplied. The separate
geometry fixtures and native OBJ/cache tests execute actual baking. GPU resource
lifetime and runtime shader cache tests are future work. Mesh preview is a CPU UV
atlas; native-material asset reload does not prove scene/GPU visual equivalence.

## Reproduction

Editor build configured with C++20, MSVC Release, static vcpkg runtime and
`C:/VulkanSDK/1.4.350.0/Bin/dxc.exe`:

```powershell
cmake -S Client -B build/client-gui-check -DBUILD_TESTING=ON
cmake --build build/client-gui-check --config Release --target SmartGeneratorTest GeometryBakerTest SmartMaterialCoreTest MaterialSubgraphTest MaterialGraphCompilerTest MaterialGraphEditorTest MaterialGraphBakingTest EditorHelpTest IxtreemeEngine -j 4
ctest --test-dir build/client-gui-check -C Release -R '^(SmartGenerator|GeometryBaker|SmartMaterialCore|MaterialSubgraph|MaterialGraphCompiler|MaterialGraphEditor|MaterialGraphBaking|EditorHelp)Test$' --output-on-failure
```

Separate editor-disabled build:

```powershell
cmake -S Client -B build/smart-material-runtime -DIXTREEME_WITH_EDITOR=OFF -DBUILD_TESTING=ON
cmake --build build/smart-material-runtime --config Release --target SmartGeneratorTest GeometryBakerTest SmartMaterialCoreTest MaterialSubgraphTest MaterialGraphCompilerTest -j 4
ctest --test-dir build/smart-material-runtime -C Release -R '^(SmartGenerator|GeometryBaker|SmartMaterialCore|MaterialSubgraph|MaterialGraphCompiler)Test$' --output-on-failure
```

The local run uses source overrides for already fetched Jolt/meshoptimizer trees
to avoid duplicate downloads; binary directories remain separate. This verifies
the new core compiles and runs without ImGui/node-editor UI. It is not a full
editor-disabled player build or gameplay rendering test.

## Final results

| Configuration | Result |
| --- | --- |
| Editor-enabled Release | `IxtreemeEngine`, seven material/geometry/generator targets and `EditorHelpTest` built successfully. |
| Editor material/geometry/generator + help CTest selection | 8/8 passed, 0 failed; 13.04 s total after generator integration and Undo preview correction. |
| Editor-disabled Release core | `IXGeometryBaker`, `IXMaterialGraph`, `SmartGeneratorTest`, `GeometryBakerTest`, `SmartMaterialCoreTest`, `MaterialSubgraphTest`, `MaterialGraphCompilerTest` built successfully. |
| Editor-disabled material/geometry/generator CTest selection | 5/5 passed, 0 failed; 9.20 s total. |
| Shader checks | All 69 operational node kinds, three material presets, an expanded subgraph and all six generator recipes compile to DXIL and SPIR-V through configured DXC; the three structural kinds expand into ordinary instructions. No DXC SKIP in the recorded runs. |
| CPU benchmark | Exit 0; values/method in `SMART_MATERIAL_PERFORMANCE.md`. |
| Updated embedded editor help | `EditorHelpTest` passed in the 8-test selection (0.15 s test time). Updated English/Hungarian help was rebuilt into `IxtreemeEngine`. |

The final tests include removal of the last layer, serialization of an empty
managed stack and adding a new layer afterward. Earlier failures and fixes are
documented in `SMART_MATERIAL_IMPLEMENTATION.md`; no failure is counted as a pass.
The selected material tests are not a claim that the entire repository test suite
ran. Build diagnostics include an existing Inspector local-name shadow warning
and informational LTCG linker messages; builds return success.

Geometry fixture benchmark runs five times with exit 0; methodology, measured
median and exclusions are in `SMART_MATERIAL_PERFORMANCE.md`. The cache/geometry
limits are not a claim that large production meshes or all permitted dimensions
were performance/stress-tested. The full repository CTest suite was not run.

The generator milestone's editor coverage includes actual preset creation/save,
metadata-loss rejection, canonical layer assignment/Undo/Redo, geometry-bound
instance overrides, native dependencies, moves and corruption/recovery. Native
baking tests verify generator authored/effective snapshots and retained scoped
override values. Core tests supply all six recipe reference values and actual DXC.
The separate generator benchmark exits 0; timing exclusions are documented.
