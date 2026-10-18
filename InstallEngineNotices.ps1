[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot)
$ErrorActionPreference = 'Stop'
$folder = Join-Path $PSScriptRoot 'ThirdParty/engine-notices'
$manifest = Get-Content -LiteralPath (Join-Path $folder 'PROVENANCE.json') -Raw | ConvertFrom-Json
function Assert-NoticePath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path) -or $Path -match '(^|/)\.\.(/|$)|[:\\]') { throw 'Unsafe local notice path' }
}
if ($manifest.kind -ne 'local-source-notices' -or $manifest.schemaVersion -ne 1 -or
    @($manifest.files.PSObject.Properties).Count -ne 21 -or @($manifest.sources.PSObject.Properties).Count -ne 101) { throw 'Local notice inventory mismatch' }
foreach ($property in $manifest.sources.PSObject.Properties) {
    Assert-NoticePath $property.Name
    if ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $property.Name)).Hash -ne $property.Value) { throw "Local vendor source changed: $($property.Name)" }
}
$payload = @{}
foreach ($property in $manifest.files.PSObject.Properties) {
    Assert-NoticePath $property.Name
    $file = Get-Item -LiteralPath (Join-Path $folder $property.Name)
    if ($file.Length -ne $property.Value.bytes -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne $property.Value.sha256) { throw 'Local notice identity mismatch' }
    $payload[$property.Name] = $file.FullName
}
$payload['PROVENANCE.json'] = Join-Path $folder 'PROVENANCE.json'
$payload['README.md'] = Join-Path $folder 'README.md'
$payload['dirent-provenance.json'] = Join-Path $PSScriptRoot 'ThirdParty/dirent-notice/PROVENANCE.json'
$payload['imgui-upstream-baselines.json'] = Join-Path $PSScriptRoot 'ThirdParty/dear-imgui-1.85-bgfx/UPSTREAM-BASELINES.json'
$identities = @{}
foreach ($entry in $payload.GetEnumerator()) {
    $file = Get-Item -LiteralPath $entry.Value
    $identities[$entry.Key] = @{bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
}
foreach ($entry in $payload.GetEnumerator()) {
    $target = Join-Path (Join-Path $BinaryRoot 'licenses/engine-local') $entry.Key
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target).Hash -ne $identities[$entry.Key].sha256) { throw 'Local notice copy mismatch' }
}
@{schemaVersion=1;files=$identities} | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $BinaryRoot 'licenses/engine-local/bundle-manifest.json')
Write-Output 'Installed 21 local vendor notices with source provenance'
