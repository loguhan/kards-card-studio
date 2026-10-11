using System.Text.Json.Nodes;
using KardsCardCompiler.Core;

namespace KardsSpecBridge;

/// <summary>
/// C# 蓝图 IR 的语句 → KardsCardBuilder spec 的语句树。
///
/// spec 语句 kind（CardGraphBuilder.cpp 实测）：
///   call | if | set | localSet | localDecl | foreach | cast | result | seq
///
/// 硬规则（与插件一致，绝不静默降级）：
///   * 语句形态的 call 必须是<b>非 pure</b> 函数（有执行引脚）；
///     pure 函数想当语句用 → 报错，提示改成表达式。
///   * IR 把出参混在 Arguments 里；spec 要求出参走 <c>outs</c>。
/// </summary>
public sealed class SpecStatementTranslator
{
    private readonly SpecExpressionTranslator _expr;

    /// <summary>IR 局部变量名 → spec 变量名（用于出参汇入的 outs）。</summary>
    private readonly IReadOnlyDictionary<string, string> _varMap;

    /// <summary>被 IR 声明为函数 Output 的变量名集合。</summary>
    private readonly IReadOnlySet<string> _outVars;

    /// <summary>函数入参名（事件参数），用于区分"入参"与"出参汇入点"。</summary>
    private readonly IReadOnlySet<string> _eventParamNames;

    /// <summary>本图是不是函数图（决定 result 节点是否合法）。</summary>
    private readonly bool _isFunctionGraph;

    public SpecStatementTranslator(
        SpecExpressionTranslator expr,
        IReadOnlyDictionary<string, string> varMap,
        IReadOnlySet<string>? outVars = null,
        IReadOnlySet<string>? eventParams = null,
        bool isFunctionGraph = true)
    {
        _expr = expr;
        _varMap = varMap;
        _outVars = outVars ?? new HashSet<string>();
        _eventParamNames = eventParams ?? new HashSet<string>();
        _isFunctionGraph = isFunctionGraph;
    }

    private bool _isEventParam(string name) => _eventParamNames.Contains(name);

    // ─────────────────────────────────────────────────────────── 入口

    public JsonArray Translate(IReadOnlyList<BlueprintStatement> body)
    {
        var arr = new JsonArray();
        foreach (var s in body)
        {
            var node = TranslateOne(s);
            // 事件图里的无值 return 没有节点可落，直接跳过
            if (node is null) continue;

            // 多出参调用产生的内部 seq 载体：拆成兄弟语句平铺
            // （插件未实现 seq 节点，见 CardGraphBuilder.cpp:926）
            if (node is JsonObject o
                && o.TryGetPropertyValue("__expand", out var flag)
                && flag?.GetValue<bool>() == true)
            {
                foreach (var child in o["body"]!.AsArray())
                    if (child is not null) arr.Add(child.DeepClone());
                continue;
            }

            arr.Add(node);
        }
        return arr;
    }

    /// <summary>翻译一条语句；返回 null 表示该语句在当前图类型下不产生节点。</summary>
    public JsonNode? TranslateOne(BlueprintStatement s) => s switch
    {
        BlueprintCallStatement c => Call(c),
        BlueprintIfStatement f => If(f),
        BlueprintAssignStatement a => Assign(a),
        BlueprintWhileStatement w => While(w),
        BlueprintReturnStatement r => Result(r),
        BlueprintLocalCallStatement lc => LocalCall(lc),

        BlueprintBreakStatement => throw new NotSupportedException(
            "break 在 spec 里没有对应节点。请把循环体改写成带标志位的 if，" +
            "或改用 kind:\"k2\" 手写 K2Node_Break。"),

        BlueprintContinueStatement => throw new NotSupportedException(
            "continue 同上：spec 无对应节点，请改写条件或走 kind:\"k2\"。"),

        _ => throw new NotSupportedException(
            $"语句 {s.GetType().Name} 无法翻译成 spec 语句。")
    };

    // ────────────────────────────────────────────────────────── 各语句

    /// <summary>
    /// 判断某个实参表达式是不是"出参汇入点"。
    ///
    /// 关键发现（对照 IntelTamperingPack.cs 实测）：
    /// IR 里出参变量<b>不一定</b>声明为 Output 角色 —— 它们通常就是普通的 Local 变量，
    /// 只是被放在 Arguments 的对应位置上表示"接住这个出参"。
    /// 因此判定依据是<b>变量是否在全局变量映射里</b>，而不是它的 Role。
    /// </summary>
    private bool IsOutSink(BlueprintVariableExpression v) =>
        !_isEventParam(v.Name) && _varMap.ContainsKey(v.Name);

