param([Parameter(Mandatory=$true)][string]$SourceRoot)
$ErrorActionPreference = 'Stop'
function Replace-Once([string]$Text, [string]$Before, [string]$After) {
    if ($Text.Contains($After)) { return $Text }
    if (($Text.Split(@($Before), [StringSplitOptions]::None).Count - 1) -ne 1) {
        throw 'Pinned bgfx swap-chain patch context mismatch'
    }
    return $Text.Replace($Before,$After)
}
$renderer = Join-Path $SourceRoot 'bgfx/src/renderer_d3d11.cpp'
$text = [IO.File]::ReadAllText($renderer)
$before = 'm_scd.swapEffect  = m_swapEffect;'
$after = $before + "`n`t`t`t`t`t`t`tm_scd.scaling     = DXGI_SCALING_STRETCH; // Falco: DISCARD cannot use SCALING_NONE."
$text = Replace-Once $text $before $after
if ([IO.File]::ReadAllText($renderer) -cne $text) { [IO.File]::WriteAllText($renderer,$text,[Text.UTF8Encoding]::new($false)) }

$dxgi = Join-Path $SourceRoot 'bgfx/src/dxgi.cpp'
$text = [IO.File]::ReadAllText($dxgi)
$flip = 'allowTearing && (_scd.swapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || _scd.swapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD)'
foreach ($prefix in @('scd.Flags |= ', 'scdFlags |= ')) {
    $text = Replace-Once $text ($prefix + 'allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;') ($prefix + $flip + ' ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;')
}
$text = Replace-Once $text 'm_tearingSupported = allowTearing;' ('m_tearingSupported = ' + $flip + ';')
if ([IO.File]::ReadAllText($dxgi) -cne $text) { [IO.File]::WriteAllText($dxgi,$text,[Text.UTF8Encoding]::new($false)) }
Write-Host 'Falco bgfx compatibility: DISCARD uses STRETCH; tearing limited to flip chains (create/resize/present).'
