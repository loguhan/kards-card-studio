using System.Text.Json.Nodes;
using KardsCardCompiler.Core;

namespace KardsSpecBridge;

/// <summary>
/// 把 (卡定义 + 蓝图函数 IR) 组装成 KardsCardBuilder 的 spec JSON。
///
/// 这里集中处理路线 A README 里记录的全部 CDO 坑：
///   * 坑 3：cardSet 必须在硬编码白名单内；Core 要映射成 Basic，
///           否则卡掉进 includeNotAttainable 分支被收藏跳过。
///   * 坑 4：isInPermanentPool 编译器不写 → 必须显式补 true。
/// </summary>
public static class SpecAssembler
{
    /// <summary>
    /// 把 (卡定义 + 蓝图函数 IR [+ 原生 spec 图]) 组装成 KardsCardBuilder 的 spec JSON。
    ///
    /// <paramref name="specSet"/> 非空时，其变量与图会<b>追加</b>在 IR 结果之后
    /// —— 两条路线可混用：能用 IR 的用 IR，IR 写不出的用原生 spec 节点。
    /// </summary>
    public static JsonObject Assemble(
        CardDefinition card,
        IReadOnlyList<BlueprintFunctionIr> functions,
        IReadOnlyList<BlueprintMemberDefinition> members,
        BridgeOptions opt,
        SpecGraphSet? specSet = null,
        bool debugDumpMap = false)
    {
        var ir = card.ToIr();

        // ── 变量：IR 的 Local/Output 变量 → Blueprint 成员变量 ──────────
        // 事件图没有真正的局部变量（UK2Node_Event 不带 LocalVariables），
        // 所以事件图里的 local 一律提升为成员变量。
        //
        // 命名加 <c>b_</c> 前缀：蓝图成员变量名不能与父类的函数名冲突
        // （实测：变量 isOrder 会撞上 BaseCardObject::IsOrder 函数，
        //  编译报 "找到名称冲突的成员变量"）。camelCase 的 IR 变量名
        //  与 PascalCase 的函数名极易相撞，统一加前缀彻底规避。
        var variables = new JsonArray();
        var usedNames = new HashSet<string>(StringComparer.Ordinal);
        var globalVarMap = new Dictionary<string, string>(StringComparer.Ordinal);

        foreach (var fn in functions)
        {
            foreach (var v in fn.Variables)
            {
                if (v.Role == BlueprintVariableRole.Input) continue; // 事件入参由图自带

                var name = Unique(usedNames, SafeVarName(v.Name));
                globalVarMap[v.Name] = name;

                variables.Add(new JsonObject
                {
                    ["name"] = name,
                    ["type"] = SpecTypeMap.ToPinType(v.Type)
                });
            }
        }

        // 显式声明的成员（ICardBlueprintDefinition.DefineBlueprintMembers）
        foreach (var m in members)
        {
            var name = Unique(usedNames, SafeVarName(m.Name));
            globalVarMap.TryAdd(m.Name, name);
            variables.Add(new JsonObject
            {
                ["name"] = name,
                ["type"] = SpecTypeMap.ToPinType(m.Type)
            });
        }

        // ── 原生 spec 变量（ISpecBlueprintDefinition）──────────────────
        // 这些变量名由作者直接控制（已经带 b_ 前缀则保持原样），
        // 因为原生 spec 节点是<b>按名字直接引用</b>它们的。
        if (specSet is not null)
        {
            foreach (var (rawName, pinType) in specSet.Variables)
            {
                var name = Unique(usedNames, rawName);
                globalVarMap.TryAdd(rawName, name);
                variables.Add(new JsonObject
                {
                    ["name"] = name,
                    ["type"] = pinType
                });
            }
        }

        var debugMap = debugDumpMap
            ? globalVarMap.OrderBy(kv => kv.Key, StringComparer.Ordinal)
                .ToDictionary(kv => kv.Key, kv => kv.Value)
            : null;

        // ── defaults：CDO 属性 ────────────────────────────────────────
        var defaults = new JsonArray
        {
            Def("Name", "name", ir.Name),
            Def("title", "text", ir.Text.Title),
            Def("Text", "text", ir.Text.Rules),
            Def("flavorText", "text", ir.Text.Flavor),
            Def("Type", "enum", MapCardType(ir.Kind, ir.UnitType)),
            Def("faction", "enum", ir.Faction.ToString()),
            Def("kredits", "int", ir.Stats.Cost),
            Def("rarity", "enum", ir.Rarity.ToString()),
            Def("cardSet", "enum", MapCardSet(ir.Set)),

            // ★ 坑 4：编译器不写这个字段，落回 C++ 默认 false → 卡进不了卡池。
            //   字段语义："Always in the card pool regardless of set"，真卡显式写 true。
            Def("isInPermanentPool", "bool", true),

            // ★ CardID 是运行时实例 ID（真卡为 0），不是目录 ID。
            Def("CardID", "int", 0),
        };

        // 单位卡专属数值
        if (ir.Kind == CardKind.Unit)
        {
            defaults.Add(Def("attack", "int", ir.Stats.Attack));
            defaults.Add(Def("defense", "int", ir.Stats.Defense));
            if (ir.Stats.Range > 0)
                defaults.Add(Def("range", "int", ir.Stats.Range));
        }

        // usedTriggers：ERegisteredCardFunction 枚举数组
        var triggers = CollectUsedTriggers(functions);
        if (specSet is not null)
        {
            // 原生 spec 图也从图名收集触发点
            foreach (var g in specSet.Graphs)
            {
                var fn = g["function"]?.GetValue<string>();
                if (fn is not null) triggers.Add(fn);
            }
        }

        // 原生 spec 附加默认值
        if (specSet is not null)
        {
            foreach (var (name, litType, value) in specSet.ExtraDefaults)
                defaults.Add(Def(name, litType, value));
        }

        triggers = triggers.Distinct().ToList();
        if (triggers.Count > 0)
        {
            var values = new JsonArray();
            foreach (var t in triggers) values.Add(t);
            defaults.Add(new JsonObject
            {
                ["name"] = "usedTriggers",
                ["enum"] = "ERegisteredCardFunction",
                ["values"] = values
            });
        }

        // ── graphs ───────────────────────────────────────────────────
        var graphs = new JsonArray();
        foreach (var fn in functions)
            graphs.Add(BuildGraph(fn, globalVarMap));

        // 原生 spec 图直接追加（作者已构造好完整节点）
        if (specSet is not null)
        {
            foreach (var g in specSet.Graphs)
                graphs.Add(g.DeepClone());
        }

        // ── 组装 ─────────────────────────────────────────────────────
        var assetPath = opt.AssetPathInPak
            ?? DeriveAssetPathInPak(opt.Package);

        var spec = new JsonObject
        {
            ["_comment"] = $"由 KardsSpecBridge 自动生成，源卡包: {ir.Name}（不要手改，改 C#）",
            ["pipeline"] = new JsonObject
            {
                ["cardName"] = opt.AssetName,
                ["pakName"] = opt.PakName,
                ["assetPathInPak"] = assetPath,
                ["filterPath"] = opt.FilterPath,
                ["hostRegistry"] = opt.HostRegistry,
                ["baseCatalog"] = opt.BaseCatalog,
                ["catalogPathInPak"] = opt.CatalogPathInPak,
                ["deckCode"] = opt.DeckCode,
                ["cardId"] = opt.CardId
            },
            ["package"] = opt.Package,
            ["assetName"] = opt.AssetName,
            ["parentClass"] = opt.ParentClass,
            ["variables"] = variables,
            ["defaults"] = defaults,
            ["graphs"] = graphs
        };

        // 调试用：IR 变量名 → 实际蓝图成员变量名
        if (debugMap is { Count: > 0 })
        {
            var dm = new JsonObject();
            foreach (var (k, v) in debugMap) dm[k] = v;
            spec["_variable_map"] = dm;
        }

        return spec;
    }

