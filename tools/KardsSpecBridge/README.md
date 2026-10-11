# KardsSpecBridge —— C# 写卡 → KardsCardBuilder

**只写 C#，拿到和手拼 K2 节点 100% 等价的卡。**

```
你写的 C# 卡包
   │  ICardPackDefinition + ICardBlueprintDefinition（蓝图 IR）
   ▼
KardsSpecBridge（本工具）      ← 把 IR 翻译成 spec JSON
   │  spec.json
   ▼
KardsCardBuilder（UE 插件）    ← 在编辑器里 NewObject 出真实 UK2Node_*
   │  编译蓝图 → 只有 0 error 才落盘
   ▼
Strip → AssetRegistry 合并 → 卡目录加行 → repak → 部署
   │
   ▼
与手拼节点完全一致的 _P.pak
```

---

## 快速开始

```powershell
cd <REPO>\workflow

# 一条命令：C# → spec → K2 节点 → pak → 部署
.\BuildFromCs.ps1 -PackCs ..\cards\IntelTamperingPack.cs

# 只出 pak，不部署
.\BuildFromCs.ps1 -PackCs ..\cards\IntelTamperingPack.cs -NoDeploy
```

产出：
```
<REPO>\_build\cs\
├─ packproj\bin\Release\net8.0\UserCardPack.dll   编译好的卡包
├─ spec_from_cs.json                              中间 spec（可读、可审）
└─ ue\ue_<卡名>\KardsDIY_<卡名>_ue_P.pak          最终 pak
```

---

## 为什么能 100% 等价

关键事实：**spec JSON 的表达力 ⊇ 手拼节点**，因为 spec 的节点最终就是插件
`NewObject` 出来的真实 `UK2Node_*`。桥接器只是替你写那份 JSON。

| 能力 | 路线 A（C# 直出字节码） | spec（路线 B） | 本桥接器 |
|---|---|---|---|
| 事件图 | ✓ | ✓ | ✓ |
| 函数图覆写（ShouldGotchaTrigger） | ✓ | ✓ | ✓ |
| 读**别的对象**的属性 | ✗ | ✓ `property` | ✓ |
| **对象数组** `TArray<UBaseCardObject*>` | ✗ | ✓ `array:object:...` | ✓ |
| `KismetArrayLibrary` 调用 | ✗ | ✓ | ✓ |
| `foreach` 宏节点 | ✗ | ✓ | ✓ |
| 原样实例化任意官方 `UK2Node_*` | ✗ | ✓ `kind:"k2"` | 待接（见下） |

也就是说：**C# 那边写不出来的效果，现在通过 spec 也能写出来了。**

---

## 已实测通过的回归

`cards\IntelTamperingPack.cs`（苏联 1K「情报篡改」）端到端跑通：

```
── 2/8 UE 内拼 K2 节点并编译 ──
  OK  编译通过并落盘
── 3/8 回读校验（CDO + 图） ──
   status_ok=True parent=/Script/kards.BaseCardObject
   graph EventGraph: 15 nodes
   CDO cardSet              Basic
   CDO isInPermanentPool    True
   CDO usedTriggers         (OnOtherCardPlayedFromHand)
── 4b/8 Strip ──
   导出对象数 = 4
  OK  包 flags=0x80040200 含 PKG_FilterEditorOnly
── 6/8 注册表体检 ──
   nameCount=30475 哈希区零字节=0.38%   OK 哈希表正常
    card_event_intelligence_tampering   AssetClass='/Script/Engine.Blueprint'
    card_event_intelligence_tampering_C AssetClass='/Script/Engine.BlueprintGeneratedClass'
── 7/8 卡目录加行 + 打 pak ──
  已添加 card_event_intelligence_tampering，现在 2519 行
  Packed 4 files → KardsDIY_card_event_intelligence_tampering_ue_P.pak
```

这正好复现了手写 `spec-intel-tampering.json` 的全部行为。

---

## 写卡的 C# 姿势

继承两个接口即可，和路线 A 的写法完全一样：

