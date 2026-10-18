[CmdletBinding()]
param(
    [ValidateSet('Build','Restore','Installer')][string]$Mode = 'Build',
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Workflow.Common.ps1"
try {
    Invoke-FalcoScript 'Restore-Dependencies.ps1' @()
    if ($Mode -ne 'Restore') {
        $arguments = @('-Mode','All','-DependencyLockPath', (Join-Path $PSScriptRoot 'out/local-locks/deps.lock.json'), '-ModernLockPath', (Join-Path $PSScriptRoot 'out/local-locks/modern-bgfx.lock.json'))
        if ($Rebuild) { $arguments += '-Rebuild' }
        Invoke-FalcoScript 'BuildFalcoModernBgfx.ps1' $arguments
        if ($Mode -eq 'Installer') { Invoke-FalcoScript 'Build-Installer.ps1' @('-SkipBuild') }
    }
    if ($Mode -eq 'Restore') { Write-Host 'PASS: dependencies ready.' }
    else { Write-Host 'PASS. Editor: out/falco/bin/Editor/Editor.exe' }
} catch { Write-Error $_; exit 1 }
