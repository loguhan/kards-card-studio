<#
.SYNOPSIS
    KARDS 加卡工作流 —— 从卡定义到可进游戏的 _P.pak，全自动。

.DESCRIPTION
    把一条已经验证通过的链路固化成脚本。整条链路踩过的坑都在注释里标注，
    改动前请先读 _notes 和 README.md。

.EXAMPLE
    .\KardsCardWorkflow.ps1 check          # 环境自检
    .\KardsCardWorkflow.ps1 prepare        # 从游戏 pak 提取基底 + 装编辑器插件
    .\KardsCardWorkflow.ps1 build          # 全流程构建
    .\KardsCardWorkflow.ps1 deploy         # 部署到游戏 Paks
    .\KardsCardWorkflow.ps1 verify         # 离线校验产物
    .\KardsCardWorkflow.ps1 all            # build + deploy + verify
    .\KardsCardWorkflow.ps1 rollback       # 移出 pak
    .\KardsCardWorkflow.ps1 status         # 当前状态
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('check', 'prepare', 'build', 'deploy', 'verify', 'all', 'rollback', 'status', 'help')]
    [string]$Command = 'help',

    [string]$ConfigPath = (Join-Path $PSScriptRoot 'workflow.json')
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# ────────────────────────────────────────────────────────────────
# 基础设施
# ────────────────────────────────────────────────────────────────

function Write-Step($n, $text) {
    Write-Host ''
    Write-Host ("═" * 72) -ForegroundColor DarkCyan
    Write-Host (" [{0}] {1}" -f $n, $text) -ForegroundColor Cyan
    Write-Host ("═" * 72) -ForegroundColor DarkCyan
}
function Write-Ok($t)   { Write-Host "  ✔ $t" -ForegroundColor Green }
function Write-Warn($t) { Write-Host "  ! $t" -ForegroundColor Yellow }
function Write-Err($t)  { Write-Host "  ✘ $t" -ForegroundColor Red }
function Write-Info($t) { Write-Host "    $t" -ForegroundColor Gray }

function Assert-Path($p, $what) {
    if (-not (Test-Path -LiteralPath $p)) { throw "$what 不存在: $p" }
}

function Invoke-Exe {
    # 注意：参数名不能叫 $Args —— 那是 PowerShell 的自动变量，会引发难查的绑定问题。
    param([string]$Exe, [string[]]$ArgList, [string]$What, [switch]$AllowFail)
    # UE 的输出量极大（上万行），直接抓进变量会撑爆管道缓冲而死锁。
    # 统一重定向到临时文件再读回。
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("kcardwf_" + [guid]::NewGuid().ToString('N') + ".log")
    try {
        & $Exe @ArgList *> $tmp
        $code = $LASTEXITCODE
        $out = if (Test-Path -LiteralPath $tmp) { Get-Content -LiteralPath $tmp -ErrorAction SilentlyContinue } else { @() }
        if ($code -ne 0 -and -not $AllowFail) {
            $out | Select-Object -Last 20 | ForEach-Object { Write-Info $_ }
            throw "$What 失败 (exit=$code)"
        }
        return @{ Exit = $code; Output = $out }
    } finally {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    }
}

$Cfg = Get-Content -LiteralPath $ConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json

function P($expr) { Invoke-Expression "`$Cfg.$expr" }

$WorkRoot   = P 'paths.workRoot'
$StageRoot  = Join-Path $WorkRoot 'stage'
$OutRoot    = Join-Path $WorkRoot 'out'
$DiagRoot   = Join-Path $WorkRoot 'diag'
$BaseReg    = P 'paths.baseRegistryCache'
$CatalogBase = P 'paths.catalogCache'

function Reset-Dir($p) {
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $p | Out-Null
}

function Get-GameProcs { Get-Process -Name 'kards*' -ErrorAction SilentlyContinue }

# ────────────────────────────────────────────────────────────────
# check —— 环境自检
# ────────────────────────────────────────────────────────────────

