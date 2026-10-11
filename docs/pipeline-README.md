# KARDS 蓝图写卡流水线

**写卡（UE 蓝图）→ UE4SS 注入 → 交给玩家测试 ∥ 并行烘焙打包**

```
[1] author   蓝图写卡          UE 编辑器 + KardsBlueprintRestorer / 抄 DIY 样例
      │                       产出: Content/Blueprints/Cards/<国家>/DIY/<卡名>.uasset
      ▼
[2] inject   UE4SS 注入        快烘焙改动的卡 → repak 打 *_P.pak → 丢进游戏 Paks\
      │                       产出: KardsDIY_<slug>_P.pak（秒~分钟）
      ▼
[3] handoff  交给玩家          测试包 + 测试清单 + 反馈模板
      │
      └──────► 玩家在玩的时候，你同时跑 ──────┐
                                              ▼
[4] cook     烘焙 + 正式打包    BuildCookRun -cook -pak -stage（后台，分钟~小时）
                              产出: 可发布的正式包
```

阶段 2 和阶段 4 **刻意解耦**：注入包只烘焙你改动的那几张卡，几分钟就能给玩家；
正式包走全量 BuildCookRun，慢但完整，放后台跑，两边互不干扰。

---

## 0. 先回答：有没有样例？

**有，而且是 43 张已经写好的真卡。** 在
`KardsBlueprintRestorer (1).rar` 里：

```
KardsBlueprintRestorer\Content\Blueprints\Cards\
├─ BP_CardFunctions.uasset   22.9 MB   ← 卡效果动作库（190 个动词都在这里）
├─ BP_HandCard.uasset        25 KB     ← 手牌显示
├─ Germany\DIY\events\card_event_go_all_out.uasset      110 KB  ★
├─ Germany\DIY\events\card_event_u2336.uasset           129 KB  ★
├─ Germany\DIY\units\card_unit_80th_infantry_regiment.uasset 50 KB ★
├─ Soviet\DIY\card_unit_sov_tu160m_white_swan.uasset     98 KB  ★
├─ USA\DIY\events\card_event_yzlb.uasset                256 KB  ★ 最复杂
├─ USA\DIY\events\card_event_kf1.uasset                   7 KB  ★ 最简单，从这张开始抄
└─ … 共 38 张 DIY + 5 张官方卡参照
```

**38 张 DIY 卡** 分布在 Germany / Britain / Soviet / USA / Japan，是现成的写法参考。

另外还有一份 **JSON 格式的真实卡**（FModel 导出）：

```
Germany\events\card_event_sabotage.json    23 KB
```

它把一张卡的完整数据摊开了 —— 见下面第 4 节。

一键看全部样例：

```powershell
pwsh -File pipeline.ps1 samples
```

---

## 1. 命令速查

```powershell
cd (仓库)

# 环境体检（引擎/工程/游戏/UE4SS/repak 全部路径 + 游戏内已有 pak + 已加载 mod）
pwsh -File pipeline.ps1 status

# 看 43 张样例卡
pwsh -File pipeline.ps1 samples

# [1] 从 FModel JSON 还原成可编辑蓝图（进 /Game/RestoredKards）
pwsh -File pipeline.ps1 restore -Source .\export\card_event_sabotage.json -Name card_event_sabotage

# [2] 注入：快烘焙 + 打补丁 pak + 部署到游戏
pwsh -File pipeline.ps1 inject -Cards card_event_go_all_out

# [2b] 零重启热补丁（只改数值/文本/关键字开关）
pwsh -File pipeline.ps1 hotpatch -Cards card_event_go_all_out

# [3] 交给玩家：生成测试包
pwsh -File pipeline.ps1 handoff -Cards card_event_go_all_out -Tester 老王

# [4] 正式烘焙（后台跑，与玩家测试并行）
pwsh -File pipeline.ps1 cook -Cards card_event_go_all_out -Background

# 校验 pak
pwsh -File pipeline.ps1 verify
pwsh -File pipeline.ps1 verify -Pak "<GAME>\kards\Content\Paks\KardsDIY_xxx_P.pak"

# 清理临时产物
pwsh -File pipeline.ps1 clean -Force
```

