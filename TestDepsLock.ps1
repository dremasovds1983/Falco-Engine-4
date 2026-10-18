[CmdletBinding()]
param(
    [string]$LockPath = "",
    [string]$SchemaPath = "",
    [string]$LegacyLockPath = "",
    [ValidateSet("", "Preparation", "Production")]
    [string]$ValidationMode = "",
    [switch]$FixtureMode
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $LockPath) { $LockPath = Join-Path $PSScriptRoot "deps.lock.json" }
if (-not $SchemaPath) { $SchemaPath = Join-Path $PSScriptRoot "deps.lock.schema.json" }
if (-not $LegacyLockPath) { $LegacyLockPath = Join-Path $PSScriptRoot "modern-bgfx.lock.json" }

function Stop-Validation([string]$Message) {
    throw "deps.lock validation failed: $Message"
}

function Read-Json([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Stop-Validation "$Label not found: $Path"
    }
    try {
        return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    }
    catch {
        Stop-Validation "$Label is not valid JSON: $($_.Exception.Message)"
    }
}

function Assert-ObjectShape($Object, [string[]]$Allowed, [string[]]$Required, [string]$Where) {
    if ($null -eq $Object -or $Object -isnot [pscustomobject]) {
        Stop-Validation "$Where must be an object"
    }
    $names = @($Object.PSObject.Properties.Name)
    foreach ($name in $names) {
        if ($Allowed -notcontains $name) {
            Stop-Validation "$Where contains unknown field '$name'"
        }
    }
    foreach ($name in $Required) {
        if ($names -notcontains $name) {
            Stop-Validation "$Where is missing required field '$name'"
        }
    }
}

function Assert-NonEmptyString($Value, [string]$Where) {
    if ($Value -isnot [string] -or [string]::IsNullOrWhiteSpace($Value)) {
        Stop-Validation "$Where must be a non-empty string"
    }
}

function Assert-Integer($Value, [string]$Where, [bool]$PositiveRequired) {
    if ($Value -isnot [int] -and $Value -isnot [long]) {
        Stop-Validation "$Where must be an integer"
    }
    if ($Value -lt 0 -or ($PositiveRequired -and $Value -eq 0)) {
        Stop-Validation "$Where has an invalid byte count '$Value'"
    }
}

function Assert-Sha256($Value, [string]$Where, [bool]$AllowPlaceholder) {
    Assert-NonEmptyString $Value $Where
    if ($Value -match "^[A-Fa-f0-9]{64}$") {
        return
    }
    if ($AllowPlaceholder -and $Value -in @("TBD-on-approval", "TBD-first-build")) {
        return
    }
    Stop-Validation "$Where must be a 64-digit SHA-256$(
        if ($AllowPlaceholder) { ' or an approved preparation placeholder' } else { '' }
    )"
}

function Assert-RelativePath($Value, [string]$Where) {
    Assert-NonEmptyString $Value $Where
    if ([IO.Path]::IsPathRooted($Value) -or $Value -match "^[A-Za-z]:" -or $Value -match "(^|[\\/])\.\.([\\/]|$)") {
        Stop-Validation "$Where must be a repository-relative path without '..': $Value"
    }
}

function Assert-Array($Value, [string]$Where, [bool]$NonEmpty) {
    if ($null -eq $Value -or $Value -is [string] -or $Value -isnot [System.Collections.IEnumerable]) {
        Stop-Validation "$Where must be an array"
    }
    if ($NonEmpty -and @($Value).Count -eq 0) {
        Stop-Validation "$Where must not be empty"
    }
}

