[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Workflow.Common.ps1"
try {
    if (!(Get-Command git.exe -ErrorAction SilentlyContinue)) { throw 'Install Git for Windows first.' }
    $cmakeRoot = Join-Path $PSScriptRoot 'out/build-tools/cmake-3.31.6-windows-x86_64'
    if (!(Test-Path -LiteralPath "$cmakeRoot/bin/cmake.exe")) {
        $archive = Get-FalcoDownload 'https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip' (Join-Path $PSScriptRoot 'out/downloads/cmake-3.31.6.zip') 'd163cd3ab4959b0a53fa8988f2ddbd2e6c501658201e6a154386bad9dbe4f836'
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, (Join-Path $PSScriptRoot 'out/build-tools'))
    }
    $env:Path = "$cmakeRoot/bin;" + $env:Path
    Write-Host 'Building graphics dependencies from pinned upstream commits...'
    Invoke-FalcoScript 'BuildModernBgfx.ps1' @('-Mode','All','-AllowNetwork')
    foreach ($wave in @('W0','W1','W2','W3','W4','W5','W6','W7','W8','W8b','W9','W10')) {
        if ($wave -eq 'W8') {
            $mono = (Get-Content -LiteralPath "$PSScriptRoot/deps.lock.json" -Raw | ConvertFrom-Json).components.'mono-msi'
            $inbox = Join-Path $PSScriptRoot 'deps-inbox'
            $offerName = [IO.Path]::GetFileName(([Uri]$mono.sourceOffer.url).AbsolutePath)
            $null = Get-FalcoDownload $mono.sourceOffer.url (Join-Path $inbox $offerName) $mono.sourceOffer.sha256
            $license = Join-Path $PSScriptRoot "ThirdParty/mono-$($mono.version)/LICENSE"
            if (!(Test-FalcoFile $license $mono.licenseFiles[0].sha256)) { throw 'Bundled Mono notice hash mismatch.' }
            Copy-Item -LiteralPath $license -Destination (Join-Path $inbox "mono-$($mono.version)-LICENSE") -Force
        }
        Write-Host "Dependencies: $wave (download, verify, build)..."
        Invoke-FalcoScript 'BuildDeps.ps1' @('-Mode','All','-Wave',$wave,'-AllowNetwork','-AllowToolchainRebuild')
    }
    # Record outputs only after all source builds succeed. Download/source pins stay unchanged.
    $localRoot = Join-Path $PSScriptRoot 'out/local-locks'
    New-Item -ItemType Directory -Force -Path $localRoot | Out-Null
    $deps = Get-Content -LiteralPath "$PSScriptRoot/deps.lock.json" -Raw | ConvertFrom-Json
    foreach ($property in $deps.components.PSObject.Properties) {
        foreach ($output in $property.Value.outputs) {
            $file = Get-Item -LiteralPath (Join-Path $PSScriptRoot $output.path)
            $output.bytes = $file.Length
            $output.sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        }
    }
    $modern = Get-Content -LiteralPath "$PSScriptRoot/modern-bgfx.lock.json" -Raw | ConvertFrom-Json
    foreach ($property in $modern.artifacts.PSObject.Properties) {
        $file = Get-Item -LiteralPath (Join-Path $PSScriptRoot "out/modern-bgfx/src/bgfx/.build/win64_vs2022/bin/$($property.Name)")
        $property.Value.bytes = $file.Length
        $property.Value.sha256 = (Get-FileHash -LiteralPath $file.FullName).Hash
    }
    $deps | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath "$localRoot/deps.lock.json" -Encoding UTF8
    $modern | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath "$localRoot/modern-bgfx.lock.json" -Encoding UTF8
    Write-Host 'PASS: upstream dependencies built; local output identities recorded.'
} catch { Write-Error $_; exit 1 }
