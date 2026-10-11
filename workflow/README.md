# KARDS 加卡工作流

从**卡定义**到**能进游戏收藏/对局的 `_P.pak`**，全自动。

```
写卡定义 (C#)  →  卡资产  →  CDO 修正  →  注册表合并  →  客户端卡目录  →  pak  →  部署
```

---

## 快速开始

```powershell
cd (仓库)\workflow

.\KardsCardWorkflow.ps1 check       # 环境自检
.\KardsCardWorkflow.ps1 prepare     # 首次：提取原版基底 + 装编辑器插件
.\KardsCardWorkflow.ps1 all         # build + deploy + verify
```

| 命令 | 作用 |
|---|---|
| `check` | 检查所有工具/路径/插件/游戏状态 |
| `prepare` | 从游戏主 pak 提取**原版** `AssetRegistry.bin` 和卡目录表；把 `AssetRegistryTool` 装进 UE 工程 |
| `build` | 全流程构建（8 步），产出 pak 到 `_work\out\` |
| `deploy` | 自动停游戏 → 移走旧 pak → 拷入新 pak |
| `verify` | 离线校验（pak 内容 / 哈希表 / AssetClass / cardSet / CDO / 卡目录） |
| `all` | `build` + `deploy` + `verify` |
| `rollback` | 把 `KardsDIY_*.pak` 移出游戏目录 |
| `status` | 当前 pak 与构建产物状态 |

配置全在 [workflow.json](workflow.json)。

---

## 8 步构建流程

| # | 步骤 | 工具 | 关键点 |
|---|---|---|---|
| 1 | 编译卡定义 | `dotnet build` | `ICardPackDefinition` + `ICardBlueprintDefinition` |
| 2 | 生成卡资产 | `kards-cardc build` | 产出 `.uasset/.uexp` + donor 注册表 |
| 3 | **修 CDO** | `KardsTable setprop` | 补 `isInPermanentPool=true`、`CardID=0` |
| 4 | 第一次合并 | `AssetRegistryTool Merge` | host=原版，donor=编译产物，**必须带 `-FilterPaths`** |
| 5 | 导出 JSON | `AssetRegistryTool List` | 为了下一步补字段 |
| 6 | **补 AssetClass** | `fix_registry_class.py` | 空 AssetClass → 卡进不了卡池 |
| 7 | JSON → bin | `AssetRegistryTool convert` | 用 **UE 自己的序列化器**，哈希表才对 |
| 8 | 最终合并 + 卡目录 + 打包 | `AssetRegistryTool Merge` / `KardsTable add` / `repak pack` | 卡目录加一行；pak 用 `_P.pak` + mount `../../../` |

---

## 五个必须知道的坑

这些都是实测撞出来的，改流程前务必先读。

### 坑 1 · 绝不能用 `UAssetRegistry` 的回写能力产出注册表

`KardsCardCompiler` 依赖的那套 `UAssetRegistry` 是从 **CUE4Parse（只读解析器）** 移植的，回写时把 name 哈希表**整个写成 0**：

```csharp
// FAssetRegistryWriter.cs:153-154
_w.Write((ulong)0);                                // hashVersion（读取时忽略）
for (var i = 0; i < num; i++) _w.Write((ulong)0);  // hashes（读取时忽略）
```

实测对比（header 后 24..260000 字节）：

| 文件 | 哈希区零字节 | 最长连续 0 |
|---|---|---|
| 游戏原版 | 3.3% | 4 |
| 卡编译器重序列化 | **96.8%** | **243,618** |
| CCB-TEAM 重序列化 | **96.8%** | **243,591** |

游戏靠这张表查资产名，全零 → **整张表作废 → 所有卡牌消失（包括原有的）**。

**正解**：用 **`AssetRegistryTool`**（UE 5.6 编辑器插件）在编辑器进程里合并，走 UE 自己的 `FAssetRegistryState` 序列化器。实测输出哈希区零字节 **0.38%**，与原版一致。

### 坑 2 · 空的 `AssetClass` 会让卡进不了卡池

编译器 writer 不序列化 `FAssetData::AssetClass`，合并后新卡条目该字段为空（连它自己读出的 `card_unit_t_60` 也是空的）。

游戏侧的加载路径（`FunctionLibrary.cpp` + UE 5.6 `AssetData.h`）：

```cpp
TArray<FName> UFunctionLibrary::GetStaticCardNames() {
    AssetRegistryModule.Get().GetAllAssets(Assets, true);
    for (const FAssetData& AssetData : Assets) {
        if (!AssetData.AssetName.ToString().StartsWith(TEXT("card_"))) continue;
        if (UBaseCardObject* Card = LoadCardFromAssetData(AssetData)) { ... }   // ← 卡池来源
    }
}

