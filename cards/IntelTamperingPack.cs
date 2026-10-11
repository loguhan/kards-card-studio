using KardsCardCompiler.Core;

namespace KardsDiy.IntelTampering;

/// <summary>
/// 苏联 1K 指令 —— 「情报篡改」
///
/// 规则：敌方使用指令时，先将其转换为 1 张花费相近的指令。
///
/// ── 已实现（全部对照游戏反射出来的 API 核对过）────────────────────
///   * 事件：<c>OnOtherCardPlayedFromHand(cardPlayed)</c>（BaseCardObject.h:571）。
///     属于"其他卡"事件，必须把 <c>ERegisteredCardFunction::OnOtherCardPlayedFromHand</c>
///     写进 CDO 的 <c>usedTriggers</c> 才会被派发 —— 由工作流的
///     KardsTable <c>enumarray:</c> 语法补写（见 workflow-intel.json 的 cdoExtra）。
///   * 判断"是不是指令"：<c>cardPlayed.IsOrder(out isOrder)</c>（:880）。
///     编译器的蓝图 IR 读不了"别的对象的属性"，但 <c>BlueprintCallStatement.Target</c>
///     可以指定调用目标，所以一律用**函数**取值。
///   * 取花费：<c>cardPlayed.getTotalKreditCost(out cost)</c>（:988）。
///   * 拦截：<c>GotchaTriggered(cardPlayed, Self.CardID, stopFurtherCardActions=true,
///     skipDiscardingOrder=false, out qqq)</c>（CardFunctionsStub.h:306）——
///     在敌方指令结算前把它按反制处理掉，对应卡面"**先**将其转换"的"先"。
///
/// ── 未实现：真正的"转换成 1 张花费相近的指令" ──────────────────────
///   <c>ConvertCard(CardIDs, instigatorID, convertToCardName, convertIntoCardID,
///   skipTrigger, newCardIDs)</c>（:552）的第一个参数是 <c>TArray&lt;int32&gt;</c>，
///   需要拿到"刚被打出那张卡"的运行时 CardID 才能组装。而当前工具链缺两块能力：
///     1. IR 没有对象数组类型（<c>BlueprintValueKind</c> 只有 Int32Array，
///        <c>BlueprintTypeRef</c> 只有 <c>Int32Array</c> 静态项），
///        因此 <c>GetAllActiveStaticCards</c> / <c>GetRandomCard</c> 这类
///        "出参是 TArray&lt;UBaseCardObject*&gt;" 的函数调不了，无法枚举卡池；
///     2. 编译器不生成任何 <c>KismetArrayLibrary</c> 调用（Array_Length / Array_Get /
///        Array_Add / Array_Clear 全部无支持），因此连 Int32Array 也组装不了。
///   补齐这两点后，把本文件 ④ 段替换成：
///       GetAllCardsPlayedThisBattle → Array_Get(末尾) → ids.Add(playedId)
///       → 按 cost 分档随机取 target → ConvertCard(ids, Self.CardID, target, 0, false, newIds)
///   内置的苏联指令花费表见下方注释，已经实测好了。
///
/// 内置花费表（从游戏本体实测）：
///   1K: bloody_sickle / burning_sky / comrades / final_push / men_of_steel / veteran_pilots
///   2K: assault / barrage / blood_red_sky / close_combat / counter_offensive / mobile_defense / mud
///   3K: night_witches / parade
///   4K: arctic_convoy / critical_hit
/// </summary>
public sealed class IntelTamperingPack : ICardPackDefinition, ICardBlueprintDefinition
{
    private const string KardsPackage = "/Script/kards";
    private const string BaseCardClass = "BaseCardObject";
    private const string CardFunctionsClass = "CardFunctionsStub";

    private static readonly BlueprintTypeRef BaseCardType =
        BlueprintTypeRef.Object(KardsPackage, BaseCardClass);

    private static readonly BlueprintTypeRef CardFunctionsType =
        BlueprintTypeRef.Object(KardsPackage, CardFunctionsClass);

