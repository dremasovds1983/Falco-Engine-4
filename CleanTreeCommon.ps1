Set-StrictMode -Version Latest
function Assert-CleanPhysicalPath([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point in root/output path' }
        }
        $cursor = Split-Path $cursor -Parent
    }
}
function Get-CleanRelativePath([string]$Root, [string]$Path) {
    return [IO.Path]::GetRelativePath($Root, $Path).Replace('\','/')
}
function Assert-CleanRelativePath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path) -or $Path.Contains('\') -or $Path.Contains(':') -or
        $Path -match '(^|/)\.\.?(/|$)|//|/$|[. ](/|$)|[<>"|?*\x00-\x1f]|(^|/)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|/|$)' -or [string]::IsNullOrWhiteSpace($Path)) { throw "Unsafe manifest path: $Path" }
}
function Assert-CleanManifest($Manifest) {
    if ($Manifest.schemaVersion -ne 1 -or @($Manifest.files).Count -eq 0) { throw 'Unsupported or empty manifest' }
    $null = $Manifest.publicationBlockers
    foreach ($file in $Manifest.files) {
        Assert-CleanRelativePath $file.path
        if ($file.disposition -notin @('OWN','THIRD_PARTY','REVIEW') -or $file.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or
            $file.bytes -isnot [ValueType] -or $file.bytes -lt 0 -or $file.bytes -ne [Math]::Truncate($file.bytes) -or
            [string]::IsNullOrWhiteSpace($file.reason)) { throw "Invalid inventory entry: $($file.path)" }
    }
}
function Get-CleanDeniedReason([string]$Path) {
    if ($Path -ieq 'Res/BuiltinResources/Fonts/arial.ttf') { return 'Replaced by Liberation Sans default.ttf' }
    if ($Path -match '^Engine/Classes/(brtshaderc\.cpp|md5\.cpp|md5_legacy\.h)$') { return 'PUB-230 legacy implementation excluded' }
    if ($Path -match '^Engine/UI/(imgui_impl_bgfx\.cpp|[fv]s_ocornut_imgui\.bin\.h)$') { return 'PUB-300 legacy ImGui renderer excluded' }
    if ($Path -match '^ThirdParty/dear-imgui-1\.85-bgfx/(widgets/|imgui_user\.(h|inl)$|stb_(rect_pack|textedit|truetype)\.h$)') { return 'PUB-300 unused optional ImGui additions excluded' }
    if ($Path -match '(^|/)(\.git|\.svn|\.hg)(/|$)') { return 'repository metadata' }
    if ($Path -match '^(Bgfx|Carve|steam|OpenAL|Mono|Linux|d3d|out|build|x64|deps-cache|_ai|Milestones)(/|$)') { return 'excluded root' }
    if ($Path -match '^codecs/video(/|$)|^API/(bin|obj)(/|$)|(^|/)(\.vs|\.idea)(/|$)') { return 'SDK or build metadata' }
    if ($Path -match '\.(vcxproj|csproj)\.user$|\.(pdb|ilk|obj|tlog|aps|dmp|exe|dll|lib|so|a)$') { return 'binary or IDE artifact' }
    if ($Path -match '(^|/)(id_rsa[^/]*|id_ed25519[^/]*|\.env[^/]*|[^/]+\.(pfx|p12|pem|key|keystore))$') { return 'secret filename' }
    return ''
}
function Get-CleanTreeEntries([string]$Root) {
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($Root)
    while ($pending.Count -gt 0) {
        foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            $entry
            if ($entry.PSIsContainer -and -not ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) { $pending.Push($entry.FullName) }
        }
    }
}
