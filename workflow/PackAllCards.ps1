<#
.SYNOPSIS
    把项目里所有自制卡打成 **一个** pak。

.DESCRIPTION
    为什么必须合并：每个单卡 pak 都带自己的 AssetRegistry.bin + deckCodeIDsTable2，
    游戏只会用优先级最高的那一份 —— 单卡 pak 同时装进去会互相覆盖，只有一张能生效。

    所以正确做法是一次性生成「全部卡的注册表 + 全部卡的目录 + 全部卡资产」放进一个 pak。

    流程：
      1. 扫 Content 下的自制卡（自动排除 probe/test/proof/session 之类探针）
      2. KardsCardBuilder -Mode=ScanRegistry 一次拿全部卡的注册表条目
      3. AssetRegistryTool Merge 合并进 host 基础注册表
      4. 复制基础卡目录，为每张卡加一行 deck code
      5. repak pack 成一个 pak
      6. 部署（默认部署，-NoDeploy 只出包）

.EXAMPLE
    .\PackAllCards.ps1
    .\PackAllCards.ps1 -Exclude '*probe*','*test*' -PakName KardsDIY_All_P.pak
    .\PackAllCards.ps1 -Only card_event_intelligence_tampering,card_unit_death_dragon
#>
[CmdletBinding()]
param(
    [string[]]$Exclude = @('*probe*', '*test*', '*proof*', '*session*', '*_tmp*', '*dummy*'),
    [string[]]$Only = @(),
    [string]$PakName = 'KardsDIY_AllCards_P.pak',
    [string]$DeckCodeMap = '<REPO>\workflow\cards.manifest.json',
    [int]$FirstCardId = 910100,
    [switch]$NoDeploy
)

$ErrorActionPreference = 'Stop'

$EditorCmd  = '<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Project    = '<UEPROJ>\kards.uproject'
$Repak      = '<REPAK>'
$KardsTable = '<REPO>\tools\KardsTable\bin\Release\net8.0\KardsTable.exe'
$Usmap      = 'C:\Dumper-7\5.6.1-0+++UE5\..\5.6.1-0+++UE5+Release-5.6-Fork-kards\Mappings\5.6.1-0+++UE5+Release-5.6-Fork-kards.usmap'
if (-not (Test-Path $Usmap)) { $Usmap = 'C:\Dumper-7\5.6.1-0+++UE5+Release-5.6-Fork-kards\Mappings\5.6.1-0+++UE5+Release-5.6-Fork-kards.usmap' }
$Python     = 'python'
$PaksDir    = '<GAME>\kards\Content\Paks'

$ContentRoot = '<UEPROJ>\Content\Blueprints\Cards\Mods'
$FilterPath  = '/Game/Blueprints/Cards/Mods'
$BaseReg     = '<REPO>\_work\base\AssetRegistry.bin'
$BaseCat     = '<REPO>\_work\base\deckCodeIDsTable2'
$Work        = '<REPO>\_build\allcards'
$Stage       = Join-Path $Work 'stage'

function Say($m) { Write-Host "  $m" }
function Step($m) { Write-Host ''; Write-Host "── $m ──" -ForegroundColor Cyan }
function Ok($m) { Write-Host "  OK  $m" -ForegroundColor Green }
function Bad($m) { Write-Host "  !!  $m" -ForegroundColor Red }

function RunEditor([string[]]$A, [string]$What) {
    $log = Join-Path $Work ("log_$What.txt")
    $argv = @($Project) + $A + @('-unattended', '-nosplash', '-nopause', '-stdout')
    & $EditorCmd @argv *> $log
    $code = $LASTEXITCODE
    Select-String -Path $log -Pattern 'LogKardsCardBuilderCmd|LogAssetRegistryTool' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Line -replace '^\[[^\]]+\]\s*', '' } | Select-Object -Last 8 | ForEach-Object { Say $_ }
    return $code
}

