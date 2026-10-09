param(
    [Parameter(Mandatory=$true)][string]$Bench,
    [Parameter(Mandatory=$true)][string]$OutDir,
    [ValidateSet('targeted','regression','original','debug')][string]$Suite='targeted'
)
$ErrorActionPreference='Stop'
$Bench=(Resolve-Path -LiteralPath $Bench).Path
if (Test-Path -LiteralPath $OutDir) { throw "Use a new run directory: $OutDir" }
New-Item -ItemType Directory -Path $OutDir | Out-Null
$OutDir=(Resolve-Path -LiteralPath $OutDir).Path
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Get-FileHash -LiteralPath $Bench -Algorithm SHA256 | Export-Csv (Join-Path $OutDir 'binary.csv') -NoTypeInformation
git -C $repo rev-parse HEAD | Set-Content (Join-Path $OutDir 'head.txt')
git -C $repo status --short | Set-Content (Join-Path $OutDir 'status.txt')
git -C $repo diff --binary | Set-Content -Encoding UTF8 (Join-Path $OutDir 'tracked.patch')
$sourcePaths=git -C $repo ls-files --cached --others --exclude-standard gameserver/apps/gameserver shared/map
$sourcePaths | Sort-Object -Unique | ForEach-Object {
    $path=Join-Path $repo $_
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        [pscustomobject]@{Path=$_; SHA256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
    }
} | Export-Csv (Join-Path $OutDir 'source.csv') -NoTypeInformation
Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors |
    Export-Csv (Join-Path $OutDir 'cpu.csv') -NoTypeInformation
Get-CimInstance Win32_DiskDrive | Select-Object Model,InterfaceType,Size |
    Export-Csv (Join-Path $OutDir 'storage.csv') -NoTypeInformation
$rows=@()
$oldTemp=$env:TEMP
$oldTmp=$env:TMP
function Run-Case([string]$Name,[string[]]$BenchArgs) {
    # Existing fixture helpers use temp_directory_path(). Isolate every child
    # process without changing the package geometry or checked-in test_zone.
    $env:TEMP=Join-Path $OutDir "$Name-temp"
    $env:TMP=$env:TEMP
    New-Item -ItemType Directory -Path $env:TEMP | Out-Null
    $began=[DateTime]::UtcNow
    $timer=[Diagnostics.Stopwatch]::StartNew()
    & $Bench @BenchArgs *> (Join-Path $OutDir "$Name.txt")
    $code=$LASTEXITCODE
    $timer.Stop()
    $script:rows += [pscustomobject]@{
        Name=$Name; Exit=$code; Seconds=$timer.Elapsed.TotalSeconds; StartedUTC=$began.ToString('o');
        Arguments=($BenchArgs -join ' '); Temp=$env:TEMP
    }
    $script:rows | Export-Csv (Join-Path $OutDir 'summary.csv') -NoTypeInformation
    Write-Output "$Name exit=$code elapsed=$($timer.Elapsed.TotalSeconds)"
}
try {
    if ($Suite -eq 'original') {
        foreach ($scenario in @('moving','hotspot','border')) {
            Run-Case $scenario @('--mode','readiness','--file-world','--scenario',$scenario,
                '--players','500','--mobs','200000','--warmup','60','--seconds','30',
                '--workers','4','--seed','20260922','--budget-mb','16')
        }
    } elseif ($Suite -eq 'regression') {
        foreach ($mode in @('field','loadfield','partitionscore')) { Run-Case "$mode-selftest" @("--$mode-selftest") }
        Run-Case 'routing' @('--routing-selftest','--logical-processes','2','--seconds','3','--players','40','--mobs','300')
        foreach ($mode in @('lod','activity','loadfield','partitionscore','stability','splitmerge','ghost','aoi',
            'replication','scheduler','tickrate','inputpath','netstress','presence','asfdeterminism','workerpool',
            'replv2','protocol','hygiene')) { Run-Case $mode @('--mode',$mode,'--seconds','3') }
        foreach ($cycles in @('100','1000')) { Run-Case "reclamation$cycles" @('--mode','reclamation','--cycles',$cycles) }
    } else {
        $modes=if ($Suite -eq 'debug') { @('streamadmission','streaming','worldquery','streamlife','map4','snapshot') }
               else { @('streamadmission','streaming','worldquery','streamlife','map4','snapshot','terrain',
                         'worldpackage','mapsplit','mapaudit','bootstrap') }
        foreach ($mode in $modes) { Run-Case $mode @('--mode',$mode) }
    }
} finally { $env:TEMP=$oldTemp; $env:TMP=$oldTmp }
if ($rows.Where({$_.Exit -ne 0}).Count -gt 0) { exit 1 }