UObject* FastGetAsset(...) {                       // UE 5.6 AssetData.h
    UPackage* FoundPackage = LoadPackage(nullptr, *PackageName.ToString(), ...);
    return FindObjectFast<UObject>(FoundPackage, AssetName);
}
```

`AssetClass` 为空 → `GetAsset()` 解析失败 → 返回 `nullptr` → 卡不在卡池。

**修法**：`List` 导出 → 把新卡两条条目的 `AssetClass` 改成
`/Script/Engine.Blueprint` 与 `/Script/Engine.BlueprintGeneratedClass`（照抄真卡）→ `convert` 回 bin。

### 坑 3 · 收藏只列"硬编码白名单"里的卡包

反编译 `BP_CardFunctions_GetAllActiveStaticCards`（3892 VM 字节）得到的真实逻辑：

```cpp
BasicSetOnly = PlatformUtilityFunctions.IsChinaISBN(self)

if (BasicSetOnly) {                       // China ISBN 构建
    if (cardSet != 1) SKIP;               //   只留 Basic
} else {
    for L in [1,2,3,4,5,6,7,8,9,10,12,13,15,16,17,18,19,20,21]:
        if (cardSet != L) → 试下一个
        isCardSetConfigActive(L, out isActive)
        if (!isActive) SKIP
        → NotifyCheckCardBlacklisted / NotifyCheckCardReserved → ADD

    // 所有字面量都不匹配：
    if (!includeNotAttainable) SKIP; else ADD    // ← 未知卡包靠这个参数兜底
}
```

**白名单（`ECardSetEnum` 数值）：**

| 值 | 名称 | 值 | 名称 |
|---|---|---|---|
| 1 | Basic | 12 | Legions |
| 2 | Special | 13 | WorldAtWar |
| 3 | OnlySpawnable | 15 | BrothersInArms |
| 4 | Candidate | 16 | WinterWar |
| 5 | Placeholder | 17 | CovertOps |
| 6 | Expansion1 | 18 | BloodAndIron |
| 7 | Expansion2 | 19 | NavalWarfare |
| 8 | Expansion2019 | 20 | Homefront |
| 9 | Campaign2020 | 21 | OceaniaStorm |
| 10 | Breakthrough | | |

**不在白名单**：`0 NotAvailable`、**`11 Wildcards`**、**`14 Core`**、`22 Reserved`。

`Core` 是 `Basic` 的别名（服务端 `expansionCards.json` 里两者都是 353 张），**真卡的 CDO 里存的是 `Basic`**。
用 `Core` 会让卡掉进 `includeNotAttainable` 分支，而收藏传的是 `false` → **被当成"需要特殊途径获得的卡"跳过**。

### 坑 4 · CDO 只序列化非默认值，缺的属性必须**新增**

`isInPermanentPool` 编译器不写 → 落回 C++ 默认 `false`。字段语义是
**"Always in the card pool regardless of set"**，真卡显式写入 `true`。

`KardsTable setprop` 因此支持两种操作：**改已有** + **新增缺失**。

同理 `CardID`：它是**运行时实例 ID**（`0` by default），不是目录 ID。编译器会把 `CardDefinition.Id`（如 `910101`）写进去，真卡那里是 `0`。

### 坑 5 · `Merge` 必须带 `-FilterPaths`

不带的话会全量合并 donor 的 16,314 条（都带着那个坏 writer 的数据），
直接崩在 `UnrealEditor-AssetRegistryTool.dll`（exit=3，无输出文件）。

```
-FilterPaths=/Game/Blueprints/Cards/Mods
```

---

## 已知限制（写复杂效果卡时必读）

元数据（名称/标题/文本/花费/阵营/卡包/稀有度/deck-code）**完全可写**；
但**效果逻辑的表达能力有硬边界**，因为编译器的蓝图 IR 只覆盖了一部分节点。

### 限制 1 · 没有对象数组类型

`BlueprintValueKind` 只有 `Int32Array`；`BlueprintTypeRef` 只有 `Int32Array` 静态项，
`HeaderContractImporter.ResolveType`（:118-127）只把 `TArray<int32>` 映射到 `Int32Array`，
其余 `TArray<...>` 一律返回 `null`（不支持）。

**后果**：所有"出参是 `TArray<UBaseCardObject*>`"的卡函数都调不了 ——
`GetAllActiveStaticCards`、`GetRandomCard`、`GetCardsInHandBySide`、`GetAllCardsOnBoard` …
也就是说**运行时无法枚举卡池**。

### 限制 2 · 编译器不生成任何 `KismetArrayLibrary` 调用

搜遍 `KardsCardCompiler/src` 对 `Array_Length` / `Array_Get` / `Array_Add` / `Array_Clear`
**零匹配**。所以即使有 `Int32Array` 变量，也**没法取长度、取元素、追加**。

**后果**：`ConvertCard(CardIDs, ...)` 这类需要"组装一个 int 数组"的调用做不出来。

### 限制 3 · 读不了别的对象的属性

`BlueprintMemberExpression` 编译成 `EX_InstanceVariable`（`BlueprintIrCompiler.cs:516-521`），
只读**当前类自己的**变量，不能读参数对象的字段。

**变通**：`BlueprintCallStatement` 有 `Target` 字段，可以**在别的对象上调用函数**。
所以读属性要绕道 getter —— 而且要注意 dispatch：

| 情况 | Dispatch |
|---|---|
| `cardFunction` 上的卡函数（契约里 `Event: true`） | `ObjectVirtual` |
| `UBaseCardObject` 上的普通函数（`IsOrder` / `getTotalKreditCost` …） | **`ObjectFinal`** |

用错会报 `Native function 'X' uses incompatible event/native dispatch`
（`BlueprintNativeValidator.cs:88-91`）。

### 补齐这三点的位置

| 要加的东西 | 文件 |
|---|---|
| `BlueprintValueKind.ObjectArray` + `BlueprintTypeRef.ArrayOf(...)` | `KardsCardCompiler.Core/BlueprintIr.cs` |
| `TArray<UBaseCardObject*>` 的类型映射 | `KardsCardCompiler.Kards/HeaderContractImporter.cs:118` |
| `KismetArrayLibrary` 的调用支持（4 个函数） | `KardsCardCompiler.UAsset/BlueprintIrCompiler.cs` |
| `Int32Array` 的 `FArrayProperty` 生成 | `BlueprintIrCompiler.cs:185` |

补齐后，「情报篡改」的完整效果就能落地（花费表已经实测好，见
[IntelTamperingPack.cs](../cards/IntelTamperingPack.cs) 头部注释）。

---

## 服务端那层（按需）

客户端能让卡显示并能用，**不等于服务端认识它**。服务端两层：

| 层 | 文件 | 作用 |
|---|---|---|
| 拥有关系 | `library/deckCodeIDsTable2.json` → `PlayerLibraryService.InitLibrary` | `/players/{id}/library` 返回玩家拥有的卡 |
| 卡池目录 | `library/expansionCards.json` | 按卡包分组的卡名清单（17 个包 / 2486 张） |

编译器有 `adapt-fyserver` 子命令可以把卡并进服务端表。
注意服务端 `deckCodeIDsTable2.json` 是**客户端同名表的镜像**，两处都要加。

---

## 游戏内验证

`Mods\KardsCardProbe\Scripts\main.lua` 会在开机 100 秒后、之后每 45 秒写一次
`(仓库)\_diag\ingame_probe.json`，只用 `ForEachUObject` 单遍扫描（安全）：

| 字段 | 含义 |
|---|---|
| `card_objects` / `distinct_card_classes` | 游戏内存里的卡对象/卡类数（正常 ≈ 4040 / 2020） |
| `TARGET_class_loaded` | 我的卡的类有没有被加载 |
| `TARGET_sample` | 找到时的类名 / 外层类 / 对象名 |

**警告**：不要在游戏里调用 `GetStaticCardNames()` / `GetAllActiveStaticCards()` /
任何 `AssetRegistry.GetAllAssets` / `GetAssetsByPath` —— 这个构建下会把游戏主线程堵 60 秒以上，
游戏直接卡死退出（桥接源码 `main.lua:1862` 有实测记录）。探测请走 `ForEachUObject`。

---

## 目录结构

```
card_pipeline\workflow\
├─ workflow.json              配置（所有路径）
├─ KardsCardWorkflow.ps1      主脚本
├─ README.md                  本文
└─ (运行期) ..\_work\
   ├─ base\                    从游戏 pak 提取的原版基底
   ├─ cardc\                   kards-cardc 产物
   ├─ cdo\                     修好的卡资产
   ├─ catalog\                 加好行的客户端卡目录
   ├─ diag\                    中间注册表 + 导出 JSON
   ├─ stage\                   打 pak 的目录树
   ├─ out\                     最终 pak
   └─ verify\                  校验用的临时提取物