配置全在 [pipeline.json]((仓库)\pipeline.json) —— 换工程 / 换游戏目录只改这一个文件。

---

## 2. 阶段 1：蓝图写卡

### 2.1 路线 A —— 抄样例（推荐）

最快的方式是**复制一张已有的 DIY 卡**改：

1. 在 `(UE 工程)` 打开 `kards.uproject`
2. 内容浏览器进 `Content/Blueprints/Cards/USA/DIY/events/`
3. 复制 `card_event_kf1`（7 KB，最简单）→ 改名成你的卡
4. 改 CDO 属性：`Name` / `title` / `Text` / `Type` / `faction` / `kredits` / `rarity` / `Image`
5. 打开事件图，改 `OnPlayedFromHand` 的连线（效果就写在这里）
6. 编译 + 保存

### 2.2 路线 B —— 从 FModel JSON 还原

想把游戏原版某张卡拿来当底子：

1. **FModel** 加载 `映射\*.usmap` + `密钥.txt` 里的 AES key
2. 打开 `kards-Windows.pak` → 找到目标卡
3. **必须勾上 `Read Script Data`**，然后 `Save Properties` → 导出 JSON
   （不勾的话只能还原出父类、成员变量、默认值，**事件图是空的**）
4. 用还原器插件还原：

```powershell
# 编辑器里：Tools > KARDS Blueprint Restorer > Restore Blueprint from JSON...
# 或者命令行：
pwsh -File pipeline.ps1 restore -Source .\export\card_event_sabotage.json
```

底层就是：

```powershell
UnrealEditor-Cmd.exe (UE 工程)\kards.uproject `
  -run=KardsBlueprintRestore -Source="<path>\card_event_sabotage.json"
```

默认输出到 `/Game/RestoredKards`，**不会覆盖原工程资产**。

改完大蓝图想保留自己改过的那几个函数：

```powershell
UnrealEditor-Cmd.exe kards.uproject -run=KardsBlueprintRestore `
  -Source="<path>\BP_CardFunctions.json" `
  -UseOriginalPath -OverwriteExisting -PreserveFunctions=Forecast