function PackagePathOf($File) {
    $rel = $File.FullName.Substring($ContentRoot.Length).TrimStart('\') -replace '\\', '/' -replace '\.uasset$', ''
    return "$FilterPath/$rel"
}

New-Item -ItemType Directory -Force -Path $Work | Out-Null
Write-Host 'KARDS 全卡合并打包' -ForegroundColor White

# ── 1. 发现卡 ─────────────────────────────────────────────────
Step '1/6 发现自制卡'
$all = Get-ChildItem $ContentRoot -Recurse -File -Filter *.uasset -ErrorAction SilentlyContinue
$cards = $all | Where-Object {
    $n = $_.BaseName
    if ($Only.Count -gt 0) { return $Only -contains $n }
    foreach ($p in $Exclude) { if ($n -like $p) { return $false } }
    return $true
} | Group-Object BaseName | ForEach-Object { $_.Group | Sort-Object FullName | Select-Object -First 1 } | Sort-Object BaseName

if (-not $cards) { Bad "没有可打包的卡（Content\Blueprints\Cards\Mods 下）"; exit 1 }
Say "共 $($cards.Count) 张："
$cards | ForEach-Object { Say ("  {0,-46} {1}" -f $_.BaseName, $_.FullName.Replace($ContentRoot, '')) }

# ── 2. deck code / 卡目录 ID ──────────────────────────────────
Step '2/6 卡目录行（deck code / ID）'
$map = @{}
if (Test-Path $DeckCodeMap) {
    (Get-Content -Raw -Encoding UTF8 $DeckCodeMap | ConvertFrom-Json).PSObject.Properties |
        ForEach-Object { $map[$_.Name] = @{ deck = $_.Value.deck; id = [int]$_.Value.id } }
    Say "读入 manifest：$($map.Count) 条"
}
# 从 web 流水线的任务里补齐
$jobsRoot = '<REPO>\web\data\jobs'
if (Test-Path $jobsRoot) {
    Get-ChildItem $jobsRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $jf = Join-Path $_.FullName 'job.json'
        if (Test-Path $jf) {
            try {
                $j = Get-Content -Raw -Encoding UTF8 $jf | ConvertFrom-Json
                $n = $j.request.name
                if ($n -and -not $map.ContainsKey($n)) { $map[$n] = @{ deck = $j.request.deckCode; id = [int]$j.request.cardId } }
            } catch { }
        }
    }
}
# 没有的自动分配
$used = New-Object System.Collections.Generic.HashSet[string]
foreach ($k in $map.Keys) { [void]$used.Add($map[$k].deck) }
$alpha = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'
$next = $FirstCardId
foreach ($c in $cards) {
    $n = $c.BaseName
    if (-not $map.ContainsKey($n)) {
        do { $code = '' + $alpha[$next % 36] + $alpha[($next / 36) % 36]; $next++ } while ($used.Contains($code))
        [void]$used.Add($code)
        $map[$n] = @{ deck = $code; id = $next - 1 }
        Say "  $n 自动分配 deck=$code id=$($map[$n].id)"
    } else {
        Say "  $n deck=$($map[$n].deck) id=$($map[$n].id)"
    }
}
$map | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $DeckCodeMap

# ── 3. 一次 ScanRegistry 拿全部条目 ───────────────────────────
Step '3/6 ScanRegistry（一次拿全部卡的注册表条目）'
Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $Stage 'kards\Content\Structs') | Out-Null
$donor = Join-Path $Work 'donor.bin'
$rep = Join-Path $Work 'scan.json'
Remove-Item $donor, $rep -ErrorAction SilentlyContinue
RunEditor @('-run=KardsCardBuilder', '-Mode=ScanRegistry', "-FilterPath=$FilterPath", "-Out=$donor", "-Report=$rep") 'scan' | Out-Null
if (-not (Test-Path $donor)) { Bad 'donor 注册表没生成'; exit 1 }
$scan = Get-Content -Raw -Encoding UTF8 $rep | ConvertFrom-Json
Say "donor 条目数 = $($scan.asset_count)"