    /// <summary>
    /// 语句形态的调用。IR 出参 → spec 的 outs。
    ///
    /// 纯度规则：语句形态必须是非 pure 函数。若 IR 把一个 pure 调用写成了语句
    /// （例如 <c>IsOrder(out isOrder)</c> 后面紧跟一个 if），
    /// 就翻译成 <c>localSet 变量 = &lt;pure call&gt;</c>。
    /// </summary>
    private JsonNode Call(BlueprintCallStatement c)
    {
        var isPure = FunctionPurity.InferPure(c);

        if (!isPure)
            return ImpureCall(c);

        // pure 调用当语句用 → 它必然是为了接住某个出参。
        var targets = c.Arguments
            .OfType<BlueprintVariableExpression>()
            .Where(v => IsOutSink(v))
            .ToList();

        if (targets.Count == 0)
        {
            throw new NotSupportedException(
                $"pure 调用 {c.OwnerClass}.{c.FunctionName} 被写成语句，但没有接住任何出参。" +
                "pure 函数应作为表达式使用；请检查 C# IR。");
        }

        if (targets.Count == 1)
        {
            var name = _varMap[targets[0].Name];
            // 出参引脚名：查表，查不到用变量名（多数 KARDS 出参名与变量名一致）
            var pin = OutPinFor(c.OwnerClass, c.FunctionName, targets[0].Name);
            return new JsonObject
            {
                ["kind"] = "localSet",
                ["var"] = name,
                ["value"] = _expr.TranslateCallAsExpression(c, pin)
            };
        }

        // 多个出参：spec 的表达式只能取一个 pin，因此按 pin 逐个赋值。
        //
        // ⚠ 这里**不能**用 kind:"seq" —— 插件明确未实现
        //   （CardGraphBuilder.cpp:926 "sequence nodes are not implemented yet"）。
        // 正确做法：把多条 localSet 作为<b>兄弟语句</b>平铺进当前语句块，
        // 由调用方（Block）展开。这里返回一个 seq 仅仅是内部载体，
        // 由 ExpandSequences() 在拼装语句块时拆平。
        var block = new JsonArray();
        foreach (var t in targets)
        {
            var pin = OutPinFor(c.OwnerClass, c.FunctionName, t.Name);
            block.Add(new JsonObject
            {
                ["kind"] = "localSet",
                ["var"] = _varMap[t.Name],
                ["value"] = _expr.TranslateCallAsExpression(c, pin)
            });
        }
        return new JsonObject { ["kind"] = "seq", ["body"] = block, ["__expand"] = true };
    }

    /// <summary>
    /// 解出"这个调用该把结果写到哪个引脚"。
    ///
    /// 优先级：
    ///   1. 显式表 <see cref="KnownOutPins"/>（IR 变量名 → 真实引脚名）
    ///   2. 该函数<b>只有一个</b>数据出参 → 直接用那个出参（最稳，不依赖命名）
    ///   3. 退回 IR 变量名（插件会明确报 "no output pin"，不会静默连错）
    /// </summary>
    public static string OutPinFor(string ownerClass, string function, string irVarName)
    {
        if (KnownOutPins.TryGetValue($"{ownerClass}.{function}", out var pins)
            && pins.TryGetValue(irVarName, out var p))
            return p;

        // 单出参函数：名字随便，直接用那个唯一的出参
        if (SingleOutPin.TryGetValue($"{ownerClass}.{function}", out var only))
            return only;

        return irVarName;
    }

    /// <summary>
    /// 单出参函数的出参引脚名（实测自 -Mode=Dump 的 dump）。
    /// 有这张表，IR 里的出参变量名就可以随便起。
    /// </summary>
    private static readonly Dictionary<string, string> SingleOutPin = new()
    {
        ["BaseCardObject.IsOrder"] = "isIt",
        ["BaseCardObject.IsLocation"] = "isIt",
        ["BaseCardObject.IsUnit"] = "isIt",
        ["BaseCardObject.getTotalKreditCost"] = "totalKreditCost",
        ["BaseCardObject.GetOppositeSide"] = "oppositeSide",
        ["KismetArrayLibrary.Array_Length"] = "ReturnValue",
        ["KismetArrayLibrary.Array_Get"] = "Item",
        ["CardFunctionsStub.GotchaTriggered"] = "qqq",
        ["CardFunctionsStub.RandomIntFromRangeWithStream"] = "randomResult",
        ["CardFunctionsStub.GetAllActiveStaticCards"] = "Cards",
        ["CardFunctionsStub.DamageCard"] = "targetDestroyed",
        ["CardFunctionsStub.GetLocationCardBySide"] = "card",
    };

