using System.Text.Json.Nodes;
using KardsCardCompiler.Core;

namespace KardsSpecBridge;

/// <summary>
/// C# 蓝图 IR 的表达式 → KardsCardBuilder spec 的表达式树。
///
/// spec 表达式 kind（CardGraphBuilder.cpp 实测）：
///   self | param | member | local | property | call | literal | cast | arrayMake
///
/// 关键约束（与插件哲学一致，写错就硬失败）：
///   * 作为<b>表达式</b>的 call 必须是 <b>pure</b> 函数；
///     非 pure 函数（有执行引脚）必须走 <b>语句</b>形态。
///   * 属性读取：self 的成员用 "member"；<b>别的对象</b>的属性用 "property"
///     （需要 target + owner + name）—— 这正是路线 A 做不到、我们来补的。
/// </summary>
public sealed class SpecExpressionTranslator
{
    /// <summary>IR 局部变量名 → spec 成员变量名（事件图无局部变量，一律提升为成员）。</summary>
    private readonly IReadOnlyDictionary<string, string> _localToMember;

    /// <summary>函数图的真局部变量名集合（kind:"function" 时 local 是真局部）。</summary>
    private readonly IReadOnlySet<string> _trueLocals;

    /// <summary>事件入参名 → 该入参在事件图上的引脚名（通常同名）。</summary>
    private readonly IReadOnlySet<string> _eventParams;

    /// <summary>IR 里被声明为函数 Output / InputOutput 的变量名（即"出参汇入点"）。</summary>
    private readonly IReadOnlySet<string> _outVars;

    /// <summary>全部被提升为成员变量的 IR 变量名（出参汇入点的判定依据）。</summary>
    private readonly IReadOnlySet<string> _localNames;

    public SpecExpressionTranslator(
        IReadOnlyDictionary<string, string>? localToMember = null,
        IReadOnlySet<string>? trueLocals = null,
        IReadOnlySet<string>? eventParams = null,
        IReadOnlySet<string>? outVars = null)
    {
        _localToMember = localToMember ?? new Dictionary<string, string>();
        _trueLocals = trueLocals ?? new HashSet<string>();
        _eventParams = eventParams ?? new HashSet<string>();
        _outVars = outVars ?? new HashSet<string>();
        _localNames = _localToMember.Keys.ToHashSet(StringComparer.Ordinal);
    }

    /// <summary>
    /// 判定某个实参位置是不是"出参汇入点"。
    ///
    /// 实测（IntelTamperingPack）：IR 把出参变量放在 Arguments 的对应位置，
    /// 但它们通常声明为普通 Local 角色，而非 Output。
    /// 因此不能只看 Role —— 只要该名字出现在变量映射里且不是事件入参，
    /// 就说明它是"接住出参"的汇入点。
    /// </summary>
    private bool IsOutSink(string name) =>
        !_eventParams.Contains(name) && _localNames.Contains(name);

    // ─────────────────────────────────────────────────────────── 入口

    public JsonObject Translate(BlueprintExpression e) => e switch
    {
        // ── 自身与参数 ──────────────────────────────────────────
        BlueprintSelfExpression self => new JsonObject
        {
            ["kind"] = "self"
        },

        BlueprintVariableExpression v => TranslateVariable(v),

        BlueprintMemberExpression m => new JsonObject
        {
            ["kind"] = "member",
            ["name"] = m.Name
        },

        // self 的属性：本卡 CDO 上的字段
        BlueprintSelfPropertyExpression p => new JsonObject
        {
            ["kind"] = "member",
            ["name"] = p.PropertyName
        },

        // ── 字面量 ──────────────────────────────────────────────
        BlueprintIntExpression i => Lit("int", i.Value),
        BlueprintBoolExpression b => Lit("bool", b.Value),
        BlueprintFloatExpression f => Lit("float", f.Value),
        BlueprintNameExpression n => Lit("name", n.Value),
        BlueprintStringExpression s => Lit("string", s.Value),
        BlueprintEmptyTextExpression => Lit("text", string.Empty),

        BlueprintByteExpression by when by.EnumType is not null =>
            Lit(SpecTypeMap.ToPinType(by.EnumType), by.Value),

        BlueprintByteExpression by => Lit("byte", by.Value),

        // ── 复合 ────────────────────────────────────────────────
        BlueprintNotExpression n => PureCall("Not_PreBool", ("A", Translate(n.Operand))),
        BlueprintIntBinaryExpression bin => IntBinary(bin),
        BlueprintIntComparisonExpression cmp => IntCompare(cmp),
        BlueprintBoolBinaryExpression b2 => BoolBinary(b2),
        BlueprintCallExpression ce => TranslateCallExpression(ce),
        BlueprintLocalCallExpression lc => LocalCallValue(lc),

        _ => throw new NotSupportedException(
            $"表达式 {e.GetType().Name} 无法翻译成 spec 表达式。\n" +
            "如果是新引入的 IR 类型，请在 SpecExpressionTranslator.Translate 里补 case；" +
            "不要让它静默降级。")
    };

