# Runs the built game (build-runtime) on the stress project and prints the spawn test's script lines, the
# model loads and the [PERF] loop lines in order (IX_PERF_LOG=1: one line a second, with the window's max frame).
# usage: tools/perf/spawn.ps1 -Seconds 14 -Env @{ IX_JOBS = "0" }
# Main.scene must be a spawn test variant first (Main.scene.spawntest / .staticspawn / .physdestroy).
param(
    [int]$Seconds = 14,
    [hashtable]$Env = @{}
)
$client = Resolve-Path "$PSScriptRoot\..\.."
$project = "$client\build\stress-20261007\project"
$exe = "$client\build-runtime\apps\client\Release\IxtreemeEngine.exe"
$names = @("IX_JOBS", "IX_JOBS_WORKERS", "IX_PARALLEL_CULL", "IX_PARALLEL_ANIMATION", "IX_PARALLEL_PARTICLES",
    "IX_SHADOW_SHIFT", "IX_SHADOW_LOD", "VK_INSTANCE_LAYERS")
foreach ($n in $names) { [Environment]::SetEnvironmentVariable($n, $null, "Process") }
foreach ($k in $Env.Keys) { [Environment]::SetEnvironmentVariable($k, [string]$Env[$k], "Process") }
[Environment]::SetEnvironmentVariable("IX_PERF_LOG", "1", "Process")
[Environment]::SetEnvironmentVariable("IX_GPU_PROFILE", $null, "Process")
Remove-Item "$project\ixtreeme_engine.log" -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -WorkingDirectory $project -PassThru
Start-Sleep -Seconds $Seconds
Stop-Process -Id $p.Id -Confirm:$false -ErrorAction SilentlyContinue
$p.WaitForExit(10000) | Out-Null
foreach ($k in $Env.Keys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
Get-Content "$project\ixtreeme_engine.log" |
    Where-Object { $_ -match "SPAWNTEST|Renderer loaded:|\[PERF\] loop|\[SCRIPT\]" } |
    ForEach-Object { if ($_.Length -gt 200) { $_.Substring(0, 200) } else { $_ } }
