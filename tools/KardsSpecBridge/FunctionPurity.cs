using KardsCardCompiler.Core;

namespace KardsSpecBridge;

/// <summary>
/// 函数纯度表 —— 决定一个调用该落成 spec 的<b>表达式</b>还是<b>语句</b>。
///
/// 插件的硬规则（CardGraphBuilder.cpp 实测）：
///   * 语句形态 <c>kind:"call"</c>：函数必须<b>非 pure</b>（有执行引脚），
///     否则报 <c>"call 'X' is pure; pure calls belong in expressions"</c>（:1027）。
///   * 表达式形态 <c>kind:"call"</c>：可以是 pure；若为非 pure，
///     只有在"已在语句链上"时才被 hoist（:766-776），否则报
///     <c>"'X' has execution pins but the expression is not inside a statement"</c>（:771）。
///
/// 所以 IR 的每个 call 都必须先查纯度，再决定翻译形态。
/// 这张表由 <c>-Mode=Dump</c> 反射出的 <c>blueprint_pure</c> 字段驱动；
/// 未知函数默认按"非 pure"处理（偏向语句），失败时插件会明确报错而非静默出错。
/// </summary>
public static class FunctionPurity
{
    private static readonly HashSet<string> Pure = new(StringComparer.Ordinal)
    {
        // ── KismetMathLibrary：全部 pure ──
        "KismetMathLibrary.Add_IntInt",
        "KismetMathLibrary.Subtract_IntInt",
        "KismetMathLibrary.Multiply_IntInt",
        "KismetMathLibrary.Divide_IntInt",
        "KismetMathLibrary.Percent_IntInt",
        "KismetMathLibrary.EqualEqual_IntInt",
        "KismetMathLibrary.NotEqual_IntInt",
        "KismetMathLibrary.Less_IntInt",
        "KismetMathLibrary.LessEqual_IntInt",
        "KismetMathLibrary.Greater_IntInt",
        "KismetMathLibrary.GreaterEqual_IntInt",
        "KismetMathLibrary.BooleanAND",
        "KismetMathLibrary.BooleanOR",
        "KismetMathLibrary.BooleanXOR",
        "KismetMathLibrary.Not_PreBool",
        "KismetMathLibrary.EqualEqual_ByteByte",
        "KismetMathLibrary.EqualEqual_NameName",
        "KismetMathLibrary.NotEqual_NameName",
        "KismetMathLibrary.EqualEqual_ObjectObject",
        "KismetMathLibrary.NotEqual_ObjectObject",
        "KismetMathLibrary.EqualEqual_ClassClass",
        "KismetMathLibrary.RandomIntegerInRange",
        "KismetMathLibrary.RandomInteger",

        // ── KismetArrayLibrary：全部 pure（取值类）──
        "KismetArrayLibrary.Array_Length",
        "KismetArrayLibrary.Array_Get",
        "KismetArrayLibrary.Array_IsValidIndex",
        "KismetArrayLibrary.Array_Contains",
        "KismetArrayLibrary.Array_Find",
        "KismetArrayLibrary.Array_LastIndex",

        // ── BaseCardObject：getter 类（实测由 -Mode=Dump 确认）──
        "BaseCardObject.IsOrder",
        "BaseCardObject.IsUnit",
        "BaseCardObject.getTotalKreditCost",
        "BaseCardObject.GetOppositeSide",
    };

    /// <summary>
    /// KismetArrayLibrary 的<b>修改类</b>函数是 impure（要执行引脚），必须走语句。
    /// 与上面 Pure 集合互补 —— 路线 A 完全做不到这些，是本桥接器的关键增益。
    /// </summary>
    private static readonly HashSet<string> KnownImpure = new(StringComparer.Ordinal)
    {
        "KismetArrayLibrary.Array_Add",
        "KismetArrayLibrary.Array_AddUnique",
        "KismetArrayLibrary.Array_Insert",
        "KismetArrayLibrary.Array_Remove",
        "KismetArrayLibrary.Array_RemoveItem",
        "KismetArrayLibrary.Array_Clear",
        "KismetArrayLibrary.Array_Resize",
        "KismetArrayLibrary.Array_Append",
        "KismetArrayLibrary.Array_Set",
        "KismetArrayLibrary.Array_Shuffle",
        "KismetArrayLibrary.Array_Swap",

        // CardFunctionsStub 上的动作类 —— 全部 impure
        "CardFunctionsStub.GotchaTriggered",
        "CardFunctionsStub.ConvertCard",
        "CardFunctionsStub.AddToBattleLog",
        "CardFunctionsStub.DamageCard",
        "CardFunctionsStub.RandomIntFromRangeWithStream",
        "CardFunctionsStub.GetAllActiveStaticCards",
        "CardFunctionsStub.DrawCard",
        "CardFunctionsStub.DestroyCard",
        "CardFunctionsStub.HealCard",
    };

    /// <summary>该调用是否应落成<b>表达式</b>（pure）。</summary>
    public static bool IsPure(string ownerClass, string function) =>
        Pure.Contains(Key(ownerClass, function));

    /// <summary>该调用是否已知必须落成<b>语句</b>（impure）。</summary>
    public static bool IsKnownImpure(string ownerClass, string function) =>
        KnownImpure.Contains(Key(ownerClass, function));

    /// <summary>是否在表里（任一方向）。</summary>
    public static bool IsKnown(string ownerClass, string function) =>
        IsPure(ownerClass, function) || IsKnownImpure(ownerClass, function);

    /// <summary>注册一个已知的 pure 函数（供 -Mode=Dump 回填）。</summary>
    public static void RegisterPure(string ownerClass, string function) =>
        Pure.Add(Key(ownerClass, function));

    /// <summary>注册一个已知的 impure 函数。</summary>
    public static void RegisterImpure(string ownerClass, string function) =>
        KnownImpure.Add(Key(ownerClass, function));

    /// <summary>
    /// 从 IR 的 call 推断纯度。表里没有时：
    ///   * 目标是 KismetArrayLibrary → 按命名约定猜（Get/Length/IsValid 等一律 pure）
    ///   * 其余按 impure（偏向语句 —— 语句形态对 pure 会报明确错误，
    ///     而表达式形态对 impure 的报错更难定位）
    /// </summary>
    public static bool InferPure(BlueprintCallStatement call)
    {
        if (IsPure(call.OwnerClass, call.FunctionName)) return true;
        if (IsKnownImpure(call.OwnerClass, call.FunctionName)) return false;

        // KismetArrayLibrary 的取值类按前缀判定
        if (call.OwnerClass == "KismetArrayLibrary")
        {
            var n = call.FunctionName;
            if (n.StartsWith("Array_Length", StringComparison.Ordinal)
                || n.StartsWith("Array_Get", StringComparison.Ordinal)
                || n.StartsWith("Array_IsValidIndex", StringComparison.Ordinal)
                || n.StartsWith("Array_Contains", StringComparison.Ordinal)
                || n.StartsWith("Array_Find", StringComparison.Ordinal)
                || n.StartsWith("Array_LastIndex", StringComparison.Ordinal))
                return true;
            return false;
        }

        // KismetMathLibrary 全部 pure
        if (call.OwnerClass == "KismetMathLibrary") return true;

        return false;
    }

    private static string Key(string ownerClass, string fn) => $"{ownerClass}.{fn}";
}