function Invoke-Check {
    Write-Step 'check' '环境自检'
    $bad = 0
    $items = @(
        @{ n = 'UE 编辑器';       p = (P 'engine.editorCmd') },
        @{ n = 'UE 工程';         p = (P 'engine.project') },
        @{ n = 'AssetRegistryTool 插件源'; p = (P 'engine.assetRegistryToolPlugin') },
        @{ n = 'usmap 映射';      p = (P 'tools.usmap') },
        @{ n = 'kards-cardc';     p = (P 'tools.cardcDll') },
        @{ n = 'repak';           p = (P 'tools.repak') },
        @{ n = 'KardsTable';      p = (P 'tools.kardsTable') },
        @{ n = 'fix_registry_class.py'; p = (P 'tools.fixRegistryClass') },
        @{ n = '卡定义工程';      p = (P 'card.assemblyProject') },
        @{ n = 'profile';         p = (P 'profile') },
        @{ n = '游戏 Paks 目录';  p = (P 'game.paksDir') },
        @{ n = '游戏原版 pak';    p = (P 'game.basePak') }
    )
    foreach ($i in $items) {
        if (Test-Path -LiteralPath $i.p) { Write-Ok "$($i.n)" }
        else { Write-Err "$($i.n)  ->  $($i.p)"; $bad++ }
    }

    Write-Host ''
    $plug = Join-Path (P 'engine.projectPluginsDir') 'AssetRegistryTool'
    if (Test-Path -LiteralPath $plug) { Write-Ok "AssetRegistryTool 已装进工程" }
    else { Write-Warn "AssetRegistryTool 未装进工程（先跑 prepare）" }

    $reg = Join-Path (P 'game.paksDir') (P 'card.pakName')
    if (Test-Path -LiteralPath $reg) { Write-Warn "游戏里已有我们的 pak: $(P 'card.pakName')" }
    else { Write-Ok '游戏里没有我们的 pak（干净）' }

    $procs = Get-GameProcs
    if ($procs) { Write-Warn "游戏正在运行 (pid $($procs.Id -join ',')) —— 换 pak 前必须先关掉" }
    else { Write-Ok '游戏未运行' }

    Write-Host ''
    if ($bad -gt 0) { Write-Err "$bad 项缺失"; exit 1 }
    Write-Ok '环境就绪'
}

# ────────────────────────────────────────────────────────────────
# prepare —— 提取基底 + 装插件
# ────────────────────────────────────────────────────────────────

function Invoke-Prepare {
    Write-Step 'prepare' '提取基底资源 + 安装编辑器插件'

    # 1) usmap 放到 AssetRegistryTool 写死的路径
    $usmapDir = P 'tools.usmapStagingDir'
    $usmapDst = Join-Path $usmapDir (P 'tools.usmapStagingName')
    New-Item -ItemType Directory -Force -Path $usmapDir | Out-Null
    if (-not (Test-Path -LiteralPath $usmapDst)) {
        Copy-Item -LiteralPath (P 'tools.usmap') -Destination $usmapDst -Force
        Write-Ok "usmap -> $usmapDst"
    } else { Write-Ok "usmap 已就位" }

    # 2) 从游戏主 pak 提取【原版】AssetRegistry.bin 与卡目录表
    Reset-Dir (Join-Path $WorkRoot 'base')
    $repak = P 'tools.repak'; $key = P 'game.pakKey'; $basePak = P 'game.basePak'

    Invoke-Exe $repak @('-a', $key, 'get', $basePak, (P 'game.registryInPak')) -What 'repak get AssetRegistry.bin' -AllowFail | Out-Null
    & $repak -a $key get $basePak (P 'game.registryInPak') > $BaseReg 2>$null
    Assert-Path $BaseReg '原版 AssetRegistry.bin'
    Write-Ok ("原版注册表 {0:N0} 字节" -f (Get-Item $BaseReg).Length)

    foreach ($ext in @('.uasset', '.uexp')) {
        $rel = (P 'game.catalogInPak') + $ext
        $dst = $CatalogBase + $ext
        & $repak -a $key get $basePak $rel > $dst 2>$null
    }
    Assert-Path ($CatalogBase + '.uasset') '原版卡目录表'
    Write-Ok ("原版卡目录 {0:N0} 字节" -f (Get-Item ($CatalogBase + '.uasset')).Length)

    # 3) 装 AssetRegistryTool 插件 + 启用 + 禁掉会弹窗打断启动的坏插件
    $plugDst = Join-Path (P 'engine.projectPluginsDir') 'AssetRegistryTool'
    if (Test-Path -LiteralPath $plugDst) { Remove-Item -LiteralPath $plugDst -Recurse -Force }
    Copy-Item -LiteralPath (P 'engine.assetRegistryToolPlugin') -Destination $plugDst -Recurse -Force
    Write-Ok "插件已装 -> $plugDst"

    $uproj = P 'engine.project'
    $doc = Get-Content -LiteralPath $uproj -Raw -Encoding UTF8
    if ($doc -notmatch '"AssetRegistryTool"') {
        $doc = $doc -replace '(\s*)\]\s*\}\s*$', ",{`"Name`":`"AssetRegistryTool`",`"Enabled`":true,`"TargetAllowList`":[`"Editor`"]},{`"Name`":`"FullMatchRuntimeProbe`",`"Enabled`":false}]}"
        Set-Content -LiteralPath $uproj -Value $doc -Encoding UTF8 -NoNewline
        Write-Ok '已在 .uproject 启用 AssetRegistryTool 并禁用 FullMatchRuntimeProbe'
    } else { Write-Ok '.uproject 已配置' }
}