    /// <summary>运行时注册（供 -Mode=Dump 回填）。</summary>
    public static void RegisterSingleOutPin(string ownerClass, string function, string pin) =>
        SingleOutPin[$"{ownerClass}.{function}"] = pin;

    /// <summary>非 pure 调用 → spec 语句形态，出参走 outs。</summary>
    private JsonObject ImpureCall(BlueprintCallStatement c)
    {
        var o = new JsonObject
        {
            ["kind"] = "call",
            ["owner"] = SpecTypeMap.ClassPath(c.OwnerPackage, c.OwnerClass),
            ["function"] = c.FunctionName
        };

        if (c.Target is not null)
            o["target"] = _expr.Translate(c.Target);

        var args = new JsonObject();
        var outs = new JsonObject();

        for (var i = 0; i < c.Arguments.Count; i++)
        {
            var arg = c.Arguments[i];

            // 出参汇入：IR 用变量名在实参位置上表达"接住这个出参"
            if (arg is BlueprintVariableExpression v && IsOutSink(v))
            {
                outs[OutPinFor(c.OwnerClass, c.FunctionName, v.Name)] = new JsonObject
                {
                    ["var"] = _varMap[v.Name]
                };
                continue;
            }

            args[ArgPinName(c, arg, i)] = _expr.Translate(arg);
        }

        if (args.Count > 0) o["args"] = args;
        if (outs.Count > 0) o["outs"] = outs;
        return o;
    }

    /// <summary>
    /// IR 出参变量名 → 真实出参引脚名。
    ///
    /// 实测（-Mode=Dump）出参名常常与 IR 变量名不一致，例如
    /// <c>GotchaTriggered(... out qqq)</c> —— IR 里变量叫 stopped，引脚叫 qqq。
    /// 表里查不到时退回变量名本身（多数 KARDS 出参名与变量名一致）。
    /// </summary>
    private static readonly Dictionary<string, Dictionary<string, string>> KnownOutPins = new()
    {
        ["KismetArrayLibrary.Array_Get"] = new() { ["Item"] = "Item" },
        ["BaseCardObject.IsOrder"] = new() { ["isIt"] = "isIt" },
        ["BaseCardObject.IsLocation"] = new() { ["isIt"] = "isIt" },
        ["BaseCardObject.getTotalKreditCost"] = new() { ["totalKreditCost"] = "totalKreditCost" },
        ["BaseCardObject.GetOppositeSide"] = new() { ["oppositeSide"] = "oppositeSide" },

        // 实测引脚名见 CardFunctionsStub dump
        ["CardFunctionsStub.GotchaTriggered"] = new() { ["qqq"] = "qqq" },
        ["CardFunctionsStub.RandomIntFromRangeWithStream"] = new() { ["randomResult"] = "randomResult" },
        ["CardFunctionsStub.ConvertCard"] = new() { ["CardIDs"] = "CardIDs", ["newCardIDs"] = "newCardIDs" },
        ["CardFunctionsStub.GetAllActiveStaticCards"] = new() { ["Cards"] = "Cards" },
    };

    private JsonObject If(BlueprintIfStatement f)
    {
        var o = new JsonObject
        {
            ["kind"] = "if",
            ["cond"] = _expr.Translate(f.Condition),
            ["then"] = Translate(f.Then)
        };
        if (f.Else is { Count: > 0 })
            o["else"] = Translate(f.Else);
        return o;
    }

    private JsonObject Assign(BlueprintAssignStatement a)
    {
        // 目标是 self 成员 → set；是（真）局部变量 → localSet
        var (kind, name) = a.Destination switch
        {
            BlueprintMemberExpression m => ("set", m.Name),
            BlueprintSelfPropertyExpression p => ("set", p.PropertyName),
            BlueprintVariableExpression v => (
                _varMap.TryGetValue(v.Name, out var mapped) ? "localSet" : "set",
                _varMap.TryGetValue(v.Name, out var mapped2) ? mapped2 : v.Name),
            _ => throw new NotSupportedException(
                "赋值目标必须是变量：spec 的 set/localSet 只接受变量名。")
        };

        return new JsonObject
        {
            ["kind"] = kind,
            ["var"] = name,
            ["value"] = _expr.Translate(a.Value)
        };
    }

