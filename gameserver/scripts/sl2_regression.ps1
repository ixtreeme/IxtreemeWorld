param(
    [Parameter(Mandatory=$true)][string]$Bench,
    [Parameter(Mandatory=$true)][string]$OutDir,
    [ValidateSet('targeted','regression','debug','matrix','calibration','original')][string]$Suite='targeted',
    [string]$Baseline='',
    [string]$BaselineSource=''
)
# SL-1 runner conventions: fresh output, serial native runs, isolated fixtures.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$Bench=(Resolve-Path -LiteralPath $Bench).Path
if(Test-Path -LiteralPath $OutDir){throw "Use a fresh output directory: $OutDir"}
New-Item -ItemType Directory -Path $OutDir | Out-Null
$OutDir=(Resolve-Path -LiteralPath $OutDir).Path
if($Baseline){$Baseline=(Resolve-Path -LiteralPath $Baseline).Path; $BaselineSource=(Resolve-Path -LiteralPath $BaselineSource).Path}
$paths=git -C $repo ls-files --cached --others --exclude-standard gameserver shared/map docs
$source=@($paths | Sort-Object -Unique | ForEach-Object {
    $file=Join-Path $repo $_
    if(Test-Path -LiteralPath $file -PathType Leaf){[pscustomobject]@{Path=$_;SHA256=(Get-FileHash -LiteralPath $file).Hash}}
})
$source | Export-Csv "$OutDir/source.csv" -NoTypeInformation
git -C $repo status --short | Set-Content "$OutDir/status.txt"
git -C $repo rev-parse HEAD | Set-Content "$OutDir/head.txt"
Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors | Export-Csv "$OutDir/cpu.csv" -NoTypeInformation
Get-CimInstance Win32_DiskDrive | Select-Object Model,Size | Export-Csv "$OutDir/storage.csv" -NoTypeInformation
Get-FileHash -LiteralPath $Bench | Export-Csv "$OutDir/binary.csv" -NoTypeInformation
$rows=@(); $oldTemp=$env:TEMP; $oldTmp=$env:TMP
function Run-Case([string]$Name,[string[]]$BenchArgs,[bool]$UseBaseline=$false) {
    $binary=if($UseBaseline){$Baseline}else{$Bench}
    if(-not $binary){throw "Missing binary for $Name"}
    $env:TEMP=Join-Path $OutDir "$Name-temp"; $env:TMP=$env:TEMP
    New-Item -ItemType Directory -Path $env:TEMP | Out-Null
    $started=[DateTime]::UtcNow; $timer=[Diagnostics.Stopwatch]::StartNew()
    & $binary @BenchArgs *> "$OutDir/$Name.txt"
    $code=$LASTEXITCODE; $timer.Stop(); $ended=[DateTime]::UtcNow
    $log=Get-Content -LiteralPath "$OutDir/$Name.txt" -Raw
    # Count explicit assertion lines, not diagnostic strings such as cas_failures.
    $pass=[regex]::Matches($log,'(?m)^.*: PASS\s*$').Count
    $fail=[regex]::Matches($log,'(?m)^.*: FAIL\s*$').Count
    $skip=[regex]::Matches($log,'(?m)^.*: SKIPPED\b.*$').Count
    $fixtureFiles=@(Get-ChildItem -LiteralPath $env:TEMP -File -Recurse)
    $fixtureHashes=@($fixtureFiles | ForEach-Object {[pscustomobject]@{Path=$_.FullName.Substring($env:TEMP.Length+1);Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash}})
    $fixtureHashes | Export-Csv "$OutDir/$Name-fixtures.csv" -NoTypeInformation
    $sourceFile=if($UseBaseline){Join-Path (Split-Path $BaselineSource) 'source.csv'}else{"$OutDir/source.csv"}
    $script:rows += [pscustomobject]@{Name=$Name;Version=$(if($UseBaseline){'B1'}else{'N'});Exit=$code;AssertionPass=$pass;AssertionFail=$fail;Skipped=$skip;
        Seconds=$timer.Elapsed.TotalSeconds;StartedUTC=$started.ToString('o');EndedUTC=$ended.ToString('o');Arguments=($BenchArgs -join ' ');
        Binary=$binary;BinarySHA256=(Get-FileHash -LiteralPath $binary).Hash;SourceManifest=$sourceFile;SourceManifestSHA256=(Get-FileHash -LiteralPath $sourceFile).Hash;
        FixtureManifestSHA256=(Get-FileHash -LiteralPath "$OutDir/$Name-fixtures.csv").Hash;Temp=$env:TEMP}
    $script:rows | Export-Csv "$OutDir/summary.csv" -NoTypeInformation
    $script:rows | ConvertTo-Json -Depth 4 | Set-Content "$OutDir/runmanifest.json"
    Write-Output "$Name exit=$code assertions=$pass/$fail/$skip elapsed=$($timer.Elapsed.TotalSeconds)"
}
try {
    if($Suite -in @('original','calibration','matrix')) {
        $common=@('--mode','readiness','--players','500','--mobs','200000','--warmup','60','--seconds','30','--workers','4','--seed','20260922')
        if($Suite -eq 'calibration') { Run-Case 'C-moving-v1' ($common+@('--file-world','--scenario','c-moving-v1','--budget-mb','16')) }
        elseif($Suite -eq 'original') { foreach($scenario in @('moving','hotspot','border')) {
            if($Baseline){Run-Case "B1-$scenario" ($common+@('--file-world','--scenario',$scenario,'--budget-mb','16')) $true}
            Run-Case "N-$scenario" ($common+@('--file-world','--scenario',$scenario,'--budget-mb','16'))
        }} else {
            $smoke=@('--mode','readiness','--scenario','dense','--players','40','--mobs','3000','--warmup','3','--seconds','3','--workers','4','--seed','20260922')
            if($Baseline){Run-Case 'B1-smoke' $smoke $true};Run-Case 'N-smoke' $smoke
            foreach($scenario in @('dense','spread')) {
                $players=if($scenario -eq 'dense'){'500'}else{'7000'}
                $caseArgs=@('--mode','readiness','--scenario',$scenario,'--players',$players,'--mobs','200000','--warmup','60','--seconds','30','--workers','4','--seed','20260922')
                if($Baseline){Run-Case "B1-synthetic-$scenario" $caseArgs $true};Run-Case "N-synthetic-$scenario" $caseArgs
                Run-Case "N-file-$scenario-128" ($caseArgs+@('--file-world','--budget-mb','128'))
            }
            foreach($budget in @('16','3')){Run-Case "N-soak-$budget" @('--mode','streamsoak','--cycles','60','--budget-mb',$budget)}
            Run-Case 'N-C-moving-v1' ($common+@('--file-world','--scenario','c-moving-v1','--budget-mb','16'))
            for($pair=1;$pair -le 3;$pair++) { foreach($residency in @('eager','streamed')) {
                $caseArgs=@('--mode','readiness','--file-world','--world-km','4','--zones-x','2','--zones-y','2','--scenario','dense','--players','100','--mobs','3000','--warmup','10','--seconds','15','--workers','4','--seed','20260922','--asf-off','--budget-mb','3')
                if($residency -eq 'eager'){$caseArgs+=@('--eager-terrain')}
                if($Baseline){Run-Case "B1-query-$residency-$pair" $caseArgs $true};Run-Case "N-query-$residency-$pair" $caseArgs
            }}
        }
    } elseif($Suite -eq 'regression') {
        foreach($mode in @('field','loadfield','partitionscore')){Run-Case "$mode-selftest" @("--$mode-selftest")}
        Run-Case 'routing' @('--routing-selftest','--logical-processes','2','--seconds','3','--players','40','--mobs','300')
        foreach($mode in @('lod','activity','loadfield','partitionscore','stability','splitmerge','ghost','aoi','replication','scheduler','tickrate','inputpath','netstress','presence','asfdeterminism','workerpool','replv2','protocol','hygiene')){Run-Case $mode @('--mode',$mode,'--seconds','3')}
        foreach($cycles in @('100','1000')){Run-Case "reclamation$cycles" @('--mode','reclamation','--cycles',$cycles)}
    } else {
        $modes=if($Suite -eq 'debug'){@('streamadmission','streaming','worldquery','streamlife','map4','snapshot','worldpackage')}else{@('streamadmission','streaming','worldquery','streamlife','map4','snapshot','terrain','worldpackage','mapsplit','mapaudit','bootstrap')}
        foreach($mode in $modes){Run-Case $mode @('--mode',$mode)}
    }
} finally {$env:TEMP=$oldTemp;$env:TMP=$oldTmp}
if($rows.Where({$_.Exit -ne 0 -or $_.AssertionFail -gt 0}).Count){exit 1}
