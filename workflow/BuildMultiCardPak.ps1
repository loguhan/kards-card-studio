<#
.SYNOPSIS
    把**多张卡**合并成**一个 pak**。

.DESCRIPTION
    和 BuildUeCard.ps1 的区别：那个是「一张卡一个 pak」，这个是「N 张卡一个 pak」。
    分解方式：
      每张卡  —— Build（拼节点/编译）→ Verify（回读）→ Strip（只留生成类）
      整体一次 —— ScanRegistry → Merge（host + 全部卡）→ 卡目录加 N 行 → 组 stage → repak

    关键点：ScanRegistry 是**按 filterPath 整体扫**的，所以只要所有卡都在同一个
    filterPath 下，一次扫描就能把 N 张卡全扫进 donor，再一次性合并进 host 注册表。
    Strip 必须先于 Scan 之后的 Merge（顺序注释见下）。

.EXAMPLE
    pwsh -File BuildMultiCardPak.ps1 -Manifest=C:\work\manifest.json -WorkRoot=C:\work
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Manifest,
    [string]$WorkRoot = '',
    [switch]$NoPluginBuild,
    [switch]$NoDeploy,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$BuildRoot = $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($WorkRoot)) { $WorkRoot = $BuildRoot }
if (-not [System.IO.Path]::IsPathRooted($Manifest)) { $Manifest = Join-Path (Get-Location) $Manifest }

$Editor      = '<UE>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$Project     = '<UEPROJ>\kards.uproject'
$BuildBat    = '<UE>\Engine\Build\BatchFiles\Build.bat'
$Python      = 'python'
$Usmap       = 'C:\Dumper-7\5.6.1-0+++UE5+Release-5.6-Fork-kards\Mappings\5.6.1-0+++UE5+Release-5.6-Fork-kards.usmap'
$Repak       = '<REPAK>'
$KardsTable  = '<REPO>\tools\KardsTable\bin\Release\net8.0\KardsTable.exe'
$PaksDir     = '<GAME>\kards\Content\Paks'

function Say([string]$m) { Write-Host "  $m" }
function Step([string]$m) { Write-Host "`n[$m]" -ForegroundColor Cyan }
function Ok([string]$m) { Write-Host "  OK  $m" -ForegroundColor Green }
function Bad([string]$m) { Write-Host "  !!  $m" -ForegroundColor Red }

function Run-Editor([string[]]$Args2, [string]$What) {
    $out = & $Editor $Project @Args2 -unattended -nosplash -nopause -stdout 2>&1
    $out | Where-Object { $_ -match 'LogKardsCardBuilderCmd|Error:|Fatal' } | Select-Object -First 20 | ForEach-Object { Say $_ }
    return $LASTEXITCODE
}

# ── 读 manifest ───────────────────────────────────────────────
$M = Get-Content -Raw -Encoding UTF8 $Manifest | ConvertFrom-Json
$Cards = @($M.cards)
if ($Cards.Count -lt 1) { throw 'manifest 里没有 cards' }
Write-Host "多卡合并：$($Cards.Count) 张 -> $($M.pakName)" -ForegroundColor Yellow

# ── 1/4 编译插件 ─────────────────────────────────────────────
Step '1/4 编译 KardsCardBuilder 插件'
if ($NoPluginBuild) { Say '跳过 (-NoPluginBuild)' }
else {
    & $BuildBat kardsEditor Win64 Development "-project=$Project" -waitmutex 2>&1 |
        Select-String -Pattern 'error C|error LNK|Result:' | Select-Object -First 10 | ForEach-Object { Say $_ }
    if ($LASTEXITCODE -ne 0) { throw '插件编译失败' }
    Ok '插件已编译'
}