    // ─────────────────────────────────────────────────── 变量与参数

    private JsonObject TranslateVariable(BlueprintVariableExpression v)
    {
        // ① 事件入参 → param 引脚
        if (_eventParams.Contains(v.Name))
            return new JsonObject { ["kind"] = "param", ["name"] = v.Name };

        // ② 函数图的真局部变量 → local
        if (_trueLocals.Contains(v.Name))
            return new JsonObject { ["kind"] = "local", ["name"] = v.Name };

        // ③ 事件图里被提升的"局部变量" → 成员变量
        if (_localToMember.TryGetValue(v.Name, out var member))
            return new JsonObject { ["kind"] = "member", ["name"] = member };

        // ④ 兜底：当作 self 成员读取（IR 里 local 与 member 共用变量名空间）
        return new JsonObject { ["kind"] = "member", ["name"] = v.Name };
    }

    // ────────────────────────────────────────────── 高层糖 → 显式节点

    private JsonObject IntBinary(BlueprintIntBinaryExpression b)
    {
        var fn = b.Operator switch
        {
            BlueprintIntOperator.Add => "Add_IntInt",
            BlueprintIntOperator.Subtract => "Subtract_IntInt",
            BlueprintIntOperator.Multiply => "Multiply_IntInt",
            BlueprintIntOperator.Divide => "Divide_IntInt",
            BlueprintIntOperator.Modulo => "Percent_IntInt",
            _ => throw new NotSupportedException($"int operator {b.Operator}")
        };
        return PureCall(fn, ("A", Translate(b.Left)), ("B", Translate(b.Right)));
    }

    private JsonObject IntCompare(BlueprintIntComparisonExpression c)
    {
        var fn = c.Operator switch
        {
            BlueprintComparison.Equal => "EqualEqual_IntInt",
            BlueprintComparison.NotEqual => "NotEqual_IntInt",
            BlueprintComparison.Less => "Less_IntInt",
            BlueprintComparison.LessOrEqual => "LessEqual_IntInt",
            BlueprintComparison.Greater => "Greater_IntInt",
            BlueprintComparison.GreaterOrEqual => "GreaterEqual_IntInt",
            _ => throw new NotSupportedException($"comparison {c.Operator}")
        };
        return PureCall(fn, ("A", Translate(c.Left)), ("B", Translate(c.Right)));
    }

    private JsonObject BoolBinary(BlueprintBoolBinaryExpression b)
    {
        var fn = b.Operator switch
        {
            BlueprintBoolOperator.And => "BooleanAND",
            BlueprintBoolOperator.Or => "BooleanOR",
            BlueprintBoolOperator.Xor => "BooleanXOR",
            _ => throw new NotSupportedException($"bool operator {b.Operator}")
        };
        return PureCall(fn, ("A", Translate(b.Left)), ("B", Translate(b.Right)));
    }

