[CmdletBinding()]
param(
    [ValidateSet("Check", "Fetch", "Configure", "Build", "All")]
    [string]$Mode = "All",

    [string]$OutputRoot = "",

    [switch]$AllowNetwork,

    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $PSScriptRoot "out\modern-bgfx"
}

$lockPath = Join-Path $PSScriptRoot "modern-bgfx.lock.json"
$lock = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8 | ConvertFrom-Json
$dependencyLockPath = Join-Path $PSScriptRoot "deps.lock.json"
$dependencyLock = Get-Content -LiteralPath $dependencyLockPath -Raw -Encoding UTF8 | ConvertFrom-Json
$sourceRoot = Join-Path $OutputRoot "src"
$logRoot = Join-Path $OutputRoot "logs"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$WorkingDirectory = $PSScriptRoot
    )

    Write-Host "> $FilePath $($Arguments -join ' ')"
    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
        }
    }
    finally {
        Pop-Location
    }
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

function Test-Prerequisites {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw "git is required."
    }

    $msbuild = Get-Vs2022MsBuild
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include"
    $sdkVersions = @(Get-ChildItem -LiteralPath $sdkRoot -Directory -ErrorAction SilentlyContinue)
    if ($sdkVersions.Count -eq 0) {
        throw "Windows 10/11 SDK headers were not found."
    }

    Write-Host "Lock: $lockPath"
    Write-Host "Output: $OutputRoot"
    Write-Host "MSBuild: $msbuild"
    Write-Host "Windows SDK: $($sdkVersions[-1].Name)"
}

function Get-PinnedRepository {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)]$Spec
    )

    if (-not $AllowNetwork) {
        throw "Network access is disabled. Re-run with -AllowNetwork after reviewing modern-bgfx.lock.json."
    }

    $target = Join-Path $sourceRoot $Name
    if (-not (Test-Path -LiteralPath (Join-Path $target ".git"))) {
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Invoke-Checked git @("-c", "safe.directory=$target", "init", "--quiet") $target
        Invoke-Checked git @("-c", "safe.directory=$target", "remote", "add", "origin", [string]$Spec.url) $target
    }

    $originOutput = & git -c "safe.directory=$target" -C $target remote get-url origin
    $origin = if ($null -eq $originOutput) { "" } else { ([string]$originOutput).Trim() }
    if ($LASTEXITCODE -ne 0 -or $origin -ne [string]$Spec.url) {
        throw "$Name origin mismatch. Expected '$($Spec.url)', got '$origin'."
    }

    $existingHead = & git -c "safe.directory=$target" -C $target rev-parse --verify --quiet HEAD
    if ($LASTEXITCODE -eq 0 -and [string]$existingHead -eq [string]$Spec.commit) {
        Write-Host "Using cached $Name at $existingHead"
        return
    }

    if ($Name -eq 'bgfx') {
        # Demo textures/models and documentation are not inputs to the engine/tool build.
        Invoke-Checked git @('-c', "safe.directory=$target", 'config', 'remote.origin.promisor', 'true') $target
        Invoke-Checked git @('-c', "safe.directory=$target", 'config', 'remote.origin.partialclonefilter', 'blob:none') $target
        Invoke-Checked git @('-c', "safe.directory=$target", 'sparse-checkout', 'set', '--no-cone', '/*', '!/examples/runtime/', '!/docs/') $target
        Invoke-Checked git @('-c', "safe.directory=$target", 'fetch', '--progress', '--filter=blob:none', '--depth', '1', 'origin', [string]$Spec.commit) $target
    } else {
        Invoke-Checked git @('-c', "safe.directory=$target", 'fetch', '--progress', '--depth', '1', 'origin', [string]$Spec.commit) $target
    }
    Invoke-Checked git @("-c", "safe.directory=$target", "checkout", "--detach", "--force", [string]$Spec.commit) $target

    $actualOutput = & git -c "safe.directory=$target" -C $target rev-parse HEAD
    $actual = if ($null -eq $actualOutput) { "" } else { ([string]$actualOutput).Trim() }
    if ($LASTEXITCODE -ne 0 -or $actual -ne [string]$Spec.commit) {
        throw "$Name commit mismatch. Expected '$($Spec.commit)', got '$actual'."
    }
}

function Fetch-Sources {
    New-Item -ItemType Directory -Force -Path $sourceRoot | Out-Null
    Get-PinnedRepository "bx" $lock.repositories.bx
    Get-PinnedRepository "bimg" $lock.repositories.bimg
    Get-PinnedRepository "bgfx" $lock.repositories.bgfx
}

