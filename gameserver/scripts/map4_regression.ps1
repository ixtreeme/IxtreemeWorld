param(
    [Parameter(Mandatory=$true)][string]$Bench,
    [Parameter(Mandatory=$true)][string]$OutDir,
    [ValidateSet('regression','readiness','readiness-capacity','debug')][string]$Suite = 'regression',
    [string]$Baseline = (Join-Path $PSScriptRoot '../../build/map4-baseline/worldbench-map3.exe')
)
$ErrorActionPreference = 'Stop'
$Bench = (Resolve-Path -LiteralPath $Bench).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path
Get-FileHash -LiteralPath $Bench -Algorithm SHA256 | Export-Csv (Join-Path $OutDir 'binary.csv') -NoTypeInformation
$results = @()
function Run-Case([string]$Name, [string[]]$BenchArgs) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $Bench @BenchArgs *> (Join-Path $OutDir "$Name.txt")
    $code = $LASTEXITCODE
    $timer.Stop()
    $script:results += [pscustomobject]@{ Name=$Name; Exit=$code; Seconds=[math]::Round($timer.Elapsed.TotalSeconds,2); Arguments=($BenchArgs -join ' ') }
    $script:results | Export-Csv (Join-Path $OutDir 'summary.csv') -NoTypeInformation
    Write-Output "$Name exit=$code elapsed=$($timer.Elapsed.TotalSeconds)"
}
if ($Suite -eq 'regression') {
    foreach ($mode in @('field','loadfield','partitionscore')) { Run-Case "$mode-selftest" @("--$mode-selftest") }
    Run-Case 'routing' @('--routing-selftest','--logical-processes','2','--seconds','3','--players','40','--mobs','300')
    foreach ($mode in @('lod','activity','loadfield','partitionscore','stability','splitmerge','ghost','aoi','replication','scheduler',
                        'tickrate','inputpath','netstress','presence','asfdeterminism','workerpool','replv2','protocol','hygiene')) {
        Run-Case $mode @('--mode',$mode,'--seconds','3')
    }
    Run-Case 'reclamation100' @('--mode','reclamation','--cycles','100')
    Run-Case 'reclamation1000' @('--mode','reclamation','--cycles','1000')
    Run-Case 'streamsoak16' @('--mode','streamsoak','--cycles','60','--budget-mb','16')
    Run-Case 'streamsoak3' @('--mode','streamsoak','--cycles','60','--budget-mb','3')
} elseif ($Suite -eq 'debug') {
    foreach ($mode in @('map4','worldpackage','terrain','streaming','worldquery','streamlife')) {
        Run-Case $mode @('--mode',$mode)
    }
} elseif ($Suite -eq 'readiness-capacity') {
    # Additional capacity characterization; never replaces the 16 MiB
    # acceptance cases or changes their failure status.
    foreach ($scenario in @('moving','hotspot','border')) {
        Run-Case "file-$scenario-128" @('--mode','readiness','--file-world','--scenario',$scenario,'--players','500','--mobs','200000',
                                         '--warmup','60','--seconds','30','--workers','4','--seed','20260922','--budget-mb','128')
    }
} else {
    # A timing-sensitive scheduler failure warrants a quiet paired rerun.
    $regressionSummary = Join-Path (Split-Path $OutDir) 'regression/summary.csv'
    if ((Test-Path -LiteralPath $regressionSummary) -and
        (Import-Csv -LiteralPath $regressionSummary | Where-Object { $_.Name -eq 'scheduler' -and $_.Exit -ne '0' })) {
        if ($Baseline -and (Test-Path -LiteralPath $Baseline)) {
            $currentBench = $Bench
            $Bench = (Resolve-Path -LiteralPath $Baseline).Path
            Run-Case 'baseline-scheduler' @('--mode','scheduler','--seconds','3')
            $Bench = $currentBench
        }
        Run-Case 'scheduler-confirmation' @('--mode','scheduler','--seconds','3')
    }
    foreach ($scenario in @('dense','spread')) {
        $players = if ($scenario -eq 'dense') { '500' } else { '7000' }
        if ($Baseline -and (Test-Path -LiteralPath $Baseline)) {
            $currentBench = $Bench
            $Bench = (Resolve-Path -LiteralPath $Baseline).Path
            Run-Case "baseline-$scenario" @('--mode','readiness','--scenario',$scenario,'--players',$players,'--mobs','200000',
                                              '--warmup','60','--seconds','30','--workers','4','--seed','20260922')
            $Bench = $currentBench
        }
        Run-Case "synthetic-$scenario" @('--mode','readiness','--scenario',$scenario,'--players',$players,'--mobs','200000',
                                          '--warmup','60','--seconds','30','--workers','4','--seed','20260922')
        Run-Case "file-$scenario" @('--mode','readiness','--file-world','--scenario',$scenario,'--players',$players,'--mobs','200000',
                                     '--warmup','60','--seconds','30','--workers','4','--seed','20260922','--budget-mb','128')
    }
    foreach ($scenario in @('moving','hotspot','border')) {
        Run-Case "file-$scenario" @('--mode','readiness','--file-world','--scenario',$scenario,'--players','500','--mobs','200000',
                                     '--warmup','60','--seconds','30','--workers','4','--seed','20260922','--budget-mb','16')
    }
    # Same package, seed, population, topology and trace for residency A/B.
    Run-Case 'small-eager' @('--mode','readiness','--file-world','--eager-terrain','--world-km','4','--zones-x','2','--zones-y','2',
                            '--scenario','dense','--players','100','--mobs','3000','--warmup','10','--seconds','15','--workers','4','--asf-off')
    Run-Case 'small-streamed' @('--mode','readiness','--file-world','--world-km','4','--zones-x','2','--zones-y','2',
                               '--scenario','dense','--players','100','--mobs','3000','--warmup','10','--seconds','15','--workers','4','--asf-off','--budget-mb','3')
}
if ($results.Where({ $_.Exit -ne 0 }).Count -gt 0) { exit 1 }