# ── 2/4 每张卡：Build -> Verify -> Strip ─────────────────────
Step '2/4 逐卡拼节点 / 回读 / Strip'
$built = @()
foreach ($c in $Cards) {
    $name = $c.cardName
    $pkg = $c.package
    $cardWork = Join-Path $WorkRoot ('card_' + $name)
    New-Item -ItemType Directory -Force -Path $cardWork | Out-Null
    $repBuild = Join-Path $cardWork 'build_report.json'
    Remove-Item $repBuild -ErrorAction SilentlyContinue

    Say "── $name"

    if ($c.skipBuild) {
        Say '   跳过拼节点（资产已由编辑器会话落盘）'
        '{"success":true,"compiler_errors":0,"compiler_messages":[],"errors":[]}' |
            Set-Content -Encoding UTF8 $repBuild
    } else {
        $a = @('-run=KardsCardBuilder', '-Mode=Build', "-Spec=$($c.specPath)", "-Report=$repBuild")
        if ($Force) { $a += '-Force' }
        if ((Run-Editor $a 'build') -ne 0) {
            if (Test-Path $repBuild) {
                & $Python -c "import json,io,sys;d=json.load(io.open(sys.argv[1],encoding='utf-8-sig'));[print('     ERR ',e) for e in d['errors']];[print('     MSG ',m['message'][:200]) for m in d['compiler_messages'][:8]]" $repBuild
            }
            throw "$name 构建失败"
        }

        # Verify + Strip 只在「本轮真的重新拼了图」时做。
        # 已经 Strip 过的包带了 PKG_FilterEditorOnly + PKG_Cooked，编辑器加载不了，
        # 再跑 Verify/Strip 只会报 "cannot load ..."。
        $repVerify = Join-Path $cardWork 'verify_report.json'
        Remove-Item $repVerify -ErrorAction SilentlyContinue
        Run-Editor @('-run=KardsCardBuilder', '-Mode=Verify', "-Package=$pkg", "-Report=$repVerify") 'verify' | Out-Null

        $repStrip = Join-Path $cardWork 'strip_report.json'
        Remove-Item $repStrip -ErrorAction SilentlyContinue
        Run-Editor @('-run=KardsCardBuilder', '-Mode=Strip', "-Package=$pkg", "-Report=$repStrip") 'strip' | Out-Null
    }

    # 定位资产文件
    $rel = $pkg -replace '^/Game/', 'Content/'
    $contentUa = Join-Path '<UEPROJ>' ($rel + '.uasset')
    if (-not (Test-Path $contentUa)) { throw "$name 没有落盘资产：$contentUa" }

    $exLine = (& $KardsTable exports $contentUa $Usmap 2>&1 | Select-String -Pattern 'exports=(\d+)' | Select-Object -First 1)
    $n = if ($exLine -and $exLine.Line -match 'exports=(\d+)') { [int]$Matches[1] } else { -1 }
    if ($n -lt 0) { throw "$name 读不出资产" }

    # 导出数 > 12 说明这个包还没 Strip（里面有 UBlueprint / UEdGraph / K2Node 这些
    # 编辑器对象，Shipping 引擎链接时会 Fatal error）。就地 Strip 再复查。
    if ($n -gt 12) {
        Say "   导出数 $n —— 未 Strip，就地 Strip"
        $repStrip = Join-Path $cardWork 'strip_report.json'
        Remove-Item $repStrip -ErrorAction SilentlyContinue
        Run-Editor @('-run=KardsCardBuilder', '-Mode=Strip', "-Package=$pkg", "-Report=$repStrip") 'strip' | Out-Null
        $exLine = (& $KardsTable exports $contentUa $Usmap 2>&1 | Select-String -Pattern 'exports=(\d+)' | Select-Object -First 1)
        $n = if ($exLine -and $exLine.Line -match 'exports=(\d+)') { [int]$Matches[1] } else { -1 }
    }
    if ($n -lt 0) { throw "$name Strip 后读不出资产" }
    if ($n -gt 12) { throw "$name 导出对象数 $n 过多（包里还有编辑器对象）" }

    $flagScript = @'
import struct,sys
b=open(sys.argv[1],'rb').read()
o=24
v4,v5,vlic=struct.unpack_from('<iii',b,12)
o+=20
th=struct.unpack_from('<i',b,o)[0]; o+=4
cc=struct.unpack_from('<i',b,o)[0]; o+=4+cc*20
n=struct.unpack_from('<i',b,o)[0]; o+=4+n
fl=struct.unpack_from('<I',b,o)[0]
print('0x%08X' % fl)
sys.exit(0 if (fl & 0x80000000) else 1)
'@
    $flags = (& $Python -c $flagScript $contentUa 2>&1 | Select-Object -Last 1)
    if ($LASTEXITCODE -ne 0) {
        # 导出数检查可能漏掉（有些未 Strip 的包导出数不多），但 flags 骗不了人：
        # 没有 PKG_FilterEditorOnly 就是还没 Strip。就地 Strip 再复查。
        Say "   flags=$flags 缺 FilterEditorOnly —— 未 Strip，就地 Strip"
        $repStrip = Join-Path $cardWork 'strip_report.json'
        Remove-Item $repStrip -ErrorAction SilentlyContinue
        Run-Editor @('-run=KardsCardBuilder', '-Mode=Strip', "-Package=$pkg", "-Report=$repStrip") 'strip' | Out-Null
        $exLine = (& $KardsTable exports $contentUa $Usmap 2>&1 | Select-String -Pattern 'exports=(\d+)' | Select-Object -First 1)
        $n = if ($exLine -and $exLine.Line -match 'exports=(\d+)') { [int]$Matches[1] } else { -1 }
        $flags = (& $Python -c $flagScript $contentUa 2>&1 | Select-Object -Last 1)
        if ($n -lt 0) { throw "$name Strip 后读不出资产" }
        if ($n -gt 12) { throw "$name 导出对象数 $n 过多（包里还有编辑器对象）" }
        if ($LASTEXITCODE -ne 0) { throw "$name 包 flags=$flags 缺 PKG_FilterEditorOnly" }
    }

    Ok ("$name  exports=$n  flags=$flags")
    $built += [pscustomobject]@{ name = $name; package = $pkg; contentUa = $contentUa
        deckCode = $c.deckCode; cardId = $c.cardId }
}