    /// <summary>
    /// 函数图的终结：把结果写到返回值引脚（<c>kind:"result"</c>）。
    ///
    /// 注意：<c>result</c> <b>只能出现在函数图</b>里（插件报
    /// "'result' is only valid inside a function graph"）。
    /// 事件图里的 <c>return</c> 只能靠"不接后续语句"表达，
    /// 因此这里由 <see cref="_isFunctionGraph"/> 分流。
    /// </summary>
    private JsonNode Result(BlueprintReturnStatement r)
    {
        if (!_isFunctionGraph)
        {
            if (r.Value is not null)
                throw new NotSupportedException(
                    "事件图里不能 return 一个值（result 节点只在函数图中有效）。" +
                    "请把它改写成独立的 kind:\"function\" 图，例如 ShouldGotchaTrigger。");

            // 无值 return 在事件图里没有节点可落 —— 它只表示"到此为止"。
            // spec 不支持 seq（插件明确报 "sequence nodes are not implemented yet"），
            // 所以这里返回 null，由 Translate 过滤掉。
            return null!;
        }

        var o = new JsonObject { ["kind"] = "result" };
        if (r.Value is not null)
            o["args"] = new JsonObject { ["returnValue"] = _expr.Translate(r.Value) };
        return o;
    }

    private JsonObject LocalCall(BlueprintLocalCallStatement lc)
    {
        var o = new JsonObject
        {
            ["kind"] = "call",
            ["function"] = lc.FunctionName
        };
        var args = new JsonObject();
        for (var i = 0; i < lc.Arguments.Count; i++)
            args[$"arg{i}"] = _expr.Translate(lc.Arguments[i]);
        if (args.Count > 0) o["args"] = args;
        return o;
    }

    // ─────────────────────────────────────────────────────────── 循环

    /// <summary>
    /// IR 的 while 是通用条件循环；spec 的 foreach 是 ForEachLoop<b>宏节点</b>。
    /// 两者只在"遍历数组"这一种形态下等价，因此这里做<b>精确形态识别</b>：
    ///
    ///     foreach (var x in arr) { body }
    ///
    /// 在 IR 里被降级成（CardBlueprintLowerer 的产物）：
    ///     idx = 0;
    ///     while (idx &lt; Array_Length(arr)) { x = Array_Get(arr, idx); body; idx = idx + 1; }
    ///
    /// 识别不出来就<b>硬失败</b>，绝不静默产出语义错误的图。
    /// 通用 while 请改用 kind:"k2" 手写 K2Node_MacroInstance / K2Node_While。
    /// </summary>
    private JsonObject While(BlueprintWhileStatement w)
    {
        var loop = TryRecognizeForeach(w);
        if (loop is not null) return loop;

        throw new NotSupportedException(
            "无法把该 while 识别成 foreach。spec 的 foreach 是 ForEachLoop 宏节点，" +
            "只覆盖\"遍历数组\"语义；通用条件循环没有直接对应。\n" +
            $"  条件表达式: {w.Condition.GetType().Name}\n" +
            $"  循环体语句数: {w.Body.Count}\n" +
            "请改写为遍历数组，或使用 kind:\"k2\" 原样实例化 K2Node_MacroInstance。");
    }

