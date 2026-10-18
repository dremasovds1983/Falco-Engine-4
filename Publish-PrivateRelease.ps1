[CmdletBinding()]
param([Parameter(Mandatory)][string]$Tag, [Parameter(Mandatory)][string]$Title, [Parameter(Mandatory)][string[]]$Files)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/Workflow.Common.ps1"
$headers = Get-FalcoGitHubHeaders
$lock = Get-FalcoWorkflowLock
$api = "https://api.github.com/repos/$($lock.repository)"
$repo = Invoke-RestMethod -Uri $api -Headers $headers
if (!$repo.private) { throw 'Release upload requires a private repository.' }
$head = & git -c "safe.directory=$PSScriptRoot" -C $PSScriptRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Commit the source before creating a release.' }
$releases = @(Invoke-RestMethod -Uri "$api/releases?per_page=100" -Headers $headers)
$release = @($releases | Where-Object tag_name -eq $Tag)
if ($release.Count -eq 0) {
    $body = @{tag_name=$Tag;target_commitish=[string]$head;name=$Title;body='Private modern development build. See README.md for one-command build and installer generation.';draft=$false;prerelease=$true} | ConvertTo-Json
    $release = @(Invoke-RestMethod -Method Post -Uri "$api/releases" -Headers $headers -ContentType 'application/json' -Body $body)
}
if ($release.Count -ne 1) { throw 'Ambiguous release.' }
foreach ($file in $Files) {
    $path = [IO.Path]::GetFullPath($file)
    $name = [IO.Path]::GetFileName($path)
    if (@($release[0].assets | Where-Object name -eq $name).Count) { throw "Asset already exists; choose a new release tag: $name" }
    $upload = $release[0].upload_url.Split('{')[0] + '?name=' + [Uri]::EscapeDataString($name)
    Write-Host "Uploading $name..."
    $asset = Invoke-RestMethod -Method Post -Uri $upload -Headers $headers -ContentType 'application/octet-stream' -InFile $path
    $hash = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
    if ($asset.size -ne (Get-Item -LiteralPath $path).Length) { throw 'Uploaded size mismatch.' }
    if ($asset.PSObject.Properties.Name -contains 'digest' -and $asset.digest -and $asset.digest -ne "sha256:$hash") { throw 'Uploaded digest mismatch.' }
    Write-Host "PASS: $($asset.browser_download_url) SHA256=$hash"
}
