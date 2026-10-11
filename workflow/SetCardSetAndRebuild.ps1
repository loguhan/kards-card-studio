<#
把现有各任务的 spec 改成指定 cardSet，重建卡资产，然后交给多卡合并脚本打包部署。

用法：
  pwsh -File SetCardSetAndRebuild.ps1 -CardSet Expansion2 -OutDir <work> [-Deploy]
#>
[CmdletBinding()]
param(
    [string]$CardSet = 'Expansion2',
    [string]$OutDir  = '<REPO>\_work\expansion',
    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'
$Editor     = '<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Project    = '<UEPROJ>\kards.uproject'
$JobsRoot   = '<REPO>\web\data\jobs'
$MultiScript= '<REPO>\workflow\BuildMultiCardPak.ps1'
$Content    = '<UEPROJ>\Content'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$specDir = Join-Path $OutDir 'specs'
New-Item -ItemType Directory -Force -Path $specDir | Out-Null

function Say([string]$m) { Write-Host "  $m" }
function Step([string]$m) { Write-Host "`n[$m]" -ForegroundColor Cyan }
function Ok([string]$m) { Write-Host "  OK  $m" -ForegroundColor Green }

# ── 1) 收集每个已完成任务的最新 spec，改 cardSet ──────────────
Step "1/3 改写 spec 的 cardSet -> $CardSet"
$cards = @()
foreach ($d in Get-ChildItem $JobsRoot -Directory) {
    $jf = Join-Path $d.FullName 'job.json'
    if (-not (Test-Path $jf)) { continue }
    $j = Get-Content -Raw -Encoding UTF8 $jf | ConvertFrom-Json
    if ($j.status -ne 'done') { continue }

    # 取最后一轮的 spec
    $specs = Get-ChildItem $d.FullName -Filter 'spec_r*.json' -ErrorAction SilentlyContinue |
        Sort-Object { [int]($_.BaseName -replace '\D', '') }
    if (-not $specs) { Say "跳过（无 spec）: $($j.request.name)"; continue }
    $spec = Get-Content -Raw -Encoding UTF8 $specs[-1].FullName | ConvertFrom-Json

    $hit = $false
    foreach ($def in $spec.defaults) { if ($def.name -eq 'cardSet') { $def.value = $CardSet; $hit = $true } }
    if (-not $hit) {
        $spec.defaults += [pscustomobject]@{ name = 'cardSet'; type = 'enum'; value = $CardSet }
    }
    $name = $j.request.name
    $specPath = Join-Path $specDir "$name.json"
    $spec | ConvertTo-Json -Depth 40 | Set-Content -Encoding UTF8 $specPath

    $cards += [pscustomobject]@{
        cardName = $name
        package  = "/Game/Blueprints/Cards/Mods/$($j.request.faction)/$name"
        deckCode = $j.request.deckCode
        cardId   = [int]$j.request.cardId
        specPath = $specPath
        skipBuild= $false
    }
    Say "$name  cardSet=$CardSet"
}
Ok "共 $($cards.Count) 张卡"

# ── 2) 逐卡重建（带新 cardSet 的 CDO）────────────────────────
Step '2/3 重建卡资产'
$rebuilt = 0
foreach ($c in $cards) {
    $work = Join-Path $OutDir ('rb_' + $c.cardName)
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $rep = Join-Path $work 'build_report.json'
    Remove-Item $rep -ErrorAction SilentlyContinue
    $out = & $Editor $Project -run=KardsCardBuilder -Mode=Build "-Spec=$($c.specPath)" "-Report=$rep" -Force -unattended -nosplash -nopause -stdout 2>&1
    $okLine = $out | Select-String -Pattern 'report written.*success=(true|false)' | Select-Object -First 1
    if ($okLine -match 'success=false') {
        Say "  !! $($c.cardName) 重建失败"
        $out | Select-String -Pattern 'Error:' | Select-Object -First 3 | ForEach-Object { Say "     $_" }
        continue
    }
    $rebuilt++
    Say "  $($c.cardName) 已重建"
}
Ok "重建 $rebuilt / $($cards.Count)"

# ── 3) 多卡合并 + 部署 ───────────────────────────────────────
Step '3/3 合并成一个 pak'
$m = [pscustomobject]@{
    pakName          = "KardsDIY_$CardSet`_P.pak"
    filterPath       = '/Game/Blueprints/Cards/Mods'
    hostRegistry     = '<REPO>\_work\base\AssetRegistry.bin'
    baseCatalog      = '<REPO>\_work\base\deckCodeIDsTable2'
    catalogPathInPak = 'kards/Content/Structs/deckCodeIDsTable2'
    cards            = $cards
}
$manifest = Join-Path $OutDir 'manifest.json'
$m | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 $manifest

$args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $MultiScript,
           '-Manifest', $manifest, '-WorkRoot', $OutDir, '-NoPluginBuild')
if (-not $Deploy) { $args2 += '-NoDeploy' }
& 'C:\Program Files\PowerShell\7\pwsh.exe' @args2