```

---

## 依赖

| 组件 | 位置 | 用途 |
|---|---|---|
| UE 5.6 | `(UE)` | 编辑器 + 序列化器 |
| UE 工程 | `(UE 工程)\kards.uproject` | 承载 AssetRegistryTool |
| **AssetRegistryTool** | `<KARDS>\_kards_writecard\_analysis\AssetRegistryTool-main\` | **注册表合并（正解）** |
| KardsCardCompiler | `<KCC>` | 写卡编译器 |
| KardsTable | `(仓库)\tools\KardsTable\` | 编辑 cooked DataTable / CDO |
| fix_registry_class.py | `(仓库)\tools\` | 补 AssetClass |
| repak | `<REPAK>` | pak 打包 |
| usmap | `C:\Dumper-7\...\Mappings\` | 属性名映射（`AssetRegistryTool` 路径写死） |

---

## 排查

| 症状 | 原因 |
|---|---|
| **所有卡牌消失（含原有的）** | pak 里的 `AssetRegistry.bin` 哈希表被清零 → 别用 `UAssetRegistry` 回写 |
| 卡在对局里能用、收藏不显示 | `cardSet` 不在白名单（坑 3） |
| 卡连对局都用不了 | `AssetClass` 为空（坑 2）或卡池过滤掉了（坑 4） |
| `Merge` exit=3 无输出 | 漏了 `-FilterPaths`（坑 5） |
| 编辑器启动被弹窗打断 | `FullMatchRuntimeProbe` 插件模块缺失，在 `.uproject` 里禁掉 |
| 换 pak 报"文件被占用" | 游戏在跑，`deploy` 会自动停 |

**一键回滚**：`.\KardsCardWorkflow.ps1 rollback`

> 测试 pak 停用时必须**整个移出游戏安装目录**，放进 `Paks` 子目录或游戏内备份目录**仍会被加载**。

---

# 路线 B：UE 侧写蓝图（BuildUeCard.ps1）

路线 A（`KardsCardWorkflow.ps1`）是「C# 编译器拼字节码」。路线 B 是
「**UE 编辑器自己 NewObject 出 K2 节点**」，用于编译器拼不出来的图
（对象数组、循环、跨对象取属性等）。

```
.\BuildUeCard.ps1 -NoPluginBuild -Force      # 全流程
.\BuildUeCard.ps1 -DumpApi /Script/kards.BaseCardObject   # 只反射真实签名
.\BuildUeCard.ps1 -DumpEnum /Script/kards.ECardSetEnum    # 只反射枚举
```

## 8 步

| # | 动作 | 实现 |
|---|---|---|
| 1 | 编译 `KardsCardBuilder` 插件 | `Build.bat kardsEditor Win64 Development` |
| 2 | 拼 K2 节点 + 编译蓝图 | `-run=KardsCardBuilder -Mode=Build -Spec=<json>` |
| 3 | 回读校验 CDO / 图 | `-Mode=Verify -Package=/Game/...` |
| 4 | 扫 donor 注册表 | `-Mode=ScanRegistry -FilterPath=/Game/Blueprints/Cards/Mods` |
| 5 | 合并进 host 注册表 | `AssetRegistryTool Merge`（**必须带 `-FilterPaths`**） |
| 6 | 注册表体检 | nameCount / 哈希区零字节 / AssetClass |
| 7 | 卡目录加行 + repak | `KardsTable add` + `repak pack --mount-point ../../../` |
| 8 | 部署 | 拷进 `Paks\` |

## KardsCardBuilder 命令

| Mode | 参数 | 说明 |
|---|---|---|
| `Dump` | `-Class=/Script/<mod>.<Class>` `[-Filter=子串]` | 反射 UFunction 签名 + UPROPERTY + 引脚类型 |
| `DumpEnum` | `-Enum=/Script/<mod>.<Enum>` | 枚举全部值 |
| `DumpModule` | `-Module=<name>` | 列出模块所有 UClass |
| `Build` | `-Spec=<abs json>` `[-Force]` | 拼图 → 编译 → **只有 0 error 才落盘** |
| `Verify` | `-Package=/Game/...` | 载回资产，导出 CDO 与节点清单 |
| `ScanRegistry` | `-FilterPath=` `-Out=<abs bin>` | 用 `FAssetRegistryState::Save` 写 donor |

`-Force` 会先删掉磁盘上已有的 `.uasset/.uexp`；`SavePackage` 不会静默覆盖已有资产。

## spec 语法

节点是**声明式语句树**，每个语句/表达式都落成真实的 `UK2Node_*`：

```jsonc
{
  "package": "/Game/Blueprints/Cards/Mods/Soviet/units/<name>",
  "parentClass": "/Script/kards.BaseCardObject",
  "variables": [ { "name": "tmp_cost", "type": "int" } ],   // 落成 Blueprint 成员变量
  "defaults":  [ { "name": "Type", "type": "enum", "value": "gotcha" },
                 { "name": "usedTriggers", "enum": "ERegisteredCardFunction",
                   "values": ["OnOtherCardPlayedFromHand"] } ],
  "graphs": [ { "kind": "event", "function": "OnOtherCardPlayedFromHand", "body": [ ... ] } ]
}
```

### 语句 `kind`

| kind | 落地节点 |
|---|---|
| `call` | `UK2Node_CallFunction`（**必须是非 pure 函数**）+ 出参用 `outs`/`result` 接住 |
| `if` | `UK2Node_IfThenElse`（`then` / `else`） |
| `set` / `localSet` | `UK2Node_VariableSet` |
| `cast` | `UK2Node_DynamicCast`（impure） |
| `seq` | 未实现（会明确报错，不静默降级） |

### 表达式 `kind`

| kind | 落地节点 |
|---|---|
| `self` | `UK2Node_Self`（schema 会把 `PSC_Self` 解析成父类） |
| `param` | 事件入参引脚 |
| `member` / `local` | `UK2Node_VariableGet`（self 成员） |
| `property` | `UK2Node_VariableGet` + `SetExternalMember` —— **读别的对象的属性** |
| `call` | `UK2Node_CallFunction`（**必须 pure**），`pin` 指定取哪个出参 |
| `literal` | 写引脚默认值（`bool/int/int64/byte/float/string/name/text/object:/class:/enum:`） |
| `cast` | `UK2Node_DynamicCast`（pure） |
| `arrayMake` | `UK2Node_MakeArray`，`type` 是元素类型 |

引脚类型词表：`bool byte int int64 float double string name text`
`enum:/Script/..  struct:/Script/..  object:/Script/..  class:/Script/..  interface:/Script/..`
数组/集合加前缀：`array:` `set:`。

### 编译器做不到、路线 B 能做的

| 能力 | 编译器 IR | 路线 B |
|---|---|---|
| 读别的对象的属性 | ✗ | ✓ `property` |
| 对象数组 `TArray<UBaseCardObject*>` | ✗ | ✓ `arrayMake` + `array:` 类型 |
| 调 `KismetArrayLibrary` | ✗ | ✓ 当普通静态函数调 |
| 局部变量 | ✓（函数图） | 事件图无局部变量 → 用成员变量代替 |

## 已知限制

* 只支持**事件图**（`kind:"event"`，即 `BlueprintImplementableEvent` 覆写）。
  函数图 / `BlueprintNativeEvent` 覆写（如 `ShouldGotchaTrigger`）暂不支持。
* 事件图没有真正的局部变量（`UK2Node_Event` 不带 `LocalVariables`），
  spec 里的 `local*` 一律落成 Blueprint 成员变量。
* 不插转换节点：类型不一致时直接报错并列出双方引脚类型，不会偷偷加 cast。
* 不支持的 `kind` 一律硬失败，不静默降级。

## 坑（血泪）：必须 Strip 掉编辑器对象

**症状**：进游戏 Fatal error，100% 稳定复现。

**原因**：`UPackage::SavePackage` 默认把整个 `UBlueprint` 当顶层资产写进去，
包里就带上了 `UBlueprint` / `UEdGraph` / 一大堆 `UK2Node_*`。
这些类属于**编辑器专用模块 `BlueprintGraph`**，Shipping 包里根本不存在，
游戏链接 import 时直接炸。

对比（导出对象数）：

| 资产 | 导出数 | 内容 |
|---|---|---|
| 游戏本体 `card_event_comrades` | 9 | 只有 `_C` 类 + CDO + 函数 |
| 能用的 mod 卡 `card_unit_sov_defector_artillery` | 3 | 同上 |
| **不 Strip 的编辑器产物** | **41** | 多了 `UBlueprint` / `EventGraph` / 全部 K2Node |

**修法**：`-Mode=Strip`（`BuildUeCard.ps1` 第 4b 步，在 `ScanRegistry` 之后、`Merge` 之前）

```
GeneratedClass->ClassGeneratedBy = nullptr;   // 唯一的反向引用，且是 editor-only
Blueprint->GeneratedClass = nullptr;
UPackage::SavePackage(Package, GeneratedClass, Filename, Args /* TopLevelFlags = RF_NoFlags */);
```

`RF_NoFlags` = 只保留从 `InAsset` 可达的对象；`RF_Public|RF_Standalone`
会把同样带这两个 flag 的 `UBlueprint` 强行留下。

**顺序不能反**：注册表条目是编辑器按「Blueprint 资产」登记的，
Strip 之后 `ScanRegistry` 什么都扫不到（`donor assets = 0`）。
所以必须 Build → ScanRegistry → Strip → Merge → Pack。

Strip 后 `ScanRegistry` 还要**手工补一条 `<name>_C` 条目**
（`AssetClass = BlueprintGeneratedClass`），因为 cooked 卡包在注册表里是两条：
`card_X`（Blueprint）+ `card_X_C`（BlueprintGeneratedClass）。
标签从 Blueprint 那条复制，别自己编。

流水线里已加断言：导出对象数 > 12 直接 fail，不让带编辑器对象的包出门。

## 坑（致命）：包必须带 PKG_FilterEditorOnly

**这才是 100% Fatal error 的真凶。**

Shipping 引擎的硬门槛（`LinkerLoad.cpp` 约 1615 行）：

```cpp
// don't load packages that contain editor only data in builds that don't support that and vise versa
if (!FPlatformProperties::HasEditorOnlyData() && !(Summary.GetPackageFlags() & PKG_FilterEditorOnly))
{
    // "Unable to load package. Package contains EditorOnly data which is not supported by the current build."
    return LINKER_Failed;
}
```

**`PKG_FilterEditorOnly` 是 `0x80000000`**（`ObjectMacros.h`），
不是 `0x00040000`（那是 `PKG_RequiresLocalizationGather`）。这个常量搞错过一次，
白追了一轮。

对照实测：

| 资产 | PackageFlags | 解码 |
|---|---|---|
| 游戏本体卡 | `0x80042200` | FilterEditorOnly + RequiresLocGather + **UnversionedProperties** + Cooked |
| 能用的 mod 卡 | `0x80002200` | FilterEditorOnly + **UnversionedProperties** + Cooked |
| 不设 flag 的编辑器产物 | `0x00040000` | 只有 RequiresLocGather → **必然 LINKER_Failed** |

**修法**：存盘前把包自己的 flag 设上：

```cpp
Package->SetPackageFlags(PKG_FilterEditorOnly | PKG_Cooked);
```

为什么这样就自洽：`FSaveContext::IsFilterEditorOnly()` 就是
`Package->HasAnyPackageFlags(PKG_FilterEditorOnly)`。
所以设上它，**写**的时候会跳过 `CPF_EditorOnly` 属性，
**读**的时候也会跳过 —— 两边一致，不会流错位。
（`SavePackage2.cpp` 最后也是用 `Linker->IsFilterEditorOnly()` 去置 summary 里的这个 flag。）

**不要设 `PKG_UnversionedProperties`**：那会改成「无属性名、按二进制顺序」的序列化格式，
而编辑器写出来的是带 tag 的版本化属性，硬设会让读方按错格式解析 → 花屏/崩溃。
我们的包 `v4=522 v5=1017 vlic=0`，和游戏同引擎版本，版本化路径没问题。

流水线已加断言：Strip 后直接读 `.uasset` 头部，`flags & 0x80000000` 为 0 就 fail。

## pak 形态说明（踩过的坑）

* UE 编辑器 `UPackage::SavePackage` 出来的是**单文件 `.uasset`**
  （`TotalHeaderSize < 文件大小`，导出数据内联，`SerialObjectOffset >= TotalHeaderSize`）。
  游戏本体的 cooked 资产是**分体**（`.uasset` + `.uexp`，`TotalHeaderSize == .uasset 大小`）。
  两种 UE 的 `FLinkerLoad` 都认：`FAsyncArchive` 只在
  `TotalSize() == HeaderSize` 时才切到 `.uexp`。
* 编辑器写出的包带 `PKG_FilterEditorOnly`，游戏本体 cooked 包也带这个 flag，
  而 `zz_ChinaExperiment_2_P.pak` 里的资产同样带 —— 这是已验证能进的形态。
* `Merge` 出来的 `AssetRegistry.bin` 哈希区零字节 0.38%（游戏本体同量级）；
  `UAssetRegistry`（C#）会写成 100% → **整个卡库消失**。

