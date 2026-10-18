[CmdletBinding()]
param([Parameter(Mandatory)][string]$MSBuild, [switch]$LegacyReference, [string]$DependencyLockPath = (Join-Path $PSScriptRoot 'deps.lock.json'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
& (Join-Path $PSScriptRoot 'PrepareManifoldHeaders.ps1') -DependencyLockPath $DependencyLockPath
$testRoot = Join-Path $PSScriptRoot 'out/falco/csg-corpus'
New-Item -ItemType Directory -Force $testRoot | Out-Null
$sourcePath = Join-Path $PSScriptRoot 'Engine/Components/CSGBrush.cpp'
$source = [IO.File]::ReadAllText($sourcePath).Replace("`r`n", "`n")
$start = $source.IndexOf("`tvoid CSGBrush::makeCube()")
$end = $source.IndexOf("`tvoid CSGBrush::setBrushOperation")
if ($start -lt 0 -or $end -le $start) { throw 'CSGBrush primitive method boundaries changed.' }
$prefix = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'ModernBgfxCompat/csg_brush_fixture_prefix.h'))
[IO.File]::WriteAllText((Join-Path $testRoot 'brush-fixtures.h'), $prefix + "`n" + $source.Substring($start,$end-$start))
Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256 | ConvertTo-Json | Set-Content (Join-Path $testRoot 'fixture-source.json')
$project = Join-Path $PSScriptRoot 'ModernBgfxCompat/CSGManifoldCorpus.vcxproj'
& $MSBuild $project /t:Rebuild /p:Configuration=Release /p:Platform=x64 "/p:FalcoCSGLegacyReference=$($LegacyReference.IsPresent.ToString().ToLowerInvariant())" /m:1 /nr:false /v:minimal /nologo "/bl:$testRoot/corpus.binlog" 2>&1 | Tee-Object (Join-Path $testRoot 'build.log')
if ($LASTEXITCODE -ne 0) { throw "CSG corpus build failed: $LASTEXITCODE" }
foreach ($run in 1..2) {
    & (Join-Path $testRoot 'CSGManifoldCorpus.exe') (Join-Path $testRoot "mesh-$run.txt") 2>&1 | Tee-Object (Join-Path $testRoot "run-$run.log")
    if ($LASTEXITCODE -ne 0) { throw "CSG corpus run $run failed: $LASTEXITCODE" }
}
$hashes = @(1..2 | ForEach-Object { Get-FileHash (Join-Path $testRoot "mesh-$_.txt") -Algorithm SHA256 })
if ($hashes[0].Hash -ne $hashes[1].Hash) { throw 'CSG mesh serialization differs between processes.' }
$hashes | ConvertTo-Json | Set-Content (Join-Path $testRoot 'determinism.json')
Write-Output "PASS: CSG corpus and cross-process geometry/property determinism ($($hashes[0].Hash)); legacy comparison enabled: $($LegacyReference.IsPresent)."
