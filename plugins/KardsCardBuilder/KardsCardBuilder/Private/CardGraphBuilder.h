#pragma once

#include "CoreMinimal.h"
#include "CardSpec.h"

class UBlueprint;
class UEdGraph;

struct FCardDiagnostics
{
    TArray<FString> Errors;
    TArray<FString> Notes;

    void Error(const FString& Message) { Errors.Add(Message); }
    void Note(const FString& Message) { Notes.Add(Message); }
    bool HasErrors() const { return Errors.Num() > 0; }
};

/**
 * Assemble one K2 graph (event or function) of a card blueprint out of real
 * UK2Node_* objects, wired with the K2 schema. Returns false on the first hard
 * failure; every failure is also recorded in Diag.
 */
bool BuildCardGraph(UBlueprint* Blueprint, UEdGraph* Graph, const FCardGraph& Spec, FCardDiagnostics& Diag);

/**
 * Instantiate an arbitrary official UK2Node_* graph verbatim (spec kind "k2"):
 * nodes are created by class name through the editor's own node classes and wired
 * by pin name. This is what makes 100% card coverage possible -- the statement-tree
 * mode above is only a convenience layer over a subset of node kinds.
 */
bool BuildRawK2Graph(UBlueprint* Blueprint, UEdGraph* Graph, const FCardGraph& Spec, FCardDiagnostics& Diag);

// ─────────────────────────────────────────────────────────────────────────────
// 编辑器会话用的公共原语：让 AI 直接操作节点图，覆盖度 = 编辑器自身能力。
// ─────────────────────────────────────────────────────────────────────────────

/** 按官方类名建节点（K2Node_Xxx 或完整路径），返回已 AllocateDefaultPins 的节点。 */
UEdGraphNode* KardsK2CreateNode(UEdGraph* Graph, const FString& ClassPath, int32 PosX, int32 PosY, FString& Error);

/** 同上，但先设好决定引脚形状的字段（function/owner/variable/macro/target）。 */
UEdGraphNode* KardsK2CreateNodeEx(UEdGraph* Graph, const FString& ClassPath, int32 PosX, int32 PosY,
    const FString& Function, const FString& OwnerClass, const FString& Variable,
    const FString& Macro, const FString& TargetClass, FString& Error);

/** 按 UPROPERTY 名反射赋值（含 EdGraphPinType / 枚举 / 对象类 / 结构体 / 数组）。 */
bool KardsK2SetProperty(UEdGraphNode* Node, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& Error);

/** 反射调用节点上的成员函数；非 UFUNCTION 的常用 C++ 方法走垫片。 */
bool KardsK2CallApi(UEdGraphNode* Node, const FString& FunctionName, const TArray<TSharedPtr<FJsonValue>>& Args, FString& Error);

/** 回读整张图（节点 / 引脚 / 连线 / 位置）为 JSON 文本，供 AI 迭代。 */
FString KardsK2DumpGraph(UEdGraph* Graph);

/** 在可编辑引脚基类节点上加一个用户定义引脚（自定义事件/函数签名）。 */
bool KardsK2AddPin(UEdGraphNode* Node, const FString& PinName, const FString& PinTypeSpec, bool bOutput, FString& Error);

/** 新建一张本地函数图（全新的 UFunction，不是覆写父类）。返回入口节点，OutResult 接结果节点。 */
UEdGraphNode* KardsK2AddFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName, UEdGraphNode*& OutResult, FString& Error);

/** 在事件图里建一个自定义事件节点。 */
UEdGraphNode* KardsK2AddCustomEvent(UEdGraph* Graph, const FString& EventName, FString& Error);