    public IReadOnlyCollection<CardDefinition> DefineCards() => new[]
    {
        CardDefinition.Event(
                id: 910102,
                name: "card_event_intelligence_tampering",
                faction: Faction.Soviet,
                cost: 1,
                deckCodeId: "VA")
            .Text(
                title: "情报篡改",
                rules: "敌方使用指令时，先将其转换为 1 张花费相近的指令。",
                flavor: "「命令是向那边开炮……那边。」")
            .FromSet(CardSet.Basic)
            .RarityOf(CardRarity.Common)
    };

    private static BlueprintVariableExpression Var(string n, BlueprintTypeRef t) => new(n, t);

    private static BlueprintSelfPropertyExpression SelfProp(string n, BlueprintTypeRef t) =>
        new(KardsPackage, BaseCardClass, n, t);

    private static BlueprintCallStatement CardFn(string name, params BlueprintExpression[] a) =>
        new(BlueprintCallDispatch.ObjectVirtual, KardsPackage, CardFunctionsClass, name, a,
            SelfProp("cardFunction", CardFunctionsType));

    /// <summary>
    /// 在指定卡对象上调用普通函数（用来读它的属性）。
    /// 注意：契约里 IsOrder / getTotalKreditCost 这类是普通函数（Event=false），
    /// 必须用 ObjectFinal；ObjectVirtual 只留给事件（cardFunction 上的那些）。
    /// </summary>
    private static BlueprintCallStatement OnCard(string fn, BlueprintExpression card,
        params BlueprintExpression[] a) =>
        new(BlueprintCallDispatch.ObjectFinal, KardsPackage, BaseCardClass, fn, a, card);

    public IReadOnlyList<BlueprintFunctionIr> DefineBlueprintFunctions(CardIr card)
    {
        var cardPlayed = Var("cardPlayed", BaseCardType);
        var isOrder = Var("isOrder", BlueprintTypeRef.Boolean);
        var cost = Var("cost", BlueprintTypeRef.Int32);
        var stopped = Var("stopped", BlueprintTypeRef.Boolean);

        var selfId = SelfProp("CardID", BlueprintTypeRef.Int32);

        var body = new List<BlueprintStatement>
        {
            // ① cardPlayed.IsOrder(out isOrder); 不是指令就什么都不做
            OnCard("IsOrder", cardPlayed, isOrder),
            new BlueprintIfStatement(
                new BlueprintNotExpression(isOrder),
                new BlueprintStatement[] { new BlueprintReturnStatement() }),

            // ② cardPlayed.getTotalKreditCost(out cost); 0K 指令不拦
            OnCard("getTotalKreditCost", cardPlayed, cost),
            new BlueprintIfStatement(
                new BlueprintIntComparisonExpression(
                    BlueprintComparison.LessOrEqual, cost, new BlueprintIntExpression(0)),
                new BlueprintStatement[] { new BlueprintReturnStatement() }),

            // ③ 反制掉这张指令（对应"先"拦截）
            CardFn("GotchaTriggered",
                cardPlayed,
                selfId,
                new BlueprintBoolExpression(true),   // stopFurtherCardActions
                new BlueprintBoolExpression(false),  // skipDiscardingOrder
                stopped),
        };

        return new[]
        {
            new BlueprintFunctionIr(
                Name: "OnOtherCardPlayedFromHand",
                Variables: new[]
                {
                    new BlueprintVariableDefinition("cardPlayed", BaseCardType, BlueprintVariableRole.Input),
                    new BlueprintVariableDefinition("isOrder", BlueprintTypeRef.Boolean),
                    new BlueprintVariableDefinition("cost", BlueprintTypeRef.Int32),
                    new BlueprintVariableDefinition("stopped", BlueprintTypeRef.Boolean),
                },
                Body: body,
                OverrideOwnerPackage: KardsPackage,
                OverrideOwnerClass: BaseCardClass)
        };
    }
}
