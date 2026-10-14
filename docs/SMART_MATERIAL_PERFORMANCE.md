# Smart Material performance baseline

Measured 2026-10-12 on Windows x64, Intel Core i7-10700K @ 3.80 GHz, MSVC
19.51 Release, C++20, static runtime. This is a local baseline with no before/after
comparison or claimed speedup. No concurrent build or test was launched during
the recorded benchmark. Other system load was not controlled.

## Method and measured results

`SmartMaterialCoreTest --benchmark` performs one warmup and records the median of
five runs per operation. Production compiler, preview, UV baker and instance APIs
are used. Weathered Stone has 46 nodes/46 reachable instructions. Optional textures
use neutral constants; geometry fields are explicit preview inputs, not mesh data.
Preview includes evaluation, shading and RGBA8 conversion; it excludes GPU upload.
UV bake produces in-memory channel images and excludes file writing and import.

| Operation | Median |
| --- | ---: |
| Weathered Stone validation + IR compilation | 1.9112 ms |
| HLSL source generation | 0.1449 ms |
| Weathered Stone sphere preview, 128 x 128 | 17.2816 ms |
| Weathered Stone sphere preview, 256 x 256 | 66.0659 ms |
| 1000 validated instance override updates + buffer packing | 0.7148 ms |
| Existing Weathered Metal UV bake, 128 x 128 | 5.6766 ms |
| Weathered Stone DXC to DXIL | 60.1102 ms |
| Weathered Stone DXC to SPIR-V | 166.1397 ms |

DXC measurements separately run the generated Weathered Stone source with
`-T ps_6_0 -E GraphValidationPS`, adding `-spirv` for SPIR-V. They include
PowerShell process launch and output-file writing, so they are end-to-end CLI
latency rather than compiler-internal timing. The DXC executable is
`C:/VulkanSDK/1.4.350.0/Bin/dxc.exe`; the source is the compiler-test artifact
`build/client-gui-check/libs/render/graph-shader-tests/weathered-stone.rgba.hlsl`.
Each backend also uses one warmup and the median of five runs.

## Memory and resource evidence

- Stone's numeric parameter buffer is 176 bytes (11 float4 fields).
- `sizeof(MaterialInstanceBinding)` is 232 bytes on this ABI, excluding heap
  allocations for strings, maps and the parameter buffer. This is not total
  instance memory. Two bindings are tested sharing the same const program.
- The 256-square preview output is 262144 bytes. Evaluation workspace and image
  temporaries are additional allocations; workspace is reused across texels.
- Stone declares four texture bindings. This is source-layout information, not
  GPU texture sample count, SPIR-V reflection or measured cache behavior.
- Dynamic override updates do not call Compile or change generated HLSL. Graph
  topology/default changes compile again; cosmetic node edits reuse the program.

## Reproduction

After the builds and shader tests in `SMART_MATERIAL_TESTS.md`:

```powershell
& './build/client-gui-check/libs/render/Release/SmartMaterialCoreTest.exe' --benchmark
$materialDxc = 'C:/VulkanSDK/1.4.350.0/Bin/dxc.exe'
$materialShader = (Resolve-Path 'build/client-gui-check/libs/render/graph-shader-tests/weathered-stone.rgba.hlsl').Path
foreach ($materialBackend in @('dxil', 'spirv')) {
    $materialArgs = @('-T', 'ps_6_0', '-E', 'GraphValidationPS', '-Fo', ($materialShader + '.profile.' + $materialBackend), $materialShader)
    if ($materialBackend -eq 'spirv') { $materialArgs = @('-spirv') + $materialArgs }
    & $materialDxc @materialArgs
    if ($LASTEXITCODE -ne 0) { throw 'DXC warmup failed' }
    $materialTimes = for ($materialRun = 0; $materialRun -lt 5; $materialRun++) {
        (Measure-Command { & $materialDxc @materialArgs; if ($LASTEXITCODE -ne 0) { throw 'DXC profile failed' } }).TotalMilliseconds
    }
    $materialTimes = @($materialTimes | Sort-Object)
    Write-Output ($materialBackend + ' median ms: ' + $materialTimes[2])
}
```

## Subgraph milestone measurements

The same machine/Release method additionally measures 32 connected calls to one
in-memory GUID-backed asset. All calls feed a sum into roughness; the expanded IR
has 129 reachable instructions and 32 separately scoped float4 parameters (512
bytes). The resulting HLSL is 26075 bytes. Run:

```powershell
& './build/client-gui-check/libs/render/Release/MaterialSubgraphTest.exe' --benchmark
```

