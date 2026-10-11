using System.Text.Json.Nodes;

namespace KardsSpecBridge;

/// <summary>
/// <b>C# 卡牌逻辑 DSL</b> —— 用接近自然语言的 C# 写卡，底层生成 spec 节点。
///
/// 设计目标：<b>100% 覆盖所有卡</b>，同时让作者写的是"逻辑"而不是"节点树"。
///
/// <code>
/// public IReadOnlyList&lt;JsonObject&gt; Build() => new[]
/// {
///     Logic.When(Trigger.OnOtherCardDealDamage)
///          .WithParams("cardDealingDamage", "toCard", "Damage",
///                      "isCombatDamage", "CounterDamage", "isRedirected")
///          .OnlyIf(Logic.SelfEquals("cardDealingDamage"))
///          .OnlyIf(Logic.ParamTrue("isCombatDamage"))
///          .OnlyIf(Logic.ParamPositive("Damage"))
///          .Then(
///              Logic.DamageFriendlyHq("Damage"),
///              Logic.GiveSelfToOpponent())
///          .AsEvent(owner: "/Script/kards.BaseCardObject")
/// };
/// </code>
///
/// 需要 IR 表达不了的能力时，直接用 <see cref="Spec"/> 构造原生节点，
/// 两者可在同一个图里混用。
/// </summary>
public static class Logic
{
    // ═══════════════════════════════════════════════════ 条件构造

    /// <summary>入参 == Self。</summary>
    public static JsonObject SelfEquals(string paramName) =>
        Spec.ObjectEquals(Spec.Param(paramName), Spec.Self());

    /// <summary>入参 != Self。</summary>
    public static JsonObject SelfNotEquals(string paramName) =>
        Spec.ObjectNotEquals(Spec.Param(paramName), Spec.Self());

    /// <summary>布尔入参为 true。</summary>
    public static JsonObject ParamTrue(string paramName) => Spec.Param(paramName);

    /// <summary>布尔入参为 false。</summary>
    public static JsonObject ParamFalse(string paramName) => Spec.Not(Spec.Param(paramName));

    /// <summary>入参 &gt; 0。</summary>
    public static JsonObject ParamPositive(string paramName) =>
        Spec.IntGreater(Spec.Param(paramName), Spec.Int(0));

    /// <summary>入参 &gt;= 某值。</summary>
    public static JsonObject ParamAtLeast(string paramName, int value) =>
        Spec.IntGreaterEqual(Spec.Param(paramName), Spec.Int(value));

    /// <summary>入参 == 某值。</summary>
    public static JsonObject ParamEqualsInt(string paramName, int value) =>
        Spec.IntEquals(Spec.Param(paramName), Spec.Int(value));

    /// <summary>两个入参相等。</summary>
    public static JsonObject ParamEquals(string a, string b) =>
        Spec.ObjectEquals(Spec.Param(a), Spec.Param(b));

    /// <summary>成员变量为 true。</summary>
    public static JsonObject MemberTrue(string memberName) => Spec.Member(memberName);

    /// <summary>成员 != 某值。</summary>
    public static JsonObject MemberAtLeast(string memberName, int value) =>
        Spec.IntGreaterEqual(Spec.Member(memberName), Spec.Int(value));

    /// <summary>读别的对象的属性（G2：IR 做不到）。</summary>
    public static JsonObject CardProperty(string paramName, string ownerClass, string prop) =>
        Spec.Property(Spec.Param(paramName), ownerClass, prop);

    /// <summary>所有条件取与。</summary>
    public static JsonObject All(params JsonObject[] conditions) =>
        conditions.Length switch
        {
            0 => Spec.Bool(true),
            1 => conditions[0],
            _ => conditions.Aggregate(Spec.BoolAnd)
        };

    /// <summary>任一条件成立。</summary>
    public static JsonObject Any(params JsonObject[] conditions) =>
        conditions.Length switch
        {
            0 => Spec.Bool(false),
            1 => conditions[0],
            _ => conditions.Aggregate(Spec.BoolOr)
        };