    /// <summary>
    /// 识别 "for i in 0..Array_Length(arr)" 形态，返回 foreach 语句；否则 null。
    /// </summary>
    private JsonObject? TryRecognizeForeach(BlueprintWhileStatement w)
    {
        // ── 条件必须是 <int_compare Less (var idx) (Array_Length arr)> ──
        if (w.Condition is not BlueprintIntComparisonExpression cmp) return null;
        if (cmp.Operator is not (BlueprintComparison.Less or BlueprintComparison.LessOrEqual)) return null;
        if (cmp.Left is not BlueprintVariableExpression idxVar) return null;
        if (!_varMap.TryGetValue(idxVar.Name, out var idxName)) return null;

        // 右侧必须是 KismetArrayLibrary.Array_Length(arr)
        var arrayExpr = TryUnwrapArrayLength(cmp.Right);
        if (arrayExpr is null) return null;

        // ── 循环体首句必须是 idxVar = Array_Get(arr, idxVar) 形态的"取元素" ──
        if (w.Body.Count == 0) return null;
        var (elemTarget, elementArray) = TryRecognizeElementFetch(w.Body[0], idxName);
        if (elemTarget is null || elementArray is null) return null;

        // 两次调用的数组必须是同一个表达式（按 JSON 结构比对）
        if (!JsonNode.DeepEquals(arrayExpr, elementArray)) return null;

        // ── 尾巴必须是把 idx 自增 1 ──
        var tail = w.Body[^1];
        if (!IsIncrementByOne(tail, idxName)) return null;

        // 中间的语句才是真正的循环体
        var bodyStatements = w.Body.Skip(1).Take(w.Body.Count - 2).ToArray();

        var o = new JsonObject
        {
            ["kind"] = "foreach",
            ["array"] = arrayExpr,
            ["outs"] = new JsonObject
            {
                // spec 的 foreach 出参；Array Element 绑到取元素那个变量
                ["Array Element"] = new JsonObject
                {
                    ["var"] = _varMap.TryGetValue(elemTarget, out var m) ? m : elemTarget
                }
            },
            ["then"] = Translate(bodyStatements)
        };
        return o;
    }

    /// <summary>从表达式里剥出 Array_Length 的数组参数。</summary>
    private JsonObject? TryUnwrapArrayLength(BlueprintExpression e)
    {
        if (e is not BlueprintCallExpression { Call: var call }) return null;
        if (call.FunctionName != "Array_Length") return null;
        if (call.OwnerClass != "KismetArrayLibrary") return null;
        if (call.Arguments.Count != 1) return null;
        return _expr.Translate(call.Arguments[0]);
    }

    /// <summary>识别 <c>elemVar = Arr[idx]</c>（IR 里是 assign(var, Array_Get(arr, idx))）。</summary>
    private (string? ElementVar, JsonObject? Array) TryRecognizeElementFetch(
        BlueprintStatement s, string idxName)
    {
        if (s is not BlueprintAssignStatement a) return (null, null);
        if (a.Destination is not BlueprintVariableExpression dv) return (null, null);
        if (a.Value is not BlueprintCallExpression { Call: var call }) return (null, null);
        if (call.FunctionName != "Array_Get") return (null, null);
        if (call.OwnerClass != "KismetArrayLibrary") return (null, null);
        if (call.Arguments.Count != 2) return (null, null);
        if (call.Arguments[1] is not BlueprintVariableExpression iv || iv.Name != idxName)
            return (null, null);

        return (dv.Name, _expr.Translate(call.Arguments[0]));
    }

    /// <summary>判断语句是不是 <c>idx = idx + 1</c>。</summary>
    private static bool IsIncrementByOne(BlueprintStatement s, string idxName)
    {
        if (s is not BlueprintAssignStatement a) return false;
        if (a.Destination is not BlueprintVariableExpression dv || dv.Name != idxName) return false;
        if (a.Value is not BlueprintIntBinaryExpression { Operator: BlueprintIntOperator.Add } add)
            return false;
        if (add.Left is not BlueprintVariableExpression l || l.Name != idxName) return false;
        return add.Right is BlueprintIntExpression { Value: 1 };
    }

    // ────────────────────────────────────────────────────────── 引脚命名

    /// <summary>
    /// 入参引脚名。spec 按<b>引脚名</b>匹配，因此这里必须给出真实函数的形参名。
    /// 已知映射表覆盖常用 KARDS 函数；未收录的走 arg{i} 并会被插件明确报错
    /// （"cannot find pin"），不会静默连错。
    /// </summary>
    private static string ArgPinName(BlueprintCallStatement c, BlueprintExpression arg, int index)
    {
        var pin = ArgPinNameFor(c.OwnerClass, c.FunctionName, index);
        // 有真实形参名时优先用它；否则退回 arg{i}
        return pin;
    }

    /// <summary>按 (宿主类, 函数, 实参序号) 查真实形参名；查不到返回 arg{index}。</summary>
    public static string ArgPinNameFor(string ownerClass, string function, int index) =>
        KnownPinNames.TryGetValue($"{ownerClass}.{function}", out var pins)
            && index < pins.Length
            ? pins[index]
            : $"arg{index}";

    private static string OutPinName(BlueprintCallStatement c, string irVarName) => irVarName;

