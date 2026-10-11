using KardsCardCompiler.Core;

namespace KardsDiy.SovietDefector;

/// <summary>
/// 苏联 1K 1/1 炮兵 —— 「变节炮组」
///
/// 规则：攻击时，友方总部也受到等同于本次攻击力的伤害；随后本单位的控制权移交给对方。
///
/// 实现要点（都经过对游戏本体 BP_CardFunctions 反编译结果的核对）：
///   * 挂 <c>OnAfterAttack</c>。游戏本体的 ExecuteOnAfterAttackEvents 会
///     对未压制的攻击者调用 attacker.OnAfterAttack(defender, afterShockAttack, attackCost)，
///     所以实现这个事件就能拿到"我打完了一下"的时机。
///   * 伤害量取 <c>Self.getTotalAttack()</c>（含增益），而不是裸 attack 字段。
///   * 友方总部用 <c>Self.side</c> + <c>GetLocationCardBySide</c> 取，
///     不依赖 GetClientSide（那是本地客户端视角，联机下不可靠）。
///   * 控制权移交用 <c>TakeControlOfEnemyUnit</c>。反编译显示它内部就是
///     <c>ChangeUnitOwnership(card.CardID, instigatorID, card.side, card.GetOppositeSide(), ...)</c>，
///     也就是"把这张卡翻到它的对侧"——正好是"移交给对方"。
/// </summary>
public sealed class SovietDefectorArtilleryPack : ICardPackDefinition, ICardBlueprintDefinition
{
    private const string KardsPackage = "/Script/kards";
    private const string BaseCardClass = "BaseCardObject";
    private const string CardFunctionsClass = "CardFunctionsStub";

    private static readonly BlueprintTypeRef BaseCardType =
        BlueprintTypeRef.Object(KardsPackage, BaseCardClass);

    private static readonly BlueprintTypeRef CardFunctionsType =
        BlueprintTypeRef.Object(KardsPackage, CardFunctionsClass);

    private static readonly BlueprintTypeRef SideType =
        BlueprintTypeRef.Enum(KardsPackage, "ESideEnum");

    public IReadOnlyCollection<CardDefinition> DefineCards() => new[]
    {
        CardDefinition.Unit(
                id: 910101,
                name: "card_unit_sov_defector_artillery",
                faction: Faction.Soviet,
                cost: 1,
                attack: 1,
                defense: 1,
                range: 1,
                unitType: CardUnitType.Artillery)
            .WithDeckCodeId("V9")
            .Text(
                title: "变节炮组",
                rules: "攻击时：友方总部也受到等同于本次攻击力的伤害；随后本单位的控制权移交给对方。",
                flavor: "「命令是向那边开炮……那边。」")
            .FromSet(CardSet.Basic)
            .RarityOf(CardRarity.Common)
    };

    public IReadOnlyList<BlueprintFunctionIr> DefineBlueprintFunctions(CardIr card)
    {
        // 事件签名参数（游戏派发时传入；本卡不使用，但必须声明）
        var defenderCard = new BlueprintVariableExpression("defenderCard", BaseCardType);
        var wasShockAttack = new BlueprintVariableExpression("wasShockAttack", BlueprintTypeRef.Boolean);
        var attackCost = new BlueprintVariableExpression("attackCost", BlueprintTypeRef.Int32);

        // 局部变量
        var mySide = new BlueprintVariableExpression("mySide", SideType);
        var friendlyHq = new BlueprintVariableExpression("friendlyHq", BaseCardType);
        var friendlyHqId = new BlueprintVariableExpression("friendlyHqId", BlueprintTypeRef.Int32);
        var damage = new BlueprintVariableExpression("damage", BlueprintTypeRef.Int32);
        var targetDestroyed = new BlueprintVariableExpression("targetDestroyed", BlueprintTypeRef.Boolean);

        // Self 及其属性
        var selfRef = new BlueprintSelfExpression(BaseCardType);
        var selfId = new BlueprintSelfPropertyExpression(
            KardsPackage, BaseCardClass, "CardID", BlueprintTypeRef.Int32);
        var selfSide = new BlueprintSelfPropertyExpression(
            KardsPackage, BaseCardClass, "side", SideType);
        var cardFunction = new BlueprintSelfPropertyExpression(
            KardsPackage, BaseCardClass, "cardFunction", CardFunctionsType);

        var body = new List<BlueprintStatement>
        {
            // 1) damage = Self.getTotalAttack()   —— 含增益的总攻击力
            new BlueprintCallStatement(
                BlueprintCallDispatch.SelfFinal, KardsPackage, BaseCardClass,
                "getTotalAttack",
                new BlueprintExpression[] { damage }),

            // 2) mySide = Self.side
            new BlueprintAssignStatement(mySide, selfSide),

            // 3) cardFunction.GetLocationCardBySide(out friendlyHq, out friendlyHqId, mySide)
            new BlueprintCallStatement(
                BlueprintCallDispatch.ObjectVirtual, KardsPackage, CardFunctionsClass,
                "GetLocationCardBySide",
                new BlueprintExpression[] { friendlyHq, friendlyHqId, mySide },
                cardFunction),

            // 4) cardFunction.DamageCard(friendlyHq, damage, Self.CardID,
            //                            isRedirected=false, fromFight=false,
            //                            isFightDefenderDamage=false, out targetDestroyed)
            new BlueprintCallStatement(
                BlueprintCallDispatch.ObjectVirtual, KardsPackage, CardFunctionsClass,
                "DamageCard",
                new BlueprintExpression[]
                {
                    friendlyHq,
                    damage,
                    selfId,
                    new BlueprintBoolExpression(false),
                    new BlueprintBoolExpression(false),
                    new BlueprintBoolExpression(false),
                    targetDestroyed
                },
                cardFunction),

            // 5) cardFunction.TakeControlOfEnemyUnit(Self, Self.CardID)
            //    内部 = ChangeUnitOwnership(Self.CardID, instigatorID, Self.side,
            //                               Self.GetOppositeSide(), ...)
            //    即：把这张卡翻到对侧 —— 控制权移交给对方。
            new BlueprintCallStatement(
                BlueprintCallDispatch.ObjectVirtual, KardsPackage, CardFunctionsClass,
                "TakeControlOfEnemyUnit",
                new BlueprintExpression[] { selfRef, selfId },
                cardFunction),
        };

        return new[]
        {
            new BlueprintFunctionIr(
                Name: "OnAfterAttack",
                Variables: new[]
                {
                    new BlueprintVariableDefinition("defenderCard", BaseCardType, BlueprintVariableRole.Input),
                    new BlueprintVariableDefinition("wasShockAttack", BlueprintTypeRef.Boolean, BlueprintVariableRole.Input),
                    new BlueprintVariableDefinition("attackCost", BlueprintTypeRef.Int32, BlueprintVariableRole.Input),
                    new BlueprintVariableDefinition("mySide", SideType),
                    new BlueprintVariableDefinition("friendlyHq", BaseCardType),
                    new BlueprintVariableDefinition("friendlyHqId", BlueprintTypeRef.Int32),
                    new BlueprintVariableDefinition("damage", BlueprintTypeRef.Int32),
                    new BlueprintVariableDefinition("targetDestroyed", BlueprintTypeRef.Boolean),
                },
                Body: body,
                OverrideOwnerPackage: KardsPackage,
                OverrideOwnerClass: BaseCardClass)
        };
    }
}
