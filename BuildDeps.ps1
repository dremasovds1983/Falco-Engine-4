[CmdletBinding()]
param(
    [ValidateSet("Check", "Fetch", "Build", "All", "SelfTest")]
    [string]$Mode = "Check",
    [string]$Wave = "all",
    [switch]$AllowNetwork,
    [string]$CacheDir = "",
    [switch]$Rebuild,
    [string]$LockPath = "",
    [switch]$AllowToolchainRebuild,
    [switch]$KeepWork
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = $PSScriptRoot
if (-not $LockPath) { $LockPath = Join-Path $script:RepoRoot "deps.lock.json" }
if (-not $CacheDir) { $CacheDir = Join-Path $script:RepoRoot "deps-cache" }
$script:OutRoot = Join-Path $script:RepoRoot "out\deps"
$script:StatePath = Join-Path $script:OutRoot "_state.json"
$script:StateSchemaVersion = 2
$script:RunId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), $PID
$script:RunRoot = Join-Path $script:OutRoot "_runs\$script:RunId"
# F-W2-63 / DEC-068. The work root must be run-invariant: cl.exe embeds source paths into
# objects (measured on W4 through pin drift of +/-2 bytes tracking the run-id length), so a
# work root named after the run id made every rebuild's bytes depend on the process id's
# digit count - the second half of F-W2-62, masked in the probe because it extracted its
# source once into a fixed directory. Scratch collisions between sequential runs are handled
# by Reset-WorkScratch at every extraction site. Run-scoped artifacts (logs, reports) stay
# under the run-id-named run root above.
$script:WorkRoot = Join-Path $script:OutRoot "_work\fixed"
$script:ReportEvents = New-Object System.Collections.Generic.List[object]
$script:ToolchainIdentity = $null

function Stop-BuildDeps([string]$Message) {
    throw "BuildDeps failed: $Message"
}

function Add-ReportEvent([string]$Kind, [string]$Component, [string]$Status, [string]$Message, $Data = $null) {
    $script:ReportEvents.Add([pscustomobject]@{
        timestamp = (Get-Date).ToUniversalTime().ToString("o")
        kind = $Kind
        component = $Component
        status = $Status
        message = $Message
        data = $Data
    })
}

function Join-PathLiteral([string]$Base, [string]$Child) {
    # F-W2-34. Windows PowerShell 5.1 resolves the drive qualifier of Join-Path's first
    # argument through the PSDrive provider and fails with "Cannot find drive. A drive with
    # the name 'X' does not exist." when that drive is not mounted; PowerShell 7 does not,
    # and on a non-Windows host Join-Path additionally emits '/' as the separator. A staging
    # plan is pure path arithmetic over roots that need not exist yet, so it must not touch
    # the provider and must not depend on the host separator. Windows separators only.
    $left = $Base.TrimEnd([char]92, [char]47)
    $right = $Child.TrimStart([char]92, [char]47)
    if ([string]::IsNullOrEmpty($left)) { return $right }
    if ([string]::IsNullOrEmpty($right)) { return $left }
    return $left + [string][char]92 + $right
}

function Get-NormalizedFullPath([string]$Path) {
    # F-W2-28. .NET path APIs resolve a relative path against [Environment]::CurrentDirectory,
    # which PowerShell does not keep in sync with Set-Location, so the same relative string
    # could normalise here and be created somewhere else by New-Item -Path. Every relative path
    # is therefore anchored to the repository root explicitly. All existing callers pass
    # absolute paths, so this changes no existing behaviour.
    $candidate = $Path
    if (-not [IO.Path]::IsPathRooted($candidate)) {
        $candidate = Join-Path $script:RepoRoot $candidate
    }
    return [IO.Path]::GetFullPath($candidate).TrimEnd('\', '/')
}

function Assert-PathUnder([string]$Path, [string]$AllowedRoot, [string]$Label) {
    $full = Get-NormalizedFullPath $Path
    $root = Get-NormalizedFullPath $AllowedRoot
    if ($full -ne $root -and -not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Stop-BuildDeps "$Label escapes allowed root '$root': $full"
    }
    return $full
}

function Assert-RelativeSafePath([string]$Path, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Path) -or [IO.Path]::IsPathRooted($Path) -or
        $Path -match "^[A-Za-z]:" -or $Path -match "(^|[\\/])\.\.([\\/]|$)") {
        Stop-BuildDeps "$Label is not a safe relative path: '$Path'"
    }
}

function Assert-PinnedSha256([string]$Sha256, [string]$Label) {
    if ($Sha256 -notmatch "^[A-Fa-f0-9]{64}$") {
        Stop-BuildDeps "$Label does not contain a production SHA-256 pin"
    }
}

function Get-FileIdentity([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Stop-BuildDeps "file not found: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    return [pscustomobject]@{
        bytes = [long]$item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
    }
}

function Test-FileIdentity([string]$Path, [long]$ExpectedBytes, [string]$ExpectedSha256, [switch]$QuarantineOnFailure) {
    Assert-PinnedSha256 $ExpectedSha256 "expected file identity"
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $identity = Get-FileIdentity $Path
    $valid = $identity.bytes -eq $ExpectedBytes -and $identity.sha256 -eq $ExpectedSha256.ToUpperInvariant()
    if (-not $valid -and $QuarantineOnFailure) {
        $cacheRoot = Assert-PathUnder $CacheDir $script:RepoRoot "cache directory"
        $verifiedPath = Assert-PathUnder $Path $cacheRoot "quarantine target"
        $quarantinePath = "$verifiedPath.quarantine-$script:RunId"
        if (Test-Path -LiteralPath $quarantinePath) {
            Stop-BuildDeps "quarantine target already exists: $quarantinePath"
        }
        Move-Item -LiteralPath $verifiedPath -Destination $quarantinePath
        Add-ReportEvent "cache" "" "QUARANTINED" "Cached file identity mismatch" @{
            original = $verifiedPath
            quarantine = $quarantinePath
            actualBytes = $identity.bytes
            actualSha256 = $identity.sha256
        }
    }
    return $valid
}

function Get-ArchiveExtension([string]$Url) {
    $path = ([Uri]$Url).AbsolutePath.ToLowerInvariant()
    foreach ($extension in @(".tar.gz", ".tar.xz", ".tgz", ".zip", ".msi", ".tar")) {
        if ($path.EndsWith($extension)) { return $extension }
    }
    return ".archive"
}

function Get-CachePath([string]$Id, $Component) {
    $safeId = ($Id -replace "[^A-Za-z0-9._-]", "_")
    $safeVersion = ([string]$Component.version -replace "[^A-Za-z0-9._-]", "_")
    Assert-PinnedSha256 $Component.archive.sha256 "components.$Id.archive.sha256"
    $extension = Get-ArchiveExtension $Component.archive.url
    return Join-Path $CacheDir ("{0}-{1}-{2}{3}" -f $safeId, $safeVersion, $Component.archive.sha256.Substring(0, 12).ToLowerInvariant(), $extension)
}

function Invoke-LockValidation([string]$Path) {
    $validator = Join-Path $script:RepoRoot "TestDepsLock.ps1"
    $legacy = Join-Path $script:RepoRoot "modern-bgfx.lock.json"
    $schema = Join-Path $script:RepoRoot "deps.lock.schema.json"
    $result = & powershell -NoProfile -ExecutionPolicy Bypass -File $validator -LockPath $Path -SchemaPath $schema -LegacyLockPath $legacy 2>&1
    if ($LASTEXITCODE -ne 0) {
        Stop-BuildDeps "lock validation rejected '$Path': $($result | Out-String)"
    }
    Add-ReportEvent "lock" "" "PASS" ($result | Out-String).Trim()
}

function Read-DependencyLock([string]$Path) {
    Invoke-LockValidation $Path
    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch {
        Stop-BuildDeps "cannot parse lock after validation: $($_.Exception.Message)"
    }
}

function Get-SelectedComponentNames($Lock, [string]$RequestedWave) {
    $validWaves = @("all", "core", "W0", "W1", "W2", "W3", "W4", "W5", "W6", "W7", "W8", "W8b", "W9", "W10")
    if ($RequestedWave -notin $validWaves) {
        Stop-BuildDeps "unknown wave '$RequestedWave'; expected one of: $($validWaves -join ', ')"
    }
    $names = @($Lock.components.PSObject.Properties.Name)
    if ($RequestedWave -eq "all") { return $names }
    return @($names | Where-Object { $Lock.components.$_.wave -eq $RequestedWave })
}

function Get-TopologicalOrder($Components, [string[]]$SelectedNames) {
    $selected = @{}
    foreach ($name in $SelectedNames) { $selected[$name] = $true }
    $state = @{}
    $result = New-Object System.Collections.Generic.List[string]

    function Visit-Node([string]$Name, [string[]]$Trail) {
        if ($state[$Name] -eq 1) { Stop-BuildDeps "dependency cycle: $(($Trail + $Name) -join ' -> ')" }
        if ($state[$Name] -eq 2) { return }
        $state[$Name] = 1
        foreach ($dependency in @($Components.$Name.dependsOn)) {
            if ($selected.ContainsKey($dependency)) { Visit-Node $dependency ($Trail + $Name) }
        }
        $state[$Name] = 2
        $result.Add($Name)
    }

    foreach ($name in $SelectedNames) { Visit-Node $name @() }
    return @($result)
}

function Test-ArchiveEntry([string]$EntryPath, [bool]$IsLink, [string[]]$ExpectedExecutables) {
    Assert-RelativeSafePath $EntryPath "archive entry"
    if ($EntryPath.Length -gt 240) { Stop-BuildDeps "archive entry exceeds 240 characters: '$EntryPath'" }
    if ($IsLink) { Stop-BuildDeps "archive contains a link entry: '$EntryPath'" }
    $extension = [IO.Path]::GetExtension($EntryPath).ToLowerInvariant()
    if ($extension -in @(".exe", ".dll", ".bat", ".cmd", ".ps1")) {
        $normalized = $EntryPath.Replace('\', '/')
        # F-W2-61: a $null expected list must behave as an empty one - the honest refusal
        # below - never as a null-method crash inside this pipeline.
        $allowed = @(@($ExpectedExecutables) | Where-Object { $null -ne $_ } | ForEach-Object { $_.Replace('\', '/') })
        if ($normalized -notin $allowed) {
            Stop-BuildDeps "archive contains unexpected executable payload: '$EntryPath'"
        }
    }
}

function Test-ZipArchive([string]$ArchivePath, [string[]]$ExpectedExecutables) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            $unixType = ($entry.ExternalAttributes -shr 16) -band 0xF000
            Test-ArchiveEntry $entry.FullName ($unixType -eq 0xA000) $ExpectedExecutables
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Get-ArchiveExcludedPrefixes([string]$ComponentId) {
    # W4 / DEC-038. The SDL GitHub tarball carries exactly two symlink entries, both confined to
    # the Android packaging directory that the Windows build never reads. Rather than weakening
    # the link guard, the whole directory is declared excluded: its entries are neither checked
    # nor extracted, so a link never materialises on disk. The list is hard-coded per component
    # for the same reason Get-ClSourcePlan hard-codes its layout: the lock schema is frozen.
    switch ($ComponentId) {
        "sdl2" { return @("SDL-release-2.0.12/android-project-ant/") }
    }
    return @()
}

function Test-ArchiveEntryExcluded([string]$EntryPath, [string[]]$ExcludedPrefixes) {
    $normalized = $EntryPath.Replace('\', '/')
    foreach ($prefix in @($ExcludedPrefixes)) {
        if (-not $prefix) { continue }
        $p = ([string]$prefix).Replace('\', '/')
        if ($normalized.StartsWith($p, [StringComparison]::OrdinalIgnoreCase)) { return $true }
        if (($normalized + "/") -eq $p) { return $true }
    }
    return $false
}

function Test-TarArchive([string]$ArchivePath, [string[]]$ExpectedExecutables, [string[]]$ExcludedPrefixes = @()) {
    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
    if (-not $tar) { Stop-BuildDeps "tar.exe is required to inspect tar archives" }
    # Windows bsdtar and supported GNU tar emit the entry type as the first
    # character and preserve the complete path in field 9. SelfTest exercises
    # traversal, symlink and hardlink headers against the installed tar.exe.
    $lines = & $tar.Source -tvf $ArchivePath 2>&1
    if ($LASTEXITCODE -ne 0) { Stop-BuildDeps "tar listing failed: $($lines | Out-String)" }
    foreach ($line in @($lines)) {
        $text = [string]$line
        $isLink = $text.Length -gt 0 -and $text[0] -in @('l', 'h')
        $entryPath = ($text -split "\s+", 9)[-1]
        if ($entryPath -match " -> ") { $entryPath = ($entryPath -split " -> ", 2)[0] }
        if (Test-ArchiveEntryExcluded $entryPath $ExcludedPrefixes) { continue }
        Test-ArchiveEntry $entryPath $isLink $ExpectedExecutables
    }
}

function Assert-FreshEmptyDirectory([string]$Path) {
    $safe = Assert-PathUnder $Path $script:WorkRoot "work directory"
    if (Test-Path -LiteralPath $safe) {
        if ((Get-ChildItem -LiteralPath $safe -Force | Select-Object -First 1)) {
            Stop-BuildDeps "extraction directory is not empty: $safe"
        }
    }
    else {
        New-Item -ItemType Directory -Path $safe -Force | Out-Null
    }
    return $safe
}

function Reset-WorkScratch([string]$Path) {
    # F-W2-63 / DEC-068. The work root is run-invariant, so an extraction destination may be
    # occupied by an earlier run's tree; an honest rebuild must not be stopped by its own
    # predecessor. The destination is driver-owned scratch: it is wiped, never merged. The
    # path is anchored under the work root before anything is removed.
    $safe = Assert-PathUnder $Path $script:WorkRoot "work scratch directory"
    if (Test-Path -LiteralPath $safe) {
        Remove-Item -LiteralPath $safe -Recurse -Force
    }
    return $safe
}

function Assert-NoReparsePoints([string]$Root) {
    foreach ($item in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Stop-BuildDeps "extracted tree contains a reparse point: $($item.FullName)"
        }
    }
}

function Assert-LicenseFiles($Component, [string]$SourceRoot, [string]$ComponentId) {
    foreach ($licenseFile in @($Component.licenseFiles)) {
        Assert-RelativeSafePath $licenseFile.pathInSource "components.$ComponentId.licenseFiles.pathInSource"
        $path = Join-Path $SourceRoot $licenseFile.pathInSource
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            if ((@($licenseFile.PSObject.Properties.Name) -contains "optional") -and $licenseFile.optional) { continue }
            Stop-BuildDeps "components.$ComponentId license file not found after extraction: $($licenseFile.pathInSource)"
        }
        Assert-PinnedSha256 $licenseFile.sha256 "components.$ComponentId license SHA-256"
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $licenseFile.sha256) {
            Stop-BuildDeps "components.$ComponentId license file SHA-256 mismatch: $($licenseFile.pathInSource)"
        }
    }
}

function Assert-ExpectedExecutableInventory([string]$SourceRoot, [string[]]$ExpectedExecutables, [string]$ComponentId) {
    $expected = @{}
    foreach ($path in $ExpectedExecutables) {
        Assert-RelativeSafePath $path "components.$ComponentId.expectedExecutables[]"
        $expected[$path.Replace('\', '/').ToLowerInvariant()] = $true
    }
    $actual = @()
    foreach ($file in Get-ChildItem -LiteralPath $SourceRoot -Recurse -Force -File) {
        if ($file.Extension.ToLowerInvariant() -in @(".exe", ".dll", ".bat", ".cmd", ".ps1")) {
            $relative = $file.FullName.Substring((Get-NormalizedFullPath $SourceRoot).Length).TrimStart('\', '/').Replace('\', '/')
            $actual += $relative
            if (-not $expected.ContainsKey($relative.ToLowerInvariant())) {
                Stop-BuildDeps "components.$ComponentId extracted unexpected executable payload: '$relative'"
            }
        }
    }
    foreach ($path in $expected.Keys) {
        if ($path -notin @($actual | ForEach-Object { $_.ToLowerInvariant() })) {
            Stop-BuildDeps "components.$ComponentId expected executable is missing after extraction: '$path'"
        }
    }
    Add-ReportEvent "executable-inventory" $ComponentId "PASS" "Executable inventory matches lock" @{ files = $actual }
}

function Assert-PatchFiles($Component, [string]$ComponentId) {
    foreach ($patch in @($Component.patches)) {
        Assert-RelativeSafePath $patch.file "components.$ComponentId.patches.file"
        Assert-PinnedSha256 $patch.sha256 "components.$ComponentId.patches.sha256"
        $patchPath = Join-Path $script:RepoRoot $patch.file
        if (-not (Test-Path -LiteralPath $patchPath -PathType Leaf)) {
            Stop-BuildDeps "components.$ComponentId patch file not found: $($patch.file)"
        }
        if ((Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash -ne $patch.sha256) {
            Stop-BuildDeps "components.$ComponentId patch SHA-256 mismatch: $($patch.file)"
        }
    }
}

function Resolve-ArchiveSourceRoot($Component, [string]$ExtractionRoot) {
    $requiredLicenses = @($Component.licenseFiles | Where-Object {
        (@($_.PSObject.Properties.Name) -notcontains "optional") -or -not $_.optional
    })
    $licensesAtRoot = @($requiredLicenses | Where-Object {
        Test-Path -LiteralPath (Join-Path $ExtractionRoot $_.pathInSource) -PathType Leaf
    }).Count -eq $requiredLicenses.Count
    if ($licensesAtRoot) { return $ExtractionRoot }

    $children = @(Get-ChildItem -LiteralPath $ExtractionRoot -Directory -Force)
    if ($children.Count -eq 1) {
        $candidate = $children[0].FullName
        $licensesAtCandidate = @($requiredLicenses | Where-Object {
            Test-Path -LiteralPath (Join-Path $candidate $_.pathInSource) -PathType Leaf
        }).Count -eq $requiredLicenses.Count
        if ($licensesAtCandidate) { return $candidate }
    }
    return $ExtractionRoot
}

function Invoke-VerifiedPatches($Component, [string]$SourceRoot, [string]$ComponentId) {
    Assert-PatchFiles $Component $ComponentId
    if (@($Component.patches).Count -eq 0) { return }
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if (-not $git) { Stop-BuildDeps "git.exe is required to apply declared patches for components.$ComponentId" }
    foreach ($patch in @($Component.patches)) {
        $patchPath = Join-Path $script:RepoRoot $patch.file
        & $git.Source -C $SourceRoot apply --check --whitespace=nowarn $patchPath
        if ($LASTEXITCODE -ne 0) { Stop-BuildDeps "git apply --check failed for components.$ComponentId patch '$($patch.file)'" }
        & $git.Source -C $SourceRoot apply --whitespace=nowarn $patchPath
        if ($LASTEXITCODE -ne 0) { Stop-BuildDeps "git apply failed for components.$ComponentId patch '$($patch.file)'" }
        Add-ReportEvent "patch" $ComponentId "APPLIED" $patch.reason @{ file = $patch.file; sha256 = $patch.sha256 }
    }
}

function Expand-VerifiedArchive([string]$ArchivePath, [string]$Destination, $Component, [string]$ComponentId) {
    # F-W2-61: this used to be `$expected = if (...) { @(...) } else { @() }`. Assigning from
    # an if whose branch emits an EMPTY array goes through pipeline semantics and yields $null,
    # not an empty array - and a [string[]] parameter keeps that $null, which then enumerates
    # as a single $null element inside Test-ArchiveEntry's allowed-list pipeline. The defect
    # slept from W2 on: every earlier archive either declared expectedExecutables (boost, sdl2)
    # or carried no executable-suffixed entry at all. The first W9 run woke it on
    # openal-soft's single logging/log.cmd. Explicit assignment, with nulls filtered.
    $expected = @()
    if (@($Component.PSObject.Properties.Name) -contains "expectedExecutables") {
        $expected = @(@($Component.expectedExecutables) | Where-Object { $null -ne $_ })
    }
    if (@($Component.PSObject.Properties.Name) -notcontains "archive") {
        Stop-BuildDeps "components.$ComponentId has no archive identity for extraction"
    }
    $cacheRoot = Assert-PathUnder $CacheDir $script:RepoRoot "cache directory"
    $verifiedArchive = Assert-PathUnder $ArchivePath $cacheRoot "archive input"
    if (-not (Test-FileIdentity $verifiedArchive $Component.archive.bytes $Component.archive.sha256 -QuarantineOnFailure)) {
        Stop-BuildDeps "components.$ComponentId archive changed after Fetch and was quarantined"
    }
    # F-W2-63: the destination may hold the previous run's tree under the run-invariant work
    # root; wipe it first (removal only - on identity rejection above nothing was created).
    # The emptiness gate stays as the last line of defense and for direct callers.
    $null = Reset-WorkScratch $Destination
    $destinationPath = Assert-FreshEmptyDirectory $Destination
    $excludedPrefixes = @(Get-ArchiveExcludedPrefixes $ComponentId)
    $lower = $verifiedArchive.ToLowerInvariant()
    if ($lower.EndsWith(".zip")) {
        if (@($excludedPrefixes).Count -gt 0) {
            Stop-BuildDeps ("components.$ComponentId declares excluded archive prefixes, but the zip " +
                "extractor cannot skip entries; the exclusion mechanism supports tar archives only")
        }
        Test-ZipArchive $verifiedArchive $expected
        [IO.Compression.ZipFile]::ExtractToDirectory($verifiedArchive, $destinationPath)
    }
    elseif ($lower.EndsWith(".tar") -or $lower.EndsWith(".tar.gz") -or $lower.EndsWith(".tgz") -or $lower.EndsWith(".tar.xz")) {
        Test-TarArchive $verifiedArchive $expected $excludedPrefixes
        $tar = (Get-Command tar.exe -ErrorAction Stop).Source
        $tarArguments = @("-xf", $verifiedArchive, "-C", $destinationPath)
        foreach ($prefix in $excludedPrefixes) {
            # bsdtar matches --exclude globs against full entry names; both the directory entry
            # itself and everything under it must be named.
            $trimmed = ([string]$prefix).TrimEnd('/')
            $tarArguments += @("--exclude", $trimmed, "--exclude", "$trimmed/*")
        }
        & $tar @tarArguments
        if ($LASTEXITCODE -ne 0) { Stop-BuildDeps "tar extraction failed for components.$ComponentId" }
    }
    elseif ($lower.EndsWith(".msi")) {
        Invoke-MsiAdministrativeExtract $verifiedArchive $destinationPath $ComponentId
    }
    else {
        Stop-BuildDeps "unsupported archive type: $ArchivePath"
    }
    Assert-NoReparsePoints $destinationPath
    Assert-ExpectedExecutableInventory $destinationPath $expected $ComponentId
    $sourceRoot = Resolve-ArchiveSourceRoot $Component $destinationPath
    Assert-LicenseFiles $Component $sourceRoot $ComponentId
    Invoke-VerifiedPatches $Component $sourceRoot $ComponentId
    return $sourceRoot
}

function Get-AuthenticodeDisposition([string]$Status, [string]$ComponentId) {
    switch ($Status) {
        "Valid" { return "PASS" }
        { $_ -in @("HashMismatch", "NotTrusted") } {
            Stop-BuildDeps "components.$ComponentId MSI Authenticode status is '$($_)'; refusing administrative extraction"
        }
        { $_ -in @("NotSigned", "UnknownError") } { return "WARN" }
        default { Stop-BuildDeps "components.$ComponentId MSI returned unsupported Authenticode status '$($_)'" }
    }
}

function Invoke-MsiAdministrativeExtract([string]$MsiPath, [string]$Destination, [string]$ComponentId) {
    $signature = Get-AuthenticodeSignature -LiteralPath $MsiPath
    $disposition = Get-AuthenticodeDisposition ([string]$signature.Status) $ComponentId
    if ($disposition -eq "PASS") {
            Add-ReportEvent "signature" $ComponentId "PASS" "MSI Authenticode signature is valid" @{
                subject = $signature.SignerCertificate.Subject
                thumbprint = $signature.SignerCertificate.Thumbprint
            }
    }
    else {
        Add-ReportEvent "signature" $ComponentId "WARN" "MSI Authenticode status is '$($signature.Status)'; SHA-256 pin remains authoritative."
    }
    $msiexec = Join-Path $env:SystemRoot "System32\msiexec.exe"
    if (-not (Test-Path -LiteralPath $msiexec -PathType Leaf)) { Stop-BuildDeps "msiexec.exe not found" }
    $logPath = Join-Path $script:RunRoot "$ComponentId-msiexec.log"
    $arguments = @("/a", "`"$MsiPath`"", "/qn", "TARGETDIR=`"$Destination`"", "/l*v", "`"$logPath`"")
    $process = Start-Process -FilePath $msiexec -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) { Stop-BuildDeps "msiexec /a failed for components.$ComponentId with exit code $($process.ExitCode)" }
}

function Invoke-Fetch($Lock, [string[]]$Names) {
    $missing = New-Object System.Collections.Generic.List[string]
    foreach ($name in $Names) {
        $component = $Lock.components.$name
        if (@($component.PSObject.Properties.Name) -notcontains "archive") { continue }
        if ($component.archive.sha256 -notmatch "^[A-Fa-f0-9]{64}$" -or [long]$component.archive.bytes -le 0) {
            Stop-BuildDeps "components.$name archive is not production-pinned; fill SHA-256 and bytes before Fetch"
        }
        if (-not $component.archive.url.StartsWith("https://", [StringComparison]::OrdinalIgnoreCase)) {
            Stop-BuildDeps "components.$name archive URL must use HTTPS"
        }
        $cachePath = Get-CachePath $name $component
        if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
            if (-not (Test-FileIdentity $cachePath $component.archive.bytes $component.archive.sha256 -QuarantineOnFailure)) {
                Stop-BuildDeps "components.$name cached archive was quarantined"
            }
            Add-ReportEvent "fetch" $name "CACHED" "Verified immutable cache entry" @{ path = $cachePath }
            continue
        }
        $missing.Add($name)
    }
    if ($missing.Count -eq 0) { return }
    Assert-NetworkAllowed @($missing)

    New-Item -ItemType Directory -Path (Assert-PathUnder $CacheDir $script:RepoRoot "cache directory") -Force | Out-Null
    foreach ($name in $missing) {
        $component = $Lock.components.$name
        $cachePath = Get-CachePath $name $component
        $partialPath = "$cachePath.partial-$script:RunId"
        Write-Host "Downloading $name from $($component.archive.url)..."
        $ProgressPreference = "SilentlyContinue"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $downloadUrls = @([string]$component.archive.url)
        if (([Uri]$component.archive.url).Host -eq 'downloads.sourceforge.net') {
            $downloadUrls = @(
                ([string]$component.archive.url).Replace('downloads.sourceforge.net', 'netix.dl.sourceforge.net'),
                ([string]$component.archive.url).Replace('downloads.sourceforge.net', 'pilotfiber.dl.sourceforge.net')
            ) + $downloadUrls
        }
        foreach ($downloadUrl in $downloadUrls) {
            try {
                Invoke-WebRequest -Uri $downloadUrl -UserAgent 'FalcoEngine-Build/1.0' -OutFile $partialPath -UseBasicParsing
                if (Test-FileIdentity $partialPath $component.archive.bytes $component.archive.sha256) { break }
                Write-Host "Rejected unexpected download content from $downloadUrl"
            } catch {
                Write-Host "Download failed from ${downloadUrl}: $($_.Exception.Message)"
            }
        }
        if (!(Test-Path -LiteralPath $partialPath -PathType Leaf)) {
            Stop-BuildDeps "all download locations failed for components.$name"
        }
        if (-not (Test-FileIdentity $partialPath $component.archive.bytes $component.archive.sha256)) {
            $identity = Get-FileIdentity $partialPath
            $quarantinePath = "$partialPath.quarantine"
            Move-Item -LiteralPath $partialPath -Destination $quarantinePath
            Stop-BuildDeps "components.$name download identity mismatch; preserved as $quarantinePath ($($identity.bytes) bytes, $($identity.sha256))"
        }
        Move-Item -LiteralPath $partialPath -Destination $cachePath
        Add-ReportEvent "fetch" $name "DOWNLOADED" "Downloaded and verified archive" @{
            expectedUrl = $component.archive.url
            path = $cachePath
            bytes = $component.archive.bytes
            sha256 = $component.archive.sha256
        }
    }
}

function Assert-NetworkAllowed([string[]]$MissingComponents) {
    if (-not $AllowNetwork) {
        Stop-BuildDeps "network is disabled; missing cache entries: $($MissingComponents -join ', ')"
    }
}

function Switch-WavePrefix([string]$StagingPath, [string]$ActivePath) {
    $staging = Assert-PathUnder $StagingPath $script:OutRoot "wave staging path"
    $active = Assert-PathUnder $ActivePath $script:OutRoot "wave active path"
    if (-not (Test-Path -LiteralPath $staging -PathType Container)) {
        Stop-BuildDeps "wave staging directory not found: $staging"
    }
    $previous = "$active.prev-$script:RunId"
    if (Test-Path -LiteralPath $previous) { Stop-BuildDeps "rollback prefix already exists: $previous" }
    $movedPrevious = $false
    if (Test-Path -LiteralPath $active -PathType Container) {
        Move-Item -LiteralPath $active -Destination $previous
        $movedPrevious = $true
    }
    try {
        Move-Item -LiteralPath $staging -Destination $active
    }
    catch {
        if ($movedPrevious -and -not (Test-Path -LiteralPath $active)) {
            Move-Item -LiteralPath $previous -Destination $active
        }
        Stop-BuildDeps "atomic prefix switch failed and rollback was attempted: $($_.Exception.Message)"
    }
    return $(if ($movedPrevious) { $previous } else { $null })
}

function Invoke-Check($Lock, [string[]]$Names) {
    $verified = 0
    $pending = 0
    foreach ($name in $Names) {
        foreach ($output in @($Lock.components.$name.outputs)) {
            if ($output.sha256 -notmatch "^[A-Fa-f0-9]{64}$" -or [long]$output.bytes -le 0) {
                $pending++
                continue
            }
            $path = Join-Path $script:RepoRoot $output.path
            if (-not (Test-FileIdentity $path $output.bytes $output.sha256)) {
                Stop-BuildDeps "components.$name output missing or mismatched: $($output.path)"
            }
            $verified++
        }
    }
    Add-ReportEvent "check" "" "PASS" "Output verification complete" @{ verified = $verified; pending = $pending }
    Write-Output "PASS: Check verified $verified pinned outputs; $pending preparation outputs remain pending."
}

function Get-MsvcToolPaths([string]$InstallationPath, [string]$VcToolsVersion) {
    if (-not $InstallationPath) { Stop-BuildDeps "MSVC installation path must not be empty" }
    if (-not $VcToolsVersion) { Stop-BuildDeps "MSVC tools version must not be empty" }
    $toolsBin = Join-Path $InstallationPath "VC\Tools\MSVC\$VcToolsVersion\bin\Hostx64\x64"
    return [pscustomobject]@{
        toolsBin = $toolsBin
        cl = Join-Path $toolsBin "cl.exe"
        link = Join-Path $toolsBin "link.exe"
        lib = Join-Path $toolsBin "lib.exe"
        dumpbin = Join-Path $toolsBin "dumpbin.exe"
    }
}

function Get-Vs2022BuildTools([string]$VsVersionRange) {
    if (-not $VsVersionRange) {
        Stop-BuildDeps "Visual Studio version range must be provided from deps.lock toolchain.vs"
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        Stop-BuildDeps "vswhere.exe was not found; install Visual Studio 2022 with Desktop development with C++"
    }
    $installationPath = (& $vswhere -latest -products * -version $VsVersionRange `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Out-String).Trim()
    if (-not $installationPath) {
        Stop-BuildDeps "Visual Studio 2022 C++ x64 tools were not found for version range '$VsVersionRange'"
    }
    $msbuild = Join-Path $installationPath "MSBuild\Current\Bin\MSBuild.exe"
    if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
        Stop-BuildDeps "MSBuild.exe was not found at '$msbuild'"
    }
    $vcToolsVersionFile = Join-Path $installationPath "VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
    if (-not (Test-Path -LiteralPath $vcToolsVersionFile -PathType Leaf)) {
        Stop-BuildDeps "default MSVC tools version file was not found at '$vcToolsVersionFile'"
    }
    $vcToolsVersion = (Get-Content -LiteralPath $vcToolsVersionFile -Raw).Trim()
    $toolPaths = Get-MsvcToolPaths $installationPath $vcToolsVersion
    foreach ($toolPath in @($toolPaths.cl, $toolPaths.link, $toolPaths.lib, $toolPaths.dumpbin)) {
        if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
            Stop-BuildDeps "x64 MSVC tool was not found at '$toolPath'"
        }
    }
    return [pscustomobject]@{
        installationPath = $installationPath
        vcToolsVersion = $vcToolsVersion
        cl = $toolPaths.cl
        link = $toolPaths.link
        lib = $toolPaths.lib
        msbuild = $msbuild
        dumpbin = $toolPaths.dumpbin
    }
}

# F-W2-43: the SDK probe below reads <WindowsTargetPlatformVersion> out of the repository's project
# files and insists on exactly one value. It used to walk the whole tree. CMake writes a fully
# resolved version ("10.0.26100.0") into the projects it generates under out\deps\_build\, while the
# engine's own projects carry the short form ("10.0"), so the first successful dependency build made
# every later -Mode Check fail: the probe saw two values and - correctly - refused to choose.
# The generated trees are exactly the roots .gitignore already keeps out of the tree, so the probe
# skips the same set. .git is added because an object store is not project source.
# Measured on 2026-07-28: 46 projects carry "10.0" (25 outside out\, 21 GENie-generated inside it)
# and 16 carry "10.0.26100.0", all sixteen under out\deps\_build\w2\.
$script:ProjectScanExcludedRoots = @(
    "out",
    "deps-cache",
    "deps-inbox",
    ".vs",
    ".git",
    "x64",
    "Bgfx\bimg",
    "Bgfx\bx"
)

function Get-ProjectPlatformVersions([string]$Root) {
    $rootFull = ([IO.Path]::GetFullPath($Root)).TrimEnd('\')
    $excludedPrefixes = @(
        $script:ProjectScanExcludedRoots | ForEach-Object {
            ([IO.Path]::GetFullPath((Join-Path $rootFull $_))).TrimEnd('\') + '\'
        }
    )
    $versions = New-Object System.Collections.Generic.List[string]
    $scannedCount = 0
    $skippedCount = 0
    $pattern = "<WindowsTargetPlatformVersion>([^<]+)</WindowsTargetPlatformVersion>"
    foreach ($file in @(Get-ChildItem -LiteralPath $rootFull -Recurse -File -Filter "*.vcxproj")) {
        $isExcluded = $false
        foreach ($prefix in $excludedPrefixes) {
            if ($file.FullName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                $isExcluded = $true
                break
            }
        }
        if ($isExcluded) {
            $skippedCount++
            continue
        }
        $scannedCount++
        foreach ($match in [regex]::Matches([IO.File]::ReadAllText($file.FullName), $pattern)) {
            $versions.Add($match.Groups[1].Value.Trim())
        }
    }
    return [pscustomobject]@{
        versions = @(@($versions) | Sort-Object -Unique)
        scannedCount = $scannedCount
        skippedCount = $skippedCount
        excludedRoots = @($script:ProjectScanExcludedRoots)
    }
}

function Get-WindowsSdkVersion {
    $installedRootsPath = "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots"
    $installedRoots = Get-Item -LiteralPath $installedRootsPath -ErrorAction Stop
    $kitsRoot10 = [string](Get-ItemProperty -LiteralPath $installedRootsPath -ErrorAction Stop).KitsRoot10
    if ([string]::IsNullOrWhiteSpace($kitsRoot10)) {
        Stop-BuildDeps "Windows 10 SDK root was not found in Installed Roots"
    }

    $candidates = @(
        $installedRoots.GetSubKeyNames() |
            Where-Object { $_ -match "^\d+\.\d+\.\d+\.\d+$" } |
            Sort-Object { [version]$_ } -Unique
    )
    if ($candidates.Count -eq 0) {
        Stop-BuildDeps "Windows SDK version candidates were not found in Installed Roots"
    }

    $projectScan = Get-ProjectPlatformVersions $script:RepoRoot
    $projectVersions = @($projectScan.versions)
    if ($projectVersions.Count -ne 1) {
        Stop-BuildDeps ("expected one WindowsTargetPlatformVersion in projects, found: " +
            "$($projectVersions -join ', '); scanned $($projectScan.scannedCount) projects, " +
            "skipped $($projectScan.skippedCount) under excluded roots " +
            "$($projectScan.excludedRoots -join ', ')")
    }

    $projectVersion = $projectVersions[0]
    $matches = @(
        $candidates | Where-Object {
            $_ -eq $projectVersion -or
            ($projectVersion -match "^\d+\.\d+$" -and $_.StartsWith($projectVersion + ".", [StringComparison]::Ordinal))
        }
    )
    if ($matches.Count -ne 1) {
        Stop-BuildDeps "Windows SDK '$projectVersion' is ambiguous against installed candidates: $($candidates -join ', ')"
    }

    $script:WindowsSdkDiscovery = [pscustomobject]@{
        kitsRoot10 = $kitsRoot10
        projectVersion = $projectVersion
        candidates = $candidates
        selectedVersion = $matches[0]
        scannedProjects = $projectScan.scannedCount
        skippedProjects = $projectScan.skippedCount
        excludedRoots = $projectScan.excludedRoots
    }
    return $matches[0]
}

function Get-ToolchainIdentity([hashtable]$Override) {
    if ($PSBoundParameters.ContainsKey("Override")) {
        if ($Mode -ne "SelfTest") {
            Stop-BuildDeps "toolchain identity override is restricted to SelfTest"
        }
        $selfTestRoot = Join-Path $script:OutRoot "_selftest"
        $null = Assert-PathUnder $script:RunRoot $selfTestRoot "toolchain identity override root"
        return [pscustomobject]$Override
    }

    function Get-ToolFileIdentity([string]$Path) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            Stop-BuildDeps "tool was not found at '$Path'"
        }
        $item = Get-Item -LiteralPath $Path
        return [ordered]@{
            version = [string]$item.VersionInfo.ProductVersion
            bytes = [long]$item.Length
            sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
        }
    }

    $lock = Read-DependencyLock $LockPath
    $vsVersionRange = [string]$lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $vsTools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $instances = @(
        & $vswhere -latest -products * -version $vsVersionRange `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8 |
            ConvertFrom-Json
    )
    if ($instances.Count -ne 1) {
        Stop-BuildDeps "expected one selected Visual Studio 2022 C++ instance, found $($instances.Count)"
    }
    $instance = $instances[0]
    if ([string]$instance.installationPath -ne [string]$vsTools.installationPath) {
        Stop-BuildDeps "Visual Studio discovery returned inconsistent installation paths"
    }

    $identity = [ordered]@{
        schemaVersion = 1
        toolsetFamily = [string]$lock.toolchain.toolset
        arch = [string]$lock.toolchain.arch
        crt = [string]$lock.toolchain.crt
        config = [string]$lock.toolchain.config
        vs = [ordered]@{
            productDisplayVersion = [string]$instance.catalog.productDisplayVersion
            installationVersion = [string]$instance.installationVersion
            instanceId = [string]$instance.instanceId
            installationPath = [string]$instance.installationPath
        }
        vcTools = [ordered]@{ version = [string]$vsTools.vcToolsVersion }
        cl = Get-ToolFileIdentity $vsTools.cl
        link = Get-ToolFileIdentity $vsTools.link
        lib = Get-ToolFileIdentity $vsTools.lib
        dumpbin = Get-ToolFileIdentity $vsTools.dumpbin
        msbuild = Get-ToolFileIdentity $vsTools.msbuild
        windowsSdk = [ordered]@{ version = [string](Get-WindowsSdkVersion) }
    }

    $cmakeCommand = Get-Command "cmake.exe" -CommandType Application -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $cmakeCommand) {
        $cmakeVersionOutput = @(& $cmakeCommand.Source --version 2>&1)
        if ($LASTEXITCODE -ne 0 -or $cmakeVersionOutput.Count -eq 0 -or
            [string]$cmakeVersionOutput[0] -notmatch "^cmake version (.+)$") {
            Stop-BuildDeps "cmake version could not be determined"
        }
        $identity["cmake"] = [ordered]@{
            version = [string]$Matches[1]
            path = [string]$cmakeCommand.Source
        }
    }
    return [pscustomobject]$identity
}

function Normalize-ProcessPathVariable {
    $pathVariables = @(
        [Environment]::GetEnvironmentVariables("Process").GetEnumerator() |
            Where-Object { $_.Key -ieq "PATH" }
    )
    if ($pathVariables.Count -le 1) { return }
    $pathValue = [string]$pathVariables[0].Value
    foreach ($pathVariable in $pathVariables) {
        [Environment]::SetEnvironmentVariable([string]$pathVariable.Key, $null, "Process")
    }
    [Environment]::SetEnvironmentVariable("Path", $pathValue, "Process")
    Add-ReportEvent "environment" "" "NORMALIZED" "Removed duplicate case-insensitive PATH variables for child build tools"
}

function Get-StringSha256([string]$Value) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace("-", "")
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-ToolchainFingerprint($Identity) {
    $canonical = [ordered]@{
        schemaVersion = [int]$Identity.schemaVersion
        toolsetFamily = [string]$Identity.toolsetFamily
        arch = [string]$Identity.arch
        crt = [string]$Identity.crt
        config = [string]$Identity.config
        vs = [ordered]@{
            productDisplayVersion = [string]$Identity.vs.productDisplayVersion
            installationVersion = [string]$Identity.vs.installationVersion
        }
        vcTools = [ordered]@{ version = [string]$Identity.vcTools.version }
        cl = [ordered]@{
            version = [string]$Identity.cl.version
            bytes = [long]$Identity.cl.bytes
            sha256 = ([string]$Identity.cl.sha256).ToUpperInvariant()
        }
        link = [ordered]@{
            version = [string]$Identity.link.version
            bytes = [long]$Identity.link.bytes
            sha256 = ([string]$Identity.link.sha256).ToUpperInvariant()
        }
        lib = [ordered]@{
            version = [string]$Identity.lib.version
            bytes = [long]$Identity.lib.bytes
            sha256 = ([string]$Identity.lib.sha256).ToUpperInvariant()
        }
        dumpbin = [ordered]@{
            version = [string]$Identity.dumpbin.version
            bytes = [long]$Identity.dumpbin.bytes
            sha256 = ([string]$Identity.dumpbin.sha256).ToUpperInvariant()
        }
        msbuild = [ordered]@{
            version = [string]$Identity.msbuild.version
            bytes = [long]$Identity.msbuild.bytes
            sha256 = ([string]$Identity.msbuild.sha256).ToUpperInvariant()
        }
        windowsSdk = [ordered]@{ version = [string]$Identity.windowsSdk.version }
    }
    if (@($Identity.PSObject.Properties.Name) -contains "cmake") {
        if ($null -eq $Identity.cmake) {
            Stop-BuildDeps "cmake identity must be absent rather than null"
        }
        $canonical["cmake"] = [ordered]@{ version = [string]$Identity.cmake.version }
    }
    return Get-StringSha256 ($canonical | ConvertTo-Json -Compress -Depth 10)
}

function Compare-ToolchainIdentity($OldIdentity, $NewIdentity) {
    function Get-ComparableToolchainFields($Identity) {
        $fields = [ordered]@{
            "schemaVersion" = $Identity.schemaVersion
            "toolsetFamily" = $Identity.toolsetFamily
            "arch" = $Identity.arch
            "crt" = $Identity.crt
            "config" = $Identity.config
            "vs.productDisplayVersion" = $Identity.vs.productDisplayVersion
            "vs.installationVersion" = $Identity.vs.installationVersion
            "vcTools.version" = $Identity.vcTools.version
            "cl.version" = $Identity.cl.version
            "cl.bytes" = $Identity.cl.bytes
            "cl.sha256" = $Identity.cl.sha256
            "link.version" = $Identity.link.version
            "link.bytes" = $Identity.link.bytes
            "link.sha256" = $Identity.link.sha256
            "lib.version" = $Identity.lib.version
            "lib.bytes" = $Identity.lib.bytes
            "lib.sha256" = $Identity.lib.sha256
            "dumpbin.version" = $Identity.dumpbin.version
            "dumpbin.bytes" = $Identity.dumpbin.bytes
            "dumpbin.sha256" = $Identity.dumpbin.sha256
            "msbuild.version" = $Identity.msbuild.version
            "msbuild.bytes" = $Identity.msbuild.bytes
            "msbuild.sha256" = $Identity.msbuild.sha256
            "windowsSdk.version" = $Identity.windowsSdk.version
        }
        if (@($Identity.PSObject.Properties.Name) -contains "cmake" -and $null -ne $Identity.cmake) {
            $fields["cmake.version"] = $Identity.cmake.version
        }
        return $fields
    }

    $oldFields = Get-ComparableToolchainFields $OldIdentity
    $newFields = Get-ComparableToolchainFields $NewIdentity
    $fieldNames = @($oldFields.Keys) + @($newFields.Keys | Where-Object { -not $oldFields.Contains($_) })
    $differences = foreach ($field in $fieldNames) {
        $oldValue = if ($oldFields.Contains($field)) { $oldFields[$field] } else { "<absent>" }
        $newValue = if ($newFields.Contains($field)) { $newFields[$field] } else { "<absent>" }
        if ([string]$oldValue -cne [string]$newValue) {
            [pscustomobject]@{ field = $field; old = $oldValue; new = $newValue }
        }
    }
    return @($differences)
}

function Get-W1RecipeFingerprint($Lock) {
    $projectRoot = Join-Path $script:RepoRoot "deps\projects\W1"
    $projectIdentities = @(
        Get-ChildItem -LiteralPath $projectRoot -File |
            Sort-Object Name |
            ForEach-Object {
                $identity = Get-FileIdentity $_.FullName
                [ordered]@{ name = $_.Name; bytes = $identity.bytes; sha256 = $identity.sha256 }
            }
    )
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $value = [ordered]@{
        toolchain = $Lock.toolchain
        libogg = $Lock.components.libogg
        libvorbis = $Lock.components.libvorbis
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        projects = $projectIdentities
    } | ConvertTo-Json -Depth 20 -Compress
    return Get-StringSha256 $value
}

function Get-WaveFingerprint([string]$RecipeFingerprint, [string]$ToolchainFingerprint) {
    Assert-PinnedSha256 $RecipeFingerprint "recipe fingerprint"
    Assert-PinnedSha256 $ToolchainFingerprint "toolchain fingerprint"
    return Get-StringSha256 ("$RecipeFingerprint|$ToolchainFingerprint")
}

function Get-StatePath {
    return Assert-PathUnder $script:StatePath $script:OutRoot "state file path"
}

function Get-WaveState([string]$WaveName) {
    $statePath = Get-StatePath
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) { return $null }
    try {
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        if (@($state.PSObject.Properties.Name) -notcontains "waves" -or $null -eq $state.waves) { return $null }
        $schemaVersion = if (@($state.PSObject.Properties.Name) -contains "schemaVersion") {
            [string]$state.schemaVersion
        } else { "<absent>" }
        if ($schemaVersion -ne [string]$script:StateSchemaVersion) {
            Add-ReportEvent "state" $WaveName "STALE" ("State schema version '$schemaVersion' is not the supported " +
                "version $script:StateSchemaVersion; migration requires a rebuild rather than an assumption")
            return $null
        }
        $property = $state.waves.PSObject.Properties[$WaveName]
        if ($null -eq $property) { return $null }
        return $property.Value
    }
    catch {
        Add-ReportEvent "state" $WaveName "STALE" "State file is unreadable and will not be trusted"
        return $null
    }
}

function Get-WaveStateDisposition([string]$WaveName, [string]$RecipeFingerprint,
    [string]$ToolchainFingerprint, $ToolchainIdentity) {
    Assert-PinnedSha256 $RecipeFingerprint "recipe fingerprint"
    Assert-PinnedSha256 $ToolchainFingerprint "toolchain fingerprint"
    if ($null -eq $ToolchainIdentity) {
        Stop-BuildDeps "wave state disposition requires the current toolchain identity"
    }

    function New-Disposition([string]$Status, [string]$Reason, $State, $Differences) {
        return [pscustomobject]@{
            status = $Status
            reason = $Reason
            state = $State
            differences = @($Differences)
        }
    }

    $waveState = Get-WaveState $WaveName
    if ($null -eq $waveState) {
        return New-Disposition "S3" "no usable state record for the wave" $null @()
    }
    $recordNames = @($waveState.PSObject.Properties.Name)
    foreach ($required in @("recipeFingerprint", "toolchainFingerprint", "waveFingerprint",
        "toolchainIdentity", "outputs")) {
        if ($recordNames -notcontains $required) {
            Add-ReportEvent "state" $WaveName "STALE" "State record is missing the required field '$required'"
            return New-Disposition "S3" "state record is missing '$required'" $null @()
        }
    }
    $recordedRecipe = [string]$waveState.recipeFingerprint
    $recordedToolchain = [string]$waveState.toolchainFingerprint
    if ($recordedRecipe -notmatch "^[A-Fa-f0-9]{64}$" -or $recordedToolchain -notmatch "^[A-Fa-f0-9]{64}$") {
        Add-ReportEvent "state" $WaveName "STALE" "State record contains a malformed fingerprint"
        return New-Disposition "S3" "state record contains a malformed fingerprint" $null @()
    }
    if ([string]$waveState.waveFingerprint -ne (Get-WaveFingerprint $recordedRecipe $recordedToolchain)) {
        Add-ReportEvent "state" $WaveName "STALE" "Composite wave fingerprint does not match its recorded parts"
        return New-Disposition "S3" "composite wave fingerprint is inconsistent" $null @()
    }
    if ($recordedRecipe -ne $RecipeFingerprint) {
        return New-Disposition "S3" "recipe fingerprint changed" $waveState @()
    }
    if ($recordedToolchain -ne $ToolchainFingerprint) {
        $differences = @()
        try {
            $differences = @(Compare-ToolchainIdentity $waveState.toolchainIdentity $ToolchainIdentity)
        }
        catch {
            $differences = @([pscustomobject]@{
                field = "toolchainIdentity"
                old = "<unreadable in state>"
                new = $ToolchainFingerprint
            })
        }
        if ($AllowToolchainRebuild) { return New-Disposition "S3" "toolchain changed; rebuilding from pinned sources" $waveState @() }
        return New-Disposition "S2" "recipe unchanged but toolchain identity changed" $waveState $differences
    }
    foreach ($output in @($waveState.outputs)) {
        $path = Join-Path $script:RepoRoot $output.path
        if (-not (Test-FileIdentity $path ([long]$output.bytes) ([string]$output.sha256))) {
            Add-ReportEvent "state" $WaveName "STALE" "Recorded output no longer matches its identity: $($output.path)"
            return New-Disposition "S3" "recorded outputs no longer match their identities" $waveState @()
        }
    }
    if (@($waveState.outputs).Count -le 0) {
        return New-Disposition "S3" "state record carries no outputs" $waveState @()
    }
    return New-Disposition "S1" "recipe and toolchain match and every recorded output verified" $waveState @()
}

function Write-WaveState([string]$WaveName, [string]$RecipeFingerprint, [string]$ToolchainFingerprint,
    $ToolchainIdentity, [object[]]$Outputs) {
    if ($null -eq $ToolchainIdentity) {
        Stop-BuildDeps "wave state requires the toolchain identity that produced the outputs"
    }
    $waveFingerprint = Get-WaveFingerprint $RecipeFingerprint $ToolchainFingerprint
    $statePath = Get-StatePath
    $waves = [ordered]@{}
    if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        try {
            $existing = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
            $existingSchema = if (@($existing.PSObject.Properties.Name) -contains "schemaVersion") {
                [string]$existing.schemaVersion
            } else { "<absent>" }
            if ($existingSchema -eq [string]$script:StateSchemaVersion -and
                @($existing.PSObject.Properties.Name) -contains "waves" -and $null -ne $existing.waves) {
                foreach ($property in $existing.waves.PSObject.Properties) {
                    $waves[$property.Name] = $property.Value
                }
            }
            else {
                Add-ReportEvent "state" $WaveName "STALE" ("Replacing a state file with schema version " +
                    "'$existingSchema'; records of other waves are not carried over")
            }
        }
        catch {
            Add-ReportEvent "state" $WaveName "STALE" "Replacing unreadable state file after successful build"
        }
    }
    $waves[$WaveName] = [ordered]@{
        recipeFingerprint = $RecipeFingerprint
        toolchainFingerprint = $ToolchainFingerprint
        waveFingerprint = $waveFingerprint
        toolchainIdentity = $ToolchainIdentity
        completedAt = (Get-Date).ToUniversalTime().ToString("o")
        outputs = $Outputs
    }
    $state = [ordered]@{ schemaVersion = $script:StateSchemaVersion; waves = $waves }
    New-Item -ItemType Directory -Path (Split-Path -Parent $statePath) -Force | Out-Null
    $temporary = "$statePath.tmp-$script:RunId"
    [IO.File]::WriteAllText($temporary, ($state | ConvertTo-Json -Depth 20), (New-Object Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temporary -Destination $statePath -Force
}

function Invoke-W1MsBuild([string]$MsBuild, [string]$ProjectName, [string]$SourceRoot,
    [string]$OggIncludeRoot, [string]$StagingRoot, [string]$IntermediateRoot, [string]$VcToolsVersion) {
    if (-not $VcToolsVersion) {
        Stop-BuildDeps "MSBuild invocation requires the pinned MSVC tools version"
    }
    $project = Join-Path $script:RepoRoot "deps\projects\W1\$ProjectName.vcxproj"
    $logPath = Join-Path $script:RunRoot "$ProjectName.log"
    # MSBuild runs these projects from deps/projects/W1. Relative object paths
    # keep both COFF object metadata and lib.exe member names independent of
    # the absolute checkout directory (compiler /pathmap alone is insufficient).
    $relativeIntermediateRoot = "..\..\..\" + $IntermediateRoot.Substring($script:RepoRoot.Length).TrimStart('\','/')
    $arguments = @(
        $project,
        "/m",
        "/t:Rebuild",
        "/p:Configuration=Release",
        "/p:Platform=x64",
        "/p:VCToolsVersion=$VcToolsVersion",
        "/p:W1SourceRoot=$SourceRoot",
        "/p:W1OggIncludeRoot=$OggIncludeRoot",
        "/p:W1OutputRoot=$StagingRoot",
        "/p:W1IntermediateRoot=$relativeIntermediateRoot",
        "/p:W1ObjectMapRoot=$IntermediateRoot",
        "/p:W1WorkMapRoot=$script:WorkRoot",
        "/fl",
        "/flp:logfile=$logPath;verbosity=diagnostic",
        "/nologo"
    )
    $output = & $MsBuild @arguments 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $output | Select-Object -Last 80 | ForEach-Object { Write-Output $_ }
        Stop-BuildDeps "MSBuild failed for $ProjectName with exit code $exitCode; log: $logPath"
    }
    if (@($output | Where-Object { [string]$_ -match 'D9007|D9002' }).Count -gt 0) {
        Stop-BuildDeps "W1 compiler rejected a required option; log: $logPath"
    }
    $warningKeys = foreach ($line in $output) {
        $match = [regex]::Match([string]$line, '(?<file>[^\\/:]+\.c)\((?<line>\d+),\d+\):\s+warning\s+(?<code>C\d+):')
        if ($match.Success) {
            "$($match.Groups['file'].Value):$($match.Groups['line'].Value):$($match.Groups['code'].Value)"
        }
    }
    $warningCount = @($warningKeys | Sort-Object -Unique).Count
    Add-ReportEvent "build" $ProjectName "PASS" "MSBuild completed" @{ log = $logPath; uniqueWarnings = $warningCount }
    Write-Output "PASS: $ProjectName compiled (0 errors; $warningCount unique warnings; full log: $logPath)"
}

function Assert-StaticCrt([string]$Dumpbin, [string]$LibraryPath, [string]$ComponentId) {
    $directives = (& $Dumpbin /NOLOGO /DIRECTIVES $LibraryPath 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { Stop-BuildDeps "dumpbin /DIRECTIVES failed for '$LibraryPath'" }
    if ($directives -match '(?i)DEFAULTLIB:"?(?:MSVCRT|MSVCRTD|LIBCMTD)"?(?:\s|$)') {
        Stop-BuildDeps "components.$ComponentId output contains a forbidden dynamic or debug CRT directive"
    }
    if ($directives -notmatch '(?i)DEFAULTLIB:"?LIBCMT"?(?:\s|$)') {
        Stop-BuildDeps "components.$ComponentId output does not prove /MT through DEFAULTLIB:LIBCMT"
    }
    Add-ReportEvent "crt" $ComponentId "PASS" "dumpbin proves static Release CRT /MT" @{
        path = $LibraryPath
        required = "DEFAULTLIB:LIBCMT"
    }
}

function Invoke-W1Build($Lock, [string[]]$Names) {
    if (($Names -join ",") -ne "libogg,libvorbis") {
        Stop-BuildDeps "W1 recipe requires the topological component order libogg, libvorbis"
    }
    foreach ($name in $Names) {
        $component = $Lock.components.$name
        if ($component.wave -ne "W1" -or $component.buildProfile.system -ne "msbuild" -or
            "generated-v143-project" -notin @($component.buildProfile.flags) -or
            "RuntimeLibrary=MultiThreaded" -notin @($component.buildProfile.flags)) {
            Stop-BuildDeps "components.$name is not the approved W1 MSBuild /MT recipe"
        }
    }

    $vsVersionRange = [string]$Lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $tools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    Normalize-ProcessPathVariable
    $recipeFingerprint = Get-W1RecipeFingerprint $Lock
    if ($null -eq $script:ToolchainIdentity) {
        Stop-BuildDeps "toolchain identity must be resolved before a wave build"
    }
    $toolchainIdentity = $script:ToolchainIdentity
    $toolchainFingerprint = Get-ToolchainFingerprint $toolchainIdentity
    $activeRoot = Join-Path $script:OutRoot "w1"
    $disposition = Get-WaveStateDisposition "W1" $recipeFingerprint $toolchainFingerprint $toolchainIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Add-ReportEvent "toolchain" "W1" "DRIFT" "Recipe unchanged but toolchain identity changed" @{
            recipeFingerprint = $recipeFingerprint
            recordedToolchainFingerprint = [string]$disposition.state.toolchainFingerprint
            currentToolchainFingerprint = $toolchainFingerprint
            differences = @($disposition.differences)
        }
        Stop-BuildDeps ("TOOLCHAIN DRIFT: recipe unchanged but toolchain identity changed ($differenceTable). " +
            "Pinned outputs cannot be reproduced. Run the controlled re-pin procedure " +
            "(see _ai/MSVC_OUTPUT_PIN_AND_REPIN_POLICY.md section 5) or restore the pinned toolchain.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        foreach ($output in @($Lock.components.libogg.outputs) + @($Lock.components.libvorbis.outputs)) {
            Assert-StaticCrt $tools.dumpbin (Join-Path $script:RepoRoot $output.path) "W1"
        }
        Add-ReportEvent "toolchain" "W1" "MATCH" "Toolchain fingerprint matches the recorded wave state" @{
            toolchainFingerprint = $toolchainFingerprint
        }
        Add-ReportEvent "state" "W1" "CACHED" "Recipe fingerprint and output identities match; build skipped"
        Write-Output "PASS: W1 state and outputs verified; build skipped. Use -Rebuild to force compilation."
        return
    }
    Add-ReportEvent "state" "W1" "STALE" "W1 will be rebuilt: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $oggSource = Expand-VerifiedArchive (Get-CachePath "libogg" $Lock.components.libogg) `
        (Join-Path $script:WorkRoot "libogg") $Lock.components.libogg "libogg"
    $vorbisSource = Expand-VerifiedArchive (Get-CachePath "libvorbis" $Lock.components.libvorbis) `
        (Join-Path $script:WorkRoot "libvorbis") $Lock.components.libvorbis "libvorbis"

    $stagingRoot = Join-Path $script:OutRoot "_staging\w1-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W1 staging root"
    New-Item -ItemType Directory -Path (Join-Path $stagingRoot "lib"), (Join-Path $stagingRoot "include\ogg"),
        (Join-Path $stagingRoot "include\vorbis"), (Join-Path $stagingRoot "licenses") -Force | Out-Null
    # MSVC embeds the object output path even with /Brepro. Keep the intermediate
    # root stable while /pathmap normalizes the run-specific extracted sources.
    $intermediateRoot = Join-Path $script:OutRoot "_build\w1\obj"

    Invoke-W1MsBuild $tools.msbuild "libogg_static" $oggSource (Join-Path $oggSource "include") $stagingRoot $intermediateRoot $tools.vcToolsVersion
    Copy-Item -LiteralPath (Join-Path $oggSource "include\ogg\ogg.h"), (Join-Path $oggSource "include\ogg\os_types.h") `
        -Destination (Join-Path $stagingRoot "include\ogg")
    Copy-Item -LiteralPath (Join-Path $oggSource "COPYING") -Destination (Join-Path $stagingRoot "licenses\libogg-COPYING")

    $oggIncludeRoot = Join-Path $stagingRoot "include"
    Invoke-W1MsBuild $tools.msbuild "libvorbis_static" $vorbisSource $oggIncludeRoot $stagingRoot $intermediateRoot $tools.vcToolsVersion
    Invoke-W1MsBuild $tools.msbuild "libvorbisfile_static" $vorbisSource $oggIncludeRoot $stagingRoot $intermediateRoot $tools.vcToolsVersion
    Copy-Item -LiteralPath (Join-Path $vorbisSource "include\vorbis\codec.h"),
        (Join-Path $vorbisSource "include\vorbis\vorbisenc.h"), (Join-Path $vorbisSource "include\vorbis\vorbisfile.h") `
        -Destination (Join-Path $stagingRoot "include\vorbis")
    Copy-Item -LiteralPath (Join-Path $vorbisSource "COPYING") -Destination (Join-Path $stagingRoot "licenses\libvorbis-COPYING")

    $libraries = @(
        [pscustomobject]@{ component = "libogg"; path = Join-Path $stagingRoot "lib\libogg_static.lib" },
        [pscustomobject]@{ component = "libvorbis"; path = Join-Path $stagingRoot "lib\libvorbis_static.lib" },
        [pscustomobject]@{ component = "libvorbis"; path = Join-Path $stagingRoot "lib\libvorbisfile_static.lib" }
    )
    foreach ($library in $libraries) {
        if (-not (Test-Path -LiteralPath $library.path -PathType Leaf)) {
            Stop-BuildDeps "W1 expected output is missing: $($library.path)"
        }
        Assert-StaticCrt $tools.dumpbin $library.path $library.component
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($output in @($Lock.components.libogg.outputs) + @($Lock.components.libvorbis.outputs)) {
        $path = Join-Path $script:RepoRoot $output.path
        $identity = Get-FileIdentity $path
        $stateOutputs += [ordered]@{ path = $output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    Write-WaveState "W1" $recipeFingerprint $toolchainFingerprint $toolchainIdentity $stateOutputs
    Add-ReportEvent "prefix" "W1" "PASS" "W1 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output "PASS: W1 built with MSVC v143 x64 Release /MT and activated at '$activeRoot'."
    $pinContext = "PINCTX: cl=$($toolchainIdentity.cl.version) link=$($toolchainIdentity.link.version)" +
        " lib=$($toolchainIdentity.lib.version) sdk=$($toolchainIdentity.windowsSdk.version)"
    if (@($toolchainIdentity.PSObject.Properties.Name) -contains "cmake") {
        $pinContext += " cmake=$($toolchainIdentity.cmake.version)"
    }
    $pinContext += " fp=$toolchainFingerprint"
    Write-Output $pinContext
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Expand-RecipeToken([string]$Value, [string]$Label, [string]$W2Prefix = "") {
    # DEC-024 / F-W2-24. CMake resolves a relative path in a -D argument against its own
    # working directory, not against the repository root, so the recipe carries the token
    # ${REPO_ROOT} and the driver is obliged to expand it. Forward slashes are used because
    # CMake treats a backslash in a cache value as an escape character.
    $rootForCMake = $script:RepoRoot.Replace('\', '/').TrimEnd('/')
    $expanded = $Value.Replace('${REPO_ROOT}', $rootForCMake)
    if ($expanded -match '\$\{') {
        Stop-BuildDeps ("$Label contains an unexpanded token after substitution and must not be " +
            "passed to cmake verbatim: '$expanded'")
    }
    if (-not $W2Prefix) { return $expanded }

    # F-W2-30. libzip is configured against ${REPO_ROOT}/out/deps/w2/include and
    # ${REPO_ROOT}/out/deps/w2/lib/zs.lib - the FINAL location of the wave prefix. During the
    # build that directory does not hold this run's zlib yet: Switch-WavePrefix activates the
    # staging tree atomically only after the whole wave succeeds, so out/deps/w2 is either
    # absent or still holds the previous run. The recipe means "the W2 prefix", and during the
    # build the W2 prefix is the staging tree, so the expansion is redirected there. This is
    # the same rule the W1 driver already follows: it configures libvorbis against
    # (Join-Path $stagingRoot "include"), not against the active out/deps/w1.
    $activePrefix = "$rootForCMake/out/deps/w2"
    $stagingPrefix = $W2Prefix.Replace('\', '/').TrimEnd('/')
    $index = $expanded.IndexOf($activePrefix, [StringComparison]::OrdinalIgnoreCase)
    while ($index -ge 0) {
        $end = $index + $activePrefix.Length
        # Rewrite whole path segments only: '.../out/deps/w2-old' must not match '.../out/deps/w2'.
        if ($end -eq $expanded.Length -or $expanded[$end] -eq '/') {
            $expanded = $expanded.Substring(0, $index) + $stagingPrefix + $expanded.Substring($end)
            $index = $expanded.IndexOf($activePrefix, ($index + $stagingPrefix.Length),
                [StringComparison]::OrdinalIgnoreCase)
        }
        else {
            $index = $expanded.IndexOf($activePrefix, ($index + 1), [StringComparison]::OrdinalIgnoreCase)
        }
    }
    return $expanded
}

function Expand-RecipeFlags($Flags, [string]$ComponentId, [string]$W2Prefix = "") {
    $expanded = @()
    foreach ($flag in @($Flags)) {
        $expanded += Expand-RecipeToken ([string]$flag) "components.$ComponentId buildProfile flag" $W2Prefix
    }
    return @($expanded)
}

function Assert-NoRecipeToken($Component, [string]$ComponentId) {
    # DEC-024 rule 6: the token is admissible only in buildProfile.flags. The schema cannot
    # enforce this ($defs/relativePath rejects only a drive prefix, a leading slash and '..',
    # so a tokenised path passes validation - F-W2-25), therefore the driver enforces it.
    $names = @($Component.PSObject.Properties.Name)
    $fields = @()
    foreach ($output in @($Component.outputs)) {
        $fields += [pscustomobject]@{ label = "outputs[].path"; value = [string]$output.path }
    }
    if ($names -contains "licenseFiles") {
        foreach ($licenseFile in @($Component.licenseFiles)) {
            $fields += [pscustomobject]@{ label = "licenseFiles[].pathInSource"; value = [string]$licenseFile.pathInSource }
        }
    }
    if ($names -contains "patches") {
        foreach ($patch in @($Component.patches)) {
            $fields += [pscustomobject]@{ label = "patches[].file"; value = [string]$patch.file }
        }
    }
    if ($names -contains "expectedExecutables") {
        foreach ($executable in @($Component.expectedExecutables)) {
            $fields += [pscustomobject]@{ label = "expectedExecutables[]"; value = [string]$executable }
        }
    }
    foreach ($field in $fields) {
        if ($field.value -match '\$\{') {
            Stop-BuildDeps ("components.$ComponentId $($field.label) must not carry a substitution token " +
                "(DEC-024 rule 6): '$($field.value)'")
        }
    }
}

function Assert-CMakeRecipe($Component, [string]$ComponentId) {
    if ([string]$Component.buildProfile.system -ne "cmake") {
        Stop-BuildDeps "components.$ComponentId is not a cmake recipe"
    }
    if ([string]$Component.buildProfile.inherit -ne "toolchain") {
        Stop-BuildDeps "components.$ComponentId must inherit the pinned toolchain"
    }
    $flags = @($Component.buildProfile.flags)
    if ($flags.Count -le 0) {
        Stop-BuildDeps "components.$ComponentId declares no cmake flags"
    }
    if ("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded" -notin $flags) {
        Stop-BuildDeps "components.$ComponentId does not pin the static release CRT through CMAKE_MSVC_RUNTIME_LIBRARY"
    }
    if ("-DBUILD_SHARED_LIBS=ON" -in $flags) {
        Stop-BuildDeps "components.$ComponentId requests a shared library; W2 is a static-only wave"
    }
    foreach ($output in @($Component.outputs)) {
        Assert-RelativeSafePath ([string]$output.path) "components.$ComponentId.outputs.path"
    }
    Assert-NoRecipeToken $Component $ComponentId
}

function Assert-SharedCMakeRecipe($Component, [string]$ComponentId) {
    # W9. The shared-library variant of the cmake gate: the LGPL obliges dynamic linking, so
    # the recipe must say SHARED out loud. The CRT is deliberately NOT required to be pinned -
    # the owner's decision (DEC-060, ответ «Б») keeps W9 on cmake's documented /MD default for
    # DLLs - but a STATIC CRT flag is refused: a DLL carrying its own CRT copy has a separate
    # heap and allocator state, a known cross-boundary defect, not a configuration choice.
    if ([string]$Component.buildProfile.system -ne "cmake") {
        Stop-BuildDeps "components.$ComponentId is not a cmake recipe"
    }
    if ([string]$Component.buildProfile.inherit -ne "toolchain") {
        Stop-BuildDeps "components.$ComponentId must inherit the pinned toolchain"
    }
    $flags = @($Component.buildProfile.flags)
    if ($flags.Count -le 0) {
        Stop-BuildDeps "components.$ComponentId declares no cmake flags"
    }
    if ("-DLIBTYPE=SHARED" -notin $flags) {
        Stop-BuildDeps "components.$ComponentId does not declare -DLIBTYPE=SHARED; the LGPL wave must link dynamically"
    }
    if ("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded" -in $flags) {
        Stop-BuildDeps ("components.$ComponentId requests a static CRT inside a shared library; " +
            "a DLL with its own CRT copy is a cross-boundary defect, not a choice")
    }
    foreach ($output in @($Component.outputs)) {
        Assert-RelativeSafePath ([string]$output.path) "components.$ComponentId.outputs.path"
    }
    Assert-NoRecipeToken $Component $ComponentId
}

function Assert-B2Recipe($Component, [string]$ComponentId) {
    # W7. boost is the only component whose buildProfile.system is "b2". The recipe flags ARE
    # the b2 property list, so - exactly like the cl gate (F-W2-35) - this is the only place
    # the command line can be judged before it is executed.
    if ([string]$Component.buildProfile.system -ne "b2") {
        Stop-BuildDeps "components.$ComponentId is not a b2 recipe"
    }
    if ([string]$Component.buildProfile.inherit -ne "toolchain") {
        Stop-BuildDeps "components.$ComponentId must inherit the pinned toolchain"
    }
    $flags = @($Component.buildProfile.flags)
    if ($flags.Count -le 0) {
        Stop-BuildDeps "components.$ComponentId declares no b2 properties"
    }
    foreach ($required in @("link=static", "runtime-link=static", "variant=release",
        "address-model=64", "threading=multi")) {
        if ($required -notin $flags) {
            Stop-BuildDeps "components.$ComponentId does not pin '$required'; W7 is a static release /MT wave"
        }
    }
    foreach ($flag in $flags) {
        $value = [string]$flag
        if ($value -match '^(?i)(link|runtime-link)=shared$' -or $value -match '^(?i)variant=(debug|profile)$') {
            Stop-BuildDeps "components.$ComponentId carries the forbidden b2 property '$value'"
        }
        if ($value -match '^(?i)(toolset=|--user-config|--build-dir|--stagedir|--layout|--hash|-j)') {
            Stop-BuildDeps ("components.$ComponentId carries the driver-owned b2 argument '$value'; the driver " +
                "supplies it and the recipe must not, or the two would disagree silently")
        }
    }
    foreach ($output in @($Component.outputs)) {
        Assert-RelativeSafePath ([string]$output.path) "components.$ComponentId.outputs.path"
    }
    Assert-NoRecipeToken $Component $ComponentId
}

function Assert-ClRecipe($Component, [string]$ComponentId) {
    # F-W2-35. bc7enc is the only component in the lock whose buildProfile.system is "cl": it is
    # compiled straight out of the engine tree, so there is no build system to drive and the
    # flags in the recipe ARE the command line. That makes this gate the only place where the
    # command line can be judged before it is executed.
    if ([string]$Component.buildProfile.system -ne "cl") {
        Stop-BuildDeps "components.$ComponentId is not a cl recipe"
    }
    if ([string]$Component.buildProfile.inherit -ne "toolchain") {
        Stop-BuildDeps "components.$ComponentId must inherit the pinned toolchain"
    }
    if ([string]$Component.kind -ne "in-tree") {
        Stop-BuildDeps ("components.$ComponentId is driven by cl, which compiles sources that already live " +
            "in the repository, so kind must be 'in-tree'")
    }
    if (@($Component.patches).Count -gt 0) {
        Stop-BuildDeps ("components.$ComponentId declares patches, but a cl recipe compiles the engine tree " +
            "in place and this driver must never modify repository sources")
    }
    $flags = @($Component.buildProfile.flags)
    if ($flags.Count -le 0) {
        Stop-BuildDeps "components.$ComponentId declares no cl flags"
    }
    if ("/MT" -notin $flags) {
        Stop-BuildDeps "components.$ComponentId does not pin the static release CRT through /MT"
    }
    if ("/TC" -notin $flags) {
        Stop-BuildDeps ("components.$ComponentId does not force the C language through /TC; the sources are C " +
            "and cl must not be left to infer the language from the file name")
    }
    foreach ($flag in $flags) {
        $value = [string]$flag
        if ($value -match '^/M(?:D|Dd|Td)$') {
            Stop-BuildDeps ("components.$ComponentId requests a non-static or debug CRT through '$value'; " +
                "W2 is a static release /MT wave")
        }
        if ($value -match '^/EH') {
            Stop-BuildDeps ("components.$ComponentId carries the C++ exception flag '$value', which is " +
                "meaningless in a /TC translation unit and would only produce a warning")
        }
        if ($value -match '^/(?:LD|LDd|GL|LTCG)$') {
            Stop-BuildDeps ("components.$ComponentId carries '$value', which W2 forbids for a statically " +
                "archived object")
        }
        if ($value -match '^/(?:c|nologo|Fo|Fe|Fd|Brepro|pathmap|experimental)') {
            Stop-BuildDeps ("components.$ComponentId carries the driver-owned flag '$value'; the driver " +
                "supplies it and the recipe must not, or the two would disagree silently")
        }
    }
    foreach ($output in @($Component.outputs)) {
        Assert-RelativeSafePath ([string]$output.path) "components.$ComponentId.outputs.path"
    }
    Assert-NoRecipeToken $Component $ComponentId
}

function Get-ClSourcePlan([string]$ComponentId) {
    # deps.lock.json has no field that names an in-tree source root, and DEC-025 (answer P-2)
    # permits only a minimal edit of buildProfile.flags, so the layout is hard-coded here -
    # exactly as the W0 recipe hard-codes out\modern-bgfx\src\bx.
    # DEC-025 (answer P-1): only bc7enc16.c is compiled. Both symbols the engine links,
    # bc7enc16_compress_block_init and bc7enc16_compress_block, live in that one file;
    # bc7decomp.c (BC7 decoding) is not used by the engine and is not built.
    switch ($ComponentId) {
        "bc7enc" {
            return [pscustomobject]@{
                sourceInTree = "codecs\textures\bc7enc"
                sources = @("bc7enc16.c")
                headers = @("bc7enc16.h")
                licenseSource = "bc7enc16.c"
                licenseMarker = "This software is available under 2 licenses"
                licenseStaged = "licenses\bc7enc\LICENSE"
            }
        }
    }
    Stop-BuildDeps "no in-tree source plan is defined for components.$ComponentId"
}

function Get-CMakeStagePlan([string]$ComponentId, [string]$SourceRoot, [string]$BuildRoot, [string]$StagingRoot) {
    # F-W2-22. Neither component populates out/deps/w2 by itself: libzip is configured with
    # -DLIBZIP_DO_INSTALL=OFF and every install(...) call in zlib/CMakeLists.txt sits inside
    # if(ZLIB_INSTALL), which the recipe disables. 'cmake --install' is therefore inapplicable
    # and copying is the driver's duty. The origin column is normative, not cosmetic:
    # zconf.h and zipconf.h are generated into the build directory and differ from the
    # same-named files in the source tree.
    switch ($ComponentId) {
        "zlib" {
            return @(
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "Release\zs.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\zs.lib")
                },
                [pscustomobject]@{
                    role = "header"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "zlib.h")
                    to = (Join-PathLiteral $StagingRoot "include\zlib.h")
                },
                [pscustomobject]@{
                    role = "header"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "zconf.h")
                    to = (Join-PathLiteral $StagingRoot "include\zconf.h")
                },
                [pscustomobject]@{
                    # F-W2-48: the zlib LICENSE is pinned in the lock and verified at extraction,
                    # but the first W2 build shipped no copy of it into the staged licenses tree -
                    # unlike libzip and bc7enc. Third-party notices need the staged copy.
                    role = "license"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "LICENSE")
                    to = (Join-PathLiteral $StagingRoot "licenses\zlib\LICENSE")
                }
            )
        }
        "libzip" {
            return @(
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "lib\Release\zip.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\zip.lib")
                },
                [pscustomobject]@{
                    role = "header"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "lib\zip.h")
                    to = (Join-PathLiteral $StagingRoot "include\zip.h")
                },
                [pscustomobject]@{
                    role = "header"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "zipconf.h")
                    to = (Join-PathLiteral $StagingRoot "include\zipconf.h")
                },
                [pscustomobject]@{
                    role = "license"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "LICENSE")
                    to = (Join-PathLiteral $StagingRoot "licenses\libzip\LICENSE")
                }
            )
        }
        "sdl2" {
            # W4. Two libraries from one cmake build: SDL_SHARED=OFF makes the static target's
            # OUTPUT_NAME plain "SDL2" (CMakeLists line 2096), and SDL2main is always static.
            # The header tree travels through Copy-StagedHeaderTree in the wave driver
            # (DEC-066, phase A).
            return @(
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "Release\SDL2.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\SDL2.lib")
                },
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "Release\SDL2main.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\SDL2main.lib")
                },
                [pscustomobject]@{
                    role = "license"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "COPYING.txt")
                    to = (Join-PathLiteral $StagingRoot "licenses\sdl2\COPYING.txt")
                }
            )
        }
        "freetype" {
            # W3. The plan carries the library and the licence; the header tree travels through
            # Copy-StagedHeaderTree in the wave driver (DEC-070, phase A) because it is a whole
            # directory, not a fixed file list like the entries here.
            return @(
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "Release\freetype.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\freetype.lib")
                },
                [pscustomobject]@{
                    role = "license"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "docs\FTL.TXT")
                    to = (Join-PathLiteral $StagingRoot "licenses\freetype\FTL.TXT")
                }
            )
        }
        "bullet" {
            # W5. Five static libraries from one cmake build. Bullet's root CMakeLists sets
            # LIBRARY_OUTPUT_PATH to ${CMAKE_BINARY_DIR}/lib (the distributable-projectfiles
            # branch does not apply), so the multi-config generator writes lib\Release\ inside
            # the build tree - the libzip shape, not the sdl2/freetype Release\ shape. Release
            # names carry no postfix (only Debug/MinSizeRel/RelWithDebInfo get one). The header
            # tree travels through Copy-StagedHeaderTree in the wave driver (DEC-073, phase A).
            return @(
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "lib\Release\BulletCollision.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\BulletCollision.lib")
                },
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "lib\Release\BulletDynamics.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\BulletDynamics.lib")
                },
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "lib\Release\BulletInverseDynamics.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\BulletInverseDynamics.lib")
                },
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "lib\Release\BulletSoftBody.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\BulletSoftBody.lib")
                },
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "lib\Release\LinearMath.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\LinearMath.lib")
                },
                [pscustomobject]@{
                    role = "license"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "LICENSE.txt")
                    to = (Join-PathLiteral $StagingRoot "licenses\bullet\LICENSE.txt")
                }
            )
        }
        "openal-soft" {
            # W9. The first SHARED wave: the LGPL requires dynamic linking, so the recipe pins
            # -DLIBTYPE=SHARED and the outputs are the DLL plus its import library. With the
            # Visual Studio generator and no output-directory overrides in upstream's
            # CMakeLists, both land in Release\ at the build root. CRT: the recipe deliberately
            # carries no CMAKE_MSVC_RUNTIME_LIBRARY - the owner's decision (DEC-060, ответ «Б»)
            # is to ride cmake's documented default for a DLL (/MD); the shared-recipe gate
            # still refuses a static-CRT flag, which WOULD be a defect in a DLL.
            return @(
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "Release\OpenAL32.dll")
                    to = (Join-PathLiteral $StagingRoot "bin\OpenAL32.dll")
                },
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "Release\OpenAL32.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\OpenAL32.lib")
                },
                [pscustomobject]@{
                    role = "license"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "COPYING")
                    to = (Join-PathLiteral $StagingRoot "licenses\openal-soft\COPYING")
                }
            )
        }
        "assimp" {
            # W6. One static library. Assimp's root CMakeLists routes archives through
            # ASSIMP_ARCHIVE_OUTPUT_DIRECTORY = ${CMAKE_CURRENT_BINARY_DIR}/lib (the
            # CMAKE_ARCHIVE_OUTPUT_DIRECTORY branch does not apply - the recipe does not set
            # it), so the lib\Release shape of libzip and bullet. The name is upstream's own
            # scheme: vc${MSVC_TOOLSET_VERSION} for the pinned v143 toolset plus the "-mt"
            # suffix; the Debug "d" postfix does not apply to a Release build. The bundled
            # zlibstatic.lib (ASSIMP_BUILD_ZLIB=ON) lands beside it and is deliberately not
            # staged: no lock output names it and the engine links its own zlib. The header
            # tree travels through Copy-StagedHeaderTree in the wave driver (DEC-075, phase A).
            return @(
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "lib\Release\assimp-vc143-mt.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\assimp-vc143-mt.lib")
                },
                [pscustomobject]@{
                    role = "license"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "LICENSE")
                    to = (Join-PathLiteral $StagingRoot "licenses\assimp\LICENSE")
                }
            )
        }
        "manifold" {
            # W10. One static library, the last component of the programme. MEASURED on the
            # first W10 build (DEC-064): manifold routes archives through an output-directory
            # override into lib\Release\ at the build root - the libzip/bullet/assimp shape,
            # not the per-subdirectory src\Release\ this plan first expected from reading the
            # tree. The build also produces manifoldc.lib (the C bindings) and a Clipper2
            # archive under _deps\ (manifold's own FetchContent dependency); neither is
            # staged - no lock output names them, the assimp/zlibstatic precedent. NOTICE is
            # optional in the lock and absent from the measured archive (DEC-063), so only
            # LICENSE is staged.
            return @(
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $BuildRoot "lib\Release\manifold.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\manifold.lib")
                },
                [pscustomobject]@{
                    role = "license"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "LICENSE")
                    to = (Join-PathLiteral $StagingRoot "licenses\manifold\LICENSE")
                }
            )
        }
    }
    Stop-BuildDeps "no staging plan is defined for components.$ComponentId"
}

function Get-MsBuildStagePlan([string]$ComponentId, [string]$SourceRoot, [string]$StagingRoot) {
    # FreeImage builds in-source: the upstream project writes its output under the project's own
    # x64\Release directory, so unlike the cmake plans there is no separate build root here.
    switch ($ComponentId) {
        "freeimage" {
            return @(
                [pscustomobject]@{
                    role = "library"; origin = "build"
                    from = (Join-PathLiteral $SourceRoot "Source\FreeImageLib\x64\Release\FreeImageLib.lib")
                    to = (Join-PathLiteral $StagingRoot "lib\FreeImageLib.lib")
                },
                [pscustomobject]@{
                    role = "header"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "Source\FreeImage.h")
                    to = (Join-PathLiteral $StagingRoot "include\FreeImage.h")
                },
                [pscustomobject]@{
                    role = "license"; origin = "source"
                    from = (Join-PathLiteral $SourceRoot "license-fi.txt")
                    to = (Join-PathLiteral $StagingRoot "licenses\freeimage\license-fi.txt")
                }
            )
        }
    }
    Stop-BuildDeps "no msbuild staging plan is defined for components.$ComponentId"
}

function Get-B2StagePlan([string]$ComponentId, [string]$SourceRoot, [string]$B2StageRoot, [string]$StagingRoot) {
    # W7. b2's own 'stage' target collects the built archives into <--stagedir>\lib, so unlike
    # the cmake plans the origin directory here is b2's stage tree, not the build tree. The file
    # names are upstream's versioned layout tagging for the pinned toolchain: vc143 (compiler),
    # mt (threading=multi), s (runtime-link=static), x64 (address-model=64), 1_74 (release).
    # The header forest travels through Copy-StagedHeaderTree in the wave driver (DEC-083).
    switch ($ComponentId) {
        "boost" {
            $libraries = @("filesystem", "system", "serialization", "regex", "iostreams",
                "date_time", "thread", "chrono")
            $plan = @()
            foreach ($name in $libraries) {
                $fileName = "libboost_$name-vc143-mt-s-x64-1_74.lib"
                $plan += [pscustomobject]@{
                    role = "library"; origin = "b2-stage"
                    from = (Join-PathLiteral $B2StageRoot "lib\$fileName")
                    to = (Join-PathLiteral $StagingRoot "lib\$fileName")
                }
            }
            $plan += [pscustomobject]@{
                role = "license"; origin = "source"
                from = (Join-PathLiteral $SourceRoot "LICENSE_1_0.txt")
                to = (Join-PathLiteral $StagingRoot "licenses\boost\LICENSE_1_0.txt")
            }
            return $plan
        }
    }
    Stop-BuildDeps "no b2 staging plan is defined for components.$ComponentId"
}

function Assert-MsBuildRecipe($Component, [string]$ComponentId) {
    # W3. freeimage is the only component whose buildProfile.system is "msbuild": the upstream
    # ships Visual Studio projects and no CMakeLists. The recipe flags are property assignments,
    # and this gate is the only place they can be judged before MSBuild consumes them.
    if ([string]$Component.buildProfile.system -ne "msbuild") {
        Stop-BuildDeps "components.$ComponentId is not an msbuild recipe"
    }
    if ([string]$Component.buildProfile.inherit -ne "toolchain") {
        Stop-BuildDeps "components.$ComponentId must inherit the pinned toolchain"
    }
    $flags = @($Component.buildProfile.flags)
    if ($flags.Count -le 0) {
        Stop-BuildDeps "components.$ComponentId declares no msbuild flags"
    }
    foreach ($required in @("Configuration=Release", "Platform=x64", "RuntimeLibrary=MultiThreaded")) {
        if ($required -notin $flags) {
            Stop-BuildDeps "components.$ComponentId does not pin '$required' in its msbuild recipe"
        }
    }
    if ("FREEIMAGE_LIB" -notin $flags) {
        Stop-BuildDeps ("components.$ComponentId does not declare FREEIMAGE_LIB; without it the headers " +
            "describe a DLL interface while the output is a static library")
    }
    foreach ($output in @($Component.outputs)) {
        Assert-RelativeSafePath ([string]$output.path) "components.$ComponentId.outputs.path"
    }
    Assert-NoRecipeToken $Component $ComponentId
}

function Assert-MsiexecRecipe($Component, [string]$ComponentId) {
    # W8. mono ships as upstream-built Windows binaries inside an MSI: there is no compiler
    # step, so this gate pins the extraction contract instead of compiler properties - the
    # administrative image mode (/a /qn, nothing is installed), the exact MSI identity, the
    # exact licence text, and the exact source-offer archive that the LGPL obliges the
    # project to hold alongside the binaries (DEC-053, DEC-054).
    if ([string]$Component.kind -ne "binary") {
        Stop-BuildDeps "components.$ComponentId is not a binary component"
    }
    if ([string]$Component.buildProfile.system -ne "msiexec-extract") {
        Stop-BuildDeps "components.$ComponentId is not an msiexec-extract recipe"
    }
    if ([string]$Component.buildProfile.inherit -ne "toolchain") {
        Stop-BuildDeps "components.$ComponentId must inherit the pinned toolchain"
    }
    $flags = @($Component.buildProfile.flags | ForEach-Object { [string]$_ })
    if (($flags -join "|") -ne "/a|/qn|no-install") {
        Stop-BuildDeps ("components.$ComponentId must pin exactly the msiexec-extract flags " +
            "'/a', '/qn', 'no-install'; found: '$($flags -join ' ')'")
    }
    if (@($Component.PSObject.Properties.Name) -notcontains "archive") {
        Stop-BuildDeps "components.$ComponentId declares no archive"
    }
    if (-not ([string]$Component.archive.url).ToLowerInvariant().EndsWith(".msi")) {
        Stop-BuildDeps "components.$ComponentId archive is not an MSI"
    }
    Assert-PinnedSha256 ([string]$Component.archive.sha256) "components.$ComponentId.archive.sha256"
    if ([long]$Component.archive.bytes -le 0) {
        Stop-BuildDeps "components.$ComponentId archive bytes are not pinned"
    }
    $licenses = @($Component.licenseFiles)
    if ($licenses.Count -ne 1 -or [string]$licenses[0].pathInSource -ne "LICENSE") {
        Stop-BuildDeps "components.$ComponentId must declare exactly one licence file named LICENSE"
    }
    Assert-PinnedSha256 ([string]$licenses[0].sha256) "components.$ComponentId licence SHA-256"
    if (@($Component.PSObject.Properties.Name) -notcontains "sourceOffer") {
        Stop-BuildDeps "components.$ComponentId declares no source offer; the LGPL requires one"
    }
    if (-not ([string]$Component.sourceOffer.url).StartsWith("https://", [StringComparison]::OrdinalIgnoreCase)) {
        Stop-BuildDeps "components.$ComponentId source offer URL must use HTTPS"
    }
    Assert-PinnedSha256 ([string]$Component.sourceOffer.sha256) "components.$ComponentId.sourceOffer.sha256"
    foreach ($output in @($Component.outputs)) {
        Assert-RelativeSafePath ([string]$output.path) "components.$ComponentId.outputs.path"
    }
    Assert-NoRecipeToken $Component $ComponentId
}

function Assert-McsRecipe($Component, [string]$ComponentId) {
    # W8b. FalcoEngine.dll is compiled from the project's own API\ sources by the mcs.exe that
    # W8 staged and the lock pins byte for byte. The recipe gate pins that contract: the lock's
    # msbuild-typed profile must carry exactly the three declared properties (the producer IS
    # mcs from W8 - a recipe silently pointing elsewhere must stop), and the component must
    # depend on mono-msi and nothing else, because the compiler comes from there (DEC-057).
    if ([string]$Component.kind -ne "in-tree") {
        Stop-BuildDeps "components.$ComponentId is not an in-tree component"
    }
    if ([string]$Component.buildProfile.system -ne "msbuild") {
        Stop-BuildDeps "components.$ComponentId is not the approved msbuild-typed recipe"
    }
    if ([string]$Component.buildProfile.inherit -ne "toolchain") {
        Stop-BuildDeps "components.$ComponentId must inherit the pinned toolchain"
    }
    $flags = @($Component.buildProfile.flags | ForEach-Object { [string]$_ })
    if (($flags -join "|") -ne "producer=mcs-from-W8|target=library|sdk=4.8") {
        Stop-BuildDeps ("components.$ComponentId must pin exactly the flags 'producer=mcs-from-W8', " +
            "'target=library', 'sdk=4.8'; found: '$($flags -join ' ')'")
    }
    if ((@($Component.dependsOn | ForEach-Object { [string]$_ }) -join "|") -ne "mono-msi") {
        Stop-BuildDeps "components.$ComponentId must depend on exactly 'mono-msi'"
    }
    $outputs = @($Component.outputs)
    if ($outputs.Count -ne 1) {
        Stop-BuildDeps "components.$ComponentId must declare exactly one output"
    }
    Assert-RelativeSafePath ([string]$outputs[0].path) "components.$ComponentId.outputs.path"
    Assert-NoRecipeToken $Component $ComponentId
}

function Get-CsprojCompilePlan([string]$CsprojPath, [string]$ComponentId) {
    # W8b. The .csproj is the authoritative statement of what the assembly is made of: the
    # explicit <Compile Include> list (no -recurse globbing - a stray .cs file must not slip
    # in) and the <Reference Include> list of framework assemblies. Parsed with an explicit
    # XML namespace because the empty <ItemGroup /> in this very project makes property-style
    # traversal die under strict mode (measured in the cs27 probe).
    if (-not (Test-Path -LiteralPath $CsprojPath -PathType Leaf)) {
        Stop-BuildDeps "components.$ComponentId project file not found: $CsprojPath"
    }
    $xml = New-Object Xml.XmlDocument
    $xml.Load($CsprojPath)
    $namespaces = New-Object Xml.XmlNamespaceManager $xml.NameTable
    $namespaces.AddNamespace("m", "http://schemas.microsoft.com/developer/msbuild/2003")
    $sources = @()
    foreach ($node in $xml.SelectNodes("//m:Compile/@Include", $namespaces)) {
        $include = [string]$node.Value
        Assert-RelativeSafePath $include "components.$ComponentId compile include"
        $sources += $include
    }
    if (@($sources).Count -le 0) {
        Stop-BuildDeps "components.$ComponentId project declares no source files"
    }
    $references = @()
    foreach ($node in $xml.SelectNodes("//m:Reference/@Include", $namespaces)) {
        $name = [string]$node.Value
        if ($name -notmatch '^[A-Za-z0-9.]+$') {
            Stop-BuildDeps "components.$ComponentId reference is not a plain assembly name: '$name'"
        }
        $references += $name
    }
    return [pscustomobject]@{ sources = @($sources); references = @($references) }
}

function Assert-MsiImageShape([string]$ImageRoot, [string]$MsiFileName, [string]$ComponentId) {
    # W8. Measured on the owner's probe (DEC-053): an administrative extract of the mono MSI
    # yields exactly Mono\ (the payload), System\ and System64\ (Microsoft VC++ redistributables
    # that a real installation would drop into Windows system directories) and a copy of the MSI
    # itself. Only Mono\ is staged; the redistributables are Microsoft system plumbing, not mono
    # code, and the MSI copy is extraction residue. Anything else at the top level means the
    # image does not match the measured layout and the wave must stop, not guess.
    $allowed = @{}
    foreach ($name in @("mono", "system", "system64", $MsiFileName.ToLowerInvariant())) {
        $allowed[$name] = $true
    }
    foreach ($entry in @(Get-ChildItem -LiteralPath $ImageRoot -Force)) {
        if (-not $allowed.ContainsKey($entry.Name.ToLowerInvariant())) {
            Stop-BuildDeps ("components.$ComponentId administrative image carries an unexpected " +
                "top-level entry: '$($entry.Name)'")
        }
    }
    $monoRoot = Join-Path $ImageRoot "Mono"
    if (-not (Test-Path -LiteralPath $monoRoot -PathType Container)) {
        Stop-BuildDeps "components.$ComponentId administrative image carries no Mono directory"
    }
    return $monoRoot
}

function Copy-StagedHeaderTree([string]$SourceDir, [string]$DestinationDir, [string]$ComponentId) {
    # Phase A of the retargeting programme (DEC-066). The engine's include paths must move
    # from the legacy vendored trees onto out\deps\, so each wave now stages the header tree
    # of its verified sources next to its pinned libraries. Only header-type files travel
    # (.h/.hpp/.hh/.inl/.ipp - boost needs the last two), the relative structure is preserved,
    # and an empty result is a stop: a wave that claims to stage headers and stages none is
    # lying to its consumers.
    if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
        Stop-BuildDeps "components.$ComponentId header source directory is missing: $SourceDir"
    }
    $sourceFull = Get-NormalizedFullPath $SourceDir
    $headerFiles = @(Get-ChildItem -LiteralPath $sourceFull -Recurse -Force -File |
        Where-Object { $_.Extension.ToLowerInvariant() -in @(".h", ".hpp", ".hh", ".inl", ".ipp") })
    if (@($headerFiles).Count -le 0) {
        Stop-BuildDeps "components.$ComponentId header source directory carries no header files: $SourceDir"
    }
    foreach ($file in $headerFiles) {
        $relative = $file.FullName.Substring($sourceFull.Length).TrimStart('\', '/')
        $destination = Assert-PathUnder (Join-PathLiteral $DestinationDir $relative) $DestinationDir `
            "components.$ComponentId staged header"
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
    }
    Assert-NoReparsePoints $DestinationDir
    $stagedCount = @(Get-ChildItem -LiteralPath $DestinationDir -Recurse -Force -File).Count
    if ($stagedCount -ne @($headerFiles).Count) {
        Stop-BuildDeps ("components.$ComponentId staged header count $stagedCount does not match " +
            "the source header count $(@($headerFiles).Count)")
    }
    Add-ReportEvent "stage" $ComponentId "PASS" "Header tree staged for retargeting" @{
        from = $sourceFull
        to = $DestinationDir
        headers = $stagedCount
    }
    return $stagedCount
}

function Get-StagedOutputPath([string]$OutputPath, [string]$StagingRoot, [string]$ComponentId,
    [string]$WavePrefix = "out/deps/w2/") {
    # The staging root becomes the active wave prefix atomically through Switch-WavePrefix, so
    # every declared output must be addressable inside the staging tree before activation. The
    # prefix defaults to W2 so that every pre-W3 call site keeps its exact behaviour; W3 passes
    # its own prefix explicitly.
    $normalized = $OutputPath.Replace('\', '/')
    $prefix = $WavePrefix
    if (-not $normalized.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        Stop-BuildDeps "components.$ComponentId output '$OutputPath' is not inside the wave prefix '$prefix'"
    }
    $relative = $normalized.Substring($prefix.Length)
    if ([string]::IsNullOrWhiteSpace($relative)) {
        Stop-BuildDeps "components.$ComponentId output '$OutputPath' does not name a file inside the wave prefix"
    }
    return Assert-PathUnder (Join-PathLiteral $StagingRoot ($relative.Replace('/', '\'))) $StagingRoot `
        "components.$ComponentId staged output"
}

function Copy-StagedArtifacts($Plan, [string]$ComponentId, [string]$StagingRoot) {
    $staged = @()
    foreach ($item in @($Plan)) {
        if (-not (Test-Path -LiteralPath $item.from -PathType Leaf)) {
            Stop-BuildDeps ("components.$ComponentId staged $($item.role) is missing in the $($item.origin) " +
                "tree: $($item.from)")
        }
        $destination = Assert-PathUnder $item.to $StagingRoot "components.$ComponentId staged $($item.role)"
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $item.from -Destination $destination -Force
        $identity = Get-FileIdentity $destination
        Add-ReportEvent "stage" $ComponentId "PASS" "Staged $($item.role) from the $($item.origin) tree" @{
            from = $item.from
            to = $destination
            origin = $item.origin
            bytes = $identity.bytes
            sha256 = $identity.sha256
        }
        $staged += $destination
    }
    return @($staged)
}

function Get-W2RecipeFingerprint($Lock) {
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $patchIdentities = @()
    foreach ($name in @("zlib", "libzip")) {
        foreach ($patch in @($Lock.components.$name.patches)) {
            $patchPath = Join-Path $script:RepoRoot ([string]$patch.file)
            $patchIdentity = Get-FileIdentity $patchPath
            $patchIdentities += [ordered]@{
                component = $name
                file = [string]$patch.file
                bytes = $patchIdentity.bytes
                sha256 = $patchIdentity.sha256
            }
        }
    }
    # bc7enc is built from sources that live in the repository, so no archive hash covers it.
    # Its recipe and the identity of every file it compiles must therefore enter the
    # fingerprint: without them an edited in-tree source would leave the recorded wave state
    # looking current and the wave would never be rebuilt.
    $inTreeIdentities = @()
    foreach ($name in @("bc7enc")) {
        if ([string]$Lock.components.$name.buildProfile.system -ne "cl") { continue }
        $plan = Get-ClSourcePlan $name
        $inTreeRoot = Join-PathLiteral $script:RepoRoot $plan.sourceInTree
        foreach ($relative in (@($plan.sources) + @($plan.headers))) {
            $inTreePath = Join-PathLiteral $inTreeRoot $relative
            if (-not (Test-Path -LiteralPath $inTreePath -PathType Leaf)) {
                Stop-BuildDeps "components.$name in-tree source is missing and cannot be fingerprinted: $inTreePath"
            }
            $inTreeIdentity = Get-FileIdentity $inTreePath
            $inTreeIdentities += [ordered]@{
                component = $name
                file = $relative
                bytes = $inTreeIdentity.bytes
                sha256 = $inTreeIdentity.sha256
            }
        }
    }
    $value = [ordered]@{
        toolchain = $Lock.toolchain
        zlib = $Lock.components.zlib
        libzip = $Lock.components.libzip
        bc7enc = $Lock.components.bc7enc
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        patches = $patchIdentities
        inTreeSources = $inTreeIdentities
    } | ConvertTo-Json -Depth 20 -Compress
    return Get-StringSha256 $value
}

function Invoke-CMakeStep([string]$CMake, [string]$ComponentId, [string]$StepName, [string[]]$Arguments) {
    $logPath = Join-Path $script:RunRoot "$ComponentId-$StepName.log"
    New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
    # F-W2-51: cmake writes its warnings - including the deprecation warning that a
    # CMAKE_POLICY_VERSION_MINIMUM override provokes on old projects - to stderr, and the bare
    # capture below repeated F-W2-40 for the cmake path: the first stderr line became a
    # terminating ErrorRecord before the exit-code gate. Route through Invoke-NativeCapture,
    # exactly as the cl path does; the gate stays the exit code and the log (F-W2-18).
    $capture = Invoke-NativeCapture $CMake $Arguments
    $output = $capture.output
    $exitCode = $capture.exitCode
    [IO.File]::WriteAllText($logPath, (($output | Out-String)), (New-Object Text.UTF8Encoding($false)))
    if ($exitCode -ne 0) {
        $output | Select-Object -Last 80 | ForEach-Object { Write-Output $_ }
        Stop-BuildDeps "cmake $StepName failed for components.$ComponentId with exit code $exitCode; log: $logPath"
    }
    Add-ReportEvent "build" $ComponentId "PASS" "cmake $StepName completed" @{ log = $logPath }
    return $logPath
}

function Write-B2UserConfig([string]$Path, [string]$ClPath, [string]$SetupPath) {
    # W7. Boost 1.74's own toolset auto-detection predates Visual Studio 2022, so the compiler
    # is named explicitly instead of being guessed: version 14.3 gives the vc143 tag that the
    # pinned output names carry, and the command is the exact cl.exe from the resolved toolchain
    # identity. Forward slashes because a backslash opens an escape in a quoted jam string.
    #
    # F-W2-57: naming the command is not enough. msvc.jam still has to find the environment
    # script vcvarsall.bat, and for 14.3 it cannot: .known-versions ends at 14.2 (msvc.jam
    # line 2047); locate-default-setup only GLOBs the command's own directory and its parent
    # (line 1944); and the "five levels up to VC\Auxiliary\Build" correction in
    # generate-setup-cmd matches only the literal strings 14.2 and 14.1 (lines 1028-1068),
    # while our exact-version is 14.44.35207. So the search returns nothing, upstream falls
    # back to inventing "<cl directory>\vcvarsall.bat", which does not exist, and
    # set-setup-command (line 1112) then makes every object and archive target depend on an
    # msvc-setup.nup that can never be built - measured as "...skipped 121 targets..." in the
    # fourth W7 run. The documented <setup> option pre-empts that search, because upstream
    # assigns the located default with ?= (lines 1374-1377). The path must be spelled with
    # forward slashes: measured with --debug-configuration, the forward-slash form reports
    # setup: 'C:/.../vcvarsall.bat amd64' for every condition, while the native backslash form
    # reports setup: '' - the value is silently discarded (probes A and B, DEC-049).
    if (-not $ClPath) { Stop-BuildDeps "the b2 user-config requires the pinned cl.exe path" }
    if ($ClPath.Contains('"')) {
        Stop-BuildDeps "the pinned cl path must not carry a double quote: $ClPath"
    }
    if (-not $SetupPath) { Stop-BuildDeps "the b2 user-config requires the pinned vcvarsall.bat path" }
    if ($SetupPath.Contains('"')) {
        Stop-BuildDeps "the pinned setup script path must not carry a double quote: $SetupPath"
    }
    $content = "using msvc : 14.3 : `"$($ClPath.Replace('\', '/'))`" : " +
        "<setup>`"$($SetupPath.Replace('\', '/'))`" ;`n"
    if ($content.Contains([string][char]92)) {
        Stop-BuildDeps "the generated b2 user-config must carry no backslash: $content"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $content, (New-Object Text.UTF8Encoding($false)))
    return $Path
}

function Get-B2ToolsetRoot([string]$InstallationPath) {
    # F-W2-56. bootstrap.bat's :Config_VC142 branch configures the compiler environment by
    # calling "%B2_TOOLSET_ROOT%Auxiliary\Build\vcvarsall.bat", and when B2_TOOLSET_ROOT is
    # empty it fills it from %VS160COMNTOOLS% - the Visual Studio 2019 variable, absent on a
    # VS2022-only machine. The guard around that fallback is if "_%B2_TOOLSET_ROOT%_" == "__",
    # so a value set by the caller survives untouched. This composes that value from the same
    # installation path the toolchain gate already resolved. The trailing separator is part of
    # the contract: upstream concatenates the remainder of the path directly onto it.
    if (-not $InstallationPath) {
        Stop-BuildDeps "the Visual Studio installation path is required to derive the b2 toolset root"
    }
    if ($InstallationPath.Contains('"')) {
        Stop-BuildDeps "the Visual Studio installation path must not carry a double quote: $InstallationPath"
    }
    return (Join-PathLiteral $InstallationPath "VC") + [string][char]92
}

function Invoke-B2Step([string]$Executable, [string[]]$Arguments, [string]$ComponentId,
    [string]$StepName, [string]$WorkingDirectory, [hashtable]$Environment) {
    # The same capture shape as Invoke-CMakeStep (F-W2-51): bootstrap and b2 both write progress
    # and warnings to stderr, and the gate must stay the exit code and the log (F-W2-18). The
    # working directory matters: bootstrap.bat writes b2.exe into the directory it runs from.
    # $Environment names variables the child must see (F-W2-56). They are set on this process
    # for the duration of the call only and restored afterwards, including restoration to
    # "absent" when there was no previous value; a previous value is captured solely to be put
    # back and is never inspected, reported or logged.
    $logPath = Join-Path $script:RunRoot "$ComponentId-$StepName.log"
    New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
    $savedEnvironment = @{}
    if ($null -ne $Environment) {
        foreach ($name in @($Environment.Keys)) {
            $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
            [Environment]::SetEnvironmentVariable($name, [string]$Environment[$name], "Process")
        }
    }
    Push-Location $WorkingDirectory
    try {
        $capture = Invoke-NativeCapture $Executable $Arguments
    }
    finally {
        Pop-Location
        foreach ($name in @($savedEnvironment.Keys)) {
            [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], "Process")
        }
    }
    [IO.File]::WriteAllText($logPath, (($capture.output | Out-String)), (New-Object Text.UTF8Encoding($false)))
    if ($capture.exitCode -ne 0) {
        $capture.output | Select-Object -Last 80 | ForEach-Object { Write-Output $_ }
        Stop-BuildDeps "b2 $StepName failed for components.$ComponentId with exit code $($capture.exitCode); log: $logPath"
    }
    if ($StepName -eq 'stage' -and @($capture.output | Where-Object { [string]$_ -match '\bD900[27]\b' }).Count) {
        Stop-BuildDeps "b2 compiler ignored an option; log: $logPath"
    }
    Add-ReportEvent "build" $ComponentId "PASS" "b2 $StepName completed" @{ log = $logPath }
    return $logPath
}

function Get-CMakeDeterminismArguments() {
    # F-W2-62 / DEC-067. lib.exe stamps every archive member with the build-time clock, so a
    # byte-honest rebuild of an unchanged W4 recipe produced a different SDL2.lib (+2 bytes,
    # different hash) and the pin gate rightly stopped: no cmake wave was rebuild-reproducible,
    # and the pins had held only because a pinned wave had never been rebuilt. /Brepro replaces
    # those timestamps with a reproducible value. The librarian takes it through the static
    # linker flags; the Release-config variable is empty by default under this generator
    # (measured), so setting it is a pure addition, not an override of a default.
    return @("-DCMAKE_STATIC_LINKER_FLAGS_RELEASE=/Brepro")
}

function Get-CMakeDeterminismEnvironment() {
    # The compiler side of F-W2-62: cl.exe embeds timestamps in .obj members unless /Brepro.
    # cmake folds $env:CFLAGS / $env:CXXFLAGS into the initial CMAKE_C_FLAGS / CMAKE_CXX_FLAGS
    # at the first configure, additively to the platform defaults - passing -DCMAKE_C_FLAGS on
    # the command line would REPLACE those defaults instead. Probed on W4: with this
    # environment and the librarian flag, a same-directory double build became byte-identical.
    # LDFLAGS seeds the exe/shared/module linker flag variables by the same documented cmake
    # mechanism, for the one wave that links a DLL (W9); its effect is proven by that wave's
    # own double build before any re-pin. The environment is applied around the configure step
    # only: cmake bakes the values into the cache, and every cmake wave wipes its build
    # directory before configuring, so the cache is always freshly seeded. The build directory
    # itself must stay fixed per component - lib.exe embeds .obj paths, and equal-length but
    # different directories were measured to give equal sizes with different hashes - which the
    # wave drivers already guarantee with their constant out/deps/_build/<wave> locations.
    return [ordered]@{ CFLAGS = "/Brepro"; CXXFLAGS = "/Brepro"; LDFLAGS = "/Brepro" }
}

function Invoke-CMakeBuild([string]$CMake, [string]$ComponentId, $Component, [string]$SourceRoot,
    [string]$BuildRoot, [string]$VcToolsVersion, [string]$StagingRoot, [string[]]$ExtraConfigureArguments = @()) {
    if (-not $VcToolsVersion) {
        Stop-BuildDeps "cmake invocation requires the pinned MSVC tools version"
    }
    if (-not $StagingRoot) {
        Stop-BuildDeps "cmake invocation requires the W2 staging root for prefix redirection"
    }
    New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
    $flags = Expand-RecipeFlags $Component.buildProfile.flags $ComponentId $StagingRoot
    Add-ReportEvent "build" $ComponentId "PASS" "Recipe flags expanded for cmake" @{
        declared = @($Component.buildProfile.flags)
        effective = @($flags)
        stagingRoot = $StagingRoot
    }
    $determinismArguments = Get-CMakeDeterminismArguments
    $determinismEnvironment = Get-CMakeDeterminismEnvironment
    if ($ComponentId -in @('zlib', 'libzip', 'freetype', 'sdl2', 'bullet', 'assimp', 'openal-soft', 'manifold')) {
        # Separate maps are required by MSVC; a semicolon becomes part of the
        # replacement string. CMake already uses relative archive member names.
        $mapPrefix = switch ($ComponentId) { 'freetype' { 'W3' }; 'sdl2' { 'W4' }; 'bullet' { 'W5' }; 'assimp' { 'W6' }; 'openal-soft' { 'W9' }; 'manifold' { 'W10' }; default { 'W2' } }
        $maps = " /experimental:deterministic /pathmap:`"$SourceRoot=$($mapPrefix)SRC`" /pathmap:`"$BuildRoot=$($mapPrefix)OBJ`" /pathmap:`"$StagingRoot=$($mapPrefix)OUT`""
        $determinismEnvironment.CFLAGS += $maps
        $determinismEnvironment.CXXFLAGS += $maps
    }
    $configureArguments = @(
        "-S", $SourceRoot,
        "-B", $BuildRoot,
        "-G", "Visual Studio 17 2022",
        "-A", "x64",
        "-T", "version=$VcToolsVersion",
        "-DCMAKE_CONFIGURATION_TYPES=Release"
    ) + $determinismArguments + $flags + $ExtraConfigureArguments
    Add-ReportEvent "build" $ComponentId "PASS" "Determinism flags injected (F-W2-62)" @{
        configureArguments = @($determinismArguments)
        environmentNames = @($determinismEnvironment.Keys)
        value = "/Brepro"
    }
    # F-W2-62 / DEC-067: the environment is set for the configure step only and restored in
    # finally - the Invoke-B2Step discipline: a previous value is captured solely to be put
    # back, including restoration to "absent", and is never inspected, reported or logged.
    $savedEnvironment = @{}
    foreach ($name in @($determinismEnvironment.Keys)) {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
        [Environment]::SetEnvironmentVariable($name, [string]$determinismEnvironment[$name], "Process")
    }
    try {
        $null = Invoke-CMakeStep $CMake $ComponentId "configure" $configureArguments
    }
    finally {
        foreach ($name in @($savedEnvironment.Keys)) {
            [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], "Process")
        }
    }
    $null = Invoke-CMakeStep $CMake $ComponentId "compile" @("--build", $BuildRoot, "--config", "Release")
    Write-Output "PASS: $ComponentId configured and compiled with cmake (Visual Studio 17 2022 x64, toolset $VcToolsVersion)."
}

function Export-EmbeddedLicenseBlock([string]$SourcePath, [string]$Marker, [string]$DestinationPath,
    [string]$ComponentId) {
    # bc7enc ships no LICENSE file: the dual MIT / Unlicense grant is a comment block at the end
    # of bc7enc16.c. A staged license must be the grant and nothing else, so the block is
    # delimited by the dashed rails that frame it and the enclosing /* */ is excluded - a file
    # carrying a stray comment terminator is not a license text.
    if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf)) {
        Stop-BuildDeps "components.$ComponentId license source is missing: $SourcePath"
    }
    $text = [System.IO.File]::ReadAllText($SourcePath)
    $lines = @($text -split "`r?`n")
    $markerIndex = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Contains($Marker)) { $markerIndex = $i; break }
    }
    if ($markerIndex -lt 0) {
        Stop-BuildDeps "components.$ComponentId license marker '$Marker' was not found in $SourcePath"
    }
    $rail = '^-{70,}\s*$'
    if ($markerIndex -lt 1 -or $lines[$markerIndex - 1] -notmatch $rail) {
        Stop-BuildDeps ("components.$ComponentId license marker is not preceded by the dashed rail that " +
            "opens the grant block in $SourcePath")
    }
    $start = $markerIndex - 1
    $end = -1
    for ($i = $lines.Count - 1; $i -gt $start; $i--) {
        if ($lines[$i] -match $rail) { $end = $i; break }
    }
    if ($end -lt 0) {
        Stop-BuildDeps "components.$ComponentId license block has no closing dashed rail in $SourcePath"
    }
    $block = @()
    for ($i = $start; $i -le $end; $i++) { $block += $lines[$i] }
    if ($block.Count -lt 10) {
        Stop-BuildDeps ("components.$ComponentId license block is implausibly short ($($block.Count) lines) " +
            "and is unlikely to be the full grant")
    }
    foreach ($line in $block) {
        if ($line.Contains("/*") -or $line.Contains("*/")) {
            Stop-BuildDeps ("components.$ComponentId license block carries a comment delimiter and would not " +
                "be a standalone license text: '$line'")
        }
    }
    $content = ($block -join "`n") + "`n"
    New-Item -ItemType Directory -Path (Split-Path -Parent $DestinationPath) -Force | Out-Null
    [IO.File]::WriteAllText($DestinationPath, $content, (New-Object Text.UTF8Encoding($false)))
    $identity = Get-FileIdentity $DestinationPath
    $extract = [pscustomobject]@{
        path = $DestinationPath
        firstLine = ($start + 1)
        lastLine = ($end + 1)
        lines = $block.Count
        bytes = $identity.bytes
        sha256 = $identity.sha256
    }
    Add-ReportEvent "license" $ComponentId "PASS" "Embedded license grant extracted from the in-tree source" @{
        from = $SourcePath
        to = $DestinationPath
        firstLine = $extract.firstLine
        lastLine = $extract.lastLine
        lines = $extract.lines
        bytes = $extract.bytes
        sha256 = $extract.sha256
    }
    return $extract
}

function Invoke-NativeCapture([string]$Executable, [string[]]$Arguments) {
    # Windows PowerShell 5.1 with $ErrorActionPreference = "Stop" promotes every stderr line a
    # native tool writes - once it is merged into the success stream by 2>&1 - into a TERMINATING
    # ErrorRecord. cl.exe reports its warnings on stderr, so one warning aborted the whole run
    # before the exit-code gate below could ever be reached: in CS07C a single D9007 killed the W2
    # build with zlib and libzip already compiled, and not even the log file was written, because
    # the write sits on the line after the invocation. F-W2-18 requires the gate to be the exit
    # code and the artefact and never the error text - which is only possible if stderr is allowed
    # to arrive as data. Invoke-FixtureLockCheck already uses this exact save/restore shape.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Executable @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    return [pscustomobject]@{ output = @($output); exitCode = $exitCode }
}

# F-W2-44: bc7enc is the only component this driver compiles by calling cl.exe directly. Everything
# else reaches the compiler through MSBuild (W1) or through cmake's Visual Studio generator (zlib,
# libzip), and both of those establish the compiler's include search list for themselves. A bare
# PowerShell process has no such list, so the first bc7enc compile that ever ran - CS07F - died with
# "fatal error C1034: stdlib.h: no include search path" before reading a line of the codec. The
# defect had been dormant since the cl driver was written, because until CS07F no cl invocation had
# ever got as far as opening a source file.
# DEC-028 (owner answer 1-A): the list is asked of Visual Studio itself by running the vcvars64.bat
# that ships with the installation, instead of being reassembled from the registry by hand. That
# guarantees the same Windows SDK cmake already selected for zlib and libzip, so a single wave is
# never built out of two different sets of CRT headers.
# DEC-028 (owner answer 2-A): nothing is imported into this process. The roots are handed to cl as
# driver-owned /I arguments, exactly like /Brepro and /pathmap:, so they appear verbatim in the
# compile log and no later child process - cmake in particular - inherits a mutated environment.
# Only INCLUDE is read out of that environment. The rest of it is the operator's own shell
# environment, is never parsed, never logged and never echoed.
$script:MsvcIncludeRootsCache = $null
$script:MsvcIncludeDiscovery = $null

function Get-BatchIncludeRoots([string]$BatchPath, [string]$WorkDirectory) {
    if (-not (Test-Path -LiteralPath $BatchPath -PathType Leaf)) {
        Stop-BuildDeps "the Visual Studio environment script was not found: $BatchPath"
    }
    New-Item -ItemType Directory -Path $WorkDirectory -Force | Out-Null
    $marker = "FALCO-INCLUDE:"
    # A generated wrapper is used instead of a quoted `cmd /c` one-liner because the vcvars path
    # contains both spaces and parentheses, and cmd's quoting rules around them are a known source
    # of silent misparsing. The wrapper is left on disk in the run directory so that a failed
    # resolution can be read afterwards instead of reconstructed. It deliberately contains no
    # parenthesised block: inside one, the ")" in "Program Files (x86)" would end the block early.
    $wrapperPath = Join-Path $WorkDirectory "read-include.cmd"
    # The "not defined" guard is load-bearing, not defensive padding: cmd leaves an undefined
    # variable's reference in the text it echoes, so without it a batch file that cleared INCLUDE
    # would emit the literal "%INCLUDE%" and that would be parsed as a perfectly good include root
    # pointing nowhere. The failure would then surface as a C1034 from cl, which is exactly the
    # diagnosis-free failure this whole change exists to remove. Exit code 2 can only come from
    # this guard, because a non-zero code out of the batch file itself is converted to 1 on the
    # line above before this line is ever reached.
    $wrapperLines = @(
        "@echo off",
        "call `"$BatchPath`" >NUL",
        "if errorlevel 1 exit /b 1",
        "if not defined INCLUDE exit /b 2",
        "echo $marker%INCLUDE%",
        "exit /b 0"
    )
    [IO.File]::WriteAllText($wrapperPath, (($wrapperLines -join "`r`n") + "`r`n"),
        (New-Object Text.ASCIIEncoding))
    $comspec = Join-Path $env:SystemRoot "System32\cmd.exe"
    if (-not (Test-Path -LiteralPath $comspec -PathType Leaf)) {
        Stop-BuildDeps "cmd.exe was not found at '$comspec'"
    }
    $capture = Invoke-NativeCapture $comspec @("/d", "/c", $wrapperPath)
    if ($capture.exitCode -eq 2) {
        Stop-BuildDeps ("the Visual Studio environment script produced an empty include list: " +
            "$BatchPath; wrapper: $wrapperPath")
    }
    if ($capture.exitCode -ne 0) {
        Stop-BuildDeps ("the Visual Studio environment script failed with exit code " +
            "$($capture.exitCode); wrapper: $wrapperPath")
    }
    $markedLines = @($capture.output | Where-Object { [string]$_ -like "$marker*" })
    if ($markedLines.Count -ne 1) {
        Stop-BuildDeps ("expected exactly one include line from the Visual Studio environment " +
            "script, found $($markedLines.Count); wrapper: $wrapperPath")
    }
    $roots = @()
    foreach ($entry in @((([string]$markedLines[0]).Substring($marker.Length)) -split ";")) {
        # The trailing separator matters beyond tidiness: an argument that ends in a backslash is
        # re-quoted by the Windows command-line escaping rules and cl would receive a broken /I.
        $trimmed = ([string]$entry).Trim().TrimEnd("\")
        if ($trimmed) { $roots += $trimmed }
    }
    if ($roots.Count -le 0) {
        Stop-BuildDeps "the Visual Studio environment script produced an empty include list: $BatchPath"
    }
    return @($roots)
}

function Assert-IncludeRootsUsable([string[]]$Roots, [string[]]$Sentinels) {
    foreach ($sentinel in @($Sentinels)) {
        $resolved = ""
        foreach ($root in @($Roots)) {
            $candidate = Join-Path $root $sentinel
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $resolved = $candidate; break }
        }
        if (-not $resolved) {
            Stop-BuildDeps ("the compiler include search list does not resolve '$sentinel'; " +
                "roots searched ($(@($Roots).Count)): " + (@($Roots) -join " | "))
        }
    }
}

function Get-UcrtVersionsFromRoots([string[]]$Roots) {
    # F-W2-46. vcvars64.bat writes the Windows SDK half of INCLUDE with a lower-case 'include'
    # segment, while [regex]::Match is case-sensitive by default - unlike PowerShell's own -match
    # operator. The first live run therefore printed an empty SDK version beside a perfectly
    # working include list. Windows paths do not distinguish case; neither may this pattern.
    return @(@(@($Roots) | ForEach-Object {
        $match = [regex]::Match([string]$_, "(?i)\\include\\([^\\]+)\\ucrt$")
        if ($match.Success) { $match.Groups[1].Value }
    }) | Sort-Object -Unique)
}

function Get-MsvcIncludeRoots($Tools) {
    if ($null -ne $script:MsvcIncludeRootsCache) { return @($script:MsvcIncludeRootsCache) }
    $vcvars = Join-Path $Tools.installationPath "VC\Auxiliary\Build\vcvars64.bat"
    $roots = @(Get-BatchIncludeRoots $vcvars (Join-Path $script:RunRoot "msvc-env"))
    # stdlib.h lives in the Windows SDK's ucrt directory and limits.h in the MSVC toolset's own
    # include directory, so requiring both proves that the two independent halves of the search
    # list arrived. Failing here costs a second and names what is missing; failing inside cl costs
    # the whole wave and reports itself as a bare C1034 that names nothing.
    Assert-IncludeRootsUsable $roots @("stdlib.h", "limits.h")
    $sdkVersions = @(Get-UcrtVersionsFromRoots $roots)
    $script:MsvcIncludeDiscovery = [pscustomobject]@{
        vcvars = $vcvars
        rootCount = $roots.Count
        roots = @($roots)
        sdkVersions = @($sdkVersions)
    }
    Add-ReportEvent "build" "" "PASS" "Compiler include search list resolved from the VS environment" @{
        vcvars = $vcvars
        rootCount = $roots.Count
        roots = @($roots)
        sdkVersions = @($sdkVersions)
    }
    $script:MsvcIncludeRootsCache = @($roots)
    return @($roots)
}

function Invoke-ClCompile([string]$Cl, [string]$ComponentId, [string[]]$Flags, [string]$SourceFile,
    [string]$ObjectFile, [string]$SourceRoot, [string]$IntermediateRoot, [string[]]$IncludeRoots) {
    New-Item -ItemType Directory -Path $script:RunRoot, (Split-Path -Parent $ObjectFile) -Force | Out-Null
    if (Test-Path -LiteralPath $ObjectFile) { Remove-Item -LiteralPath $ObjectFile -Force }
    # /Brepro drops the timestamp; /pathmap normalises the two absolute roots MSVC embeds in the
    # object even with /Brepro. This is the same pair the W1 project sheet applies, and it is
    # supplied by the driver rather than by the recipe so that the recipe stays machine-neutral.
    $pathMaps = @("/pathmap:$($SourceRoot.TrimEnd('\'))=Z:\src", "/pathmap:$($IntermediateRoot.TrimEnd('\'))=Z:\obj")
    # DEC-026 (owner answer 1-A): MSVC 14.44 refuses to honour /pathmap: unless
    # /experimental:deterministic is also on the command line - it drops the option and says so with
    # warning D9007 (CS07C). Without /pathmap: the object still carries the two absolute roots and
    # the pinned SHA-256 stops reproducing on another machine, so the flag is added rather than
    # /pathmap: being abandoned. It is driver-owned, exactly like /Brepro and /pathmap:.
    # F-W2-44 / DEC-028 (owner answer 2-A). The include roots are driver-owned, exactly like
    # /Brepro and /pathmap:, and are placed with them rather than in the recipe: they are absolute
    # paths of this machine and deps.lock.json has to stay machine-neutral.
    $includeArguments = @()
    foreach ($root in @($IncludeRoots)) {
        if ($root) { $includeArguments += "/I$root" }
    }
    $arguments = @("/nologo", "/c") + @($Flags) +
        @("/Brepro", "/experimental:deterministic") + $pathMaps + $includeArguments +
        @("/Fo$ObjectFile", $SourceFile)
    $unit = [IO.Path]::GetFileNameWithoutExtension($SourceFile)
    $logPath = Join-Path $script:RunRoot "$ComponentId-$unit-cl.log"
    $capture = Invoke-NativeCapture $Cl $arguments
    $output = $capture.output
    $exitCode = $capture.exitCode
    [IO.File]::WriteAllText($logPath, (($output | Out-String)), (New-Object Text.UTF8Encoding($false)))
    # F-W2-18: gate on the exit code and on the artefact, never on error text.
    if ($exitCode -ne 0) {
        $output | Select-Object -Last 80 | ForEach-Object { Write-Output $_ }
        Stop-BuildDeps ("cl failed for components.$ComponentId source '$SourceFile' with exit code " +
            "$exitCode; log: $logPath")
    }
    if (-not (Test-Path -LiteralPath $ObjectFile -PathType Leaf)) {
        Stop-BuildDeps "cl reported success for components.$ComponentId but produced no object: $ObjectFile"
    }
    # D9007 is a command-line WARNING, not an error: cl drops the rejected option and compiles
    # anyway with exit code 0. A silent drop of /pathmap: would leave the absolute build roots in
    # the object and the pinned SHA-256 would not reproduce elsewhere - a defect that would only
    # surface much later, as an unexplained pin mismatch. So it is surfaced here, in the same shape
    # Invoke-ClArchive uses for LNK4044.
    $pathMapApplied = (@($output | Where-Object { [string]$_ -match "D9007" }).Count -eq 0)
    if (-not $pathMapApplied) {
        Add-ReportEvent "build" $ComponentId "WARN" "cl rejected a driver-owned command-line option (D9007)" @{
            source = $SourceFile
            log = $logPath
        }
        Write-Output ("WARN: cl rejected a driver-owned option for components.$ComponentId (D9007). The object " +
            "may still carry absolute build paths and its SHA-256 may not reproduce on another machine; " +
            "read $logPath before pinning it.")
    }
    $warningKeys = foreach ($line in $output) {
        $match = [regex]::Match([string]$line,
            '(?<file>[^\\/:]+\.c)\((?<line>\d+)(?:,\d+)?\):\s+warning\s+(?<code>C\d+):')
        if ($match.Success) {
            "$($match.Groups['file'].Value):$($match.Groups['line'].Value):$($match.Groups['code'].Value)"
        }
    }
    $unique = @($warningKeys | Sort-Object -Unique)
    $codes = @($unique | ForEach-Object { @($_ -split ":")[2] } | Sort-Object -Unique)
    # DEC-025 (answer P-3): no /wd suppression is added blind. The codes are reported so that
    # each one can be judged on the evidence of a real build and, if it is later suppressed,
    # justified individually in the warning baseline.
    $codeSummary = ""
    if ($codes.Count -gt 0) { $codeSummary = "; codes: $($codes -join ', ')" }
    Add-ReportEvent "build" $ComponentId "PASS" "cl compiled a translation unit" @{
        source = $SourceFile
        object = $ObjectFile
        log = $logPath
        uniqueWarnings = $unique.Count
        warningCodes = $codes
        pathMapApplied = $pathMapApplied
    }
    Write-Output ("PASS: $ComponentId/$unit compiled with cl (0 errors; $($unique.Count) unique " +
        "warnings$codeSummary; full log: $logPath)")
}

function Invoke-ClArchive([string]$Lib, [string]$ComponentId, [string[]]$ObjectFiles, [string]$OutputFile) {
    New-Item -ItemType Directory -Path $script:RunRoot, (Split-Path -Parent $OutputFile) -Force | Out-Null
    if (Test-Path -LiteralPath $OutputFile) { Remove-Item -LiteralPath $OutputFile -Force }
    $logPath = Join-Path $script:RunRoot "$ComponentId-lib.log"
    # Same hazard as in Invoke-ClCompile: lib.exe writes LNK4044 to stderr, and under
    # $ErrorActionPreference = "Stop" that alone would abort the run before the exit-code gate.
    # lib.exe stores the arguments as member names; compiler path maps cannot
    # change those. Resolve them relative to a stable object directory.
    Push-Location -LiteralPath (Split-Path -Parent $ObjectFiles[0])
    try {
        $relativeObjects = @($ObjectFiles | ForEach-Object { Resolve-Path -LiteralPath $_ -Relative })
        $arguments = @("/nologo", "/MACHINE:X64", "/BREPRO", "/OUT:$OutputFile") + $relativeObjects
        $capture = Invoke-NativeCapture $Lib $arguments
    }
    finally { Pop-Location }
    $output = $capture.output
    $exitCode = $capture.exitCode
    [IO.File]::WriteAllText($logPath, (($output | Out-String)), (New-Object Text.UTF8Encoding($false)))
    if ($exitCode -ne 0) {
        $output | Select-Object -Last 80 | ForEach-Object { Write-Output $_ }
        Stop-BuildDeps "lib failed for components.$ComponentId with exit code $exitCode; log: $logPath"
    }
    if (-not (Test-Path -LiteralPath $OutputFile -PathType Leaf)) {
        Stop-BuildDeps "lib reported success for components.$ComponentId but produced no archive: $OutputFile"
    }
    # /BREPRO is documented for link.exe, not for lib.exe. An option lib.exe does not recognise is
    # reported as LNK4044 - a WARNING, not an error - so a silent rejection would leave member
    # timestamps in the archive and the pinned SHA-256 would not reproduce between two identical
    # runs. That would surface much later as an unreproducible pin, so it is surfaced here instead.
    $reproducible = (@($output | Where-Object { [string]$_ -match "LNK4044" }).Count -eq 0)
    if (-not $reproducible) {
        Add-ReportEvent "build" $ComponentId "WARN" "lib.exe did not accept /BREPRO (LNK4044)" @{ log = $logPath }
        Write-Output ("WARN: lib.exe did not accept /BREPRO for components.$ComponentId (LNK4044). The archive " +
            "may carry member timestamps and its SHA-256 may not reproduce; read $logPath before pinning it.")
    }
    $identity = Get-FileIdentity $OutputFile
    Add-ReportEvent "build" $ComponentId "PASS" "lib archived the compiled objects" @{
        objects = @($ObjectFiles)
        output = $OutputFile
        log = $logPath
        reproducible = $reproducible
        bytes = $identity.bytes
        sha256 = $identity.sha256
    }
    Write-Output "PASS: $ComponentId archived with lib ($($identity.bytes) bytes; full log: $logPath)"
}

function Invoke-ClBuild($Tools, [string]$ComponentId, $Component, [string]$StagingRoot,
    [string]$IntermediateRoot, [string[]]$IncludeRoots) {
    if (-not $StagingRoot) { Stop-BuildDeps "the cl driver requires the W2 staging root" }
    if (-not $IntermediateRoot) { Stop-BuildDeps "the cl driver requires a stable intermediate root" }
    $plan = Get-ClSourcePlan $ComponentId
    Assert-RelativeSafePath $plan.sourceInTree "components.$ComponentId in-tree source root"
    $sourceRoot = Assert-PathUnder (Join-PathLiteral $script:RepoRoot $plan.sourceInTree) $script:RepoRoot `
        "components.$ComponentId in-tree source root"
    if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
        Stop-BuildDeps "components.$ComponentId in-tree source root is missing: $sourceRoot"
    }
    # These sources are part of the engine repository. They are never extracted, never patched
    # and never written to; licenseFiles pins them by SHA-256, so this call is simultaneously
    # the license check and the tamper check on the build input.
    Assert-LicenseFiles $Component $sourceRoot $ComponentId
    $flags = Expand-RecipeFlags $Component.buildProfile.flags $ComponentId $StagingRoot
    Add-ReportEvent "build" $ComponentId "PASS" "Recipe flags expanded for cl" @{
        declared = @($Component.buildProfile.flags)
        effective = @($flags)
    }
    $objectRoot = Assert-PathUnder (Join-PathLiteral $IntermediateRoot $ComponentId) $script:OutRoot `
        "components.$ComponentId object directory"
    $objects = @()
    foreach ($source in @($plan.sources)) {
        Assert-RelativeSafePath $source "components.$ComponentId source"
        $sourceFile = Assert-PathUnder (Join-PathLiteral $sourceRoot $source) $sourceRoot `
            "components.$ComponentId source"
        if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
            Stop-BuildDeps "components.$ComponentId source is missing: $sourceFile"
        }
        $objectFile = Join-PathLiteral $objectRoot ([IO.Path]::GetFileNameWithoutExtension($source) + ".obj")
        Invoke-ClCompile $Tools.cl $ComponentId $flags $sourceFile $objectFile $sourceRoot `
            $IntermediateRoot $IncludeRoots
        $objects += $objectFile
    }
    if ($objects.Count -le 0) {
        Stop-BuildDeps "components.$ComponentId declares no source to compile"
    }
    $outputs = @($Component.outputs)
    if ($outputs.Count -ne 1) {
        Stop-BuildDeps ("components.$ComponentId must declare exactly one archive output for the cl driver, " +
            "but declares $($outputs.Count)")
    }
    $archive = Get-StagedOutputPath ([string]$outputs[0].path) $StagingRoot $ComponentId
    Invoke-ClArchive $Tools.lib $ComponentId $objects $archive
    $headerPlan = @()
    foreach ($header in @($plan.headers)) {
        Assert-RelativeSafePath $header "components.$ComponentId header"
        $headerPlan += [pscustomobject]@{
            role = "header"; origin = "source"
            from = (Join-PathLiteral $sourceRoot $header)
            to = (Join-PathLiteral $StagingRoot ("include\" + (Split-Path -Leaf $header)))
        }
    }
    if ($headerPlan.Count -gt 0) {
        $null = Copy-StagedArtifacts $headerPlan $ComponentId $StagingRoot
    }
    $license = Export-EmbeddedLicenseBlock (Join-PathLiteral $sourceRoot $plan.licenseSource) `
        $plan.licenseMarker (Join-PathLiteral $StagingRoot $plan.licenseStaged) $ComponentId
    Write-Output ("PASS: $ComponentId compiled with cl and archived with lib; license extracted from " +
        "$($plan.licenseSource) lines $($license.firstLine)-$($license.lastLine) " +
        "($($license.lines) lines, $($license.bytes) bytes).")
}

function Invoke-W2Build($Lock, [string[]]$Names) {
    $expected = @("zlib", "libzip", "bc7enc")
    foreach ($name in $expected) {
        if ($name -notin $Names) { Stop-BuildDeps "W2 recipe requires the component '$name' to be selected" }
    }
    foreach ($name in $Names) {
        if ($name -notin $expected) { Stop-BuildDeps "W2 recipe does not accept the component '$name'" }
    }
    if ([array]::IndexOf($Names, "zlib") -ge [array]::IndexOf($Names, "libzip")) {
        Stop-BuildDeps "W2 recipe requires the topological component order zlib before libzip"
    }

    # Pre-flight over the whole wave before any work starts: a wave that cannot be finished
    # must not be started, otherwise the prefix and the state record would describe half a wave.
    $cmakeNames = @()
    $clNames = @()
    foreach ($name in $Names) {
        $system = [string]$Lock.components.$name.buildProfile.system
        if ($system -eq "cmake") { $cmakeNames += $name; continue }
        if ($system -eq "cl") { $clNames += $name; continue }
        Stop-BuildDeps ("build recipes are not implemented yet for buildProfile.system '$system' " +
            "(components.$name); W2 cannot be completed until that driver is added")
    }
    foreach ($name in $Names) {
        $component = $Lock.components.$name
        if ([string]$component.wave -ne "W2") { Stop-BuildDeps "components.$name is not a W2 component" }
        if ($name -in $cmakeNames) { Assert-CMakeRecipe $component $name }
        else { Assert-ClRecipe $component $name }
    }

    $vsVersionRange = [string]$Lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $tools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    Normalize-ProcessPathVariable
    if ($null -eq $script:ToolchainIdentity) {
        Stop-BuildDeps "toolchain identity must be resolved before a wave build"
    }
    $toolchainIdentity = $script:ToolchainIdentity
    if (@($toolchainIdentity.PSObject.Properties.Name) -notcontains "cmake") {
        Stop-BuildDeps "W2 requires cmake.exe on PATH; the resolved toolchain identity carries no cmake record"
    }
    $cmake = [string]$toolchainIdentity.cmake.path
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
        Stop-BuildDeps "the cmake recorded in the toolchain identity is not present: '$cmake'"
    }

    $recipeFingerprint = Get-W2RecipeFingerprint $Lock
    $toolchainFingerprint = Get-ToolchainFingerprint $toolchainIdentity
    $activeRoot = Join-Path $script:OutRoot "w2"
    $disposition = Get-WaveStateDisposition "W2" $recipeFingerprint $toolchainFingerprint $toolchainIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Add-ReportEvent "toolchain" "W2" "DRIFT" "Recipe unchanged but toolchain identity changed" @{
            recipeFingerprint = $recipeFingerprint
            recordedToolchainFingerprint = [string]$disposition.state.toolchainFingerprint
            currentToolchainFingerprint = $toolchainFingerprint
            differences = @($disposition.differences)
        }
        Stop-BuildDeps ("TOOLCHAIN DRIFT: recipe unchanged but toolchain identity changed ($differenceTable). " +
            "Pinned outputs cannot be reproduced. Run the controlled re-pin procedure " +
            "(see _ai/MSVC_OUTPUT_PIN_AND_REPIN_POLICY.md section 5) or restore the pinned toolchain.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        foreach ($name in $Names) {
            foreach ($output in @($Lock.components.$name.outputs)) {
                Assert-StaticCrt $tools.dumpbin (Join-Path $script:RepoRoot $output.path) $name
            }
        }
        Add-ReportEvent "toolchain" "W2" "MATCH" "Toolchain fingerprint matches the recorded wave state" @{
            toolchainFingerprint = $toolchainFingerprint
        }
        Add-ReportEvent "state" "W2" "CACHED" "Recipe fingerprint and output identities match; build skipped"
        Write-Output "PASS: W2 state and outputs verified; build skipped. Use -Rebuild to force compilation."
        return
    }
    Add-ReportEvent "state" "W2" "STALE" "W2 will be rebuilt: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w2-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W2 staging root"
    New-Item -ItemType Directory -Path (Join-Path $stagingRoot "lib"), (Join-Path $stagingRoot "include"),
        (Join-Path $stagingRoot "licenses") -Force | Out-Null

    # The staged include directory is the source of truth for the next component in the wave:
    # libzip is configured against the W2 prefix, and FindZLIB inspects that directory as a
    # whole, so zlib.h and zconf.h must both be in place before libzip configures. dependsOn
    # guarantees the order; this driver guarantees the contents, and refuses to configure a
    # component whose declared dependencies have not been staged into this run's prefix yet.
    $stagedIncludeRoot = Join-Path $stagingRoot "include"
    foreach ($name in $cmakeNames) {
        $component = $Lock.components.$name
        foreach ($dependency in @($component.dependsOn)) {
            foreach ($output in @($Lock.components.$dependency.outputs)) {
                $stagedDependency = Get-StagedOutputPath ([string]$output.path) $stagingRoot $dependency
                if (-not (Test-Path -LiteralPath $stagedDependency -PathType Leaf)) {
                    Stop-BuildDeps ("components.$name declares dependsOn components.$dependency, whose output " +
                        "is not staged in this run's W2 prefix yet: $stagedDependency")
                }
            }
        }
        $sourceRoot = Expand-VerifiedArchive (Get-CachePath $name $component) `
            (Join-Path $script:WorkRoot $name) $component $name
        $buildRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w2\$name") $script:OutRoot `
            "components.$name build directory"
        if (Test-Path -LiteralPath $buildRoot) { Remove-Item -LiteralPath $buildRoot -Recurse -Force }
        Invoke-CMakeBuild $cmake $name $component $sourceRoot $buildRoot $tools.vcToolsVersion $stagingRoot
        $plan = Get-CMakeStagePlan $name $sourceRoot $buildRoot $stagingRoot
        $null = Copy-StagedArtifacts $plan $name $stagingRoot
        if ($name -eq "zlib") {
            # FindZLIB reads the directory as a whole. zconf.h is generated into the build tree
            # and is a different file from the same-named one in the source tree (F-W2-22).
            foreach ($header in @("zlib.h", "zconf.h")) {
                if (-not (Test-Path -LiteralPath (Join-Path $stagedIncludeRoot $header) -PathType Leaf)) {
                    Stop-BuildDeps ("$header was not staged into the W2 include prefix; libzip cannot be " +
                        "configured against this run's zlib")
                }
            }
        }
    }

    # F-W2-35 / DEC-025. bc7enc has no archive to fetch, no CMakeLists to configure and nothing
    # to patch: it is compiled straight out of the engine tree by cl and archived by lib. The
    # intermediate root is kept stable across runs for the reason W1 keeps its own stable -
    # MSVC embeds the object path in the object even with /Brepro - and the component name is
    # appended to it, which is safe because the selection gate above admits no component
    # named 'obj' and every cmake build root is a sibling of this directory.
    $intermediateRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w2\obj") $script:OutRoot `
        "W2 intermediate root"
    # F-W2-44 / DEC-028. Resolved once per run and before the first cl invocation, so that a
    # missing or broken Visual Studio environment stops the wave with a message that names the
    # cause instead of a bare C1034 in the middle of a compile. Nothing is resolved at all when the
    # wave has no cl component, because running vcvars would then be work with no consumer.
    $includeRoots = @()
    if (@($clNames).Count -gt 0) {
        $includeRoots = @(Get-MsvcIncludeRoots $tools)
        Write-Output ("PASS: cl include list resolved from $($script:MsvcIncludeDiscovery.vcvars) " +
            "($($script:MsvcIncludeDiscovery.rootCount) roots; Windows SDK ucrt: " +
            "$(@($script:MsvcIncludeDiscovery.sdkVersions) -join ', '))")
    }
    foreach ($name in $clNames) {
        $component = $Lock.components.$name
        foreach ($dependency in @($component.dependsOn)) {
            foreach ($output in @($Lock.components.$dependency.outputs)) {
                $stagedDependency = Get-StagedOutputPath ([string]$output.path) $stagingRoot $dependency
                if (-not (Test-Path -LiteralPath $stagedDependency -PathType Leaf)) {
                    Stop-BuildDeps ("components.$name declares dependsOn components.$dependency, whose output " +
                        "is not staged in this run's W2 prefix yet: $stagedDependency")
                }
            }
        }
        Invoke-ClBuild $tools $name $component $stagingRoot $intermediateRoot $includeRoots
    }

    foreach ($name in $Names) {
        foreach ($output in @($Lock.components.$name.outputs)) {
            $stagedLibrary = Get-StagedOutputPath ([string]$output.path) $stagingRoot $name
            if (-not (Test-Path -LiteralPath $stagedLibrary -PathType Leaf)) {
                Stop-BuildDeps "W2 expected output is missing before activation: $stagedLibrary"
            }
            Assert-StaticCrt $tools.dumpbin $stagedLibrary $name
        }
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($name in $Names) {
        foreach ($output in @($Lock.components.$name.outputs)) {
            $path = Join-Path $script:RepoRoot ([string]$output.path)
            $identity = Get-FileIdentity $path
            $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
        }
    }
    Write-WaveState "W2" $recipeFingerprint $toolchainFingerprint $toolchainIdentity $stateOutputs
    Add-ReportEvent "prefix" "W2" "PASS" "W2 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output ("PASS: W2 built with MSVC v143 x64 Release /MT through cmake (zlib, libzip) and cl+lib " +
        "(bc7enc) and activated at '$activeRoot'.")
    $pinContext = "PINCTX: cl=$($toolchainIdentity.cl.version) link=$($toolchainIdentity.link.version)" +
        " lib=$($toolchainIdentity.lib.version) sdk=$($toolchainIdentity.windowsSdk.version)" +
        " cmake=$($toolchainIdentity.cmake.version) fp=$toolchainFingerprint"
    Write-Output $pinContext
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Complete-MappedResourceArchive($Tools, [string]$BuildRoot, [ValidateSet('freetype', 'assimp')][string]$ComponentId) {
    # lib.exe's implicit resource conversion embeds absolute cwd/input paths.
    # Preconvert with the same MSVC cvtres, trimming the build root, then archive
    # the original ordered member list with that resource object substituted.
    if ($ComponentId -eq 'freetype') {
        $memberRoot = $BuildRoot
        $library = Join-Path $BuildRoot 'Release\freetype.lib'
        $resource = 'freetype.dir\Release\ftver.res'
        $converted = 'freetype.dir\Release\ftver.resource.obj'
        $objectPattern = 'freetype.dir\Release\*.obj'
        $expectedCount = 43
    }
    else {
        $memberRoot = Join-Path $BuildRoot 'code'
        $library = Join-Path $BuildRoot 'lib\Release\assimp-vc143-mt.lib'
        $resource = 'assimp.dir\Release\assimp.res'
        $converted = 'assimp.dir\Release\assimp.resource.obj'
        $objectPattern = 'assimp.dir\Release\*.obj'
        $expectedCount = 233
    }
    $cvtres = Join-Path (Split-Path $Tools.lib -Parent) 'cvtres.exe'
    if (-not (Test-Path -LiteralPath $cvtres -PathType Leaf)) { Stop-BuildDeps 'Pinned MSVC cvtres is missing' }
    $listing = Invoke-NativeCapture $Tools.lib @('/nologo', '/LIST', $library)
    if ($listing.exitCode -ne 0) { Stop-BuildDeps "Cannot list the $ComponentId archive" }
    $members = @($listing.output | ForEach-Object { ([string]$_).Trim() } | Where-Object { $_ })
    if ($members.Count -ne $expectedCount -or @($members | Where-Object { $_ -eq $resource }).Count -ne 1 -or
        @($members | Sort-Object -Unique).Count -ne $expectedCount) { Stop-BuildDeps "Unexpected $ComponentId member inventory" }
    foreach ($member in $members) {
        Assert-RelativeSafePath $member "$ComponentId archive input"
        if ($member -ne $resource -and $member -notlike $objectPattern) { Stop-BuildDeps "Unexpected $ComponentId member type" }
        $inputPath = Assert-PathUnder (Join-Path $memberRoot $member) $memberRoot "$ComponentId archive input"
        if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) { Stop-BuildDeps "Missing $ComponentId archive input" }
    }
    Push-Location -LiteralPath $memberRoot
    try {
        $conversion = Invoke-NativeCapture $cvtres @('/nologo', '/machine:x64', '/Brepro', "/trimfile:$memberRoot", "/out:$converted", $resource)
        $logPath = Join-Path $script:RunRoot "$ComponentId-resource.log"
        [IO.File]::WriteAllText($logPath, ($conversion.output | Out-String))
        if ($conversion.exitCode -ne 0 -or -not (Test-Path -LiteralPath $converted -PathType Leaf)) { Stop-BuildDeps "$ComponentId resource conversion failed" }
        $resourceBytes = [IO.File]::ReadAllBytes((Join-Path $memberRoot $converted))
        if ([Text.Encoding]::UTF8.GetString($resourceBytes).IndexOf($BuildRoot, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            Stop-BuildDeps "$ComponentId resource still contains the absolute build root"
        }
        $objects = @($members | ForEach-Object { if ($_ -eq $resource) { $converted } else { $_ } })
        $archive = Invoke-NativeCapture $Tools.lib (@('/nologo', '/MACHINE:X64', '/Brepro', "/OUT:$library") + $objects)
        [IO.File]::AppendAllText($logPath, ($archive.output | Out-String))
        if ($archive.exitCode -ne 0 -or @($archive.output | Where-Object { [string]$_ -match 'LNK4044' }).Count) { Stop-BuildDeps "$ComponentId resource archive failed" }
    }
    finally { Pop-Location }
    Add-ReportEvent 'build' $ComponentId 'PASS' 'Resource preconverted without absolute build paths; original archive inventory preserved' @{
        log = $logPath; members = $members.Count; cvtres = (Get-FileIdentity $cvtres)
    }
}

function Write-MsBuildDeterminismProps([string]$SourceRoot, [string]$ComponentId) {
    # F-W2-64 / DEC-071. The msbuild path had no /Brepro: the W3 double build reproduced
    # freetype (cmake) byte for byte while FreeImageLib.lib differed hash-only at equal size -
    # the lib.exe member timestamps, the compiler half of F-W2-62 all over again. The flag
    # travels through a driver-generated Directory.Build.props at the extracted source root:
    # msbuild imports it automatically for every project below it, the injection is visible in
    # the diagnostic build log, and not one upstream file is edited - the Write-B2UserConfig
    # precedent (DEC-049). The gate refuses a tree that already carries such a file: silently
    # shadowing an upstream build configuration would change more than the flag.
    $path = Join-PathLiteral $SourceRoot "Directory.Build.props"
    if (Test-Path -LiteralPath $path) {
        Stop-BuildDeps ("components.$ComponentId source tree already carries Directory.Build.props; " +
            "refusing to overwrite an upstream build configuration")
    }
    $compilerOptions = '/Brepro %(AdditionalOptions)'
    if ($ComponentId -eq 'freeimage') {
        # All referenced codec projects build under this source root; their
        # archive member names are already relative to each project directory.
        $escapedRoot = [System.Security.SecurityElement]::Escape($SourceRoot)
        $compilerOptions = "/Brepro /experimental:deterministic /pathmap:&quot;$escapedRoot=W3FREEIMAGE&quot; %(AdditionalOptions)"
    }
    $content = @(
        '<Project>',
        '  <ItemDefinitionGroup>',
        '    <ClCompile>',
        "      <AdditionalOptions>$compilerOptions</AdditionalOptions>",
        '    </ClCompile>',
        '    <Lib>',
        '      <AdditionalOptions>/Brepro %(AdditionalOptions)</AdditionalOptions>',
        '    </Lib>',
        '  </ItemDefinitionGroup>',
        '</Project>'
    ) -join "`n"
    [IO.File]::WriteAllText($path, $content + "`n", (New-Object Text.UTF8Encoding($false)))
    Add-ReportEvent "build" $ComponentId "PASS" "Determinism props written (F-W2-64)" @{
        path = $path
        flag = "/Brepro"
        groups = @("ClCompile", "Lib")
    }
    return $path
}

function Invoke-FreeImageMsBuild($Tools, [string]$ComponentId, [string]$SourceRoot,
    [string]$WindowsSdkVersion) {
    if (-not $WindowsSdkVersion) {
        Stop-BuildDeps "the FreeImage msbuild invocation requires the resolved Windows SDK version"
    }
    # The upstream 2017 project pins toolset v141 and SDK 10.0.16299.0, neither of which is
    # installed. Both are overridden on the command line - the same DEC-028 reasoning as the
    # /I flags: a property on the command line is visible in the build log and verifiable after
    # the fact, and not one upstream file is edited to apply it. Building the FreeImageLib
    # project also builds its nine ProjectReference libraries.
    $project = Join-PathLiteral $SourceRoot "Source\FreeImageLib\FreeImageLib.2017.vcxproj"
    if (-not (Test-Path -LiteralPath $project -PathType Leaf)) {
        Stop-BuildDeps "components.$ComponentId project was not found after extraction: $project"
    }
    $logPath = Join-Path $script:RunRoot "$ComponentId-msbuild.log"
    $arguments = @(
        $project,
        "/m",
        "/t:Rebuild",
        "/p:Configuration=Release",
        "/p:Platform=x64",
        "/p:PlatformToolset=v143",
        "/p:VCToolsVersion=$($Tools.vcToolsVersion)",
        "/p:WindowsTargetPlatformVersion=$WindowsSdkVersion",
        "/fl",
        "/flp:logfile=$logPath;verbosity=diagnostic",
        "/nologo"
    )
    # F-W2-51: same routing as the cmake step - stderr must arrive as data, the gate is the
    # exit code and the log.
    $capture = Invoke-NativeCapture $Tools.msbuild $arguments
    $output = $capture.output
    $exitCode = $capture.exitCode
    if ($exitCode -ne 0) {
        $output | Select-Object -Last 80 | ForEach-Object { Write-Output $_ }
        Stop-BuildDeps "MSBuild failed for components.$ComponentId with exit code $exitCode; log: $logPath"
    }
    $warningKeys = foreach ($line in $output) {
        $match = [regex]::Match([string]$line,
            '(?<file>[^\\/:]+\.(?:c|cc|cpp|cxx))\((?<line>\d+),\d+\):\s+warning\s+(?<code>C\d+):')
        if ($match.Success) {
            "$($match.Groups['file'].Value):$($match.Groups['line'].Value):$($match.Groups['code'].Value)"
        }
    }
    $warningCount = @($warningKeys | Sort-Object -Unique).Count
    Add-ReportEvent "build" $ComponentId "PASS" "MSBuild completed with command-line toolset and SDK overrides" @{
        log = $logPath
        uniqueWarnings = $warningCount
        platformToolset = "v143"
        windowsSdk = $WindowsSdkVersion
    }
    Write-Output ("PASS: $ComponentId compiled with msbuild (Release x64, toolset v143 " +
        "$($Tools.vcToolsVersion), SDK $WindowsSdkVersion; $warningCount unique warnings; full log: $logPath)")
}

function Get-W3RecipeFingerprint($Lock, $Tools) {
    # Same construction as Get-W2RecipeFingerprint: the driver text plus every patch the wave's
    # components declare. The explicit resource converter also participates in
    # cache identity so replacing cvtres cannot reuse the old W3 result.
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $patchIdentities = @()
    foreach ($name in @("freetype", "freeimage")) {
        foreach ($patch in @($Lock.components.$name.patches)) {
            $patchPath = Join-Path $script:RepoRoot ([string]$patch.file)
            $patchIdentity = Get-FileIdentity $patchPath
            $patchIdentities += [ordered]@{
                component = $name
                file = [string]$patch.file
                bytes = $patchIdentity.bytes
                sha256 = $patchIdentity.sha256
            }
        }
    }
    $material = [ordered]@{
        resourceConverter = Get-FileIdentity (Join-Path (Split-Path $Tools.lib -Parent) "cvtres.exe")
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        patches = $patchIdentities
    }
    $json = $material | ConvertTo-Json -Depth 6 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes([string]$json)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    return ([BitConverter]::ToString($hash)).Replace("-", "")
}

function Invoke-W3Build($Lock, [string[]]$Names) {
    $expected = @("freetype", "freeimage")
    foreach ($name in $expected) {
        if ($name -notin $Names) { Stop-BuildDeps "W3 recipe requires the component '$name' to be selected" }
    }
    foreach ($name in $Names) {
        if ($name -notin $expected) { Stop-BuildDeps "W3 recipe does not accept the component '$name'" }
    }

    $cmakeNames = @("freetype")
    $msbuildNames = @("freeimage")
    foreach ($name in $Names) {
        $component = $Lock.components.$name
        if ([string]$component.wave -ne "W3") { Stop-BuildDeps "components.$name is not a W3 component" }
        if ($name -in $cmakeNames) { Assert-CMakeRecipe $component $name }
        else { Assert-MsBuildRecipe $component $name }
    }

    $vsVersionRange = [string]$Lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $tools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    Normalize-ProcessPathVariable
    if ($null -eq $script:ToolchainIdentity) {
        Stop-BuildDeps "toolchain identity must be resolved before a wave build"
    }
    $toolchainIdentity = $script:ToolchainIdentity
    if (@($toolchainIdentity.PSObject.Properties.Name) -notcontains "cmake") {
        Stop-BuildDeps "W3 requires cmake.exe on PATH; the resolved toolchain identity carries no cmake record"
    }
    $cmake = [string]$toolchainIdentity.cmake.path
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
        Stop-BuildDeps "the cmake recorded in the toolchain identity is not present: '$cmake'"
    }
    $windowsSdkVersion = [string]$toolchainIdentity.windowsSdk.version
    if (-not $windowsSdkVersion) {
        Stop-BuildDeps "the toolchain identity carries no Windows SDK version; W3 cannot override the FreeImage project SDK"
    }

    $recipeFingerprint = Get-W3RecipeFingerprint $Lock $tools
    $toolchainFingerprint = Get-ToolchainFingerprint $toolchainIdentity
    $activeRoot = Join-Path $script:OutRoot "w3"
    $wavePrefix = "out/deps/w3/"
    $disposition = Get-WaveStateDisposition "W3" $recipeFingerprint $toolchainFingerprint $toolchainIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Add-ReportEvent "toolchain" "W3" "DRIFT" "Recipe unchanged but toolchain identity changed" @{
            recipeFingerprint = $recipeFingerprint
            recordedToolchainFingerprint = [string]$disposition.state.toolchainFingerprint
            currentToolchainFingerprint = $toolchainFingerprint
            differences = @($disposition.differences)
        }
        Stop-BuildDeps ("TOOLCHAIN DRIFT: recipe unchanged but toolchain identity changed ($differenceTable). " +
            "Pinned outputs cannot be reproduced. Run the controlled re-pin procedure " +
            "(see _ai/MSVC_OUTPUT_PIN_AND_REPIN_POLICY.md section 5) or restore the pinned toolchain.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        foreach ($name in $Names) {
            foreach ($output in @($Lock.components.$name.outputs)) {
                Assert-StaticCrt $tools.dumpbin (Join-Path $script:RepoRoot $output.path) $name
            }
        }
        Add-ReportEvent "toolchain" "W3" "MATCH" "Toolchain fingerprint matches the recorded wave state" @{
            toolchainFingerprint = $toolchainFingerprint
        }
        Add-ReportEvent "state" "W3" "CACHED" "Recipe fingerprint and output identities match; build skipped"
        Write-Output "PASS: W3 state and outputs verified; build skipped. Use -Rebuild to force compilation."
        return
    }
    Add-ReportEvent "state" "W3" "STALE" "W3 will be rebuilt: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w3-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W3 staging root"
    New-Item -ItemType Directory -Path (Join-Path $stagingRoot "lib"), (Join-Path $stagingRoot "include"),
        (Join-Path $stagingRoot "licenses") -Force | Out-Null

    foreach ($name in $cmakeNames) {
        $component = $Lock.components.$name
        $sourceRoot = Expand-VerifiedArchive (Get-CachePath $name $component) `
            (Join-Path $script:WorkRoot $name) $component $name
        $buildRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w3\$name") $script:OutRoot `
            "components.$name build directory"
        if (Test-Path -LiteralPath $buildRoot) { Remove-Item -LiteralPath $buildRoot -Recurse -Force }
        Invoke-CMakeBuild $cmake $name $component $sourceRoot $buildRoot $tools.vcToolsVersion $stagingRoot
        Complete-MappedResourceArchive $tools $buildRoot 'freetype'
        $plan = Get-CMakeStagePlan $name $sourceRoot $buildRoot $stagingRoot
        $null = Copy-StagedArtifacts $plan $name $stagingRoot
        if ($name -eq "freetype") {
            # DEC-070 (phase A). The engine includes FreeType as ../FreeType/include, and the
            # legacy tree has the raw upstream include\ shape (ft2build.h + freetype\** with
            # internal\ - measured over the bridge), so the upstream include\ is staged as-is.
            # freetype runs before freeimage, so the shared include\ is empty and the
            # staged-count gate of Copy-StagedHeaderTree holds; freeimage's single FreeImage.h
            # lands afterwards through its existing stage plan.
            $null = Copy-StagedHeaderTree (Join-Path $sourceRoot "include") `
                (Join-Path $stagingRoot "include") $name
            # DEC-071. cmake generates ftconfig.h/ftoption.h into the build tree and upstream's
            # own install() ships exactly those, excluding the source copies (CMakeLists lines
            # 545-553). On this machine's cmake the generated copies are CRLF (measured:
            # content-equivalent, +1 byte per line - 51 and 1029; the legacy tree carried the
            # same 1665-byte CRLF ftconfig.h), so the DEC-070 assumption "byte-copies, no
            # override needed" was wrong about the bytes even though right about the content.
            # The two files the library was actually configured with replace the source
            # copies - the zconf.h origin=build precedent.
            foreach ($configHeader in @("ftconfig.h", "ftoption.h")) {
                $generated = Join-PathLiteral $buildRoot "include\freetype\config\$configHeader"
                if (-not (Test-Path -LiteralPath $generated -PathType Leaf)) {
                    Stop-BuildDeps "components.freetype generated config header is missing: $generated"
                }
                Copy-Item -LiteralPath $generated -Destination `
                    (Join-PathLiteral $stagingRoot "include\freetype\config\$configHeader") -Force
            }
            Add-ReportEvent "stage" $name "PASS" "Generated config headers staged over source copies (DEC-071)" @{
                headers = @("freetype/config/ftconfig.h", "freetype/config/ftoption.h")
            }
        }
    }

    foreach ($name in $msbuildNames) {
        $component = $Lock.components.$name
        $sourceRoot = Expand-VerifiedArchive (Get-CachePath $name $component) `
            (Join-Path $script:WorkRoot $name) $component $name
        $null = Write-MsBuildDeterminismProps $sourceRoot $name
        Invoke-FreeImageMsBuild $tools $name $sourceRoot $windowsSdkVersion
        $plan = Get-MsBuildStagePlan $name $sourceRoot $stagingRoot
        $null = Copy-StagedArtifacts $plan $name $stagingRoot
    }

    foreach ($name in $Names) {
        foreach ($output in @($Lock.components.$name.outputs)) {
            $stagedLibrary = Get-StagedOutputPath ([string]$output.path) $stagingRoot $name $wavePrefix
            if (-not (Test-Path -LiteralPath $stagedLibrary -PathType Leaf)) {
                Stop-BuildDeps "W3 expected output is missing before activation: $stagedLibrary"
            }
            Assert-StaticCrt $tools.dumpbin $stagedLibrary $name
        }
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($name in $Names) {
        foreach ($output in @($Lock.components.$name.outputs)) {
            $path = Join-Path $script:RepoRoot ([string]$output.path)
            $identity = Get-FileIdentity $path
            $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
        }
    }
    Write-WaveState "W3" $recipeFingerprint $toolchainFingerprint $toolchainIdentity $stateOutputs
    Add-ReportEvent "prefix" "W3" "PASS" "W3 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output ("PASS: W3 built with MSVC v143 x64 Release /MT through cmake (freetype) and msbuild " +
        "(freeimage) and activated at '$activeRoot'.")
    $pinContext = "PINCTX: cl=$($toolchainIdentity.cl.version) link=$($toolchainIdentity.link.version)" +
        " lib=$($toolchainIdentity.lib.version) sdk=$($toolchainIdentity.windowsSdk.version)" +
        " cmake=$($toolchainIdentity.cmake.version) fp=$toolchainFingerprint"
    Write-Output $pinContext
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Get-W4RecipeFingerprint($Lock) {
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $patchIdentities = @()
    foreach ($name in @("sdl2")) {
        foreach ($patch in @($Lock.components.$name.patches)) {
            $patchPath = Join-Path $script:RepoRoot ([string]$patch.file)
            $patchIdentity = Get-FileIdentity $patchPath
            $patchIdentities += [ordered]@{
                component = $name
                file = [string]$patch.file
                bytes = $patchIdentity.bytes
                sha256 = $patchIdentity.sha256
            }
        }
    }
    $material = [ordered]@{
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        patches = $patchIdentities
    }
    $json = $material | ConvertTo-Json -Depth 6 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes([string]$json)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    return ([BitConverter]::ToString($hash)).Replace("-", "")
}

function Repair-Sdl2HidapiInclude([string]$SourceRoot) {
    # SDL 2.0.12 appends an unquoted -I path to a compiler flag string. Preserve
    # that include and its order, quoting it for checkouts containing spaces.
    $path = Join-Path $SourceRoot 'cmake\sdlchecks.cmake'
    $before = 'set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${LIBUSB_CFLAGS} -I${SDL2_SOURCE_DIR}/src/hidapi/hidapi")'
    $after = 'set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${LIBUSB_CFLAGS} -I\"${SDL2_SOURCE_DIR}/src/hidapi/hidapi\"")'
    $text = [IO.File]::ReadAllText($path)
    if ([regex]::Matches($text, [regex]::Escape($before)).Count -ne 1) { Stop-BuildDeps 'Unexpected SDL2 HIDAPI include recipe; refusing to modify it' }
    $original = Get-FileIdentity $path
    [IO.File]::WriteAllText($path, $text.Replace($before, $after), (New-Object Text.UTF8Encoding($false)))
    Add-ReportEvent 'build' 'sdl2' 'PASS' 'Quoted upstream HIDAPI include in extracted source copy' @{
        before = $original; after = (Get-FileIdentity $path)
    }
}

function Invoke-W4Build($Lock, [string[]]$Names) {
    if (@($Names) -join "," -ne "sdl2") {
        Stop-BuildDeps "W4 recipe accepts exactly the component 'sdl2'"
    }
    $component = $Lock.components.sdl2
    if ([string]$component.wave -ne "W4") { Stop-BuildDeps "components.sdl2 is not a W4 component" }
    Assert-CMakeRecipe $component "sdl2"

    $vsVersionRange = [string]$Lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $tools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    Normalize-ProcessPathVariable
    if ($null -eq $script:ToolchainIdentity) {
        Stop-BuildDeps "toolchain identity must be resolved before a wave build"
    }
    $toolchainIdentity = $script:ToolchainIdentity
    if (@($toolchainIdentity.PSObject.Properties.Name) -notcontains "cmake") {
        Stop-BuildDeps "W4 requires cmake.exe on PATH; the resolved toolchain identity carries no cmake record"
    }
    $cmake = [string]$toolchainIdentity.cmake.path
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
        Stop-BuildDeps "the cmake recorded in the toolchain identity is not present: '$cmake'"
    }

    $recipeFingerprint = Get-W4RecipeFingerprint $Lock
    $toolchainFingerprint = Get-ToolchainFingerprint $toolchainIdentity
    $activeRoot = Join-Path $script:OutRoot "w4"
    $wavePrefix = "out/deps/w4/"
    $disposition = Get-WaveStateDisposition "W4" $recipeFingerprint $toolchainFingerprint $toolchainIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Stop-BuildDeps ("TOOLCHAIN DRIFT: recipe unchanged but toolchain identity changed ($differenceTable). " +
            "Pinned outputs cannot be reproduced. Run the controlled re-pin procedure " +
            "(see _ai/MSVC_OUTPUT_PIN_AND_REPIN_POLICY.md section 5) or restore the pinned toolchain.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        foreach ($output in @($component.outputs)) {
            Assert-StaticCrt $tools.dumpbin (Join-Path $script:RepoRoot $output.path) "sdl2"
        }
        Add-ReportEvent "state" "W4" "CACHED" "Recipe fingerprint and output identities match; build skipped"
        Write-Output "PASS: W4 state and outputs verified; build skipped. Use -Rebuild to force compilation."
        return
    }
    Add-ReportEvent "state" "W4" "STALE" "W4 will be rebuilt: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w4-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W4 staging root"
    New-Item -ItemType Directory -Path (Join-Path $stagingRoot "lib"), (Join-Path $stagingRoot "include"),
        (Join-Path $stagingRoot "licenses") -Force | Out-Null

    $sourceRoot = Expand-VerifiedArchive (Get-CachePath "sdl2" $component) `
        (Join-Path $script:WorkRoot "sdl2") $component "sdl2"
    Repair-Sdl2HidapiInclude $sourceRoot
    $buildRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w4\sdl2") $script:OutRoot `
        "components.sdl2 build directory"
    if (Test-Path -LiteralPath $buildRoot) { Remove-Item -LiteralPath $buildRoot -Recurse -Force }
    Invoke-CMakeBuild $cmake "sdl2" $component $sourceRoot $buildRoot $tools.vcToolsVersion $stagingRoot
    $plan = Get-CMakeStagePlan "sdl2" $sourceRoot $buildRoot $stagingRoot
    $null = Copy-StagedArtifacts $plan "sdl2" $stagingRoot
    # DEC-066 (phase A pilot): the engine consumes SDL headers as <SDL2/...> relative to the
    # legacy ..\SDL root, and that legacy SDL2\ folder is byte-wise the upstream include\
    # (measured, including SDL_config.h.cmake). Staging upstream include\ under include\SDL2
    # makes ..\out\deps\w4\include a drop-in replacement for the legacy path.
    $null = Copy-StagedHeaderTree (Join-Path $sourceRoot "include") `
        (Join-Path $stagingRoot "include\SDL2") "sdl2"

    foreach ($output in @($component.outputs)) {
        $stagedLibrary = Get-StagedOutputPath ([string]$output.path) $stagingRoot "sdl2" $wavePrefix
        if (-not (Test-Path -LiteralPath $stagedLibrary -PathType Leaf)) {
            Stop-BuildDeps "W4 expected output is missing before activation: $stagedLibrary"
        }
        Assert-StaticCrt $tools.dumpbin $stagedLibrary "sdl2"
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($output in @($component.outputs)) {
        $path = Join-Path $script:RepoRoot ([string]$output.path)
        $identity = Get-FileIdentity $path
        $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    Write-WaveState "W4" $recipeFingerprint $toolchainFingerprint $toolchainIdentity $stateOutputs
    Add-ReportEvent "prefix" "W4" "PASS" "W4 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output "PASS: W4 built with MSVC v143 x64 Release /MT through cmake (sdl2) and activated at '$activeRoot'."
    $pinContext = "PINCTX: cl=$($toolchainIdentity.cl.version) link=$($toolchainIdentity.link.version)" +
        " lib=$($toolchainIdentity.lib.version) sdk=$($toolchainIdentity.windowsSdk.version)" +
        " cmake=$($toolchainIdentity.cmake.version) fp=$toolchainFingerprint"
    Write-Output $pinContext
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Get-W5RecipeFingerprint($Lock) {
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $patchIdentities = @()
    foreach ($name in @("bullet")) {
        foreach ($patch in @($Lock.components.$name.patches)) {
            $patchPath = Join-Path $script:RepoRoot ([string]$patch.file)
            $patchIdentity = Get-FileIdentity $patchPath
            $patchIdentities += [ordered]@{
                component = $name
                file = [string]$patch.file
                bytes = $patchIdentity.bytes
                sha256 = $patchIdentity.sha256
            }
        }
    }
    $material = [ordered]@{
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        patches = $patchIdentities
    }
    $json = $material | ConvertTo-Json -Depth 6 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes([string]$json)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    return ([BitConverter]::ToString($hash)).Replace("-", "")
}

function Invoke-W5Build($Lock, [string[]]$Names) {
    if (@($Names) -join "," -ne "bullet") {
        Stop-BuildDeps "W5 recipe accepts exactly the component 'bullet'"
    }
    $component = $Lock.components.bullet
    if ([string]$component.wave -ne "W5") { Stop-BuildDeps "components.bullet is not a W5 component" }
    Assert-CMakeRecipe $component "bullet"

    $vsVersionRange = [string]$Lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $tools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    Normalize-ProcessPathVariable
    if ($null -eq $script:ToolchainIdentity) {
        Stop-BuildDeps "toolchain identity must be resolved before a wave build"
    }
    $toolchainIdentity = $script:ToolchainIdentity
    if (@($toolchainIdentity.PSObject.Properties.Name) -notcontains "cmake") {
        Stop-BuildDeps "W5 requires cmake.exe on PATH; the resolved toolchain identity carries no cmake record"
    }
    $cmake = [string]$toolchainIdentity.cmake.path
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
        Stop-BuildDeps "the cmake recorded in the toolchain identity is not present: '$cmake'"
    }

    $recipeFingerprint = Get-W5RecipeFingerprint $Lock
    $toolchainFingerprint = Get-ToolchainFingerprint $toolchainIdentity
    $activeRoot = Join-Path $script:OutRoot "w5"
    $wavePrefix = "out/deps/w5/"
    $disposition = Get-WaveStateDisposition "W5" $recipeFingerprint $toolchainFingerprint $toolchainIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Stop-BuildDeps ("TOOLCHAIN DRIFT: recipe unchanged but toolchain identity changed ($differenceTable). " +
            "Pinned outputs cannot be reproduced. Run the controlled re-pin procedure " +
            "(see _ai/MSVC_OUTPUT_PIN_AND_REPIN_POLICY.md section 5) or restore the pinned toolchain.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        foreach ($output in @($component.outputs)) {
            Assert-StaticCrt $tools.dumpbin (Join-Path $script:RepoRoot $output.path) "bullet"
        }
        Add-ReportEvent "state" "W5" "CACHED" "Recipe fingerprint and output identities match; build skipped"
        Write-Output "PASS: W5 state and outputs verified; build skipped. Use -Rebuild to force compilation."
        return
    }
    Add-ReportEvent "state" "W5" "STALE" "W5 will be rebuilt: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w5-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W5 staging root"
    # Unlike W4 no include directory is created: the bullet plan stages no headers, and an
    # always-empty directory would only be a standing question for every later audit.
    New-Item -ItemType Directory -Path (Join-Path $stagingRoot "lib"),
        (Join-Path $stagingRoot "licenses") -Force | Out-Null

    $sourceRoot = Expand-VerifiedArchive (Get-CachePath "bullet" $component) `
        (Join-Path $script:WorkRoot "bullet") $component "bullet"
    $buildRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w5\bullet") $script:OutRoot `
        "components.bullet build directory"
    if (Test-Path -LiteralPath $buildRoot) { Remove-Item -LiteralPath $buildRoot -Recurse -Force }
    Invoke-CMakeBuild $cmake "bullet" $component $sourceRoot $buildRoot $tools.vcToolsVersion $stagingRoot
    $plan = Get-CMakeStagePlan "bullet" $sourceRoot $buildRoot $stagingRoot
    $null = Copy-StagedArtifacts $plan "bullet" $stagingRoot
    # DEC-073 (phase A). The engine includes Bullet as ../Bullet/include, and the legacy tree
    # is the upstream src\ header shape (btBulletCollisionCommon.h + btBulletDynamicsCommon.h
    # at the root; BulletCollision\, BulletDynamics\, BulletInverseDynamics\, BulletSoftBody\,
    # LinearMath\, clew\ - measured over the bridge). The whole src\ header tree is staged: a
    # strict superset of the legacy subset (the Bullet3* families ride along), which costs
    # inert verified headers but rules out any missing internal cross-include - bullet headers
    # include each other relative to the src\ root, so the staged root is a drop-in for the
    # legacy path. No generated headers here, unlike freetype: bullet's cmake writes none.
    $null = Copy-StagedHeaderTree (Join-Path $sourceRoot "src") `
        (Join-Path $stagingRoot "include") "bullet"

    foreach ($output in @($component.outputs)) {
        $stagedLibrary = Get-StagedOutputPath ([string]$output.path) $stagingRoot "bullet" $wavePrefix
        if (-not (Test-Path -LiteralPath $stagedLibrary -PathType Leaf)) {
            Stop-BuildDeps "W5 expected output is missing before activation: $stagedLibrary"
        }
        Assert-StaticCrt $tools.dumpbin $stagedLibrary "bullet"
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($output in @($component.outputs)) {
        $path = Join-Path $script:RepoRoot ([string]$output.path)
        $identity = Get-FileIdentity $path
        $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    Write-WaveState "W5" $recipeFingerprint $toolchainFingerprint $toolchainIdentity $stateOutputs
    Add-ReportEvent "prefix" "W5" "PASS" "W5 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output "PASS: W5 built with MSVC v143 x64 Release /MT through cmake (bullet) and activated at '$activeRoot'."
    $pinContext = "PINCTX: cl=$($toolchainIdentity.cl.version) link=$($toolchainIdentity.link.version)" +
        " lib=$($toolchainIdentity.lib.version) sdk=$($toolchainIdentity.windowsSdk.version)" +
        " cmake=$($toolchainIdentity.cmake.version) fp=$toolchainFingerprint"
    Write-Output $pinContext
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Get-W6RecipeFingerprint($Lock, $Tools) {
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $patchIdentities = @()
    foreach ($name in @("assimp")) {
        foreach ($patch in @($Lock.components.$name.patches)) {
            $patchPath = Join-Path $script:RepoRoot ([string]$patch.file)
            $patchIdentity = Get-FileIdentity $patchPath
            $patchIdentities += [ordered]@{
                component = $name
                file = [string]$patch.file
                bytes = $patchIdentity.bytes
                sha256 = $patchIdentity.sha256
            }
        }
    }
    $material = [ordered]@{
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        patches = $patchIdentities
        resourceConverter = Get-FileIdentity (Join-Path (Split-Path $Tools.lib -Parent) "cvtres.exe")
    }
    $json = $material | ConvertTo-Json -Depth 6 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes([string]$json)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    return ([BitConverter]::ToString($hash)).Replace("-", "")
}

function Invoke-W6Build($Lock, [string[]]$Names) {
    if (@($Names) -join "," -ne "assimp") {
        Stop-BuildDeps "W6 recipe accepts exactly the component 'assimp'"
    }
    $component = $Lock.components.assimp
    if ([string]$component.wave -ne "W6") { Stop-BuildDeps "components.assimp is not a W6 component" }
    Assert-CMakeRecipe $component "assimp"

    $vsVersionRange = [string]$Lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $tools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    Normalize-ProcessPathVariable
    if ($null -eq $script:ToolchainIdentity) {
        Stop-BuildDeps "toolchain identity must be resolved before a wave build"
    }
    $toolchainIdentity = $script:ToolchainIdentity
    if (@($toolchainIdentity.PSObject.Properties.Name) -notcontains "cmake") {
        Stop-BuildDeps "W6 requires cmake.exe on PATH; the resolved toolchain identity carries no cmake record"
    }
    $cmake = [string]$toolchainIdentity.cmake.path
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
        Stop-BuildDeps "the cmake recorded in the toolchain identity is not present: '$cmake'"
    }

    $recipeFingerprint = Get-W6RecipeFingerprint $Lock $tools
    $toolchainFingerprint = Get-ToolchainFingerprint $toolchainIdentity
    $activeRoot = Join-Path $script:OutRoot "w6"
    $wavePrefix = "out/deps/w6/"
    $disposition = Get-WaveStateDisposition "W6" $recipeFingerprint $toolchainFingerprint $toolchainIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Stop-BuildDeps ("TOOLCHAIN DRIFT: recipe unchanged but toolchain identity changed ($differenceTable). " +
            "Pinned outputs cannot be reproduced. Run the controlled re-pin procedure " +
            "(see _ai/MSVC_OUTPUT_PIN_AND_REPIN_POLICY.md section 5) or restore the pinned toolchain.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        foreach ($output in @($component.outputs)) {
            Assert-StaticCrt $tools.dumpbin (Join-Path $script:RepoRoot $output.path) "assimp"
        }
        Add-ReportEvent "state" "W6" "CACHED" "Recipe fingerprint and output identities match; build skipped"
        Write-Output "PASS: W6 state and outputs verified; build skipped. Use -Rebuild to force compilation."
        return
    }
    Add-ReportEvent "state" "W6" "STALE" "W6 will be rebuilt: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w6-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W6 staging root"
    # As in W5, no include directory is created: the assimp plan stages no headers.
    New-Item -ItemType Directory -Path (Join-Path $stagingRoot "lib"),
        (Join-Path $stagingRoot "licenses") -Force | Out-Null

    $sourceRoot = Expand-VerifiedArchive (Get-CachePath "assimp" $component) `
        (Join-Path $script:WorkRoot "assimp") $component "assimp"
    $buildRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w6\assimp") $script:OutRoot `
        "components.assimp build directory"
    if (Test-Path -LiteralPath $buildRoot) { Remove-Item -LiteralPath $buildRoot -Recurse -Force }
    Invoke-CMakeBuild $cmake "assimp" $component $sourceRoot $buildRoot $tools.vcToolsVersion $stagingRoot
    Complete-MappedResourceArchive $tools $buildRoot 'assimp'
    $plan = Get-CMakeStagePlan "assimp" $sourceRoot $buildRoot $stagingRoot
    $null = Copy-StagedArtifacts $plan "assimp" $stagingRoot
    # DEC-075 (phase A). The engine includes Assimp as ../Assimp/include, and the legacy tree
    # is the upstream include\ shape (assimp\** with Compiler\ and port\AndroidJNI\; .h, .hpp
    # and .inl all travel) PLUS the generated config.h - measured over the bridge and against
    # the pinned 5.2.3 archive. The header filter drops config.h.in and .editorconfig by
    # extension.
    $null = Copy-StagedHeaderTree (Join-Path $sourceRoot "include") `
        (Join-Path $stagingRoot "include") "assimp"
    # config.h is generated by CONFIGURE_FILE into <build>\include\assimp\ (root CMakeLists
    # lines 240-248) and is the one generated member of assimp's public header set (listed in
    # PUBLIC_HEADERS in code\CMakeLists.txt; revision.h is also generated but lands at the
    # build root, is internal to code\ and is absent from the legacy tree). The freetype /
    # zconf.h origin=build precedent: the file the library was actually configured with.
    $generatedConfig = Join-PathLiteral $buildRoot "include\assimp\config.h"
    if (-not (Test-Path -LiteralPath $generatedConfig -PathType Leaf)) {
        Stop-BuildDeps "components.assimp generated config.h is missing: $generatedConfig"
    }
    Copy-Item -LiteralPath $generatedConfig -Destination `
        (Join-PathLiteral $stagingRoot "include\assimp\config.h") -Force
    Add-ReportEvent "stage" "assimp" "PASS" "Generated config.h staged (DEC-075)" @{
        header = "assimp/config.h"
    }

    foreach ($output in @($component.outputs)) {
        $stagedLibrary = Get-StagedOutputPath ([string]$output.path) $stagingRoot "assimp" $wavePrefix
        if (-not (Test-Path -LiteralPath $stagedLibrary -PathType Leaf)) {
            Stop-BuildDeps "W6 expected output is missing before activation: $stagedLibrary"
        }
        Assert-StaticCrt $tools.dumpbin $stagedLibrary "assimp"
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($output in @($component.outputs)) {
        $path = Join-Path $script:RepoRoot ([string]$output.path)
        $identity = Get-FileIdentity $path
        $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    Write-WaveState "W6" $recipeFingerprint $toolchainFingerprint $toolchainIdentity $stateOutputs
    Add-ReportEvent "prefix" "W6" "PASS" "W6 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output "PASS: W6 built with MSVC v143 x64 Release /MT through cmake (assimp) and activated at '$activeRoot'."
    $pinContext = "PINCTX: cl=$($toolchainIdentity.cl.version) link=$($toolchainIdentity.link.version)" +
        " lib=$($toolchainIdentity.lib.version) sdk=$($toolchainIdentity.windowsSdk.version)" +
        " cmake=$($toolchainIdentity.cmake.version) fp=$toolchainFingerprint"
    Write-Output $pinContext
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Complete-BoostArchives($Tools, [string]$BuildRoot, $Plan) {
    # Boost 1.74 passes absolute object names to lib.exe. Rearchive the exact
    # ordered inventory from the verified build with names relative to BuildRoot.
    $counts = @{ chrono = 3; date_time = 1; filesystem = 10; iostreams = 2;
        regex = 17; serialization = 35; system = 1; thread = 5 }
    $libraries = @($Plan | Where-Object { $_.role -eq 'library' })
    if ($libraries.Count -ne $counts.Count) { Stop-BuildDeps 'Unexpected Boost archive count' }
    foreach ($entry in $libraries) {
        $library = [string]$entry.from
        $name = [IO.Path]::GetFileName($library)
        if ($name -notmatch '^libboost_(chrono|date_time|filesystem|iostreams|regex|serialization|system|thread)-vc143-mt-s-x64-1_74\.lib$') {
            Stop-BuildDeps 'Unexpected Boost archive name'
        }
        $component = $Matches[1]
        $listing = Invoke-NativeCapture $Tools.lib @('/nologo', '/LIST', $library)
        if ($listing.exitCode -ne 0) { Stop-BuildDeps "Cannot list Boost $component archive" }
        $members = @($listing.output | ForEach-Object { ([string]$_).Trim() } | Where-Object { $_ })
        if ($members.Count -ne $counts[$component] -or @($members | Sort-Object -Unique).Count -ne $members.Count) {
            Stop-BuildDeps "Unexpected Boost $component member inventory"
        }
        $objects = @(foreach ($member in $members) {
            if (-not [IO.Path]::IsPathRooted($member)) { Stop-BuildDeps 'Expected absolute Boost object path' }
            $inputPath = Assert-PathUnder $member $BuildRoot 'Boost archive input'
            if ([IO.Path]::GetExtension($inputPath) -ne '.obj' -or -not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
                Stop-BuildDeps 'Missing or invalid Boost archive input'
            }
            $relative = $inputPath.Substring($BuildRoot.Length + 1)
            Assert-RelativeSafePath $relative 'Boost archive input'
            $relative
        })
        Push-Location -LiteralPath $BuildRoot
        try { $archive = Invoke-NativeCapture $Tools.lib (@('/nologo', '/MACHINE:X64', '/Brepro', "/OUT:$library") + $objects) }
        finally { Pop-Location }
        $logPath = Join-Path $script:RunRoot "boost-$component-archive.log"
        [IO.File]::WriteAllText($logPath, ($archive.output | Out-String))
        if ($archive.exitCode -ne 0 -or @($archive.output | Where-Object { [string]$_ -match 'LNK4044' }).Count) {
            Stop-BuildDeps "Boost $component relative archive failed"
        }
        Add-ReportEvent 'build' 'boost' 'PASS' 'Original ordered objects archived with relative member paths' @{
            library = $name; members = $members.Count; log = $logPath
        }
    }
}

function Get-W7RecipeFingerprint($Lock) {
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $patchIdentities = @()
    foreach ($name in @("boost")) {
        foreach ($patch in @($Lock.components.$name.patches)) {
            $patchPath = Join-Path $script:RepoRoot ([string]$patch.file)
            $patchIdentity = Get-FileIdentity $patchPath
            $patchIdentities += [ordered]@{
                component = $name
                file = [string]$patch.file
                bytes = $patchIdentity.bytes
                sha256 = $patchIdentity.sha256
            }
        }
    }
    $material = [ordered]@{
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        patches = $patchIdentities
    }
    $json = $material | ConvertTo-Json -Depth 6 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes([string]$json)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    return ([BitConverter]::ToString($hash)).Replace("-", "")
}

function Invoke-W7Build($Lock, [string[]]$Names) {
    if (@($Names) -join "," -ne "boost") {
        Stop-BuildDeps "W7 recipe accepts exactly the component 'boost'"
    }
    $component = $Lock.components.boost
    if ([string]$component.wave -ne "W7") { Stop-BuildDeps "components.boost is not a W7 component" }
    Assert-B2Recipe $component "boost"

    $vsVersionRange = [string]$Lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $tools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    Normalize-ProcessPathVariable
    if ($null -eq $script:ToolchainIdentity) {
        Stop-BuildDeps "toolchain identity must be resolved before a wave build"
    }
    $toolchainIdentity = $script:ToolchainIdentity
    # F-W2-54: the toolchain identity records carry version/bytes/sha256 only - no file path -
    # for the MSVC tools; cmake is the sole identity record with a path field. The first W7 run
    # died right here reading $toolchainIdentity.cl.path. The pinned cl.exe path comes from
    # Get-Vs2022BuildTools, which has already verified its existence; the identity still proves
    # WHICH cl this is through its hash, and the drift gate below still guards it.
    $cl = [string]$tools.cl
    if (-not (Test-Path -LiteralPath $cl -PathType Leaf)) {
        Stop-BuildDeps "the pinned cl.exe is not present: '$cl'"
    }

    $recipeFingerprint = Get-W7RecipeFingerprint $Lock
    $toolchainFingerprint = Get-ToolchainFingerprint $toolchainIdentity
    $activeRoot = Join-Path $script:OutRoot "w7"
    $wavePrefix = "out/deps/w7/"
    $disposition = Get-WaveStateDisposition "W7" $recipeFingerprint $toolchainFingerprint $toolchainIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Stop-BuildDeps ("TOOLCHAIN DRIFT: recipe unchanged but toolchain identity changed ($differenceTable). " +
            "Pinned outputs cannot be reproduced. Run the controlled re-pin procedure " +
            "(see _ai/MSVC_OUTPUT_PIN_AND_REPIN_POLICY.md section 5) or restore the pinned toolchain.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        foreach ($output in @($component.outputs)) {
            Assert-StaticCrt $tools.dumpbin (Join-Path $script:RepoRoot $output.path) "boost"
        }
        Add-ReportEvent "state" "W7" "CACHED" "Recipe fingerprint and output identities match; build skipped"
        Write-Output "PASS: W7 state and outputs verified; build skipped. Use -Rebuild to force compilation."
        return
    }
    Add-ReportEvent "state" "W7" "STALE" "W7 will be rebuilt: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w7-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W7 staging root"
    # As in W5 and W6, no include directory is created: the boost plan stages no headers.
    New-Item -ItemType Directory -Path (Join-Path $stagingRoot "lib"),
        (Join-Path $stagingRoot "licenses") -Force | Out-Null

    $sourceRoot = Expand-VerifiedArchive (Get-CachePath "boost" $component) `
        (Join-Path $script:WorkRoot "boost") $component "boost"

    # Step 1 - bootstrap: upstream's own two sanctioned scripts (DEC-045) compile the b2 engine
    # from source with the toolchain on PATH. bootstrap.bat writes b2.exe into its own working
    # directory, which is the disposable work tree - the repository is never touched.
    $bootstrap = Join-Path $sourceRoot "bootstrap.bat"
    if (-not (Test-Path -LiteralPath $bootstrap -PathType Leaf)) {
        Stop-BuildDeps "the verified boost tree carries no bootstrap.bat: $bootstrap"
    }
    $comSpec = [string]$env:ComSpec
    if (-not (Test-Path -LiteralPath $comSpec -PathType Leaf)) {
        Stop-BuildDeps "cmd.exe was not found through ComSpec"
    }
    # F-W2-55: b2 4.3.0's guess_toolset.bat finds Visual Studio 2022 through vswhere but maps
    # it to "vcunk" and refuses (measured: bootstrap.log of the first W7 run; the script's own
    # support list ends at vc142). The toolset is therefore named explicitly. The vc142 branch
    # only selects how the engine build configures its environment and then runs the cl that
    # environment provides - which is the pinned v143; the label does not choose a compiler.
    # F-W2-56: naming the toolset got past the "Unknown toolset: vcunk" refusal, but the vc142
    # branch then set up nothing: it derives B2_TOOLSET_ROOT from %VS160COMNTOOLS% (Visual Studio
    # 2019), which is empty here, so its call to "%B2_TOOLSET_ROOT%Auxiliary\Build\vcvarsall.bat"
    # became a relative path, Call_If_Exists silently skipped it, and cl was absent from the
    # child's PATH (measured: bootstrap.log of the third W7 run). Upstream leaves a pre-set
    # B2_TOOLSET_ROOT alone, so the driver hands it the pinned Visual Studio root and upstream's
    # own vcvarsall does the rest. The boost sources are not touched.
    $toolsetRoot = Get-B2ToolsetRoot $tools.installationPath
    $vcvarsall = Join-Path $toolsetRoot "Auxiliary\Build\vcvarsall.bat"
    if (-not (Test-Path -LiteralPath $vcvarsall -PathType Leaf)) {
        Stop-BuildDeps ("the pinned Visual Studio installation carries no vcvarsall.bat at " +
            "'$vcvarsall'; the b2 bootstrap cannot configure the compiler environment")
    }
    Add-ReportEvent "build" "boost" "PASS" "b2 bootstrap toolset root pinned to the resolved Visual Studio" @{
        b2ToolsetRoot = $toolsetRoot
        vcvarsall = $vcvarsall
    }
    $null = Invoke-B2Step $comSpec @("/d", "/c", "call bootstrap.bat vc142") "boost" "bootstrap" $sourceRoot `
        @{ B2_TOOLSET_ROOT = $toolsetRoot }
    $b2 = Join-Path $sourceRoot "b2.exe"
    if (-not (Test-Path -LiteralPath $b2 -PathType Leaf)) {
        Stop-BuildDeps "bootstrap reported success but produced no b2.exe: $b2"
    }
    Write-Output "PASS: b2 engine bootstrapped from the verified boost tree."

    # Step 2 - build and stage. The toolset is named explicitly through a generated user-config
    # (Write-B2UserConfig) because b2 4.3.0 predates Visual Studio 2022; --layout=versioned is
    # pinned so the staged names match the pinned outputs regardless of upstream defaults.
    # F-W2-57: the same vcvarsall.bat that the bootstrap gate above already resolved and proved
    # to exist is handed to the generated user-config as the <setup> option, because msvc.jam
    # cannot locate it for a 14.3 toolset on its own and silently degrades to skipping every
    # compile target. One resolution, one existence check, two consumers.
    $userConfig = Write-B2UserConfig (Join-Path $script:RunRoot "boost-user-config.jam") $cl $vcvarsall
    $flags = Expand-RecipeFlags $component.buildProfile.flags "boost" $stagingRoot
    Add-ReportEvent "build" "boost" "PASS" "Recipe properties expanded for b2" @{
        declared = @($component.buildProfile.flags)
        effective = @($flags)
    }
    $buildRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w7\boost") $script:OutRoot `
        "components.boost build directory"
    $b2StageRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w7\boost-stage") $script:OutRoot `
        "components.boost b2 stage directory"
    foreach ($directory in @($buildRoot, $b2StageRoot)) {
        if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    }
    # F-W2-59: --hash collapses the property chain in the build-tree paths (release\
    # address-model-64\link-static\runtime-link-static\[threadapi-win32\]threading-multi) into
    # one 32-character digest directory. Without it the archive response files exceed the
    # Windows 260-character path limit and b2 dies with "[errno 2] failed to write output
    # file '...lib.rsp'": measured in the fifth W7 run, thread needs 263 characters (its
    # extra threadapi-win32 element makes it the longest), wserialization 262, serialization
    # 261, while everything at 255 or shorter built fine. With --hash the longest archive
    # response file measures 212 characters (dry-run probe H, DEC-050); the alternative
    # --abbreviate-paths leaves 246 and was rejected as too close to the limit. Only the
    # bin.v2 build tree is affected - staged names come from --layout=versioned and the
    # probe showed them unchanged.
    # Keep absolute maps out of b2 properties: --hash includes those properties
    # in object directory names. Archive member paths are made relative below.
    $compilerMaps = " /experimental:deterministic /pathmap:`"$sourceRoot=W7SRC`" /pathmap:`"$buildRoot=W7OBJ`""
    $b2Environment = @{ _CL_ = ([string]$env:_CL_ + $compilerMaps) }
    $b2Arguments = @(
        "--user-config=$userConfig",
        "toolset=msvc-14.3",
        "--layout=versioned",
        "--hash",
        "--build-dir=$buildRoot",
        "--stagedir=$b2StageRoot",
        # DEC-083. The b2 path had no /Brepro - the last build system of the programme to get
        # it. Measured on the pinned 1.74.0 msvc.jam (owner probe): line 1923 binds
        # <archiveflags> to the AROPTIONS of the archive action (lib.exe), and line 1873 binds
        # <cflags> to the OPTIONS of msvc.compile - the parent of both compile.c and
        # compile.c++, so one property covers every translation unit. Command-line properties
        # are additive and appear verbatim in the b2 command lines of the log.
        "cflags=/Brepro",
        "archiveflags=/Brepro",
        "-j$([Environment]::ProcessorCount)"
    ) + $flags + @("stage")
    $null = Invoke-B2Step $b2 $b2Arguments "boost" "stage" $sourceRoot $b2Environment
    Write-Output "PASS: boost compiled and staged with b2 (msvc-14.3, static release /MT)."

    $plan = Get-B2StagePlan "boost" $sourceRoot $b2StageRoot $stagingRoot
    Complete-BoostArchives $tools $buildRoot $plan
    $null = Copy-StagedArtifacts $plan "boost" $stagingRoot
    # DEC-083 (phase A). The engine includes Boost as ../Boost - the include root IS the
    # directory that holds boost\ (measured over the bridge: legacy Boost\ = boost\ + lib\).
    # The whole upstream boost\ header forest is staged under include\boost - the largest
    # header set of phase A (thousands of files; .hpp/.ipp/.h all travel - the extension
    # filter of Copy-StagedHeaderTree was designed for exactly this tree). No generated
    # headers: b2 builds in-tree and configures nothing into boost\.
    $null = Copy-StagedHeaderTree (Join-Path $sourceRoot "boost") `
        (Join-Path $stagingRoot "include\boost") "boost"

    foreach ($output in @($component.outputs)) {
        $stagedLibrary = Get-StagedOutputPath ([string]$output.path) $stagingRoot "boost" $wavePrefix
        if (-not (Test-Path -LiteralPath $stagedLibrary -PathType Leaf)) {
            Stop-BuildDeps "W7 expected output is missing before activation: $stagedLibrary"
        }
        Assert-StaticCrt $tools.dumpbin $stagedLibrary "boost"
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($output in @($component.outputs)) {
        $path = Join-Path $script:RepoRoot ([string]$output.path)
        $identity = Get-FileIdentity $path
        $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    Write-WaveState "W7" $recipeFingerprint $toolchainFingerprint $toolchainIdentity $stateOutputs
    Add-ReportEvent "prefix" "W7" "PASS" "W7 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output "PASS: W7 built with MSVC v143 x64 Release /MT through b2 (boost) and activated at '$activeRoot'."
    $pinContext = "PINCTX: cl=$($toolchainIdentity.cl.version) link=$($toolchainIdentity.link.version)" +
        " lib=$($toolchainIdentity.lib.version) sdk=$($toolchainIdentity.windowsSdk.version)" +
        " fp=$toolchainFingerprint"
    Write-Output $pinContext
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Get-W8RecipeFingerprint($Lock) {
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $patchIdentities = @()
    foreach ($name in @("mono-msi")) {
        foreach ($patch in @($Lock.components.$name.patches)) {
            $patchPath = Join-Path $script:RepoRoot ([string]$patch.file)
            $patchIdentity = Get-FileIdentity $patchPath
            $patchIdentities += [ordered]@{
                component = $name
                file = [string]$patch.file
                bytes = $patchIdentity.bytes
                sha256 = $patchIdentity.sha256
            }
        }
    }
    $material = [ordered]@{
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        patches = $patchIdentities
    }
    $json = $material | ConvertTo-Json -Depth 6 -Compress
    return Get-StringSha256 ([string]$json)
}

function Invoke-W8Build($Lock, [string[]]$Names) {
    if (@($Names) -join "," -ne "mono-msi") {
        Stop-BuildDeps "W8 recipe accepts exactly the component 'mono-msi'"
    }
    $component = $Lock.components.'mono-msi'
    if ([string]$component.wave -ne "W8") { Stop-BuildDeps "components.mono-msi is not a W8 component" }
    Assert-MsiexecRecipe $component "mono-msi"

    $recipeFingerprint = Get-W8RecipeFingerprint $Lock
    # W8 carries no compiler, so the second half of the wave fingerprint is not the MSVC
    # identity: a Visual Studio update must not mark upstream-built mono binaries stale, and a
    # swapped MSI must. The wave identity is therefore the identity of the pinned inputs
    # themselves - the MSI, the LGPL source-offer archive and the licence text (DEC-054). The
    # record keeps the version/bytes/sha256 field shape of toolchain identities so the state
    # machinery reads it unchanged.
    $waveIdentity = [ordered]@{
        extractor = [ordered]@{ version = "msiexec-administrative-image" }
        msi = [ordered]@{
            version = [string]$component.version
            bytes = [long]$component.archive.bytes
            sha256 = [string]$component.archive.sha256
        }
        sourceOffer = [ordered]@{ sha256 = [string]$component.sourceOffer.sha256 }
        license = [ordered]@{ sha256 = [string]@($component.licenseFiles)[0].sha256 }
    }
    $toolchainFingerprint = Get-StringSha256 ([string]($waveIdentity | ConvertTo-Json -Depth 6 -Compress))

    $activeRoot = Join-Path $script:OutRoot "w8"
    $wavePrefix = "out/deps/w8/"
    $disposition = Get-WaveStateDisposition "W8" $recipeFingerprint $toolchainFingerprint $waveIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Stop-BuildDeps ("W8 INPUT DRIFT: recipe unchanged but the pinned mono inputs changed " +
            "($differenceTable). The staged binaries would no longer match the recorded wave; " +
            "re-pin deliberately or restore the pinned archives.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        Add-ReportEvent "state" "W8" "CACHED" "Recipe fingerprint and output identities match; extraction skipped"
        Write-Output "PASS: W8 state and outputs verified; extraction skipped. Use -Rebuild to force it."
        return
    }
    Add-ReportEvent "state" "W8" "STALE" "W8 will be extracted: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w8-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W8 staging root"
    New-Item -ItemType Directory -Path $stagingRoot, (Join-Path $stagingRoot "licenses\mono") -Force | Out-Null

    # Gate 1 - the MSI, byte for byte. The cache entry carries the canonical name derived from
    # the lock; the identity check quarantines on mismatch exactly as for source archives.
    $cacheRoot = Assert-PathUnder $CacheDir $script:RepoRoot "cache directory"
    $msiPath = Assert-PathUnder (Get-CachePath "mono-msi" $component) $cacheRoot "archive input"
    if (-not (Test-Path -LiteralPath $msiPath -PathType Leaf)) {
        Stop-BuildDeps ("components.mono-msi cache entry is missing: '$msiPath'; copy the verified " +
            "MSI from deps-inbox under this canonical name (RUNBOOK section 46)")
    }
    if (-not (Test-FileIdentity $msiPath ([long]$component.archive.bytes) ([string]$component.archive.sha256) -QuarantineOnFailure)) {
        Stop-BuildDeps "components.mono-msi cached MSI changed and was quarantined"
    }

    # Gate 2 - the LGPL source offer and the licence text. The offer archive must be present
    # and match its pin on every W8 build: shipping mono binaries without holding the exact
    # corresponding sources is not a state this project is allowed to be in. The licence text
    # is a single file the owner extracts once from the verified offer archive (the driver
    # cannot read .tar.xz itself: the bundled Windows tar has no xz filter - DEC-054); its pin
    # in the lock makes the loose file tamper-evident.
    $inboxRoot = Join-Path $script:RepoRoot "deps-inbox"
    $offerName = [string](Split-Path -Leaf ([Uri]$component.sourceOffer.url).AbsolutePath)
    $offerPath = Join-Path $inboxRoot $offerName
    if (-not (Test-Path -LiteralPath $offerPath -PathType Leaf)) {
        Stop-BuildDeps "components.mono-msi LGPL source offer archive is missing: '$offerPath'"
    }
    $offerHash = (Get-FileHash -LiteralPath $offerPath -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($offerHash -ne ([string]$component.sourceOffer.sha256).ToUpperInvariant()) {
        Stop-BuildDeps ("components.mono-msi source offer SHA-256 mismatch: expected " +
            "$($component.sourceOffer.sha256), measured $offerHash")
    }
    $licensePin = [string]@($component.licenseFiles)[0].sha256
    $licensePath = Join-Path $inboxRoot ("mono-{0}-LICENSE" -f [string]$component.version)
    if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) {
        Stop-BuildDeps ("components.mono-msi licence text is missing: '$licensePath'; extract LICENSE " +
            "from the verified source offer archive (RUNBOOK section 46)")
    }
    $licenseHash = (Get-FileHash -LiteralPath $licensePath -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($licenseHash -ne $licensePin.ToUpperInvariant()) {
        Stop-BuildDeps ("components.mono-msi licence text SHA-256 mismatch: expected $licensePin, " +
            "measured $licenseHash")
    }

    # Step 1 - administrative extraction into the disposable work tree. The Authenticode gate
    # and the msiexec invocation live in Invoke-MsiAdministrativeExtract. The executable
    # inventory gate that archive waves run is deliberately not applied here: the lock pins the
    # MSI byte for byte, so its payload cannot vary, and enumerating the thousands of upstream
    # runtime files in the lock would add bookkeeping without adding a guarantee (DEC-054). The
    # measured shape gate below still refuses an image that does not match the probe.
    # F-W2-63: this direct caller wipes its own scratch too - the msi image path is now
    # run-invariant and a second W8 run must not stop on its predecessor's leftovers.
    $null = Reset-WorkScratch (Join-Path $script:WorkRoot "mono-msi")
    $imageRoot = Assert-FreshEmptyDirectory (Join-Path $script:WorkRoot "mono-msi")
    Invoke-MsiAdministrativeExtract $msiPath $imageRoot "mono-msi"
    Assert-NoReparsePoints $imageRoot
    # F-W2-60: msiexec /a names the MSI copy inside the image after the file it was INVOKED
    # with - here the canonical cache name - not after the upstream download name. The first
    # W8 run proved it: the shape gate stopped on 'mono-msi-6.8.0.105-dcbe043636f4.msi'
    # while this line was deriving the expectation from the archive URL. The invoked file's
    # own name is the correct, measured expectation.
    $msiFileName = [string](Split-Path -Leaf $msiPath)
    $monoRoot = Assert-MsiImageShape $imageRoot $msiFileName "mono-msi"
    $imageFiles = @(Get-ChildItem -LiteralPath $monoRoot -Recurse -Force -File)
    Add-ReportEvent "build" "mono-msi" "PASS" "Administrative image extracted and shape-verified" @{
        monoFiles = $imageFiles.Count
        image = $imageRoot
    }
    Write-Output "PASS: mono administrative image extracted ($($imageFiles.Count) files under Mono)."

    # Step 2 - stage: the whole Mono tree verbatim, the runtime DLL copied to the wave root as
    # the outputs declare, and the verified licence text. The count gate proves the recursive
    # copy carried every file.
    Copy-Item -LiteralPath $monoRoot -Destination (Join-Path $stagingRoot "Mono") -Recurse -Force
    $sgenSource = Join-Path $monoRoot "bin\mono-2.0-sgen.dll"
    if (-not (Test-Path -LiteralPath $sgenSource -PathType Leaf)) {
        Stop-BuildDeps "components.mono-msi image carries no Mono\bin\mono-2.0-sgen.dll"
    }
    Copy-Item -LiteralPath $sgenSource -Destination (Join-Path $stagingRoot "mono-2.0-sgen.dll") -Force
    Copy-Item -LiteralPath $licensePath -Destination (Join-Path $stagingRoot "licenses\mono\LICENSE") -Force
    Assert-NoReparsePoints $stagingRoot
    $stagedCount = @(Get-ChildItem -LiteralPath (Join-Path $stagingRoot "Mono") -Recurse -Force -File).Count
    if ($stagedCount -ne $imageFiles.Count) {
        Stop-BuildDeps ("components.mono-msi staged file count $stagedCount does not match the " +
            "extracted image count $($imageFiles.Count)")
    }

    foreach ($output in @($component.outputs)) {
        $stagedOutput = Get-StagedOutputPath ([string]$output.path) $stagingRoot "mono-msi" $wavePrefix
        if (-not (Test-Path -LiteralPath $stagedOutput -PathType Leaf)) {
            Stop-BuildDeps "W8 expected output is missing before activation: $stagedOutput"
        }
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($output in @($component.outputs)) {
        $path = Join-Path $script:RepoRoot ([string]$output.path)
        $identity = Get-FileIdentity $path
        $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    Write-WaveState "W8" $recipeFingerprint $toolchainFingerprint $waveIdentity $stateOutputs
    Add-ReportEvent "prefix" "W8" "PASS" "W8 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output ("PASS: W8 mono administrative image staged and activated at '$activeRoot' " +
        "(upstream binaries, no compilation; $stagedCount files under Mono).")
    Write-Output ("PINCTX: msi=$($component.archive.sha256) offer=$($component.sourceOffer.sha256) " +
        "license=$licensePin fp=$toolchainFingerprint")
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Get-W8bRecipeFingerprint($Plan, [string]$ProjectRoot, [string]$CsprojPath) {
    # W8b is in-tree: the sources ARE the recipe. The fingerprint therefore covers the driver,
    # the project file and every listed source file, in project order - editing any .cs file
    # honestly re-opens the wave instead of being skipped as "already built".
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $csprojIdentity = Get-FileIdentity $CsprojPath
    $sourceIdentities = @()
    foreach ($source in @($Plan.sources)) {
        $path = Join-PathLiteral $ProjectRoot ($source.Replace('/', '\'))
        $identity = Get-FileIdentity $path
        $sourceIdentities += [ordered]@{ file = $source; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    $material = [ordered]@{
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        csproj = [ordered]@{ bytes = $csprojIdentity.bytes; sha256 = $csprojIdentity.sha256 }
        sources = $sourceIdentities
    }
    $json = $material | ConvertTo-Json -Depth 6 -Compress
    return Get-StringSha256 ([string]$json)
}

function Invoke-W8bBuild($Lock, [string[]]$Names) {
    if (@($Names) -join "," -ne "falco-csharp-api") {
        Stop-BuildDeps "W8b recipe accepts exactly the component 'falco-csharp-api'"
    }
    $component = $Lock.components.'falco-csharp-api'
    if ([string]$component.wave -ne "W8b") { Stop-BuildDeps "components.falco-csharp-api is not a W8b component" }
    Assert-McsRecipe $component "falco-csharp-api"

    # The compiler comes from W8 and must match the lock's pinned identities before it is
    # trusted to produce a pinnable output. All three pieces are verified: the compiler, the
    # runtime that hosts it and the runtime's native core.
    $mono = $Lock.components.'mono-msi'
    $toolPins = @{}
    foreach ($output in @($mono.outputs)) {
        $toolPins[([string]$output.path)] = $output
    }
    $requiredTools = @("out/deps/w8/Mono/lib/mono/4.5/mcs.exe", "out/deps/w8/Mono/bin/mono.exe",
        "out/deps/w8/mono-2.0-sgen.dll")
    $toolIdentity = [ordered]@{}
    foreach ($toolPath in $requiredTools) {
        if (-not $toolPins.ContainsKey($toolPath)) {
            Stop-BuildDeps "components.mono-msi does not pin the W8b tool '$toolPath'"
        }
        $pin = $toolPins[$toolPath]
        Assert-PinnedSha256 ([string]$pin.sha256) "components.mono-msi output '$toolPath'"
        $fullPath = Join-Path $script:RepoRoot ($toolPath.Replace('/', '\'))
        if (-not (Test-FileIdentity $fullPath ([long]$pin.bytes) ([string]$pin.sha256))) {
            Stop-BuildDeps ("W8b compiler input does not match its W8 pin: '$toolPath'; " +
                "rebuild or re-verify W8 before building W8b")
        }
        $toolIdentity[$toolPath] = [ordered]@{ bytes = [long]$pin.bytes; sha256 = [string]$pin.sha256 }
    }

    $projectRoot = Join-Path $script:RepoRoot "API"
    $csprojPath = Join-Path $projectRoot "API.csproj"
    $plan = Get-CsprojCompilePlan $csprojPath "falco-csharp-api"
    Add-ReportEvent "build" "falco-csharp-api" "PASS" "Compile plan read from the project file" @{
        sources = @($plan.sources).Count
        references = @($plan.references)
    }
    $recipeFingerprint = Get-W8bRecipeFingerprint $plan $projectRoot $csprojPath
    $waveIdentity = [ordered]@{
        compiler = [ordered]@{ version = [string]$mono.version }
        tools = $toolIdentity
    }
    $toolchainFingerprint = Get-StringSha256 ([string]($waveIdentity | ConvertTo-Json -Depth 6 -Compress))

    $activeRoot = Join-Path $script:OutRoot "w8b"
    $wavePrefix = "out/deps/w8b/"
    $disposition = Get-WaveStateDisposition "W8b" $recipeFingerprint $toolchainFingerprint $waveIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Stop-BuildDeps ("W8b INPUT DRIFT: recipe unchanged but the pinned compiler inputs changed " +
            "($differenceTable). Re-verify W8 or re-pin deliberately.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        Add-ReportEvent "state" "W8b" "CACHED" "Recipe fingerprint and output identities match; compilation skipped"
        Write-Output "PASS: W8b state and outputs verified; compilation skipped. Use -Rebuild to force it."
        return
    }
    Add-ReportEvent "state" "W8b" "STALE" "W8b will be compiled: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w8b-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W8b staging root"
    New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null

    # The exact command the cs27 probe proved twice over: upstream's own compile.sh options
    # (-target:library, -nowarn:CS8029, -codepage:65001, -debug-) plus the project's Release
    # profile (-optimize+, -unsafe, -define:TRACE, the declared framework references) plus
    # -deterministic, which mcs 6.8 honours: two probe runs produced byte-identical output
    # (121 856 B, 02CAA41C...). Sources and references come from the project file, never from
    # a hand-kept list. The response file lands in the run directory as the record.
    $outputPath = Join-Path $stagingRoot "FalcoEngine.dll"
    $responseLines = @("-target:library", "-optimize+", "-unsafe", "-define:TRACE", "-nowarn:CS8029",
        "-codepage:65001", "-debug-", "-deterministic")
    foreach ($reference in @($plan.references)) {
        $responseLines += "-r:$reference.dll"
    }
    foreach ($source in @($plan.sources)) {
        $sourcePath = Join-PathLiteral $projectRoot ($source.Replace('/', '\'))
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            Stop-BuildDeps "components.falco-csharp-api source file is missing: '$source'"
        }
        $responseLines += "`"$sourcePath`""
    }
    $responseFile = Join-Path $script:RunRoot "falco-csharp-api-mcs.rsp"
    [IO.File]::WriteAllLines($responseFile, $responseLines)

    $monoExe = Join-Path $script:RepoRoot "out\deps\w8\Mono\bin\mono.exe"
    $mcsExe = Join-Path $script:RepoRoot "out\deps\w8\Mono\lib\mono\4.5\mcs.exe"
    $capture = Invoke-NativeCapture $monoExe @($mcsExe, "-out:$outputPath", "@$responseFile")
    $logPath = Join-Path $script:RunRoot "falco-csharp-api-mcs.log"
    [IO.File]::WriteAllLines($logPath, @($capture.output | ForEach-Object { [string]$_ }))
    if ($capture.exitCode -ne 0) {
        Stop-BuildDeps "mcs compilation failed for components.falco-csharp-api with exit code $($capture.exitCode); log: $logPath"
    }
    if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
        Stop-BuildDeps "mcs reported success but produced no output: $outputPath"
    }
    Write-Output "PASS: FalcoEngine.dll compiled with the pinned mcs (deterministic, static plan from API.csproj)."

    foreach ($output in @($component.outputs)) {
        $stagedOutput = Get-StagedOutputPath ([string]$output.path) $stagingRoot "falco-csharp-api" $wavePrefix
        if (-not (Test-Path -LiteralPath $stagedOutput -PathType Leaf)) {
            Stop-BuildDeps "W8b expected output is missing before activation: $stagedOutput"
        }
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($output in @($component.outputs)) {
        $path = Join-Path $script:RepoRoot ([string]$output.path)
        $identity = Get-FileIdentity $path
        $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    Write-WaveState "W8b" $recipeFingerprint $toolchainFingerprint $waveIdentity $stateOutputs
    Add-ReportEvent "prefix" "W8b" "PASS" "W8b staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output "PASS: W8b built with the pinned mcs from W8 and activated at '$activeRoot'."
    Write-Output ("PINCTX: mcs=$($toolIdentity['out/deps/w8/Mono/lib/mono/4.5/mcs.exe'].sha256) " +
        "mono=$($toolIdentity['out/deps/w8/Mono/bin/mono.exe'].sha256) fp=$toolchainFingerprint")
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Get-W9RecipeFingerprint($Lock) {
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $patchIdentities = @()
    foreach ($name in @("openal-soft")) {
        foreach ($patch in @($Lock.components.$name.patches)) {
            $patchPath = Join-Path $script:RepoRoot ([string]$patch.file)
            $patchIdentity = Get-FileIdentity $patchPath
            $patchIdentities += [ordered]@{
                component = $name
                file = [string]$patch.file
                bytes = $patchIdentity.bytes
                sha256 = $patchIdentity.sha256
            }
        }
    }
    $material = [ordered]@{
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        patches = $patchIdentities
    }
    $json = $material | ConvertTo-Json -Depth 6 -Compress
    return Get-StringSha256 ([string]$json)
}

function Invoke-W9Build($Lock, [string[]]$Names) {
    if (@($Names) -join "," -ne "openal-soft") {
        Stop-BuildDeps "W9 recipe accepts exactly the component 'openal-soft'"
    }
    $component = $Lock.components.'openal-soft'
    if ([string]$component.wave -ne "W9") { Stop-BuildDeps "components.openal-soft is not a W9 component" }
    Assert-SharedCMakeRecipe $component "openal-soft"

    $vsVersionRange = [string]$Lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $tools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    Normalize-ProcessPathVariable
    if ($null -eq $script:ToolchainIdentity) {
        Stop-BuildDeps "toolchain identity must be resolved before a wave build"
    }
    $toolchainIdentity = $script:ToolchainIdentity
    if (@($toolchainIdentity.PSObject.Properties.Name) -notcontains "cmake") {
        Stop-BuildDeps "W9 requires cmake.exe on PATH; the resolved toolchain identity carries no cmake record"
    }
    $cmake = [string]$toolchainIdentity.cmake.path
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
        Stop-BuildDeps "the cmake recorded in the toolchain identity is not present: '$cmake'"
    }

    $recipeFingerprint = Get-W9RecipeFingerprint $Lock
    $toolchainFingerprint = Get-ToolchainFingerprint $toolchainIdentity
    $activeRoot = Join-Path $script:OutRoot "w9"
    $wavePrefix = "out/deps/w9/"
    $disposition = Get-WaveStateDisposition "W9" $recipeFingerprint $toolchainFingerprint $toolchainIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Stop-BuildDeps ("TOOLCHAIN DRIFT: recipe unchanged but toolchain identity changed ($differenceTable). " +
            "Pinned outputs cannot be reproduced. Run the controlled re-pin procedure " +
            "(see _ai/MSVC_OUTPUT_PIN_AND_REPIN_POLICY.md section 5) or restore the pinned toolchain.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        # W9 stages a DLL and its import library: Assert-StaticCrt does not apply (the DLL is
        # deliberately /MD per DEC-060, and an import library carries no object code to judge).
        # The identity check inside the disposition already proved the bytes.
        Add-ReportEvent "state" "W9" "CACHED" "Recipe fingerprint and output identities match; build skipped"
        Write-Output "PASS: W9 state and outputs verified; build skipped. Use -Rebuild to force compilation."
        return
    }
    Add-ReportEvent "state" "W9" "STALE" "W9 will be rebuilt: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w9-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W9 staging root"
    # bin\ appears for the first time in a wave: the DLL is a runtime artefact, not a linker
    # input, and the declared outputs keep the two apart (bin\OpenAL32.dll, lib\OpenAL32.lib).
    New-Item -ItemType Directory -Path (Join-Path $stagingRoot "bin"),
        (Join-Path $stagingRoot "lib"), (Join-Path $stagingRoot "licenses") -Force | Out-Null

    $sourceRoot = Expand-VerifiedArchive (Get-CachePath "openal-soft" $component) `
        (Join-Path $script:WorkRoot "openal-soft") $component "openal-soft"
    $buildRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w9\openal-soft") $script:OutRoot `
        "components.openal-soft build directory"
    if (Test-Path -LiteralPath $buildRoot) { Remove-Item -LiteralPath $buildRoot -Recurse -Force }
    Invoke-CMakeBuild $cmake "openal-soft" $component $sourceRoot $buildRoot $tools.vcToolsVersion $stagingRoot
    $plan = Get-CMakeStagePlan "openal-soft" $sourceRoot $buildRoot $stagingRoot
    $null = Copy-StagedArtifacts $plan "openal-soft" $stagingRoot
    # DEC-078 (phase A). The engine includes OpenAL as ../OpenAL/include; the legacy tree is
    # the old Creative SDK shape (flat headers + an AL\ subset + alut/alu/xram relics tied to
    # the alut.lib/ALu.lib phase-C question). openal-soft's public surface is include\AL\
    # alone - six headers, all static in the source tree (measured on the pinned 1.25.2
    # archive: no CONFIGURE_FILE targets include\AL; the generated config.h/version.h are
    # private to the build). Whether the engine's own #include lines use the flat or the AL\
    # form is measured in phase B when the paths are retargeted.
    $null = Copy-StagedHeaderTree (Join-Path $sourceRoot "include\AL") `
        (Join-Path $stagingRoot "include\AL") "openal-soft"

    foreach ($output in @($component.outputs)) {
        $stagedArtifact = Get-StagedOutputPath ([string]$output.path) $stagingRoot "openal-soft" $wavePrefix
        if (-not (Test-Path -LiteralPath $stagedArtifact -PathType Leaf)) {
            Stop-BuildDeps "W9 expected output is missing before activation: $stagedArtifact"
        }
        # No Assert-StaticCrt here - see the S1 branch comment and DEC-060.
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($output in @($component.outputs)) {
        $path = Join-Path $script:RepoRoot ([string]$output.path)
        $identity = Get-FileIdentity $path
        $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    Write-WaveState "W9" $recipeFingerprint $toolchainFingerprint $toolchainIdentity $stateOutputs
    Add-ReportEvent "prefix" "W9" "PASS" "W9 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output "PASS: W9 built with MSVC v143 x64 Release through cmake (openal-soft, SHARED per LGPL) and activated at '$activeRoot'."
    $pinContext = "PINCTX: cl=$($toolchainIdentity.cl.version) link=$($toolchainIdentity.link.version)" +
        " lib=$($toolchainIdentity.lib.version) sdk=$($toolchainIdentity.windowsSdk.version)" +
        " cmake=$($toolchainIdentity.cmake.version) fp=$toolchainFingerprint"
    Write-Output $pinContext
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Get-W10RecipeFingerprint($Lock) {
    . (Join-Path $script:RepoRoot 'Clipper2Source.ps1')
    $clipper = Get-FalcoClipper2Bundle
    $clipperVerifier = Get-FileIdentity (Join-Path $script:RepoRoot 'Clipper2Source.ps1')
    $driverIdentity = Get-FileIdentity $PSCommandPath
    $patchIdentities = @()
    foreach ($name in @("manifold")) {
        foreach ($patch in @($Lock.components.$name.patches)) {
            $patchPath = Join-Path $script:RepoRoot ([string]$patch.file)
            $patchIdentity = Get-FileIdentity $patchPath
            $patchIdentities += [ordered]@{
                component = $name
                file = [string]$patch.file
                bytes = $patchIdentity.bytes
                sha256 = $patchIdentity.sha256
            }
        }
    }
    $material = [ordered]@{
        clipperSource = $clipper.sha256
        clipperVerifier = $clipperVerifier.sha256
        driver = [ordered]@{ bytes = $driverIdentity.bytes; sha256 = $driverIdentity.sha256 }
        patches = $patchIdentities
    }
    $json = $material | ConvertTo-Json -Depth 6 -Compress
    return Get-StringSha256 ([string]$json)
}

function Invoke-W10Build($Lock, [string[]]$Names) {
    if (@($Names) -join "," -ne "manifold") {
        Stop-BuildDeps "W10 recipe accepts exactly the component 'manifold'"
    }
    $component = $Lock.components.manifold
    if ([string]$component.wave -ne "W10") { Stop-BuildDeps "components.manifold is not a W10 component" }
    # The static gate of W2-W6 applies unchanged: manifold pins the static release CRT and
    # forbids shared libraries - the last wave walks the oldest, best-proven rail.
    Assert-CMakeRecipe $component "manifold"

    $vsVersionRange = [string]$Lock.toolchain.vs
    if (-not $vsVersionRange) {
        Stop-BuildDeps "deps.lock toolchain.vs version range is missing or empty"
    }
    $tools = Get-Vs2022BuildTools -VsVersionRange $vsVersionRange
    Normalize-ProcessPathVariable
    if ($null -eq $script:ToolchainIdentity) {
        Stop-BuildDeps "toolchain identity must be resolved before a wave build"
    }
    $toolchainIdentity = $script:ToolchainIdentity
    if (@($toolchainIdentity.PSObject.Properties.Name) -notcontains "cmake") {
        Stop-BuildDeps "W10 requires cmake.exe on PATH; the resolved toolchain identity carries no cmake record"
    }
    $cmake = [string]$toolchainIdentity.cmake.path
    if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
        Stop-BuildDeps "the cmake recorded in the toolchain identity is not present: '$cmake'"
    }

    $recipeFingerprint = Get-W10RecipeFingerprint $Lock
    $toolchainFingerprint = Get-ToolchainFingerprint $toolchainIdentity
    $activeRoot = Join-Path $script:OutRoot "w10"
    $wavePrefix = "out/deps/w10/"
    $disposition = Get-WaveStateDisposition "W10" $recipeFingerprint $toolchainFingerprint $toolchainIdentity
    if ($disposition.status -eq "S2") {
        $differenceTable = (@($disposition.differences) |
            ForEach-Object { "$($_.field) $($_.old) -> $($_.new)" }) -join "; "
        Stop-BuildDeps ("TOOLCHAIN DRIFT: recipe unchanged but toolchain identity changed ($differenceTable). " +
            "Pinned outputs cannot be reproduced. Run the controlled re-pin procedure " +
            "(see _ai/MSVC_OUTPUT_PIN_AND_REPIN_POLICY.md section 5) or restore the pinned toolchain.")
    }
    if (-not $Rebuild -and $disposition.status -eq "S1") {
        foreach ($output in @($component.outputs)) {
            Assert-StaticCrt $tools.dumpbin (Join-Path $script:RepoRoot $output.path) "manifold"
        }
        Add-ReportEvent "state" "W10" "CACHED" "Recipe fingerprint and output identities match; build skipped"
        Write-Output "PASS: W10 state and outputs verified; build skipped. Use -Rebuild to force compilation."
        return
    }
    Add-ReportEvent "state" "W10" "STALE" "W10 will be rebuilt: $($disposition.reason)"

    New-Item -ItemType Directory -Path $script:RunRoot, $script:WorkRoot -Force | Out-Null
    $stagingRoot = Join-Path $script:OutRoot "_staging\w10-$script:RunId"
    $stagingRoot = Assert-PathUnder $stagingRoot $script:OutRoot "W10 staging root"
    New-Item -ItemType Directory -Path (Join-Path $stagingRoot "lib"),
        (Join-Path $stagingRoot "licenses") -Force | Out-Null

    $sourceRoot = Expand-VerifiedArchive (Get-CachePath "manifold" $component) `
        (Join-Path $script:WorkRoot "manifold") $component "manifold"
    $buildRoot = Assert-PathUnder (Join-Path $script:OutRoot "_build\w10\manifold") $script:OutRoot `
        "components.manifold build directory"
    if (Test-Path -LiteralPath $buildRoot) { Remove-Item -LiteralPath $buildRoot -Recurse -Force }
    . (Join-Path $script:RepoRoot 'Clipper2Source.ps1')
    $clipper = Get-FalcoClipper2Bundle
    $upstreamDeps = [IO.File]::ReadAllText((Join-Path $sourceRoot 'cmake/manifoldDeps.cmake'))
    if ($upstreamDeps -notmatch ('GIT_TAG\s+' + $clipper.manifest.commit + '\b')) { Stop-BuildDeps 'Manifold Clipper2 commit differs from offline source' }
    $clipperSource = Join-Path $buildRoot 'clipper2-source'
    New-Item -ItemType Directory -Force -Path $clipperSource | Out-Null
    foreach ($name in $clipper.manifest.files.PSObject.Properties.Name) {
        $target = Join-Path $clipperSource $name
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item -LiteralPath (Join-Path $clipper.root $name) -Destination $target
        if ((Get-FileHash -LiteralPath $target).Hash -ne $clipper.manifest.files.$name.sha256) { Stop-BuildDeps 'Clipper2 staging identity mismatch' }
    }
    Invoke-CMakeBuild $cmake "manifold" $component $sourceRoot $buildRoot $tools.vcToolsVersion $stagingRoot @(
        "-DFETCHCONTENT_SOURCE_DIR_CLIPPER2=$clipperSource", '-DFETCHCONTENT_FULLY_DISCONNECTED=ON')
    Add-ReportEvent 'source' 'clipper2' 'PASS' 'Pinned offline source used; FetchContent networking disabled' @{commit=$clipper.manifest.commit; provenance=$clipper.sha256}
    $plan = Get-CMakeStagePlan "manifold" $sourceRoot $buildRoot $stagingRoot
    $null = Copy-StagedArtifacts $plan "manifold" $stagingRoot

    foreach ($output in @($component.outputs)) {
        $stagedLibrary = Get-StagedOutputPath ([string]$output.path) $stagingRoot "manifold" $wavePrefix
        if (-not (Test-Path -LiteralPath $stagedLibrary -PathType Leaf)) {
            Stop-BuildDeps "W10 expected output is missing before activation: $stagedLibrary"
        }
        Assert-StaticCrt $tools.dumpbin $stagedLibrary "manifold"
    }

    $previous = Switch-WavePrefix $stagingRoot $activeRoot
    $stateOutputs = @()
    foreach ($output in @($component.outputs)) {
        $path = Join-Path $script:RepoRoot ([string]$output.path)
        $identity = Get-FileIdentity $path
        $stateOutputs += [ordered]@{ path = [string]$output.path; bytes = $identity.bytes; sha256 = $identity.sha256 }
    }
    Write-WaveState "W10" $recipeFingerprint $toolchainFingerprint $toolchainIdentity $stateOutputs
    Add-ReportEvent "prefix" "W10" "PASS" "W10 staging prefix activated atomically" @{ active = $activeRoot; previous = $previous }
    Write-Output "PASS: W10 built with MSVC v143 x64 Release /MT through cmake (manifold) and activated at '$activeRoot'."
    $pinContext = "PINCTX: cl=$($toolchainIdentity.cl.version) link=$($toolchainIdentity.link.version)" +
        " lib=$($toolchainIdentity.lib.version) sdk=$($toolchainIdentity.windowsSdk.version)" +
        " cmake=$($toolchainIdentity.cmake.version) fp=$toolchainFingerprint"
    Write-Output $pinContext
    foreach ($output in $stateOutputs) {
        Write-Output "PIN: $($output.path) | $($output.bytes) bytes | $($output.sha256)"
    }
}

function Invoke-Build($Lock, [string[]]$Names) {
    if ($Wave -eq "W1") {
        Invoke-W1Build $Lock $Names
        return
    }
    if ($Wave -eq "W2") {
        Invoke-W2Build $Lock $Names
        return
    }
    if ($Wave -eq "W3") {
        Invoke-W3Build $Lock $Names
        return
    }
    if ($Wave -eq "W4") {
        Invoke-W4Build $Lock $Names
        return
    }
    if ($Wave -eq "W5") {
        Invoke-W5Build $Lock $Names
        return
    }
    if ($Wave -eq "W6") {
        Invoke-W6Build $Lock $Names
        return
    }
    if ($Wave -eq "W7") {
        Invoke-W7Build $Lock $Names
        return
    }
    if ($Wave -eq "W8") {
        Invoke-W8Build $Lock $Names
        return
    }
    if ($Wave -eq "W8b") {
        Invoke-W8bBuild $Lock $Names
        return
    }
    if ($Wave -eq "W9") {
        Invoke-W9Build $Lock $Names
        return
    }
    if ($Wave -eq "W10") {
        Invoke-W10Build $Lock $Names
        return
    }
    $unsupported = @($Names | Where-Object { $_ -ne "genie" })
    if ($unsupported.Count -gt 0) {
        Stop-BuildDeps "build recipes are not implemented yet for: $($unsupported -join ', ')"
    }
    if ($Names.Count -eq 0) { Stop-BuildDeps "no components selected for wave '$Wave'" }

    $component = $Lock.components.genie
    if ($component.wave -ne "W0" -or $component.buildProfile.system -ne "in-tree" -or
        "verify-only" -notin @($component.buildProfile.flags)) {
        Stop-BuildDeps "components.genie is not the approved W0 verify-only recipe"
    }
    $output = @($component.outputs)
    if ($output.Count -ne 1) { Stop-BuildDeps "components.genie must declare exactly one output" }
    $geniePath = Join-Path $script:RepoRoot $output[0].path
    if (-not (Test-FileIdentity $geniePath $output[0].bytes $output[0].sha256)) {
        Stop-BuildDeps "components.genie output identity mismatch: $($output[0].path)"
    }

    $bxRoot = Join-Path $script:RepoRoot "out\modern-bgfx\src\bx"
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if (-not $git) { Stop-BuildDeps "git.exe is required to verify W0 provenance" }
    $actualCommit = (& $git.Source -c "safe.directory=$bxRoot" -C $bxRoot rev-parse HEAD 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $Lock.components.bx.commit) {
        Stop-BuildDeps "W0 bx checkout commit mismatch: expected $($Lock.components.bx.commit), actual '$actualCommit'"
    }
    $relativeGenie = "tools/bin/windows/genie.exe"
    $trackedEntry = (& $git.Source -c "safe.directory=$bxRoot" -C $bxRoot ls-files -s -- $relativeGenie 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $trackedEntry) {
        Stop-BuildDeps "W0 GENie is not tracked by the pinned bx checkout"
    }
    $worktreeStatus = (& $git.Source -c "safe.directory=$bxRoot" -C $bxRoot status --short -- $relativeGenie 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $worktreeStatus) {
        Stop-BuildDeps "W0 GENie differs from the pinned bx Git object: '$worktreeStatus'"
    }

    if ($Rebuild) {
        Add-ReportEvent "build" "genie" "INFO" "-Rebuild has no effect for the approved verify-only W0 recipe"
    }
    Add-ReportEvent "build" "genie" "PASS" "Pinned official GENie prebuilt verified without execution" @{
        path = $output[0].path
        bytes = $output[0].bytes
        sha256 = $output[0].sha256
        bxCommit = $actualCommit
        gitIndexEntry = $trackedEntry
        genieSourceCommit = $component.commit
        license = "EXTERNAL-PENDING"
    }
    Write-Output "PASS: W0 GENie verified by SHA-256, size, bx commit and Git index; license source remains preparation-pending."
}

function Invoke-SelfTest {
    $selfRoot = Join-Path $script:OutRoot "_selftest\$script:RunId"
    $selfRoot = Assert-PathUnder $selfRoot (Join-Path $script:OutRoot "_selftest") "self-test root"
    New-Item -ItemType Directory -Path $selfRoot -Force | Out-Null
    $script:WorkRoot = Join-Path $selfRoot "work"
    $script:RunRoot = Join-Path $selfRoot "report"
    $script:CacheDirBeforeSelfTest = $CacheDir
    $script:CacheDir = Join-Path $selfRoot "cache"
    $script:StatePath = Join-Path $selfRoot "state\_state.json"
    New-Item -ItemType Directory -Path $script:WorkRoot, $script:RunRoot, $script:CacheDir -Force | Out-Null
    $results = New-Object System.Collections.Generic.List[object]

    function Run-Case([string]$Id, [scriptblock]$Body) {
        try {
            & $Body
            $results.Add([pscustomobject]@{ id = $Id; status = "PASS"; message = "" })
            Write-Output "PASS $Id"
        }
        catch {
            $results.Add([pscustomobject]@{ id = $Id; status = "FAIL"; message = $_.Exception.Message })
            Write-Output "FAIL $Id`: $($_.Exception.Message)"
        }
    }

    function Expect-Failure([scriptblock]$Body, [string]$Pattern) {
        $message = ""
        try { & $Body }
        catch { $message = $_.Exception.Message }
        if (-not $message) { throw "expected failure did not occur" }
        if ($message -notmatch $Pattern) { throw "unexpected failure: $message" }
    }

    function New-ToolchainFixture([int]$Variant = 0, [string]$InstallationPath = "C:\VS-A",
        [string]$CMakePath = "C:\CMake-A", [switch]$IncludeCMake) {
        $fixture = @{
            schemaVersion = 1
            toolsetFamily = "v143"
            arch = "x64"
            crt = "MT"
            config = "Release"
            vs = @{
                productDisplayVersion = "17.10.5"
                installationVersion = "17.10.35013.160"
                instanceId = "fixture-a"
                installationPath = $InstallationPath
            }
            vcTools = @{ version = "14.40.33807" }
            cl = @{ version = "19.40.33813"; bytes = 101; sha256 = ("A" * 64) }
            link = @{ version = "14.40.33813"; bytes = 102; sha256 = ("B" * 64) }
            lib = @{ version = "14.40.33813"; bytes = 103; sha256 = ("C" * 64) }
            dumpbin = @{ version = "14.40.33813"; bytes = 104; sha256 = ("D" * 64) }
            msbuild = @{ version = "17.10.4"; bytes = 105; sha256 = ("E" * 64) }
            windowsSdk = @{ version = "10.0.22621.0" }
        }
        if ($IncludeCMake) {
            $fixture["cmake"] = @{ version = "3.29.5"; path = $CMakePath }
        }
        switch ($Variant) {
            1 { $fixture.toolsetFamily = "v144" }
            2 { $fixture.arch = "arm64" }
            3 { $fixture.crt = "MD" }
            4 { $fixture.config = "Debug" }
            5 {
                $fixture.vs.productDisplayVersion = "17.11.0"
                $fixture.vs.installationVersion = "17.11.1.0"
            }
            6 { $fixture.vcTools.version = "14.41.0" }
            7 { $fixture.cl = @{ version = "19.41.0"; bytes = 201; sha256 = ("1" * 64) } }
            8 { $fixture.link = @{ version = "14.41.0"; bytes = 202; sha256 = ("2" * 64) } }
            9 { $fixture.lib = @{ version = "14.41.0"; bytes = 203; sha256 = ("3" * 64) } }
            10 { $fixture.dumpbin = @{ version = "14.41.0"; bytes = 204; sha256 = ("4" * 64) } }
            11 { $fixture.msbuild = @{ version = "17.11.0"; bytes = 205; sha256 = ("5" * 64) } }
            12 { $fixture.windowsSdk.version = "10.0.26100.0" }
        }
        return $fixture
    }

    function New-FixtureZip([string]$Path, [object[]]$Entries) {
        Add-Type -AssemblyName System.IO.Compression
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($item in $Entries) {
                $entry = $archive.CreateEntry($item.path)
                $writer = New-Object IO.StreamWriter($entry.Open())
                try { $writer.Write([string]$item.content) } finally { $writer.Dispose() }
            }
        }
        finally {
            $archive.Dispose()
        }
    }

    function Set-TarText([byte[]]$Header, [int]$Offset, [int]$Length, [string]$Text) {
        $bytes = [Text.Encoding]::ASCII.GetBytes($Text)
        if ($bytes.Length -gt $Length) { throw "tar fixture field is too long: $Text" }
        [Array]::Copy($bytes, 0, $Header, $Offset, $bytes.Length)
    }

    function Set-TarOctal([byte[]]$Header, [int]$Offset, [int]$Length, [long]$Value) {
        $text = [Convert]::ToString($Value, 8).PadLeft($Length - 1, '0') + [char]0
        Set-TarText $Header $Offset $Length $text
    }

    function New-FixtureTar([string]$Path, [object[]]$Entries) {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            foreach ($item in $Entries) {
                $header = New-Object byte[] 512
                $content = [Text.Encoding]::UTF8.GetBytes([string]$item.content)
                $type = [string]$item.type
                $size = if ($type -eq "0") { $content.Length } else { 0 }
                Set-TarText $header 0 100 ([string]$item.path)
                Set-TarOctal $header 100 8 511
                Set-TarOctal $header 108 8 0
                Set-TarOctal $header 116 8 0
                Set-TarOctal $header 124 12 $size
                Set-TarOctal $header 136 12 0
                for ($i = 148; $i -lt 156; $i++) { $header[$i] = 32 }
                $header[156] = [Text.Encoding]::ASCII.GetBytes($type)[0]
                if (@($item.PSObject.Properties.Name) -contains "link") {
                    Set-TarText $header 157 100 ([string]$item.link)
                }
                Set-TarText $header 257 6 ("ustar" + [char]0)
                Set-TarText $header 263 2 "00"
                Set-TarText $header 265 32 "selftest"
                Set-TarText $header 297 32 "selftest"
                $sum = 0L
                foreach ($value in $header) { $sum += $value }
                $checksum = [Convert]::ToString($sum, 8).PadLeft(6, '0') + [char]0 + " "
                Set-TarText $header 148 8 $checksum
                $stream.Write($header, 0, $header.Length)
                if ($size -gt 0) {
                    $stream.Write($content, 0, $content.Length)
                    $padding = (512 - ($content.Length % 512)) % 512
                    if ($padding -gt 0) { $stream.Write((New-Object byte[] $padding), 0, $padding) }
                }
            }
            $stream.Write((New-Object byte[] 1024), 0, 1024)
        }
        finally {
            $stream.Dispose()
        }
    }

    function New-MiniLock {
        $license = @([ordered]@{ pathInSource = "LICENSE"; sha256 = "TBD-on-approval"; license = "MIT" })
        $profile = [ordered]@{ system = "in-tree"; flags = @(); inherit = "toolchain" }
        return [ordered]@{
            '$schema' = "./deps.lock.schema.json"
            schemaVersion = 3
            approvedBy = "selftest"
            approvedAt = "2026-07-23"
            mode = "preparation"
            toolchain = [ordered]@{ vs = "[17.0,18.0)"; toolset = "v143"; arch = "x64"; crt = "MT"; config = "Release" }
            components = [ordered]@{
                a = [ordered]@{
                    kind = "in-tree"; wave = "W0"; upstream = "selftest:a"; version = "1"
                    licenseFiles = $license; patches = @(); buildProfile = $profile
                    outputs = @([ordered]@{ path = "out/deps/W0/a.lib"; sha256 = "TBD-first-build"; bytes = 0 })
                    dependsOn = @()
                }
                b = [ordered]@{
                    kind = "in-tree"; wave = "W1"; upstream = "selftest:b"; version = "1"
                    licenseFiles = $license; patches = @(); buildProfile = $profile
                    outputs = @([ordered]@{ path = "out/deps/W1/b.lib"; sha256 = "TBD-first-build"; bytes = 0 })
                    dependsOn = @("a")
                }
            }
        }
    }

    function Copy-JsonObject($Value) {
        return ($Value | ConvertTo-Json -Depth 30 | ConvertFrom-Json)
    }

    function Write-FixtureLock($Value, [string]$Name) {
        $path = Join-Path $selfRoot $Name
        [IO.File]::WriteAllText($path, ($Value | ConvertTo-Json -Depth 30), (New-Object Text.UTF8Encoding($false)))
        return $path
    }

    function Invoke-FixtureLockCheck([string]$Path, [string]$RequestedMode = "") {
        $validator = Join-Path $script:RepoRoot "TestDepsLock.ps1"
        $schema = Join-Path $script:RepoRoot "deps.lock.schema.json"
        $arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $validator, "-LockPath", $Path, "-SchemaPath", $schema, "-FixtureMode")
        if ($RequestedMode) { $arguments += @("-ValidationMode", $RequestedMode) }
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = & powershell @arguments 2>&1
            $exitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $previousPreference
        }
        return [pscustomobject]@{ exitCode = $exitCode; output = ($output | Out-String) }
    }

    Run-Case "T-TC-FINGERPRINT-STABLE" {
        $base = New-ToolchainFixture -IncludeCMake
        $reordered = @{}
        foreach ($name in @("cmake", "windowsSdk", "msbuild", "dumpbin", "lib", "link", "cl",
            "vcTools", "vs", "config", "crt", "arch", "toolsetFamily", "schemaVersion")) {
            $reordered[$name] = $base[$name]
        }
        $reordered["vs"] = @{
            installationPath = $base.vs.installationPath
            instanceId = $base.vs.instanceId
            installationVersion = $base.vs.installationVersion
            productDisplayVersion = $base.vs.productDisplayVersion
        }
        $identityA = Get-ToolchainIdentity -Override $base
        $identityB = Get-ToolchainIdentity -Override $reordered
        $fingerprintA = Get-ToolchainFingerprint $identityA
        if ($fingerprintA -ne (Get-ToolchainFingerprint $identityA) -or
            $fingerprintA -ne (Get-ToolchainFingerprint $identityB)) {
            throw "toolchain fingerprint is not stable"
        }
    }
    Run-Case "T-TC-FINGERPRINT-FIELD" {
        $baseIdentity = Get-ToolchainIdentity -Override (New-ToolchainFixture)
        $baseFingerprint = Get-ToolchainFingerprint $baseIdentity
        $changedFingerprints = New-Object "System.Collections.Generic.HashSet[string]"
        foreach ($variant in 1..12) {
            $changed = Get-ToolchainFingerprint (
                Get-ToolchainIdentity -Override (New-ToolchainFixture -Variant $variant)
            )
            if ($changed -eq $baseFingerprint) { throw "variant $variant did not change the fingerprint" }
            if (-not $changedFingerprints.Add($changed)) { throw "variant $variant duplicated a fingerprint" }
        }
        if ($changedFingerprints.Count -ne 12) { throw "expected 12 distinct changed fingerprints" }
        $sdkIdentity = Get-ToolchainIdentity -Override (New-ToolchainFixture -Variant 12)
        $sdkDifference = @(Compare-ToolchainIdentity $baseIdentity $sdkIdentity)
        if ($sdkDifference.Count -ne 1 -or $sdkDifference[0].field -ne "windowsSdk.version") {
            throw "toolchain comparison did not isolate the changed SDK field"
        }
    }
    Run-Case "T-TC-FINGERPRINT-PATHS" {
        $identityA = Get-ToolchainIdentity -Override (
            New-ToolchainFixture -InstallationPath "C:\VS-A" -CMakePath "C:\CMake-A" -IncludeCMake
        )
        $identityB = Get-ToolchainIdentity -Override (
            New-ToolchainFixture -InstallationPath "D:\VS-B" -CMakePath "D:\CMake-B" -IncludeCMake
        )
        if ((Get-ToolchainFingerprint $identityA) -ne (Get-ToolchainFingerprint $identityB)) {
            throw "non-normative paths changed the fingerprint"
        }
    }

    Run-Case "T-TC-TOOLPATH-SINGLE-SOURCE" {
        $fixturePaths = Get-MsvcToolPaths "X:\vsroot" "14.99.12345"
        $expectedBin = "X:\vsroot\VC\Tools\MSVC\14.99.12345\bin\Hostx64\x64"
        if ($fixturePaths.toolsBin -ne $expectedBin) { throw "tool bin directory was not derived from the pinned version" }
        foreach ($toolPath in @($fixturePaths.cl, $fixturePaths.link, $fixturePaths.lib, $fixturePaths.dumpbin)) {
            if ((Split-Path $toolPath -Parent) -ne $expectedBin) {
                throw "tool '$toolPath' is not derived from the single pinned MSVC directory"
            }
        }
        Expect-Failure { Get-MsvcToolPaths "X:\vsroot" "" } "tools version"
    }

    function New-SdkScanFixture([string]$Name) {
        # Reproduces, in miniature, the tree measured on 2026-07-28: engine projects and a deeply
        # nested vendored project carry the short version, CMake's generated projects under
        # out\deps\_build\ carry the resolved one, and GENie's generated projects under out\ carry
        # the short one. The fixture is only interesting because it holds both values.
        $root = Join-Path $selfRoot "sdk-scan\$Name"
        $files = [ordered]@{
            "Engine\Engine.vcxproj" = "10.0"
            "Bgfx\bgfx\build\projects\vs2019\bgfx.vcxproj" = "10.0"
            "out\modern-bgfx\src\bgfx\build\projects\vs2022\bx.vcxproj" = "10.0"
            "out\deps\_build\w2\zlib\zlibstatic.vcxproj" = "10.0.26100.0"
            "Bgfx\bimg\bimg.vcxproj" = "8.1"
        }
        foreach ($relative in @($files.Keys)) {
            $path = Join-Path $root $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
            $text = "<Project><PropertyGroup><WindowsTargetPlatformVersion>" +
                $files[$relative] +
                "</WindowsTargetPlatformVersion></PropertyGroup></Project>`n"
            [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding($false)))
        }
        return $root
    }

    Run-Case "T-SDK-SCAN-EXCLUDES-GENERATED" {
        # F-W2-43. Generated project trees must not reach the SDK probe: a successful dependency
        # build writes projects carrying a different version string, and without this filter the
        # build's own output made the next -Mode Check refuse to start.
        $root = New-SdkScanFixture "filtered"
        $scan = Get-ProjectPlatformVersions $root
        $versions = @($scan.versions)
        if ($versions.Count -ne 1) {
            throw "the filtered scan returned $($versions.Count) versions: $($versions -join ', ')"
        }
        if ($versions[0] -ne "10.0") { throw "the filtered scan selected '$($versions[0])'" }
        if ($scan.scannedCount -ne 2) { throw "expected two scanned projects, got $($scan.scannedCount)" }
        if ($scan.skippedCount -ne 3) { throw "expected three skipped projects, got $($scan.skippedCount)" }
        # The control half of the same case: the deeply nested vendored project is NOT generated
        # and must survive the filter, or the filter would be passing by throwing away too much.
        $kept = Join-Path $root "Bgfx\bgfx\build\projects\vs2019\bgfx.vcxproj"
        if (-not (Test-Path -LiteralPath $kept -PathType Leaf)) { throw "the fixture is incomplete" }
        $script:ProjectScanExcludedRoots | ForEach-Object {
            if ($_ -eq "Bgfx\bgfx") { throw "the vendored bgfx tree must not be excluded" }
        }
    }
    Run-Case "T-SDK-SCAN-CONTROL-UNFILTERED" {
        # Control pair (the lesson of CS07D): unless the unfiltered scan of the same fixture really
        # does return both values, T-SDK-SCAN-EXCLUDES-GENERATED would keep passing even if the
        # hazard it guards against had silently disappeared from the fixture.
        $root = New-SdkScanFixture "unfiltered"
        $previous = $script:ProjectScanExcludedRoots
        try {
            $script:ProjectScanExcludedRoots = @()
            $scan = Get-ProjectPlatformVersions $root
        }
        finally {
            $script:ProjectScanExcludedRoots = $previous
        }
        $versions = @($scan.versions)
        if ($versions.Count -ne 3) {
            throw "the unfiltered control returned $($versions.Count) versions: $($versions -join ', ')"
        }
        if (@($versions | Where-Object { $_ -eq "10.0.26100.0" }).Count -ne 1) {
            throw "the unfiltered control did not see the generated version"
        }
        if ($scan.skippedCount -ne 0) { throw "an empty exclusion list must skip nothing" }
        if ($script:ProjectScanExcludedRoots.Count -eq 0) { throw "the exclusion list was not restored" }
    }
    Run-Case "T-STATE-PATH-REDIRECT" {
        $redirected = Get-StatePath
        $null = Assert-PathUnder $redirected $selfRoot "redirected state path"
        $productionState = Join-Path $script:OutRoot "_state.json"
        $productionBefore = if (Test-Path -LiteralPath $productionState -PathType Leaf) {
            (Get-FileHash -LiteralPath $productionState -Algorithm SHA256).Hash
        } else { "" }
        Write-WaveState "WSELFTEST" ("f" * 64) ("e" * 64) `
            (Get-ToolchainIdentity -Override (New-ToolchainFixture)) @()
        $roundTrip = Get-WaveState "WSELFTEST"
        if ($null -eq $roundTrip -or [string]$roundTrip.recipeFingerprint -ne ("f" * 64)) {
            throw "state round-trip through the redirected path failed"
        }
        if (-not (Test-Path -LiteralPath $redirected -PathType Leaf)) {
            throw "redirected state file was not created under the self-test root"
        }
        $productionAfter = if (Test-Path -LiteralPath $productionState -PathType Leaf) {
            (Get-FileHash -LiteralPath $productionState -Algorithm SHA256).Hash
        } else { "" }
        if ($productionBefore -ne $productionAfter) {
            throw "production state file changed during a self-test state round-trip"
        }
    }

    Run-Case "T-STATE-PATH-CONTAINMENT" {
        $savedStatePath = $script:StatePath
        try {
            $script:StatePath = Join-Path $script:RepoRoot "outside\_state.json"
            Expect-Failure { Get-StatePath } "state file path"
        }
        finally {
            $script:StatePath = $savedStatePath
        }
    }

    Run-Case "T-TC-WAVEFP-COMPOSITE" {
        $recipe = "a" * 64
        $toolchain = "b" * 64
        $composite = Get-WaveFingerprint $recipe $toolchain
        if ($composite -ne (Get-WaveFingerprint $recipe $toolchain)) { throw "composite fingerprint is not stable" }
        if ($composite -eq (Get-WaveFingerprint $toolchain $recipe)) {
            throw "composite fingerprint ignores the order of its parts"
        }
        if ($composite -eq $recipe -or $composite -eq $toolchain) {
            throw "composite fingerprint equals one of its parts"
        }
        if ($composite -notmatch "^[A-F0-9]{64}$") { throw "composite fingerprint is not an upper-case SHA-256" }
        Expect-Failure { Get-WaveFingerprint "" $toolchain } "recipe fingerprint"
        Expect-Failure { Get-WaveFingerprint $recipe "zz" } "toolchain fingerprint"
    }

    Run-Case "T-TC-STATE-SCHEMA1-REJECT" {
        $savedStatePath = $script:StatePath
        try {
            $script:StatePath = Join-Path $selfRoot "state\schema1\_state.json"
            New-Item -ItemType Directory -Path (Split-Path -Parent $script:StatePath) -Force | Out-Null
            $legacy = [ordered]@{
                schemaVersion = 1
                waves = [ordered]@{
                    WLEGACY = [ordered]@{
                        fingerprint = ("c" * 64)
                        completedAt = "2026-01-01T00:00:00.0000000Z"
                        outputs = @()
                    }
                }
            }
            [IO.File]::WriteAllText((Get-StatePath), ($legacy | ConvertTo-Json -Depth 20),
                (New-Object Text.UTF8Encoding($false)))
            $eventsBefore = $script:ReportEvents.Count
            if ($null -ne (Get-WaveState "WLEGACY")) { throw "a schema 1 state record was accepted" }
            $recorded = @($script:ReportEvents | Select-Object -Skip $eventsBefore |
                Where-Object { $_.kind -eq "state" -and $_.component -eq "WLEGACY" -and $_.status -eq "STALE" })
            if ($recorded.Count -ne 1) { throw "schema 1 rejection did not record exactly one STALE event" }
            $disposition = Get-WaveStateDisposition "WLEGACY" ("a" * 64) ("b" * 64) `
                (Get-ToolchainIdentity -Override (New-ToolchainFixture))
            if ($disposition.status -ne "S3") { throw "a legacy state record did not degrade to a rebuild" }
        }
        finally {
            $script:StatePath = $savedStatePath
        }
    }

    Run-Case "T-TC-STATE-SCHEMA2-ROUNDTRIP" {
        $savedStatePath = $script:StatePath
        try {
            $script:StatePath = Join-Path $selfRoot "state\schema2\_state.json"
            $identity = Get-ToolchainIdentity -Override (New-ToolchainFixture)
            $recipe = "a" * 64
            $toolchain = Get-ToolchainFingerprint $identity
            Write-WaveState "WROUND" $recipe $toolchain $identity @(
                [ordered]@{ path = "out/deps/w1/lib/fixture.lib"; bytes = 1; sha256 = ("d" * 64) }
            )
            $raw = Get-Content -LiteralPath (Get-StatePath) -Raw | ConvertFrom-Json
            if ([int]$raw.schemaVersion -ne 2) { throw "state schema version 2 was not written" }
            $record = Get-WaveState "WROUND"
            if ($null -eq $record) { throw "a schema 2 state record was not readable" }
            if ([string]$record.recipeFingerprint -ne $recipe -or
                [string]$record.toolchainFingerprint -ne $toolchain) {
                throw "the split fingerprints were not preserved"
            }
            if ([string]$record.waveFingerprint -ne (Get-WaveFingerprint $recipe $toolchain)) {
                throw "the composite wave fingerprint was not derived from its parts"
            }
            if ([string]$record.toolchainIdentity.cl.version -ne [string]$identity.cl.version) {
                throw "the toolchain identity was not stored for diagnostics"
            }
            if (@($record.PSObject.Properties.Name) -contains "fingerprint") {
                throw "the schema 1 fingerprint field is still written"
            }
        }
        finally {
            $script:StatePath = $savedStatePath
        }
    }

    Run-Case "T-TC-DRIFT-DETECT" {
        $savedStatePath = $script:StatePath
        try {
            $script:StatePath = Join-Path $selfRoot "state\drift\_state.json"
            $identityA = Get-ToolchainIdentity -Override (New-ToolchainFixture)
            $identityB = Get-ToolchainIdentity -Override (New-ToolchainFixture -Variant 7)
            $recipe = "a" * 64
            Write-WaveState "WDRIFT" $recipe (Get-ToolchainFingerprint $identityA) $identityA @(
                [ordered]@{ path = "out/deps/w1/lib/fixture.lib"; bytes = 1; sha256 = ("d" * 64) }
            )
            $drifted = Get-WaveStateDisposition "WDRIFT" $recipe (Get-ToolchainFingerprint $identityB) $identityB
            if ($drifted.status -ne "S2") { throw "toolchain drift was not classified as S2" }
            $clDifference = @($drifted.differences | Where-Object { $_.field -eq "cl.version" })
            if ($clDifference.Count -ne 1) { throw "the drift report did not isolate the changed compiler version" }
            if ([string]$clDifference[0].old -ne "19.40.33813" -or
                [string]$clDifference[0].new -ne "19.41.0") {
                throw "the drift report is missing the old or the new compiler version"
            }
            $unchanged = Get-WaveStateDisposition "WDRIFT" $recipe (Get-ToolchainFingerprint $identityA) $identityA
            if ($unchanged.status -eq "S2") { throw "an unchanged toolchain was reported as drift" }
        }
        finally {
            $script:StatePath = $savedStatePath
        }
    }

    Run-Case "T-TC-STATE-RECIPE-DRIFT" {
        $savedStatePath = $script:StatePath
        try {
            $script:StatePath = Join-Path $selfRoot "state\recipe\_state.json"
            $identity = Get-ToolchainIdentity -Override (New-ToolchainFixture)
            $toolchain = Get-ToolchainFingerprint $identity
            Write-WaveState "WRECIPE" ("a" * 64) $toolchain $identity @()
            $disposition = Get-WaveStateDisposition "WRECIPE" ("e" * 64) $toolchain $identity
            if ($disposition.status -ne "S3") { throw "a recipe change must be classified as a plain rebuild" }
            if ([string]$disposition.reason -notmatch "recipe fingerprint") {
                throw "the recipe change reason was not reported"
            }
        }
        finally {
            $script:StatePath = $savedStatePath
        }
    }

    Run-Case "T-TC-STATE-MERGE-SCHEMA-EVENT" {
        $savedStatePath = $script:StatePath
        try {
            $script:StatePath = Join-Path $selfRoot "state\merge\_state.json"
            New-Item -ItemType Directory -Path (Split-Path -Parent $script:StatePath) -Force | Out-Null
            $legacy = [ordered]@{
                schemaVersion = 1
                waves = [ordered]@{
                    WOTHER = [ordered]@{
                        fingerprint = ("c" * 64)
                        completedAt = "2026-01-01T00:00:00.0000000Z"
                        outputs = @()
                    }
                }
            }
            [IO.File]::WriteAllText((Get-StatePath), ($legacy | ConvertTo-Json -Depth 20),
                (New-Object Text.UTF8Encoding($false)))
            $identity = Get-ToolchainIdentity -Override (New-ToolchainFixture)
            $eventsBefore = $script:ReportEvents.Count
            Write-WaveState "WNEW" ("a" * 64) (Get-ToolchainFingerprint $identity) $identity @()
            $recorded = @($script:ReportEvents | Select-Object -Skip $eventsBefore |
                Where-Object { $_.kind -eq "state" -and $_.component -eq "WNEW" -and $_.status -eq "STALE" })
            if ($recorded.Count -lt 1) { throw "replacing a foreign-schema state file was silent" }
            if ([string]$recorded[0].message -notmatch "not carried over") {
                throw "the replacement event does not state the consequence"
            }
            $written = Get-Content -LiteralPath (Get-StatePath) -Raw | ConvertFrom-Json
            if (@($written.waves.PSObject.Properties.Name) -notcontains "WNEW") {
                throw "the new wave record was not written"
            }
            if (@($written.waves.PSObject.Properties.Name) -contains "WOTHER") {
                throw "fixture expectation changed: a foreign wave survived the schema replacement"
            }
        }
        finally {
            $script:StatePath = $savedStatePath
        }
    }

    Run-Case "T-PATH-VALID" { Assert-RelativeSafePath "src/include/file.h" "fixture" }
    Run-Case "T-PATH-TRAVERSAL" { Expect-Failure { Assert-RelativeSafePath "..\evil.txt" "fixture" } "safe relative" }
    Run-Case "T-PATH-ABSOLUTE" { Expect-Failure { Assert-RelativeSafePath "C:\evil.txt" "fixture" } "safe relative" }
    Run-Case "T-ENTRY-SYMLINK" { Expect-Failure { Test-ArchiveEntry "safe/link" $true @() } "link entry" }
    Run-Case "T-ENTRY-EXE" { Expect-Failure { Test-ArchiveEntry "bin/payload.exe" $false @() } "unexpected executable" }
    Run-Case "T-ENTRY-ALLOWLIST" { Test-ArchiveEntry "bin/tool.exe" $false @("bin/tool.exe") }
    Run-Case "T-ZIP-VALID" {
        $zip = Join-Path $selfRoot "valid.zip"
        New-FixtureZip $zip @([pscustomobject]@{ path = "src/readme.txt"; content = "safe" })
        Test-ZipArchive $zip @()
    }
    Run-Case "T-ZIP-TRAVERSAL" {
        $zip = Join-Path $selfRoot "traversal.zip"
        New-FixtureZip $zip @([pscustomobject]@{ path = "../evil.txt"; content = "blocked" })
        Expect-Failure { Test-ZipArchive $zip @() } "safe relative"
        if (Test-Path -LiteralPath (Join-Path $selfRoot "evil.txt")) { throw "traversal file was created" }
    }
    Run-Case "T-ZIP-EXE" {
        $zip = Join-Path $selfRoot "payload.zip"
        New-FixtureZip $zip @([pscustomobject]@{ path = "bin/payload.exe"; content = "" })
        Expect-Failure { Test-ZipArchive $zip @() } "unexpected executable"
    }
    Run-Case "T-TAR-VALID" {
        $tarPath = Join-Path $selfRoot "valid.tar"
        New-FixtureTar $tarPath @([pscustomobject]@{ path = "src/readme.txt"; type = "0"; content = "safe" })
        Test-TarArchive $tarPath @()
    }
    Run-Case "T-TAR-TRAVERSAL" {
        $tarPath = Join-Path $selfRoot "traversal.tar"
        New-FixtureTar $tarPath @([pscustomobject]@{ path = "../evil.txt"; type = "0"; content = "blocked" })
        Expect-Failure { Test-TarArchive $tarPath @() } "safe relative|tar listing failed"
    }
    Run-Case "T-TAR-SYMLINK" {
        $tarPath = Join-Path $selfRoot "symlink.tar"
        New-FixtureTar $tarPath @([pscustomobject]@{ path = "safe/link"; type = "2"; link = "../../evil"; content = "" })
        Expect-Failure { Test-TarArchive $tarPath @() } "link entry"
    }
    Run-Case "T-TAR-HARDLINK" {
        $tarPath = Join-Path $selfRoot "hardlink.tar"
        New-FixtureTar $tarPath @([pscustomobject]@{ path = "safe/hardlink"; type = "1"; link = "../outside"; content = "" })
        Expect-Failure { Test-TarArchive $tarPath @() } "link entry"
    }
    Run-Case "T-TAR-EXCLUDED-PREFIX" {
        # W4 / DEC-038. An excluded prefix silences the guard for its own subtree and nothing
        # else: the same link entry outside the prefix must still stop the wave.
        if (-not (Test-ArchiveEntryExcluded "SDL-release-2.0.12/android-project-ant/src" `
                @("SDL-release-2.0.12/android-project-ant/"))) {
            throw "an entry under the excluded prefix was not recognised as excluded"
        }
        if (Test-ArchiveEntryExcluded "SDL-release-2.0.12/src/SDL.c" `
                @("SDL-release-2.0.12/android-project-ant/")) {
            throw "an entry outside the excluded prefix was wrongly excluded"
        }
        if (Test-ArchiveEntryExcluded "SDL-release-2.0.12/src/SDL.c" @()) {
            throw "an empty exclusion list must exclude nothing"
        }
        $sdl2Prefixes = @(Get-ArchiveExcludedPrefixes "sdl2")
        if (@($sdl2Prefixes).Count -ne 1 -or $sdl2Prefixes[0] -ne "SDL-release-2.0.12/android-project-ant/") {
            throw "the sdl2 exclusion table must name exactly the Android packaging directory"
        }
        if (@(Get-ArchiveExcludedPrefixes "zlib").Count -ne 0) {
            throw "components without declared exclusions must get an empty list"
        }
    }
    Run-Case "T-SHA-TBD" { Expect-Failure { Assert-PinnedSha256 "TBD-on-approval" "fixture" } "production SHA-256" }
    Run-Case "T-SHA-BAD" { Expect-Failure { Assert-PinnedSha256 ("A" * 63) "fixture" } "production SHA-256" }
    $validMiniLock = New-MiniLock
    Run-Case "T-LOCK-VALID" {
        $path = Write-FixtureLock $validMiniLock "lock-valid.json"
        $check = Invoke-FixtureLockCheck $path
        if ($check.exitCode -ne 0) { throw "valid fixture rejected: $($check.output)" }
    }
    Run-Case "T-LOCK-TBD-PRODUCTION" {
        $path = Write-FixtureLock $validMiniLock "lock-production-tbd.json"
        $check = Invoke-FixtureLockCheck $path "Production"
        if ($check.exitCode -eq 0) { throw "production fixture with TBD unexpectedly passed" }
    }
    Run-Case "T-LOCK-BAD-SHA" {
        $lock = Copy-JsonObject $validMiniLock
        $lock.components.a.outputs[0].sha256 = "bad"
        $check = Invoke-FixtureLockCheck (Write-FixtureLock $lock "lock-bad-sha.json")
        if ($check.exitCode -eq 0) { throw "bad SHA fixture unexpectedly passed" }
    }
    Run-Case "T-LOCK-CYCLE" {
        $lock = Copy-JsonObject $validMiniLock
        $lock.components.a.dependsOn = @("b")
        $check = Invoke-FixtureLockCheck (Write-FixtureLock $lock "lock-cycle.json")
        if ($check.exitCode -eq 0) { throw "cycle fixture unexpectedly passed" }
    }
    Run-Case "T-LOCK-DUPLICATE-OUTPUT" {
        $lock = Copy-JsonObject $validMiniLock
        $lock.components.b.outputs[0].path = $lock.components.a.outputs[0].path
        $check = Invoke-FixtureLockCheck (Write-FixtureLock $lock "lock-duplicate-output.json")
        if ($check.exitCode -eq 0) { throw "duplicate output fixture unexpectedly passed" }
    }

    $payload = Join-Path $selfRoot "payload.bin"
    [IO.File]::WriteAllBytes($payload, [byte[]](1, 2, 3, 4, 5))
    $payloadIdentity = Get-FileIdentity $payload
    Run-Case "T-HASH-VALID" {
        if (-not (Test-FileIdentity $payload $payloadIdentity.bytes $payloadIdentity.sha256)) { throw "valid payload rejected" }
    }
    Run-Case "T-HASH-SIZE" {
        if (Test-FileIdentity $payload ($payloadIdentity.bytes + 1) $payloadIdentity.sha256) { throw "wrong size accepted" }
    }
    Run-Case "T-EXPAND-IDENTITY" {
        $archive = Join-Path $script:CacheDir "expand-valid.zip"
        New-FixtureZip $archive @([pscustomobject]@{ path = "src/readme.txt"; content = "safe" })
        $identity = Get-FileIdentity $archive
        $component = [pscustomobject]@{
            archive = [pscustomobject]@{ bytes = $identity.bytes; sha256 = $identity.sha256 }
            expectedExecutables = @()
            licenseFiles = @([pscustomobject]@{ pathInSource = "OPTIONAL-LICENSE"; sha256 = "TBD-on-approval"; license = "MIT"; optional = $true })
            patches = @()
        }
        $destination = Join-Path $script:WorkRoot "expand-valid"
        Expand-VerifiedArchive $archive $destination $component "fixture" | Out-Null
        if (-not (Test-Path -LiteralPath (Join-Path $destination "src\readme.txt") -PathType Leaf)) { throw "verified archive was not extracted" }
    }
    Run-Case "T-EXPAND-TOCTOU" {
        $archive = Join-Path $script:CacheDir "expand-poisoned.zip"
        New-FixtureZip $archive @([pscustomobject]@{ path = "src/readme.txt"; content = "expected" })
        $identity = Get-FileIdentity $archive
        [IO.File]::AppendAllText($archive, "tampered")
        $component = [pscustomobject]@{
            archive = [pscustomobject]@{ bytes = $identity.bytes; sha256 = $identity.sha256 }
            expectedExecutables = @()
            licenseFiles = @()
            patches = @()
        }
        $destination = Join-Path $script:WorkRoot "expand-poisoned"
        Expect-Failure { Expand-VerifiedArchive $archive $destination $component "fixture" } "changed after Fetch"
        if (Test-Path -LiteralPath $destination) { throw "destination was created before identity rejection" }
        if (-not (Get-ChildItem -LiteralPath $script:CacheDir -Filter "expand-poisoned.zip.quarantine-*" | Select-Object -First 1)) { throw "TOCTOU archive quarantine evidence missing" }
    }
    Run-Case "T-AUTHENTICODE-POLICY" {
        if ((Get-AuthenticodeDisposition "Valid" "fixture") -ne "PASS") { throw "Valid signature policy mismatch" }
        if ((Get-AuthenticodeDisposition "NotSigned" "fixture") -ne "WARN") { throw "NotSigned signature policy mismatch" }
        if ((Get-AuthenticodeDisposition "UnknownError" "fixture") -ne "WARN") { throw "UnknownError signature policy mismatch" }
        Expect-Failure { Get-AuthenticodeDisposition "HashMismatch" "fixture" } "refusing"
        Expect-Failure { Get-AuthenticodeDisposition "NotTrusted" "fixture" } "refusing"
    }
    Run-Case "T-QUARANTINE" {
        $poisoned = Join-Path $script:CacheDir "poisoned.bin"
        [IO.File]::WriteAllBytes($poisoned, [byte[]](9, 9, 9))
        if (Test-FileIdentity $poisoned $payloadIdentity.bytes $payloadIdentity.sha256 -QuarantineOnFailure) { throw "poisoned cache accepted" }
        if (Test-Path -LiteralPath $poisoned) { throw "poisoned cache remained active" }
        if (-not (Get-ChildItem -LiteralPath $script:CacheDir -Filter "poisoned.bin.quarantine-*" | Select-Object -First 1)) { throw "quarantine evidence missing" }
    }
    Run-Case "T-PATCH-HASH" {
        $patchPath = Join-Path $selfRoot "fixture.patch"
        [IO.File]::WriteAllText($patchPath, "fixture patch")
        $patchSha = (Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash
        $relativePatch = $patchPath.Substring((Get-NormalizedFullPath $script:RepoRoot).Length).TrimStart('\', '/')
        $component = [pscustomobject]@{
            patches = @([pscustomobject]@{ file = $relativePatch; sha256 = $patchSha; reason = "self-test" })
        }
        Assert-PatchFiles $component "fixture"
        $component.patches[0].sha256 = "0" * 64
        Expect-Failure { Assert-PatchFiles $component "fixture" } "SHA-256 mismatch"
    }
    Run-Case "T-EXEC-INVENTORY" {
        $tree = Join-Path $selfRoot "inventory"
        New-Item -ItemType Directory -Path (Join-Path $tree "bin") -Force | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $tree "bin\tool.exe"), [byte[]]@())
        Assert-ExpectedExecutableInventory $tree @("bin/tool.exe") "fixture"
        Expect-Failure { Assert-ExpectedExecutableInventory $tree @() "fixture" } "unexpected executable"
    }
    Run-Case "T-FRESH-NONEMPTY" {
        $notEmpty = Join-Path $script:WorkRoot "not-empty"
        New-Item -ItemType Directory -Path $notEmpty -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $notEmpty "marker"), "x")
        Expect-Failure { Assert-FreshEmptyDirectory $notEmpty } "not empty"
    }
    Run-Case "T-MSBUILD-DETERMINISM-PROPS" {
        # F-W2-64 / DEC-071: the msbuild wave carries /Brepro through a driver-generated
        # Directory.Build.props; an upstream tree that already has one is a stop, not a merge.
        $root = Join-Path $script:WorkRoot "msbuild-props"
        New-Item -ItemType Directory -Path $root -Force | Out-Null
        $path = Write-MsBuildDeterminismProps $root "fixture"
        $content = [IO.File]::ReadAllText($path)
        foreach ($group in @("<ClCompile>", "<Lib>")) {
            if (-not $content.Contains($group)) { throw "props file lacks the $group group" }
        }
        if (([regex]::Matches($content, [regex]::Escape("/Brepro %(AdditionalOptions)"))).Count -ne 2) {
            throw "props file must carry /Brepro exactly twice, additively"
        }
        $xml = [xml]$content
        if ($null -eq $xml.SelectSingleNode("/Project/ItemDefinitionGroup")) {
            throw "props file is not a well-formed msbuild project"
        }
        Expect-Failure { Write-MsBuildDeterminismProps $root "fixture" } "already carries"
    }
    Run-Case "T-W3-RESOURCE-CONVERTER-FINGERPRINT" {
        $converterRoot = Join-Path $script:WorkRoot 'converter-fingerprint'
        New-Item -ItemType Directory -Path $converterRoot -Force | Out-Null
        $converter = Join-Path $converterRoot 'cvtres.exe'
        $toolsFixture = [pscustomobject]@{ lib = (Join-Path $converterRoot 'lib.exe') }
        $lockFixture = [pscustomobject]@{ components = [pscustomobject]@{
            freetype = [pscustomobject]@{ patches = @() }
            freeimage = [pscustomobject]@{ patches = @() }
        } }
        [IO.File]::WriteAllText($converter, 'first converter fixture')
        $before = Get-W3RecipeFingerprint $lockFixture $toolsFixture
        [IO.File]::WriteAllText($converter, 'changed converter fixture')
        $after = Get-W3RecipeFingerprint $lockFixture $toolsFixture
        if ($before -eq $after) { throw 'Converter changes must invalidate W3 cache identity' }
    }
    Run-Case "T-W6-RESOURCE-CONVERTER-FINGERPRINT" {
        $converterRoot = Join-Path $script:WorkRoot 'converter-fingerprint-w6'
        New-Item -ItemType Directory -Path $converterRoot -Force | Out-Null
        $converter = Join-Path $converterRoot 'cvtres.exe'
        $toolsFixture = [pscustomobject]@{ lib = (Join-Path $converterRoot 'lib.exe') }
        $lockFixture = [pscustomobject]@{ components = [pscustomobject]@{
            assimp = [pscustomobject]@{ patches = @() }
        } }
        [IO.File]::WriteAllText($converter, 'first converter fixture')
        $before = Get-W6RecipeFingerprint $lockFixture $toolsFixture
        [IO.File]::WriteAllText($converter, 'changed converter fixture')
        $after = Get-W6RecipeFingerprint $lockFixture $toolsFixture
        if ($before -eq $after) { throw 'Converter changes must invalidate W6 cache identity' }
    }
    Run-Case "T-SDL2-HIDAPI-INCLUDE-REPAIR-GATE" {
        $sdlRoot = Join-Path $script:WorkRoot 'sdl include fixture'
        New-Item -ItemType Directory -Path (Join-Path $sdlRoot 'cmake') -Force | Out-Null
        $sdlRecipe = Join-Path $sdlRoot 'cmake\sdlchecks.cmake'
        [IO.File]::WriteAllText($sdlRecipe, 'unrecognized recipe')
        Expect-Failure { Repair-Sdl2HidapiInclude $sdlRoot } 'Unexpected SDL2'
        if ([IO.File]::ReadAllText($sdlRecipe) -ne 'unrecognized recipe') { throw 'Unrecognized source must remain unchanged' }
        $line = 'set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${LIBUSB_CFLAGS} -I${SDL2_SOURCE_DIR}/src/hidapi/hidapi")'
        [IO.File]::WriteAllText($sdlRecipe, $line + "`n" + $line)
        Expect-Failure { Repair-Sdl2HidapiInclude $sdlRoot } 'Unexpected SDL2'
        if ([IO.File]::ReadAllText($sdlRecipe) -ne ($line + "`n" + $line)) { throw 'Ambiguous source must remain unchanged' }
    }
    Run-Case "T-WORK-SCRATCH-RESET" {
        # F-W2-63 / DEC-068: the work root is run-invariant, so an extraction destination may
        # hold a previous run's tree. Reset-WorkScratch clears it, refuses anything outside
        # the work root, and Expand-VerifiedArchive wipes before the emptiness gate.
        $scratch = Join-Path $script:WorkRoot "reset-fixture"
        New-Item -ItemType Directory -Path (Join-Path $scratch "nested") -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $scratch "nested\leftover.txt"), "stale")
        $null = Reset-WorkScratch $scratch
        if (Test-Path -LiteralPath $scratch) { throw "leftover tree survived Reset-WorkScratch" }
        $null = Reset-WorkScratch $scratch
        Expect-Failure { Reset-WorkScratch (Join-Path $script:OutRoot "w2") } "work scratch"
        $archive = Join-Path $script:CacheDir "expand-over-leftover.zip"
        New-FixtureZip $archive @([pscustomobject]@{ path = "src/readme.txt"; content = "fresh" })
        $identity = Get-FileIdentity $archive
        $component = [pscustomobject]@{
            archive = [pscustomobject]@{ bytes = $identity.bytes; sha256 = $identity.sha256 }
            expectedExecutables = @()
            licenseFiles = @()
            patches = @()
        }
        $destination = Join-Path $script:WorkRoot "expand-over-leftover"
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $destination "leftover.txt"), "previous run")
        Expand-VerifiedArchive $archive $destination $component "fixture" | Out-Null
        if (Test-Path -LiteralPath (Join-Path $destination "leftover.txt")) {
            throw "previous run's leftover survived extraction"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $destination "src\readme.txt") -PathType Leaf)) {
            throw "archive was not extracted over the leftover"
        }
    }
    Run-Case "T-DAG-ORDER" {
        $components = [pscustomobject]@{
            a = [pscustomobject]@{ dependsOn = @() }
            b = [pscustomobject]@{ dependsOn = @("a") }
        }
        $order = @(Get-TopologicalOrder $components @("a", "b"))
        if (($order -join ",") -ne "a,b") { throw "unexpected order: $($order -join ',')" }
    }
    Run-Case "T-DAG-CYCLE" {
        $components = [pscustomobject]@{
            a = [pscustomobject]@{ dependsOn = @("b") }
            b = [pscustomobject]@{ dependsOn = @("a") }
        }
        Expect-Failure { Get-TopologicalOrder $components @("a", "b") } "cycle"
    }
    Run-Case "T-NETWORK-DENY" {
        Expect-Failure { Assert-NetworkAllowed @("fixture") } "network is disabled"
    }
    Run-Case "T-NETWORK-AST" {
        $tokens = $null
        $parseErrors = $null
        $ast = [Management.Automation.Language.Parser]::ParseFile($PSCommandPath, [ref]$tokens, [ref]$parseErrors)
        if (@($parseErrors).Count -ne 0) { throw "BuildDeps.ps1 AST parse errors: $($parseErrors -join '; ')" }
        $networkCalls = @($ast.FindAll({
            param($node)
            $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq "Invoke-WebRequest"
        }, $true))
        if ($networkCalls.Count -ne 1) { throw "expected exactly one Invoke-WebRequest call, found $($networkCalls.Count)" }
        $owner = $networkCalls[0].Parent
        while ($null -ne $owner -and $owner -isnot [Management.Automation.Language.FunctionDefinitionAst]) { $owner = $owner.Parent }
        if ($null -eq $owner -or $owner.Name -ne "Invoke-Fetch") {
            throw "Invoke-WebRequest exists outside Invoke-Fetch"
        }
    }
    Run-Case "T-ATOMIC-SWITCH" {
        $active = Join-Path $selfRoot "prefix\W0"
        $staging = Join-Path $selfRoot "prefix\W0-staging"
        New-Item -ItemType Directory -Path $active, $staging -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $active "marker"), "old")
        [IO.File]::WriteAllText((Join-Path $staging "marker"), "new")
        $previous = Switch-WavePrefix $staging $active
        if ((Get-Content -LiteralPath (Join-Path $active "marker") -Raw) -ne "new") { throw "new prefix is not active" }
        if (-not $previous -or (Get-Content -LiteralPath (Join-Path $previous "marker") -Raw) -ne "old") { throw "rollback prefix missing" }
    }
    Run-Case "T-ATOMIC-ROLLBACK" {
        $active = Join-Path $selfRoot "rollback\W1"
        $staging = Join-Path $selfRoot "rollback\W1-staging"
        New-Item -ItemType Directory -Path $active, $staging -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $active "marker"), "old")
        $lockedMarker = Join-Path $staging "marker"
        [IO.File]::WriteAllText($lockedMarker, "new")
        $handle = [IO.File]::Open($lockedMarker, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        try {
            Expect-Failure { Switch-WavePrefix $staging $active } "atomic prefix switch failed"
        }
        finally {
            $handle.Dispose()
        }
        if (-not (Test-Path -LiteralPath $active -PathType Container)) { throw "old prefix was not restored" }
        if ((Get-Content -LiteralPath (Join-Path $active "marker") -Raw) -ne "old") { throw "restored prefix contains wrong marker" }
        if (-not (Test-Path -LiteralPath $staging -PathType Container)) { throw "failed staging was lost" }
    }

    function New-CMakeRecipeFixture([string]$ComponentId = "zlib", [string]$System = "cmake",
        [string]$OutputPath = "out/deps/w2/lib/zs.lib", [string[]]$ExtraFlags = @()) {
        $flags = @("-DZLIB_BUILD_STATIC=ON", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded") + $ExtraFlags
        $recipe = [ordered]@{
            kind = "archive"
            wave = "W2"
            version = "1.3.2"
            licenseFiles = @(@{ pathInSource = "LICENSE"; sha256 = ("a" * 64); license = "Zlib" })
            patches = @()
            buildProfile = [ordered]@{ system = $System; flags = $flags; inherit = "toolchain" }
            outputs = @(@{ path = $OutputPath; sha256 = "TBD-first-build"; bytes = 0 })
            dependsOn = @()
        }
        return ($recipe | ConvertTo-Json -Depth 20 | ConvertFrom-Json)
    }

    function New-ClRecipeFixture([string]$System = "cl", [string]$Kind = "in-tree",
        [string]$OutputPath = "out/deps/w2/lib/bc7enc.lib", [string[]]$Flags = @(), [switch]$WithPatch) {
        $recipeFlags = @($Flags)
        if ($recipeFlags.Count -eq 0) {
            $recipeFlags = @("/TC", "/O2", "/Ob2", "/DNDEBUG", "/MT", "/GS", "/Gy", "/Zc:inline")
        }
        $recipePatches = @()
        if ($WithPatch) {
            $recipePatches = @(@{ file = "patches/bc7enc/001-example.patch"; sha256 = ("e" * 64) })
        }
        $recipe = [ordered]@{
            kind = $Kind
            wave = "W2"
            version = "in-tree-approved"
            licenseFiles = @(@{ pathInSource = "bc7enc16.c"; sha256 = ("d" * 64); license = "MIT OR Unlicense" })
            patches = $recipePatches
            buildProfile = [ordered]@{ system = $System; inherit = "toolchain"; flags = $recipeFlags }
            outputs = @(@{ path = $OutputPath; sha256 = "TBD-first-build"; bytes = 0 })
            dependsOn = @()
        }
        return ($recipe | ConvertTo-Json -Depth 20 | ConvertFrom-Json)
    }

    function New-W2FixtureLock([string]$Bc7encSystem = "cl",
        [string[]]$Bc7encFlags = @("/O2", "/MT", "/EHsc")) {
        # A three-component W2 fixture with the real shape of deps.lock.json: two cmake
        # recipes and bc7enc, which the cl driver builds out of the engine tree.
        # SAFETY: the default bc7enc flags are deliberately the pre-CS-07 set, which the cl
        # recipe gate rejects (no /TC, and /EHsc is meaningless in a /TC unit). Every
        # Invoke-W2Build call in this SelfTest therefore fails at the selection check, the
        # pre-flight or the recipe gate - all of which run before Get-Vs2022BuildTools - so no
        # self-test can resolve real tooling and start a real build on the machine.
        $crt = "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
        $lock = [ordered]@{
            toolchain = [ordered]@{ vs = "[17.0,18.0)" }
            components = [ordered]@{
                zlib = [ordered]@{
                    kind = "archive"; wave = "W2"; version = "1.3.2"
                    licenseFiles = @(@{ pathInSource = "LICENSE"; sha256 = ("a" * 64); license = "Zlib" })
                    patches = @()
                    buildProfile = [ordered]@{
                        system = "cmake"; inherit = "toolchain"
                        flags = @("-DZLIB_BUILD_STATIC=ON", $crt)
                    }
                    outputs = @(@{ path = "out/deps/w2/lib/zs.lib"; sha256 = "TBD-first-build"; bytes = 0 })
                    dependsOn = @()
                }
                libzip = [ordered]@{
                    kind = "archive"; wave = "W2"; version = "1.11.4"
                    licenseFiles = @(@{ pathInSource = "LICENSE"; sha256 = ("b" * 64); license = "BSD-3-Clause" })
                    patches = @(@{ file = "patches/libzip/001-rename-core-entrypoints.patch"; sha256 = ("c" * 64) })
                    buildProfile = [ordered]@{
                        system = "cmake"; inherit = "toolchain"
                        flags = @('-DZLIB_INCLUDE_DIR=${REPO_ROOT}/out/deps/w2/include', $crt)
                    }
                    outputs = @(@{ path = "out/deps/w2/lib/zip.lib"; sha256 = "TBD-first-build"; bytes = 0 })
                    dependsOn = @("zlib")
                }
                bc7enc = [ordered]@{
                    kind = "in-tree"; wave = "W2"; version = "2021-12-27"
                    licenseFiles = @(@{ pathInSource = "LICENSE"; sha256 = ("d" * 64); license = "MIT" })
                    patches = @()
                    buildProfile = [ordered]@{
                        system = $Bc7encSystem; inherit = "toolchain"
                        flags = @($Bc7encFlags)
                    }
                    outputs = @(@{ path = "out/deps/w2/lib/bc7enc.lib"; sha256 = "TBD-first-build"; bytes = 0 })
                    dependsOn = @()
                }
            }
        }
        return ($lock | ConvertTo-Json -Depth 20 | ConvertFrom-Json)
    }

    Run-Case "T-CMAKE-TOKEN-EXPAND" {
        $expanded = Expand-RecipeToken '-DZLIB_INCLUDE_DIR=${REPO_ROOT}/out/deps/w2/include' "fixture"
        $expectedRoot = $script:RepoRoot.Replace('\', '/').TrimEnd('/')
        if ($expanded -ne "-DZLIB_INCLUDE_DIR=$expectedRoot/out/deps/w2/include") {
            throw "token was not expanded to the repository root: '$expanded'"
        }
        if ($expanded -match '\$\{') { throw "expanded flag still carries a token" }
        if ($expanded.Contains('\')) { throw "expanded flag must use forward slashes for cmake" }
    }
    Run-Case "T-CMAKE-TOKEN-RESIDUAL" {
        Expect-Failure { Expand-RecipeToken '-DX=${UNKNOWN_TOKEN}/y' "fixture" } "unexpanded token"
        Expect-Failure { Expand-RecipeToken '-DX=${REPO_ROOT}/a/${OTHER}' "fixture" } "unexpanded token"
    }
    Run-Case "T-CMAKE-TOKEN-STAGING" {
        # F-W2-30. During a build the W2 prefix is the staging tree, because Switch-WavePrefix
        # activates out/deps/w2 only after the whole wave has succeeded.
        $staging = "D:\repo\out\deps\_staging\w2-run"
        $expectedStaging = "D:/repo/out/deps/_staging/w2-run"
        $rootForCMake = $script:RepoRoot.Replace('\', '/').TrimEnd('/')
        $include = Expand-RecipeToken '-DZLIB_INCLUDE_DIR=${REPO_ROOT}/out/deps/w2/include' "fixture" $staging
        if ($include -ne "-DZLIB_INCLUDE_DIR=$expectedStaging/include") {
            throw "the W2 prefix was not redirected to the staging tree: '$include'"
        }
        $library = Expand-RecipeToken '-DZLIB_LIBRARY=${REPO_ROOT}/out/deps/w2/lib/zs.lib' "fixture" $staging
        if ($library -ne "-DZLIB_LIBRARY=$expectedStaging/lib/zs.lib") {
            throw "the staged library path was not redirected: '$library'"
        }
        # Whole path segments only: a sibling directory whose name merely starts with 'w2'
        # must survive untouched.
        $sibling = Expand-RecipeToken '-DX=${REPO_ROOT}/out/deps/w2-old/include' "fixture" $staging
        if ($sibling -ne "-DX=$rootForCMake/out/deps/w2-old/include") {
            throw "a sibling prefix was rewritten: '$sibling'"
        }
        # Without a staging root the expansion must keep the recorded final location.
        $plain = Expand-RecipeToken '-DZLIB_INCLUDE_DIR=${REPO_ROOT}/out/deps/w2/include' "fixture"
        if ($plain -ne "-DZLIB_INCLUDE_DIR=$rootForCMake/out/deps/w2/include") {
            throw "expansion without a staging root must not be redirected: '$plain'"
        }
    }
    Run-Case "T-CMAKE-FLAGS-EXPAND" {
        $flags = Expand-RecipeFlags @(
            '-DZLIB_INCLUDE_DIR=${REPO_ROOT}/out/deps/w2/include',
            '-DZLIB_LIBRARY=${REPO_ROOT}/out/deps/w2/lib/zs.lib',
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
        ) "libzip"
        if (@($flags).Count -ne 3) { throw "flag count changed during expansion" }
        foreach ($flag in $flags) {
            if ($flag -match '\$\{') { throw "flag '$flag' still carries a token" }
        }
        if ($flags[2] -ne "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded") { throw "a token-free flag was altered" }
    }
    Run-Case "T-CMAKE-RECIPE-GATE" {
        Assert-CMakeRecipe (New-CMakeRecipeFixture) "zlib"
        Expect-Failure { Assert-CMakeRecipe (New-CMakeRecipeFixture -System "msbuild") "zlib" } "not a cmake recipe"
        Expect-Failure {
            Assert-CMakeRecipe (New-CMakeRecipeFixture -ExtraFlags @("-DBUILD_SHARED_LIBS=ON")) "zlib"
        } "static-only wave"
    }
    Run-Case "T-CMAKE-RECIPE-CRT" {
        $recipe = New-CMakeRecipeFixture
        $recipe.buildProfile.flags = @("-DZLIB_BUILD_STATIC=ON")
        Expect-Failure { Assert-CMakeRecipe $recipe "zlib" } "CMAKE_MSVC_RUNTIME_LIBRARY"
    }
    Run-Case "T-CMAKE-DETERMINISM-FLAGS" {
        # F-W2-62 / DEC-067: every cmake wave must carry /Brepro for both the compiler and the
        # librarian, or a rebuild of an unchanged recipe cannot reproduce its pinned bytes.
        $arguments = @(Get-CMakeDeterminismArguments)
        if ($arguments.Count -ne 1) {
            throw "expected exactly one determinism configure argument, found $($arguments.Count)"
        }
        if ($arguments[0] -ne "-DCMAKE_STATIC_LINKER_FLAGS_RELEASE=/Brepro") {
            throw "librarian determinism argument changed: '$($arguments[0])'"
        }
        $environment = Get-CMakeDeterminismEnvironment
        $expectedNames = @("CFLAGS", "CXXFLAGS", "LDFLAGS")
        $actualNames = @($environment.Keys)
        if (($actualNames -join ",") -ne ($expectedNames -join ",")) {
            throw "determinism environment names changed: $($actualNames -join ', ')"
        }
        foreach ($name in $expectedNames) {
            if ([string]$environment[$name] -ne "/Brepro") {
                throw "determinism environment $name must be /Brepro, found '$($environment[$name])'"
            }
        }
    }
    Run-Case "T-CMAKE-TOKEN-SCOPE" {
        # DEC-024 rule 6: the token is admissible only in buildProfile.flags.
        Expect-Failure {
            Assert-CMakeRecipe (New-CMakeRecipeFixture -OutputPath 'out/deps/w2/lib/${REPO_ROOT}.lib') "zlib"
        } "substitution token"
        $recipe = New-CMakeRecipeFixture
        $recipe.patches = @(([ordered]@{ file = 'patches/${REPO_ROOT}/x.patch'; sha256 = ("b" * 64) } |
            ConvertTo-Json | ConvertFrom-Json))
        Expect-Failure { Assert-NoRecipeToken $recipe "zlib" } "patches\[\].file"
    }
    Run-Case "T-CMAKE-PLAN-ZLIB" {
        $plan = Get-CMakeStagePlan "zlib" "S:\src" "B:\build" "G:\stage"
        # F-W2-48 raised the plan to four artefacts; F-W2-49: this count lives in TWO cases
        # (here and T-CMAKE-STAGE-COPY), and the cs07j edit updated only one of them because
        # the consumers were recalled from memory instead of enumerated by search.
        if (@($plan).Count -ne 4) { throw "the zlib staging plan must carry exactly four artefacts" }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].origin -ne "source" -or
            $license[0].from -ne "S:\src\LICENSE" -or
            $license[0].to -ne "G:\stage\licenses\zlib\LICENSE") {
            throw "the zlib LICENSE must be staged from the source tree into licenses\zlib (F-W2-48)"
        }
        $library = @($plan | Where-Object { $_.role -eq "library" })
        if ($library.Count -ne 1 -or $library[0].from -ne "B:\build\Release\zs.lib") {
            throw "zs.lib must come from the Release directory of the build tree"
        }
        $zlibHeader = @($plan | Where-Object { $_.to -eq "G:\stage\include\zlib.h" })
        if ($zlibHeader.Count -ne 1 -or $zlibHeader[0].origin -ne "source" -or $zlibHeader[0].from -ne "S:\src\zlib.h") {
            throw "zlib.h must be taken from the source tree"
        }
        # F-W2-22 trap: zconf.h exists in the source tree as well and is a different file.
        $zconfHeader = @($plan | Where-Object { $_.to -eq "G:\stage\include\zconf.h" })
        if ($zconfHeader.Count -ne 1 -or $zconfHeader[0].origin -ne "build" -or $zconfHeader[0].from -ne "B:\build\zconf.h") {
            throw "zconf.h must be taken from the build tree, where configure_file generates it"
        }
    }
    Run-Case "T-CMAKE-PLAN-LIBZIP" {
        $plan = Get-CMakeStagePlan "libzip" "S:\src" "B:\build" "G:\stage"
        if (@($plan).Count -ne 4) { throw "the libzip staging plan must carry exactly four artefacts" }
        $library = @($plan | Where-Object { $_.role -eq "library" })
        if ($library.Count -ne 1 -or $library[0].from -ne "B:\build\lib\Release\zip.lib" -or
            $library[0].to -ne "G:\stage\lib\zip.lib") {
            throw "zip.lib must come from lib\Release of the build tree"
        }
        $zipHeader = @($plan | Where-Object { $_.to -eq "G:\stage\include\zip.h" })
        if ($zipHeader.Count -ne 1 -or $zipHeader[0].origin -ne "source" -or $zipHeader[0].from -ne "S:\src\lib\zip.h") {
            throw "zip.h must be taken from the patched source tree"
        }
        $zipconfHeader = @($plan | Where-Object { $_.to -eq "G:\stage\include\zipconf.h" })
        if ($zipconfHeader.Count -ne 1 -or $zipconfHeader[0].origin -ne "build") {
            throw "zipconf.h must be taken from the build tree, where it is generated"
        }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].to -ne "G:\stage\licenses\libzip\LICENSE") {
            throw "the libzip license must be staged under licenses\libzip"
        }
    }
    Run-Case "T-CMAKE-PLAN-UNKNOWN" {
        # The fixture must stay a component with NO plan. freetype held the role until W3,
        # sdl2 until W4, bullet until W5, assimp until W6, openal-soft until W9, manifold
        # until W10. Every real cmake component now has a plan - for the first time since W2
        # the fixture is a synthetic name, which keeps guarding the same property: an unknown
        # component must stop, never silently stage nothing (F-W2-49).
        Expect-Failure { Get-CMakeStagePlan "no-such-component" "S:\src" "B:\build" "G:\stage" } "no staging plan is defined"
    }
    Run-Case "T-CMAKE-PLAN-MANIFOLD" {
        # W10/DEC-064. Two artefacts: the static library from the measured lib\Release\
        # build output, and the canonical Apache-2.0 LICENSE. NOTICE is optional and absent.
        $plan = Get-CMakeStagePlan "manifold" "S:\src" "B:\build" "G:\stage"
        if (@($plan).Count -ne 2) { throw "the manifold staging plan must carry exactly two artefacts" }
        $library = @($plan | Where-Object { $_.role -eq "library" })
        if ($library.Count -ne 1 -or $library[0].origin -ne "build" -or
            $library[0].from -ne "B:\build\lib\Release\manifold.lib" -or
            $library[0].to -ne "G:\stage\lib\manifold.lib") {
            throw "manifold.lib must come from lib\Release of the build tree into lib\"
        }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].origin -ne "source" -or
            $license[0].from -ne "S:\src\LICENSE" -or
            $license[0].to -ne "G:\stage\licenses\manifold\LICENSE") {
            throw "LICENSE must be staged from the source tree into licenses\manifold"
        }
    }
    Run-Case "T-ARCHIVE-EXPECTED-EMPTY" {
        # F-W2-61. An executable-suffixed entry against an EMPTY allowed list must be the
        # honest "unexpected executable payload" stop; a $null list must behave identically
        # instead of dying with a null-method error, which is what the first W9 run measured.
        Expect-Failure { Test-ArchiveEntry "pkg-1.0/tools/helper.cmd" $false @() } `
            "unexpected executable payload"
        Expect-Failure { Test-ArchiveEntry "pkg-1.0/tools/helper.cmd" $false $null } `
            "unexpected executable payload"
        # A declared entry passes against the same gate.
        Test-ArchiveEntry "pkg-1.0/tools/helper.cmd" $false @("pkg-1.0/tools/helper.cmd")
    }
    Run-Case "T-STAGE-HEADER-TREE" {
        # DEC-066. The header stager copies only header-type files, preserves the relative
        # structure, refuses an empty source, and its count gate compares staged against found.
        $headerRoot = Join-Path $selfRoot "header-tree"
        $headerSource = Join-Path $headerRoot "src"
        New-Item -ItemType Directory -Path (Join-Path $headerSource "nested\deep") -Force | Out-Null
        foreach ($fixture in @("top.h", "nested\inner.hpp", "nested\deep\detail.ipp")) {
            [IO.File]::WriteAllText((Join-Path $headerSource $fixture), "// $fixture",
                (New-Object Text.UTF8Encoding($false)))
        }
        [IO.File]::WriteAllText((Join-Path $headerSource "nested\impl.c"), "int x;",
            (New-Object Text.UTF8Encoding($false)))
        [IO.File]::WriteAllText((Join-Path $headerSource "README.txt"), "not a header",
            (New-Object Text.UTF8Encoding($false)))
        $headerDestination = Join-Path $headerRoot "staged"
        $count = Copy-StagedHeaderTree $headerSource $headerDestination "fixture"
        if ($count -ne 3) { throw "the header stager must copy exactly the three headers, copied: $count" }
        foreach ($expected in @("top.h", "nested\inner.hpp", "nested\deep\detail.ipp")) {
            if (-not (Test-Path -LiteralPath (Join-Path $headerDestination $expected) -PathType Leaf)) {
                throw "staged header tree is missing '$expected'"
            }
        }
        if (Test-Path -LiteralPath (Join-Path $headerDestination "nested\impl.c")) {
            throw "the header stager must not copy source files"
        }
        Expect-Failure {
            Copy-StagedHeaderTree (Join-Path $headerRoot "no-such-dir") (Join-Path $headerRoot "x") "fixture"
        } "header source directory is missing"
        $emptySource = Join-Path $headerRoot "empty"
        New-Item -ItemType Directory -Path $emptySource -Force | Out-Null
        Expect-Failure {
            Copy-StagedHeaderTree $emptySource (Join-Path $headerRoot "y") "fixture"
        } "carries no header files"
    }
    Run-Case "T-CMAKE-PLAN-OPENAL" {
        # W9/DEC-060. Three artefacts: the DLL into bin\, the import library into lib\, the
        # COPYING from the source tree into licenses\openal-soft\.
        $plan = Get-CMakeStagePlan "openal-soft" "S:\src" "B:\build" "G:\stage"
        if (@($plan).Count -ne 3) { throw "the openal-soft staging plan must carry exactly three artefacts" }
        $libraries = @($plan | Where-Object { $_.role -eq "library" })
        if ($libraries.Count -ne 2 -or
            $libraries[0].origin -ne "build" -or $libraries[0].from -ne "B:\build\Release\OpenAL32.dll" -or
            $libraries[0].to -ne "G:\stage\bin\OpenAL32.dll" -or
            $libraries[1].origin -ne "build" -or $libraries[1].from -ne "B:\build\Release\OpenAL32.lib" -or
            $libraries[1].to -ne "G:\stage\lib\OpenAL32.lib") {
            throw "the openal-soft libraries must come from Release\ into bin\ and lib\"
        }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].origin -ne "source" -or
            $license[0].from -ne "S:\src\COPYING" -or
            $license[0].to -ne "G:\stage\licenses\openal-soft\COPYING") {
            throw "COPYING must be staged from the source tree into licenses\openal-soft"
        }
    }
    Run-Case "T-CMAKE-RECIPE-SHARED" {
        # W9/DEC-060. The shared gate: SHARED must be declared, a static CRT flag must stop,
        # and the deliberately absent CRT pin (owner decision «Б») must pass.
        function New-SharedRecipeFixture([string]$System = "cmake", [string[]]$DropFlags = @(), [string[]]$AddFlags = @()) {
            $flags = @("-DLIBTYPE=SHARED", "-DALSOFT_UTILS=OFF", "-DALSOFT_EXAMPLES=OFF",
                "-DALSOFT_TESTS=OFF", "-DALSOFT_BACKEND_WASAPI=ON") | Where-Object { $_ -notin $DropFlags }
            return [pscustomobject]@{
                buildProfile = [pscustomobject]@{ system = $System; inherit = "toolchain"; flags = @($flags + $AddFlags) }
                outputs = @(
                    [pscustomobject]@{ path = "out/deps/w9/bin/OpenAL32.dll" },
                    [pscustomobject]@{ path = "out/deps/w9/lib/OpenAL32.lib" })
            }
        }
        Assert-SharedCMakeRecipe (New-SharedRecipeFixture) "openal-soft"
        Expect-Failure { Assert-SharedCMakeRecipe (New-SharedRecipeFixture -System "b2") "openal-soft" } `
            "not a cmake recipe"
        Expect-Failure {
            Assert-SharedCMakeRecipe (New-SharedRecipeFixture -DropFlags @("-DLIBTYPE=SHARED")) "openal-soft"
        } "does not declare -DLIBTYPE=SHARED"
        Expect-Failure {
            Assert-SharedCMakeRecipe (New-SharedRecipeFixture -AddFlags @("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded")) "openal-soft"
        } "static CRT inside a shared library"
    }
    Run-Case "T-CMAKE-PLAN-FREETYPE" {
        $plan = Get-CMakeStagePlan "freetype" "S:\src" "B:\build" "G:\stage"
        if (@($plan).Count -ne 2) { throw "the freetype staging plan must carry exactly two artefacts" }
        $library = @($plan | Where-Object { $_.role -eq "library" })
        if ($library.Count -ne 1 -or $library[0].from -ne "B:\build\Release\freetype.lib" -or
            $library[0].to -ne "G:\stage\lib\freetype.lib") {
            throw "freetype.lib must come from the Release directory of the build tree"
        }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].origin -ne "source" -or
            $license[0].from -ne "S:\src\docs\FTL.TXT" -or
            $license[0].to -ne "G:\stage\licenses\freetype\FTL.TXT") {
            throw "the FTL licence must be staged from docs\FTL.TXT of the source tree"
        }
    }
    Run-Case "T-CMAKE-PLAN-SDL2" {
        $plan = Get-CMakeStagePlan "sdl2" "S:\src" "B:\build" "G:\stage"
        if (@($plan).Count -ne 3) { throw "the sdl2 staging plan must carry exactly three artefacts" }
        $libraries = @($plan | Where-Object { $_.role -eq "library" })
        if ($libraries.Count -ne 2 -or
            $libraries[0].from -ne "B:\build\Release\SDL2.lib" -or
            $libraries[1].from -ne "B:\build\Release\SDL2main.lib") {
            throw "both SDL2 libraries must come from the Release directory of the build tree"
        }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].to -ne "G:\stage\licenses\sdl2\COPYING.txt") {
            throw "COPYING.txt must be staged into licenses\sdl2"
        }
    }
    Run-Case "T-CMAKE-PLAN-BULLET" {
        $plan = Get-CMakeStagePlan "bullet" "S:\src" "B:\build" "G:\stage"
        if (@($plan).Count -ne 6) { throw "the bullet staging plan must carry exactly six artefacts" }
        $libraries = @($plan | Where-Object { $_.role -eq "library" })
        $expectedNames = @("BulletCollision", "BulletDynamics", "BulletInverseDynamics",
            "BulletSoftBody", "LinearMath")
        if ($libraries.Count -ne 5) { throw "the bullet staging plan must carry exactly five libraries" }
        for ($i = 0; $i -lt $expectedNames.Count; $i++) {
            $name = $expectedNames[$i]
            if ($libraries[$i].origin -ne "build" -or
                $libraries[$i].from -ne "B:\build\lib\Release\$name.lib" -or
                $libraries[$i].to -ne "G:\stage\lib\$name.lib") {
                throw "$name.lib must come from lib\Release of the build tree (LIBRARY_OUTPUT_PATH)"
            }
        }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].origin -ne "source" -or
            $license[0].from -ne "S:\src\LICENSE.txt" -or
            $license[0].to -ne "G:\stage\licenses\bullet\LICENSE.txt") {
            throw "LICENSE.txt must be staged from the source tree into licenses\bullet"
        }
    }
    Run-Case "T-CMAKE-PLAN-ASSIMP" {
        $plan = Get-CMakeStagePlan "assimp" "S:\src" "B:\build" "G:\stage"
        if (@($plan).Count -ne 2) { throw "the assimp staging plan must carry exactly two artefacts" }
        $library = @($plan | Where-Object { $_.role -eq "library" })
        if ($library.Count -ne 1 -or $library[0].origin -ne "build" -or
            $library[0].from -ne "B:\build\lib\Release\assimp-vc143-mt.lib" -or
            $library[0].to -ne "G:\stage\lib\assimp-vc143-mt.lib") {
            throw "assimp-vc143-mt.lib must come from lib\Release of the build tree"
        }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].origin -ne "source" -or
            $license[0].from -ne "S:\src\LICENSE" -or
            $license[0].to -ne "G:\stage\licenses\assimp\LICENSE") {
            throw "LICENSE must be staged from the source tree into licenses\assimp"
        }
    }
    Run-Case "T-MSBUILD-PLAN-FREEIMAGE" {
        $plan = Get-MsBuildStagePlan "freeimage" "S:\src" "G:\stage"
        if (@($plan).Count -ne 3) { throw "the freeimage staging plan must carry exactly three artefacts" }
        $library = @($plan | Where-Object { $_.role -eq "library" })
        if ($library.Count -ne 1 -or
            $library[0].from -ne "S:\src\Source\FreeImageLib\x64\Release\FreeImageLib.lib" -or
            $library[0].to -ne "G:\stage\lib\FreeImageLib.lib") {
            throw "FreeImageLib.lib must come from the project's own x64\Release output directory"
        }
        $header = @($plan | Where-Object { $_.role -eq "header" })
        if ($header.Count -ne 1 -or $header[0].from -ne "S:\src\Source\FreeImage.h") {
            throw "FreeImage.h must be staged from the source tree"
        }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].to -ne "G:\stage\licenses\freeimage\license-fi.txt") {
            throw "license-fi.txt must be staged into licenses\freeimage"
        }
        Expect-Failure { Get-MsBuildStagePlan "bullet" "S:\src" "G:\stage" } "no msbuild staging plan is defined"
    }
    Run-Case "T-MSBUILD-RECIPE-GATE" {
        function New-MsBuildRecipeFixture([string]$System = "msbuild", [string[]]$DropFlags = @()) {
            $flags = @("Configuration=Release", "Platform=x64", "RuntimeLibrary=MultiThreaded",
                "FREEIMAGE_LIB") | Where-Object { $_ -notin $DropFlags }
            return [pscustomobject]@{
                buildProfile = [pscustomobject]@{ system = $System; inherit = "toolchain"; flags = @($flags) }
                outputs = @([pscustomobject]@{ path = "out/deps/w3/lib/FreeImageLib.lib" })
            }
        }
        Assert-MsBuildRecipe (New-MsBuildRecipeFixture) "freeimage"
        Expect-Failure { Assert-MsBuildRecipe (New-MsBuildRecipeFixture -System "cmake") "freeimage" } `
            "not an msbuild recipe"
        Expect-Failure {
            Assert-MsBuildRecipe (New-MsBuildRecipeFixture -DropFlags @("RuntimeLibrary=MultiThreaded")) "freeimage"
        } "does not pin 'RuntimeLibrary=MultiThreaded'"
        Expect-Failure {
            Assert-MsBuildRecipe (New-MsBuildRecipeFixture -DropFlags @("FREEIMAGE_LIB")) "freeimage"
        } "does not declare FREEIMAGE_LIB"
    }
    Run-Case "T-MSIEXEC-RECIPE-GATE" {
        # W8/DEC-054. The msiexec-extract gate pins the extraction contract: the exact flags,
        # the MSI identity, exactly one LICENSE with a production pin, and the HTTPS source
        # offer the LGPL requires. Every knob the fixture exposes is one honest stop.
        function New-MsiRecipeFixture([string]$Kind = "binary", [string]$System = "msiexec-extract",
            [string[]]$Flags = @("/a", "/qn", "no-install"), [string]$ArchiveUrl = "https://host.invalid/mono-x64-0.msi",
            [string]$LicenseSha = ("A" * 64), [string]$OfferUrl = "https://host.invalid/mono-src.tar.xz",
            [string]$OfferSha = ("B" * 64), [switch]$DropSourceOffer) {
            $fixture = [ordered]@{
                kind = $Kind
                buildProfile = [pscustomobject]@{ system = $System; inherit = "toolchain"; flags = @($Flags) }
                archive = [pscustomobject]@{ url = $ArchiveUrl; sha256 = ("C" * 64); bytes = [long]1024 }
                licenseFiles = @([pscustomobject]@{ pathInSource = "LICENSE"; sha256 = $LicenseSha; license = "LGPL-2.1-or-later and MIT" })
                outputs = @([pscustomobject]@{ path = "out/deps/w8/Mono/bin/mono.exe" })
                patches = @()
            }
            if (-not $DropSourceOffer) {
                $fixture["sourceOffer"] = [pscustomobject]@{ url = $OfferUrl; sha256 = $OfferSha }
            }
            return [pscustomobject]$fixture
        }
        Assert-MsiexecRecipe (New-MsiRecipeFixture) "mono-msi"
        Expect-Failure { Assert-MsiexecRecipe (New-MsiRecipeFixture -Kind "archive") "mono-msi" } `
            "not a binary component"
        Expect-Failure { Assert-MsiexecRecipe (New-MsiRecipeFixture -System "cmake") "mono-msi" } `
            "not an msiexec-extract recipe"
        Expect-Failure { Assert-MsiexecRecipe (New-MsiRecipeFixture -Flags @("/a", "/qn")) "mono-msi" } `
            "must pin exactly the msiexec-extract flags"
        Expect-Failure {
            Assert-MsiexecRecipe (New-MsiRecipeFixture -Flags @("/a", "/qn", "no-install", "/forcerestart")) "mono-msi"
        } "must pin exactly the msiexec-extract flags"
        Expect-Failure { Assert-MsiexecRecipe (New-MsiRecipeFixture -ArchiveUrl "https://host.invalid/mono.zip") "mono-msi" } `
            "archive is not an MSI"
        Expect-Failure { Assert-MsiexecRecipe (New-MsiRecipeFixture -LicenseSha "TBD-on-approval") "mono-msi" } `
            "does not contain a production SHA-256 pin"
        Expect-Failure { Assert-MsiexecRecipe (New-MsiRecipeFixture -DropSourceOffer) "mono-msi" } `
            "declares no source offer"
        Expect-Failure { Assert-MsiexecRecipe (New-MsiRecipeFixture -OfferUrl "http://host.invalid/src.tar.xz") "mono-msi" } `
            "source offer URL must use HTTPS"
        Expect-Failure { Assert-MsiexecRecipe (New-MsiRecipeFixture -OfferSha "TBD-on-approval") "mono-msi" } `
            "does not contain a production SHA-256 pin"
    }
    Run-Case "T-MSI-IMAGE-SHAPE" {
        # W8/DEC-054. The shape gate encodes the measured administrative-image layout: Mono\
        # plus the two Microsoft redistributable directories plus the extraction's own MSI
        # copy, nothing else, and Mono\ must actually exist.
        $shapeRoot = Join-Path $selfRoot "msi-image-shape"
        $goodImage = Join-Path $shapeRoot "good"
        New-Item -ItemType Directory -Path (Join-Path $goodImage "Mono"),
            (Join-Path $goodImage "System"), (Join-Path $goodImage "System64") -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $goodImage "mono-x64-0.msi"), "stub", (New-Object Text.UTF8Encoding($false)))
        $resolved = Assert-MsiImageShape $goodImage "mono-x64-0.msi" "mono-msi"
        if ([string]$resolved -ne [string](Join-Path $goodImage "Mono")) {
            throw "the shape gate must return the Mono payload root, got: '$resolved'"
        }
        $strayImage = Join-Path $shapeRoot "stray"
        New-Item -ItemType Directory -Path (Join-Path $strayImage "Mono"),
            (Join-Path $strayImage "Unexpected") -Force | Out-Null
        Expect-Failure { Assert-MsiImageShape $strayImage "mono-x64-0.msi" "mono-msi" } `
            "unexpected top-level entry: 'Unexpected'"
        $emptyImage = Join-Path $shapeRoot "empty"
        New-Item -ItemType Directory -Path (Join-Path $emptyImage "System64") -Force | Out-Null
        Expect-Failure { Assert-MsiImageShape $emptyImage "mono-x64-0.msi" "mono-msi" } `
            "carries no Mono directory"
    }
    Run-Case "T-MCS-RECIPE-GATE" {
        # W8b/DEC-057. The gate pins the mcs-from-W8 contract: kind, system, the exact three
        # declared properties, the single mono-msi dependency and the single declared output.
        function New-McsRecipeFixture([string]$Kind = "in-tree", [string]$System = "msbuild",
            [string[]]$Flags = @("producer=mcs-from-W8", "target=library", "sdk=4.8"),
            [string[]]$DependsOn = @("mono-msi"),
            [object[]]$Outputs = @([pscustomobject]@{ path = "out/deps/w8b/FalcoEngine.dll" })) {
            return [pscustomobject]@{
                kind = $Kind
                buildProfile = [pscustomobject]@{ system = $System; inherit = "toolchain"; flags = @($Flags) }
                dependsOn = @($DependsOn)
                outputs = @($Outputs)
                patches = @()
            }
        }
        Assert-McsRecipe (New-McsRecipeFixture) "falco-csharp-api"
        Expect-Failure { Assert-McsRecipe (New-McsRecipeFixture -Kind "archive") "falco-csharp-api" } `
            "not an in-tree component"
        Expect-Failure { Assert-McsRecipe (New-McsRecipeFixture -System "cmake") "falco-csharp-api" } `
            "not the approved msbuild-typed recipe"
        Expect-Failure {
            Assert-McsRecipe (New-McsRecipeFixture -Flags @("producer=csc", "target=library", "sdk=4.8")) "falco-csharp-api"
        } "must pin exactly the flags"
        Expect-Failure { Assert-McsRecipe (New-McsRecipeFixture -DependsOn @()) "falco-csharp-api" } `
            "must depend on exactly 'mono-msi'"
        Expect-Failure {
            Assert-McsRecipe (New-McsRecipeFixture -Outputs @(
                [pscustomobject]@{ path = "out/deps/w8b/FalcoEngine.dll" },
                [pscustomobject]@{ path = "out/deps/w8b/Extra.dll" })) "falco-csharp-api"
        } "must declare exactly one output"
    }
    Run-Case "T-MCS-COMPILE-PLAN" {
        # W8b/DEC-057. The plan reader takes the explicit Compile list and the Reference list
        # from the project XML - with the namespace, because the empty <ItemGroup /> in the
        # real project kills property-style traversal under strict mode (measured, cs27 probe).
        $planRoot = Join-Path $selfRoot "mcs-plan"
        New-Item -ItemType Directory -Path $planRoot -Force | Out-Null
        $goodProject = Join-Path $planRoot "good.csproj"
        [IO.File]::WriteAllText($goodProject, (
            '<?xml version="1.0" encoding="utf-8"?>' + "`n" +
            '<Project ToolsVersion="15.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">' + "`n" +
            '  <ItemGroup>' + "`n" +
            '    <Reference Include="System" />' + "`n" +
            '    <Reference Include="System.Xml.Linq" />' + "`n" +
            '  </ItemGroup>' + "`n" +
            '  <ItemGroup>' + "`n" +
            '    <Compile Include="Core\A.cs" />' + "`n" +
            '    <Compile Include="Math\B.cs" />' + "`n" +
            '  </ItemGroup>' + "`n" +
            '  <ItemGroup />' + "`n" +
            '</Project>'), (New-Object Text.UTF8Encoding($false)))
        $plan = Get-CsprojCompilePlan $goodProject "falco-csharp-api"
        if ((@($plan.sources) -join "|") -ne "Core\A.cs|Math\B.cs") {
            throw "the compile plan must carry the two listed sources in order: '$(@($plan.sources) -join '|')'"
        }
        if ((@($plan.references) -join "|") -ne "System|System.Xml.Linq") {
            throw "the compile plan must carry the two listed references in order: '$(@($plan.references) -join '|')'"
        }
        $emptyProject = Join-Path $planRoot "empty.csproj"
        [IO.File]::WriteAllText($emptyProject, (
            '<?xml version="1.0" encoding="utf-8"?>' + "`n" +
            '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemGroup /></Project>'),
            (New-Object Text.UTF8Encoding($false)))
        Expect-Failure { Get-CsprojCompilePlan $emptyProject "falco-csharp-api" } `
            "declares no source files"
        $traversalProject = Join-Path $planRoot "traversal.csproj"
        [IO.File]::WriteAllText($traversalProject, (
            '<?xml version="1.0" encoding="utf-8"?>' + "`n" +
            '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemGroup>' +
            '<Compile Include="..\evil.cs" /></ItemGroup></Project>'),
            (New-Object Text.UTF8Encoding($false)))
        Expect-Failure { Get-CsprojCompilePlan $traversalProject "falco-csharp-api" } `
            "not a safe relative path"
        $oddReferenceProject = Join-Path $planRoot "oddref.csproj"
        [IO.File]::WriteAllText($oddReferenceProject, (
            '<?xml version="1.0" encoding="utf-8"?>' + "`n" +
            '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><ItemGroup>' +
            '<Reference Include="C:\evil\thing.dll" /><Compile Include="A.cs" /></ItemGroup></Project>'),
            (New-Object Text.UTF8Encoding($false)))
        Expect-Failure { Get-CsprojCompilePlan $oddReferenceProject "falco-csharp-api" } `
            "not a plain assembly name"
    }
    Run-Case "T-B2-RECIPE-GATE" {
        function New-B2RecipeFixture([string]$System = "b2", [string[]]$DropFlags = @(), [string[]]$AddFlags = @()) {
            $flags = @("address-model=64", "variant=release", "link=static", "runtime-link=static",
                "threading=multi", "--with-filesystem") | Where-Object { $_ -notin $DropFlags }
            return [pscustomobject]@{
                buildProfile = [pscustomobject]@{ system = $System; inherit = "toolchain"; flags = @($flags + $AddFlags) }
                outputs = @([pscustomobject]@{ path = "out/deps/w7/lib/libboost_filesystem-vc143-mt-s-x64-1_74.lib" })
            }
        }
        Assert-B2Recipe (New-B2RecipeFixture) "boost"
        Expect-Failure { Assert-B2Recipe (New-B2RecipeFixture -System "cmake") "boost" } "not a b2 recipe"
        Expect-Failure {
            Assert-B2Recipe (New-B2RecipeFixture -DropFlags @("runtime-link=static")) "boost"
        } "does not pin 'runtime-link=static'"
        Expect-Failure {
            Assert-B2Recipe (New-B2RecipeFixture -AddFlags @("runtime-link=shared")) "boost"
        } "forbidden b2 property"
        Expect-Failure {
            Assert-B2Recipe (New-B2RecipeFixture -AddFlags @("toolset=msvc-14.2")) "boost"
        } "driver-owned b2 argument"
        # F-W2-59: --hash joined the driver-owned list with CS-21; a recipe must not smuggle
        # or contradict it, for the same reason as the other driver-owned arguments.
        Expect-Failure {
            Assert-B2Recipe (New-B2RecipeFixture -AddFlags @("--hash")) "boost"
        } "driver-owned b2 argument"
    }
    Run-Case "T-B2-PLAN-BOOST" {
        $plan = Get-B2StagePlan "boost" "S:\src" "T:\b2stage" "G:\stage"
        if (@($plan).Count -ne 9) { throw "the boost staging plan must carry exactly nine artefacts" }
        $libraries = @($plan | Where-Object { $_.role -eq "library" })
        if ($libraries.Count -ne 8) { throw "the boost staging plan must carry exactly eight libraries" }
        $expectedNames = @("filesystem", "system", "serialization", "regex", "iostreams",
            "date_time", "thread", "chrono")
        for ($i = 0; $i -lt $expectedNames.Count; $i++) {
            $fileName = "libboost_$($expectedNames[$i])-vc143-mt-s-x64-1_74.lib"
            if ($libraries[$i].origin -ne "b2-stage" -or
                $libraries[$i].from -ne "T:\b2stage\lib\$fileName" -or
                $libraries[$i].to -ne "G:\stage\lib\$fileName") {
                throw "$fileName must come from lib of the b2 stage tree"
            }
        }
        $license = @($plan | Where-Object { $_.role -eq "license" })
        if ($license.Count -ne 1 -or $license[0].origin -ne "source" -or
            $license[0].from -ne "S:\src\LICENSE_1_0.txt" -or
            $license[0].to -ne "G:\stage\licenses\boost\LICENSE_1_0.txt") {
            throw "LICENSE_1_0.txt must be staged from the source tree into licenses\boost"
        }
        Expect-Failure { Get-B2StagePlan "manifold" "S:\src" "T:\b2stage" "G:\stage" } "no b2 staging plan is defined"
    }
    Run-Case "T-B2-USERCONFIG" {
        $configPath = Join-Path $selfRoot "b2-userconfig\user-config.jam"
        $written = Write-B2UserConfig $configPath "C:\VS\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe" `
            "C:\VS\VC\Auxiliary\Build\vcvarsall.bat"
        $content = [IO.File]::ReadAllText($written)
        # F-W2-57. The expected text is the exact line that probe A proved: the command, then
        # the <setup> option, both quoted, both spelled with forward slashes only.
        $expected = "using msvc : 14.3 : `"C:/VS/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe`"" +
            " : <setup>`"C:/VS/VC/Auxiliary/Build/vcvarsall.bat`" ;`n"
        if ($content -ne $expected) { throw "the generated user-config does not match the expected jam line: '$content'" }
        if ($content.Contains([string][char]92)) { throw "the generated user-config carries a backslash: '$content'" }
        if ($content -notmatch '<setup>"') { throw "the generated user-config carries no quoted <setup> option: '$content'" }
        # The setup script is a distinct argument, not appended to the command.
        if ([regex]::Matches($content, '"').Count -ne 4) {
            throw "the generated user-config must carry exactly two quoted values: '$content'"
        }
        Expect-Failure {
            Write-B2UserConfig (Join-Path $selfRoot "b2-userconfig\bad.jam") "" "C:\VS\VC\Auxiliary\Build\vcvarsall.bat"
        } "requires the pinned cl"
        Expect-Failure {
            Write-B2UserConfig (Join-Path $selfRoot "b2-userconfig\bad.jam") 'C:\odd"quote\cl.exe' `
                "C:\VS\VC\Auxiliary\Build\vcvarsall.bat"
        } "must not carry a double quote"
        Expect-Failure {
            Write-B2UserConfig (Join-Path $selfRoot "b2-userconfig\bad.jam") `
                "C:\VS\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe" ""
        } "requires the pinned vcvarsall.bat"
        Expect-Failure {
            Write-B2UserConfig (Join-Path $selfRoot "b2-userconfig\bad.jam") `
                "C:\VS\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe" 'C:\odd"quote\vcvarsall.bat'
        } "setup script path must not carry a double quote"
    }
    Run-Case "T-B2-TOOLSET-ROOT" {
        # F-W2-56. The value handed to bootstrap.bat must end with a separator, because upstream
        # concatenates "Auxiliary\Build\vcvarsall.bat" straight onto it.
        $root = Get-B2ToolsetRoot "C:\Program Files\Microsoft Visual Studio\2022\Community"
        if ($root -ne "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\") {
            throw "the derived b2 toolset root is wrong: '$root'"
        }
        if (-not $root.EndsWith([string][char]92)) { throw "the derived root does not end with a separator" }
        if ($root.Contains("\\")) { throw "the derived root carries a doubled separator: '$root'" }
        # A path already carrying a trailing separator must not produce a doubled one.
        if ((Get-B2ToolsetRoot "C:\VS\Community\") -ne "C:\VS\Community\VC\") {
            throw "a trailing separator on the installation path was not normalised"
        }
        Expect-Failure { Get-B2ToolsetRoot "" } "installation path is required"
        Expect-Failure { Get-B2ToolsetRoot 'C:\odd"quote\VS' } "must not carry a double quote"
    }
    Run-Case "T-CMAKE-OUTPUT-MAP" {
        $stage = Join-Path $selfRoot "w2-outputs"
        New-Item -ItemType Directory -Path $stage -Force | Out-Null
        $mapped = Get-StagedOutputPath "out/deps/w2/lib/zs.lib" $stage "zlib"
        if ($mapped -ne (Join-Path $stage "lib\zs.lib")) { throw "output path was not mapped into the staging tree: $mapped" }
        Expect-Failure { Get-StagedOutputPath "out/deps/w1/lib/zs.lib" $stage "zlib" } "not inside the wave prefix"
        # The prefix is a parameter since W3: the same output is admissible under its own wave.
        $w3Mapped = Get-StagedOutputPath "out/deps/w3/lib/freetype.lib" $stage "freetype" "out/deps/w3/"
        if (-not $w3Mapped.EndsWith("lib\freetype.lib")) { throw "W3 prefix mapping is wrong: '$w3Mapped'" }
        Expect-Failure { Get-StagedOutputPath "out/deps/w2/lib/zs.lib" $stage "zlib" "out/deps/w3/" } "not inside the wave prefix"
        Expect-Failure { Get-StagedOutputPath "out/deps/w2/" $stage "zlib" } "does not name a file"
    }
    Run-Case "T-CMAKE-STAGE-COPY" {
        $source = Join-Path $selfRoot "stage-src"
        $build = Join-Path $selfRoot "stage-build"
        $stage = Join-Path $selfRoot "stage-out"
        New-Item -ItemType Directory -Path $source, (Join-Path $build "Release"), $stage -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $source "zlib.h"), "source header")
        [IO.File]::WriteAllText((Join-Path $source "zconf.h"), "source zconf - MUST NOT be staged")
        [IO.File]::WriteAllText((Join-Path $source "LICENSE"), "zlib license text")
        [IO.File]::WriteAllText((Join-Path $build "zconf.h"), "generated zconf")
        [IO.File]::WriteAllText((Join-Path $build "Release\zs.lib"), "library bytes")
        $staged = Copy-StagedArtifacts (Get-CMakeStagePlan "zlib" $source $build $stage) "zlib" $stage
        # F-W2-48: the license entry raises the zlib plan from three artefacts to four.
        if (@($staged).Count -ne 4) { throw "four artefacts were expected in the staged result" }
        $stagedZconf = [IO.File]::ReadAllText((Join-Path $stage "include\zconf.h"))
        if ($stagedZconf -ne "generated zconf") { throw "the staged zconf.h is the source copy, not the generated one" }
        if (-not (Test-Path -LiteralPath (Join-Path $stage "lib\zs.lib") -PathType Leaf)) { throw "zs.lib was not staged" }
        if (-not (Test-Path -LiteralPath (Join-Path $stage "licenses\zlib\LICENSE") -PathType Leaf)) {
            throw "the zlib LICENSE was not staged (F-W2-48)"
        }
    }
    Run-Case "T-CMAKE-STAGE-MISSING" {
        $source = Join-Path $selfRoot "stage-missing-src"
        $build = Join-Path $selfRoot "stage-missing-build"
        $stage = Join-Path $selfRoot "stage-missing-out"
        New-Item -ItemType Directory -Path $source, $build, $stage -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $source "zlib.h"), "source header")
        Expect-Failure {
            Copy-StagedArtifacts (Get-CMakeStagePlan "zlib" $source $build $stage) "zlib" $stage
        } "is missing in the build tree"
    }
    Run-Case "T-CL-RECIPE-GATE" {
        # The cl recipe IS the command line, so this gate is the only place it can be judged
        # before it runs.
        Assert-ClRecipe (New-ClRecipeFixture) "bc7enc"
        Expect-Failure { Assert-ClRecipe (New-ClRecipeFixture -System "cmake") "bc7enc" } "not a cl recipe"
        Expect-Failure { Assert-ClRecipe (New-ClRecipeFixture -Kind "archive") "bc7enc" } "kind must be 'in-tree'"
        Expect-Failure { Assert-ClRecipe (New-ClRecipeFixture -WithPatch) "bc7enc" } "declares patches"
        Expect-Failure {
            Assert-ClRecipe (New-ClRecipeFixture -Flags @("/TC", "/O2")) "bc7enc"
        } "does not pin the static release CRT through /MT"
        Expect-Failure {
            Assert-ClRecipe (New-ClRecipeFixture -Flags @("/O2", "/MT")) "bc7enc"
        } "does not force the C language through /TC"
        Expect-Failure {
            Assert-ClRecipe (New-ClRecipeFixture -Flags @("/TC", "/O2", "/MT", "/MD")) "bc7enc"
        } "non-static or debug CRT"
        Expect-Failure {
            Assert-ClRecipe (New-ClRecipeFixture -Flags @("/TC", "/O2", "/MT", "/EHsc")) "bc7enc"
        } "C\+\+ exception flag"
        Expect-Failure {
            Assert-ClRecipe (New-ClRecipeFixture -Flags @("/TC", "/O2", "/MT", "/GL")) "bc7enc"
        } "statically archived object"
        foreach ($owned in @("/c", "/nologo", "/Fooverride.obj", "/Brepro", "/pathmap:a=b",
            "/experimental:deterministic")) {
            Expect-Failure {
                Assert-ClRecipe (New-ClRecipeFixture -Flags (@("/TC", "/O2", "/MT") + @($owned))) "bc7enc"
            } "driver-owned flag"
        }
        # The flags the owner approved for DEC-025 must survive the gate unchanged, and /Gy,
        # /GS and /Zc:inline must not be mistaken for /GL or for a driver-owned flag.
        Assert-ClRecipe (New-ClRecipeFixture -Flags @("/TC", "/O2", "/Ob2", "/DNDEBUG", "/MT",
            "/GS", "/Gy", "/Zc:inline")) "bc7enc"
        Expect-Failure {
            Assert-ClRecipe (New-ClRecipeFixture -OutputPath 'out/deps/w2/lib/${REPO_ROOT}.lib') "bc7enc"
        } "substitution token"
    }
    Run-Case "T-CL-SOURCE-PLAN" {
        $plan = Get-ClSourcePlan "bc7enc"
        # DEC-025 (answer P-1): bc7decomp.c is not compiled.
        if (@($plan.sources).Count -ne 1 -or @($plan.sources)[0] -ne "bc7enc16.c") {
            throw "the bc7enc source plan must compile exactly bc7enc16.c"
        }
        if ("bc7decomp.c" -in @($plan.sources)) { throw "bc7decomp.c must not be compiled" }
        if ($plan.sourceInTree -ne "codecs\textures\bc7enc") {
            throw "unexpected in-tree source root: $($plan.sourceInTree)"
        }
        Assert-RelativeSafePath $plan.sourceInTree "fixture in-tree source root"
        if ($plan.licenseStaged -ne "licenses\bc7enc\LICENSE") {
            throw "the bc7enc license must be staged under licenses\bc7enc"
        }
        Expect-Failure { Get-ClSourcePlan "freetype" } "no in-tree source plan is defined"
    }
    function New-ClStubShim([string]$Path, [string]$StderrLine, [int]$ExitCode) {
        # A .cmd shim is the only dependency-free way to get a real NATIVE process to write to
        # stderr here. The hazard under test is specifically how the engine treats a native
        # command's stderr when 2>&1 merges it under $ErrorActionPreference = "Stop", so a
        # PowerShell function could not reproduce it and the case would prove nothing.
        $lines = @(
            "@echo off",
            "echo %* > `"%FALCO_SELFTEST_CLARGS%`"",
            "echo $StderrLine 1>&2"
        )
        if ($ExitCode -eq 0) { $lines += "echo stub-object > `"%FALCO_SELFTEST_OBJ%`"" }
        $lines += "exit /b $ExitCode"
        [IO.File]::WriteAllText($Path, (($lines -join "`r`n") + "`r`n"), (New-Object Text.ASCIIEncoding))
    }
    Run-Case "T-CL-STDERR-NONFATAL" {
        # CS07C: cl wrote one command-line warning to stderr, $ErrorActionPreference = "Stop" turned
        # it into a terminating error at the '&' call, and the run died before the exit-code gate -
        # and before the very next line, which writes the log. Two components' worth of successful
        # work was discarded because of a warning. This case pins the fix.
        $stubRoot = Join-Path $selfRoot "cl-stderr"
        $srcRoot = Join-Path $stubRoot "src"
        $objRoot = Join-Path $stubRoot "obj"
        New-Item -ItemType Directory -Path $stubRoot, $srcRoot, $objRoot -Force | Out-Null
        $sourceFile = Join-Path $srcRoot "unit.c"
        [IO.File]::WriteAllText($sourceFile, "int unit(void) { return 0; }`n",
            (New-Object Text.UTF8Encoding($false)))
        $objectFile = Join-Path $objRoot "unit.obj"
        $okStub = Join-Path $stubRoot "cl-ok.cmd"
        $failStub = Join-Path $stubRoot "cl-fail.cmd"
        New-ClStubShim $okStub "stub: a diagnostic written to stderr" 0
        New-ClStubShim $failStub "stub: a diagnostic written to stderr" 1
        $previous = $ErrorActionPreference
        $env:FALCO_SELFTEST_OBJ = $objectFile
        $env:FALCO_SELFTEST_CLARGS = (Join-Path $stubRoot "args.txt")
        try {
            $ErrorActionPreference = "Stop"
            $out = @(Invoke-ClCompile $okStub "bc7enc" @("/TC", "/O2", "/MT") $sourceFile $objectFile `
                $srcRoot $objRoot @())
            if ($ErrorActionPreference -ne "Stop") {
                throw "the native capture wrapper did not restore the error preference"
            }
            if (-not (Test-Path -LiteralPath $objectFile -PathType Leaf)) {
                throw "the stub object was not produced"
            }
            if (@($out | Where-Object { [string]$_ -match "^PASS: bc7enc/unit compiled with cl" }).Count -ne 1) {
                throw "a stderr line with exit code 0 must still be reported as PASS"
            }
            $logPath = Join-Path $script:RunRoot "bc7enc-unit-cl.log"
            if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) { throw "the cl log was not written" }
            if ([IO.File]::ReadAllText($logPath) -notmatch "written to stderr") {
                throw "the stderr text did not reach the log"
            }
            # Tolerating stderr must not tolerate failure: the exit-code gate still has to fire.
            Expect-Failure {
                Invoke-ClCompile $failStub "bc7enc" @("/TC", "/O2", "/MT") $sourceFile $objectFile `
                    $srcRoot $objRoot @()
            } "with exit code 1"
        }
        finally {
            $ErrorActionPreference = $previous
            Remove-Item Env:\FALCO_SELFTEST_OBJ -ErrorAction SilentlyContinue
            Remove-Item Env:\FALCO_SELFTEST_CLARGS -ErrorAction SilentlyContinue
        }
    }
    Run-Case "T-CL-D9007-SURFACED" {
        # DEC-026 (owner answer 1-A): /experimental:deterministic is added so that MSVC 14.44
        # honours /pathmap: instead of dropping it with D9007. D9007 is a WARNING, so the compile
        # still exits 0 - a silent drop would leave the absolute build roots in the object and the
        # pinned SHA-256 would quietly stop reproducing. Both halves are pinned here: the flag is
        # on the command line and it precedes /pathmap:, and a D9007 that appears anyway is
        # surfaced rather than swallowed.
        $stubRoot = Join-Path $selfRoot "cl-d9007"
        $srcRoot = Join-Path $stubRoot "src"
        $objRoot = Join-Path $stubRoot "obj"
        New-Item -ItemType Directory -Path $stubRoot, $srcRoot, $objRoot -Force | Out-Null
        $sourceFile = Join-Path $srcRoot "mapped.c"
        [IO.File]::WriteAllText($sourceFile, "int mapped(void) { return 0; }`n",
            (New-Object Text.UTF8Encoding($false)))
        $objectFile = Join-Path $objRoot "mapped.obj"
        $argsPath = Join-Path $stubRoot "args.txt"
        $noisyStub = Join-Path $stubRoot "cl-d9007.cmd"
        $quietStub = Join-Path $stubRoot "cl-quiet.cmd"
        New-ClStubShim $noisyStub `
            "cl : command line warning D9007: /pathmap: requires /experimental:deterministic - ignored" 0
        New-ClStubShim $quietStub "stub: nothing unusual" 0
        $env:FALCO_SELFTEST_OBJ = $objectFile
        $env:FALCO_SELFTEST_CLARGS = $argsPath
        try {
            $out = @(Invoke-ClCompile $noisyStub "bc7enc" @("/TC", "/O2", "/MT") $sourceFile $objectFile `
                $srcRoot $objRoot @())
            $commandLine = [IO.File]::ReadAllText($argsPath)
            $deterministicAt = $commandLine.IndexOf("/experimental:deterministic")
            $pathMapAt = $commandLine.IndexOf("/pathmap:")
            if ($deterministicAt -lt 0) {
                throw "the driver did not put /experimental:deterministic on the cl command line"
            }
            if ($pathMapAt -lt 0) { throw "the driver did not put /pathmap: on the cl command line" }
            if ($deterministicAt -gt $pathMapAt) {
                throw "/experimental:deterministic must precede /pathmap: on the command line"
            }
            if (@($out | Where-Object { [string]$_ -match "^WARN: cl rejected a driver-owned option" }).Count -ne 1) {
                throw "D9007 was not surfaced as a WARN line"
            }
            $quiet = @(Invoke-ClCompile $quietStub "bc7enc" @("/TC", "/O2", "/MT") $sourceFile $objectFile `
                $srcRoot $objRoot @())
            if (@($quiet | Where-Object { [string]$_ -match "D9007" }).Count -ne 0) {
                throw "a compile without D9007 must not raise the D9007 warning"
            }
        }
        finally {
            Remove-Item Env:\FALCO_SELFTEST_OBJ -ErrorAction SilentlyContinue
            Remove-Item Env:\FALCO_SELFTEST_CLARGS -ErrorAction SilentlyContinue
        }
    }
    function New-IncludeEnvStub([string]$Path, [string]$IncludeValue, [int]$ExitCode) {
        # A .cmd stub stands in for vcvars64.bat. The mechanics under test are cmd's own - a batch
        # file that sets INCLUDE, and a wrapper that has to survive spaces and parentheses in the
        # path it calls - so a PowerShell fake would prove nothing about the real thing.
        $lines = @("@echo off", "set INCLUDE=$IncludeValue", "exit /b $ExitCode")
        [IO.File]::WriteAllText($Path, (($lines -join "`r`n") + "`r`n"), (New-Object Text.ASCIIEncoding))
    }
    Run-Case "T-CL-INCLUDE-FROM-VCVARS" {
        # F-W2-44. The include list is read out of the Visual Studio environment, not rebuilt by
        # hand. Three properties are pinned: the list is split on ';', empty entries are dropped,
        # and a trailing backslash is removed - the last one is not cosmetic, because an argument
        # ending in a backslash is re-quoted by the Windows escaping rules and cl would then
        # receive a corrupted /I.
        $envRoot = Join-Path $selfRoot "cl-include-env"
        # The fixture directory deliberately carries a space and parentheses in its name: that is
        # the exact shape of "Program Files (x86)" that made a quoted cmd one-liner unsafe.
        $vcRoot = Join-Path $envRoot "Tool Kit (x86)\vc"
        # F-W2-46: the segment is spelled lower-case 'include' on purpose - that is how the real
        # vcvars64.bat writes it, and the fixture that first used a capital 'Include' let a
        # case-sensitive version pattern pass the self-test while failing on the live machine.
        $ucrtRoot = Join-Path $envRoot "Windows Kits\10\include\10.0.26100.0\ucrt"
        New-Item -ItemType Directory -Path $vcRoot, $ucrtRoot -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $vcRoot "limits.h"), "/* fixture */`n",
            (New-Object Text.UTF8Encoding($false)))
        [IO.File]::WriteAllText((Join-Path $ucrtRoot "stdlib.h"), "/* fixture */`n",
            (New-Object Text.UTF8Encoding($false)))
        $stub = Join-Path $envRoot "vcvars-stub.cmd"
        New-IncludeEnvStub $stub "$vcRoot\;;$ucrtRoot" 0
        $roots = @(Get-BatchIncludeRoots $stub (Join-Path $envRoot "work"))
        if ($roots.Count -ne 2) {
            throw "expected two include roots from the stub environment, got $($roots.Count): $($roots -join ' | ')"
        }
        if ($roots[0] -ne $vcRoot) { throw "the first root was not preserved verbatim: '$($roots[0])'" }
        if ($roots[1] -ne $ucrtRoot) { throw "the second root was not preserved verbatim: '$($roots[1])'" }
        $versions = @(Get-UcrtVersionsFromRoots $roots)
        if (@($versions).Count -ne 1 -or [string]$versions[0] -ne "10.0.26100.0") {
            throw ("the ucrt SDK version was not extracted from a lower-case include path: " +
                "'$($versions -join ', ')'")
        }
        $wrapper = Join-Path $envRoot "work\read-include.cmd"
        if (-not (Test-Path -LiteralPath $wrapper -PathType Leaf)) {
            throw "the generated wrapper was not left on disk for diagnosis"
        }
        # A batch file that fails must stop the wave, not return a half-list.
        $failing = Join-Path $envRoot "vcvars-fail.cmd"
        New-IncludeEnvStub $failing "$vcRoot" 1
        Expect-Failure { Get-BatchIncludeRoots $failing (Join-Path $envRoot "work-fail") } "exit code 1"
        # So must one that leaves INCLUDE empty: an empty list would otherwise reach cl as no /I at
        # all, which is precisely the CS07F failure wearing a different hat.
        $empty = Join-Path $envRoot "vcvars-empty.cmd"
        New-IncludeEnvStub $empty "" 0
        Expect-Failure { Get-BatchIncludeRoots $empty (Join-Path $envRoot "work-empty") } "empty include list"
    }
    Run-Case "T-CL-INCLUDE-SENTINEL-GATE" {
        # F-W2-44. CS07F spent a whole sanctioned build run to learn that a header was unreachable,
        # and reported it as a bare C1034 that named no search path. The gate turns that into a
        # message naming the header and every root that was searched, before cl is ever started.
        $gateRoot = Join-Path $selfRoot "cl-include-gate"
        $vcRoot = Join-Path $gateRoot "vc"
        $ucrtRoot = Join-Path $gateRoot "ucrt"
        New-Item -ItemType Directory -Path $vcRoot, $ucrtRoot -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $vcRoot "limits.h"), "/* fixture */`n",
            (New-Object Text.UTF8Encoding($false)))
        # Positive control first: with both sentinels present the gate must stay silent. Without
        # this half the negative half would still pass if the gate rejected everything.
        [IO.File]::WriteAllText((Join-Path $ucrtRoot "stdlib.h"), "/* fixture */`n",
            (New-Object Text.UTF8Encoding($false)))
        Assert-IncludeRootsUsable @($vcRoot, $ucrtRoot) @("stdlib.h", "limits.h")
        Remove-Item -LiteralPath (Join-Path $ucrtRoot "stdlib.h") -Force
        Expect-Failure {
            Assert-IncludeRootsUsable @($vcRoot, $ucrtRoot) @("stdlib.h", "limits.h")
        } "does not resolve 'stdlib.h'"
        # A root list that is short one half must fail too, even though the other half resolves.
        Expect-Failure {
            Assert-IncludeRootsUsable @($vcRoot) @("stdlib.h")
        } "does not resolve 'stdlib.h'"
    }
    Run-Case "T-CL-INCLUDE-FLAGS-ON-COMMAND-LINE" {
        # F-W2-44 / DEC-028 (owner answer 2-A). The roots must reach cl as /I on the command line,
        # where the compile log records them, and not through the process environment, where a
        # later cmake child would silently inherit them. Both the presence and the position are
        # pinned: /I has to precede the source file, or cl ignores it for that translation unit.
        $flagRoot = Join-Path $selfRoot "cl-include-flags"
        $srcRoot = Join-Path $flagRoot "src"
        $objRoot = Join-Path $flagRoot "obj"
        $incA = Join-Path $flagRoot "inc-a"
        $incB = Join-Path $flagRoot "inc-b"
        New-Item -ItemType Directory -Path $flagRoot, $srcRoot, $objRoot, $incA, $incB -Force | Out-Null
        $sourceFile = Join-Path $srcRoot "flags.c"
        [IO.File]::WriteAllText($sourceFile, "int flags(void) { return 0; }`n",
            (New-Object Text.UTF8Encoding($false)))
        $objectFile = Join-Path $objRoot "flags.obj"
        $argsPath = Join-Path $flagRoot "args.txt"
        $stub = Join-Path $flagRoot "cl-ok.cmd"
        New-ClStubShim $stub "stub: nothing unusual" 0
        $env:FALCO_SELFTEST_OBJ = $objectFile
        $env:FALCO_SELFTEST_CLARGS = $argsPath
        $previousInclude = $env:INCLUDE
        # A distinctive sentinel is used rather than an empty string because PowerShell deletes an
        # environment variable that is assigned "": $env:INCLUDE would then read back as $null and
        # a comparison against "" would report a mutation that never happened. The sentinel also
        # tests more than the empty string did - it fails if the driver sets INCLUDE and equally if
        # the driver clears it.
        $sentinel = "FALCO-SELFTEST-INCLUDE-UNTOUCHED"
        try {
            $env:INCLUDE = $sentinel
            Invoke-ClCompile $stub "bc7enc" @("/TC", "/O2", "/MT") $sourceFile $objectFile `
                $srcRoot $objRoot @($incA, $incB) | Out-Null
            $commandLine = [IO.File]::ReadAllText($argsPath)
            $firstAt = $commandLine.IndexOf("/I$incA")
            $secondAt = $commandLine.IndexOf("/I$incB")
            $sourceAt = $commandLine.IndexOf($sourceFile)
            if ($firstAt -lt 0) { throw "the driver did not pass the first include root as /I" }
            if ($secondAt -lt 0) { throw "the driver did not pass the second include root as /I" }
            if ($sourceAt -lt 0) { throw "the source file did not reach the cl command line" }
            if ($firstAt -gt $sourceAt -or $secondAt -gt $sourceAt) {
                throw "every /I must precede the source file on the cl command line"
            }
            if ($firstAt -gt $secondAt) { throw "the include roots reached cl out of order" }
            if ($env:INCLUDE -ne $sentinel) {
                throw "the driver changed the process INCLUDE variable instead of passing /I"
            }
            # An empty root list must still produce a valid command line - that is the shape every
            # non-cl caller and every other self-test case uses.
            Invoke-ClCompile $stub "bc7enc" @("/TC", "/O2", "/MT") $sourceFile $objectFile `
                $srcRoot $objRoot @() | Out-Null
            if ([IO.File]::ReadAllText($argsPath).IndexOf("/I") -ge 0) {
                throw "an empty include list must not put /I on the cl command line"
            }
        }
        finally {
            $env:INCLUDE = $previousInclude
            Remove-Item Env:\FALCO_SELFTEST_OBJ -ErrorAction SilentlyContinue
            Remove-Item Env:\FALCO_SELFTEST_CLARGS -ErrorAction SilentlyContinue
        }
    }
    Run-Case "T-CL-LICENSE-EXTRACT" {
        # bc7enc carries its dual grant as a comment block, not as a LICENSE file. The staged
        # file must be the grant and nothing else - in particular not the enclosing comment.
        $licenseRoot = Join-Path $selfRoot "cl-license"
        New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
        $rail = ("-" * 78)
        $marker = "This software is available under 2 licenses"
        $grant = @(
            "/* file header comment", "int stub(void) { return 0; }", "",
            "/*", $rail,
            "$marker -- choose whichever you prefer.",
            $rail,
            "ALTERNATIVE A - MIT License", "Copyright(c) 2018 Example Author",
            "Permission is hereby granted, free of charge, to any person obtaining a copy",
            "", "ALTERNATIVE B - Public Domain (www.unlicense.org)",
            "This is free and unencumbered software released into the public domain.",
            "", "Anyone is free to copy, modify, publish, use, compile, sell...",
            $rail, "*/"
        )
        $sourcePath = Join-Path $licenseRoot "sample.c"
        [IO.File]::WriteAllText($sourcePath, (($grant -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
        $destination = Join-Path $licenseRoot "LICENSE"
        $extract = Export-EmbeddedLicenseBlock $sourcePath $marker $destination "bc7enc"
        if ($extract.firstLine -ne 5 -or $extract.lastLine -ne 16 -or $extract.lines -ne 12) {
            throw "unexpected license boundary: lines $($extract.firstLine)-$($extract.lastLine)"
        }
        $staged = [IO.File]::ReadAllText($destination)
        if ($staged.Contains("/*") -or $staged.Contains("*/")) {
            throw "the staged license carries a comment delimiter"
        }
        if (-not $staged.StartsWith($rail)) { throw "the staged license does not open with the dashed rail" }
        if (-not $staged.EndsWith("$rail`n")) { throw "the staged license does not close with the dashed rail" }
        if ($staged.Contains("int stub")) { throw "source code leaked into the staged license" }
        # A source with no grant, a marker with no opening rail, a block that is too short and a
        # block that swallows a comment terminator must all be refused rather than staged.
        $noMarker = Join-Path $licenseRoot "no-marker.c"
        [IO.File]::WriteAllText($noMarker, "int stub(void) { return 0; }`n", (New-Object Text.UTF8Encoding($false)))
        Expect-Failure { Export-EmbeddedLicenseBlock $noMarker $marker $destination "bc7enc" } "was not found"
        $noRail = Join-Path $licenseRoot "no-rail.c"
        [IO.File]::WriteAllText($noRail, (("code", "$marker -- choose.", $rail) -join "`n"),
            (New-Object Text.UTF8Encoding($false)))
        Expect-Failure { Export-EmbeddedLicenseBlock $noRail $marker $destination "bc7enc" } "dashed rail"
        $tooShort = Join-Path $licenseRoot "short.c"
        [IO.File]::WriteAllText($tooShort, (($rail, "$marker -- choose.", $rail) -join "`n"),
            (New-Object Text.UTF8Encoding($false)))
        Expect-Failure { Export-EmbeddedLicenseBlock $tooShort $marker $destination "bc7enc" } "implausibly short"
        $withComment = @($grant)
        $withComment[14] = "Anyone is free to copy ... */ and then some"
        $dirty = Join-Path $licenseRoot "dirty.c"
        [IO.File]::WriteAllText($dirty, (($withComment -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
        Expect-Failure { Export-EmbeddedLicenseBlock $dirty $marker $destination "bc7enc" } "comment delimiter"
    }
    Run-Case "T-W2-FINGERPRINT-BC7ENC" {
        # Before CS-07 the W2 fingerprint was built from zlib and libzip only, so neither an
        # edited bc7enc recipe nor an edited in-tree source moved it: the recorded wave state
        # kept looking current and the wave would never have been rebuilt. Both halves of that
        # defect are checked here.
        #
        # The case runs against a mock repository root rather than the real tree. The driver's
        # own identity comes from $PSCommandPath and is unaffected, and the fixture's libzip
        # patch is dropped, so nothing outside this directory can influence the result - and,
        # unlike the real tree, a mock source may be edited to prove the second half.
        $fingerprintRoot = Join-PathLiteral $selfRoot "fingerprint-repo"
        $plan = Get-ClSourcePlan "bc7enc"
        $mockSourceRoot = Join-PathLiteral $fingerprintRoot $plan.sourceInTree
        [System.IO.Directory]::CreateDirectory($mockSourceRoot) | Out-Null
        foreach ($relative in (@($plan.sources) + @($plan.headers))) {
            [IO.File]::WriteAllText((Join-PathLiteral $mockSourceRoot $relative), "/* $relative v1 */`n")
        }
        $savedRepoRoot = $script:RepoRoot
        try {
            $script:RepoRoot = $fingerprintRoot
            $baseLock = New-W2FixtureLock
            $baseLock.components.libzip.patches = @()
            $baseFingerprint = Get-W2RecipeFingerprint $baseLock
            Assert-PinnedSha256 $baseFingerprint "W2 recipe fingerprint"
            if ($baseFingerprint -ne (Get-W2RecipeFingerprint $baseLock)) {
                throw "the W2 recipe fingerprint is not stable across two identical inputs"
            }
            $changedLock = New-W2FixtureLock
            $changedLock.components.libzip.patches = @()
            $changedLock.components.bc7enc.buildProfile.flags = @("/TC", "/O1", "/MT")
            if ($baseFingerprint -eq (Get-W2RecipeFingerprint $changedLock)) {
                throw "a changed bc7enc recipe did not move the W2 recipe fingerprint"
            }
            # The half that matters most: bc7enc has no archive hash, so an edited source file is
            # invisible to every other input of the fingerprint.
            [IO.File]::WriteAllText((Join-PathLiteral $mockSourceRoot $plan.sources[0]), "/* edited v2 */`n")
            if ($baseFingerprint -eq (Get-W2RecipeFingerprint $baseLock)) {
                throw "an edited in-tree bc7enc source did not move the W2 recipe fingerprint"
            }
            # A missing source must stop the wave rather than quietly fingerprint fewer files.
            [IO.File]::Delete((Join-PathLiteral $mockSourceRoot $plan.sources[0]))
            Expect-Failure {
                Get-W2RecipeFingerprint $baseLock
            } "in-tree source is missing and cannot be fingerprinted"
        }
        finally {
            $script:RepoRoot = $savedRepoRoot
        }
    }
    Run-Case "T-CMAKE-WAVE-SELECTION" {
        # The wave accepts exactly the three declared W2 components, in topological order.
        $lock = New-W2FixtureLock
        Expect-Failure { Invoke-W2Build $lock @("zlib") } "requires the component 'libzip'"
        Expect-Failure { Invoke-W2Build $lock @("zlib", "libzip") } "requires the component 'bc7enc'"
        Expect-Failure {
            Invoke-W2Build $lock @("zlib", "libzip", "bc7enc", "freetype")
        } "does not accept the component 'freetype'"
        Expect-Failure {
            Invoke-W2Build $lock @("libzip", "zlib", "bc7enc")
        } "topological component order zlib before libzip"
    }
    Run-Case "T-W2-WAVE-UNSUPPORTED-SYSTEM" {
        # CS-07 added the cl driver, so "cl" is no longer the unsupported case. The property under
        # test is unchanged: a system with no driver must be refused by the pre-flight, before any
        # work starts, rather than after half a prefix has been activated. "b2" stands in for it.
        $unknown = New-W2FixtureLock -Bc7encSystem "b2"
        $names = @("zlib", "libzip", "bc7enc")
        Expect-Failure { Invoke-W2Build $unknown $names } "not implemented yet for buildProfile\.system 'b2'"
        Expect-Failure { Invoke-W2Build $unknown $names } "components\.bc7enc"
        # With a driver present the same selection passes the pre-flight and it is then the recipe
        # gate that rejects it - which is what proves the refusal above came from the pre-flight.
        # The fixture's default bc7enc flags are the pre-CS-07 set, which the cl gate rejects.
        $cl = New-W2FixtureLock
        Expect-Failure { Invoke-W2Build $cl $names } "does not force the C language through /TC"
        # Routing is by system, not by name: the same component sent to the cmake gate fails on a
        # cmake rule instead, so the two gates cannot be confused for one another.
        $patched = New-W2FixtureLock -Bc7encSystem "cmake"
        Expect-Failure { Invoke-W2Build $patched $names } "does not pin the static release CRT"
    }
    Run-Case "T-PATH-RELATIVE-ANCHOR" {
        # F-W2-28: .NET resolves a relative path against [Environment]::CurrentDirectory, which
        # PowerShell does not keep in sync with Set-Location. Every relative path must be
        # anchored to the repository root instead.
        $saved = [Environment]::CurrentDirectory
        try {
            [Environment]::CurrentDirectory = $selfRoot
            $normalized = Get-NormalizedFullPath "deps-cache"
            if ($normalized -ne (Join-Path $script:RepoRoot "deps-cache")) {
                throw "relative path was not anchored to the repository root: $normalized"
            }
        }
        finally {
            [Environment]::CurrentDirectory = $saved
        }
        # The same closure at script scope: -CacheDir and -LockPath are anchored once, before
        # any consumer sees them. SelfTest redirects the cache, so the pre-redirect value is
        # what must be checked here.
        foreach ($pair in @(
            [pscustomobject]@{ label = "-CacheDir"; value = [string]$script:CacheDirBeforeSelfTest },
            [pscustomobject]@{ label = "-LockPath"; value = [string]$LockPath }
        )) {
            if (-not [IO.Path]::IsPathRooted($pair.value)) {
                throw "$($pair.label) was not anchored to an absolute path: '$($pair.value)'"
            }
        }
    }

    $report = [pscustomobject]@{
        schemaVersion = 1
        runId = $script:RunId
        mode = "SelfTest"
        startedAt = (Get-Date).ToUniversalTime().ToString("o")
        results = $results.ToArray()
        passed = @($results | Where-Object status -eq "PASS").Count
        failed = @($results | Where-Object status -eq "FAIL").Count
    }
    $reportPath = Join-Path $script:RunRoot "selftest.report.json"
    [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding($false)))
    if ($report.failed -gt 0) { Stop-BuildDeps "SelfTest failed $($report.failed) case(s); report: $reportPath" }
    Write-Output "PASS: SelfTest completed $($report.passed) cases; report: $reportPath"
}

function Write-RunReport([string]$Status, [string]$Message) {
    New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
    $report = [ordered]@{
        schemaVersion = 1
        runId = $script:RunId
        mode = $Mode
        wave = $Wave
        allowNetwork = [bool]$AllowNetwork
        status = $Status
        message = $Message
        events = $script:ReportEvents.ToArray()
    }
    if ($null -ne $script:ToolchainIdentity) {
        $identity = $script:ToolchainIdentity
        $toolchain = [ordered]@{
            schemaVersion = [int]$identity.schemaVersion
            toolsetFamily = [string]$identity.toolsetFamily
            arch = [string]$identity.arch
            crt = [string]$identity.crt
            config = [string]$identity.config
            vs = [ordered]@{
                productDisplayVersion = [string]$identity.vs.productDisplayVersion
                installationVersion = [string]$identity.vs.installationVersion
            }
            vcTools = [ordered]@{ version = [string]$identity.vcTools.version }
            cl = $identity.cl
            link = $identity.link
            lib = $identity.lib
            dumpbin = $identity.dumpbin
            msbuild = $identity.msbuild
            windowsSdk = $identity.windowsSdk
        }
        if (@($identity.PSObject.Properties.Name) -contains "cmake") {
            $toolchain["cmake"] = [ordered]@{ version = [string]$identity.cmake.version }
        }
        $toolchain["toolchainFingerprint"] = Get-ToolchainFingerprint $identity
        $discovery = [ordered]@{
            vs = [ordered]@{
                instanceId = [string]$identity.vs.instanceId
                installationPath = [string]$identity.vs.installationPath
            }
        }
        if (@($identity.PSObject.Properties.Name) -contains "cmake") {
            $discovery["cmake"] = [ordered]@{ path = [string]$identity.cmake.path }
        }
        $report["toolchain"] = $toolchain
        $report["discovery"] = $discovery
    }
    $path = Join-Path $script:RunRoot "builddeps.report.json"
    [IO.File]::WriteAllText($path, ($report | ConvertTo-Json -Depth 20), (New-Object Text.UTF8Encoding($false)))
}

try {
    # F-W2-28. -LockPath and -CacheDir may reach the script as relative strings. Anchor them
    # once, here, so that every later consumer agrees on one location: [IO.Path]::GetFullPath
    # inside Assert-PathUnder resolves against [Environment]::CurrentDirectory, while
    # New-Item -Path and the download step's -OutFile resolve against the PowerShell provider's
    # current location, and PowerShell does not keep the two in sync.
    $LockPath = Get-NormalizedFullPath $LockPath
    $CacheDir = Get-NormalizedFullPath $CacheDir
    if ($Mode -eq "SelfTest") {
        if ($AllowNetwork) { Stop-BuildDeps "SelfTest forbids -AllowNetwork" }
        Invoke-SelfTest
        exit 0
    }

    $lock = Read-DependencyLock $LockPath
    $selected = @(Get-SelectedComponentNames $lock $Wave)
    $ordered = @(Get-TopologicalOrder $lock.components $selected)
    Add-ReportEvent "dag" "" "PASS" "Selected component order" @{ components = $ordered }
    $script:ToolchainIdentity = Get-ToolchainIdentity

    switch ($Mode) {
        "Check" { Invoke-Check $lock $ordered }
        "Fetch" { Invoke-Fetch $lock $ordered; Write-Output "PASS: Fetch completed for wave '$Wave'." }
        "Build" { Invoke-Build $lock $ordered }
        "All" {
            Invoke-Fetch $lock $ordered
            Invoke-Build $lock $ordered
        }
    }
    Write-RunReport "PASS" "Completed"
}
catch {
    # F-W2-52: a failure must name its line. The CS08 W3 run died with a bare "cannot call a
    # method on a null-valued expression" and nothing else - the script stack trace turns such
    # a message from a riddle into an address.
    $failureStack = [string]$_.ScriptStackTrace
    try { Write-RunReport "FAIL" ("$($_.Exception.Message)`n$failureStack") } catch {}
    Write-Error -Message "$($_.Exception.Message)`n$failureStack"
    exit 1
}
