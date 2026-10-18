[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Clipper2Source.ps1')
$bundle = Get-FalcoClipper2Bundle
$destination = Join-Path $BinaryRoot 'licenses/clipper2'
# Include the small source subset as well as LICENSE/README/provenance so the
# original author notices remain attached to each source file.
foreach ($name in @($bundle.manifest.files.PSObject.Properties.Name) + 'PROVENANCE.json') {
    $source = Join-Path $bundle.root $name
    $target = Join-Path $destination $name
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $source).Hash) { throw 'Clipper2 notice copy mismatch' }
}
Write-Host 'Installed Clipper2 source attribution and Boost license'
