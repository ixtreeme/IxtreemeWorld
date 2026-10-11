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

## Limits and next profiling work

The CPU preview still runs synchronously when dirty. A 256-square update at about
66 ms exceeds a 60 Hz frame budget; jobs/cancel/coalescing are needed before larger
interactive bakes. The material core remains editor-independent, but this
milestone does not implement asynchronous or GPU baking.

GPU material cost, GPU readback parity, total allocated instance memory, cache
hit/miss rate, real texture fetch count and geometry bake time are not measured.
Runtime graph shader/variant cache and geometry baker are not implemented yet.
Measure those on actual meshes and texture resources after their integration;
this baseline cannot establish runtime GPU performance or geometric correctness.