```csharp
public sealed class MyPack : ICardPackDefinition, ICardBlueprintDefinition
{
    public IReadOnlyCollection<CardDefinition> DefineCards() => new[]
    {
        CardDefinition.Event(910103, "card_event_my_card", Faction.Soviet, 2, "VB")
            .Text("我的卡", "打出时对所有敌人造成 2 点伤害。")
            .FromSet(CardSet.Basic)
            .RarityOf(CardRarity.Common)
    };

    public IReadOnlyList<BlueprintFunctionIr> DefineBlueprintFunctions(CardIr card)
    {
        var cardPlayed = new BlueprintVariableDefinition(
            "cardPlayed", BaseCardType, BlueprintVariableRole.Input);

        return new[]
        {
            new BlueprintFunctionIr(
                Name: "OnOtherCardPlayedFromHand",
                Variables: new[] { cardPlayed, /* 出参用普通 Local 声明即可 */ },
                Body: new BlueprintStatement[] { /* ... */ },
                OverrideOwnerPackage: "/Script/kards",
                OverrideOwnerClass: "BaseCardObject")
        };
    }
}
```

**事件名规则**（决定落成 event 还是 function 图）：
* `On*` / `Receive*` 开头 → 事件图（`kind:"event"`）
* 其余 → 函数图（`kind:"function"`，可覆写 `BlueprintNativeEvent`）

---

## 踩过的坑（全部已修，改代码前必读）

### 1. 函数纯度决定落成表达式还是语句

插件硬规则（`CardGraphBuilder.cpp:1027 / :771`）：
* **语句**形态的 `call` 必须是**非 pure** 函数，否则报
  `"call 'X' is pure; pure calls belong in expressions"`
* **表达式**形态可以是 pure

所以不能无脑把 IR 的 call 当语句。`FunctionPurity.InferPure()` 负责分流：
pure 调用当语句用时，翻译成 `localSet 变量 = <pure call>`（取其出参 pin）。

> 实测踩中：`IsOrder` 是 pure，一开始被当语句发出去，直接报错。

### 2. IR 的出参**不一定**是 Output 角色

实测 `IntelTamperingPack` 里 `isOrder` / `cost` / `stopped` 都声明成普通
`Local`，却被放在 `Arguments` 的对应位置表示"接住出参"。

判定依据因此是"**名字出现在变量映射里且不是事件入参**"，而不是看 Role。
（`IsOutSink()`）

### 3. 出参引脚名和 IR 变量名经常不一致

```
GotchaTriggered(...)  →  出参引脚叫 qqq，IR 变量却叫 stopped
IsOrder()             →  出参引脚叫 isIt，IR 变量叫 isOrder
```

解法：`OutPinFor()` 三级解析 ——
① 查显式表 → ② 单出参函数直接用唯一出参（`SingleOutPin`）→ ③ 退回变量名。

**单出参表让 IR 里的出参变量可以随便起名**，这是最省心的一层。

### 4. 蓝图成员变量不能与父类函数重名

IR 变量 `isOrder` 会撞上 `BaseCardObject::IsOrder()`，编译报
**"找到名称冲突的成员变量"**。

解法：所有提升出来的成员变量统一加 `b_` 前缀（`SafeVarName`），
一次性规避 camelCase ↔ PascalCase 相撞。

### 5. `result` 只在函数图合法

事件图里出现 `result` 节点会报 `"'result' is only valid inside a function graph"`。
桥接器用 `_isFunctionGraph` 分流：事件图的无值 `return` 直接**不产生节点**。

### 6. spec 不支持 `seq`

插件明确报 `"sequence nodes are not implemented yet"`。
多出参函数（如 `ConvertCard`）目前需要走 `kind:"k2"` 或拆成多次赋值。

### 7. `IsOrder` / `getTotalKreditCost` **没有输入引脚**

