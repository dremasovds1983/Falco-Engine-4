[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$ManifestPath,
    [Parameter(Mandatory)][string]$ReportPath,
    [ValidateSet('Private','Public')][string]$Purpose = 'Public'
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/CleanTreeCommon.ps1"
$rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
$reportFile = [IO.Path]::GetFullPath($ReportPath)
Assert-CleanPhysicalPath $rootPath
Assert-CleanPhysicalPath $reportFile
if ($reportFile.StartsWith($rootPath+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Report must be outside the validated tree.' }
if ((Get-Item -LiteralPath $rootPath).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Root cannot be a reparse point.' }
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1) { throw 'Unsupported manifest schema' }
Assert-CleanManifest $manifest
$expected = @{}
foreach ($file in $manifest.files) {
    Assert-CleanRelativePath $file.path
    if ($expected.ContainsKey($file.path)) { throw "Duplicate manifest path: $($file.path)" }
    $expected[$file.path] = $file
}
$findings = [Collections.Generic.List[object]]::new()
$inventory = [Collections.Generic.List[object]]::new()
$seen = @{}
function Add-Finding([string]$Class,[string]$Path,[string]$Reason) { $findings.Add([pscustomobject]@{class=$Class;path=$Path;reason=$Reason}) }
foreach ($entry in Get-CleanTreeEntries $rootPath) {
    $relative = Get-CleanRelativePath $rootPath $entry.FullName
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { Add-Finding 'LINK-FOUND' $relative 'Reparse points are never followed'; continue }
    $reason = Get-CleanDeniedReason $relative
    if ($reason) { Add-Finding 'DENIED-PRESENT' $relative $reason; continue }
    if ($entry.PSIsContainer) { continue }
    $seen[$relative] = $true
    if (-not $expected.ContainsKey($relative)) { Add-Finding 'UNKNOWN' $relative 'Not in reviewed file inventory'; continue }
    $record = $expected[$relative]
    $hash = (Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256).Hash
    if ($entry.Length -ne $record.bytes -or $hash -ne $record.sha256) { Add-Finding 'PINNED-MISMATCH' $relative 'Size or SHA-256 differs' }
    if ($record.disposition -eq 'REVIEW') { Add-Finding 'REVIEW-PRESENT' $relative $record.reason }
    if ($entry.Extension -in @('.cpp','.h','.hpp','.c','.cs','.ps1','.py','.json','.md','.txt','.xml','.vcxproj','.csproj','.rc','.sln','.filters')) {
        $text = [IO.File]::ReadAllText($entry.FullName)
        # Report categories and paths only, never matched credential values.
        if ($text -match '-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|\bAKIA[0-9A-Z]{16}\b|\bgh[pousr]_[A-Za-z0-9]{30,}\b') { Add-Finding 'SECRET-PATTERN' $relative 'Credential-shaped content requires review' }
        if ($text -match '(?i)[A-Z]:[\\/]Users[\\/]DrimPC[\\/]|[A-Z]:[\\/]FalcoSource[\\/]') { Add-Finding 'LOCAL-PATH' $relative 'Workspace/user-specific absolute path' }
    }
    if ($entry.Extension -eq '.vcxproj') {
        [xml]$project = [IO.File]::ReadAllText($entry.FullName)
        $ns = @{m='http://schemas.microsoft.com/developer/msbuild/2003'}
        foreach ($match in Select-Xml -Xml $project -Namespace $ns -XPath '//m:ItemDefinitionGroup') {
            if ($match.Node.GetAttribute('Condition') -like '*FalcoModernBgfx*true*' -and $match.Node.GetAttribute('Condition') -notlike "*!=*true*") {
                if ($match.Node.InnerXml -match '(?i)\.\./(?:Bgfx|Carve|steam|OpenAL|Mono|d3d|Linux|codecs/video/ffmpeg)/') { Add-Finding 'MODERN-LEGACY-REFERENCE' $relative 'Active modern SDK reference' }
            }
        }
    }
    $inventory.Add([pscustomobject]@{path=$relative;bytes=$entry.Length;sha256=$hash;disposition=$record.disposition})
}
foreach ($path in $expected.Keys) { if (-not $seen.ContainsKey($path)) { Add-Finding 'MISSING-REQUIRED' $path 'Inventory file missing' } }
if ($Purpose -eq 'Public') {
    foreach ($gate in $manifest.publicationBlockers) { Add-Finding 'PUBLICATION-BLOCKER' '' $gate }
}
$failures = @($findings | Where-Object { $_.class -ne 'REVIEW-PRESENT' -or $Purpose -eq 'Public' })
$result = [ordered]@{schemaVersion=1;purpose=$Purpose;passed=($failures.Count -eq 0);publicCompositionPassed=($Purpose -eq 'Public' -and $failures.Count -eq 0);files=$inventory.Count;findings=@($findings.ToArray());inventory=@($inventory.ToArray())}
New-Item -ItemType Directory -Force (Split-Path $reportFile) | Out-Null
$result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportFile -Encoding UTF8
Write-Output "Clean-tree $Purpose validation: passed=$($result.passed), files=$($inventory.Count), failures=$($failures.Count). Report: $reportFile"
if ($failures.Count -gt 0) { exit 1 }
exit 0
