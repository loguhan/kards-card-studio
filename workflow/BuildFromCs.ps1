<#
.SYNOPSIS
    C# 写卡 → KardsCardBuilder spec → UE 侧 8 步 → pak → 部署。

.DESCRIPTION
    「只写 C#，拿到和手拼节点 100% 等价的卡」的入口脚本。

    C# 侧产物经 KardsSpecBridge 翻译成 spec JSON，再交给 BuildUeCard.ps1
    （路线 B，UE 编辑器真实拼 K2 节点 + 编译 + Strip + repak）。

    管线：
      [1] dotnet build  卡包 .cs → .dll
      [2] KardsSpecBridge  .dll → spec.json
      [3] BuildUeCard.ps1  spec.json → K2 节点 → 编译 → Strip → pak → 部署

.PARAMETER PackCs
    卡包源文件或.csproj。若是 .cs 单文件，会自动生成一个临时工程编译它。

.PARAMETER Options
    options.json 路径（流水线参数：package / registry / deckCode ...）。
    省略时用本目录的 cs-options.json（若存在）。

.PARAMETER NoDeploy
    只出 pak，不部署到游戏。

.PARAMETER SkipBridgeBuild
    跳过 KardsSpecBridge 自身编译（已编译过时用）。

.EXAMPLE
    .\BuildFromCs.ps1 -PackCs ..\cards\IntelTamperingPack.cs
    .\BuildFromCs.ps1 -PackCs ..\cards\IntelTamperingPack.cs -NoDeploy
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackCs,
    [string]$Options = '',
    [string]$WorkRoot = '',
    [switch]$NoDeploy,
    [switch]$SkipBridgeBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ── 工具链（与 BuildUeCard.ps1 共用 config.local.json）──────────
$script:RepoRoot = Split-Path $PSScriptRoot -Parent
$ToolsRoot   = Join-Path $script:RepoRoot 'tools'
$BridgeRoot  = Join-Path $ToolsRoot 'KardsSpecBridge'
$BridgeDll   = Join-Path $BridgeRoot 'bin\Release\net8.0\KardsSpecBridge.dll'
$BuildUeCard = Join-Path $PSScriptRoot 'BuildUeCard.ps1'

function Get-RepoConfig {
    param([string]$Root)
    foreach ($c in @(
            (Join-Path $Root 'config.local.json'),
            (Join-Path $PSScriptRoot 'config.local.json'),
            (Join-Path $Root 'config.example.json'),
            (Join-Path $PSScriptRoot 'config.example.json'))) {
        if (Test-Path $c) { return (Get-Content $c -Raw -Encoding UTF8 | ConvertFrom-Json) }
    }
    return $null
}

$cfg = Get-RepoConfig -Root $script:RepoRoot

function Get-Cfg {
    param([string]$Key, [string]$EnvName, [string]$Default = '')
    if ($cfg -and $cfg.PSObject.Properties.Name -contains $Key -and $cfg.$Key) { return [string]$cfg.$Key }
    if ($EnvName -and (Get-Item "env:$EnvName" -ErrorAction SilentlyContinue)) {
        return [string](Get-Item "env:$EnvName").Value
    }
    return $Default
}

# KardsCardCompiler.Core —— 路线 A 的编译器（外部依赖，需自备）
$CompilerSrc = Get-Cfg 'compilerSrc' 'KARDS_COMPILER_SRC' ''
if (-not $CompilerSrc) {
    # 尝试仓库内 vendored 副本
    $vend = Join-Path $script:RepoRoot 'vendor\KardsCardCompiler'
    if (Test-Path $vend) { $CompilerSrc = $vend }
}
$BuildRoot = Get-Cfg 'buildRoot' 'KARDS_BUILD_ROOT' (Join-Path $script:RepoRoot '_build')
if (-not $WorkRoot) { $WorkRoot = Join-Path $BuildRoot 'cs' }

New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null

function Say  ($m) { Write-Host "  $m" }
function Step ($n) { Write-Host ''; Write-Host "── $n ──" -ForegroundColor Cyan }
function Ok   ($m) { Write-Host "  OK  $m" -ForegroundColor Green }
function Bad  ($m) { Write-Host "  !!  $m" -ForegroundColor Red }

# ── KardsCardCompiler.Core 定位（卡包编译必需）──────────────────
if ($CompilerSrc) {
    $CoreDll = Join-Path $CompilerSrc 'src\KardsCardCompiler.Core\bin\Release\net8.0\KardsCardCompiler.Core.dll'
    $CoreProj = Join-Path $CompilerSrc 'src\KardsCardCompiler.Core\KardsCardCompiler.Core.csproj'
    if (-not (Test-Path $CoreDll) -and (Test-Path $CoreProj)) {
        Say 'KardsCardCompiler.Core 未编译，正在构建…'
        & dotnet build $CoreProj -c Release -v q --nologo *> (Join-Path $WorkRoot 'core_build.log')
        if ($LASTEXITCODE -ne 0) {
            Bad 'KardsCardCompiler.Core 编译失败'
            Get-Content (Join-Path $WorkRoot 'core_build.log') -Tail 25 | ForEach-Object { Say $_ }
            throw 'core build 失败'
        }
    }
}

# ── 0. 解析输入 ────────────────────────────────────────────────
$PackCs = (Resolve-Path $PackCs).Path
if (-not $Options) {
    $def = Join-Path $PSScriptRoot 'cs-options.json'
    if (Test-Path $def) { $Options = $def }
}

