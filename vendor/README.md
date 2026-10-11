# vendor/ —— 外部依赖放置处

本目录**不提交内容**（见 `.gitignore`）。请把你自己的依赖放进来。

## 必需：KardsCardCompiler

卡包 IR 定义（`CardDefinition` / `CardBlueprintDefinition` / `BlueprintFunctionIr`）
来自这个仓库。没有它，C# 卡包无法编译。

```powershell
git clone <KardsCardCompiler 仓库> vendor/KardsCardCompiler
dotnet build vendor/KardsCardCompiler/src/KardsCardCompiler.Core -c Release
```

或者不放进 vendor，改用环境变量指过去：

```powershell
$env:KARDS_COMPILER_SRC = 'C:\你的路径\KardsCardCompiler'
```

## 路径解析顺序

1. 环境变量 `KARDS_COMPILER_SRC`
2. `vendor/KardsCardCompiler`（本目录）
3. 都没有 → 编译时报错并给出提示

---

## 可选：KARDS 基础数据

流水线需要从原版游戏主 pak 里取出两样东西（**不要提交到仓库**）：

```
AssetRegistry.bin
deckCodeIDsTable2.uasset
deckCodeIDsTable2.uexp
```

放到 `config.local.json` 的 `baseDir` 指向的目录里，或用环境变量
`KARDS_BASE_DIR` 指定。

解包方法见根目录 `README.md` 的「准备基础数据」一节。