```

### 2.3 还原器的限制（务必知道）

| 限制 | 后果 | 绕法 |
|---|---|---|
| `RawExport.Data` 是不透明 base64 | 解不了，还原直接报错停下 | 换导出器 / 换映射表 |
| `ScriptBytecodeRaw` 只有诊断用 | 拿不到节点，只有裸 Kismet 字节 | 必须要有解析后的 `ScriptBytecode` 或 `Script` |
| 没勾 `Read Script Data` | 只剩父类 + 变量 + 默认值 + 空事件壳 | FModel 里勾上再导 |
| `-AllowSkeleton` 只跳过"必须有脚本"检查 | **不**解码上面两种 raw 格式 | 别指望它救场 |

---

## 3. 阶段 2：UE4SS 注入

三条注入通道，按"改了什么"选：

| 改了什么 | 用哪条 | 生效方式 | 耗时 |
|---|---|---|---|
| **蓝图连线**（新效果、新逻辑） | `inject` → 补丁 pak | 玩家重启游戏 | 秒~分钟 |
| **纯数据**（数值、文案、关键字开关） | `hotpatch` → UE4SS 改 CDO | **零重启，立刻生效** | 秒 |
| **全新卡**（游戏里原本没有的卡名） | `inject` + AssetRegistry | 玩家重启游戏 | 分钟 |

### 3.1 补丁 pak（主力）

```powershell
pwsh -File pipeline.ps1 inject -Cards card_event_go_all_out
```

它做四件事：

1. **只烘焙你指定的包**（`-run=Cook -CookPackages=@list.txt`），不是全量
2. **打补丁 pak**：`repak pack <cooked> KardsDIY_<slug>_P.pak --mount-point ../../../`
3. **校验**：`repak list` 确认条目数 > 0
4. **部署**：拷到 `<GAME>\kards\Content\Paks\`

> **`_P.pak` 后缀是关键** —— UE 把 `*_P.pak` 当补丁包按优先级挂载，会**覆盖原版同名资产**。
> 不加 `_P` 就是普通包，优先级不够，你的改动不生效。

### 3.2 零重启热补丁

游戏已经在跑的时候，纯数据改动不用重启 —— 走 `ue4ss_mcp`：

```powershell
pwsh -File pipeline.ps1 hotpatch -Cards card_event_go_all_out
```

原理：`<UE4SS>` 通过文件 spool 协议（`commands/*.json` → `acks/*.json`）
让游戏里的 Lua 桥在**游戏线程**里执行：

```
1. ue4ss_list_assets / ue4ss_load_asset   找到卡的 BlueprintGeneratedClass
2. ue4ss_get_property <CDO> kredits       读当前值
3. ue4ss_set_property <CDO> kredits 5     写回
4. 已经在场上的实例要单独改；新抽到的卡自动用新值
```

**只能改数据，不能改连线。** 改结构必须走 inject。

### 3.3 LogicMods（另一条路）

游戏里已经有 `Content\Paks\LogicMods\` 目录，RE-UE4SS 的 `BPModLoaderMod` 会挂载里面的 pak
并自动生成 ModActor —— **不用动 AssetRegistry**，适合"加一张全新卡"。

---

## 4. 一张卡到底要填什么

从 `card_event_sabotage.json`（法国 · 破坏行动）实测出来的真实数据，**只有 20 个属性**：

```jsonc
{
  "Name":    "card_event_sabotage",              // FName 主键，SpawnCard* 靠它找卡
  "title":   { "Namespace": "card_france", "Key": "card_event_sabotage_title",
               "SourceString": "SABOTAGE" },     // FText，必须带 Namespace+Key 才能本地化
  "Type":    "ETypeEnum::order",                 // 单位/指令/阵地…
  "faction": "EFactionEnum::France",
  "kredits": 7,                                  // 费用
  "Text":    { "Namespace": "card_france", "Key": "card_event_sabotage_text",
               "SourceString": "Choose 1 of 3 cards in the enemy hand. Discard it..." },
  "flavorText": { "Namespace": "card_france", "Key": "card_event_sabotage_history", ... },
  "rarity":  "ERarityEnum::Uncommon",
  "cardSet": "ECardSetEnum::Expansion2019",
  "returnSet": "ECardSetEnum::CurrentExpansion",
  "soundEnterPlay": { "ObjectName": "SoundWave'War_UI_Drum_Movement_Field_Snares_4'" },
  "Image":   { "AssetPathName": "/Game/Assets/Textures/Images/France/DontUseOrTemp/t_french_resistance" },
  "artist":  "WWII French Resistance Poster",
  "historyPhoto": { "AssetPathName": "/Game/Assets/Textures/HistoricalPhotos/SABOTAGE" },
  "historyPhotoSize": { "X": 512.0, "Y": 364.0 },
  "historyPhotoSource": "https://www.dday-overlord.com/",
  "historyPhotoDescription": { "Namespace": "card_france", "Key": "card_event_sabotage_photo", ... },
  "OnAttackVisualsSoft": { "AssetPathName": "/Game/Blueprints/Effects/BP_AttackOrder.BP_AttackOrder_C" },
  "isDirectDamage": true                          // AI 启发式标记
}
```

**剩下 115 个属性用默认值就行。** 单位卡再多填 `attack` / `defense` / `range` / `operationCost`
和关键字 bool（`hasBlitz` / `hasGuard` / `hasAmbush` / `hasSmokescreen` / `hasMobilize` / `hasFury` / `hasShock` …）。

### 一张卡要实现哪几个函数

同一个 JSON 里的 `FuncMap`，只有 5 个：

| 函数 | 作用 | 要不要写 |
|---|---|---|
| `OnPlayedFromHand` | **效果本体** | 要 |
| `ExecuteUbergraph_card_event_sabotage` | 事件图入口（UE 自动生成） | 自动 |
| `CanPlayFromHand` | 能不能打出（返回 bool + 原因） | 需要条件时写 |
| `GetChooseSpawnCards` | 抉择卡：给玩家选哪几张 | 抉择卡写 |
| `OnHandTargetSelected` | 选了手牌目标之后 | 需要选手牌时写 |

### 效果怎么写

在 `OnPlayedFromHand` 里调 `BP_CardFunctions`（对应模板的 `ACardFunctionsStub`）的 190 个动词，例如：

```
OnPlayedFromHand(Method)
  ├─ GetOppositeSide(out enemySide)
  ├─ cardFunction.GetLocationCardBySide(out hq, out hqId, enemySide)
  └─ cardFunction.DamageCard(hq, 3, Self.CardID, false, false, false, out destroyed)
```

**触发注册**：`UBaseCardObject.usedTriggers`（`TArray<ERegisteredCardFunction>`，68 个值）
决定哪些 `OnOtherCard*` 事件会被派发。**没有 `RegisterCardFunction`，没有注册表** ——
就是在卡上勾数组 + 在蓝图里实现同名事件。

> 注意：枚举名和事件名有拼写差 ——
> `OnOtherCardAblitiesChanged` → 事件是 `OnOtherCardAbilitiesChanged`；
> `OnOtherCardBlitzChange` → `OnOtherCardBlitzChanged`；
> `OnOtherUnitUnPinned` → `OnOtherUnitUnpinned`；
> `OnStartofTurn` → `OnStartOfTurn`。

---

## 5. 阶段 4：烘焙 + 正式打包

```powershell
pwsh -File pipeline.ps1 cook -Cards card_event_go_all_out -Background
```

底层是标准 UE 打包：

```powershell
RunUAT.bat BuildCookRun -project=<kards.uproject> -noP4 -platform=Win64 -clientconfig=Shipping `
  -cook -build -stage -pak -compressed -archive -archivedirectory=<out>
```

后台跑的时候：

```powershell
Get-Content (仓库)\_release\<slug>\build.log -Wait -Tail 40   # 跟进度
```

**并行注意**：全量烘焙很吃 CPU/内存，玩家打联机时可能卡顿，建议错峰。

---

## 6. 目录结构

```
(仓库)\
├─ README.md              本文
├─ pipeline.json          全部路径与参数配置
├─ pipeline.ps1           总入口（8 个子命令）
├─ _stage\                每个卡的中间产物（cooked / pak / inject.json / cook.log）
├─ _handoff\              给玩家的交付包（zip）
├─ _release\              正式烘焙产物
└─ _logs\pipeline.log     全程日志
```

---

## 7. 已知坑

| 坑 | 说明 |
|---|---|
| **`_P.pak` 必须带** | 不带就是普通包，覆盖不了原版资产 |
| **mount point 必须是 `../../../`** | 和游戏原包一致，写错挂载失败 |
| **编辑器资产不能直接用** | 必须是烘焙过的（有 `.uexp`）。`pipeline.ps1 verify` 会检查 |
| **新增卡要进 AssetRegistry** | 否则游戏枚举不到这张卡。改已有卡不用 |
| **`UnrealPak.exe`（2016 版）不能用** | 打不出 V11 pak，用 repak 或 `UnrealPak-Tool.exe` |
| **RE-UE4SS 只在启动时挂载 pak** | 补丁 pak 必须重启游戏；纯数据改动才走 hotpatch |
| **测试包停用要整个移出游戏目录** | 放进 `Paks` 子目录或游戏内备份目录**仍会被加载** |
| **数值有加密** | `setAndEncrypt*` / `getAndDecrypt*`；直接改 `attack`/`defense` 可能被反篡改检测 |
| **`ECardEffectTypes` 被裁剪过** | 只剩 `Passive=9` / `Trigger=13`，值有洞，别依赖它 |

---

## 8. 第一次跑之前

1. 把还原器插件放进工程：
   拷 `KardsBlueprintRestorer` 到
   `(UE 工程)\Plugins\KardsBlueprintRestorer`
   （或直接用它自带的 `Plugins\KardsBlueprintRestorer`），启用后重启编辑器。
2. 确认工程里已有 KARDS 原生类型（`UBaseCardObject` / `ACardFunctionsStub`），
   否则蓝图父类解析不了。
3. `pwsh -File pipeline.ps1 status` 应该全绿。
