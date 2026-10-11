using System.Text.Json.Nodes;
using KardsCardCompiler.Core;

namespace KardsSpecBridge;

/// <summary>
/// <b>原生 spec 构建 API</b> —— 直接构造 KardsCardBuilder 的 spec 节点，
/// 补上蓝图 IR 表达不了的能力。
///
/// 蓝图 IR（<c>BlueprintExpression</c>/<c>BlueprintStatement</c>）缺四块，
/// 导致它无法 100% 覆盖 spec：
///   G1 <c>cast</c>        —— 无转型表达式（动态 Cast / 向上转型）
///   G2 <c>property</c>    —— 无"读别的对象属性"
///   G3 <c>arrayMake</c>   —— 无数组构造（如 <c>TArray&lt;int&gt;</c>）
///   G5 <c>kind:"k2"</c>   —— 无原生 UK2Node_* 逃生舱
///
/// 本类与 <see cref="Ir"/> 可自由混用：能用 IR 的地方用 IR，
/// IR 写不出的地方用这里直接造 spec 节点，最终都会走同一个插件。
/// </summary>
public static class Spec
{
    // ═════════════════════════════════════════════════════ 表达式

    /// <summary>self —— 本卡对象。</summary>
    public static JsonObject Self() => new() { ["kind"] = "self" };

    /// <summary>函数入参引脚。</summary>
    public static JsonObject Param(string name) => new()
    {
        ["kind"] = "param",
        ["name"] = name
    };

    /// <summary>本卡的成员变量。</summary>
    public static JsonObject Member(string name) => new()
    {
        ["kind"] = "member",
        ["name"] = name
    };

    /// <summary>函数图的真局部变量（仅 kind:"function" 图）。</summary>
    public static JsonObject Local(string name) => new()
    {
        ["kind"] = "local",
        ["name"] = name
    };

    /// <summary>
    /// <b>G2</b>：读<b>别的对象</b>的属性。
    /// 例：<c>Spec.Property(Spec.Param("opp"), "/Script/kards.BaseCardObject", "side")</c>
    /// </summary>
    public static JsonObject Property(JsonObject target, string ownerClass, string name) => new()
    {
        ["kind"] = "property",
        ["owner"] = SpecTypeMap.Norm(ownerClass),
        ["name"] = name,
        ["target"] = target
    };

    /// <summary>
    /// <b>G1</b>：动态 Cast。目标类型不符时走 <c>cast_ok</c>/<c>cast_fail</c> 分支
    /// （语句形态），或作为纯转型表达式使用。
    /// </summary>
    public static JsonObject Cast(JsonObject value, string targetClass) => new()
    {
        ["kind"] = "cast",
        ["class"] = SpecTypeMap.Norm(targetClass),
        ["target"] = value
    };

    /// <summary>向上转型到 /Script/CoreUObject.Object（喂给通用比较/容器）。</summary>
    public static JsonObject AsObject(JsonObject value) =>
        Cast(value, "/Script/CoreUObject.Object");

    /// <summary>
    /// <b>G3</b>：数组构造（MakeArray）。
    /// 插件要求<b>至少一项</b>（"arrayMake needs at least one item"）。
    /// 空数组请用变量默认值表达。
    /// </summary>
    public static JsonObject ArrayMake(string elementPinType, params JsonObject[] items)
    {
        if (items.Length == 0)
            throw new ArgumentException(
                "arrayMake 至少需要一项（插件硬性要求）。空数组请改用变量默认值。",
                nameof(items));

        var arr = new JsonArray();
        foreach (var i in items) arr.Add(i);
        return new JsonObject
        {
            ["kind"] = "arrayMake",
            ["type"] = elementPinType,
            ["items"] = arr
        };
    }

    /// <summary>int 数组构造。</summary>
    public static JsonObject IntArray(params int[] values) =>
        ArrayMake("int", values.Select(v => (JsonObject)Literal("int", v)).ToArray());

    // ── 字面量 ─────────────────────────────────────────────────────

    public static JsonObject Literal(string type, object? value) => new()
    {
        ["kind"] = "literal",
        ["type"] = type,
        ["value"] = JsonValue.Create(value)
    };

    public static JsonObject Int(int v) => Literal("int", v);
    public static JsonObject Bool(bool v) => Literal("bool", v);
    public static JsonObject Float(float v) => Literal("float", v);
    public static JsonObject Str(string v) => Literal("string", v);
    public static JsonObject Name(string v) => Literal("name", v);
    public static JsonObject Text(string v) => Literal("text", v);

