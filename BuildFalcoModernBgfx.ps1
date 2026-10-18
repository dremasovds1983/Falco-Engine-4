[CmdletBinding()]
param(
    [ValidateSet("Check", "Engine", "ShaderSmoke", "CSGSmoke", "Player", "Editor", "All")]
    [string]$Mode = "Check",

    [string]$OutputRoot = "",

    [string]$DependencyLockPath = "",
    [string]$ModernLockPath = "",
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $PSScriptRoot "out\falco"
}

$modernRoot = Join-Path $PSScriptRoot "out\modern-bgfx"
$modernSourceRoot = Join-Path $modernRoot "src"
$modernBinaryRoot = Join-Path $modernSourceRoot "bgfx\.build\win64_vs2022\bin"
$lockPath = Join-Path $PSScriptRoot "modern-bgfx.lock.json"
$depsLockPath = Join-Path $PSScriptRoot "deps.lock.json"
if ($DependencyLockPath) { $depsLockPath = $DependencyLockPath }
if ($ModernLockPath) { $lockPath = $ModernLockPath }
$warningBaselineScript = Join-Path $PSScriptRoot "WarningBaseline.ps1"
$logRoot = Join-Path $OutputRoot "logs"
$runId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), $PID
$runLogRoot = Join-Path $logRoot $runId
$buildTarget = if ($Rebuild) { "Rebuild" } else { "Build" }

function Normalize-ProcessPathVariable {
    $pathVariables = @(
        [System.Environment]::GetEnvironmentVariables("Process").GetEnumerator() |
            Where-Object { $_.Key -ieq "PATH" }
    )
    if ($pathVariables.Count -le 1) {
        return
    }

    $pathValue = [string]$pathVariables[0].Value
    foreach ($pathVariable in $pathVariables) {
        [System.Environment]::SetEnvironmentVariable(
            [string]$pathVariable.Key,
            $null,
            "Process"
        )
    }
    [System.Environment]::SetEnvironmentVariable("Path", $pathValue, "Process")
    Write-Host "Normalized duplicate PATH variables for child build tools."
}

