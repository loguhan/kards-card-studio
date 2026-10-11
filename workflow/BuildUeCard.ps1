<#
.SYNOPSIS
    UE 侧写卡流水线 —— 用 UE 编辑器自己拼 K2 节点，编译通过才出包。

.DESCRIPTION
    与 KardsCardWorkflow.ps1（编译器路线）并列的第二条路线。
    区别：
      * 蓝图不是「编译器拼字节码」，而是 KardsCardBuilder 在编辑器里
        直接 NewObject 出 UK2Node_Event / CallFunction / IfThenElse / VariableGet /
        VariableSet / MakeArray，用 UEdGraphSchema_K2 连线，再交给
        FKismetEditorUtilities::CompileBlueprint 编译。
      * 编译只要有一个 error 就 **不落盘**（命令返回非 0，资产不存在）。
      * 注册表不再由 C# writer 产出，而是 KardsCardBuilder -Mode=ScanRegistry
        用 UE 自己的 FAssetRegistryState::Save 写 donor，再交给
        AssetRegistryTool Merge 合并到 host。

    用法：
      .\BuildUeCard.ps1                                  # 用默认 spec 跑全流程
      .\BuildUeCard.ps1 -Spec ..\..\x.json               # 指定 spec
      .\BuildUeCard.ps1 -NoPluginBuild                   # 跳过 UBT 编译插件
      .\BuildUeCard.ps1 -NoDeploy                        # 只出 pak 不部署
      .\BuildUeCard.ps1 -DumpApi /Script/kards.BaseCardObject   # 只反射 API
#>
[CmdletBinding()]
param(
    [string]$Spec = (Join-Path $PSScriptRoot 'spec-intel-tampering.json'),
    [string]$WorkRoot = '',
    [string]$DumpApi,
    [string]$DumpEnum,
    [switch]$NoPluginBuild,
    [switch]$NoDeploy,
    [switch]$SkipBuild,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ── 工具链路径 ────────────────────────────────────────────────
# 优先读取 config.local.json（各人机器不同，不入库）；
# 不存在则用 config.example.json 的示例值 → 仍不对就报错提示。
$script:RepoRoot = Split-Path $PSScriptRoot -Parent

function Get-ToolchainConfig {
    param([string]$Root)
    $candidates = @(
        (Join-Path $Root 'config.local.json'),
        (Join-Path $PSScriptRoot 'config.local.json'),
        (Join-Path $Root 'config.example.json'),
        (Join-Path $PSScriptRoot 'config.example.json')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            Write-Verbose "配置: $c"
            return (Get-Content $c -Raw -Encoding UTF8 | ConvertFrom-Json)
        }
    }
    return $null
}

$cfg = Get-ToolchainConfig -Root $script:RepoRoot

# 取值优先级：配置文件 > 环境变量 > 报错
function Resolve-Path2 {
    param([string]$Key, [string]$EnvName, [string]$Fallback = '')
    if ($cfg -and $cfg.PSObject.Properties.Name -contains $Key -and $cfg.$Key) {
        return [string]$cfg.$Key
    }
    if ($EnvName -and (Get-Item "env:$EnvName" -ErrorAction SilentlyContinue)) {
        return [string](Get-Item "env:$EnvName").Value
    }
    if ($Fallback) { return $Fallback }
    return ''
}

$Engine      = Resolve-Path2 'engine'      'KARDS_ENGINE'
$EditorCmd   = Resolve-Path2 'editorCmd'   'KARDS_EDITOR_CMD'
$UBT         = Resolve-Path2 'ubt'         'KARDS_UBT'
$Project     = Resolve-Path2 'project'     'KARDS_PROJECT'
$ProjectRoot = Resolve-Path2 'projectRoot' 'KARDS_PROJECT_ROOT'
$Repak       = Resolve-Path2 'repak'       'KARDS_REPAK'
$Usmap       = Resolve-Path2 'usmap'       'KARDS_USMAP'
$Python      = Resolve-Path2 'python'      'KARDS_PYTHON'
$PaksDir     = Resolve-Path2 'paksDir'     'KARDS_PAKS_DIR'
$PakKey      = Resolve-Path2 'pakKey'      'KARDS_PAK_KEY'

