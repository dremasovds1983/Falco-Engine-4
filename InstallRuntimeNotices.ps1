[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot)
$ErrorActionPreference = 'Stop'
$sourceRoot = Join-Path $PSScriptRoot 'ThirdParty/mono-6.8.0.105'
$provenance = Get-Content -LiteralPath (Join-Path $sourceRoot 'PROVENANCE.json') -Raw | ConvertFrom-Json
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'deps.lock.json') -Raw | ConvertFrom-Json
$mono = $lock.components.'mono-msi'
if ($provenance.version -ne $mono.version -or $provenance.source.sha256 -ne $mono.sourceOffer.sha256 -or $provenance.source.url -ne $mono.sourceOffer.url) {
    throw 'Mono notice provenance differs from the pinned runtime source'
}
if ($provenance.files.LICENSE.sha256 -ne $mono.licenseFiles[0].sha256) { throw 'Mono LICENSE differs from lock' }
$names = @($provenance.files.PSObject.Properties.Name | Sort-Object)
if ('PATENTS.TXT' -notin $names) { throw 'Mono root PATENTS.TXT is required' }
$rootPatent = $provenance.supplementalSources.'PATENTS.TXT'
if ($rootPatent.commit -notmatch '^[0-9a-f]{40}$' -or $rootPatent.url -ne "https://raw.githubusercontent.com/mono/mono/$($rootPatent.commit)/PATENTS.TXT" -or $rootPatent.sha256 -ne $provenance.files.'PATENTS.TXT'.sha256) {
    throw 'Mono root patent source provenance mismatch'
}
foreach ($name in $names) {
    if ([IO.Path]::IsPathRooted($name) -or $name -match '(^|/)\.\.(/|$)|[:\\]') { throw 'Unsafe Mono notice path' }
    $file = Get-Item -LiteralPath (Join-Path $sourceRoot $name)
    $pin = $provenance.files.$name
    if ($file.Length -ne $pin.bytes -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne $pin.sha256) {
        throw "Mono notice identity mismatch: $name"
    }
}
$destination = Join-Path $BinaryRoot 'licenses/mono'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($name in $names) {
    $target = Join-Path $destination $name
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target).Hash -ne $provenance.files.$name.sha256) { throw "Mono staged notice mismatch: $name" }
}
Write-Output "Installed $($names.Count) verified Mono notice files (original paths retained)"