    /// <summary>
    /// 常用函数的形参名表（顺序对应调用实参顺序）。
    /// 数据来自 spec-intel-tampering.json 已实测通过的用例 + kards header 契约。
    /// 新函数请用 -Mode=Dump 反射出真实签名后补进来。
    /// </summary>
    private static readonly Dictionary<string, string[]> KnownPinNames = new()
    {
        // ── KismetMathLibrary ──
        ["KismetMathLibrary.Add_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.Subtract_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.Multiply_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.Divide_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.Percent_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.EqualEqual_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.NotEqual_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.Less_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.LessEqual_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.Greater_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.GreaterEqual_IntInt"] = new[] { "A", "B" },
        ["KismetMathLibrary.BooleanAND"] = new[] { "A", "B" },
        ["KismetMathLibrary.BooleanOR"] = new[] { "A", "B" },
        ["KismetMathLibrary.BooleanXOR"] = new[] { "A", "B" },
        ["KismetMathLibrary.Not_PreBool"] = new[] { "A" },
        ["KismetMathLibrary.EqualEqual_ByteByte"] = new[] { "A", "B" },
        ["KismetMathLibrary.EqualEqual_NameName"] = new[] { "A", "B" },
        ["KismetMathLibrary.NotEqual_NameName"] = new[] { "A", "B" },
        ["KismetMathLibrary.EqualEqual_ObjectObject"] = new[] { "A", "B" },
        ["KismetMathLibrary.NotEqual_ObjectObject"] = new[] { "A", "B" },

        // ── KismetArrayLibrary（路线 A 完全做不到的）──
        ["KismetArrayLibrary.Array_Length"] = new[] { "TargetArray" },
        ["KismetArrayLibrary.Array_Get"] = new[] { "TargetArray", "Index" },
        ["KismetArrayLibrary.Array_Add"] = new[] { "TargetArray", "NewItem" },
        ["KismetArrayLibrary.Array_Clear"] = new[] { "TargetArray" },

        // ── BaseCardObject ──
        // 实测（-Mode=Dump /Script/kards.BaseCardObject）：
        //   IsOrder            [callable,pure,native]  只有 out isIt，<b>无输入引脚</b>
        //   getTotalKreditCost [callable,pure,native]  只有 out totalKreditCost，无输入
        //   GetOppositeSide    [callable,pure,native]  只有 out oppositeSide，无输入
        ["BaseCardObject.IsOrder"] = Array.Empty<string>(),
        ["BaseCardObject.IsLocation"] = Array.Empty<string>(),
        ["BaseCardObject.getTotalKreditCost"] = Array.Empty<string>(),
        ["BaseCardObject.GetOppositeSide"] = Array.Empty<string>(),

        // ── CardFunctionsStub ──
        ["CardFunctionsStub.GotchaTriggered"] = new[]
            { "card", "instigatorID", "stopFurtherCardActions", "skipDiscardingOrder", "qqq" },
        ["CardFunctionsStub.GetAllActiveStaticCards"] = new[]
            { "includeNotAttainable", "includeReserved", "Cards" },
        ["CardFunctionsStub.ConvertCard"] = new[]
            { "CardIDs", "instigatorID", "convertToCardName", "convertIntoCardID", "skipTrigger", "newCardIDs" },
        ["CardFunctionsStub.AddToBattleLog"] = new[]
            { "CardID", "instigatorID", "cardHidden", "showAnimation" },
        ["CardFunctionsStub.RandomIntFromRangeWithStream"] = new[]
            { "Minimum", "Maximum", "randomResult" },
        ["CardFunctionsStub.DamageCard"] = new[]
            { "card", "amount", "damagerCardID", "isRedirected", "fromFight", "isFightDefenderDamage" },
        ["CardFunctionsStub.TakeControlOfEnemyUnit"] = new[] { "card", "instigatorID" },
        ["CardFunctionsStub.ReleaseControlOfEnemyUnit"] = new[]
            { "card", "instigatorID", "originalLocation", "originalSide" },
        ["CardFunctionsStub.GetLocationCardBySide"] = new[] { "side" },
        ["CardFunctionsStub.DamageMultipleCards"] = new[] { "amount", "damagerCardID" },
    };

    /// <summary>供外部（如 Dump 模式回填）注册新函数的形参名。</summary>
    public static void RegisterPins(string ownerClass, string function, params string[] pins) =>
        KnownPinNames[$"{ownerClass}.{function}"] = pins;
}