$resolvedLockPath = (Resolve-Path -LiteralPath $LockPath).Path
if ($FixtureMode) {
    $fixtureRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "out\deps\_selftest")).TrimEnd('\', '/')
    if (-not $resolvedLockPath.StartsWith($fixtureRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Stop-Validation "FixtureMode is restricted to generated locks under '$fixtureRoot'"
    }
}

$lock = Read-Json $resolvedLockPath "dependency lock"
$schema = Read-Json $SchemaPath "dependency lock schema"
$legacy = if ($FixtureMode) { $null } else { Read-Json $LegacyLockPath "modern bgfx legacy lock" }
$root = Split-Path -Parent $resolvedLockPath

Assert-ObjectShape $lock @('$schema', 'schemaVersion', 'approvedBy', 'approvedAt', 'mode', 'toolchain', 'components') @('schemaVersion', 'approvedBy', 'approvedAt', 'mode', 'toolchain', 'components') "root"
if ($lock.schemaVersion -ne 3) { Stop-Validation "schemaVersion must be 3" }
Assert-NonEmptyString $lock.approvedBy "approvedBy"
$date = [DateTime]::MinValue
if (-not [DateTime]::TryParseExact($lock.approvedAt, "yyyy-MM-dd", [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::None, [ref]$date)) {
    Stop-Validation "approvedAt must use YYYY-MM-DD"
}
if ($lock.mode -notin @("preparation", "production")) {
    Stop-Validation "mode must be 'preparation' or 'production'"
}

$effectiveMode = if ($ValidationMode) { $ValidationMode.ToLowerInvariant() } else { $lock.mode }
$production = $effectiveMode -eq "production"

Assert-ObjectShape $lock.toolchain @('vs', 'toolset', 'arch', 'crt', 'config') @('vs', 'toolset', 'arch', 'crt', 'config') "toolchain"
Assert-NonEmptyString $lock.toolchain.vs "toolchain.vs"
if ($lock.toolchain.toolset -ne "v143" -or $lock.toolchain.arch -ne "x64" -or $lock.toolchain.crt -ne "MT" -or $lock.toolchain.config -ne "Release") {
    Stop-Validation "toolchain must be v143/x64/MT/Release"
}

if (-not $FixtureMode) {
    Assert-ObjectShape $legacy @('schemaVersion', 'capturedAt', 'source', 'repositories', 'configuration', 'artifacts') @('schemaVersion', 'repositories', 'artifacts') "modern-bgfx legacy lock"
    if ($legacy.schemaVersion -ne 2) {
        Stop-Validation "modern-bgfx legacy lock schemaVersion must be 2"
    }
    if ($legacy.repositories -isnot [pscustomobject] -or $legacy.artifacts -isnot [pscustomobject]) {
        Stop-Validation "modern-bgfx legacy lock repositories and artifacts must be objects"
    }
}

if ($null -eq $lock.components -or $lock.components -isnot [pscustomobject]) {
    Stop-Validation "components must be a non-empty object"
}
$componentNames = @($lock.components.PSObject.Properties.Name)
if ($componentNames.Count -eq 0) { Stop-Validation "components must not be empty" }

$componentAllowed = @('kind', 'wave', 'upstream', 'version', 'commit', 'archive', 'licenseFiles', 'patches', 'buildProfile', 'outputs', 'dependsOn', 'sourceOffer', 'expectedExecutables')
$componentRequired = @('kind', 'wave', 'upstream', 'version', 'licenseFiles', 'patches', 'buildProfile', 'outputs', 'dependsOn')
$waves = @('core', 'W0', 'W1', 'W2', 'W3', 'W4', 'W5', 'W6', 'W7', 'W8', 'W8b', 'W9', 'W10')
$systems = @('b2', 'cl', 'cmake', 'genie', 'in-tree', 'msbuild', 'msiexec-extract')
$outputOwners = @{}
$outputCount = 0

foreach ($componentName in $componentNames) {
    if ($componentName -cnotmatch "^[a-z0-9][a-z0-9._-]*$") {
        Stop-Validation "invalid component name '$componentName'"
    }
    $component = $lock.components.$componentName
    $where = "components.$componentName"
    Assert-ObjectShape $component $componentAllowed $componentRequired $where
    if ($component.kind -notin @('git', 'archive', 'binary', 'in-tree')) { Stop-Validation "$where.kind is invalid" }
    if ($component.wave -notin $waves) { Stop-Validation "$where.wave is invalid" }
    Assert-NonEmptyString $component.upstream "$where.upstream"
    Assert-NonEmptyString $component.version "$where.version"

    $componentFields = @($component.PSObject.Properties.Name)
    if ($component.kind -eq 'git') {
        if ($componentFields -notcontains 'commit' -or $component.commit -cnotmatch "^[A-Fa-f0-9]{40}$") {
            Stop-Validation "$where.commit must pin a 40-digit Git commit"
        }
    }
    if ($component.kind -in @('archive', 'binary') -and $componentFields -notcontains 'archive') {
        Stop-Validation "$where.archive is required for kind '$($component.kind)'"
    }

    if ($componentFields -contains 'archive') {
        Assert-ObjectShape $component.archive @('url', 'sha256', 'bytes') @('url', 'sha256', 'bytes') "$where.archive"
        Assert-NonEmptyString $component.archive.url "$where.archive.url"
        Assert-Sha256 $component.archive.sha256 "$where.archive.sha256" (-not $production)
        Assert-Integer $component.archive.bytes "$where.archive.bytes" $production
    }

    Assert-Array $component.licenseFiles "$where.licenseFiles" $true
    for ($i = 0; $i -lt @($component.licenseFiles).Count; $i++) {
        $item = @($component.licenseFiles)[$i]
        $itemWhere = "$where.licenseFiles[$i]"
        Assert-ObjectShape $item @('pathInSource', 'sha256', 'license', 'optional') @('pathInSource', 'sha256', 'license') $itemWhere
        Assert-RelativePath $item.pathInSource "$itemWhere.pathInSource"
        Assert-Sha256 $item.sha256 "$itemWhere.sha256" (-not $production)
        Assert-NonEmptyString $item.license "$itemWhere.license"
        if (@($item.PSObject.Properties.Name) -contains 'optional' -and $item.optional -isnot [bool]) {
            Stop-Validation "$itemWhere.optional must be boolean"
        }
    }

    Assert-Array $component.patches "$where.patches" $false
    for ($i = 0; $i -lt @($component.patches).Count; $i++) {
        $patch = @($component.patches)[$i]
        $patchWhere = "$where.patches[$i]"
        Assert-ObjectShape $patch @('file', 'sha256', 'reason') @('file', 'sha256', 'reason') $patchWhere
        Assert-RelativePath $patch.file "$patchWhere.file"
        Assert-Sha256 $patch.sha256 "$patchWhere.sha256" (-not $production)
        Assert-NonEmptyString $patch.reason "$patchWhere.reason"
        $patchPath = Join-Path $root $patch.file
        if (-not (Test-Path -LiteralPath $patchPath -PathType Leaf)) { Stop-Validation "$patchWhere.file not found: $($patch.file)" }
        if ($patch.sha256 -match "^[A-Fa-f0-9]{64}$") {
            $actualPatchSha = (Get-FileHash -LiteralPath $patchPath -Algorithm SHA256).Hash
            if ($actualPatchSha -ne $patch.sha256) { Stop-Validation "$patchWhere SHA-256 mismatch" }
        }
    }

    Assert-ObjectShape $component.buildProfile @('system', 'flags', 'inherit') @('system', 'flags', 'inherit') "$where.buildProfile"
    if ($component.buildProfile.system -notin $systems) { Stop-Validation "$where.buildProfile.system is invalid" }
    Assert-Array $component.buildProfile.flags "$where.buildProfile.flags" $false
    foreach ($flag in @($component.buildProfile.flags)) { Assert-NonEmptyString $flag "$where.buildProfile.flags[]" }
    if ($component.buildProfile.inherit -ne 'toolchain') { Stop-Validation "$where.buildProfile.inherit must be 'toolchain'" }

    Assert-Array $component.outputs "$where.outputs" $true
    for ($i = 0; $i -lt @($component.outputs).Count; $i++) {
        $output = @($component.outputs)[$i]
        $outputWhere = "$where.outputs[$i]"
        Assert-ObjectShape $output @('path', 'sha256', 'bytes') @('path', 'sha256', 'bytes') $outputWhere
        Assert-RelativePath $output.path "$outputWhere.path"
        Assert-Sha256 $output.sha256 "$outputWhere.sha256" (-not $production)
        Assert-Integer $output.bytes "$outputWhere.bytes" $production
        $key = $output.path.ToLowerInvariant().Replace('\', '/')
        if ($outputOwners.ContainsKey($key)) { Stop-Validation "duplicate output '$($output.path)' in '$componentName' and '$($outputOwners[$key])'" }
        $outputOwners[$key] = $componentName
        $outputCount++
    }

    Assert-Array $component.dependsOn "$where.dependsOn" $false
    $seenDependencies = @{}
    foreach ($dependency in @($component.dependsOn)) {
        Assert-NonEmptyString $dependency "$where.dependsOn[]"
        if ($dependency -notin $componentNames) { Stop-Validation "$where depends on unknown component '$dependency'" }
        if ($dependency -eq $componentName) { Stop-Validation "$where cannot depend on itself" }
        if ($seenDependencies.ContainsKey($dependency)) { Stop-Validation "$where contains duplicate dependency '$dependency'" }
        $seenDependencies[$dependency] = $true
    }

    if ($componentFields -contains 'sourceOffer') {
        Assert-ObjectShape $component.sourceOffer @('url', 'sha256') @('url', 'sha256') "$where.sourceOffer"
        Assert-NonEmptyString $component.sourceOffer.url "$where.sourceOffer.url"
        Assert-Sha256 $component.sourceOffer.sha256 "$where.sourceOffer.sha256" (-not $production)
    }

    if ($componentFields -contains 'expectedExecutables') {
        Assert-Array $component.expectedExecutables "$where.expectedExecutables" $false
        $seenExecutables = @{}
        foreach ($expectedExecutable in @($component.expectedExecutables)) {
            Assert-RelativePath $expectedExecutable "$where.expectedExecutables[]"
            $executableKey = $expectedExecutable.ToLowerInvariant().Replace('\', '/')
            if ($seenExecutables.ContainsKey($executableKey)) {
                Stop-Validation "$where contains duplicate expected executable '$expectedExecutable'"
            }
            $seenExecutables[$executableKey] = $true
        }
    }
}

$visitState = @{}
function Visit-Component([string]$Name, [string[]]$Trail) {
    if ($visitState[$Name] -eq 1) { Stop-Validation "dependency cycle: $(($Trail + $Name) -join ' -> ')" }
    if ($visitState[$Name] -eq 2) { return }
    $visitState[$Name] = 1
    foreach ($dependency in @($lock.components.$Name.dependsOn)) {
        Visit-Component $dependency ($Trail + $Name)
    }
    $visitState[$Name] = 2
}
foreach ($componentName in $componentNames) { Visit-Component $componentName @() }

if (-not $FixtureMode) {
    foreach ($coreName in @('bx', 'bimg', 'bgfx')) {
        if ($coreName -notin $componentNames) { Stop-Validation "legacy compatibility component '$coreName' is missing" }
        $legacyCommit = $legacy.repositories.$coreName.commit
        if ($lock.components.$coreName.commit -ne $legacyCommit) {
            Stop-Validation "$coreName commit differs from modern-bgfx.lock.json"
        }
    }

    $legacyArtifacts = @($legacy.artifacts.PSObject.Properties)
    foreach ($artifactProperty in $legacyArtifacts) {
        $artifactName = $artifactProperty.Name
        $artifact = $artifactProperty.Value
        $matchedArtifacts = @()
        foreach ($coreName in @('bx', 'bimg', 'bgfx')) {
            $matchedArtifacts += @($lock.components.$coreName.outputs | Where-Object { [IO.Path]::GetFileName($_.path) -eq $artifactName })
        }
        if ($matchedArtifacts.Count -ne 1) { Stop-Validation "legacy artifact '$artifactName' must map to exactly one core output" }
        if ($matchedArtifacts[0].sha256 -ne $artifact.sha256 -or [long]$matchedArtifacts[0].bytes -ne [long]$artifact.bytes) {
            Stop-Validation "legacy artifact '$artifactName' hash/size differs from modern-bgfx.lock.json"
        }
    }
}

# This is intentionally dependency-free. It also verifies that the checked-in
# schema remains the schema-3 contract that this validator implements.
if ($schema.'$schema' -ne 'https://json-schema.org/draft/2020-12/schema' -or $schema.properties.schemaVersion.const -ne 3) {
    Stop-Validation "deps.lock.schema.json is not the expected draft-2020-12 schema-3 contract"
}
$definitions = @($schema.'$defs'.PSObject.Properties.Name)
foreach ($requiredDefinition in @('sha256', 'wave', 'relativePath', 'toolchain', 'archive', 'licenseFile', 'patch', 'buildProfile', 'output', 'sourceOffer', 'component')) {
    if ($requiredDefinition -notin $definitions) { Stop-Validation "schema definition '$requiredDefinition' is missing" }
}

$compatibilityStatus = if ($FixtureMode) { "fixture mode" } else { "legacy core matches" }
Write-Output "PASS: deps.lock schema 3 ($effectiveMode); $($componentNames.Count) components; $outputCount unique outputs; DAG valid; $compatibilityStatus."
