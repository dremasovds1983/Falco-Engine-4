[CmdletBinding()]
param(
    [ValidateSet("Capture", "Check", "SelfTest")]
    [string]$Mode = "Check",

    [string]$LogRoot = "",

    [string]$BaselinePath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($BaselinePath)) {
    $BaselinePath = Join-Path $PSScriptRoot "warning-baseline.json"
}

$targetLogs = [ordered]@{
    "Engine|modern" = "Engine.log"
    "Player|modern" = "Player.log"
    "Editor|modern" = "Editor.log"
    "ShadercSmoke|modern" = "ShadercSmoke.log"
}

function Write-Utf8Json {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $json = $Value | ConvertTo-Json -Depth 12
    [IO.File]::WriteAllText($Path, $json + "`n", (New-Object Text.UTF8Encoding($false)))
}

function Convert-ToRepoPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    $repoRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') + '\'
    if ($fullPath.StartsWith($repoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        return $fullPath.Substring($repoRoot.Length).Replace('\', '/')
    }
    return $fullPath.Replace('\', '/')
}

function Get-WarningClass {
    param(
        [Parameter(Mandatory = $true)][string]$Location,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $normalized = $Location.Replace('\', '/').ToLowerInvariant()
    if ($normalized -match '(^|/)(engine|editor|player|modernbgfxcompat)/') {
        if ($normalized -match '(^|/)engine/glm/') {
            return "thirdparty"
        }
        return "falco"
    }

    if ($normalized -match '(^|/)(out/deps|out/modern-bgfx|bgfx/bgfx/3rdparty|boost|bullet|carve|codecs|freeimage|freetype|libzip|mono|openal|sdl|assimp)/' -or
        $normalized -match '^c:/program files') {
        return "thirdparty"
    }

    if ($Location -ieq "LINK") {
        if ($Message -match '(?i)(Engine\.lib|Editor\.obj|Player\.obj|ModernBgfxCompat)') {
            return "falco"
        }
        return "thirdparty"
    }

    # Unknown compiler paths are kept visible and conservatively attributed to Falco.
    return "falco"
}

function Get-WarningRecords {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string[]]$Lines,
        [Parameter(Mandatory = $true)][string]$Target
    )

    $records = New-Object System.Collections.ArrayList
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($rawLine in $Lines) {
        $line = ([string]$rawLine).Trim()
        $match = [regex]::Match($line,
            '^(?<location>.+?)(?:\((?<line>\d+)(?:,\d+)?\))?:\s+warning\s+(?<id>(?:C|LNK)\d+):\s*(?<message>.*?)(?:\s+\[[^\]]+\.vcxproj\])?$')
        if (-not $match.Success) {
            continue
        }

        $location = $match.Groups['location'].Value.Trim()
        $lineNumber = if ($match.Groups['line'].Success) { [int]$match.Groups['line'].Value } else { 0 }
        $id = $match.Groups['id'].Value.ToUpperInvariant()
        $message = $match.Groups['message'].Value.Trim()
        $identityTail = if ($lineNumber -eq 0) { $message } else { "" }
        $key = "$Target|$id|$location|$lineNumber|$identityTail"
        if (-not $seen.Add($key)) {
            continue
        }

        [void]$records.Add([pscustomobject][ordered]@{
            target = $Target
            id = $id
            class = Get-WarningClass -Location $location -Message $message
            location = $location.Replace('\', '/')
            line = $lineNumber
            message = $message
        })
    }
    return @($records)
}

function Get-TargetSummary {
    param([Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records)

    $byId = [ordered]@{}
    foreach ($idGroup in @($Records | Group-Object id | Sort-Object Name)) {
        $falco = @($idGroup.Group | Where-Object { $_.class -eq "falco" }).Count
        $thirdparty = @($idGroup.Group | Where-Object { $_.class -eq "thirdparty" }).Count
        $byId[$idGroup.Name] = [ordered]@{
            falco = $falco
            thirdparty = $thirdparty
        }
    }

    return [ordered]@{
        total = @($Records).Count
        byId = $byId
    }
}

function Get-SuppressionRecords {
    $extensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.vcxproj', '.props', '.targets')
    $roots = @('Engine', 'Editor', 'Player', 'ModernBgfxCompat')
    $counts = @{}

    foreach ($root in $roots) {
        $rootPath = Join-Path $PSScriptRoot $root
        foreach ($file in Get-ChildItem -LiteralPath $rootPath -Recurse -File) {
            if ($extensions -notcontains $file.Extension.ToLowerInvariant()) {
                continue
            }

            $repoPath = Convert-ToRepoPath $file.FullName
            foreach ($line in [IO.File]::ReadAllLines($file.FullName)) {
                foreach ($match in [regex]::Matches($line, '(?i)(?:/W0)(?=\s|<|$)')) {
                    $key = "$repoPath|compiler-option|W0"
                    $counts[$key] = 1 + [int]$counts[$key]
                }
                foreach ($match in [regex]::Matches($line, '(?i)/wd(?<code>\d+)')) {
                    $code = $match.Groups['code'].Value
                    $key = "$repoPath|compiler-option|$code"
                    $counts[$key] = 1 + [int]$counts[$key]
                }
                foreach ($match in [regex]::Matches($line, '(?i)#\s*pragma\s+warning\s*\(\s*disable\s*:\s*(?<codes>[^\)]+)\)')) {
                    foreach ($code in @($match.Groups['codes'].Value -split '[,\s]+' | Where-Object { $_ -match '^\d+$' })) {
                        $key = "$repoPath|pragma-disable|$code"
                        $counts[$key] = 1 + [int]$counts[$key]
                    }
                }
                foreach ($match in [regex]::Matches($line, '(?i)<DisableSpecificWarnings>(?<codes>.*?)</DisableSpecificWarnings>')) {
                    foreach ($code in @($match.Groups['codes'].Value -split '[;,\s]+' | Where-Object { $_ -match '^\d+$' })) {
                        $key = "$repoPath|msbuild-disable|$code"
                        $counts[$key] = 1 + [int]$counts[$key]
                    }
                }
            }
        }
    }

    $records = foreach ($key in @($counts.Keys | Sort-Object)) {
        $parts = $key -split '\|', 3
        [ordered]@{
            path = $parts[0]
            kind = $parts[1]
            code = $parts[2]
            count = [int]$counts[$key]
        }
    }
    return @($records)
}

function Get-RunSummary {
    param([Parameter(Mandatory = $true)][string]$Root)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "Warning log root does not exist: $Root"
    }

    $targets = [ordered]@{}
    $logEvidence = [ordered]@{}
    foreach ($target in $targetLogs.Keys) {
        $logPath = Join-Path $Root $targetLogs[$target]
        if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) {
            throw "Full warning gate requires $($targetLogs[$target]): $logPath"
        }

        $lines = [IO.File]::ReadAllLines($logPath)
        if (-not (@($lines | Where-Object { $_ -match '(?i)\bRebuild\b' }).Count -gt 0)) {
            throw "Warning baseline accepts only full Rebuild logs: $logPath"
        }

        $records = @(Get-WarningRecords -Lines $lines -Target $target)
        $targets[$target] = Get-TargetSummary -Records $records
        $item = Get-Item -LiteralPath $logPath
        $logEvidence[$target] = [ordered]@{
            path = Convert-ToRepoPath $logPath
            bytes = [int64]$item.Length
            sha256 = (Get-FileHash -LiteralPath $logPath -Algorithm SHA256).Hash.ToUpperInvariant()
        }
    }

    return [ordered]@{
        runId = Split-Path -Leaf ([IO.Path]::GetFullPath($Root).TrimEnd('\'))
        targets = $targets
        logs = $logEvidence
        suppressions = @(Get-SuppressionRecords)
    }
}

function Get-PropertyNames {
    param($Object)
    if ($null -eq $Object) { return @() }
    return @($Object.PSObject.Properties | ForEach-Object { $_.Name })
}

function Compare-RunToBaseline {
    param(
        [Parameter(Mandatory = $true)]$Baseline,
        [Parameter(Mandatory = $true)]$Run
    )

    $violations = New-Object System.Collections.ArrayList
    $deltas = New-Object System.Collections.ArrayList
    foreach ($target in $targetLogs.Keys) {
        $baselineTarget = $Baseline.targets.$target
        $currentTarget = $Run.targets[$target]
        if ($null -eq $baselineTarget) {
            [void]$violations.Add("baseline target is missing: $target")
            continue
        }

        if ([int]$currentTarget.total -gt [int]$baselineTarget.total) {
            [void]$violations.Add("$target total grew: $($baselineTarget.total) -> $($currentTarget.total)")
        }
        if ([int]$currentTarget.total -ne [int]$baselineTarget.total) {
            [void]$deltas.Add("$target total $($baselineTarget.total) -> $($currentTarget.total)")
        }

        $baselineIds = @(Get-PropertyNames $baselineTarget.byId)
        $currentIds = @($currentTarget.byId.Keys)
        $ids = @(($baselineIds + $currentIds) | Sort-Object -Unique)
        foreach ($id in $ids) {
            $baselineProperty = $baselineTarget.byId.PSObject.Properties[$id]
            $baselineId = if ($null -eq $baselineProperty) { $null } else { $baselineProperty.Value }
            $currentId = if ($currentTarget.byId.Contains($id)) { $currentTarget.byId[$id] } else { $null }
            if ($null -eq $baselineId -and $null -ne $currentId) {
                [void]$violations.Add("$target introduced warning id $id")
                continue
            }

            foreach ($class in @('falco', 'thirdparty')) {
                $before = if ($null -eq $baselineId) { 0 } else { [int]$baselineId.$class }
                $after = if ($null -eq $currentId) { 0 } else { [int]$currentId[$class] }
                if ($after -gt $before) {
                    [void]$violations.Add("$target $id/$class grew: $before -> $after")
                }
                if ($after -ne $before) {
                    [void]$deltas.Add("$target $id/$class $before -> $after")
                }
            }
        }
    }

    $allowedSuppressions = @{}
    foreach ($entry in @($Baseline.suppressionAllowlist)) {
        $allowedSuppressions["$($entry.path)|$($entry.kind)|$($entry.code)"] = [int]$entry.count
    }
    foreach ($entry in @($Run.suppressions)) {
        $key = "$($entry.path)|$($entry.kind)|$($entry.code)"
        $allowed = if ($allowedSuppressions.ContainsKey($key)) { [int]$allowedSuppressions[$key] } else { 0 }
        if ([int]$entry.count -gt $allowed) {
            [void]$violations.Add("new warning suppression: $key count $($entry.count), allowed $allowed")
        }
    }

    return [ordered]@{
        verdict = if ($violations.Count -eq 0) { "PASS" } else { "FAIL" }
        violations = @($violations)
        deltas = @($deltas)
    }
}

function Invoke-SelfTest {
    $sample = @(
        'D:\repo\Engine\Foo.cpp(10,3): warning C4244: conversion [D:\repo\Engine\Engine.vcxproj]',
        'D:\repo\Engine\Foo.cpp(10,3): warning C4244: with [D:\repo\Engine\Engine.vcxproj]',
        'D:\repo\out\deps\w5\include\Third.h(4,1): warning C4267: conversion [D:\repo\Engine\Engine.vcxproj]',
        'LINK : warning LNK4099: first external library [D:\repo\Player\Player.vcxproj]',
        'LINK : warning LNK4099: second external library [D:\repo\Player\Player.vcxproj]'
    )
    $records = @(Get-WarningRecords -Lines $sample -Target 'Engine|modern')
    if ($records.Count -ne 4) { throw "SelfTest expected 4 unique diagnostics, got $($records.Count)" }
    $summary = Get-TargetSummary -Records $records
    if ($summary.byId.C4244.falco -ne 1 -or $summary.byId.C4267.thirdparty -ne 1 -or
        $summary.byId.LNK4099.thirdparty -ne 2) {
        throw "SelfTest classification or aggregation failed"
    }

    $baseline = @'
{"targets":{"Engine|modern":{"total":1,"byId":{"C4244":{"falco":1,"thirdparty":0}}},"Player|modern":{"total":0,"byId":{}},"Editor|modern":{"total":0,"byId":{}},"ShadercSmoke|modern":{"total":0,"byId":{}}},"suppressionAllowlist":[]}
'@ | ConvertFrom-Json
    $emptyTarget = [ordered]@{ total = 0; byId = [ordered]@{} }
    $run = [ordered]@{
        targets = [ordered]@{
            "Engine|modern" = [ordered]@{
                total = 2
                byId = [ordered]@{ C4244 = [ordered]@{ falco = 2; thirdparty = 0 } }
            }
            "Player|modern" = $emptyTarget
            "Editor|modern" = $emptyTarget
            "ShadercSmoke|modern" = $emptyTarget
        }
        suppressions = @()
    }
    $growth = Compare-RunToBaseline -Baseline $baseline -Run $run
    if ($growth.verdict -ne 'FAIL' -or @($growth.violations | Where-Object { $_ -match 'C4244/falco grew' }).Count -ne 1) {
        throw "SelfTest failed to reject warning growth"
    }

    $run.targets['Engine|modern'] = [ordered]@{
        total = 1
        byId = [ordered]@{ C4999 = [ordered]@{ falco = 1; thirdparty = 0 } }
    }
    $newId = Compare-RunToBaseline -Baseline $baseline -Run $run
    if ($newId.verdict -ne 'FAIL' -or @($newId.violations | Where-Object { $_ -match 'introduced warning id C4999' }).Count -ne 1) {
        throw "SelfTest failed to reject a new warning id"
    }

    $run.targets['Engine|modern'] = [ordered]@{
        total = 1
        byId = [ordered]@{ C4244 = [ordered]@{ falco = 1; thirdparty = 0 } }
    }
    $run.suppressions = @([ordered]@{ path = 'Engine/New.cpp'; kind = 'pragma-disable'; code = '4244'; count = 1 })
    $suppression = Compare-RunToBaseline -Baseline $baseline -Run $run
    if ($suppression.verdict -ne 'FAIL' -or @($suppression.violations | Where-Object { $_ -match 'new warning suppression' }).Count -ne 1) {
        throw "SelfTest failed to reject a new warning suppression"
    }

    Write-Host "PASS: WarningBaseline SelfTest completed parser, classifier, growth, new-id, and suppression gates."
}

if ($Mode -eq 'SelfTest') {
    Invoke-SelfTest
    exit 0
}

if ([string]::IsNullOrWhiteSpace($LogRoot)) {
    throw "-LogRoot is required for $Mode mode."
}

$run = Get-RunSummary -Root $LogRoot
if ($Mode -eq 'Capture') {
    $baseline = [ordered]@{
        schemaVersion = 1
        capturedFromRunId = $run.runId
        capturePolicy = "Explicit Capture only; never updated by Check or by the build."
        classifierRules = @(
            [ordered]@{ match = "Engine|Editor|Player|ModernBgfxCompat paths, except Engine/glm"; class = "falco" },
            [ordered]@{ match = "out/deps|out/modern-bgfx|Bgfx/bgfx/3rdparty|dependency roots|toolchain paths"; class = "thirdparty" },
            [ordered]@{ match = "unknown compiler path"; class = "falco" }
        )
        targets = $run.targets
        suppressionAllowlist = $run.suppressions
        captureLogs = $run.logs
    }
    Write-Utf8Json -Value $baseline -Path $BaselinePath
    Write-Host "PASS: warning baseline captured explicitly from $($run.runId): $BaselinePath"
    exit 0
}

if (-not (Test-Path -LiteralPath $BaselinePath -PathType Leaf)) {
    throw "Warning baseline is missing: $BaselinePath"
}
$baseline = Get-Content -LiteralPath $BaselinePath -Raw -Encoding UTF8 | ConvertFrom-Json
if ([int]$baseline.schemaVersion -ne 1) {
    throw "Unsupported warning baseline schemaVersion: $($baseline.schemaVersion)"
}

$comparison = Compare-RunToBaseline -Baseline $baseline -Run $run
$report = [ordered]@{
    schemaVersion = 1
    baselineRunId = [string]$baseline.capturedFromRunId
    runId = $run.runId
    fullRebuildValidated = $true
    verdict = $comparison.verdict
    violations = $comparison.violations
    deltas = $comparison.deltas
    targets = $run.targets
    suppressions = $run.suppressions
    logs = $run.logs
}
$reportPath = Join-Path $LogRoot "warning-report.json"
Write-Utf8Json -Value $report -Path $reportPath

if ($comparison.verdict -ne 'PASS') {
    foreach ($violation in $comparison.violations) {
        Write-Host "FAIL: $violation"
    }
    throw "Warning baseline regression detected. See $reportPath"
}

foreach ($delta in $comparison.deltas) {
    Write-Host "IMPROVEMENT: $delta"
}
Write-Host "PASS: warning baseline accepted run $($run.runId); report: $reportPath"
