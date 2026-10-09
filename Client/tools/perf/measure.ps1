# Runs the built game (build-runtime) on the stress project with the given environment and prints its
# last [PERF] lines (with -Gpu also the [PERF] gpu lines).
# usage: tools/perf/measure.ps1 -Seconds 25 -Env @{ IX_SHADOW_LOD = "0" } [-Gpu]
# The stress project is made by make_stress_scene.js (Client/build/stress-20261007/project, not in git).
param(
    [int]$Seconds = 25,
    [hashtable]$Env = @{},
    [switch]$Gpu
)
$client = Resolve-Path "$PSScriptRoot\..\.."
$project = "$client\build\stress-20261007\project"
$exe = "$client\build-runtime\apps\client\Release\IxtreemeEngine.exe"
$names = @("IX_JOBS", "IX_JOBS_WORKERS", "IX_PARALLEL_CULL", "IX_PARALLEL_ANIMATION", "IX_PARALLEL_PARTICLES",
    "IX_SHADOW_SHIFT", "IX_SHADOW_LOD", "VK_INSTANCE_LAYERS")
foreach ($n in $names) { [Environment]::SetEnvironmentVariable($n, $null, "Process") }
foreach ($k in $Env.Keys) { [Environment]::SetEnvironmentVariable($k, [string]$Env[$k], "Process") }
[Environment]::SetEnvironmentVariable("IX_PERF_LOG", "2", "Process")
[Environment]::SetEnvironmentVariable("IX_GPU_PROFILE", $(if ($Gpu) { "1" } else { $null }), "Process")
Remove-Item "$project\ixtreeme_engine.log" -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -WorkingDirectory $project -PassThru
Start-Sleep -Seconds $Seconds
$lines = Select-String -Path "$project\ixtreeme_engine.log" -Pattern "\[PERF\]|\[JOBS\]" | ForEach-Object { $_.Line }
# Only the process this script started.
Stop-Process -Id $p.Id -Confirm:$false -ErrorAction SilentlyContinue
$p.WaitForExit(10000) | Out-Null
foreach ($k in $Env.Keys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
$lines | Where-Object { $_ -match "\[JOBS\]" } | Select-Object -First 1
$lines | Where-Object { $_ -match "\[PERF\]" } | Select-Object -Last $(if ($Gpu) { 4 } else { 2 })
