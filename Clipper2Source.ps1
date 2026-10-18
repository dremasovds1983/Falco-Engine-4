# Shared verification for the offline Manifold dependency and its shipped notices.
function Get-FalcoClipper2Bundle {
    $ErrorActionPreference = 'Stop'
    $folder = Join-Path $PSScriptRoot 'ThirdParty/clipper2-46f639177fe4'
    $provenance = Join-Path $folder 'PROVENANCE.json'
    $expected = 'af0e2158125c6388a3d70aa2a1174584e24abe6735d7cb6166aba65c35d2ed07'
    $cursor = $folder
    while ($cursor) {
        if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked Clipper2 input' }
        $cursor = Split-Path $cursor -Parent
    }
    # Inspect directories before descending, so no linked subtree is followed.
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($folder)
    $actual = @()
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked Clipper2 input' }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            else { $actual += $item.FullName.Substring($folder.Length + 1).Replace('\','/') }
        }
    }
    if ((Get-FileHash -LiteralPath $provenance).Hash -ne $expected) { throw 'Clipper2 provenance identity mismatch' }
    $manifest = Get-Content -LiteralPath $provenance -Raw | ConvertFrom-Json
    $names = @($manifest.files.PSObject.Properties.Name)
    if ($manifest.schemaVersion -ne 1 -or $manifest.kind -ne 'pinned-clipper2-source' -or
        $manifest.commit -ne '46f639177fe418f9689e8ddb74f08a870c71f5b4' -or $names.Count -ne 19 -or
        @(Compare-Object @($names + 'PROVENANCE.json' | Sort-Object) @($actual | Sort-Object)).Count) { throw 'Clipper2 inventory mismatch' }
    foreach ($name in $names) {
        if ([IO.Path]::IsPathRooted($name) -or $name -match '(^|/)\.\.(/|$)|[:\\]') { throw 'Unsafe Clipper2 input path' }
        $file = Get-Item -LiteralPath (Join-Path $folder $name)
        if ($file.Length -ne $manifest.files.$name.bytes -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne $manifest.files.$name.sha256) { throw "Clipper2 source identity mismatch: $name" }
    }
    return [pscustomobject]@{ root=$folder; manifest=$manifest; sha256=$expected }
}
