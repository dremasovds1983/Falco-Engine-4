[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot)
$ErrorActionPreference = 'Stop'
$fonts = Join-Path $BinaryRoot 'BuiltinResources/Fonts'
$obsolete = [IO.Path]::GetFullPath((Join-Path $fonts 'arial.ttf'))
$archived = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'Res/BuiltinResources/Fonts/arial.ttf'))
if ($obsolete -eq $archived) { throw 'Refusing to modify archived font input' }
$folder = Join-Path $PSScriptRoot 'ThirdParty/liberation-fonts-2.1.5'
$manifest = Get-Content -LiteralPath (Join-Path $folder 'PROVENANCE.json') -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or @($manifest.files.PSObject.Properties).Count -ne 3) { throw 'Default font manifest mismatch' }
$font = Join-Path $PSScriptRoot 'Res/BuiltinResources/Fonts/default.ttf'
if ((Get-FileHash -LiteralPath $font).Hash -ne $manifest.fontSha256) { throw 'Default font identity mismatch' }
foreach ($entry in $manifest.files.PSObject.Properties) {
    if ($entry.Name -notin @('LICENSE','AUTHORS','README.md')) { throw 'Unsafe default font notice path' }
    if ((Get-FileHash -LiteralPath (Join-Path $folder $entry.Name)).Hash -ne $entry.Value) { throw 'Default font notice identity mismatch' }
}
New-Item -ItemType Directory -Force -Path $fonts | Out-Null
$target = Join-Path $fonts 'default.ttf'
Copy-Item -LiteralPath $font -Destination $target -Force
if ((Get-FileHash -LiteralPath $target).Hash -ne $manifest.fontSha256) { throw 'Default font copy mismatch' }
# Remove only the obsolete generated payload, never the archived input.
if (Test-Path -LiteralPath $obsolete) { Remove-Item -LiteralPath $obsolete -Force }
$destination = Join-Path $BinaryRoot 'licenses/liberation-fonts'
New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($name in @('LICENSE','AUTHORS','README.md','PROVENANCE.json')) {
    $source = Join-Path $folder $name
    $target = Join-Path $destination $name
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $target).Hash) { throw 'Default font notice copy mismatch' }
}
Write-Host 'Installed Liberation Sans default font'
