[CmdletBinding()]
param(
    [string]$BinaryRoot = '',
    [string]$PolicyPath = '',
    [switch]$PayloadOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $BinaryRoot) { $BinaryRoot = $PSScriptRoot }
if (-not $PolicyPath) { $PolicyPath = Join-Path $PSScriptRoot 'RuntimePrerequisites.json' }
$policy = Get-Content -LiteralPath $PolicyPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($policy.schemaVersion -ne 1 -or $policy.architecture -ne 'x64') { throw 'Unsupported runtime prerequisite policy.' }
$expectedDlls = @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
if (@($policy.requiredDlls).Count -ne 4 -or @($expectedDlls | Where-Object { $_ -notin $policy.requiredDlls }).Count -ne 0 -or
    @($policy.payloads).Count -ne 2 -or 'mono-2.0-sgen.dll' -notin $policy.payloads.name -or 'OpenAL32.dll' -notin $policy.payloads.name) { throw 'Incomplete runtime prerequisite policy.' }
$minimum = [version]$policy.minimumVersion
$results = @()
$payloadResults = @()
$passed = $true
foreach ($pin in $policy.payloads) {
    if ($pin.name -notmatch '^[A-Za-z0-9_.-]+\.dll$' -or @($pin.allowedSha256).Count -eq 0) { throw 'Invalid payload policy.' }
    foreach ($digest in $pin.allowedSha256) { if ($digest -notmatch '^[0-9a-fA-F]{64}$') { throw 'Invalid payload hash.' } }
    $file = Join-Path $BinaryRoot $pin.name
    $actual = if (Test-Path -LiteralPath $file -PathType Leaf) { (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
    $ok = $null -ne $actual -and $actual -in $pin.allowedSha256
    if (-not $ok) { $passed = $false }
    $payloadResults += [pscustomobject]@{ name = $pin.name; sha256 = $actual; passed = $ok }
}
if (-not $PayloadOnly) {
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'The candidate requires 64-bit Windows.' }
    # A 32-bit PowerShell on 64-bit Windows must inspect the real 64-bit system directory.
    $systemDirectory = Join-Path $env:SystemRoot $(if ([Environment]::Is64BitProcess) { 'System32' } else { 'Sysnative' })
    foreach ($name in $policy.requiredDlls) {
        if ($name -notmatch '^[A-Za-z0-9_.-]+\.dll$') { throw 'Invalid runtime DLL name.' }
        # Check directories used by the shipped native processes before System32.
        # A bad local shadow must not be masked by a good system installation.
        foreach ($processDirectory in @($BinaryRoot, (Join-Path $BinaryRoot 'Mono/bin'))) {
            # The pinned Mono launcher/runtime import VCRUNTIME140 only; OpenAL at
            # the application root imports all four policy DLLs.
            if ($processDirectory -ne $BinaryRoot -and $name -ne 'vcruntime140.dll') { continue }
            $candidates = @((Join-Path $processDirectory $name), (Join-Path $systemDirectory $name))
            $file = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
            $actualVersion = $null
            $machine = $null
            $reason = 'Missing DLL'
            $ok = $false
            if ($file) {
                try {
                    $stream = [IO.File]::OpenRead($file)
                    $reader = New-Object IO.BinaryReader($stream)
                    try {
                        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'Not a PE file' }
                        $stream.Position = 0x3c
                        $offset = $reader.ReadInt32()
                        if ($offset -lt 64 -or $offset -gt $stream.Length - 6) { throw 'Invalid PE offset' }
                        $stream.Position = $offset
                        if ($reader.ReadUInt32() -ne 0x4550) { throw 'Invalid PE signature' }
                        $machine = $reader.ReadUInt16()
                    } finally { $reader.Dispose(); $stream.Dispose() }
                    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($file)
                    $actualVersion = [version]('{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart)
                    $ok = $machine -eq 0x8664 -and $actualVersion -ge $minimum
                    $reason = if ($machine -ne 0x8664) { 'Wrong architecture; x64 required' } elseif ($actualVersion -lt $minimum) { 'Runtime is older than the verified build tools' } else { 'Version and architecture satisfy policy' }
                } catch { $reason = $_.Exception.Message }
            }
            if (-not $ok) { $passed = $false }
            $results += [pscustomobject]@{ name = $name; processDirectory = $processDirectory; path = $file; version = [string]$actualVersion; machine = $machine; passed = $ok; reason = $reason }
        }
    }
}
[pscustomobject]@{
    passed = $passed; payloadOnly = [bool]$PayloadOnly; minimumVersion = [string]$minimum
    payloads = $payloadResults; runtime = $results
    scope = 'Read-only file/version/architecture preflight; does not load DLLs, start apps or prove visual/runtime behavior.'
    documentationUrl = $policy.documentationUrl
} | ConvertTo-Json -Depth 6
if (-not $passed) { throw 'Runtime prerequisites failed. See the results above and RUNTIME-REQUIREMENTS.md.' }