    /// <summary>
    /// 把 IR 的 call 直接翻成 spec 的<b>表达式</b> call（用于 pure 函数，
    /// 或已被语句链 hoist 的 impure 调用）。可显式指定取哪个出参引脚。
    ///
    /// 注意：pure 函数的"出参"在 IR 里同样占据 Arguments 的一个位置
    /// （实测 IsOrder(out isOrder)：Arguments = [isOrder]，而出参引脚名是 isIt）。
    /// 这些位置<b>不能</b>进 args —— 它们的值是本节点的输出，不是输入。
    /// </summary>
    public JsonObject TranslateCallAsExpression(BlueprintCallStatement call, string? resultPin = null)
    {
        var o = new JsonObject
        {
            ["kind"] = "call",
            ["owner"] = SpecTypeMap.ClassPath(call.OwnerPackage, call.OwnerClass),
            ["function"] = call.FunctionName
        };

        if (call.Target is not null)
            o["target"] = Translate(call.Target);

        var a = new JsonObject();
        for (var i = 0; i < call.Arguments.Count; i++)
        {
            // 出参汇入点不进 args（它的值来自本节点的输出引脚）
            if (call.Arguments[i] is BlueprintVariableExpression v && IsOutSink(v.Name))
                continue;

            var pin = ArgPinName(call, i);
            var translated = Translate(call.Arguments[i]);
            a[pin] = NeedsUpcastToObject(call.OwnerClass, call.FunctionName, pin)
                ? UpcastToObject(translated)
                : translated;
        }
        if (a.Count > 0) o["args"] = a;

        o["pin"] = resultPin ?? "ReturnValue";
        return o;
    }

    /// <summary>
    /// 形参声明为基类 <c>Object</c>、但实参是派生类的函数。
    /// 插件不做隐式 Cast，这些引脚需要桥接器自动插一层向上转型。
    /// </summary>
    private static readonly Dictionary<string, string[]> AutoUpcastToObjectPins = new()
    {
        ["KismetMathLibrary.EqualEqual_ObjectObject"] = new[] { "A", "B" },
        ["KismetMathLibrary.NotEqual_ObjectObject"] = new[] { "A", "B" },
    };

    /// <summary>该 (owner, fn, pin) 是否需要自动向上转型到 Object。</summary>
    public static bool NeedsUpcastToObject(string ownerClass, string function, string pin) =>
        AutoUpcastToObjectPins.TryGetValue($"{ownerClass}.{function}", out var pins)
        && pins.Contains(pin, StringComparer.Ordinal);

    /// <summary>供 Dump 回填用。</summary>
    public static void RegisterUpcastPins(string ownerClass, string function, params string[] pins) =>
        AutoUpcastToObjectPins[$"{ownerClass}.{function}"] = pins;

    private JsonObject TranslateCallExpression(BlueprintCallExpression ce)
    {
        var call = ce.Call;
        var o = new JsonObject
        {
            ["kind"] = "call",
            ["owner"] = SpecTypeMap.ClassPath(call.OwnerPackage, call.OwnerClass),
            ["function"] = call.FunctionName
        };

        if (call.Target is not null)
            o["target"] = Translate(call.Target);

        // 表达式形态不接受 outs；出参汇入只能出现在语句形态里。
        var a = new JsonObject();
        for (var i = 0; i < call.Arguments.Count; i++)
        {
            if (call.Arguments[i] is BlueprintVariableExpression v && IsOutSink(v.Name))
                continue;

            var pin = ArgPinName(call, i);
            var translated = Translate(call.Arguments[i]);
            a[pin] = NeedsUpcastToObject(call.OwnerClass, call.FunctionName, pin)
                ? UpcastToObject(translated)
                : translated;
        }
        if (a.Count > 0) o["args"] = a;

        // 取哪个出参：默认 ReturnValue（CardSpec.h:39 的 ResultPin 语义）
        o["pin"] = "ReturnValue";
        return o;
    }

    private JsonObject LocalCallValue(BlueprintLocalCallExpression lc)
    {
        var o = new JsonObject
        {
            ["kind"] = "call",
            ["function"] = lc.FunctionName,
            ["pin"] = "ReturnValue"
        };
        if (lc.Arguments.Count > 0)
            o["args"] = BuildArgs(lc.Arguments);
        return o;
    }

    // ───────────────────────────────────────────────────────── 构造助手

