#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/** Expression node in the card spec. Mirrors a real K2 data node. */
enum class ECardExprKind : uint8
{
    Self,
    Param,
    Local,
    Member,
    Call,
    Literal,
    Cast,
    ArrayMake,
    /** Read a property of another object: Target is the object, Name the property. */
    Property
};

struct FCardExpr;
using FCardExprPtr = TSharedPtr<FCardExpr>;

struct FCardExpr
{
    ECardExprKind Kind = ECardExprKind::Literal;

    /** param / local / member name, or the function name for Call, or the class path for Cast. */
    FString Name;

    /** Optional explicit owner class for Call (defaults to the static type of Target). */
    FString OwnerClass;

    /** Which output pin of the call to read. Empty = auto (ReturnValue, else the single data out). */
    FString ResultPin;

    /** Literal vocabulary: bool,int,int64,byte,float,double,string,name,text,object:,class:,enum:. */
    FString LiteralType;
    FString LiteralValue;

    /** Receiver for Call (defaults to Self when omitted). */
    FCardExprPtr Target;

    /** Named argument pins. */
    TArray<TPair<FString, FCardExprPtr>> Args;

    /** Positional arguments; mapped onto the function's parameters in declaration order. */
    TArray<FCardExprPtr> ArgList;

    /** Element expressions for ArrayMake; LiteralType holds the element pin type. */
    TArray<FCardExprPtr> Items;
};

enum class ECardStmtKind : uint8
{
    Call,
    If,
    Set,
    LocalSet,
    LocalDecl,
    Seq,
    Cast,
    /** Terminal of a function graph: writes the result node's output pins. */
    Result,
    /** Macro loop: Array in, Then = LoopBody, Outs bind "Index" / "Array Element". */
    Foreach
};

struct FCardStmt;
using FCardStmtPtr = TSharedPtr<FCardStmt>;

/** Where an out-parameter of an impure call is written. */
struct FCardOutSink
{
    bool bToLocal = false;
    FString Name;
};

struct FCardStmt
{
    ECardStmtKind Kind = ECardStmtKind::Call;

    /** function name (Call) or variable name (Set/LocalSet/LocalDecl). */
    FString Name;
    FString OwnerClass;
    FString DeclType;

    FCardExprPtr Target;
    FCardExprPtr Value;

    TArray<TPair<FString, FCardExprPtr>> Args;
    /** Positional arguments (args given as a JSON array). */
    TArray<FCardExprPtr> ArgList;
    TArray<TPair<FString, FCardOutSink>> Outs;

    TArray<FCardStmtPtr> Then;
    TArray<FCardStmtPtr> Else;
};

enum class ECardGraphKind : uint8
{
    Event,
    Function,
    /** 原样实例化官方 UK2Node_* 图：节点按类名建，连线按引脚名连。 */
    RawK2
};

/** 一个官方 K2 节点。Class 可写 K2Node_CallFunction 或完整类路径。 */
struct FCardNodeSpec
{
    FString Id;
    FString Class;
    FString Function;      // CallFunction
    FString OwnerClass;    // CallFunction / Event / VariableGet-Set 的宿主类
    FString Variable;      // VariableGet / VariableSet（self 成员）
    FString Macro;         // MacroInstance 的宏图路径
    FString TargetClass;   // DynamicCast
    int32 PosX = 0;
    int32 PosY = 0;
    FString Comment;
    /** 引脚默认值：引脚名 -> 字面量文本（枚举写枚举项名，对象写路径）。 */
    TArray<TPair<FString, FString>> Defaults;

    /**
     * 原始 JSON。构建器用反射按名处理：
     *   props  —— 任意 UPROPERTY 赋值（含 EdGraphPinType / 枚举 / 对象类 / 结构体 / 数组）
     *   api    —— 任意成员函数调用（AddInputPin / SetMacroGraph / AddOptionPinToNode …）
     *   pins   —— 引脚级操作（默认值 / 覆盖引脚类型）
     * 这就是「和编辑器一样 100% 能建」的那一层：不硬编码节点类。
     */
    TSharedPtr<FJsonObject> Raw;
};

/** 一条引脚连线。 */
struct FCardLinkSpec
{
    FString FromId;
    FString FromPin;
    FString ToId;
    FString ToPin;
};

struct FCardGraph
{
    ECardGraphKind Kind = ECardGraphKind::Event;
    FString Function;
    FString OwnerClass;
    TArray<FCardStmtPtr> Body;

    /** kind = k2 时使用 */
    TArray<FCardNodeSpec> Nodes;
    TArray<FCardLinkSpec> Links;
};

/** Which macro graph a Foreach statement instantiates. */
namespace CardMacros
{
    const TCHAR* const ForEachLoop =
        TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:ForEachLoop");
}

/** A blueprint member variable declaration. */
struct FCardVariable
{
    FString Name;
    FString Type;
    FString DefaultValue;
};

/** A default value written onto the generated class CDO after a clean compile. */
struct FCardDefault
{
    FString Name;
    bool bIsArray = false;
    FString EnumType;     // enum: for scalars, the enum type used to resolve a name
    FString LiteralType;
    FString LiteralValue;
    TArray<FString> Values;
};

struct FCardSpec
{
    FString Package;
    FString AssetName;
    FString ParentClass = TEXT("/Script/kards.BaseCardObject");
    TArray<FCardVariable> Variables;
    TArray<FCardDefault> Defaults;
    TArray<FCardGraph> Graphs;
};

/** Parse a spec JSON document. */
bool ParseCardSpec(const FString& Text, FCardSpec& Out, FString& Error);

/** Build an FEdGraphPinType from the spec vocabulary. */
bool PinTypeFromSpec(const FString& Spec, FEdGraphPinType& Out, FString& Error);

/** Human readable pin type, used by the dump mode. */
FString PinTypeToString(const FEdGraphPinType& Type);

/** Resolve a class by object path. */
UClass* ResolveClass(const FString& Path);