# KardsTable 优先用仓库内源码构建的产物
$KardsTable  = Resolve-Path2 'kardsTable' 'KARDS_TABLE' `
                   (Join-Path $script:RepoRoot 'tools\KardsTable\bin\Release\net8.0\KardsTable.exe')

# 派生默认值
if (-not $EditorCmd  -and $Engine) { $EditorCmd = Join-Path $Engine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe' }
if (-not $UBT        -and $Engine) { $UBT       = Join-Path $Engine 'Engine\Build\BatchFiles\Build.bat' }
if (-not $ProjectRoot -and $Project) { $ProjectRoot = Split-Path $Project -Parent }

$BuildRoot   = Resolve-Path2 'buildRoot' 'KARDS_BUILD_ROOT' (Join-Path $script:RepoRoot '_build')
if (-not $WorkRoot) { $WorkRoot = $BuildRoot }
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null

# ── 配置自检：缺什么就明确说 ──────────────────────────────────
function Assert-Toolchain {
    $missing = @()
    foreach ($pair in @(
            @{N='engine  (UE 安装目录)';      V=$Engine;      E='KARDS_ENGINE'},
            @{N='editorCmd (UnrealEditor-Cmd)';V=$EditorCmd;  E='KARDS_EDITOR_CMD'},
            @{N='project (kards.uproject)';   V=$Project;     E='KARDS_PROJECT'},
            @{N='repak   (repak.exe)';        V=$Repak;       E='KARDS_REPAK'},
            @{N='paksDir (游戏 Paks 目录)';    V=$PaksDir;     E='KARDS_PAKS_DIR'},
            @{N='pakKey  (pak AES 密钥)';      V=$PakKey;      E='KARDS_PAK_KEY'})) {
        if (-not $pair.V) { $missing += "  $($pair.N)  → 配置 $($pair.E)" }
    }
    if ($missing.Count -gt 0) {
        Write-Host ''
        Write-Host '缺少工具链配置。请复制 config.example.json 为 config.local.json 并填写：' -ForegroundColor Red
        Write-Host (Join-Path $script:RepoRoot 'config.example.json') -ForegroundColor Yellow
        Write-Host ''
        $missing | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        Write-Host ''
        Write-Host '也可以改用环境变量（见 README）。' -ForegroundColor Yellow
        throw '工具链未配置'
    }
}

function Say  ($m) { Write-Host "  $m" }
function Step ($n) { Write-Host ''; Write-Host "── $n ──" -ForegroundColor Cyan }
function Ok   ($m) { Write-Host "  OK  $m" -ForegroundColor Green }
function Bad  ($m) { Write-Host "  !!  $m" -ForegroundColor Red }

Assert-Toolchain

# 注意：-Param=value 在 PowerShell 里不会被变量展开，必须自己拼成一个字符串。
function Run-Editor {
    param([string[]]$Args2, [string]$What)
    $argv = @($Project) + $Args2 + @('-unattended', '-nosplash', '-nopause', '-stdout', '-utf8output')
    $log = Join-Path $WorkRoot ('log_' + ($What -replace '[^A-Za-z0-9]', '_') + '.txt')
    & $EditorCmd @argv *> $log
    $code = $LASTEXITCODE
    $lines = Select-String -Path $log -Pattern 'LogKardsCardBuilderCmd|LogAssetRegistryTool' -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Line -replace '^\[[^\]]+\]\s*', '' }
    $lines | Select-Object -Last 12 | ForEach-Object { Say $_ }
    return $code
}

# ── 只反射 API / 枚举 ─────────────────────────────────────────
if ($DumpApi -or $DumpEnum) {
    New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
    if ($DumpApi) {
        $rep = Join-Path $WorkRoot 'dump.json'
        Remove-Item $rep -ErrorAction SilentlyContinue
        Run-Editor @('-run=KardsCardBuilder', '-Mode=Dump', "-Class=$DumpApi", "-Report=$rep") 'dump' | Out-Null
        & $Python (Join-Path $BuildRoot 'q.py') $rep all | Out-Host
    }
    if ($DumpEnum) {
        $rep = Join-Path $WorkRoot 'dump_enum.json'
        Remove-Item $rep -ErrorAction SilentlyContinue
        Run-Editor @('-run=KardsCardBuilder', '-Mode=DumpEnum', "-Enum=$DumpEnum", "-Report=$rep") 'dumpenum' | Out-Null
        & $Python -c "import json,io,sys;d=json.load(io.open(sys.argv[1],encoding='utf-8-sig'));print(', '.join('%s=%d'%(v['name'],v['value']) for v in d['values']))" $rep
    }
    return
}

$Spec = (Resolve-Path $Spec).Path
$Cfg = Get-Content -Raw -Encoding UTF8 $Spec | ConvertFrom-Json
$P = $Cfg.pipeline
if (-not $P) { throw "spec 里缺 pipeline 段" }

$CardName  = $P.cardName
$PakName   = $P.pakName
$Pkg       = $Cfg.package
$Work      = Join-Path $WorkRoot ('ue_' + $CardName)
$ContentUa = Join-Path $ProjectRoot ('Content\' + $Pkg.Substring('/Game/'.Length).Replace('/', '\') + '.uasset')

Write-Host "KARDS UE 写卡流水线" -ForegroundColor White
Say "spec      $Spec"
Say "package   $Pkg"
Say "asset     $ContentUa"

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

# ── 1/8 编译编辑器插件 ────────────────────────────────────────
Step '1/8 编译 KardsCardBuilder 插件'
if ($NoPluginBuild) {
    Say '跳过 (-NoPluginBuild)'
} else {
    $out = & $UBT kardsEditor Win64 Development "-project=$Project" -waitmutex 2>&1
    if ($out -match 'error [CL][0-9]+' -or $out -match 'Result: Failed') {
        $out | Select-String -Pattern 'error [CL][0-9]+|Result:' | Select-Object -First 20 | ForEach-Object { Say $_.Line }
        throw 'UBT 编译失败'
    }
    Ok '插件已编译'
}

# ── 2/8 在编辑器里拼节点 + 编译蓝图 ───────────────────────────
Step '2/8 UE 内拼 K2 节点并编译'
$repBuild = Join-Path $WorkRoot 'build_report.json'
Remove-Item $repBuild -ErrorAction SilentlyContinue
$buildArgs = @('-run=KardsCardBuilder', '-Mode=Build', "-Spec=$Spec", "-Report=$repBuild")
if ($Force) { $buildArgs += '-Force' }
if ($SkipBuild) {
    # 资产已由常驻编辑器会话（-Mode=Session，AI 直接操作节点图）落盘，这里只做校验。
    if (-not (Test-Path $ContentUa)) { throw "会话没有落盘资产：$ContentUa" }
    '{"success":true,"compiler_errors":0,"compiler_messages":[],"errors":[]}' |
        Set-Content -Encoding UTF8 $repBuild
    Ok "跳过拼节点（会话已落盘并编译通过）：$ContentUa"
} else {
$code = Run-Editor $buildArgs 'build'
if ($code -ne 0) {
    Bad '蓝图编译失败 —— 没有落盘任何资产'
    if (Test-Path $repBuild) {
        & $Python -c "import json,io,sys;d=json.load(io.open(sys.argv[1],encoding='utf-8-sig'));[print('   ERR ',e) for e in d['errors']];[print('   MSG ',m['severity'],m['message'][:220]) for m in d['compiler_messages'][:15]]" $repBuild
    }
    throw 'build 阶段失败'
}
if (-not (Test-Path $ContentUa)) { throw "命令成功但没有资产：$ContentUa" }
Ok "编译通过并落盘：$ContentUa"
}

# ── 3/8 回读校验 ──────────────────────────────────────────────
Step '3/8 回读校验（CDO + 图）'
$repVerify = Join-Path $WorkRoot 'verify_report.json'
Remove-Item $repVerify -ErrorAction SilentlyContinue
Run-Editor @('-run=KardsCardBuilder', '-Mode=Verify', "-Package=$Pkg", "-Report=$repVerify") 'verify' | Out-Null
& $Python -c @"
import json,io,sys
d=json.load(io.open(sys.argv[1],encoding='utf-8-sig'))
print('   status_ok=%s parent=%s' % (d.get('status_ok'), d.get('parent_class')))
for g in d.get('graphs',[]): print('   graph %s: %d nodes' % (g['name'], g['nodes']))
keep=('Name','title','Text','flavorText','Type','faction','kredits','rarity','cardSet','isInPermanentPool','usedTriggers')
for p in d.get('properties',[]):
    if p['name'] in keep: print('   CDO %-20s %s' % (p['name'], p['value'][:80]))
"@ $repVerify

# ── 4/8 扫出 donor 注册表 ─────────────────────────────────────
Step '4/8 ScanRegistry（UE 自己的 FAssetRegistryState::Save）'
$donor = Join-Path $Work 'donor.bin'
Remove-Item $donor -ErrorAction SilentlyContinue
$repScan = Join-Path $WorkRoot 'scan_report.json'
Remove-Item $repScan -ErrorAction SilentlyContinue
Run-Editor @('-run=KardsCardBuilder', '-Mode=ScanRegistry', "-FilterPath=$($P.filterPath)", "-Out=$donor", "-Report=$repScan") 'scan' | Out-Null
if (-not (Test-Path $donor)) { throw 'donor 注册表没生成' }
& $Python -c "import json,io,sys;d=json.load(io.open(sys.argv[1],encoding='utf-8-sig'));print('   donor assets = %d' % d['asset_count']);[print('   ',a['object_path'],'|',a['asset_class']) for a in d['assets']]" $repScan

# ── 5/8 合并进 host 注册表 ────────────────────────────────────
Step '4b/8 Strip —— 只留生成类（丢掉 UBlueprint / UEdGraph / K2Node）'
# 关键：UK2Node_* / UEdGraph 属于编辑器专用的 BlueprintGraph 模块，
# Shipping 包里根本没有这些类。带着它们出包，游戏链接 import 时直接 Fatal error。
# 顺序不能反：注册表条目是编辑器按「Blueprint 资产」登记的，所以必须先 ScanRegistry
# 再 Strip。
$repStrip = Join-Path $WorkRoot 'strip_report.json'
Remove-Item $repStrip -ErrorAction SilentlyContinue
Run-Editor @('-run=KardsCardBuilder', '-Mode=Strip', "-Package=$Pkg", "-Report=$repStrip") 'strip' | Out-Null

# UE 退出后文件句柄/落盘可能还差一拍，直接读到半截文件会误报"导出对象数过多"。
# 这里轮询重试，直到读出合理值或明确超时。
function Get-ExportCount {
    param([string]$Asset, [string]$Map, [int]$Retries = 8, [int]$DelayMs = 400)
    for ($i = 0; $i -lt $Retries; $i++) {
        $out = & $KardsTable exports $Asset $Map 2>&1
        $m = $out | Select-String -Pattern 'exports=(\d+)' | Select-Object -First 1
        if ($m -and $m.Line -match 'exports=(\d+)') {
            $v = [int]$Matches[1]
            # 0 表示没读到有效内容；>64 基本可判定是读到了半截/旧文件
            if ($v -gt 0 -and $v -le 64) { return $v }
            if ($i -lt $Retries - 1) { Start-Sleep -Milliseconds $DelayMs; continue }
            return $v
        }
        if ($i -lt $Retries - 1) { Start-Sleep -Milliseconds $DelayMs }
    }
    return -1
}

$n = Get-ExportCount -Asset $ContentUa -Map $Usmap
Say "导出对象数 = $n"
if ($n -lt 0) { throw 'Strip 后读不出资产' }
if ($n -gt 12) {
    Bad "导出对象数 $n 过多 —— 包里还有编辑器对象，游戏会 Fatal error"
    throw 'Strip 未生效'
}
Ok "只剩生成类 + CDO + 函数（$n 个导出）"

# 硬门槛：Shipping 引擎拒绝任何不带 PKG_FilterEditorOnly(0x80000000) 的包
#   if (!HasEditorOnlyData() && !(Flags & PKG_FilterEditorOnly)) return LINKER_Failed;
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
$flags = (& $Python -c $flagScript $ContentUa 2>&1 | Select-Object -Last 1)
if ($LASTEXITCODE -ne 0) {
    Bad "包 flags=$flags —— 缺 PKG_FilterEditorOnly(0x80000000)，Shipping 引擎必然 LINKER_Failed / Fatal error"
    throw 'Strip 没有设上 PKG_FilterEditorOnly'
}
Ok "包 flags=$flags 含 PKG_FilterEditorOnly"

Step '5/8 AssetRegistryTool Merge（host + donor）'
$merged = Join-Path $Work 'AssetRegistry.bin'
Remove-Item $merged -ErrorAction SilentlyContinue
Run-Editor @('-run=AssetRegistryTool', 'Merge',
    "-HostAssetRegistry=$($P.hostRegistry)", "-AssetRegistry=$donor",
    "-OutputAssetRegistry=$merged", "-FilterPaths=$($P.filterPath)",
    '-OverwriteExistingAssets') 'merge' | Out-Null
if (-not (Test-Path $merged)) { throw '合并失败' }
Ok ("合并后 {0:N0} 字节" -f (Get-Item $merged).Length)

# ── 6/8 校验注册表（哈希表 + AssetClass）──────────────────────
Step '6/8 注册表体检'
$listJson = Join-Path $Work 'reg.json'
Remove-Item $listJson -ErrorAction SilentlyContinue
Run-Editor @('-run=AssetRegistryTool', 'List', "-AssetRegistry=$merged", "-OutFile=$listJson") 'list' | Out-Null
& $Python -c @"
import json,io,struct,sys
sys.stdout.reconfigure(encoding='utf-8')
reg, js, name = sys.argv[1], sys.argv[2], sys.argv[3]
d=open(reg,'rb').read()
c,_=struct.unpack_from('<ii',d,24); hv=struct.unpack_from('<Q',d,32)[0]
seg=d[40:40+c*8]; z=100*seg.count(0)/len(seg)
print('   nameCount=%d hashVersion=0x%016X 哈希区零字节=%.2f%%' % (c,hv,z))
print('   ', 'OK 哈希表正常' if z<5 else '!! 哈希表被清零 —— 会毁掉整个卡库')
a=json.load(io.open(js,encoding='utf-16'))['Assets']
hits=[e for e in a if name in e.get('AssetName','')]
print('   命中 %d 条' % len(hits))
for e in hits:
    t={x['TagName']:x['TagValue'] for x in e.get('TagsAndValues',[])}
    print('   ', e['AssetName'], 'AssetClass=', repr(e.get('AssetClass')), 'cardSet=', t.get('cardSet'))
    if not e.get('AssetClass'): print('       !! AssetClass 为空 -> GetAsset() 解析失败 -> 卡进不了卡池')
"@ $merged $listJson $CardName

# ── 7/8 卡目录加行 + 组 pak ───────────────────────────────────
Step '7/8 卡目录加行 + 打 pak'
$catDir = Join-Path $Work 'catalog'
New-Item -ItemType Directory -Force -Path $catDir | Out-Null
foreach ($ext in @('.uasset', '.uexp')) {
    $src = $P.baseCatalog + $ext
    if (Test-Path $src) { Copy-Item $src (Join-Path $catDir ('deckCodeIDsTable2' + $ext)) -Force }
}
$catAsset = Join-Path $catDir 'deckCodeIDsTable2.uasset'
& $KardsTable add $catAsset $Usmap $CardName "card=$CardName" "deck_code_id=$($P.deckCode)" "ID=$($P.cardId)" $catAsset 2>&1 |
    Where-Object { $_ -match '设 |已添加|已写出' } | ForEach-Object { Say $_ }

$stage = Join-Path $Work 'stage'
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Split-Path (Join-Path $stage $P.assetPathInPak)) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path (Join-Path $stage $P.catalogPathInPak)) | Out-Null
Copy-Item $merged (Join-Path $stage 'kards\AssetRegistry.bin') -Force
foreach ($ext in @('.uasset', '.uexp')) {
    $s = $ContentUa -replace '\.uasset$', $ext
    if (Test-Path $s) { Copy-Item $s (Join-Path $stage ($P.assetPathInPak + $ext)) -Force }
}
foreach ($ext in @('.uasset', '.uexp')) {
    $s = Join-Path $catDir ('deckCodeIDsTable2' + $ext)
    if (Test-Path $s) { Copy-Item $s (Join-Path $stage ($P.catalogPathInPak + $ext)) -Force }
}
Get-ChildItem $stage -Recurse -File | ForEach-Object { Say ("{0,12:N0}  {1}" -f $_.Length, $_.FullName.Substring($stage.Length + 1)) }

$pak = Join-Path $Work $PakName
Remove-Item $pak -ErrorAction SilentlyContinue
& $Repak pack $stage $pak --mount-point '../../../' 2>&1 | Select-Object -Last 2 | ForEach-Object { Say $_ }
if (-not (Test-Path $pak)) { throw 'repak pack 失败' }
Ok ("pak -> $pak  ({0:N0} 字节)" -f (Get-Item $pak).Length)
& $Repak list $pak 2>&1 | ForEach-Object { Say "  in-pak: $_" }

# ── 8/8 部署 ──────────────────────────────────────────────────
Step '8/8 部署'
if ($NoDeploy) {
    Say "跳过 (-NoDeploy)；pak 在 $pak"
} else {
    Copy-Item $pak $PaksDir -Force
    Ok "已部署到 $PaksDir\$PakName"
    Get-ChildItem $PaksDir -File | ForEach-Object { Say ("{0,14:N0}  {1}" -f $_.Length, $_.Name) }
}

Write-Host ''
Write-Host '完成。' -ForegroundColor Green