    // ═══════════════════════════════════════════════════ 动作封装

    /// <summary>对"友方总部"造成与某入参等量的伤害。</summary>
    public static JsonObject DamageFriendlyHq(string amountParam, string? outDestroyedVar = null) =>
        DamageHq(Spec.Member("b_side"), amountParam, outDestroyedVar);

    /// <summary>对指定方总部造成伤害。</summary>
    public static JsonObject DamageHq(JsonObject sideExpr, string amountParam,
        string? outDestroyedVar = null)
    {
        return Spec.CallStmt("/Script/kards.CardFunctionsStub", "DamageCard",
            new Dictionary<string, JsonObject>
            {
                ["card"] = Spec.Member("b_hq"),
                ["amount"] = Spec.Param(amountParam),
                ["damagerCardID"] = Spec.SelfCardId(),
                ["isRedirected"] = Spec.Bool(false),
                ["fromFight"] = Spec.Bool(false),
                ["isFightDefenderDamage"] = Spec.Bool(false),
            },
            outDestroyedVar is null
                ? null
                : new Dictionary<string, string> { ["targetDestroyed"] = outDestroyedVar },
            target: Spec.Member("cardFunction"));
    }

    /// <summary>取某一方总部（写入两个成员变量 —— 桥接器会拆平）。</summary>
    public static JsonObject FetchHq(JsonObject sideExpr, string outCardVar, string outIdVar) =>
        Spec.CallStmt("/Script/kards.CardFunctionsStub", "GetLocationCardBySide",
            new Dictionary<string, JsonObject> { ["side"] = sideExpr },
            new Dictionary<string, string>
            {
                ["card"] = outCardVar,
                ["locationCardID"] = outIdVar,
            },
            target: Spec.Member("cardFunction"));

    /// <summary>把本卡的控制权交给对方。</summary>
    public static JsonObject GiveSelfToOpponent() =>
        Spec.TakeControlOfEnemyUnit(Spec.Self(), Spec.SelfCardId());

    /// <summary>把指定卡的控制权交给对方。</summary>
    public static JsonObject GiveControlToOpponent(string paramName) =>
        Spec.TakeControlOfEnemyUnit(Spec.Param(paramName), Spec.SelfCardId());

    /// <summary>对某张入参卡造成固定伤害。</summary>
    public static JsonObject DamageParam(string paramCard, int amount,
        string? outDestroyedVar = null) =>
        Spec.CallStmt("/Script/kards.CardFunctionsStub", "DamageCard",
            new Dictionary<string, JsonObject>
            {
                ["card"] = Spec.Param(paramCard),
                ["amount"] = Spec.Int(amount),
                ["damagerCardID"] = Spec.SelfCardId(),
                ["isRedirected"] = Spec.Bool(false),
                ["fromFight"] = Spec.Bool(false),
                ["isFightDefenderDamage"] = Spec.Bool(false),
            },
            outDestroyedVar is null
                ? null
                : new Dictionary<string, string> { ["targetDestroyed"] = outDestroyedVar },
            target: Spec.Member("cardFunction"));

    /// <summary>抽牌。</summary>
    public static JsonObject DrawCards(int count) =>
        Spec.CallStmt("/Script/kards.CardFunctionsStub", "DrawCardsFromDeckBySide",
            new Dictionary<string, JsonObject>
            {
                ["instigatorID"] = Spec.SelfCardId(),
                ["side"] = Spec.SelfSide(),
                ["NumCards"] = Spec.Int(count),
                ["cardSeen"] = Spec.Bool(true),
                ["OpponentDraw"] = Spec.Bool(false),
                ["drawDelay"] = Spec.Int(0),
            },
            null,
            target: Spec.Member("cardFunction"));

    /// <summary>给某一方加 K 币。</summary>
    public static JsonObject GiveKredits(int amount) =>
        Spec.CallStmt("/Script/kards.CardFunctionsStub", "GiveKreditsBySide",
            new Dictionary<string, JsonObject>
            {
                ["side"] = Spec.SelfSide(),
                ["kredits"] = Spec.Int(amount),
                ["instigatorID"] = Spec.SelfCardId(),
            },
            null,
            target: Spec.Member("cardFunction"));