| Operation | Median |
| --- | ---: |
| Validation + 32-call expansion + IR compilation | 12.2956 ms |
| HLSL generation | 0.4064 ms |
| CPU sphere preview, 128 x 128 | 26.0766 ms |
| 1000 scoped instance updates + buffer packing | 1.1004 ms |

The resolver is called once per compilation for the shared GUID. Validated
interfaces are cached once per GUID within compilation as well: the same fixture
measured 16.1337 ms before that change, compared with 12.2956 ms afterward.
These are local five-run medians with uncontrolled background load, not a general
speedup guarantee. Expansion copies caller-specific IR bodies; instances continue
sharing the resulting immutable program.

This benchmark excludes native file resolution, IO, texture fetching, GPU execution
and real geometry. The native cache's 256-asset budget/replacement is a correctness
stress test, not a cache hit/miss-rate or frame-time benchmark. Geometry measurements
are recorded separately below; runtime shader-variant caching remains unimplemented.

## Remaining profiling work

The CPU preview still runs synchronously when dirty. A 256-square update at about
66 ms exceeds a 60 Hz frame budget; material preview/export jobs are still needed
before larger interactive bakes. The independent geometry baker now has a
cancellable worker; material previews and exports remain synchronous. GPU baking
is still unimplemented.

GPU material cost, total allocated instance memory, cache hit/miss rate and real
texture fetch count are not measured. Graph shaders are evaluated per pixel; their
cost against the baked textures is not measured either.

Graph shader compilation (2026-10-14, `GraphShaderTest`, three runs): the four
presets, each compiled to both variants with dxc and reflected, took 1329, 1342 and
1333 ms, about 0.33 s per material. Publishing pays this once on the editor thread. The fixture timing below does not establish
performance on large production meshes or runtime GPU performance.

## Geometry reference timing — 2026-10-12

Same MSVC Release machine as the preceding measurements. Command:
`build/client-gui-check/libs/render/Release/GeometryBakerTest.exe --benchmark`.
Five process runs exit 0; each first executes the geometry correctness fixtures,
then times only a synchronous box bake. The median is **67.7521 ms** (range
66.3321..67.8176 ms) for 12 triangles, a 256-square atlas, 52,628 covered texels,
32 AO rays, ray distance 3, bias 0.0001, seed 1, identity transform and four-texel
padding. This is 1,684,096 AO rays plus one thickness ray per covered texel.

Timing includes mesh validation/hash, seam topology/curvature/cavity, UV raster,
BVH construction/rays, tangent frame, dilation and result validation. It excludes
Assimp import, native GUID resolution, cache encode/decode/file IO, graph evaluation,
PNG/material publication, worker scheduling and GPU work. Cache size is
6,357,112 bytes at 256 square; in-memory temporary arrays/BVH/topology add overhead.
The benchmark proves this fixture timing only; it is not a production-mesh,
million-triangle, frame-latency or cache hit/miss-rate result. The editor worker
keeps this computation off the UI thread, while import and material preview/export
still need broader profiling and jobs for large assets.

## Smart generator timing — 2026-10-12

Same Release machine. `SmartGeneratorTest.exe --benchmark` exits 0 and measures
each recipe with two warmups and the median of five samples per operation.
Compilation expands a single resolver-backed call into the ordinary Example
material, with its mask also selected. Preview uses a real 128-square concave
corner geometry bake, Scalar Selected Output, NoiseInfluence .5, default other
controls and no texture resources. Geometry baking itself is outside these timings.

| Recipe | Compile (ms) | Preview 128 (ms) | Reachable IR instructions | Numeric parameter bytes |
| --- | ---: | ---: | ---: | ---: |
| Edge Wear | 7.2286 | 11.8860 | 35 | 96 |
| Dirt Accumulation | 6.7800 | 10.8983 | 33 | 80 |
| Rust Spread | 7.9076 | 13.9433 | 43 | 112 |
| Moss Growth | 12.8472 | 21.5602 | 67 | 160 |
| Dust Accumulation | 6.7429 | 12.1965 | 37 | 96 |
| Water Streaks | 14.1589 | 25.7261 | 75 | 160 |

Compilation includes schema validation, scoped expansion and reachable IR; the
resolver supplies in-memory definitions. Preview includes real atlas sampling,
shared evaluation and RGBA output allocation. Native import/GUID IO, shader DXC,
file/cache IO, material PNG/export, GPU and job scheduling are excluded. Parameter
byte counts exclude instance/container heap overhead. These are fixture timings,
not production multi-generator materials, GPU timings or a 60 Hz latency guarantee.
Material preview remains synchronous; larger layered/256-square previews need
future jobs and shared-IR optimization, rather than assuming this table scales linearly.
