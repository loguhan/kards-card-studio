# KARDS Card Studio

> 用 **C# 写卡牌逻辑**，编译出与手拼蓝图节点 100% 等价的 `.pak`。
>
> 作者：**愚蠢**

---

## 这是什么

一套完整的 KARDS 自制卡流水线。你只写 C#，它负责：

```
你的 C# 卡包
   │  KardsSpecBridge（把 C# 翻成编辑器指令）
   ▼
spec JSON
   │  KardsCardBuilder（UE 编辑器插件，真实 NewObject 出 K2 节点）
   ▼
真正的 UK2Node_* 图 → 编辑器编译 → 仅编译干净才落盘
   │  Strip（只留生成类，可被 shipping 加载）
   ▼
资产注册表合并 → 卡目录写行 → repak 打包
   ▼
KardsDIY_<卡名>_ue_P.pak
```

**关键点**：不是伪造字节码，是在真编辑器里拼真节点，交给真编译器编译。

---

## 能力

| 能力 | 支持 |
|---|---|
| 卡面（费用/攻防/距离/兵种/阵营/卡集/稀有度） | ✅ |
| 触发事件（50+ 个 `ERegisteredCardFunction`） | ✅ |
| 条件分支 / 复合效果 | ✅ |
| 读其它卡的属性 | ✅ |
| 类型转换（含动态 Cast） | ✅ |
| 遍历前线 / 卡组 | ✅ |
| 数组构造 | ✅ |
| 控制权转交、总部伤害 | ✅ |
| 任意官方 `UK2Node_*`（逃生舱） | ✅ |

---

## 快速开始

### 1. 环境要求

