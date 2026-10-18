$ErrorActionPreference = 'Stop'
$script:WorkflowRoot = $PSScriptRoot

function Invoke-FalcoScript([string]$Name, [string[]]$Arguments) {
    $hostExe = Join-Path $PSHOME 'powershell.exe'
    if (!(Test-Path -LiteralPath $hostExe)) { $hostExe = Join-Path $PSHOME 'pwsh.exe' }
    & $hostExe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $script:WorkflowRoot $Name) @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed (exit $LASTEXITCODE)." }
}

function Get-FalcoGitHubHeaders {
    $token = $env:GH_TOKEN
    if (!$token) { $token = $env:GITHUB_TOKEN }
    if (!$token -and (Get-Command gh -ErrorAction SilentlyContinue)) {
        $value = & gh auth token 2>$null
        if ($LASTEXITCODE -eq 0) { $token = [string]$value }
    }
    if (!$token) {
        # Git Credential Manager may ask the user to sign in on the first PC use.
        $values = "protocol=https`nhost=github.com`n`n" | & git credential fill 2>$null
        if ($LASTEXITCODE -eq 0) {
            foreach ($line in $values) { if ($line.StartsWith('password=')) { $token = $line.Substring(9) } }
        }
    }
    if (!$token) { throw 'Private GitHub authentication required. Sign in with Git Credential Manager or gh auth login, then rerun this command.' }
    return @{ Authorization = "Bearer $($token.Trim())"; 'User-Agent' = 'FalcoEngine-PrivateBuild'; Accept = 'application/vnd.github+json' }
}

function Get-FalcoWorkflowLock {
    return Get-Content -LiteralPath (Join-Path $script:WorkflowRoot 'BuildSupport.lock.json') -Raw | ConvertFrom-Json
}

function Test-FalcoFile([string]$Path, [string]$Hash, [long]$Bytes = 0) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    if ($Bytes -gt 0 -and (Get-Item -LiteralPath $Path).Length -ne $Bytes) { return $false }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Hash
}

function Get-FalcoDownload([string]$Url, [string]$Path, [string]$Hash, [hashtable]$Headers = @{}, [long]$Bytes = 0) {
    if (Test-FalcoFile $Path $Hash $Bytes) { return $Path }
    if (!(Test-Path -LiteralPath (Split-Path $Path))) { New-Item -ItemType Directory -Force -Path (Split-Path $Path) | Out-Null }
    $partial = "$Path.partial-$([guid]::NewGuid().ToString('N'))"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Write-Host "Downloading $([IO.Path]::GetFileName($Path))..."
    $previousProgress = $ProgressPreference
    try {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Url -Headers $Headers -OutFile $partial -UseBasicParsing
    } finally { $ProgressPreference = $previousProgress }
    if (!(Test-FalcoFile $partial $Hash $Bytes)) { throw "Downloaded file failed SHA-256 verification: $partial" }
    Move-Item -LiteralPath $partial -Destination $Path -Force
    return $Path
}
