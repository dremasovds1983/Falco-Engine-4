[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot, [string]$DependencyLockPath = "")
$ErrorActionPreference = 'Stop'
$folder = Join-Path $PSScriptRoot 'ThirdParty/openal-hrtf'
$manifestPath = Join-Path $folder 'PROVENANCE.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or $manifest.kind -ne 'embedded-hrtf-notice' -or
    @($manifest.files.PSObject.Properties).Count -ne 1) { throw 'HRTF notice inventory mismatch' }
if ($DependencyLockPath) {
    $component = (Get-Content -LiteralPath $DependencyLockPath -Raw | ConvertFrom-Json).components.'openal-soft'
    if ($component.version -ne $manifest.openalVersion -or $component.archive.sha256 -ne $manifest.sourceArchive.sha256) { throw 'HRTF source version/hash mismatch.' }
    $outputs = @($component.outputs | Where-Object path -eq 'out/deps/w9/bin/OpenAL32.dll')
    if ($outputs.Count -ne 1) { throw 'Expected exactly one OpenAL runtime output.' }
    $manifest.dllSha256 = $outputs[0].sha256
}
$dll = Join-Path $BinaryRoot 'OpenAL32.dll'
if ((Get-FileHash -LiteralPath $dll).Hash -ne $manifest.dllSha256) { throw 'HRTF OpenAL binary identity mismatch' }
foreach ($entry in $manifest.files.PSObject.Properties) {
    if ($entry.Name -ne 'NOTICE.md') { throw 'Unsafe HRTF notice path' }
    $source = Join-Path $folder $entry.Name
    if ((Get-FileHash -LiteralPath $source).Hash -ne $entry.Value) { throw 'HRTF notice identity mismatch' }
}
$destination = Join-Path $BinaryRoot 'licenses/openal-hrtf'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($name in @('NOTICE.md','PROVENANCE.json')) {
    $source = Join-Path $folder $name
    $target = Join-Path $destination $name
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $source).Hash) { throw 'HRTF notice copy mismatch' }
}
if ($DependencyLockPath) {
    $manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $destination 'PROVENANCE.json') -Encoding UTF8
}
Write-Host 'Installed default HRTF attribution'
