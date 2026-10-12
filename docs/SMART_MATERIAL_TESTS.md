# Smart Material validation

All tests use real production classes. Final validation date: 2026-10-12.

## Coverage and its limits

| Test | What is actually executed |
| --- | --- |
| `SmartMaterialCoreTest` | Canonical layers and ID ownership; opacity/channel blending/order/enable/duplication/removal; v3 round-trip and malformed/desynchronized data; explicit conversion; instance JSON/shared program/packing/error recovery; real atomic IO; 22 new node reference/error cases; Weathered Stone preview and geometry-dependent rejection. |
| `MaterialSubgraphTest` | Graph v4/wrapper v1, all six typed passthrough reference values, defaults, scoped/repeated/nested parameters, preview, UV bake, layer masks, deterministic source, missing assets/resolver, recursion, schema refresh, depth/call/node/ID bounds; actual expanded DXIL/SPIR-V compilation. |
| `MaterialGraphCompilerTest` | Existing CPU arithmetic, textures, HDR/sRGB, normals, layers, cycles/validation, 2048-node traversal, v1/v2 compatibility; all 69 node kinds and three presets compile using actual DXC to DXIL and SPIR-V when configured. Missing DXC is printed as SKIP rather than asserted as GPU validation. |
| `MaterialGraphEditorTest` | Production panel/headless ImGui and existing synthetic mouse/key workflow; stack/history; graph/instance/subgraph files, GUID rescan/moves, dependency metadata, changed/malformed/fixed subgraph preview, standalone output, scoped instances; full 256-asset cache and dependency-set replacement. New binding dialogs and layer drag-and-drop pixel interactions are not independently automated. |
| `MaterialGraphBakingTest` | Actual UV images, native material/ORM and manifest reload; texture imports/moves/renames, bilinear filtering and native GUID lookup with/without a library; missing GUID cannot select a stale path; subgraph authored/effective source snapshots and scoped overrides; geometry-dependent export refusal. |

No test currently reads graph shading back from the GPU. DXC compilation checks
syntax/types/backends, **not CPU/GPU numerical parity or SPIR-V reflection**.
Geometry-context test values are explicitly supplied; they are not geometry bake
correctness tests. GPU resource lifetime and runtime cache tests are future work.

## Reproduction

Editor build configured with C++20, MSVC Release, static vcpkg runtime and
`C:/VulkanSDK/1.4.350.0/Bin/dxc.exe`:

```powershell
cmake -S Client -B build/client-gui-check -DBUILD_TESTING=ON
cmake --build build/client-gui-check --config Release --target SmartMaterialCoreTest MaterialSubgraphTest MaterialGraphCompilerTest MaterialGraphEditorTest MaterialGraphBakingTest IxtreemeEngine -j 4
ctest --test-dir build/client-gui-check -C Release -R '^(SmartMaterialCore|MaterialSubgraph|MaterialGraphCompiler|MaterialGraphEditor|MaterialGraphBaking)Test$' --output-on-failure
```

Separate editor-disabled build:

```powershell
cmake -S Client -B build/smart-material-runtime -DIXTREEME_WITH_EDITOR=OFF -DBUILD_TESTING=ON
cmake --build build/smart-material-runtime --config Release --target SmartMaterialCoreTest MaterialSubgraphTest MaterialGraphCompilerTest -j 4
ctest --test-dir build/smart-material-runtime -C Release -R '^(SmartMaterialCore|MaterialSubgraph|MaterialGraphCompiler)Test$' --output-on-failure
```

The local run uses source overrides for already fetched Jolt/meshoptimizer trees
to avoid duplicate downloads; binary directories remain separate. This verifies
the new core compiles and runs without ImGui/node-editor UI. It is not a full
editor-disabled player build or gameplay rendering test.

## Final results

| Configuration | Result |
| --- | --- |
| Editor-enabled Release | `IxtreemeEngine` and all five material test targets built successfully. |
| Editor material CTest selection | 5/5 passed, 0 failed; 8.31 s total after the asset-move regression correction. |
| Editor-disabled Release core | `IXMaterialGraph`, `SmartMaterialCoreTest`, `MaterialSubgraphTest`, `MaterialGraphCompilerTest` built successfully. |
| Editor-disabled material CTest selection | 3/3 passed, 0 failed; 7.55 s total. |
| Shader checks | All 69 operational node kinds, three presets and an expanded subgraph compile to DXIL and SPIR-V through configured DXC; the three structural kinds use expansion instead of evaluator placeholders. No DXC SKIP in the recorded runs. |
| CPU benchmark | Exit 0; values/method in `SMART_MATERIAL_PERFORMANCE.md`. |

The final tests include removal of the last layer, serialization of an empty
managed stack and adding a new layer afterward. Earlier failures and fixes are
documented in `SMART_MATERIAL_IMPLEMENTATION.md`; no failure is counted as a pass.
The selected material tests are not a claim that the entire repository test suite
ran. Build diagnostics include an existing Inspector local-name shadow warning
and informational LTCG linker messages; builds return success.
