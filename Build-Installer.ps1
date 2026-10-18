[CmdletBinding()]
param([switch]$SkipBuild, [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$')][string]$Version = '4.0.0.0')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Workflow.Common.ps1"
try {
    if (!$SkipBuild) { Invoke-FalcoScript 'Build.ps1' @() }
    $tools = Get-Content -LiteralPath "$PSScriptRoot/InstallerTools.lock.json" -Raw | ConvertFrom-Json
    $toolRoot = Join-Path $PSScriptRoot 'out/installer-tools'
    $setup = Get-FalcoDownload $tools.inno.url (Join-Path $toolRoot 'inno-setup.exe') $tools.inno.sha256 @{} $tools.inno.bytes
    $inno = Join-Path $toolRoot 'inno'
    $iscc = Join-Path $inno 'ISCC.exe'
    if (!(Test-Path -LiteralPath $iscc)) {
        $process = Start-Process -FilePath $setup -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER','/PORTABLE=1',('/DIR="'+$inno+'"')) -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $iscc)) { throw 'Portable Inno Setup extraction failed.' }
    }
    $redist = Get-FalcoDownload $tools.vcRuntime.url (Join-Path $toolRoot 'vc_redist.x64.exe') $tools.vcRuntime.sha256 @{} $tools.vcRuntime.bytes
    $bin = Join-Path $PSScriptRoot 'out/falco/bin'
    foreach ($name in @('Editor/Editor.exe','Player/Player.exe','Editor/FalcoEngine.dll','Editor/shadercRelease.exe')) {
        if (!(Test-Path -LiteralPath (Join-Path $bin $name))) { throw "Build output is missing: $name" }
    }
    $out = Join-Path $PSScriptRoot 'out/installer'
    $stage = Join-Path $out ('stage-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    $editor = Join-Path $bin 'Editor'
    foreach ($file in Get-ChildItem -LiteralPath $editor -File -Recurse) {
        if ($file.Extension -in @('.pdb','.ilk','.log','.exp') -or $file.Name -eq 'imgui.ini' -or $file.FullName -match '[\\/]ShaderCache[\\/]') { continue }
        if ($file.DirectoryName -eq $editor -and $file.Extension -eq '.lib') { continue }
        $relative = $file.FullName.Substring($editor.Length).TrimStart('\')
        $dest = Join-Path $stage $relative
        [IO.Directory]::CreateDirectory((Split-Path $dest)) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $dest
    }
    Copy-Item -LiteralPath (Join-Path $bin 'Player/Player.exe') -Destination (Join-Path $stage 'Player.exe')
    # One shared Mono/resource tree, with Editor.exe and Player.exe side by side.
    & $iscc "/DInputDir=$stage" "/DProjectRoot=$PSScriptRoot" "/DRedistFile=$redist" "/DAppVersion=$Version" "/O$out" "$PSScriptRoot/InstallerModern.iss"
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
    $installer = Join-Path $out 'FalcoEngineSetup.exe'
    Get-FileHash -LiteralPath $installer -Algorithm SHA256 | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'installer-sha256.json')
    Write-Host "PASS: $installer"
} catch { Write-Error $_; exit 1 }
