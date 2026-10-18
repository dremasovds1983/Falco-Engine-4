[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceRoot,
    [Parameter(Mandatory)][string]$Destination,
    [Parameter(Mandatory)][string]$ManifestPath,
    [Parameter(Mandatory)][string]$ReportPath,
    [ValidateSet('Private','Public')][string]$Purpose = 'Public'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/CleanTreeCommon.ps1"
$source = [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\','/')
$target = [IO.Path]::GetFullPath($Destination).TrimEnd('\','/')
Assert-CleanPhysicalPath $source
Assert-CleanPhysicalPath $target
Assert-CleanPhysicalPath $ReportPath
$reportFile = [IO.Path]::GetFullPath($ReportPath)
if ($reportFile -eq $target -or $reportFile.StartsWith($target+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Report must be outside the copied tree.' }
if (Test-Path -LiteralPath $target) { throw 'Destination must be new; existing trees are never overwritten.' }
if ($source -eq $target -or $source.StartsWith($target+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe source/destination relationship' }
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1) { throw 'Unsupported manifest schema' }
Assert-CleanManifest $manifest
if ($Purpose -eq 'Public' -and (@($manifest.publicationBlockers).Count -gt 0 -or @($manifest.files | Where-Object disposition -eq 'REVIEW').Count -gt 0)) { throw 'PUBLIC build blocked by unresolved manifest review entries/gates.' }
$seen = @{}
# Preflight every input before copying. Do not follow links in any path segment.
foreach ($file in $manifest.files) {
    Assert-CleanRelativePath $file.path
    if ($seen.ContainsKey($file.path)) { throw 'Duplicate manifest entry' }; $seen[$file.path]=$true
    if (Get-CleanDeniedReason $file.path) { throw "Denied manifest entry: $($file.path)" }
    $sourceFile = Join-Path $source $file.path
    $cursor = $sourceFile
    while ($cursor -and $cursor.Length -ge $source.Length) {
        if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked source path: $($file.path)" }
        if ($cursor -eq $source) { break }; $cursor = Split-Path $cursor
    }
    $item = Get-Item -LiteralPath $sourceFile
    if ($item.PSIsContainer -or $item.Length -ne $file.bytes -or (Get-FileHash -LiteralPath $sourceFile).Hash -ne $file.sha256) { throw "Source hash/size mismatch: $($file.path)" }
}
New-Item -ItemType Directory -Path $target | Out-Null
foreach ($file in $manifest.files) {
    $output = Join-Path $target $file.path
    New-Item -ItemType Directory -Force (Split-Path $output) | Out-Null
    Copy-Item -LiteralPath (Join-Path $source $file.path) -Destination $output
}
& "$PSScriptRoot/ValidateCleanTree.ps1" -Root $target -ManifestPath $ManifestPath -ReportPath $ReportPath -Purpose $Purpose
exit $LASTEXITCODE