    /// <summary>写战斗日志。</summary>
    public static JsonObject BattleLog() =>
        Spec.CallStmt("/Script/kards.CardFunctionsStub", "AddToBattleLog",
            new Dictionary<string, JsonObject>
            {
                ["CardID"] = Spec.SelfCardId(),
                ["instigatorID"] = Spec.SelfCardId(),
                ["cardHidden"] = Spec.Bool(false),
                ["showAnimation"] = Spec.Bool(true),
            },
            null,
            target: Spec.Member("cardFunction"));

    /// <summary>销毁某张入参卡。</summary>
    public static JsonObject DestroyParam(string paramCard) =>
        Spec.CallStmt("/Script/kards.CardFunctionsStub", "DestroyCard",
            new Dictionary<string, JsonObject>
            {
                ["card"] = Spec.Param(paramCard),
                ["destroyerCard"] = Spec.Self(),
            },
            null,
            target: Spec.Member("cardFunction"));

    // ═══════════════════════════════════════════════════ 触发事件常量

    /// <summary>ERegisteredCardFunction —— 触发点（用于 usedTriggers）。</summary>
    /// <remarks>
    /// ⚠ 全部取自 <c>-Mode=DumpEnum /Script/kards.ERegisteredCardFunction</c> 实测值。
    /// 注意<b>没有</b> <c>OnPlayedFromHand</c>，只有 <c>OnOtherCardPlayedFromHand</c>
    /// （即"<b>别的</b>卡被打出"）。本卡自己被使用时走的是
    /// <c>OnDeploymentEffectTriggered</c> / <c>OnOtherCardEnterPlay</c>。
    /// </remarks>
    public static class Trigger
    {
        // ── 伤害相关 ──
        public const string OnOtherCardDealDamage = "OnOtherCardDealDamage";
        public const string OnOtherCardReceiveDamage = "OnOtherCardReceiveDamage";
        public const string OnOtherCardDealDamageAddDamage = "OnOtherCardDealDamageAddDamage";
        public const string OnOtherCardDealDamageAddDamageAfterCalc = "OnOtherCardDealDamageAddDamageAfterCalc";
        public const string OnFatigueDamage = "OnFatigueDamage";

        // ── 攻击相关 ──
        public const string OnOtherCardAttacks = "OnOtherCardAttacks";
        public const string OnAfterOtherCardAttacks = "OnAfterOtherCardAttacks";
        public const string OnBeforeOtherCardAttacks = "OnBeforeOtherCardAttacks";
        public const string OnOtherCardAttackSwitchTarget = "OnOtherCardAttackSwitchTarget";
        public const string OnOtherCardSurvivedCombat = "OnOtherCardSurvivedCombat";

        // ── 打出 / 入场 ──
        public const string OnOtherCardPlayedFromHand = "OnOtherCardPlayedFromHand";
        public const string OnBeforeOtherCardPlayedFromHand = "OnBeforeOtherCardPlayedFromHand";
        public const string OnOtherCardEnterPlay = "OnOtherCardEnterPlay";
        public const string OnDeploymentEffectTriggered = "OnDeploymentEffectTriggered";
        public const string OnOtherCovertCardPlayedFromHand = "OnOtherCovertCardPlayedFromHand";

        // ── 死亡 / 离场 ──
        public const string OnOtherCardDestroyed = "OnOtherCardDestroyed";
        public const string OnBeforeOtherCardDestroyed = "OnBeforeOtherCardDestroyed";
        public const string OnDestructionEffectTriggered = "OnDestructionEffectTriggered";
        public const string OnOtherCardLeaveBoardOrOwner = "OnOtherCardLeaveBoardOrOwner";
        public const string OnAfterOtherCardLeaveBoardOrOwner = "OnAfterOtherCardLeaveBoardOrOwner";

