using KardsCardCompiler.Core;
using KardsSpecBridge;

namespace KardsDiy.DslProof;

/// <summary>
/// DSL 能力验证卡 —— 专门使用 IR <b>写不出</b>的三类能力，
/// 证明「C# 写逻辑 100% 覆盖」成立。
///
/// 覆盖的缺口：
///   G1 cast          —— 动态 Cast / 向上转型
///   G2 property      —— 读别的对象的属性
///   G3 arrayMake     —— 对象数组 / int 数组构造
///   G4 foreach       —— ForEachLoop 宏节点
///
/// 效果（随便编的，重点是节点类型）：
///   打出时：遍历己方前线所有单位，读它们的 attack 属性，
///          累加后对敌方总部造成等量伤害；
///          并把它们的 CardID 收集进 int 数组写战斗日志。
/// </summary>
public sealed class DslProofPack : ICardPackDefinition, ISpecBlueprintBlueprintAlias
{
    private const string BaseCardObject = "/Script/kards.BaseCardObject";

    public IReadOnlyCollection<CardDefinition> DefineCards() => new[]
    {
        CardDefinition.Event(id: 9101, name: "card_event_dsl_proof",
                faction: Faction.Soviet, cost: 3, deckCodeId: "ZZ")
            .Text("DSL 能力验证", "遍历己方前线单位，按总攻击力对敌方总部造成伤害。")
            .FromSet(CardSet.Basic)
            .RarityOf(CardRarity.Common)
    };

    public SpecGraphSet DefineSpecGraphs(CardIr card) => new()
    {
        Variables = new[]
        {
            ("b_units",   "array:object:/Script/kards.BaseCardObject"),  // ★ 对象数组
            ("b_unit",    "object:/Script/kards.BaseCardObject"),
            ("b_total",   "int"),
            ("b_atk",     "int"),
            ("b_ids",     "array:int"),                                  // ★ int 数组
            ("b_logged",  "int"),
        },

        Graphs = new[]
        {
            Logic.EventGraph(
                Logic.Trigger.OnOtherCardPlayedFromHand,
                Logic.EventBody(

                    // ① 取己方前线单位 → 对象数组（IR 做不到）
                    Spec.CallStmt("/Script/kards.CardFunctionsStub", "GetCardsInFrontlineBySide",
                        new Dictionary<string, System.Text.Json.Nodes.JsonObject>
                        {
                            ["side"] = Spec.SelfSide(),
                            ["includeCovertCards"] = Spec.Bool(false),
                        },
                        new Dictionary<string, string> { ["Cards"] = "b_units" },
                        target: Spec.Member("cardFunction")),

                    // ② 累加器清零
                    Spec.Set("b_total", Spec.Int(0)),

                    // ③ foreach 宏节点遍历对象数组（IR 只有 while）
                    Spec.ForEach(
                        Spec.Member("b_units"),
                        Logic.Build(
                            // 读"别的对象"的属性 attack —— G2，IR 做不到
                            Spec.Set("b_atk", Spec.Property(
                                Spec.Member("b_unit"), BaseCardObject, "attack")),
                            // 累加
                            Spec.Set("b_total", Spec.IntAdd(Spec.Member("b_total"), Spec.Member("b_atk")))),
                        outs: new Dictionary<string, string>
                        {
                            ["Array Element"] = "b_unit",
                        }),

                    // ④ 取敌方阵营（实测 GetOpponentSide 无入参，单出参 OutOpponentSide）
                    Spec.CallStmt("/Script/kards.CardFunctionsStub", "GetOpponentSide",
                        null,
                        new Dictionary<string, string>
                        {
                            ["OutOpponentSide"] = "b_oppSide",
                        },
                        target: Spec.Member("cardFunction")),

                    // ⑤ 累计伤害 > 0 才打
                    Spec.If(
                        Logic.MemberAtLeast("b_total", 1),
                        Logic.Build(
                            Logic.BattleLog()))),

                owner: BaseCardObject),
        },
    };
}

/// <summary>别名 —— 本文件里让验证卡同时暴露 ISpecBlueprintDefinition。</summary>
public interface ISpecBlueprintBlueprintAlias : ISpecBlueprintDefinition;
