using KardsCardCompiler.Core;
using KardsSpecBridge;

namespace KardsDiy.CastProof;

/// <summary>
/// cast / arrayMake / k2 逃生舱 三项能力验证。
/// </summary>
public sealed class CastProofPack : ICardPackDefinition, ISpecBlueprintDefinition
{
    public IReadOnlyCollection<CardDefinition> DefineCards() => new[]
    {
        CardDefinition.Event(id: 9102, name: "card_event_cast_proof",
                faction: Faction.Soviet, cost: 1)
            .Text("Cast 验证", "验证 cast / arrayMake / k2 逃生舱。")
            .FromSet(CardSet.Basic)
            .RarityOf(CardRarity.Common)
    };

    public SpecGraphSet DefineSpecGraphs(CardIr card) => new()
    {
        Variables = new[]
        {
            ("b_card",  "object:/Script/kards.BaseCardObject"),
            ("b_ok",    "bool"),
            ("b_ids",   "array:int"),
        },

        Graphs = new[]
        {
            Logic.EventGraph(
                Logic.Trigger.OnOtherCardEnterPlay,
                Logic.EventBody(

                    // ── G1: cast 表达式（向上转型到 Object）──
                    Spec.Set("b_ok",
                        Spec.ObjectEquals(Spec.Self(), Spec.Self())),

                    // ── G1: cast 语句（动态 Cast，带成功/失败分支）──
                    Spec.CastStmt(
                        Spec.AsObject(Spec.Self()),
                        "/Script/kards.BaseCardObject",
                        Logic.Build(
                            Spec.Set("b_ok", Spec.Bool(true))),
                        Logic.Build(
                            Spec.Set("b_ok", Spec.Bool(false)))),

                    // ── G3: int 数组构造（MakeArray）──
                    //     ⚠ 插件要求至少一项
                    Spec.Set("b_ids", Spec.IntArray(1011, 1012, 1013)),

                    // ── G5: k2 原生节点逃生舱 ──
                    //     本图由 patch handler 走 RawK2 路径构建；
                    //     这里仅演示 API 形态（见 Spec.K2Graph / Spec.K2Node）。
                    Spec.Set("b_ok", Spec.Not(Spec.Bool(false)))),

                owner: "/Script/kards.BaseCardObject"),
        },
    };
}