# ────────────────────────────────────────────────────────────────
# build —— 全流程
# ────────────────────────────────────────────────────────────────

function Invoke-AssetRegistryTool {
    param([string[]]$ToolArgs, [string]$What)
    $editor = P 'engine.editorCmd'
    $proj = P 'engine.project'
    $cmd = @($proj, '-run=AssetRegistryTool') + $ToolArgs +
           @('-unattended', '-nopause', '-nosplash', '-UTF8Output')
    $r = Invoke-Exe $editor $cmd -What $What -AllowFail
    $lines = $r.Output | Select-String -Pattern 'LogAssetRegistryTool' -ErrorAction SilentlyContinue |
        Where-Object { $_.Line -notmatch 'Asset with empty TagsAndValues|Listing assets from' }
    $lines | ForEach-Object { Write-Info ($_.Line -replace '^\[[^\]]+\]\s*', '') }
    return $r
}

function Invoke-Build {
    Write-Step 'build' '构建卡包'

    Assert-Path $BaseReg '基底注册表（先跑 prepare）'
    Assert-Path ($CatalogBase + '.uasset') '基底卡目录（先跑 prepare）'

    Reset-Dir $StageRoot
    Reset-Dir $OutRoot
    New-Item -ItemType Directory -Force -Path $DiagRoot | Out-Null

    $py = P 'tools.python'
    $usmap = P 'tools.usmap'
    $cardName = P 'card.name'
    $cdoExport = P 'card.cdoExport'

    # ── 1. 编译卡定义 ──────────────────────────────────────────
    Write-Host ''
    Write-Host '  ── 1/8 编译卡定义 ──' -ForegroundColor White
    $r = Invoke-Exe 'dotnet' @('build', (P 'card.assemblyProject'), '-c', 'Release') -What 'dotnet build'
    Write-Ok '卡定义已编译'

    # ── 2. 生成卡资产 + donor 注册表 ───────────────────────────
    Write-Host ''
    Write-Host '  ── 2/8 kards-cardc 生成卡资产 ──' -ForegroundColor White
    $ccOut = Join-Path $WorkRoot 'cardc'
    Reset-Dir $ccOut
    $r = Invoke-Exe 'dotnet' @(
        (P 'tools.cardcDll'), 'build',
        '--assembly', (P 'card.assembly'),
        '--type', (P 'card.type'),
        '--output', $ccOut,
        '--profile', (P 'profile'),
        '--pak-name', (P 'card.pakName')
    ) -What 'kards-cardc build'
    $r.Output | Select-Object -Last 5 | ForEach-Object { Write-Info $_ }
    $donorReg = Join-Path $ccOut 'stage\Root\AssetRegistry.bin'
    Assert-Path $donorReg 'donor 注册表'
    $cardUasset = Join-Path $ccOut ('stage\' + (P 'card.assetPathInPak') + '.uasset')
    Assert-Path $cardUasset '卡资产'

    # ── 3. 修 CDO ──────────────────────────────────────────────
    # 坑 1：CDO 只序列化非默认值，缺失属性必须【新增】。
    #       isInPermanentPool 编译器不写 -> 落回 C++ 默认 false -> 卡池过滤时被丢。
    #       CardID 是运行时实例 ID，真卡为 0。
    Write-Host ''
    Write-Host '  ── 3/8 修 CDO (isInPermanentPool / CardID) ──' -ForegroundColor White
    $cdoDir = Join-Path $WorkRoot 'cdo'
    Reset-Dir $cdoDir
    Copy-Item -LiteralPath $cardUasset -Destination $cdoDir -Force
    Copy-Item -LiteralPath ($cardUasset -replace '\.uasset$', '.uexp') -Destination $cdoDir -Force
    $cdoAsset = Join-Path $cdoDir (Split-Path $cardUasset -Leaf)
    $setArgs = @('setprop', $cdoAsset, $usmap, $cdoExport,
        'isInPermanentPool=true', 'CardID=0')
    # 卡定义可以要求补写额外的 CDO 属性（例如 usedTriggers 事件注册表）
    $extra = @()
    if ($Cfg.card.PSObject.Properties.Name -contains 'cdoExtra' -and $Cfg.card.cdoExtra) {
        $extra = @($Cfg.card.cdoExtra)
    }
    if ($extra.Count -gt 0) {
        $setArgs += $extra
        $extra | ForEach-Object { Write-Info "额外 CDO 属性: $_" }
    }
    $setArgs += $cdoAsset
    $r = Invoke-Exe (P 'tools.kardsTable') $setArgs -What 'KardsTable setprop'
    $r.Output | Where-Object { $_ -match 'OK|NEW|应用' } | ForEach-Object { Write-Info $_ }

    # ── 4. 第一次合并：host(原版) + donor(编译器产物) ───────────
    # 坑 2：Merge 必须带 -FilterPaths，否则全量合并会崩在 AssetRegistryTool.dll。
    Write-Host ''
    Write-Host '  ── 4/8 合并注册表（host + donor）──' -ForegroundColor White
    $merged1 = Join-Path $DiagRoot 'merged1.bin'
    Remove-Item -LiteralPath $merged1 -Force -ErrorAction SilentlyContinue
    Invoke-AssetRegistryTool @(
        'Merge',
        "-HostAssetRegistry=$BaseReg",
        "-AssetRegistry=$donorReg",
        "-OutputAssetRegistry=$merged1",
        "-FilterPaths=$(P 'card.filterPath')",
        '-OverwriteExistingAssets'
    ) -What 'AssetRegistryTool Merge #1' | Out-Null
    Assert-Path $merged1 '第一次合并结果'

    # ── 5. 导出 JSON ───────────────────────────────────────────
    # 坑 3：编译器 writer 不序列化 AssetClass，合并后新卡条目 AssetClass 为空。
    #       空的 AssetClass 会让 FAssetData::GetAsset() 解析不出资产 -> 卡进不了卡池。
    Write-Host ''
    Write-Host '  ── 5/8 导出 JSON 并补 AssetClass ──' -ForegroundColor White
    $listJson = Join-Path $DiagRoot 'merged1.json'
    Remove-Item -LiteralPath $listJson -Force -ErrorAction SilentlyContinue
    Invoke-AssetRegistryTool @('List', "-AssetRegistry=$merged1", "-OutFile=$listJson") -What 'AssetRegistryTool List' | Out-Null
    Assert-Path $listJson 'List 导出'

    $donorJson = Join-Path $DiagRoot 'donor_fixed.json'
    $r = Invoke-Exe $py @((P 'tools.fixRegistryClass'), $listJson, $donorJson, $cardName) -What 'fix_registry_class'
    $r.Output | ForEach-Object { Write-Info $_ }

    # ── 6. JSON -> Bin ─────────────────────────────────────────
    Write-Host ''
    Write-Host '  ── 6/8 JSON 回写为 bin（用 UE 自己的序列化器）──' -ForegroundColor White
    $donorFixed = Join-Path $DiagRoot 'donor_fixed.bin'
    Remove-Item -LiteralPath $donorFixed -Force -ErrorAction SilentlyContinue
    Invoke-AssetRegistryTool @('convert', "-JSON=$donorJson", "-OutFile=$donorFixed") -What 'AssetRegistryTool convert' | Out-Null
    Assert-Path $donorFixed '修正后的 donor'

    # ── 7. 第二次合并：host + 修好的 donor ──────────────────────
    # 坑 4：绝不能用 UAssetRegistry 的回写能力直接产出注册表 ——
    #       name 哈希表会被写成全零，游戏解析任何资产名都失败，整个卡库消失。
    Write-Host ''
    Write-Host '  ── 7/8 最终合并注册表 ──' -ForegroundColor White
    $finalReg = Join-Path $DiagRoot 'AssetRegistry.final.bin'
    Remove-Item -LiteralPath $finalReg -Force -ErrorAction SilentlyContinue
    Invoke-AssetRegistryTool @(
        'Merge',
        "-HostAssetRegistry=$BaseReg",
        "-AssetRegistry=$donorFixed",
        "-OutputAssetRegistry=$finalReg",
        '-OverwriteExistingAssets'
    ) -What 'AssetRegistryTool Merge #2' | Out-Null
    Assert-Path $finalReg '最终注册表'

    # ── 8. 卡目录加行 ──────────────────────────────────────────
    Write-Host ''
    Write-Host '  ── 8/8 往客户端卡目录加行 + 打包 ──' -ForegroundColor White
    $catDir = Join-Path $WorkRoot 'catalog'
    Reset-Dir $catDir
    foreach ($ext in @('.uasset', '.uexp')) {
        Copy-Item -LiteralPath ($CatalogBase + $ext) -Destination ($catDir + '\deckCodeIDsTable2' + $ext) -Force
    }
    $catAsset = Join-Path $catDir 'deckCodeIDsTable2.uasset'
    $r = Invoke-Exe (P 'tools.kardsTable') @(
        'add', $catAsset, $usmap, $cardName,
        "card=$cardName", "deck_code_id=$(P 'card.deckCode')", "ID=$(P 'card.id')",
        $catAsset
    ) -What 'KardsTable add'
    $r.Output | Where-Object { $_ -match '设 |已添加|已写出' } | ForEach-Object { Write-Info $_ }

    # ── 组装 pak ───────────────────────────────────────────────
    $unitsDir = Join-Path $StageRoot 'kards\Content\Blueprints\Cards\Mods\Soviet\units'
    $structsDir = Join-Path $StageRoot 'kards\Content\Structs'
    New-Item -ItemType Directory -Force -Path $unitsDir, $structsDir | Out-Null

    Copy-Item -LiteralPath $finalReg -Destination (Join-Path $StageRoot 'kards\AssetRegistry.bin') -Force
    Copy-Item -LiteralPath (Join-Path $cdoDir (Split-Path $cardUasset -Leaf)) -Destination $unitsDir -Force
    Copy-Item -LiteralPath (Join-Path $cdoDir ((Split-Path $cardUasset -Leaf) -replace '\.uasset$', '.uexp')) -Destination $unitsDir -Force
    Copy-Item -LiteralPath (Join-Path $catDir 'deckCodeIDsTable2.uasset') -Destination $structsDir -Force
    Copy-Item -LiteralPath (Join-Path $catDir 'deckCodeIDsTable2.uexp') -Destination $structsDir -Force

    $pak = Join-Path $OutRoot (P 'card.pakName')
    Invoke-Exe (P 'tools.repak') @('pack', $StageRoot, $pak, '--mount-point', '../../../') -What 'repak pack' | Out-Null
    Write-Ok "pak -> $pak"

    Write-Host ''
    Write-Host '  产物：' -ForegroundColor White
    Get-ChildItem -LiteralPath $StageRoot -Recurse -File |
        ForEach-Object { Write-Info ("{0,10:N0}  {1}" -f $_.Length, $_.FullName.Substring($StageRoot.Length + 1)) }

    Invoke-Verify -PakPath $pak
}

# ────────────────────────────────────────────────────────────────
# verify —— 离线校验
# ────────────────────────────────────────────────────────────────

function Invoke-Verify {
    param([string]$PakPath)

    Write-Step 'verify' '离线校验'
    if (-not $PakPath) { $PakPath = Join-Path $OutRoot (P 'card.pakName') }
    if (-not (Test-Path -LiteralPath $PakPath)) { $PakPath = Join-Path (P 'game.paksDir') (P 'card.pakName') }
    Assert-Path $PakPath '待校验 pak'
    Write-Info "pak: $PakPath"

    $repak = P 'tools.repak'; $key = P 'game.pakKey'; $py = P 'tools.python'
    $vdir = Join-Path $WorkRoot 'verify'
    Reset-Dir $vdir

    # a) 内容清单
    Write-Host ''
    Write-Host '  a) pak 内容' -ForegroundColor White
    $list = (& $repak -a $key list $PakPath 2>&1)
    $list | ForEach-Object { Write-Info $_ }
    $need = @(
        (P 'game.registryInPak'),
        ((P 'card.assetPathInPak') + '.uasset'),
        ((P 'card.assetPathInPak') + '.uexp'),
        ((P 'game.catalogInPak') + '.uasset'),
        ((P 'game.catalogInPak') + '.uexp')
    )
    $ok = $true
    foreach ($n in $need) {
        if ($list -match [regex]::Escape($n)) { Write-Ok "含 $n" } else { Write-Err "缺 $n"; $ok = $false }
    }

    # b) 注册表：哈希表 + AssetClass + cardSet
    Write-Host ''
    Write-Host '  b) AssetRegistry.bin' -ForegroundColor White
    $regOut = Join-Path $vdir 'AssetRegistry.bin'
    & $repak -a $key get $PakPath (P 'game.registryInPak') > $regOut 2>$null
    $listJson = Join-Path $vdir 'reg.json'
    Invoke-AssetRegistryTool @('List', "-AssetRegistry=$regOut", "-OutFile=$listJson") -What 'List 校验' | Out-Null

    $cardName = P 'card.name'
    $script = @"
import json,sys,struct
sys.stdout.reconfigure(encoding='utf-8')
reg=r'$regOut'; js=r'$listJson'; name=r'$cardName'
d=open(reg,'rb').read()
c,nsb=struct.unpack_from('<ii',d,24); hv=struct.unpack_from('<Q',d,32)[0]
seg=d[40:40+c*8]; z=100*seg.count(0)/len(seg)
print('  注册表 %d 字节  nameCount=%d  hashVersion=0x%016X  哈希区零字节 %.2f%%' % (len(d),c,hv,z))
print('  %s' % ('  ✔ 哈希表正常（<5%）' if z<5 else '  ✘ 哈希表被清零 -> 会毁掉整个卡库'))
a=json.load(open(js,encoding='utf-16'))['Assets']
hits=[e for e in a if name in e.get('AssetName','')]
print('  含 %s 的条目 %d 条' % (name, len(hits)))
for e in hits:
    t={x['TagName']:x['TagValue'] for x in e.get('TagsAndValues',[])}
    ac=e.get('AssetClass')
    flag='✔' if ac else '✘'
    print('    %s %-46s AssetClass=%r cardSet=%r' % (flag, e.get('AssetName'), ac, t.get('cardSet')))
    if not ac: print('       空 AssetClass -> GetAsset() 解析失败 -> 卡进不了卡池')
"@
    & $py -c $script

    # c) CDO 属性
    Write-Host ''
    Write-Host '  c) 卡 CDO 属性' -ForegroundColor White
    foreach ($ext in @('.uasset', '.uexp')) {
        & $repak -a $key get $PakPath ((P 'card.assetPathInPak') + $ext) > (Join-Path $vdir ('card' + $ext)) 2>$null
    }
    $r = Invoke-Exe (P 'tools.kardsTable') @('setprop', (Join-Path $vdir 'card.uasset'), (P 'tools.usmap'), (P 'card.cdoExport'), 'ZZZ_NOOP=1', (Join-Path $vdir 'card.uasset')) -What 'CDO 读取' -AllowFail
    $propLine = $r.Output | Where-Object { $_ -match '属性数=' } | Select-Object -First 1
    if ($propLine) {
        Write-Info $propLine
        $n = [int](($propLine -split '属性数=')[1])
        if ($n -ge 18) { Write-Ok "CDO 有 $n 个属性（含 isInPermanentPool）" }
        else { Write-Warn "CDO 只有 $n 个属性，可能缺 isInPermanentPool" }
    }

    # d) 卡目录
    Write-Host ''
    Write-Host '  d) 客户端卡目录' -ForegroundColor White
    & $repak -a $key get $PakPath ((P 'game.catalogInPak') + '.uasset') > (Join-Path $vdir 'cat.uasset') 2>$null
    & $repak -a $key get $PakPath ((P 'game.catalogInPak') + '.uexp') > (Join-Path $vdir 'cat.uexp') 2>$null
    # UAssetAPI 需要 .uasset 与同名 .uexp 成对存在
    $r = Invoke-Exe (P 'tools.kardsTable') @('dump', (Join-Path $vdir 'cat.uasset'), (P 'tools.usmap'), '1') -What '卡目录读取' -AllowFail
    $rowLine = $r.Output | Where-Object { $_ -match '^行数 = ' } | Select-Object -First 1
    if ($rowLine) {
        Write-Info $rowLine
        $rows = [int](($rowLine -split '行数 = ')[1])
        $cardName = P 'card.name'
        # 直接从 uexp 里确认卡名在表内（行数比原版多 1 也说明加上了）
        $uexpOut = Join-Path $vdir 'cat.uexp'
        $has = (Get-Content -LiteralPath $uexpOut -Raw -Encoding Latin1 -ErrorAction SilentlyContinue) -match [regex]::Escape($cardName)
        if ($has) { Write-Ok "卡目录 $rows 行，含 $cardName" }
        else { Write-Err "卡目录里没有 $cardName"; $ok = $false }
    }

    Write-Host ''
    if ($ok) { Write-Ok '校验通过' } else { Write-Err '校验未通过'; exit 1 }
}

# ────────────────────────────────────────────────────────────────
# deploy / rollback / status
# ────────────────────────────────────────────────────────────────

function Invoke-Deploy {
    Write-Step 'deploy' '部署到游戏'
    $pakName = P 'card.pakName'
    $src = Join-Path $OutRoot $pakName
    Assert-Path $src '待部署 pak（先跑 build）'

    $procs = Get-GameProcs
    if ($procs) {
        Write-Warn "游戏正在运行 (pid $($procs.Id -join ','))，正在停止以便替换 pak"
        $procs | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 8
    }

    $paksDir = P 'game.paksDir'
    $quar = P 'paths.quarantine'
    New-Item -ItemType Directory -Force -Path $quar | Out-Null
    Get-ChildItem -LiteralPath $paksDir -Filter 'KardsDIY_*' -File -ErrorAction SilentlyContinue | ForEach-Object {
        Move-Item -LiteralPath $_.FullName -Destination (Join-Path $quar $_.Name) -Force
        Write-Info "移出旧 pak: $($_.Name)"
    }
    Copy-Item -LiteralPath $src -Destination $paksDir -Force
    Write-Ok "已部署 $pakName"

    Write-Host ''
    Get-ChildItem -LiteralPath $paksDir -File |
        Select-Object @{ n = 'KB'; e = { [int]($_.Length / 1KB) } }, Name | Format-Table -AutoSize | Out-String | Write-Host
}

function Invoke-Rollback {
    Write-Step 'rollback' '移除 pak（恢复原版卡库）'
    $procs = Get-GameProcs
    if ($procs) {
        Write-Warn "停止游戏 (pid $($procs.Id -join ','))"
        $procs | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 8
    }
    $paksDir = P 'game.paksDir'; $quar = P 'paths.quarantine'
    New-Item -ItemType Directory -Force -Path $quar | Out-Null
    $moved = 0
    Get-ChildItem -LiteralPath $paksDir -Filter 'KardsDIY_*' -File -ErrorAction SilentlyContinue | ForEach-Object {
        Move-Item -LiteralPath $_.FullName -Destination (Join-Path $quar $_.Name) -Force
        Write-Ok "移出 $($_.Name)"; $moved++
    }
    if ($moved -eq 0) { Write-Info '没有需要移出的 pak' }
    Get-ChildItem -LiteralPath $paksDir -File | Select-Object Name | Format-Table -AutoSize | Out-String | Write-Host
}

function Invoke-Status {
    Write-Step 'status' '当前状态'
    $paksDir = P 'game.paksDir'
    Write-Host '  游戏 Paks:' -ForegroundColor White
    Get-ChildItem -LiteralPath $paksDir -File | Select-Object @{ n = 'KB'; e = { [int]($_.Length / 1KB) } }, Name |
        Format-Table -AutoSize | Out-String | Write-Host

    Write-Host '  构建产物:' -ForegroundColor White
    if (Test-Path -LiteralPath $OutRoot) {
        Get-ChildItem -LiteralPath $OutRoot -File | Select-Object @{ n = 'KB'; e = { [int]($_.Length / 1KB) } }, Name, LastWriteTime |
            Format-Table -AutoSize | Out-String | Write-Host
    } else { Write-Info '（还没构建过）' }

    $procs = Get-GameProcs
    if ($procs) { Write-Warn "游戏运行中: pid $($procs.Id -join ',')" } else { Write-Info '游戏未运行' }
}

# ────────────────────────────────────────────────────────────────
# 入口
# ────────────────────────────────────────────────────────────────

switch ($Command) {
    'check'    { Invoke-Check }
    'prepare'  { Invoke-Prepare }
    'build'    { Invoke-Build }
    'deploy'   { Invoke-Deploy }
    'verify'   { Invoke-Verify }
    'all'      { Invoke-Build; Invoke-Deploy; Invoke-Verify }
    'rollback' { Invoke-Rollback }
    'status'   { Invoke-Status }
    'help'     {
        Write-Host ''
        Write-Host ' KARDS 加卡工作流' -ForegroundColor Cyan
        Write-Host ' ─────────────────────────────────────────────────────────'
        Write-Host '   check      环境自检（工具/路径/插件/游戏状态）'
        Write-Host '   prepare    从游戏 pak 提取原版基底 + 装 AssetRegistryTool 插件'
        Write-Host '   build      全流程：卡定义 -> 资产 -> CDO 修正 -> 注册表 -> 卡目录 -> pak'
        Write-Host '   deploy     停游戏 -> 移走旧 pak -> 拷入新 pak'
        Write-Host '   verify     离线校验（pak 内容 / 哈希表 / AssetClass / cardSet / CDO / 卡目录）'
        Write-Host '   all        build + deploy + verify'
        Write-Host '   rollback   把 KardsDIY_*.pak 移出游戏目录'
        Write-Host '   status     当前 pak 与构建产物状态'
        Write-Host ''
        Write-Host '   ！换 pak 前必须关掉游戏，否则文件被锁。deploy 会自动停。'
        Write-Host '   ！测试 pak 停用时必须整个移出游戏安装目录，放进子目录仍会被加载。'
        Write-Host ''
    }
}
