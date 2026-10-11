using KardsCardCompiler.Core;
using KardsSpecBridge;

namespace KardsDiy.BlitzArtillery1011;

/// <summary>
/// 苏联 1011 —— 「闪击炮兵」
///
/// 卡面：闪击 · 炮兵 · 攻击距离 2
///       攻击造成伤害时，友方总部也受到同样伤害，然后控制权转交给对方。
///
/// ── 本文件演示「用 C# 写卡牌逻辑」────────────────────────────────
/// 逻辑不再手搓蓝图 IR 节点树，而是用 <see cref="Logic"/> / <see cref="Spec"/>
/// 这套 DSL 直接写<b>逻辑</b>，底层自动生成 spec 节点。
///
/// 表达能力 = 插件上限 = 100%（含 IR 写不出的 property / cast / arrayMake /
/// k2 原生节点逃生舱）。
///
/// ── API 均已用 -Mode=Dump 反射核对 ────────────────────────────────
///   触发  OnOtherCardDealDamage(cardDealingDamage, toCard, Damage,
///                               isCombatDamage, CounterDamage, isRedirected)
///   取总部 GetLocationCardBySide(side) → out card, out locationCardID
///   伤害   DamageCard(card, amount, damagerCardID, isRedirected,
///                     fromFight, isFightDefenderDamage) → out targetDestroyed
///   转交   TakeControlOfEnemyUnit(card, instigatorID)
/// </summary>
public sealed class BlitzArtillery1011Pack : ICardPackDefinition, ISpecBlueprintDefinition
{
    public const string TriggerName = "OnOtherCardDealDamage";
    private const string BaseCardObject = "/Script/kards.BaseCardObject";

    // ── 卡面数据 ────────────────────────────────────────────────────

    public IReadOnlyCollection<CardDefinition> DefineCards() => new[]
    {
        CardDefinition.Unit(
                id: 1011,
                name: "card_unit_blitz_artillery_1011",
                faction: Faction.Soviet,
                cost: 4,
                attack: 3,
                defense: 3,
                range: 2,                          // ★ 攻击距离 2
                unitType: CardUnitType.Artillery)  // ★ 炮兵
            .Text(
                title: "闪击炮兵",
                rules: "闪击。攻击造成伤害时，友方总部也受到同样伤害，然后本单位的控制权转交给对方。",
                flavor: "「炮口对准谁，由不得你。」")
            .FromSet(CardSet.Basic)
            .RarityOf(CardRarity.Common)
    };

    // ── 卡牌逻辑（C# DSL）──────────────────────────────────────────

    public SpecGraphSet DefineSpecGraphs(CardIr card) => new()
    {
        // 工作变量：(名字, spec 引脚类型串)
        //
        // 变量名直接带 b_ 前缀 —— 原生 spec 节点是按名字直接引用的，
        // 且必须避开父类成员名（isOrder 会撞 BaseCardObject::IsOrder）。
        Variables = new[]
        {
            ("b_side",      "enum:/Script/kards.ESideEnum"),
            ("b_hq",        "object:/Script/kards.BaseCardObject"),
            ("b_hqId",      "int"),
            ("b_destroyed", "bool"),
        },

        Graphs = new[]
        {
            Logic.EventGraph(
                TriggerName,
                Logic.EventBody(

                    // ① 取本卡所属方 → b_side
                    Spec.Set("b_side", Spec.SelfSide()),

                    // ② 三个前置条件全成立才继续：
                    //      cardDealingDamage == Self   只认本卡造成的伤害
                    //      isCombatDamage              只认"攻击"（排除技能/反制）
                    //      Damage > 0                  0 伤害不触发
                    Spec.If(
                        Logic.All(
                            Logic.SelfEquals("cardDealingDamage"),
                            Logic.ParamTrue("isCombatDamage"),
                            Logic.ParamPositive("Damage")),
                        Logic.Build(

                            // ③ 取友方总部那张卡 → b_hq / b_hqId
                            //    （两个出参，桥接器拆成兄弟语句）
                            Logic.FetchHq(Spec.Member("b_side"), "b_hq", "b_hqId"),

                            // ④ 友方总部吃同样伤害
                            Logic.DamageFriendlyHq("Damage", "b_destroyed"),

                            // ⑤ 控制权转交给对方
                            Logic.GiveSelfToOpponent(),

                            // ⑥ 写战斗日志（便于游戏内观察）
                            Logic.BattleLog()))),

                owner: BaseCardObject),
        },
    };
}
