[CmdletBinding()]
param([Parameter(Mandatory)][string]$BinaryRoot, [string]$DependencyLockPath = "")
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$policySource = Join-Path $PSScriptRoot 'RuntimePrerequisites.json'
$policyTarget = Join-Path $BinaryRoot 'RuntimePrerequisites.json'
if ($DependencyLockPath) {
    $policy = Get-Content -LiteralPath $policySource -Raw | ConvertFrom-Json
    $deps = Get-Content -LiteralPath $DependencyLockPath -Raw | ConvertFrom-Json
    foreach ($payload in $policy.payloads) {
        $component = if ($payload.name -eq 'OpenAL32.dll') { 'openal-soft' } else { 'mono-msi' }
        $relative = if ($payload.name -eq 'OpenAL32.dll') { 'out/deps/w9/bin/OpenAL32.dll' } else { 'out/deps/w8/mono-2.0-sgen.dll' }
        $outputs = @($deps.components.$component.outputs | Where-Object path -eq $relative)
        if ($outputs.Count -ne 1) { throw "Expected exactly one runtime output: $relative" }
        $payload.allowedSha256 = @(([string]$outputs[0].sha256).ToLowerInvariant())
    }
    $policy | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $policyTarget -Encoding UTF8
} else {
    Copy-Item -LiteralPath $policySource -Destination $policyTarget -Force
}
# Check against the source build's recorded output identities before shipping the checker.
& (Join-Path $PSScriptRoot 'TestRuntimePrerequisites.ps1') -BinaryRoot $BinaryRoot -PolicyPath $policyTarget -PayloadOnly
foreach ($name in @('TestRuntimePrerequisites.ps1', 'RUNTIME-REQUIREMENTS.md')) {
    $source = Join-Path $PSScriptRoot $name
    $target = Join-Path $BinaryRoot $name
    Copy-Item -LiteralPath $source -Destination $target -Force
    if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $target).Hash) { throw "Runtime prerequisite staging mismatch: $name" }
}
Write-Host 'Installed runtime prerequisite documentation and read-only preflight.'