`-Mode=Dump` 实测：
```
IsOrder             [callable,pure,native]   只有 out isIt
getTotalKreditCost  [callable,pure,native]   只有 out totalKreditCost
```
它们的 `in` 是 `self`（调用目标）。所以 **出参位置不能进 `args`**，否则报
`no input pin 'arg0' ... available: self`。

### 8. CDO 的两个必补字段（继承路线 A 的经验）

* `isInPermanentPool = true` —— 编译器不写就落回默认 `false`，卡进不了卡池
* `CardID = 0` —— 它是运行时实例 ID，不是目录 ID
* `cardSet` 必须在硬编码白名单内；**`Core` 要映射成 `Basic`**，
  否则卡掉进 `includeNotAttainable` 分支被收藏跳过

---

## 目录结构

```
card_pipeline\
├─ tools\KardsSpecBridge\
│  ├─ Program.cs                      CLI 入口
│  ├─ BridgeOptions.cs                流水线参数
│  ├─ SpecTypeMap.cs                  IR 类型 → spec 引脚词表
│  ├─ FunctionPurity.cs               函数纯度表（决定表达式/语句）
│  ├─ SpecExpressionTranslator.cs     表达式翻译
│  ├─ SpecStatementTranslator.cs      语句翻译 + foreach 识别
│  └─ SpecAssembler.cs                卡片定义 + 图 → spec
└─ workflow\
   └─ BuildFromCs.ps1                 C# → spec → UE 8 步
```

---

## 参数（可选 options.json）

省略时用内置默认（指向 `card_pipeline\_work\base\`）：

```jsonc
{
  "HostRegistry":  "E:\\KARDS\\card_pipeline\\_work\\base\\AssetRegistry.bin",
  "BaseCatalog":   "E:\\KARDS\\card_pipeline\\_work\\base\\deckCodeIDsTable2",
  "FilterPath":    "/Game/Blueprints/Cards/Mods",
  "DeckCode":      "VA",
  "CardId":        910102
}
```

未指定 `Package` / `AssetName` 时按卡名与阵营推导：
```
/Game/Blueprints/Cards/Mods/<Faction>/<units|events>/<卡名>
```

---

## 已知边界

| 项 | 状态 |
|---|---|
| 事件图 / 函数图 | ✓ 已支持 |
| pure / impure 调用分流 | ✓ 已支持 |
| 对象数组、`KismetArrayLibrary` | ✓ 已支持（路线 A 做不到） |
| `foreach` 识别 | ✓ 已支持（严格形态匹配，不匹配就硬失败） |
| 多出参函数 → 多次赋值 | ⚠ 需要 `seq`，插件未实现 → 待走 `kind:"k2"` |
| 通用 `while` 循环 | ✗ spec 无对应，请改写成遍历数组或走 `kind:"k2"` |
| `break` / `continue` | ✗ spec 无对应 |
| `kind:"k2"` 原生节点逃生舱 | ⏳ 尚未在 C# 侧暴露（见「下一步」） |

**设计原则与插件一致：不认识的一律硬失败，绝不静默降级。**
所有报错信息都带可执行的修法提示。

---

## 下一步（未完成项）

1. **`kind:"k2"` 逃生舱**：在 C# 侧暴露 `FCardNodeSpec` / `FCardLinkSpec`
   （按类名建节点 + 按引脚名连线），彻底封顶表达能力
2. **`Foreach` 的 IR 糖**：目前要手写 while 形态让识别器认出；可加一个
   `BlueprintForeachStatement` 直接产出
3. **多出参函数的 `seq` 替代**：等插件实现 `seq`，或用连续 `localSet` 拆解
4. **`-Mode=Dump` 自动回填**：把反射出的 pure/引脚名自动灌进
   `FunctionPurity` / `SingleOutPin`，免去手工维护表

---

## 相关

* [`workflow\README.md`](README.md) —— 路线 B 的 8 步详解与全部坑
* [`cards\IntelTamperingPack.cs`](../cards/IntelTamperingPack.cs) —— 示例卡包
* [`workflow\BuildUeCard.ps1`](BuildUeCard.ps1) —— 底层 UE 侧流水线