    /// <summary>枚举字面量。<paramref name="enumClass"/> 形如 /Script/KardsCore.ETypeEnum。</summary>
    public static JsonObject Enum(string enumClass, string valueName) =>
        Literal(SpecTypeMap.Norm(enumClass), valueName);

    // ── 调用 ───────────────────────────────────────────────────────

    /// <summary>
    /// Pure 函数调用（作为表达式）。参数按<b>真实引脚名</b>给。
    /// </summary>
    public static JsonObject CallPure(string ownerClass, string function,
        IDictionary<string, JsonObject>? args = null, string resultPin = "ReturnValue")
    {
        var o = new JsonObject
        {
            ["kind"] = "call",
            ["owner"] = SpecTypeMap.Norm(ownerClass),
            ["function"] = function,
            ["pin"] = resultPin
        };
        if (args is { Count: > 0 })
        {
            var a = new JsonObject();
            foreach (var (k, v) in args) a[k] = v;
            o["args"] = a;
        }
        return o;
    }

    /// <summary>带 self 目标的 pure 调用（如 card.IsOrder()）。</summary>
    public static JsonObject CallOn(JsonObject target, string ownerClass, string function,
        IDictionary<string, JsonObject>? args = null, string resultPin = "ReturnValue")
    {
        var o = CallPure(ownerClass, function, args, resultPin);
        o["target"] = target;
        return o;
    }

    // ═════════════════════════════════════════════════════ 语句

    /// <summary>非 pure 函数调用（有执行引脚）→ 语句形态。出参走 outs。</summary>
    public static JsonObject CallStmt(string ownerClass, string function,
        IDictionary<string, JsonObject>? args = null,
        IDictionary<string, string>? outs = null,
        JsonObject? target = null)
    {
        var o = new JsonObject
        {
            ["kind"] = "call",
            ["owner"] = SpecTypeMap.Norm(ownerClass),
            ["function"] = function
        };
        if (target is not null) o["target"] = target;

        if (args is { Count: > 0 })
        {
            var a = new JsonObject();
            foreach (var (k, v) in args) a[k] = v;
            o["args"] = a;
        }
        if (outs is { Count: > 0 })
        {
            var ou = new JsonObject();
            foreach (var (pin, varName) in outs)
                ou[pin] = new JsonObject { ["var"] = varName };
            o["outs"] = ou;
        }
        return o;
    }

    /// <summary>赋值给成员变量。</summary>
    public static JsonObject Set(string memberName, JsonObject value) => new()
    {
        ["kind"] = "set",
        ["var"] = memberName,
        ["value"] = value
    };

    /// <summary>赋值给局部变量（需先 localDecl）。</summary>
    public static JsonObject LocalSet(string localName, JsonObject value) => new()
    {
        ["kind"] = "localSet",
        ["var"] = localName,
        ["value"] = value
    };

    /// <summary>声明局部变量。</summary>
    public static JsonObject LocalDecl(string name, string pinType) => new()
    {
        ["kind"] = "localDecl",
        ["var"] = name,
        ["type"] = pinType
    };

    public static JsonObject If(JsonObject cond, JsonArray then,
        JsonArray? @else = null)
    {
        var o = new JsonObject
        {
            ["kind"] = "if",
            ["cond"] = cond,
            ["then"] = then
        };
        if (@else is not null) o["else"] = @else;
        return o;
    }

    /// <summary>
    /// foreach 宏节点（StandardMacros:ForEachLoop）。
    /// <paramref name="outs"/> 可绑 "Array Element" / "Array Index"。
    /// </summary>
    public static JsonObject ForEach(JsonObject array, JsonArray body,
        IDictionary<string, string>? outs = null)
    {
        var o = new JsonObject
        {
            ["kind"] = "foreach",
            ["array"] = array,
            ["then"] = body
        };
        if (outs is { Count: > 0 })
        {
            var ou = new JsonObject();
            foreach (var (pin, varName) in outs)
                ou[pin] = new JsonObject { ["var"] = varName };
            o["outs"] = ou;
        }
        return o;
    }

    /// <summary>函数图的返回值（只在 kind:"function" 图合法）。</summary>
    public static JsonObject Result(IDictionary<string, JsonObject>? args = null)
    {
        var o = new JsonObject { ["kind"] = "result" };
        if (args is { Count: > 0 })
        {
            var a = new JsonObject();
            foreach (var (k, v) in args) a[k] = v;
            o["args"] = a;
        }
        return o;
    }

