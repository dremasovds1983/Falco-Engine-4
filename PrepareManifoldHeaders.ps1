[CmdletBinding()]
param([string]$DependencyLockPath = (Join-Path $PSScriptRoot 'deps.lock.json'))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$lock = Get-Content -LiteralPath $DependencyLockPath -Raw | ConvertFrom-Json
$pin = $lock.components.manifold
$archiveName = 'manifold-{0}-{1}.tar.gz' -f $pin.version, $pin.archive.sha256.Substring(0,12).ToLowerInvariant()
$archive = Join-Path $PSScriptRoot "deps-cache/$archiveName"
if (!(Test-Path -LiteralPath $archive)) { throw "Pinned Manifold archive missing: $archive. Restore it to deps-cache first." }
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $pin.archive.sha256) { throw 'Manifold archive hash mismatch.' }
$library = Join-Path $PSScriptRoot $pin.outputs[0].path
if ((Get-FileHash -LiteralPath $library -Algorithm SHA256).Hash -ne $pin.outputs[0].sha256) { throw 'Manifold library hash mismatch.' }
# W10 originally staged only the library and license. Keep that pinned wave intact.
$destination = Join-Path $PSScriptRoot 'out/falco/manifold-headers'
New-Item -ItemType Directory -Force $destination | Out-Null
& tar -xf $archive -C $destination --strip-components 2 "manifold-$($pin.version)/include/manifold"
if ($LASTEXITCODE -ne 0) { throw 'Manifold header extraction failed.' }
Write-Output "PASS: Manifold $($pin.version) headers restored from pinned archive; library hash verified."
