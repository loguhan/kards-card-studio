# 用 C# 写卡牌逻辑 —— 100% 覆盖

> 目标：**只用 C# 写逻辑，产出与手拼蓝图节点 100% 等价的 pak。**

---

## 1. 为什么之前的 C# "没有卡牌逻辑"

`KardsCardCompiler.Core` 里有**两套完全独立**的接口：

| 接口 | 产出 | 表达能力 |
|---|---|---|
| `ICardPackDefinition` → `CardDefinition` | 卡面数据 | 只有 6 个硬编码效果 |
| `ICardBlueprintDefinition` | 蓝图 IR | 手搓节点树，表达力有缺口 |

`CardDefinition` 的"效果"词表**总共只有 6 个**：

```csharp
public enum EffectOperation
{
    Damage, Heal, Draw, ChangeKredits, SpawnCardInHand, GiveKeyword
}
public enum TriggerHook { EnterPlay, StartOfTurn, EndOfTurn, AfterAttack,
                          OtherCardEnterPlay, DrawnFromDeck, Deployment, Destruction }
public enum TargetKind { EnemyHq, FriendlyHq, Self, EnemyUnit, FriendlyUnit, AnyCard }
```

**"控制权转交""读别的卡的属性""条件分支"一个都表达不了。**

而 `ICardBlueprintDefinition` 虽然能做任意逻辑，但它是**手搓 IR 节点树**，
而且 IR 本身缺 4 块能力，所以也到不了 100%。

---

## 2. IR 缺什么（缺口清单）

| 缺口 | 说明 | 影响 |
|---|---|---|
| **G1 `cast`** | IR 无转型表达式 | 动态 Cast、向上转型写不出 |
| **G2 `property`** | IR 只有 `SelfProperty`，不能读**别的对象**的属性 | 读 `opp.side`、`card.CardID` 要绕 |
| **G3 `arrayMake`** | IR 无数组构造 | 组装 `TArray<int>` 写不出 |
| **G4 `foreach`** | IR 只有 `while` | 遍历靠形态识别，脆弱 |
| **G5 `kind:"k2"`** | IR 无原生节点通道 | 任何官方 `UK2Node_*` 都够不到 |

**G5 是能力天花板** —— 够不到它，就永远有卡写不出来。

---

## 3. 新增的三件东西

### 3.1 `Spec` —— 原生 spec 节点构建 API

`<REPO>\tools\KardsSpecBridge\Spec.cs`

直接构造插件能吃的 spec 节点，覆盖 G1/G2/G3/G5：

```csharp
Spec.Self()                                  // self
Spec.Param("Damage")                         // 入参引脚
Spec.Member("b_hq")                          // 成员变量
Spec.Property(target, ownerClass, "attack")  // ★ G2 读别的对象属性
Spec.Cast(value, "/Script/CoreUObject.Object")  // ★ G1 转型表达式
Spec.CastStmt(value, cls, then, else, outVar)   // ★ G1 Cast 语句
Spec.IntArray(1011, 1012)                    // ★ G3 MakeArray
Spec.ForEach(array, body, outs)              // ★ G4 原生 ForEachLoop
Spec.K2Node(id, class, function, ...)        // ★ G5 任意 UK2Node_*
Spec.K2Graph(fn, nodes, links)               // ★ G5 原生节点图
Spec.CallStmt(owner, fn, args, outs, target) // 非 pure 调用
Spec.If(cond, then, else)
Spec.Set / LocalSet / LocalDecl / Result
```

### 3.2 `Logic` —— 卡牌逻辑 DSL

`<REPO>\tools\KardsSpecBridge\Logic.cs`

写**逻辑**而不是节点：

```csharp
// 条件
Logic.SelfEquals("cardDealingDamage")   // 入参 == Self
Logic.ParamTrue("isCombatDamage")       // 布尔入参
Logic.ParamPositive("Damage")           // 入参 > 0
Logic.ParamAtLeast("Damage", 3)         // 入参 >= 3
Logic.CardProperty("card", owner, "attack")  // 读属性
Logic.All(a, b, c)  /  Logic.Any(a, b)

// 动作
Logic.FetchHq(side, "b_hq", "b_hqId")   // 取总部（两出参自动拆平）
Logic.DamageFriendlyHq("Damage")        // 友方总部吃等量伤害
Logic.GiveSelfToOpponent()              // 控制权转交对方
Logic.DamageParam("card", 3)            // 对入参卡造成伤害
Logic.DrawCards(2) / Logic.GiveKredits(1) / Logic.BattleLog()

// 触发点常量（全部实测校准）
Logic.Trigger.OnOtherCardDealDamage
Logic.Trigger.OnOtherCardPlayedFromHand
Logic.Trigger.OnStartofTurn / OnEndOfTurn
... 共 50+ 个
```