    // ───────────────────────────────────────────────────────────── graph

    private static JsonObject BuildGraph(
        BlueprintFunctionIr fn,
        IReadOnlyDictionary<string, string> globalVarMap)
    {
        var isEvent = IsEventGraph(fn.Name);

        // 函数图有真局部变量；事件图一律提升为成员变量
        var trueLocals = isEvent
            ? new HashSet<string>(StringComparer.Ordinal)
            : fn.Variables
                .Where(v => v.Role is BlueprintVariableRole.Local
                            or BlueprintVariableRole.Output
                            or BlueprintVariableRole.InputOutput)
                .Select(v => v.Name)
                .ToHashSet(StringComparer.Ordinal);

        // 事件入参 → param 引脚（用 IR 里的原名）
        var eventParams = fn.Variables
            .Where(v => v.Role == BlueprintVariableRole.Input)
            .Select(v => v.Name)
            .ToHashSet(StringComparer.Ordinal);

        var outVars = fn.Variables
            .Where(v => v.Role is BlueprintVariableRole.Output
                        or BlueprintVariableRole.InputOutput)
            .Select(v => v.Name)
            .ToHashSet(StringComparer.Ordinal);

        var expr = new SpecExpressionTranslator(globalVarMap, trueLocals, eventParams, outVars);
        var stmts = new SpecStatementTranslator(
            expr, globalVarMap, outVars, eventParams, isFunctionGraph: !isEvent);

        var g = new JsonObject
        {
            ["kind"] = isEvent ? "event" : "function",
            ["function"] = fn.Name,
            ["body"] = stmts.Translate(fn.Body)
        };

        if (fn.OverrideOwnerClass is not null)
            g["owner"] = SpecTypeMap.ClassPath(
                fn.OverrideOwnerPackage ?? "/Script/kards", fn.OverrideOwnerClass);

        return g;
    }

    /// <summary>
    /// 事件图 vs 函数图。插件只支持<b>事件图</b>（BluePrintImplementableEvent 覆写）
    /// 和函数图的普通形态；BlueprintNativeEvent 覆写（如 ShouldGotchaTrigger）
    /// 走 Function + owner。
    /// </summary>
    private static bool IsEventGraph(string functionName) =>
        functionName.StartsWith("On", StringComparison.Ordinal)
        || functionName.StartsWith("Receive", StringComparison.Ordinal);