function Assert-PinnedSources {
    foreach ($name in @("bx", "bimg", "bgfx")) {
        $target = Join-Path $sourceRoot $name
        $expected = [string]$lock.repositories.$name.commit
        if (-not (Test-Path -LiteralPath (Join-Path $target ".git"))) {
            throw "$name is missing. Run Fetch or All with -AllowNetwork first."
        }
        $actualOutput = & git -c "safe.directory=$target" -C $target rev-parse HEAD
        $actual = if ($null -eq $actualOutput) { "" } else { ([string]$actualOutput).Trim() }
        if ($LASTEXITCODE -ne 0 -or $actual -ne $expected) {
            throw "$name is not pinned to $expected (actual: $actual)."
        }
    }
}

function Set-FalcoBgfxResourceLimits {
    $configPath = Join-Path $sourceRoot "bgfx\src\config.h"
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "bgfx configuration header is missing: $configPath"
    }

    $text = [IO.File]::ReadAllText($configPath)
    $text = $text.Replace(
        ('#' + "`t" + 'define BGFX_CONFIG_MAX_TEXTURES (4<<10)'),
        ('#' + "`t" + 'define BGFX_CONFIG_MAX_TEXTURES (32<<10)'))
    $text = $text.Replace(
        ('#' + "`t" + 'define BGFX_CONFIG_MAX_FRAME_BUFFERS 128'),
        ('#' + "`t" + 'define BGFX_CONFIG_MAX_FRAME_BUFFERS 512'))

    $text = $text.Replace(
        ('#' + "`t" + 'define BGFX_CONFIG_MAX_VIEWS 256'),
        ('#' + "`t" + 'define BGFX_CONFIG_MAX_VIEWS 2048'))

    # Upgrade an existing checkout configured with the previous view budget.
    $text = $text.Replace(
        ('#' + "`t" + 'define BGFX_CONFIG_MAX_VIEWS 1024'),
        ('#' + "`t" + 'define BGFX_CONFIG_MAX_VIEWS 2048'))

    if (-not $text.Contains('#' + "`t" + 'define BGFX_CONFIG_MAX_TEXTURES (32<<10)') -or
        -not $text.Contains('#' + "`t" + 'define BGFX_CONFIG_MAX_FRAME_BUFFERS 512') -or
        -not $text.Contains('#' + "`t" + 'define BGFX_CONFIG_MAX_VIEWS 2048')) {
        throw "Unable to apply Falco bgfx resource limits to $configPath"
    }

    if ([IO.File]::ReadAllText($configPath) -cne $text) {
        [IO.File]::WriteAllText($configPath, $text, [Text.UTF8Encoding]::new($false))
    }
    Write-Host "Falco bgfx resource limits: 32768 textures, 512 framebuffers, 2048 views"
    & (Join-Path $PSScriptRoot 'patches/ApplyModernBgfxSwapChain.ps1') -SourceRoot $sourceRoot
}

function Assert-PinnedGenie {
    $genie = Join-Path $sourceRoot "bx\tools\bin\windows\genie.exe"
    if (-not (Test-Path -LiteralPath $genie -PathType Leaf)) {
        throw "Pinned bx does not contain the expected GENie binary: $genie"
    }
    $component = $dependencyLock.components.genie
    $outputs = @($component.outputs)
    if ($component.wave -ne "W0" -or $outputs.Count -ne 1 -or
        $outputs[0].sha256 -notmatch "^[A-Fa-f0-9]{64}$" -or [long]$outputs[0].bytes -le 0) {
        throw "deps.lock.json does not contain a production identity for W0 GENie."
    }
    $item = Get-Item -LiteralPath $genie
    if ([long]$item.Length -ne [long]$outputs[0].bytes) {
        throw "GENie size mismatch. Expected $($outputs[0].bytes), got $($item.Length). Refusing to execute."
    }
    $actualSha256 = (Get-FileHash -LiteralPath $genie -Algorithm SHA256).Hash
    if ($actualSha256 -ne [string]$outputs[0].sha256) {
        throw "GENie SHA-256 mismatch. Expected $($outputs[0].sha256), got $actualSha256. Refusing to execute."
    }
    Write-Host "GENie verified: $actualSha256 ($($item.Length) bytes)"
    return $genie
}