Write-Host ''
Write-Host 'KARDS C# 写卡流水线（桥接 KardsCardBuilder）' -ForegroundColor White
Say "卡包     $PackCs"
Say "工作根   $WorkRoot"
if ($Options) { Say "options  $Options" }

# ── 1. 编译 KardsSpecBridge（按需）──────────────────────────────
Step '1/3  准备 KardsSpecBridge'
if ($SkipBridgeBuild -and (Test-Path $BridgeDll)) {
    Say '跳过（-SkipBridgeBuild）'
} else {
    & dotnet build $BridgeRoot -c Release -v q --nologo *> (Join-Path $WorkRoot 'bridge_build.log')
    if ($LASTEXITCODE -ne 0) {
        Bad 'KardsSpecBridge 编译失败'
        Get-Content (Join-Path $WorkRoot 'bridge_build.log') -Tail 25 | ForEach-Object { Say $_ }
        throw 'bridge build 失败'
    }
    Ok $BridgeDll
}

# ── 2. 编译卡包 → dll，再翻译成 spec ───────────────────────────
Step '2/3  C# 卡包 → spec JSON'

$packDll = $null
if ($PackCs.EndsWith('.csproj', [StringComparison]::OrdinalIgnoreCase)) {
    # 用户给了完整工程
    $packDll = & dotnet build $PackCs -c Release --nologo `
        -p:OutputPath="$WorkRoot\packbin\" 2>&1 |
        Select-String -Pattern '->\s*(.+\.dll)' | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() } |
        Select-Object -Last 1
    if (-not $packDll -or -not (Test-Path $packDll)) {
        Bad '卡包工程编译失败或找不到输出 dll'
        throw 'pack build 失败'
    }
} else {
    # 单文件 .cs → 就地生成一个最小工程编译它
    $projDir = Join-Path $WorkRoot 'packproj'
    New-Item -ItemType Directory -Force -Path $projDir | Out-Null

    if (-not $CompilerSrc -or -not (Test-Path $CoreProj)) {
        Write-Host ''
        Bad '找不到 KardsCardCompiler.Core —— 卡包编译需要它。'
        Say  '请任选其一：'
        Say  '  1. 在 config.local.json 里加 "compilerSrc": "<KardsCardCompiler 仓库路径>"'
        Say  '  2. 设环境变量 KARDS_COMPILER_SRC'
        Say  '  3. 把仓库放到 <本仓库>\vendor\KardsCardCompiler'
        Write-Host ''
        throw 'KardsCardCompiler.Core 未配置'
    }

    $csproj = @"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <AssemblyName>UserCardPack</AssemblyName>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="$PackCs" />
    <Reference Include="KardsCardCompiler.Core">
      <HintPath>$CompilerSrc\src\KardsCardCompiler.Core\bin\Release\net8.0\KardsCardCompiler.Core.dll</HintPath>
      <Private>true</Private>
    </Reference>
    <!-- C# 卡牌逻辑 DSL（Spec / Logic）与 ISpecBlueprintDefinition -->
    <Reference Include="KardsSpecBridge">
      <HintPath>$BridgeRoot\bin\Release\net8.0\KardsSpecBridge.dll</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
</Project>
"@
    Set-Content -Path (Join-Path $projDir 'UserCardPack.csproj') -Value $csproj -Encoding UTF8

    $packDll = Join-Path $projDir 'bin\Release\net8.0\UserCardPack.dll'
    & dotnet build (Join-Path $projDir 'UserCardPack.csproj') -c Release -v q --nologo `
        *> (Join-Path $WorkRoot 'pack_build.log')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $packDll)) {
        Bad '卡包编译失败'
        Get-Content (Join-Path $WorkRoot 'pack_build.log') -Tail 30 | ForEach-Object { Say $_ }
        throw 'pack build 失败'
    }
}
Ok "卡包 dll: $packDll"

$specPath = Join-Path $WorkRoot 'spec_from_cs.json'
$bridgeArgs = @($BridgeDll, $packDll, $specPath)
if ($Options) { $bridgeArgs += $Options }

# 基础数据目录（AssetRegistry.bin / deckCodeIDsTable2）—— 经环境变量传给桥接器。
# 优先级：config.local.json 的 baseDir > 已设的 KARDS_BASE_DIR > 桥接器自行推导
$baseDir = Get-Cfg 'baseDir' 'KARDS_BASE_DIR' ''
if ($baseDir) { $env:KARDS_BASE_DIR = $baseDir }

& dotnet @bridgeArgs
if ($LASTEXITCODE -ne 0) {
    Bad 'C# → spec 翻译失败'
    throw 'bridge 失败'
}
if (-not (Test-Path $specPath)) { throw "spec 未生成: $specPath" }

# 语法自检：必须是合法 JSON
try { $null = Get-Content $specPath -Raw | ConvertFrom-Json }
catch { Bad "生成的 spec 不是合法 JSON: $_"; throw 'spec 语法错误' }
Ok "spec: $specPath"

# ── 3. 交给 UE 侧 8 步 ─────────────────────────────────────────
Step '3/3  spec → UE 编辑器拼 K2 节点 → pak → 部署'

$ueArgs = @{
    Spec          = $specPath
    WorkRoot      = (Join-Path $WorkRoot 'ue')
    Force         = $true
    NoPluginBuild = $true
}
if ($NoDeploy) { $ueArgs['NoDeploy'] = $true }

& $BuildUeCard @ueArgs
if ($LASTEXITCODE -ne 0) {
    Bad 'UE 侧构建失败（详见上方日志）'
    throw 'UE 侧构建失败'
}

Write-Host ''
Write-Host '完成：C# 写卡已产出与手拼节点等价的 pak。' -ForegroundColor Green