        // ── 回合 ──
        public const string OnStartofTurn = "OnStartofTurn";
        public const string OnEndOfTurn = "OnEndOfTurn";
        public const string OnBeforeStartOfTurn = "OnBeforeStartOfTurn";
        public const string OnStartOfGame = "OnStartOfGame";

        // ── 属性变化 ──
        public const string OnOtherCardBlitzChange = "OnOtherCardBlitzChange";
        public const string OnOtherCardConverted = "OnOtherCardConverted";
        public const string OnOtherCardSuppressed = "OnOtherCardSuppressed";
        public const string OnOtherCardBecomingVeteran = "OnOtherCardBecomingVeteran";
        public const string OnOtherCardKreditCostChanged = "OnOtherCardKreditCostChanged";
        public const string OnAfterOtherCardChangeAttack = "OnAfterOtherCardChangeAttack";
        public const string OnAfterOtherCardGainDefense = "OnAfterOtherCardGainDefense";
        public const string OnBeforeOtherCardGainDefense = "OnBeforeOtherCardGainDefense";
        public const string OnOtherCardLoseSmokescreen = "OnOtherCardLoseSmokescreen";
        public const string OnOtherCardReset = "OnOtherCardReset";

        // ── 位置 / 移动 ──
        public const string OnOtherCardLocationMoved = "OnOtherCardLocationMoved";
        public const string OnOtherCardMoveToFrontline = "OnOtherCardMoveToFrontline";
        public const string OnOtherCardMoveFromFrontline = "OnOtherCardMoveFromFrontline";
        public const string OnOtherCardMovedToLocationInHand = "OnOtherCardMovedToLocationInHand";
        public const string OnFrontlineOwnershipChange = "OnFrontlineOwnershipChange";
        public const string OnOtherCardRetreat = "OnOtherCardRetreat";

        // ── 牌库 / 手牌 ──
        public const string OnOtherCardDrawnFromDeck = "OnOtherCardDrawnFromDeck";
        public const string OnDeckShuffled = "OnDeckShuffled";
        public const string OnAfterDeckChanged = "OnAfterDeckChanged";
        public const string OnOtherCardSpawnedInHand = "OnOtherCardSpawnedInHand";
        public const string OnOtherCardDiscarded = "OnOtherCardDiscarded";
        public const string OnOtherCardSalvaged = "OnOtherCardSalvaged";
        public const string OnOtherCardDeveloped = "OnOtherCardDeveloped";

        // ── 其它 ──
        public const string OnIntelTriggered = "OnIntelTriggered";
        public const string OnCounterMeasureTriggered = "OnCounterMeasureTriggered";
        public const string OnOtherUnitPinned = "OnOtherUnitPinned";
        public const string OnOtherUnitUnPinned = "OnOtherUnitUnPinned";
        public const string OnOtherCardRevealed = "OnOtherCardRevealed";
        public const string OnOtherCardForecasted = "OnOtherCardForecasted";
        public const string CanOtherCardBeTargetted = "CanOtherCardBeTargetted";
    }

    /// <summary>拼一张事件图的 body。</summary>
    public static JsonArray EventBody(params JsonObject[] statements) => Build(statements);

    /// <summary>拼一张函数图的 body。</summary>
    public static JsonArray FunctionBody(params JsonObject[] statements) => Build(statements);

    /// <summary>把若干语句收成一个数组（用于 if 的 then/else）。</summary>
    public static JsonArray Build(params JsonObject[] statements)
    {
        var arr = new JsonArray();
        foreach (var s in statements) arr.Add(s);
        return arr;
    }

    /// <summary>拼一张事件图。</summary>
    public static JsonObject EventGraph(string function, JsonArray body, string? owner = null) =>
        Spec.EventGraph(function, body, owner);

    /// <summary>拼一张函数图。</summary>
    public static JsonObject FunctionGraph(string function, JsonArray body, string? owner = null) =>
        Spec.FunctionGraph(function, body, owner);
}