function Assert-GeneratedProjectBaseline {
    $projectRoot = Join-Path $sourceRoot "bgfx\.build\projects\vs2022"
    $solution = Join-Path $projectRoot "bgfx.sln"
    if (-not (Test-Path -LiteralPath $solution -PathType Leaf)) {
        throw "GENie did not create the expected solution: $solution"
    }
    $vcxprojCount = @(Get-ChildItem -LiteralPath $projectRoot -File -Filter "*.vcxproj").Count
    $solutionProjectEntries = @(Select-String -LiteralPath $solution -Pattern "^Project\(").Count
    $generatedFileCount = @(Get-ChildItem -LiteralPath $projectRoot -File).Count
    if ($vcxprojCount -ne [int]$lock.configuration.expectedVcxprojCount -or
        $solutionProjectEntries -ne [int]$lock.configuration.expectedSolutionProjectEntries -or
        $generatedFileCount -ne [int]$lock.configuration.expectedGeneratedFiles) {
        throw "Generated project baseline mismatch. Expected vcxproj/solution entries/files " +
            "$($lock.configuration.expectedVcxprojCount)/$($lock.configuration.expectedSolutionProjectEntries)/$($lock.configuration.expectedGeneratedFiles), " +
            "got $vcxprojCount/$solutionProjectEntries/$generatedFileCount."
    }
    Write-Host "Generated project baseline verified: $vcxprojCount vcxproj, $solutionProjectEntries solution entries, $generatedFileCount files."
}

function Configure-Bgfx {
    Assert-PinnedSources
    Set-FalcoBgfxResourceLimits
    $bgfxRoot = Join-Path $sourceRoot "bgfx"
    $genie = Assert-PinnedGenie

    $arguments = @()
    foreach ($option in $lock.configuration.genieOptions) {
        $arguments += [string]$option
    }
    $arguments += [string]$lock.configuration.generator
    Invoke-Checked $genie $arguments $bgfxRoot
    Assert-GeneratedProjectBaseline
}

function Write-DeterministicBuildTargets {
    # Imported after generated item definitions; do not edit upstream projects.
    # Z7 retains debug data in objects without per-run compiler PDB identities.
    $escapedRoot = [Security.SecurityElement]::Escape([IO.Path]::GetFullPath($sourceRoot))
    $content = @'
<Project>
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
    <ClCompile>
      <DebugInformationFormat>OldStyle</DebugInformationFormat>
      <AdditionalOptions>%(AdditionalOptions) /Brepro /experimental:deterministic /pathmap:&quot;__ROOT__=GFXSRC&quot;</AdditionalOptions>
    </ClCompile>
    <Lib><AdditionalOptions>%(AdditionalOptions) /Brepro</AdditionalOptions></Lib>
    <Link><AdditionalOptions>%(AdditionalOptions) /Brepro /PDBALTPATH:%_PDB%</AdditionalOptions></Link>
  </ItemDefinitionGroup>
</Project>
'@
    $path = Join-Path $logRoot 'deterministic-build.targets'
    $content = $content.Replace('__ROOT__', $escapedRoot)
    if (!(Test-Path -LiteralPath $path) -or [IO.File]::ReadAllText($path) -cne $content) {
        [IO.File]::WriteAllText($path, $content, [Text.UTF8Encoding]::new($false))
    }
    return $path
}

function Build-Bgfx {
    Assert-PinnedSources
    Set-FalcoBgfxResourceLimits
    Normalize-ProcessPathVariable
    $msbuild = Get-Vs2022MsBuild
    $bgfxRoot = Join-Path $sourceRoot "bgfx"
    $solution = Join-Path $bgfxRoot ".build\projects\vs2022\bgfx.sln"
    if (-not (Test-Path -LiteralPath $solution)) {
        throw "The bgfx solution is missing. Run Configure first."
    }

    New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
    $deterministicTargets = Write-DeterministicBuildTargets
    $buildTarget = if ($Rebuild) { "Rebuild" } else { "Build" }
    $binaryLog = Join-Path $logRoot "bgfx.binlog"
    $textLog = Join-Path $logRoot "bgfx.log"
    Invoke-Checked $msbuild @(
        $solution,
        "/t:$buildTarget",
        "/p:ForceImportAfterCppTargets=$deterministicTargets",
        "/m:1",
        "/nr:false",
        "/p:Configuration=$($lock.configuration.buildConfiguration)",
        "/p:Platform=$($lock.configuration.platform)",
        "/bl:$binaryLog",
        "/fl",
        "/flp:logfile=$textLog;verbosity=normal"
    ) $bgfxRoot
    if (Select-String -LiteralPath $textLog -Pattern '\bD900[27]\b|\bLNK4044\b' -Quiet) {
        throw "Compiler or linker ignored an option; see $textLog"
    }
}

Test-Prerequisites

switch ($Mode) {
    "Check" {
        Assert-PinnedSources
        Assert-PinnedGenie | Out-Null
        Assert-GeneratedProjectBaseline
    }
    "Fetch" { Fetch-Sources }
    "Configure" { Configure-Bgfx }
    "Build" { Build-Bgfx }
    "All" {
        Fetch-Sources
        Configure-Bgfx
        Build-Bgfx
    }
}
