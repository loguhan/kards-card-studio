<#
.SYNOPSIS
    一键准备本仓库的外部依赖与配置。

.DESCRIPTION
    做三件事：
      1. 检查 config.local.json，没有就从 config.example.json 复制
      2. 定位/克隆 KardsCardCompiler 到 vendor/，并构建它的 Core
      3. 构建本仓库自己的工具（KardsSpecBridge / KardsTable）

    路径已经配好且依赖就位时，重复运行是安全的（幂等）。

.PARAMETER CompilerSrc
    已有 KardsCardCompiler 仓库的路径。省略时会去 vendor/ 找，
    找不到就问你。

.PARAMETER SkipTools
    只准备依赖，不构建本仓库工具。

.EXAMPLE
    .\scripts\Setup.ps1
    .\scripts\Setup.ps1 -CompilerSrc C:\src\KardsCardCompiler
#>
[CmdletBinding()]
param(
    [string]$CompilerSrc = '',
    [switch]$SkipTools
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path $PSScriptRoot -Parent
$VendorDir = Join-Path $RepoRoot 'vendor'
$VendorCompiler = Join-Path $VendorDir 'KardsCardCompiler'
$LocalCfg = Join-Path $RepoRoot 'config.local.json'
$ExCfg = Join-Path $RepoRoot 'config.example.json'

function Step ($n) { Write-Host ''; Write-Host "── $n ──" -ForegroundColor Cyan }
function Ok   ($m) { Write-Host "  OK  $m" -ForegroundColor Green }
function Warn ($m) { Write-Host "  !!  $m" -ForegroundColor Yellow }
function Bad  ($m) { Write-Host "  XX  $m" -ForegroundColor Red }

Write-Host ''
Write-Host 'KARDS Card Studio —— 环境准备' -ForegroundColor White
Write-Host "仓库: $RepoRoot"

# ── 1. 配置 ───────────────────────────────────────────────────
Step '1/3  配置文件'
if (Test-Path $LocalCfg) {
    Ok 'config.local.json 已存在，跳过'
} elseif (Test-Path $ExCfg) {
    Copy-Item $ExCfg $LocalCfg
    Ok '已从 config.example.json 生成 config.local.json'
    Warn '请编辑 config.local.json 填入你机器上的真实路径：'
    Write-Host "      $LocalCfg" -ForegroundColor Yellow
} else {
    Bad '找不到 config.example.json'
}

# ── 2. KardsCardCompiler ──────────────────────────────────────
Step '2/3  KardsCardCompiler（卡包 IR 定义）'

$src = $CompilerSrc
if (-not $src) { $src = $env:KARDS_COMPILER_SRC }
if (-not $src -and (Test-Path $VendorCompiler)) { $src = $VendorCompiler }

if (-not $src) {
    Write-Host ''
    Warn '没找到 KardsCardCompiler。'
    Write-Host '  它是卡包 IR 定义（CardDefinition / BlueprintFunctionIr），构建卡包必需。' -ForegroundColor Yellow
    Write-Host ''
    $manual = Read-Host '  请输入已有仓库路径（直接回车 = 跳过）'
    if ($manual) { $src = $manual }
}

if ($src) {
    if (-not (Test-Path $src)) { throw "路径不存在: $src" }

    # 需要时软链进 vendor，让 csproj 的相对路径也能命中
    if (-not (Test-Path $VendorCompiler)) {
        New-Item -ItemType Directory -Path $VendorDir -Force | Out-Null
        if ((Resolve-Path $src).Path -ne (Resolve-Path $VendorDir).Path) {
            try {
                New-Item -ItemType Junction -Path $VendorCompiler -Target $src -ErrorAction Stop | Out-Null
                Ok "已链接 vendor/KardsCardCompiler → $src"
            } catch {
                Warn "无法建链接（$($_.Exception.Message)），改用环境变量"
                $env:KARDS_COMPILER_SRC = $src
            }
        }
    } else {
        Ok 'vendor/KardsCardCompiler 已就位'
    }

    $coreProj = Join-Path $src 'src\KardsCardCompiler.Core\KardsCardCompiler.Core.csproj'
    $coreDll = Join-Path $src 'src\KardsCardCompiler.Core\bin\Release\net8.0\KardsCardCompiler.Core.dll'

    if (-not (Test-Path $coreProj)) {
        Bad "不是有效的 KardsCardCompiler 仓库（缺 src\KardsCardCompiler.Core）: $src"
    } elseif (Test-Path $coreDll) {
        Ok 'KardsCardCompiler.Core 已编译'
    } else {
        Write-Host '  正在构建 KardsCardCompiler.Core …'
        & dotnet build $coreProj -c Release -v q --nologo
        if ($LASTEXITCODE -ne 0) { throw 'KardsCardCompiler.Core 构建失败' }
        Ok 'KardsCardCompiler.Core 构建完成'
    }
} else {
    Warn '已跳过 —— 卡包将无法编译'
}

# ── 3. 本仓库工具 ─────────────────────────────────────────────
if (-not $SkipTools) {
    Step '3/3  构建本仓库工具'
    foreach ($p in @(
            @{N='KardsSpecBridge'; P='tools\KardsSpecBridge\KardsSpecBridge.csproj'},
            @{N='KardsTable';      P='tools\KardsTable\KardsTable.csproj'})) {
        $proj = Join-Path $RepoRoot $p.P
        if (-not (Test-Path $proj)) { Warn "缺 $($p.P)"; continue }
        Write-Host "  构建 $($p.N) …"
        & dotnet build $proj -c Release -v q --nologo
        if ($LASTEXITCODE -ne 0) { Bad "$($p.N) 构建失败"; continue }
        Ok $p.N
    }
}

# ── 收尾自检 ──────────────────────────────────────────────────
Step '自检'
$cfg = $null
if (Test-Path $LocalCfg) {
    try { $cfg = Get-Content $LocalCfg -Raw -Encoding UTF8 | ConvertFrom-Json } catch {}
}

$todo = @()
foreach ($k in @(
        @{K='engine';      D='UE 5.6 安装目录'},
        @{K='project';     D='挂插件的 UE 工程 (.uproject)'},
        @{K='repak';       D='repak.exe'},
        @{K='paksDir';     D='游戏 Content\Paks 目录'},
        @{K='pakKey';      D='pak AES 密钥'},
        @{K='baseDir';     D='AssetRegistry.bin / deckCodeIDsTable2 所在目录'})) {
    $v = if ($cfg -and $cfg.PSObject.Properties.Name -contains $k.K) { [string]$cfg.($k.K) } else { '' }
    if (-not $v) { $todo += "  $($k.D)  →  config.local.json 的 $($k.K)" }
    elseif (-not (Test-Path $v) -and $k.K -ne 'pakKey') { $todo += "  $($k.D)  →  路径不存在: $v" }
}

if ($todo.Count -gt 0) {
    Write-Host ''
    Warn '还需要补以下配置才能真正跑流水线：'
    $todo | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
} else {
    Ok '全部配置就绪'
}

Write-Host ''
Write-Host '准备完成。跑一张卡试试：' -ForegroundColor Green
Write-Host '  cd workflow' -ForegroundColor Gray
Write-Host '  .\BuildFromCs.ps1 -PackCs ..\cards\BlitzArtillery1011Pack.cs -NoDeploy' -ForegroundColor Gray
Write-Host ''
