using System.Text.Json.Nodes;
using KardsCardCompiler.Core;

namespace KardsSpecBridge;

/// <summary>
/// <b>原生 spec 卡包</b>接口 —— 用 C# 直接构造 spec 节点写卡。
///
/// 与 <see cref="ICardBlueprintDefinition"/> 的区别：
///   * ICardBlueprintDefinition 产出蓝图 IR，表达力受限
///     （无 cast / property / arrayMake / k2 原生节点）
///   * 本接口产出 <b>spec 原生节点</b>，表达力 = 插件上限 = 100%
///
/// 两者可组合：卡面数据仍用 <see cref="ICardPackDefinition"/> 的
/// <see cref="CardDefinition"/>，逻辑用本接口写。
///
/// 示例：
/// <code>
/// public sealed class MyPack : ICardPackDefinition, ISpecBlueprintDefinition
/// {
///     public IReadOnlyCollection&lt;CardDefinition&gt; DefineCards() => ...;
///
///     public SpecGraphSet DefineSpecGraphs(CardIr card) => new()
///     {
///         Variables = new[]
///         {
///             ("b_hq", "object:/Script/kards.BaseCardObject"),
///             ("b_hqId", "int"),
///             ("b_destroyed", "bool"),
///         },
///         Graphs = new[]
///         {
///             Logic.EventGraph(
///                 Logic.Trigger.OnOtherCardDealDamage,
///                 Logic.EventBody(
///                     Spec.If(Logic.All(
///                             Logic.SelfEquals("cardDealingDamage"),
///                             Logic.ParamTrue("isCombatDamage"),
///                             Logic.ParamPositive("Damage")),
///                         Logic.Build(
///                             Logic.FetchHq(Spec.SelfSide(), "b_hq", "b_hqId"),
///                             Logic.DamageFriendlyHq("Damage", "b_destroyed"),
///                             Logic.GiveSelfToOpponent()))),
///                 owner: "/Script/kards.BaseCardObject"),
///         },
///     };
/// }
/// </code>
/// </summary>
public interface ISpecBlueprintDefinition
{
    /// <summary>产出本卡的原生 spec 图（变量声明 + 图）。</summary>
    SpecGraphSet DefineSpecGraphs(CardIr card);
}

/// <summary>原生 spec 图集合。</summary>
public sealed class SpecGraphSet
{
    /// <summary>成员变量：(名字, 引脚类型串)。</summary>
    public IReadOnlyList<(string Name, string PinType)> Variables { get; init; } =
        Array.Empty<(string, string)>();

    /// <summary>图：由 <see cref="Logic"/> / <see cref="Spec"/> 构造。</summary>
    public IReadOnlyList<JsonObject> Graphs { get; init; } =
        Array.Empty<JsonObject>();

    /// <summary>CDO 附加默认值：(名字, 字面量类型, 值)。</summary>
    public IReadOnlyList<(string Name, string LiteralType, object? Value)> ExtraDefaults { get; init; } =
        Array.Empty<(string, string, object?)>();
}