    /// <summary>
    /// 自动向上转型：把对象类型的实参包一层 cast 到目标类型。
    ///
    /// 插件<b>不做</b>隐式 Cast —— 实测把 BaseCardObject 接到
    /// EqualEqual_ObjectObject 的 A 会报：
    ///   "incompatible pins object:/Script/kards.BaseCardObject
    ///    -> object:/Script/CoreUObject.Object"
    ///
    /// 因此凡是形参声明为基类 Object 而实参是派生类的地方，
    /// 桥接器自动插入 spec 的 cast 表达式。
    /// </summary>
    public JsonObject Upcast(JsonObject expr, string targetClass)
    {
        return new JsonObject
        {
            ["kind"] = "cast",
            ["class"] = SpecTypeMap.Norm(targetClass),
            ["target"] = expr
        };
    }

    /// <summary>向上转型到 /Script/CoreUObject.Object（比较、容器等通用场景）。</summary>
    public JsonObject UpcastToObject(JsonObject expr) =>
        Upcast(expr, "/Script/CoreUObject.Object");

    /// <summary>
    /// 对象相等比较，自动处理类型不匹配（两边都向上转型到 Object）。
    /// </summary>
    public JsonObject ObjectEquals(JsonObject left, JsonObject right) =>
        PureCall("EqualEqual_ObjectObject",
            ("A", UpcastToObject(left)),
            ("B", UpcastToObject(right)));

    /// <summary>对象不等比较。</summary>
    public JsonObject ObjectNotEquals(JsonObject left, JsonObject right) =>
        PureCall("NotEqual_ObjectObject",
            ("A", UpcastToObject(left)),
            ("B", UpcastToObject(right)));

    /// <summary>KismetMathLibrary 等 pure 函数的表达式调用。</summary>
    public JsonObject PureCall(string fn, params (string Pin, JsonObject Expr)[] args) =>
        PureCall("/Script/Engine.KismetMathLibrary", fn, args);

    public JsonObject PureCall(string owner, string fn,
        params (string Pin, JsonObject Expr)[] args)
    {
        var o = new JsonObject
        {
            ["kind"] = "call",
            ["owner"] = SpecTypeMap.Norm(owner),
            ["function"] = fn
        };
        if (args.Length > 0)
        {
            var a = new JsonObject();
            foreach (var (pin, ex) in args) a[pin] = ex;
            o["args"] = a;
        }
        return o;
    }

    public static JsonObject Lit(string type, object? value) => new()
    {
        ["kind"] = "literal",
        ["type"] = type,
        ["value"] = JsonValue.Create(value)
    };

    /// <summary>
    /// 读<b>别的对象</b>的属性 —— 路线 A 的硬边界，spec 的 "property" 表达式。
    /// 例：{"kind":"property","owner":"/Script/kards.BaseCardObject",
    ///      "name":"CardID","target":{...}}
    /// </summary>
    public JsonObject Property(JsonObject target, string ownerClass, string propertyName) => new()
    {
        ["kind"] = "property",
        ["owner"] = SpecTypeMap.Norm(ownerClass),
        ["name"] = propertyName,
        ["target"] = target
    };

    /// <summary>对象数组构造（MakeArray）。items 至少一项 —— 空数组插件会报错。</summary>
    public static JsonObject ArrayMake(string elementType, params JsonObject[] items)
    {
        if (items.Length == 0)
            throw new ArgumentException(
                "arrayMake needs at least one item（插件实测报错）。" +
                "空数组请改用变量默认值表达。", nameof(items));

        var arr = new JsonArray();
        foreach (var i in items) arr.Add(i);
        return new JsonObject
        {
            ["kind"] = "arrayMake",
            ["type"] = elementType,
            ["items"] = arr
        };
    }

    private JsonObject BuildArgs(BlueprintCallStatement call)
    {
        var a = new JsonObject();
        for (var i = 0; i < call.Arguments.Count; i++)
            a[ArgPinName(call, i)] = Translate(call.Arguments[i]);
        return a;
    }

    private JsonObject BuildArgs(IReadOnlyList<BlueprintExpression> args)
    {
        var a = new JsonObject();
        for (var i = 0; i < args.Count; i++)
            a[$"arg{i}"] = Translate(args[i]);
        return a;
    }

    /// <summary>入参引脚名（复用语句翻译器的形参名表）。</summary>
    private static string ArgPinName(BlueprintCallStatement c, int index) =>
        SpecStatementTranslator.ArgPinNameFor(c.OwnerClass, c.FunctionName, index);
}