# ── 3/4 整体一次：Scan -> Merge -> 目录 -> stage -> repak ────
Step '3/4 ScanRegistry（一次扫全部卡）'
$donor = Join-Path $WorkRoot 'donor.bin'
Remove-Item $donor -ErrorAction SilentlyContinue
$repScan = Join-Path $WorkRoot 'scan_report.json'
Remove-Item $repScan -ErrorAction SilentlyContinue
$includeNames = ($built | ForEach-Object { $_.name }) -join ','
Run-Editor @('-run=KardsCardBuilder', '-Mode=ScanRegistry', "-FilterPath=$($M.filterPath)",
    "-Include=$includeNames", "-Out=$donor", "-Report=$repScan") 'scan' | Out-Null
if (-not (Test-Path $donor)) { throw 'donor 注册表没生成' }
& $Python -c "import json,io,sys;d=json.load(io.open(sys.argv[1],encoding='utf-8-sig'));print('   donor assets = %d' % d['asset_count']);[print('   ',a['object_path'],'|',a['asset_class']) for a in d['assets']]" $repScan

Step '3/4 AssetRegistryTool Merge（host + 全部卡）'
$merged = Join-Path $WorkRoot 'AssetRegistry.bin'
Remove-Item $merged -ErrorAction SilentlyContinue
Run-Editor @('-run=AssetRegistryTool', 'Merge',
    "-HostAssetRegistry=$($M.hostRegistry)", "-AssetRegistry=$donor",
    "-OutputAssetRegistry=$merged", "-FilterPaths=$($M.filterPath)",
    '-OverwriteExistingAssets') 'merge' | Out-Null
if (-not (Test-Path $merged)) { throw '合并失败' }
Ok ("合并后 {0:N0} 字节" -f (Get-Item $merged).Length)

