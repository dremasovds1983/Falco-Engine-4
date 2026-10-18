# Source notice superset for the pinned graphics stack, including its tools.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot)
$ErrorActionPreference = 'Stop'
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deps.lock.json') -Raw | ConvertFrom-Json
$payload = @{}
foreach ($name in @('bx','bimg','bgfx')) {
    $folder = Join-Path $PSScriptRoot "ThirdParty/$name-production-v1"
    $manifest = Get-Content -LiteralPath (Join-Path $folder 'PROVENANCE.json') -Raw | ConvertFrom-Json
    $pin = $lock.components.$name
    if ($manifest.componentId -ne $name -or $manifest.version -ne $pin.version -or
        $manifest.commit -ne $pin.commit -or $manifest.source.url -ne $pin.archive.url -or
        $manifest.source.sha256 -ne $pin.archive.sha256 -or $manifest.source.bytes -ne $pin.archive.bytes -or
        $manifest.files.LICENSE.sha256 -ne $pin.licenseFiles[0].sha256) { throw "Graphics notice provenance mismatch: $name" }
    $expectedCount = @{bx=3; bimg=16; bgfx=70}[$name]
    if (@($manifest.files.PSObject.Properties).Count -ne $expectedCount) { throw "Graphics notice inventory mismatch: $name" }
    foreach ($property in $manifest.files.PSObject.Properties) {
        $relative = $property.Name
        if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|/)\.\.(/|$)|[:\\]') { throw 'Unsafe graphics notice path' }
        $file = Get-Item -LiteralPath (Join-Path $folder $relative)
        if ($file.Length -ne $property.Value.bytes -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne $property.Value.sha256) { throw "Graphics notice identity mismatch: $relative" }
        $payload["$name/$relative"] = $file.FullName
    }
    $payload["$name/PROVENANCE.json"] = Join-Path $folder 'PROVENANCE.json'
}
# Catch's source header refers to BSL-1.0; reuse the exact independently pinned text.
$boostLicense = Join-Path $PSScriptRoot 'ThirdParty/bgfx-stack/BSL-1.0.txt'
if ((Get-FileHash -LiteralPath $boostLicense).Hash -ne $lock.components.boost.licenseFiles[0].sha256) { throw 'BSL text mismatch' }
$payload['BSL-1.0.txt'] = $boostLicense
$payload['README.md'] = Join-Path $PSScriptRoot 'ThirdParty/bgfx-stack/README.md'
$identities = @{}
foreach ($entry in $payload.GetEnumerator()) {
    $file = Get-Item -LiteralPath $entry.Value
    $identities[$entry.Key] = @{bytes=$file.Length; sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
}
# Complete preflight above before writing any output.
foreach ($entry in $payload.GetEnumerator()) {
    $target = Join-Path (Join-Path $BinaryRoot 'licenses/bgfx-stack') $entry.Key
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target).Hash -ne $identities[$entry.Key].sha256) { throw 'Graphics notice copy mismatch' }
}
@{schemaVersion=1;files=$identities} | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $BinaryRoot 'licenses/bgfx-stack/bundle-manifest.json')
Write-Output 'Installed 89 pinned graphics source notices, provenance and referenced BSL text'