### 3.3 `ISpecBlueprintDefinition` —— 原生 spec 卡包接口

```csharp
public interface ISpecBlueprintDefinition
{
    SpecGraphSet DefineSpecGraphs(CardIr card);
}

public sealed class SpecGraphSet
{
    public IReadOnlyList<(string Name, string PinType)> Variables { get; init; }
    public IReadOnlyList<JsonObject> Graphs { get; init; }
    public IReadOnlyList<(string, string, object?)> ExtraDefaults { get; init; }
}
```

**两条路线可以混用**：同一个卡包实现 `ICardPackDefinition` 定卡面、
`ICardBlueprintDefinition` 写能用 IR 的部分、
`ISpecBlueprintDefinition` 写 IR 写不出的部分。

---

## 4. 完整示例：苏联 1011「闪击炮兵」

> 闪击 · 炮兵 · 攻击距离 2
> 攻击造成伤害时，友方总部也受同样伤害，然后控制权转交对方。

```csharp
public sealed class BlitzArtillery1011Pack : ICardPackDefinition, ISpecBlueprintDefinition
{
    public IReadOnlyCollection<CardDefinition> DefineCards() => new[]
    {
        CardDefinition.Unit(id: 1011, name: "card_unit_blitz_artillery_1011",
                faction: Faction.Soviet, cost: 4, attack: 3, defense: 3,
                range: 2,                          // ★ 攻击距离 2
                unitType: CardUnitType.Artillery)  // ★ 炮兵
            .Text("闪击炮兵", "闪击。攻击造成伤害时，友方总部也受到同样伤害，然后控制权转交对方。", "…")
            .FromSet(CardSet.Basic).RarityOf(CardRarity.Common)
    };

    public SpecGraphSet DefineSpecGraphs(CardIr card) => new()
    {
        Variables = new[]
        {
            ("b_side", "enum:/Script/kards.ESideEnum"),
            ("b_hq",   "object:/Script/kards.BaseCardObject"),
            ("b_hqId", "int"),
        },
        Graphs = new[]
        {
            Logic.EventGraph(Logic.Trigger.OnOtherCardDealDamage,
                Logic.EventBody(
                    Spec.Set("b_side", Spec.SelfSide()),
                    Spec.If(
                        Logic.All(
                            Logic.SelfEquals("cardDealingDamage"),  // 只认本卡造成的伤害
                            Logic.ParamTrue("isCombatDamage"),      // 只认攻击
                            Logic.ParamPositive("Damage")),         // 0 伤害不触发
                        Logic.Build(
                            Logic.FetchHq(Spec.Member("b_side"), "b_hq", "b_hqId"),
                            Logic.DamageFriendlyHq("Damage"),       // 友方总部吃等量伤害
                            Logic.GiveSelfToOpponent(),             // 转交控制权
                            Logic.BattleLog()))),
                owner: "/Script/kards.BaseCardObject"),
        },
    };
}
```

**编译结果**：`status_ok=True`，`EventGraph: 25 nodes`，
`Type=artillery  range=2  attack=3  defense=3  kredits=4`，
`usedTriggers=(OnOtherCardDealDamage)`。

---

## 5. 已验证的能力矩阵

| 能力 | 验证卡 | 结果 |
|---|---|---|
| IR 路线（回归） | `IntelTamperingPack.cs` | ✅ 15 nodes，与基线一致 |
| 原生 spec 事件图 | `BlitzArtillery1011Pack.cs` | ✅ 25 nodes |
| **G2 `property`** 读别的对象属性 | `DslProofPack.cs` | ✅ spec 含 1 个 `property` |
| **G4 `foreach`** 遍历对象数组 | `DslProofPack.cs` | ✅ spec 含 1 个 `foreach` |
| **G3 `arrayMake`** int 数组 | `CastProofPack.cs` | ✅ |
| **G1 `cast`** 表达式 + 语句 | `CastProofPack.cs` | ✅ |
| 对象数组引脚 `array:object:...` | `DslProofPack.cs` | ✅ |

---

## 6. 必须知道的坑（全部实测踩过）

### 6.1 字段名在表达式/语句形态下不一致