function Get-Vs2022MsBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw "vswhere.exe was not found. Install Visual Studio 2022 with Desktop development with C++."
    }

    $installationPath = & $vswhere -latest -products * -version "[17.0,18.0)" `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installationPath) {
        throw "Visual Studio 2022 C++ x64 tools were not found."
    }

    $msbuild = Join-Path $installationPath "MSBuild\Current\Bin\MSBuild.exe"
    if (-not (Test-Path -LiteralPath $msbuild)) {
        throw "MSBuild was not found at $msbuild"
    }
    return $msbuild
}

function Assert-PinnedModernBgfx {
    if (-not (Test-Path -LiteralPath $lockPath)) {
        throw "Modern bgfx lock is missing: $lockPath"
    }

    $lock = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($name in @("bx", "bimg", "bgfx")) {
        $repository = Join-Path $modernSourceRoot $name
        $expected = [string]$lock.repositories.$name.commit
        if (-not (Test-Path -LiteralPath (Join-Path $repository ".git"))) {
            throw "$name checkout is missing: $repository"
        }

        $actualOutput = & git -c "safe.directory=$repository" -C $repository rev-parse HEAD
        $actual = if ($null -eq $actualOutput) { "" } else { ([string]$actualOutput).Trim() }
        if ($LASTEXITCODE -ne 0 -or $actual -ne $expected) {
            throw "$name is not pinned to $expected (actual: $actual)."
        }
    }

    if ($lock.schemaVersion -ne 2 -or $null -eq $lock.artifacts) {
        throw "Modern bgfx lock must use schemaVersion 2 with artifact hashes."
    }

    foreach ($artifact in @("bxRelease.lib", "bimgRelease.lib", "bimg_decodeRelease.lib", "bgfxRelease.lib", "shadercRelease.exe")) {
        $artifactPath = Join-Path $modernBinaryRoot $artifact
        if (-not (Test-Path -LiteralPath $artifactPath)) {
            throw "Modern bgfx artifact is missing: $artifactPath"
        }

        $expectedArtifact = $lock.artifacts.$artifact
        if ($null -eq $expectedArtifact) {
            throw "Modern bgfx artifact is not pinned in the lock: $artifact"
        }

        $actualItem = Get-Item -LiteralPath $artifactPath
        $expectedBytes = [int64]$expectedArtifact.bytes
        if ($actualItem.Length -ne $expectedBytes) {
            throw "$artifact size mismatch (expected $expectedBytes, actual $($actualItem.Length))."
        }

        $expectedHash = ([string]$expectedArtifact.sha256).ToUpperInvariant()
        $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $artifactPath).Hash.ToUpperInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "$artifact SHA-256 mismatch (expected $expectedHash, actual $actualHash)."
        }
    }
}

function Test-Prerequisites {
    Normalize-ProcessPathVariable
    Assert-PinnedModernBgfx
    & (Join-Path $PSScriptRoot "PrepareManifoldHeaders.ps1") -DependencyLockPath $depsLockPath
    $msbuild = Get-Vs2022MsBuild

    Write-Host "Modern bgfx: $modernRoot"
    Write-Host "Falco output: $OutputRoot"
    Write-Host "MSBuild: $msbuild"
    Write-Host "Renderer target: Direct3D11"
    Write-Host "Build target: $buildTarget"
    Write-Host "Run logs: $runLogRoot"
}

function Install-ModernShaderCompiler {
    param([Parameter(Mandatory = $true)][string]$BinaryRoot)

    $compilerSource = Join-Path $modernBinaryRoot "shadercRelease.exe"
    $compilerDestination = Join-Path $BinaryRoot "shadercRelease.exe"
    Copy-Item -LiteralPath $compilerSource -Destination $compilerDestination -Force

    $shaderSourceRoot = Join-Path $modernSourceRoot "bgfx\src"
    $shaderDestinationRoot = Join-Path $BinaryRoot "Shaders"
    New-Item -ItemType Directory -Force -Path $shaderDestinationRoot | Out-Null
    foreach ($shaderInclude in Get-ChildItem -LiteralPath $shaderSourceRoot -Filter "*.sh" -File) {
        Copy-Item -LiteralPath $shaderInclude.FullName -Destination $shaderDestinationRoot -Force
    }
}

function Install-PinnedManagedApi {
    param([Parameter(Mandatory = $true)][string]$BinaryRoot)

    if (-not (Test-Path -LiteralPath $depsLockPath)) {
        throw "Dependency lock is missing: $depsLockPath"
    }

    $depsLock = Get-Content -LiteralPath $depsLockPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $managedApiOutputs = @(
        $depsLock.components.'falco-csharp-api'.outputs |
            Where-Object { [string]$_.path -eq "out/deps/w8b/FalcoEngine.dll" }
    )
    if ($managedApiOutputs.Count -ne 1) {
        throw "deps.lock.json must pin exactly one out/deps/w8b/FalcoEngine.dll output."
    }

    $pin = $managedApiOutputs[0]
    $source = Join-Path $PSScriptRoot (([string]$pin.path).Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Pinned managed API is missing: $source"
    }

    $sourceItem = Get-Item -LiteralPath $source
    $expectedBytes = [int64]$pin.bytes
    $expectedHash = ([string]$pin.sha256).ToUpperInvariant()
    $actualHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($sourceItem.Length -ne $expectedBytes -or $actualHash -ne $expectedHash) {
        throw "Pinned managed API identity mismatch: $source"
    }

    $destination = Join-Path $BinaryRoot "FalcoEngine.dll"
    Copy-Item -LiteralPath $source -Destination $destination -Force
    Write-Host "Installed pinned managed API: $destination"
}

function Install-PinnedRuntimeDependencies {
    param([Parameter(Mandatory = $true)][string]$BinaryRoot)

    if (-not (Test-Path -LiteralPath $depsLockPath)) {
        throw "Dependency lock is missing: $depsLockPath"
    }

    $depsLock = Get-Content -LiteralPath $depsLockPath -Raw -Encoding UTF8 | ConvertFrom-Json

    function Get-PinnedRuntimeOutput {
        param(
            [Parameter(Mandatory = $true)][string]$Component,
            [Parameter(Mandatory = $true)][string]$Path
        )

        $componentDefinition = $depsLock.components.$Component
        if ($null -eq $componentDefinition) {
            throw "Runtime dependency component is missing from deps.lock.json: $Component"
        }

        $matches = @(
            $componentDefinition.outputs |
                Where-Object { [string]$_.path -eq $Path }
        )
        if ($matches.Count -ne 1) {
            throw "deps.lock.json must pin exactly one runtime output: $Path"
        }

        return $matches[0]
    }

    function Assert-PinnedRuntimeFile {
        param(
            [Parameter(Mandatory = $true)]$Pin,
            [Parameter(Mandatory = $true)][string]$FilePath
        )

        if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
            throw "Pinned runtime file is missing: $FilePath"
        }

        $file = Get-Item -LiteralPath $FilePath
        $expectedBytes = [int64]$Pin.bytes
        $expectedHash = ([string]$Pin.sha256).ToUpperInvariant()
        $actualHash = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($file.Length -ne $expectedBytes -or $actualHash -ne $expectedHash) {
            throw "Pinned runtime identity mismatch: $FilePath"
        }
    }

    $runtimePins = @(
        @{
            Component = "mono-msi"
            Path = "out/deps/w8/mono-2.0-sgen.dll"
            Destination = "mono-2.0-sgen.dll"
        },
        @{
            Component = "openal-soft"
            Path = "out/deps/w9/bin/OpenAL32.dll"
            Destination = "OpenAL32.dll"
        }
    )

    foreach ($runtimePin in $runtimePins) {
        $pin = Get-PinnedRuntimeOutput -Component $runtimePin.Component -Path $runtimePin.Path
        $source = Join-Path $PSScriptRoot ($runtimePin.Path.Replace('/', '\'))
        Assert-PinnedRuntimeFile -Pin $pin -FilePath $source

        $destination = Join-Path $BinaryRoot $runtimePin.Destination
        Copy-Item -LiteralPath $source -Destination $destination -Force
        Assert-PinnedRuntimeFile -Pin $pin -FilePath $destination
        Write-Host "Installed pinned runtime dependency: $destination"
    }

    $monoRoot = Join-Path $PSScriptRoot "out\deps\w8\Mono"
    $monoDestination = Join-Path $BinaryRoot "Mono"
    foreach ($directory in @("bin", "lib", "etc")) {
        Copy-RuntimeResourceTree `
            -SourceRoot (Join-Path $monoRoot $directory) `
            -DestinationRoot (Join-Path $monoDestination $directory)
    }

    $monoVerificationPins = @(
        @{ Path = "out/deps/w8/Mono/bin/mono.exe"; Destination = "Mono\bin\mono.exe" },
        @{ Path = "out/deps/w8/Mono/lib/mono/4.5/mcs.exe"; Destination = "Mono\lib\mono\4.5\mcs.exe" },
        @{ Path = "out/deps/w8/Mono/etc/mono/config"; Destination = "Mono\etc\mono\config" }
    )
    foreach ($verificationPin in $monoVerificationPins) {
        $pin = Get-PinnedRuntimeOutput -Component "mono-msi" -Path $verificationPin.Path
        Assert-PinnedRuntimeFile -Pin $pin -FilePath (Join-Path $BinaryRoot $verificationPin.Destination)
    }

    & (Join-Path $PSScriptRoot 'InstallRuntimeNotices.ps1') -BinaryRoot $BinaryRoot
    & (Join-Path $PSScriptRoot 'InstallFreeImageNotices.ps1') -BinaryRoot $BinaryRoot
    & (Join-Path $PSScriptRoot 'InstallBgfxNotices.ps1') -BinaryRoot $BinaryRoot
    & (Join-Path $PSScriptRoot 'InstallAssimpBulletNotices.ps1') -BinaryRoot $BinaryRoot
    & (Join-Path $PSScriptRoot 'InstallEngineNotices.ps1') -BinaryRoot $BinaryRoot
    & (Join-Path $PSScriptRoot 'InstallHrtfNotices.ps1') -BinaryRoot $BinaryRoot -DependencyLockPath $depsLockPath
    & (Join-Path $PSScriptRoot 'InstallClipper2Notices.ps1') -BinaryRoot $BinaryRoot
    & (Join-Path $PSScriptRoot 'InstallRuntimePrerequisites.ps1') -BinaryRoot $BinaryRoot -DependencyLockPath $depsLockPath
    Write-Host "Installed pinned Mono runtime tree: $monoDestination"
}

function Copy-RuntimeResourceTree {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$DestinationRoot,
        [string[]]$ExcludedRelativePaths = @()
    )

    if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
        throw "Runtime resource source is missing: $SourceRoot"
    }

    New-Item -ItemType Directory -Force -Path $DestinationRoot | Out-Null
    $resolvedSource = (Get-Item -LiteralPath $SourceRoot).FullName
    foreach ($item in Get-ChildItem -LiteralPath $SourceRoot -Force -Recurse -File) {
        $prefix = $resolvedSource.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        if (!$item.FullName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Resource path escapes source root.' }
        $relative = $item.FullName.Substring($prefix.Length).Replace('\', '/')
        if ($relative -in $ExcludedRelativePaths) { continue }
        $target = Join-Path $DestinationRoot $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath $item.FullName -Destination $target -Force
    }
}

function Install-RuntimeResources {
    param(
        [Parameter(Mandatory = $true)][string]$BinaryRoot,
        [switch]$IncludeEditorResources
    )

    $builtinDestination = Join-Path $BinaryRoot "BuiltinResources"
    Copy-RuntimeResourceTree -SourceRoot (Join-Path $PSScriptRoot "BuiltinResources") -DestinationRoot $builtinDestination
    Copy-RuntimeResourceTree -SourceRoot (Join-Path $PSScriptRoot "Res\BuiltinResources") -DestinationRoot $builtinDestination -ExcludedRelativePaths @('Fonts/arial.ttf')
    & (Join-Path $PSScriptRoot 'InstallDefaultFont.ps1') -BinaryRoot $BinaryRoot

    $requiredResources = @(
        (Join-Path $builtinDestination "Shaders\StandardDeferred.shader"),
        (Join-Path $builtinDestination "Primitives\Cube.fbx"),
        (Join-Path $builtinDestination "Terrain\default_diffuse.jpg")
    )

    if ($IncludeEditorResources) {
        $editorDestination = Join-Path $BinaryRoot "Editor"
        Copy-RuntimeResourceTree -SourceRoot (Join-Path $PSScriptRoot "Res\Editor") -DestinationRoot $editorDestination
        & (Join-Path $PSScriptRoot 'InstallEditorFontNotices.ps1') -BinaryRoot $BinaryRoot

		$starterContentDestination = Join-Path $BinaryRoot "StarterContent"
		Copy-RuntimeResourceTree -SourceRoot (Join-Path $PSScriptRoot "Res\StarterContent") -DestinationRoot $starterContentDestination
        $requiredResources += @(
            (Join-Path $editorDestination "Fonts\Roboto-Regular.ttf"),
            (Join-Path $editorDestination "Icons\Toolbar\new.png"),
            (Join-Path $editorDestination "Icons\Assets\file.png"),
			(Join-Path $starterContentDestination "README.md")
        )
    }

    foreach ($requiredResource in $requiredResources) {
        if (-not (Test-Path -LiteralPath $requiredResource -PathType Leaf)) {
            throw "Installed runtime resource is missing: $requiredResource"
        }
    }

    Write-Host "Installed runtime resources: $BinaryRoot"
}

function Build-ModernEngine {
    $msbuild = Get-Vs2022MsBuild
    $project = Join-Path $PSScriptRoot "Engine\Engine.vcxproj"
    $binaryRoot = Join-Path $OutputRoot "bin\Engine"
    $objectRoot = Join-Path $OutputRoot "obj\Engine"
    New-Item -ItemType Directory -Force -Path $binaryRoot, $objectRoot, $runLogRoot | Out-Null

    $binaryLog = Join-Path $runLogRoot "Engine.binlog"
    $textLog = Join-Path $runLogRoot "Engine.log"
    $arguments = @(
        $project,
        "/t:$buildTarget",
        "/m:1",
        "/nr:false",
        "/p:Configuration=Release",
        "/p:Platform=x64",
        "/p:FalcoModernBgfx=true",
        "/p:BuildProjectReferences=false",
        "/p:OutDir=$binaryRoot\",
        "/p:IntDir=$objectRoot\",
        "/bl:$binaryLog",
        "/fl",
        "/flp:logfile=$textLog;verbosity=normal"
    )

    Write-Host "> $msbuild $($arguments -join ' ')"
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Modern Engine build failed with exit code ${LASTEXITCODE}. See $textLog"
    }
}

function Test-ModernShaderCompiler {
    $msbuild = Get-Vs2022MsBuild
    $project = Join-Path $PSScriptRoot "ModernBgfxCompat\ShadercSmoke.vcxproj"
    $binaryRoot = Join-Path $OutputRoot "bin\ShadercSmoke"
    $objectRoot = Join-Path $OutputRoot "obj\ShadercSmoke"
    New-Item -ItemType Directory -Force -Path $binaryRoot, $objectRoot, $runLogRoot | Out-Null

    $binaryLog = Join-Path $runLogRoot "ShadercSmoke.binlog"
    $textLog = Join-Path $runLogRoot "ShadercSmoke.log"
    $arguments = @(
        $project,
        "/t:$buildTarget",
        "/m:1",
        "/nr:false",
        "/p:Configuration=Release",
        "/p:Platform=x64",
        "/p:OutDir=$binaryRoot\",
        "/p:IntDir=$objectRoot\",
        "/bl:$binaryLog",
        "/fl",
        "/flp:logfile=$textLog;verbosity=normal"
    )

    Write-Host "> $msbuild $($arguments -join ' ')"
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Shaderc smoke build failed with exit code ${LASTEXITCODE}. See $textLog"
    }

    Install-ModernShaderCompiler -BinaryRoot $binaryRoot
    $smokeExecutable = Join-Path $binaryRoot "ShadercSmoke.exe"
    Write-Host "> $smokeExecutable"
    & $smokeExecutable
    if ($LASTEXITCODE -ne 0) {
        throw "Shaderc smoke failed with exit code ${LASTEXITCODE}."
    }
}

function Build-ModernPlayer {
    param([switch]$SkipEngine)

    if (-not $SkipEngine) {
        Build-ModernEngine
    }

    $msbuild = Get-Vs2022MsBuild
    $project = Join-Path $PSScriptRoot "Player\Player.vcxproj"
    $binaryRoot = Join-Path $OutputRoot "bin\Player"
    $objectRoot = Join-Path $OutputRoot "obj\Player"
    New-Item -ItemType Directory -Force -Path $binaryRoot, $objectRoot, $runLogRoot | Out-Null

    $binaryLog = Join-Path $runLogRoot "Player.binlog"
    $textLog = Join-Path $runLogRoot "Player.log"
    $arguments = @(
        $project,
        "/t:$buildTarget",
        "/m:1",
        "/nr:false",
        "/p:Configuration=Release",
        "/p:Platform=x64",
        "/p:FalcoModernBgfx=true",
        "/p:BuildProjectReferences=false",
        "/p:OutDir=$binaryRoot\",
        "/p:IntDir=$objectRoot\",
        "/bl:$binaryLog",
        "/fl",
        "/flp:logfile=$textLog;verbosity=normal"
    )

    Write-Host "> $msbuild $($arguments -join ' ')"
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Modern Player build failed with exit code ${LASTEXITCODE}. See $textLog"
    }

    Install-ModernShaderCompiler -BinaryRoot $binaryRoot
    Install-PinnedManagedApi -BinaryRoot $binaryRoot
    Install-PinnedRuntimeDependencies -BinaryRoot $binaryRoot
    Install-RuntimeResources -BinaryRoot $binaryRoot
}

function Build-ModernEditor {
    param([switch]$SkipEngine)

    if (-not $SkipEngine) {
        Build-ModernEngine
    }

    $msbuild = Get-Vs2022MsBuild
    $project = Join-Path $PSScriptRoot "Editor\Editor.vcxproj"
    $binaryRoot = Join-Path $OutputRoot "bin\Editor"
    $objectRoot = Join-Path $OutputRoot "obj\Editor"
    New-Item -ItemType Directory -Force -Path $binaryRoot, $objectRoot, $runLogRoot | Out-Null

    $binaryLog = Join-Path $runLogRoot "Editor.binlog"
    $textLog = Join-Path $runLogRoot "Editor.log"
    $arguments = @(
        $project,
        "/t:$buildTarget",
        "/m:1",
        "/nr:false",
        "/p:Configuration=Release",
        "/p:Platform=x64",
        "/p:FalcoModernBgfx=true",
        "/p:BuildProjectReferences=false",
        "/p:OutDir=$binaryRoot\",
        "/p:IntDir=$objectRoot\",
        "/bl:$binaryLog",
        "/fl",
        "/flp:logfile=$textLog;verbosity=normal"
    )

    Write-Host "> $msbuild $($arguments -join ' ')"
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Modern Editor build failed with exit code ${LASTEXITCODE}. See $textLog"
    }

    Install-ModernShaderCompiler -BinaryRoot $binaryRoot
    Install-PinnedManagedApi -BinaryRoot $binaryRoot
    Install-PinnedRuntimeDependencies -BinaryRoot $binaryRoot
    Install-RuntimeResources -BinaryRoot $binaryRoot -IncludeEditorResources
}

function Test-WarningBaseline {
    if (-not (Test-Path -LiteralPath $warningBaselineScript -PathType Leaf)) {
        throw "Warning baseline gate is missing: $warningBaselineScript"
    }

    & $warningBaselineScript -Mode Check -LogRoot $runLogRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Warning baseline gate failed with exit code ${LASTEXITCODE}. See $runLogRoot\warning-report.json"
    }
}

Test-Prerequisites

switch ($Mode) {
    "Check" { }
    "Engine" { Build-ModernEngine }
    "ShaderSmoke" { Test-ModernShaderCompiler }
    "CSGSmoke" { & (Join-Path $PSScriptRoot "TestCSGManifold.ps1") -MSBuild (Get-Vs2022MsBuild) -DependencyLockPath $depsLockPath }
    "Player" { Build-ModernPlayer }
    "Editor" { Build-ModernEditor }
    "All" {
        Build-ModernEngine
        Build-ModernPlayer -SkipEngine
        Build-ModernEditor -SkipEngine
        Test-ModernShaderCompiler
        if ($Rebuild) {
            Test-WarningBaseline
        }
        else {
            Write-Host "Warning baseline gate skipped: only a full All -Rebuild is accepted."
        }
    }
}
