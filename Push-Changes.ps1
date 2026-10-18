[CmdletBinding()]
param([Parameter(Mandatory)][string]$Message, [switch]$AllowPublic)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Workflow.Common.ps1"
Push-Location $PSScriptRoot
try {
    $lock = Get-FalcoWorkflowLock
    $headers = Get-FalcoGitHubHeaders
    $repo = Invoke-RestMethod -Uri "https://api.github.com/repos/$($lock.repository)" -Headers $headers
    if (!$repo.private -and !$AllowPublic) { throw 'Public source upload requires -AllowPublic.' }
    $origin = & git -c "safe.directory=$PSScriptRoot" remote get-url origin
    if ($LASTEXITCODE -ne 0 -or $origin -notin @("https://github.com/$($lock.repository).git", "https://github.com/$($lock.repository)", "git@github.com:$($lock.repository).git")) { throw 'Unexpected Git origin.' }
    & git -c "safe.directory=$PSScriptRoot" add --all
    if ($LASTEXITCODE -ne 0) { throw 'git add failed.' }
    & git -c "safe.directory=$PSScriptRoot" diff --cached --quiet
    $diffExit = $LASTEXITCODE
    if ($diffExit -eq 1) {
        & git -c "safe.directory=$PSScriptRoot" commit -m $Message
        if ($LASTEXITCODE -ne 0) { throw 'git commit failed.' }
    } elseif ($diffExit -ne 0) { throw 'git diff failed.' }
    & git -c "safe.directory=$PSScriptRoot" push origin HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Push failed; local commit retained. Resolve divergence without force-pushing.' }
    Write-Host "PASS: changes sent to $($lock.repository)."
} finally { Pop-Location }