    /// <summary>
    /// 动态 Cast <b>语句</b>（带成功/失败分支 + 结果写入变量）。
    ///
    /// ⚠ 语句形态与表达式形态的字段名<b>不同</b>：
    ///   * 表达式：<c>{"kind":"cast","class":...}</c>  —— 读 <c>class</c>
    ///   * 语句  ：<c>{"kind":"cast","name":...}</c>   —— 读 <c>name</c>
    ///     （实测 CardSpec.cpp 的语句解析走 Statement->Name）
    ///
    /// 另外语句形态的 outs 取<b>第一个</b>条目接收 cast 结果，
    /// 不按引脚名匹配（CardGraphBuilder.cpp:938-942）。
    /// </summary>
    public static JsonObject CastStmt(JsonObject value, string targetClass,
        JsonArray onSuccess, JsonArray? onFail = null,
        string? outVar = null)
    {
        var o = new JsonObject
        {
            ["kind"] = "cast",
            ["name"] = SpecTypeMap.Norm(targetClass),   // ⚠ 语句用 name
            ["value"] = value,
            ["then"] = onSuccess
        };
        if (onFail is not null) o["else"] = onFail;
        if (outVar is not null)
        {
            // 只取第一个 sink 接收 cast 结果
            o["outs"] = new JsonObject
            {
                ["castResult"] = new JsonObject { ["var"] = outVar }
            };
        }
        return o;
    }

    // ═══════════════════════════════════════════════════ G5: k2 逃生舱

    /// <summary>
    /// <b>G5</b>：原样实例化任意官方 <c>UK2Node_*</c>。
    /// 这是表达能力的终极上限 —— 只要 UE 编辑器能放的节点，这里都能写。
    ///
    /// <paramref name="class"/> 可写短名（<c>K2Node_CallFunction</c>）或完整类路径。
    /// <paramref name="defaults"/> 是引脚默认值：引脚名 → 字面量文本
    /// （枚举写枚举项名，对象写路径）。
    /// </summary>
    public static JsonObject K2Node(string id, string @class,
        string? function = null, string? ownerClass = null,
        string? variable = null, string? macro = null,
        string? targetClass = null, int posX = 0, int posY = 0,
        string? comment = null,
        IDictionary<string, string>? defaults = null)
    {
        var o = new JsonObject
        {
            ["id"] = id,
            ["class"] = @class
        };
        if (function is not null) o["function"] = function;
        if (ownerClass is not null) o["owner"] = ownerClass;
        if (variable is not null) o["variable"] = variable;
        if (macro is not null) o["macro"] = macro;
        // ⚠ 字段名是 "target"（实测 CardSpec.cpp:547 NO->TryGetStringField("target", Node.TargetClass)）
        if (targetClass is not null) o["target"] = targetClass;
        if (posX != 0 || posY != 0)
            o["pos"] = new JsonArray(posX, posY);
        if (comment is not null) o["comment"] = comment;

        if (defaults is { Count: > 0 })
        {
            var d = new JsonObject();
            foreach (var (k, v) in defaults) d[k] = v;
            o["defaults"] = d;
        }
        return o;
    }

    /// <summary>
    /// k2 图里的一条引脚连线。
    ///
    /// 格式为<b>四元数组</b> <c>[fromId, fromPin, toId, toPin]</c>
    /// —— 实测 CardSpec.cpp:571-580，<b>不是</b>对象。
    /// </summary>
    public static JsonArray Link(string fromId, string fromPin, string toId, string toPin) =>
        new(fromId, fromPin, toId, toPin);

    // ═══════════════════════════════════════════════════════ 图

    /// <summary>事件图（覆写 OnXxx）。</summary>
    public static JsonObject EventGraph(string function, JsonArray body,
        string? owner = null) => Graph("event", function, body, null, null, owner);

    /// <summary>函数图（可覆写 BlueprintNativeEvent，或自定义函数）。</summary>
    public static JsonObject FunctionGraph(string function, JsonArray body,
        string? owner = null) => Graph("function", function, body, null, null, owner);

    /// <summary>原生 K2 节点图（G5 逃生舱）。</summary>
    public static JsonObject K2Graph(string function, JsonArray nodes, JsonArray links,
        string? owner = null) => Graph("k2", function, null, nodes, links, owner);

    private static JsonObject Graph(string kind, string function, JsonArray? body,
        JsonArray? nodes, JsonArray? links, string? owner)
    {
        var o = new JsonObject
        {
            ["kind"] = kind,
            ["function"] = function
        };
        if (body is not null) o["body"] = body;
        if (nodes is not null) o["nodes"] = nodes;
        if (links is not null) o["links"] = links;
        if (owner is not null) o["owner"] = SpecTypeMap.Norm(owner);
        return o;
    }

