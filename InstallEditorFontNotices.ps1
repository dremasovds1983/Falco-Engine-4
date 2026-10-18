[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot)
$ErrorActionPreference = 'Stop'
$folder = Join-Path $PSScriptRoot 'ThirdParty/editor-font-notices'
$manifest = Get-Content -LiteralPath (Join-Path $folder 'PROVENANCE.json') -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or $manifest.kind -ne 'editor-font-notices' -or
    @($manifest.sources.PSObject.Properties).Count -ne 23 -or
    @($manifest.files.PSObject.Properties).Count -ne 5) { throw 'Editor font notice inventory mismatch' }
foreach ($entry in $manifest.sources.PSObject.Properties) {
    if ($entry.Name -notmatch '^Res/Editor/Fonts/[A-Za-z0-9-]+\.ttf$') { throw 'Unsafe font source path' }
    $source = Join-Path $PSScriptRoot $entry.Name
    $installed = Join-Path $BinaryRoot $entry.Name.Substring(4)
    if ((Get-FileHash -LiteralPath $source).Hash -ne $entry.Value -or
        (Get-FileHash -LiteralPath $installed).Hash -ne $entry.Value) { throw 'Editor font identity mismatch' }
}
foreach ($entry in $manifest.files.PSObject.Properties) {
    if ($entry.Name -notmatch '^[A-Za-z0-9.-]+\.(txt|md)$') { throw 'Unsafe font notice path' }
    if ((Get-FileHash -LiteralPath (Join-Path $folder $entry.Name)).Hash -ne $entry.Value) { throw 'Editor font notice identity mismatch' }
}
$destination = Join-Path $BinaryRoot 'licenses/editor-fonts'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($name in @($manifest.files.PSObject.Properties.Name) + @('PROVENANCE.json')) {
    $source = Join-Path $folder $name
    $target = Join-Path $destination $name
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $target).Hash) { throw 'Editor font notice copy mismatch' }
}
Write-Host 'Installed notices for 23 editor fonts'