- Windows
- **Unreal Engine 5.6**（编辑器，含 `UnrealEditor-Cmd.exe`）
- **.NET 8 SDK**
- **repak** —— <https://github.com/trumank/repak>
- 一份 KARDS 客户端安装（用于取其 `Paks` 目录）
- KARDS 的可加载工程（用于挂载 `KardsCardBuilder` 插件）
- **[KardsCardCompiler](https://github.com/)** —— 卡包 IR 定义（外部依赖，见下）

### 2. 配置

```powershell
Copy-Item config.example.json config.local.json
notepad config.local.json
```

把里面的路径改成你机器上的真实路径：

| 字段 | 说明 |
|---|---|
| `engine` | UE 5.6 安装根目录 |
| `project` | 用来挂插件的 UE 工程（`kards.uproject`） |
| `repak` | repak.exe 路径 |
| `paksDir` | KARDS 客户端 `Content\Paks` 目录 |
| `pakKey` | pak AES 密钥 |
| `compilerSrc` | KardsCardCompiler 仓库路径 |
| `baseDir` | 存放 `AssetRegistry.bin` 与 `deckCodeIDsTable2*` 的目录（见下） |

> `config.local.json` 已在 `.gitignore` 里，**不会**被提交。

也可以改用环境变量：`KARDS_ENGINE`、`KARDS_PROJECT`、`KARDS_REPAK`、
`KARDS_PAKS_DIR`、`KARDS_PAK_KEY`、`KARDS_COMPILER_SRC`、`KARDS_BASE_DIR`。

### 3. 准备基础数据

流水线需要从**原版游戏主 pak** 里取出两样东西放进 `baseDir`：

```
AssetRegistry.bin          ← 原版资产注册表
deckCodeIDsTable2.uasset   ← 原版卡目录表
deckCodeIDsTable2.uexp
```

用 repak 解包游戏主 pak 后，它们分别位于：
`kards/AssetRegistry.bin` 与 `kards/Content/Structs/deckCodeIDsTable2.*`

```powershell
# 从你填的 paksDir 里找到主 pak（通常是 kards-Windows.pak）
repak -a <pakKey> unpack <主pak> -o base_extract
```

> 也可以跑 `.\workflow\BuildUeCard.ps1 -DumpApi /Script/kards.CardFunctionsStub`
> 之类的命令先把符号表 dump 出来，写卡时会用到。

### 4. 编译插件

把 `plugins/` 整个目录放进 UE 工程的 `Plugins/` 下：

```
<你的工程>\Plugins\KardsCardBuilder\
    KardsCardBuilder.uplugin
    Source\...
```

首次构建流水线时它会自动用 UBT 编译。

### 5. 写第一张卡

复制 `cards/BlitzArtillery1011Pack.cs` 改成你的卡。核心就是两件事：

```csharp
public sealed class MyCardPack : ICardPackDefinition, ISpecBlueprintDefinition
{
    // ① 卡面
    public IReadOnlyCollection<CardDefinition> DefineCards() => new[]
    {
        CardDefinition.Unit(id: 2001, name: "card_unit_my_card",
                faction: Faction.Soviet, cost: 3, attack: 2, defense: 4,
                range: 1, unitType: CardUnitType.Infantry)
            .Text("我的卡", "规则文本", "风味文本")
            .FromSet(CardSet.Basic).RarityOf(CardRarity.Common)
    };

    // ② 逻辑
    public SpecGraphSet DefineSpecGraphs(CardIr card) => new()
    {
        Variables = new[] { ("b_side", "enum:/Script/kards.ESideEnum") },
        Graphs = new[]
        {
            Logic.EventGraph(Logic.Trigger.OnOtherCardEnterPlay,
                Logic.EventBody(
                    Spec.Set("b_side", Spec.SelfSide()),
                    Spec.If(Logic.ParamPositive("Damage"),
                            Logic.Build(Logic.DamageFriendlyHq("Damage")))),
                owner: "/Script/kards.BaseCardObject"),
        },
    };
}
```

### 6. 构建

```powershell
cd workflow

# 完整流程（含部署）
.\BuildFromCs.ps1 -PackCs ..\cards\MyCardPack.cs

# 只出 pak，不部署
.\BuildFromCs.ps1 -PackCs ..\cards\MyCardPack.cs -NoDeploy
```

产物在 `_build\<卡名>\ue\ue_<资产名>\KardsDIY_<资产名>_ue_P.pak`。

---

## 目录结构

```
kards-card-studio/
├── cards/                      你的卡包（C#）
│   ├── BlitzArtillery1011Pack.cs    示例：苏联 1011 闪击炮兵
│   ├── IntelTamperingPack.cs        示例：苏联情报篡改（纯 IR 路线）
│   ├── DslProofPack.cs              能力验证：属性读取 / foreach
│   └── CastProofPack.cs             能力验证：cast / MakeArray
├── plugins/                    UE 编辑器插件（KardsCardBuilder）
│   └── KardsCardBuilder/Source/      拼 K2 节点 / 编译 / Strip / 扫注册表
├── tools/
│   ├── KardsSpecBridge/        C# → spec 翻译器 + 卡牌逻辑 DSL
│   │   ├── Spec.cs                    原生 spec 节点构建 API
│   │   ├── Logic.cs                   卡牌逻辑 DSL（条件/动作/触发点常量）
│   │   └── CS-LOGIC-AUTHORING.md      ★ C# 写卡完整指南
│   ├── KardsTable/             卡目录表读写
│   └── *.py                    注册表修补脚本
├── workflow/                   流水线脚本
│   ├── BuildFromCs.ps1               ★ 入口：C# → pak
│   ├── BuildUeCard.ps1               spec → UE 8 步 → pak
│   └── ...
├── docs/                       参考文档
├── config.example.json         配置模板
└── .gitignore
```

---

## 文档

| 文档 | 内容 |
|---|---|
| [CS-LOGIC-AUTHORING.md](tools/KardsSpecBridge/CS-LOGIC-AUTHORING.md) | **C# 写卡完整指南**：能力矩阵、示例、8 个实测坑、工作流 |
| [tools/KardsSpecBridge/README.md](tools/KardsSpecBridge/README.md) | 桥接器架构、翻译规则、已知限制 |
| [workflow/README.md](workflow/README.md) | 路线 B 管线细节 |
| [docs/pipeline-README.md](docs/pipeline-README.md) | 流水线总览 |

---

## 必须知道的坑

写卡时最容易卡的几处（全部实测踩过，详见 CS-LOGIC-AUTHORING.md）：

1. **`ETypeEnum` 没有 `unit`** —— 单位卡必须落到具体兵种
   （`artillery=7` / `infantry=6` / `tank=3` …）
2. **`cast` 字段名不一致** —— 表达式用 `class`，语句用 `name`
3. **插件不做隐式 Cast** —— 把 `BaseCardObject` 接到形参为基类 `Object` 的引脚会报
   `incompatible pins`，需显式向上转型
4. **`seq` 节点未实现** —— 多出参函数不能塞进一个 `seq`，桥接器改为平铺兄弟语句
5. **变量名会撞父类成员** —— 原生 spec 变量请自带 `b_` 前缀
6. **没有 `OnPlayedFromHand`** —— 只有 `OnOtherCardPlayedFromHand`（别的卡被打出）

---

## 第三方与外部依赖

本项目**不包含**以下内容，请自行获取：

| 依赖 | 说明 |
|---|---|
| [KardsCardCompiler](https://github.com/) | 卡包 IR 定义（`CardDefinition` / `BlueprintFunctionIr`） |
| [repak](https://github.com/trumank/repak) | pak 打包工具 |
| Unreal Engine 5.6 | 引擎 |
| KARDS 游戏客户端 | 基础资产（`AssetRegistry.bin` / `deckCodeIDsTable2`） |

`plugins/` 下的 `KardsCardBuilder` 为本项目自研代码。
原 `tools/UAssetRegistry`（CCB-TEAM, MIT）与 `tools/uapi` 预编译 DLL 已从仓库移除，
如需请见 [UAssetRegistry](https://github.com/CCB-TEAM/UAssetRegistry)。

---

## 许可

MIT —— 见 [LICENSE](LICENSE)。

仅供**个人学习与单机自制内容**使用。请勿用于联网对战、盈利或任何
侵犯原作品权利的用途。

---

## 署名

**愚蠢**