    // ──────────────────────────────────────────────────────────── 触发注册

    /// <summary>
    /// 从实现的函数名推出需要写进 usedTriggers 的 ERegisteredCardFunction 值。
    /// 只有 OnOther* / 回合类事件需要注册；OnPlayedFromHand 由基类默认处理。
    /// </summary>
    private static List<string> CollectUsedTriggers(IReadOnlyList<BlueprintFunctionIr> fns)
    {
        var result = new List<string>();
        foreach (var fn in fns)
        {
            var n = fn.Name;
            var needsRegistration =
                n.StartsWith("OnOther", StringComparison.Ordinal)
                || n is "OnStartOfTurn" or "OnEndOfTurn" or "OnTurnEnd"
                    or "OnCardPlayed" or "OnUnitDestroyed" or "OnCardDamaged";

            if (needsRegistration && !result.Contains(n)) result.Add(n);
        }
        return result;
    }

    // ──────────────────────────────────────────────────────────── 枚举映射

    /// <summary>
    /// cardSet 必须落在 BP_CardFunctions_GetAllActiveStaticCards 的硬编码白名单内：
    ///   1 Basic, 2 Special, 3 OnlySpawnable, 4 Candidate, 5 Placeholder,
    ///   6 Expansion1, 7 Expansion2, 8 Expansion2019, 9 Campaign2020,
    ///   10 Breakthrough, 12 Legions, 13 WorldAtWar, 15 BrothersInArms,
    ///   16 WinterWar, 17 CovertOps, 18 BloodAndIron, 19 NavalWarfare,
    ///   20 Homefront, 21 OceaniaStorm
    /// 不在白名单：0 NotAvailable、11 Wildcards、14 Core(→Basic 别名)、22 Reserved
    /// </summary>
    private static string MapCardSet(CardSet set) => set switch
    {
        // ★ 坑 3：Core 是 Basic 的别名，真卡 CDO 里存的是 Basic。
        //   写 Core 会让卡掉进 includeNotAttainable 分支 → 收藏不显示。
        CardSet.Core => "Basic",
        _ => set.ToString()
    };

    /// <summary>
    /// ETypeEnum（实测 /Script/KardsCore.ETypeEnum）：
    ///   NotAvailable=0, location=1, order=2, tank=3, fighter=4, bomber=5,
    ///   infantry=6, artillery=7, antiair=8, antitank=9, tankdestroyer=10,
    ///   gotcha=11, wildcard=12
    ///
    /// <see cref="CardUnitType"/> 与它<b>逐项同名同序</b>（见 CardModel.cs:11-26），
    /// 所以单位卡直接取小写枚举名即可，无需手写映射表。
    ///
    /// 注意<b>没有</b> "unit" 这个值 —— 单位必须落到具体兵种，
    /// 否则插件报 "default 'Type': 'unit' is not a value of ETypeEnum"。
    /// </summary>
    private static string MapCardType(CardKind kind, CardUnitType unitType) => kind switch
    {
        CardKind.Unit => unitType switch
        {
            // 单位卡：ETypeEnum 值 = CardUnitType 的小写名
            CardUnitType.NotAvailable => "infantry",   // 未指定兵种时兜底到步兵
            var t => t.ToString().ToLowerInvariant()
        },
        CardKind.Event => "order",
        CardKind.Location => "location",
        _ => "order"
    };

    // ───────────────────────────────────────────────────────────── 小工具

    /// <summary>
    /// 把 IR 变量名转成安全的蓝图成员变量名。
    ///
    /// 蓝图成员变量名不能与父类已有成员（函数/变量/属性）重名 ——
    /// 实测 <c>isOrder</c> 会撞上 <c>BaseCardObject::IsOrder()</c>，
    /// 编译报 "找到名称冲突的成员变量"。
    /// 统一加 <c>b_</c> 前缀，一次性规避所有 camelCase↔PascalCase 相撞。
    /// </summary>
    private static string SafeVarName(string irName) =>
        irName.StartsWith("b_", StringComparison.Ordinal) ? irName : "b_" + irName;

    private static JsonObject Def(string name, string type, object? value) => new()
    {
        ["name"] = name,
        ["type"] = type,
        ["value"] = JsonValue.Create(value)
    };

    private static string DeriveAssetPathInPak(string package)
    {
        // /Game/Blueprints/Cards/Mods/Soviet/units/x
        //   → kards/Content/Blueprints/Cards/Mods/Soviet/units/x
        const string gameRoot = "/Game/";
        if (!package.StartsWith(gameRoot, StringComparison.Ordinal))
            throw new ArgumentException($"package 必须以 /Game/ 开头，实际是 {package}");
        return "kards/Content/" + package[gameRoot.Length..];
    }

    private static string Unique(HashSet<string> used, string name)
    {
        if (used.Add(name)) return name;
        for (var i = 2; ; i++)
        {
            var candidate = $"{name}_{i}";
            if (used.Add(candidate)) return candidate;
        }
    }
}
