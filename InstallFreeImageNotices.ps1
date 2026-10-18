[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'ThirdParty/freeimage-3.18.0'
$manifest = Get-Content -LiteralPath (Join-Path $source 'PROVENANCE.json') -Raw | ConvertFrom-Json
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deps.lock.json') -Raw | ConvertFrom-Json
$component = $lock.components.freeimage
if ($manifest.componentId -ne 'freeimage' -or $manifest.version -ne $component.version -or
    $manifest.source.sha256 -ne $component.archive.sha256 -or $manifest.source.url -ne $component.archive.url -or
    $manifest.files.'license-fi.txt'.sha256 -ne $component.licenseFiles[0].sha256) { throw 'FreeImage notice provenance mismatch' }
$names = @($manifest.files.PSObject.Properties.Name)
if ($names.Count -ne 29) { throw 'FreeImage notice inventory differs from reviewed set' }
foreach ($name in $names) {
    if ([IO.Path]::IsPathRooted($name) -or $name -match '(^|/)\.\.(/|$)|[:\\]') { throw 'Unsafe FreeImage notice path' }
    $file = Get-Item -LiteralPath (Join-Path $source $name)
    $pin = $manifest.files.$name
    if ($file.Length -ne $pin.bytes -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne $pin.sha256) { throw "FreeImage notice identity mismatch: $name" }
}
$payload = @{}
foreach ($name in $names) { $payload[$name] = Join-Path $source $name }
$archiveName = "freeimage-$($component.version)-$($component.archive.sha256.Substring(0,12).ToLowerInvariant()).zip"
$archive = Join-Path (Join-Path $PSScriptRoot 'deps-cache') $archiveName
$archiveFile = Get-Item -LiteralPath $archive
if ($archiveFile.Length -ne $component.archive.bytes -or (Get-FileHash -LiteralPath $archive).Hash -ne $component.archive.sha256) { throw 'FreeImage source archive mismatch' }
$payload['source/FreeImage3180.zip'] = $archive
foreach ($patch in $component.patches) {
    if ([IO.Path]::IsPathRooted($patch.file) -or $patch.file -match '(^|/)\.\.(/|$)|[:\\]') { throw 'Unsafe patch path' }
    $patchPath = Join-Path $PSScriptRoot $patch.file
    if ((Get-FileHash -LiteralPath $patchPath).Hash -ne $patch.sha256) { throw 'FreeImage patch mismatch' }
    $payload[('source/'+$patch.file)] = $patchPath
}
$payload['SOURCE.md'] = Join-Path $source 'SOURCE.md'
foreach ($name in @('BuildDeps.ps1','TestDepsLock.ps1','deps.lock.json','deps.lock.schema.json','modern-bgfx.lock.json')) {
    $payload[('source/build-support/'+$name)] = Join-Path $PSScriptRoot $name
}
$identities = @{}
foreach ($entry in $payload.GetEnumerator()) {
    $file = Get-Item -LiteralPath $entry.Value
    $identities[$entry.Key] = @{ bytes=$file.Length; sha256=(Get-FileHash -LiteralPath $file.FullName).Hash }
}
foreach ($name in $payload.Keys) {
    $target = Join-Path (Join-Path $BinaryRoot 'licenses/freeimage') $name
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $payload[$name] -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target).Hash -ne $identities[$name].sha256) { throw 'FreeImage staged source/notice mismatch' }
}
@{schemaVersion=1;source=$component.archive;patches=$component.patches;files=$identities} | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $BinaryRoot 'licenses/freeimage/source/bundle-manifest.json')
Write-Output 'Installed FreeImage notices, acknowledgement, pinned source archive, patches and build-support record'