# ── 4. 合并 + 校验 ───────────────────────────────────────────
Step '4/6 合并进 host 注册表'
# 关键：只合并「真正会放进 pak 的那几张卡」。
# ScanRegistry 是按目录扫的，会把同目录下的探针卡也带进来 —— 那些资产不在 pak 里，
# 注册表引用不存在的资产会让卡池出错。
$filter = ($cards | ForEach-Object { PackagePathOf $_ }) -join ','
Say "只合并 $($cards.Count) 个包：$filter"
$reg = Join-Path $Stage 'kards\AssetRegistry.bin'
Remove-Item $reg -ErrorAction SilentlyContinue
RunEditor @('-run=AssetRegistryTool', 'Merge',
    "-HostAssetRegistry=$BaseReg", "-AssetRegistry=$donor",
    "-OutputAssetRegistry=$reg", "-FilterPaths=$filter", '-OverwriteExistingAssets') 'merge' | Out-Null
if (-not (Test-Path $reg)) { Bad '合并失败'; exit 1 }
& $Python -c @"
import struct,sys
b=open(sys.argv[1],'rb').read()
c,_=struct.unpack_from('<ii',b,24); seg=b[40:40+c*8]
z=100*seg.count(0)/len(seg)
print('  注册表 %d 字节 nameCount=%d 哈希区零字节 %.2f%% %s' % (len(b), c, z, 'OK' if z<5 else 'BROKEN'))
"@ $reg

# ── 5. 卡目录 + 资产 ─────────────────────────────────────────
Step '5/6 组 pak（卡目录 + 全部卡资产）'
foreach ($ext in @('.uasset', '.uexp')) {
    $s = $BaseCat + $ext
    if (Test-Path $s) { Copy-Item $s (Join-Path $Stage ('kards\Content\Structs\deckCodeIDsTable2' + $ext)) -Force }
}
$catAsset = Join-Path $Stage 'kards\Content\Structs\deckCodeIDsTable2.uasset'
foreach ($c in $cards) {
    $n = $c.BaseName
    & $KardsTable add $catAsset $Usmap $n "card=$n" "deck_code_id=$($map[$n].deck)" "ID=$($map[$n].id)" $catAsset 2>&1 |
        Select-String -Pattern '已添加|已写出' | ForEach-Object { Say $_.Line.Trim() }
}
$staged = 0
foreach ($c in $cards) {
    $rel = $c.FullName.Substring($ContentRoot.Length).TrimStart('\')
    $dst = Join-Path $Stage ('kards\Content\Blueprints\Cards\Mods\' + $rel)
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    Copy-Item $c.FullName $dst -Force
    $uexp = $c.FullName -replace '\.uasset$', '.uexp'
    if (Test-Path $uexp) { Copy-Item $uexp ($dst -replace '\.uasset$', '.uexp') -Force }
    $staged++
}
Say "已放入 $staged 张卡的资产"
Get-ChildItem $Stage -Recurse -File | ForEach-Object { Say ("{0,12:N0}  {1}" -f $_.Length, $_.FullName.Substring($Stage.Length + 1)) }

$pak = Join-Path $Work $PakName
Remove-Item $pak -ErrorAction SilentlyContinue
& $Repak pack $Stage $pak --mount-point '../../../' 2>&1 | Select-Object -Last 2 | ForEach-Object { Say $_ }
if (-not (Test-Path $pak)) { Bad 'repak pack 失败'; exit 1 }
Ok ("pak -> $pak  ({0:N0} 字节)" -f (Get-Item $pak).Length)

# ── 6. 部署 ──────────────────────────────────────────────────
Step '6/6 部署'
if ($NoDeploy) {
    Say "跳过（-NoDeploy）；pak 在 $pak"
} else {
    # 先清掉会互相覆盖的单卡 pak
    Get-ChildItem $PaksDir -File -Filter 'KardsDIY_*_P.pak' -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item $_.FullName -Force
        Say "移除旧单卡 pak: $($_.Name)"
    }
    Copy-Item $pak $PaksDir -Force
    Ok "已部署 $PakName"
    Get-ChildItem $PaksDir -File | ForEach-Object { Say ("{0,14:N0}  {1}" -f $_.Length, $_.Name) }
}
Write-Host ''
Write-Host "完成：$($cards.Count) 张卡在一个 pak 里" -ForegroundColor Green
