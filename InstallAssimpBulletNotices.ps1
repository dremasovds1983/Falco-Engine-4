[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot)
$ErrorActionPreference = 'Stop'
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deps.lock.json') -Raw | ConvertFrom-Json
$payload = @{}
foreach ($name in @('assimp','bullet')) {
    $pin = $lock.components.$name
    $folder = Join-Path $PSScriptRoot "ThirdParty/$name-$($pin.version)"
    $manifest = Get-Content -LiteralPath (Join-Path $folder 'PROVENANCE.json') -Raw | ConvertFrom-Json
    $licenseName = $pin.licenseFiles[0].pathInSource
    if ($manifest.componentId -ne $name -or $manifest.version -ne $pin.version -or
        $manifest.source.url -ne $pin.archive.url -or $manifest.source.sha256 -ne $pin.archive.sha256 -or
        $manifest.source.bytes -ne $pin.archive.bytes -or
        $manifest.files.$licenseName.sha256 -ne $pin.licenseFiles[0].sha256) { throw "Source notice provenance mismatch: $name" }
    if (@($manifest.files.PSObject.Properties).Count -ne @{assimp=179; bullet=84}[$name]) { throw "Source notice inventory mismatch: $name" }
    foreach ($property in $manifest.files.PSObject.Properties) {
        $relative = $property.Name
        if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|/)\.\.(/|$)|[:\\]') { throw 'Unsafe source notice path' }
        $file = Get-Item -LiteralPath (Join-Path $folder $relative)
        if ($file.Length -ne $property.Value.bytes -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne $property.Value.sha256) { throw "Source notice identity mismatch: $relative" }
        $payload["$name/$relative"] = $file.FullName
    }
    $payload["$name/PROVENANCE.json"] = Join-Path $folder 'PROVENANCE.json'
}
$payload['README.md'] = Join-Path $PSScriptRoot 'ThirdParty/assimp-bullet/README.md'
$identities = @{}
foreach ($entry in $payload.GetEnumerator()) {
    $file = Get-Item -LiteralPath $entry.Value
    $identities[$entry.Key] = @{bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
}
# No writes until both complete packages have passed preflight.
foreach ($entry in $payload.GetEnumerator()) {
    $target = Join-Path (Join-Path $BinaryRoot 'licenses/assimp-bullet') $entry.Key
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target).Hash -ne $identities[$entry.Key].sha256) { throw 'Source notice copy mismatch' }
}
@{schemaVersion=1;files=$identities} | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $BinaryRoot 'licenses/assimp-bullet/bundle-manifest.json')
Write-Output 'Installed 263 Assimp/Bullet source notices and provenance'