| 用法 | 字段 |
|---|---|
| `cast` **表达式** | `class` |
| `cast` **语句** | `name` ← 不同！ |

对象相等比较的**两个形参是基类 `Object`**，插件**不做隐式 Cast**：

```
Error: incompatible pins object:/Script/kards.BaseCardObject
       -> object:/Script/CoreUObject.Object
```

→ 用 `Spec.AsObject(...)` 显式向上转型（`Spec.ObjectEquals` 已内置）。
（桥接器也对 `EqualEqual_ObjectObject` / `NotEqual_ObjectObject` 做了自动转型。）

### 6.2 `seq` 节点未实现

`CardGraphBuilder.cpp:926` → `"sequence nodes are not implemented yet"`。

所以**多出参函数**（`GetLocationCardBySide`、`DamageCard`…）不能塞进一个 `seq`。
桥接器改为把多条 `localSet` **平铺成兄弟语句**。

### 6.3 k2 图的两处陷阱

* `links` 是**四元数组** `[fromId, fromPin, toId, toPin]`，**不是对象**
* 节点字段是 `target`（不是 `targetClass`）

### 6.4 `ETypeEnum` 没有 `unit`

```
NotAvailable=0 location=1 order=2 tank=3 fighter=4 bomber=5 infantry=6
artillery=7 antiair=8 antitank=9 tankdestroyer=10 gotcha=11 wildcard=12
```

单位卡**必须落到具体兵种**。`CardUnitType` 与它逐项同名同序，
所以桥接器直接取小写枚举名。

### 6.5 `ERegisteredCardFunction` 没有 `OnPlayedFromHand`

只有 `OnOtherCardPlayedFromHand`（**别的**卡被打出）。
本卡自己被使用时用 `OnDeploymentEffectTriggered` / `OnOtherCardEnterPlay`。

### 6.6 变量名会撞父类成员

`isOrder` 会撞 `BaseCardObject::IsOrder` → `找到名称冲突的成员变量`。
原生 spec 变量名请自带 `b_` 前缀。

### 6.7 未知函数的报错很友好

```
Error: class CardFunctionsStub has no function 'GetOppositeSide'
```

**照抄报错 → dump 一下 → 改名 → 重跑**，闭环很快。

---

## 7. 标准工作流

```powershell
cd <REPO>\workflow

# ① 先 dump 你要用的 API（关键步骤，别猜）
.\BuildUeCard.ps1 -DumpApi /Script/kards.CardFunctionsStub -WorkRoot ..\_build\dump
.\BuildUeCard.ps1 -DumpApi /Script/kards.BaseCardObject    -WorkRoot ..\_build\dumpbase
.\BuildUeCard.ps1 -DumpEnum /Script/kards.ERegisteredCardFunction -WorkRoot ..\_build\dump

# ② 写 C#（卡面 + ISpecBlueprintDefinition）

# ③ 构建（含 plugin 编译→拼节点→编译→Strip→注册表→pak）
.\BuildFromCs.ps1 -PackCs ..\cards\MyCard.cs

# ④ 只出 pak 不部署
.\BuildFromCs.ps1 -PackCs ..\cards\MyCard.cs -NoDeploy
```

产物：`_build\<name>\ue\ue_<卡名>\KardsDIY_<卡名>_ue_P.pak`

---

## 8. 覆盖度结论

| 场景 | 能否用 C# 写 | 怎么做 |
|---|---|---|
| 简单伤害/治疗/抽牌 | ✅ | `CardDefinition` 的 6 个效果 |
| 条件分支、复合效果 | ✅ | `Logic` + `Spec.If` |
| 读别的卡的属性 | ✅ | `Spec.Property` / `Logic.CardProperty` |
| 类型转换 | ✅ | `Spec.Cast` / `Spec.AsObject` |
| 遍历卡组/前线 | ✅ | `Spec.ForEach` + `GetCardsInFrontlineBySide` |
| 数组构造 | ✅ | `Spec.IntArray` / `Spec.ArrayMake` |
| 控制权转交 | ✅ | `Logic.GiveSelfToOpponent()` |
| 任意官方节点 | ✅ | `Spec.K2Node` / `Spec.K2Graph`（G5 逃生舱） |

**结论：C# 侧表达能力 = 插件上限 = 手拼节点 100% 等价。**
唯一的上限是插件自身（`seq` 未实现），而这个上限已被
"平铺兄弟语句" + "k2 逃生舱"绕开。