Step '3/4 注册表体检'
$listJson = Join-Path $WorkRoot 'reg.json'
Remove-Item $listJson -ErrorAction SilentlyContinue
Run-Editor @('-run=AssetRegistryTool', 'List', "-AssetRegistry=$merged", "-OutFile=$listJson") 'list' | Out-Null
& $Python -c @"
import json,io,struct,sys
sys.stdout.reconfigure(encoding='utf-8')
reg, js = sys.argv[1], sys.argv[2]
d=open(reg,'rb').read()
c,_=struct.unpack_from('<ii',d,24); hv=struct.unpack_from('<Q',d,32)[0]
seg=d[40:40+c*8]; z=100*seg.count(0)/len(seg)
print('   nameCount=%d hashVersion=0x%016X 哈希区零字节=%.2f%%' % (c,hv,z))
print('   ', 'OK 哈希表正常' if z<5 else '!! 哈希表被清零 —— 会毁掉整个卡库')
a=json.load(io.open(js,encoding='utf-16'))['Assets']
names=[x['AssetName'] for x in a if 'card_' in x.get('AssetName','')]
print('   注册表里 card_* 资产 %d 个' % len(names))
"@ $merged $listJson

Step '3/4 卡目录加行（每张卡一行）'
$catDir = Join-Path $WorkRoot 'catalog'
New-Item -ItemType Directory -Force -Path $catDir | Out-Null
foreach ($ext in @('.uasset', '.uexp')) {
    $src = $M.baseCatalog + $ext
    if (Test-Path $src) { Copy-Item $src (Join-Path $catDir ('deckCodeIDsTable2' + $ext)) -Force }
}
$catAsset = Join-Path $catDir 'deckCodeIDsTable2.uasset'
foreach ($b in $built) {
    & $KardsTable add $catAsset $Usmap $b.name "card=$($b.name)" "deck_code_id=$($b.deckCode)" "ID=$($b.cardId)" $catAsset 2>&1 |
        Where-Object { $_ -match '设 |已添加|已写出' } | ForEach-Object { Say "   $($b.name): $_" }
}
Ok "目录已加 $($built.Count) 行"

Step '3/4 组 stage + 打 pak'
$stage = Join-Path $WorkRoot 'stage'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Split-Path (Join-Path $stage $M.catalogPathInPak)) | Out-Null
Copy-Item $merged (Join-Path $stage 'kards\AssetRegistry.bin') -Force
foreach ($b in $built) {
    $inPak = $b.package -replace '^/Game/', 'kards/Content/'
    New-Item -ItemType Directory -Force -Path (Split-Path (Join-Path $stage $inPak)) | Out-Null
    foreach ($ext in @('.uasset', '.uexp')) {
        $s = $b.contentUa -replace '\.uasset$', $ext
        if (Test-Path $s) { Copy-Item $s (Join-Path $stage ($inPak + $ext)) -Force }
    }
}
foreach ($ext in @('.uasset', '.uexp')) {
    $s = Join-Path $catDir ('deckCodeIDsTable2' + $ext)
    if (Test-Path $s) { Copy-Item $s (Join-Path $stage ($M.catalogPathInPak + $ext)) -Force }
}
Get-ChildItem $stage -Recurse -File | ForEach-Object { Say ("{0,12:N0}  {1}" -f $_.Length, $_.FullName.Substring($stage.Length + 1)) }

$pak = Join-Path $WorkRoot $M.pakName
Remove-Item $pak -ErrorAction SilentlyContinue
& $Repak pack $stage $pak --mount-point '../../../' 2>&1 | Select-Object -Last 2 | ForEach-Object { Say $_ }
if (-not (Test-Path $pak)) { throw 'repak pack 失败' }
Ok ("pak -> $pak  ({0:N0} 字节, {1} 张卡)" -f (Get-Item $pak).Length, $built.Count)

# ── 4/4 部署 ─────────────────────────────────────────────────
Step '4/4 部署'
if ($NoDeploy) { Say "跳过 (-NoDeploy)；pak 在 $pak" }
else {
    Copy-Item $pak $PaksDir -Force
    Ok "已部署到 $PaksDir\$($M.pakName)"
}