    // ═══════════════════════════════════════════════════ 常用封装

    /// <summary>卡片对象相等（自动向上转型）。</summary>
    public static JsonObject ObjectEquals(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "EqualEqual_ObjectObject",
            new Dictionary<string, JsonObject> { ["A"] = AsObject(a), ["B"] = AsObject(b) });

    /// <summary>卡片对象不等。</summary>
    public static JsonObject ObjectNotEquals(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "NotEqual_ObjectObject",
            new Dictionary<string, JsonObject> { ["A"] = AsObject(a), ["B"] = AsObject(b) });

    public static JsonObject IntEquals(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "EqualEqual_IntInt",
            new Dictionary<string, JsonObject> { ["A"] = a, ["B"] = b });

    public static JsonObject IntGreater(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "Greater_IntInt",
            new Dictionary<string, JsonObject> { ["A"] = a, ["B"] = b });

    public static JsonObject IntGreaterEqual(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "GreaterEqual_IntInt",
            new Dictionary<string, JsonObject> { ["A"] = a, ["B"] = b });

    public static JsonObject IntLess(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "Less_IntInt",
            new Dictionary<string, JsonObject> { ["A"] = a, ["B"] = b });

    public static JsonObject IntLessEqual(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "LessEqual_IntInt",
            new Dictionary<string, JsonObject> { ["A"] = a, ["B"] = b });

    public static JsonObject IntAdd(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "Add_IntInt",
            new Dictionary<string, JsonObject> { ["A"] = a, ["B"] = b });

    public static JsonObject BoolAnd(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "BooleanAND",
            new Dictionary<string, JsonObject> { ["A"] = a, ["B"] = b });

    public static JsonObject BoolOr(JsonObject a, JsonObject b) =>
        CallPure("/Script/Engine.KismetMathLibrary", "BooleanOR",
            new Dictionary<string, JsonObject> { ["A"] = a, ["B"] = b });

    public static JsonObject Not(JsonObject a) =>
        CallPure("/Script/Engine.KismetMathLibrary", "Not_PreBool",
            new Dictionary<string, JsonObject> { ["A"] = a });

    // ── KARDS 常用动作封装 ────────────────────────────────────────

    /// <summary>对某张卡造成伤害（非 pure，需目标卡对象）。</summary>
    public static JsonObject DamageCard(JsonObject card, JsonObject amount,
        JsonObject damagerCardId, string? outToVar = null,
        JsonObject? isRedirected = null, JsonObject? fromFight = null,
        JsonObject? isFightDefenderDamage = null)
    {
        var outs = outToVar is null
            ? null
            : new Dictionary<string, string> { ["targetDestroyed"] = outToVar };

        return CallStmt("/Script/kards.CardFunctionsStub", "DamageCard",
            new Dictionary<string, JsonObject>
            {
                ["card"] = card,
                ["amount"] = amount,
                ["damagerCardID"] = damagerCardId,
                ["isRedirected"] = isRedirected ?? Bool(false),
                ["fromFight"] = fromFight ?? Bool(false),
                ["isFightDefenderDamage"] = isFightDefenderDamage ?? Bool(false),
            },
            outs,
            target: Member("cardFunction"));
    }

    /// <summary>把某张卡的控制权转交给对方。</summary>
    public static JsonObject TakeControlOfEnemyUnit(JsonObject card, JsonObject instigatorId) =>
        CallStmt("/Script/kards.CardFunctionsStub", "TakeControlOfEnemyUnit",
            new Dictionary<string, JsonObject>
            {
                ["card"] = card,
                ["instigatorID"] = instigatorId,
            },
            null,
            target: Member("cardFunction"));

    /// <summary>取某一方总部那张卡（两个出参：card / locationCardID）。</summary>
    public static JsonObject GetLocationCardBySide(JsonObject side,
        string outCardVar, string outIdVar) =>
        CallStmt("/Script/kards.CardFunctionsStub", "GetLocationCardBySide",
            new Dictionary<string, JsonObject> { ["side"] = side },
            new Dictionary<string, string>
            {
                ["card"] = outCardVar,
                ["locationCardID"] = outIdVar,
            },
            target: Member("cardFunction"));

    /// <summary>本卡所属方（/Script/kards.ESideEnum）。</summary>
    public static JsonObject SelfSide() => Member("side");

    /// <summary>本卡的运行时 ID。</summary>
    public static JsonObject SelfCardId() => Member("CardID");
}
