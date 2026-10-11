#include "CardGraphBuilder.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Select.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Self.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogKardsCardBuilder, Log, All);

namespace
{
/**
 * Data compatibility ignoring the reference/const qualifiers: a by-reference
 * parameter pin and a plain value pin of the same shape are connectable, and
 * the schema is still the final authority.
 */
bool SameDataShape(const FEdGraphPinType& A, const FEdGraphPinType& B)
{
    if (A.PinCategory != B.PinCategory) return false;
    if (A.ContainerType != B.ContainerType) return false;
    if (A.PinSubCategoryObject == B.PinSubCategoryObject) return true;
    // Byte pins interoperate with enum pins (an enum is stored as a byte); the schema
    // is still the final authority on whether the connection is actually allowed.
    return A.PinCategory == UEdGraphSchema_K2::PC_Byte;
}

/** Case-insensitive closest-name hint so a typo never becomes a silent guess. */FString NearMatches(UClass* Owner, const FString& Wanted)
{
    if (!Owner) return FString();
    TArray<FString> Names;
    for (TFieldIterator<UFunction> It(Owner, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        Names.Add(It->GetName());
    }
    Names.Sort();
    TArray<FString> Hits;
    const FString Lower = Wanted.ToLower();
    for (const FString& Name : Names)
    {
        if (Name.ToLower().Contains(Lower) || Lower.Contains(Name.ToLower()))
        {
            Hits.Add(Name);
        }
    }
    if (Hits.Num() == 0) return FString();
    if (Hits.Num() > 12) Hits.SetNum(12);
    return TEXT("  candidates: ") + FString::Join(Hits, TEXT(", "));
}

class FCardGraphAssembler
{
public:
    FCardGraphAssembler(UBlueprint* InBlueprint, UEdGraph* InGraph, const FCardGraph& InSpec, FCardDiagnostics& InDiag)
        : Blueprint(InBlueprint)
        , Graph(InGraph)
        , Spec(InSpec)
        , Diag(InDiag)
        , Schema(GetDefault<UEdGraphSchema_K2>())
    {
    }

    bool Run()
    {
        if (!CreateEntry()) return false;
        TArray<UEdGraphPin*> Tails = { EntryThen };
        if (!Block(Spec.Body, Tails)) return false;
        if (Tails.Num() > 0)
        {
            // Unconnected exec outputs are legal K2, but report it so the spec stays honest.
            Diag.Note(FString::Printf(TEXT("%s: %d execution path(s) end without a node"), *Spec.Function, Tails.Num()));
        }
        return true;
    }

private:
    UBlueprint* Blueprint;
    UEdGraph* Graph;
    const FCardGraph& Spec;
    FCardDiagnostics& Diag;
    const UEdGraphSchema_K2* Schema;

    UEdGraphNode* EntryNode = nullptr;
    UEdGraphPin* EntryThen = nullptr;
    UK2Node_FunctionEntry* FunctionEntry = nullptr;
    TSet<UK2Node_FunctionResult*> UsedResults;
    TMap<FString, UEdGraphPin*> ParamPins;
    /**
     * Execution tails of the statement currently being built. An impure call written
     * as an expression (tmp_x = GetAllActiveStaticCards(...)) is hoisted into this
     * chain instead of being rejected, which is how a human would read the spec.
     */
    TArray<UEdGraphPin*>* ActiveTails = nullptr;
    int32 Position = 0;

    bool Fail(const FString& Message)
    {
        Diag.Error(Spec.Function + TEXT(": ") + Message);
        return false;
    }

    TArray<FString> ParamPinsKeys() const
    {
        TArray<FString> Keys;
        ParamPins.GetKeys(Keys);
        if (Keys.Num() == 0 && EntryNode)
        {
            for (UEdGraphPin* Pin : EntryNode->Pins)
            {
                if (Pin) Keys.Add(TEXT("[") + Pin->PinName.ToString() + TEXT("/") +
                    PinTypeToString(Pin->PinType) + TEXT("/") + FString::FromInt(static_cast<int32>(Pin->Direction)) + TEXT("]"));
            }
        }
        return Keys;
    }

    void Locate(UEdGraphNode* Node, const FString& Comment)
    {
        Node->NodePosX = (Position % 4) * 420;
        Node->NodePosY = (Position / 4) * 260;
        Node->NodeComment = Comment;
        ++Position;
    }

    // ---------------------------------------------------------------- entry

    bool CreateEntry()
    {
        UClass* Owner = Blueprint->ParentClass;
        if (!Spec.OwnerClass.IsEmpty())
        {
            Owner = ResolveClass(Spec.OwnerClass);
            if (!Owner) return Fail(TEXT("cannot resolve owner class ") + Spec.OwnerClass);
        }
        if (!Owner) return Fail(TEXT("blueprint has no parent class"));

        UFunction* Function = Owner->FindFunctionByName(FName(*Spec.Function));
        if (!Function)
        {
            return Fail(TEXT("parent class has no function '") + Spec.Function + TEXT("'") + NearMatches(Owner, Spec.Function));
        }

        if (Spec.Kind == ECardGraphKind::Function)
        {
            // Overriding a BlueprintNativeEvent needs a real function graph, not an
            // event node: AddFunctionGraph wires UK2Node_FunctionEntry's signature to
            // the parent UFunction so the compiler treats it as an override.
            if (!Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
            {
                return Fail(TEXT("'") + Spec.Function + TEXT("' is not a blueprint event on ") + Owner->GetName() +
                    TEXT("; only BlueprintImplementableEvent / BlueprintNativeEvent can be overridden"));
            }
            FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, /*bIsUserCreated*/ true, Function);
            TArray<UK2Node_FunctionEntry*> Entries;
            Graph->GetNodesOfClass(Entries);
            if (Entries.Num() != 1)
            {
                return Fail(FString::Printf(TEXT("expected exactly one function entry, found %d"), Entries.Num()));
            }
            UK2Node_FunctionEntry* Entry = Entries[0];
            Entry->ClearExtraFlags(FUNC_BlueprintPure | FUNC_Const | FUNC_Static);
            Locate(Entry, TEXT("function ") + Spec.Function);
            EntryNode = Entry;
            FunctionEntry = Entry;
            EntryThen = Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
            if (!EntryThen) return Fail(TEXT("function entry has no then pin"));
            for (UEdGraphPin* Pin : Entry->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output) continue;
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
                ParamPins.Add(Pin->PinName.ToString(), Pin);
            }
            for (const FCardStmtPtr& Stmt : Spec.Body)
            {
                if (!Stmt.IsValid()) return Fail(TEXT("null statement in body"));
            }
            return true;
        }

        if (!Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
        {
            return Fail(TEXT("'") + Spec.Function + TEXT("' is not a blueprint event on ") + Owner->GetName());
        }

        FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
        UK2Node_Event* Event = Creator.CreateNode(false);
        Event->EventReference.SetExternalMember(Function->GetFName(), Owner);
        Event->bOverrideFunction = true;
        Creator.Finalize();
        Locate(Event, TEXT("event ") + Spec.Function);
        EntryNode = Event;

        EntryThen = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
        if (!EntryThen) return Fail(TEXT("event node has no then pin"));

        for (UEdGraphPin* Pin : Event->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output) continue;
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate) continue;
            ParamPins.Add(Pin->PinName.ToString(), Pin);
        }
        for (const FCardStmtPtr& Stmt : Spec.Body)
        {
            if (!Stmt.IsValid()) return Fail(TEXT("null statement in body"));
        }
        return true;
    }

    // ------------------------------------------------------------- plumbing

    bool Connect(UEdGraphPin* From, UEdGraphPin* To, const FString& What)
    {
        if (!From) return Fail(What + TEXT(": source pin is missing"));
        if (!To) return Fail(What + TEXT(": destination pin is missing"));
        if (From->Direction != EGPD_Output || To->Direction != EGPD_Input)
        {
            return Fail(What + TEXT(": cannot connect ") + From->GetName() + TEXT(" -> ") + To->GetName());
        }
        if (From->LinkedTo.Contains(To)) return true;
        // Wildcard pins (array literal inputs) and the 'self' pin are resolved by the schema,
        // which substitutes the calling context for PSC_Self. Everything else must match
        // exactly, and a conversion node is never inserted silently.
        const bool bRelaxed = From->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard ||
            To->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard ||
            From->PinType.PinSubCategory == UEdGraphSchema_K2::PSC_Self ||
            To->PinType.PinSubCategory == UEdGraphSchema_K2::PSC_Self;
        if (!bRelaxed && !SameDataShape(From->PinType, To->PinType))
        {
            return Fail(What + TEXT(": incompatible pins ") + PinTypeToString(From->PinType) + TEXT(" -> ") +
                PinTypeToString(To->PinType));
        }
        const FPinConnectionResponse Response = Schema->CanCreateConnection(From, To);
        if (Response.Response != CONNECT_RESPONSE_MAKE)
        {
            return Fail(What + TEXT(": schema refused ") + From->GetOwningNode()->GetName() + TEXT(".") + From->PinName.ToString() +
                TEXT(" -> ") + To->GetOwningNode()->GetName() + TEXT(".") + To->PinName.ToString() + TEXT(" (") +
                Response.Message.ToString() + TEXT(")"));
        }
        if (!Schema->TryCreateConnection(From, To) || !From->LinkedTo.Contains(To) || !To->LinkedTo.Contains(From))
        {
            return Fail(What + TEXT(": connection did not take"));
        }
        return true;
    }

    bool ConnectExec(const TArray<UEdGraphPin*>& Tails, UEdGraphPin* Input, const FString& What)
    {
        if (!Input) return Fail(What + TEXT(": node has no exec input"));
        if (Tails.Num() == 0) return Fail(What + TEXT(": unreachable statement"));
        for (UEdGraphPin* Tail : Tails)
        {
            if (!Connect(Tail, Input, What)) return false;
        }
        return true;
    }

    /**
     * KismetArrayLibrary functions (Array_Length / Array_Get / Array_Add / ...) carry
     * wildcard array pins, so they need UK2Node_CallArrayFunction; every other function
     * uses the plain call node.
     */
    UK2Node_CallFunction* MakeCallNode(UFunction* Function)
    {
        UClass* const OwnerClass = Function ? Function->GetOuterUClass() : nullptr;
        const bool bArrayFunction = OwnerClass && OwnerClass->GetName() == TEXT("KismetArrayLibrary");
        if (bArrayFunction)
        {
            FGraphNodeCreator<UK2Node_CallArrayFunction> Creator(*Graph);
            UK2Node_CallArrayFunction* Node = Creator.CreateNode(false);
            Node->SetFromFunction(Function);
            Creator.Finalize();
            return Node;
        }
        FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
        UK2Node_CallFunction* Node = Creator.CreateNode(false);
        Node->SetFromFunction(Function);
        Creator.Finalize();
        return Node;
    }

    /** 位置参数按函数声明顺序绑到对应输入引脚。 */
    bool ApplyPositionalArgs(UK2Node_CallFunction* Call, UFunction* Function, const TArray<FCardExprPtr>& ArgList, const FString& What)
    {
        int32 Index = 0;
        for (TFieldIterator<FProperty> It(Function); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_Parm)) continue;
            if (Property->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
            if (Index >= ArgList.Num()) break;
            UEdGraphPin* Pin = Call->FindPin(Property->GetFName(), EGPD_Input);
            if (!Pin) continue;   // 纯出参没有输入引脚
            if (!ValueInto(ArgList[Index], Pin, What + TEXT(".") + Property->GetName())) return false;
            ++Index;
        }
        if (Index < ArgList.Num())
        {
            return Fail(FString::Printf(TEXT("%s: %d positional args given but the function only has %d parameters"),
                *What, ArgList.Num(), Index));
        }
        return true;
    }

    UEdGraphPin* InputPin(UEdGraphNode* Node, const FString& Name, const FString& What, bool& bOk)    {
        UEdGraphPin* Pin = Node->FindPin(FName(*Name), EGPD_Input);
        if (!Pin)
        {
            TArray<FString> Available;
            for (UEdGraphPin* Candidate : Node->Pins)
            {
                if (Candidate && Candidate->Direction == EGPD_Input) Available.Add(Candidate->PinName.ToString());
            }
            // UE uniquifies pin names when a node already owns one with the same name
            // (a function result node re-using the entry's parameter names, for example),
            // so accept a unique prefix match instead of failing on "shouldIt1".
            TArray<FString> PrefixHits;
            for (const FString& Candidate : Available)
            {
                if (Candidate.StartsWith(Name)) PrefixHits.Add(Candidate);
            }
            if (PrefixHits.Num() == 1)
            {
                bOk = true;
                return Node->FindPin(FName(*PrefixHits[0]), EGPD_Input);
            }
            Fail(What + TEXT(": no input pin '") + Name + TEXT("' on ") + Node->GetClass()->GetName() +
                TEXT("; available: ") + FString::Join(Available, TEXT(", ")));
            bOk = false;
            return nullptr;
        }
        bOk = true;
        return Pin;
    }

    UEdGraphPin* OutputPin(UEdGraphNode* Node, const FString& Name, const FString& What, bool& bOk)
    {
        UEdGraphPin* Pin = Node->FindPin(FName(*Name), EGPD_Output);
        if (!Pin)
        {
            TArray<FString> Available;
            for (UEdGraphPin* Candidate : Node->Pins)
            {
                if (Candidate && Candidate->Direction == EGPD_Output) Available.Add(Candidate->PinName.ToString());
            }
            TArray<FString> PrefixHits;
            for (const FString& Candidate : Available)
            {
                if (Candidate.StartsWith(Name)) PrefixHits.Add(Candidate);
            }
            if (PrefixHits.Num() == 1)
            {
                bOk = true;
                return Node->FindPin(FName(*PrefixHits[0]), EGPD_Output);
            }
            Fail(What + TEXT(": no output pin '") + Name + TEXT("' on ") + Node->GetClass()->GetName() +
                TEXT("; available: ") + FString::Join(Available, TEXT(", ")));
            bOk = false;
            return nullptr;
        }
        bOk = true;
        return Pin;
    }

    // ------------------------------------------------------------- literals

    bool ApplyLiteral(UEdGraphPin* Pin, const FString& Type, const FString& Value, const FString& What)
    {
        if (!Pin) return Fail(What + TEXT(": literal has no destination pin"));
        FString Kind = Type;
        if (Kind.IsEmpty())
        {
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean) Kind = TEXT("bool");
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int) Kind = TEXT("int");
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Int64) Kind = TEXT("int64");
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real) Kind = TEXT("float");
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_String) Kind = TEXT("string");
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Name) Kind = TEXT("name");
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text) Kind = TEXT("text");
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
            {
                Kind = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()) ? TEXT("enum") : TEXT("byte");
            }
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object) Kind = TEXT("object");
            else if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Class) Kind = TEXT("class");
            else
            {
                return Fail(What + TEXT(": cannot infer a literal type for pin category ") + Pin->PinType.PinCategory.ToString());
            }
        }

        FString Effective = Value;
        if (Kind == TEXT("bool"))
        {
            Effective = (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1")) ? TEXT("true") : TEXT("false");
        }
        else if (Kind == TEXT("text"))
        {
            Pin->DefaultTextValue = FText::FromString(Value);
            return true;
        }
        else if (Kind == TEXT("object") || Kind == TEXT("class"))
        {
            UObject* Object = nullptr;
            if (!Value.IsEmpty() && Value != TEXT("None"))
            {
                Object = Kind == TEXT("class") ? static_cast<UObject*>(ResolveClass(Value)) : LoadObject<UObject>(nullptr, *Value);
                if (!Object) return Fail(What + TEXT(": cannot resolve ") + Kind + TEXT(" '") + Value + TEXT("'"));
            }
            Pin->DefaultObject = Object;
            return true;
        }
        else if (Kind == TEXT("enum"))
        {
            UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
            if (!Enum)
            {
                return Fail(What + TEXT(": enum literal on a non-enum pin"));
            }
            const int64 Index = Enum->GetValueByNameString(Value);
            if (Index == INDEX_NONE)
            {
                TArray<FString> Names;
                for (int32 I = 0; I < Enum->NumEnums(); ++I) Names.Add(Enum->GetNameStringByIndex(I));
                return Fail(What + TEXT(": '") + Value + TEXT("' is not a value of ") + Enum->GetName() +
                    TEXT("; values: ") + FString::Join(Names, TEXT(", ")));
            }
            Effective = Value;
        }

        const FString Invalid = Schema->IsPinDefaultValid(Pin, Effective, nullptr, FText());
        if (!Invalid.IsEmpty())
        {
            return Fail(What + TEXT(": invalid default '") + Effective + TEXT("' (") + Invalid + TEXT(")"));
        }
        Schema->TrySetDefaultValue(*Pin, Effective);
        return true;
    }

    // ---------------------------------------------------------- expressions

    UClass* StaticClassOf(const FCardExprPtr& Expr)
    {
        if (!Expr.IsValid()) return Blueprint->ParentClass;
        switch (Expr->Kind)
        {
        case ECardExprKind::Self:
            return Blueprint->ParentClass;
        case ECardExprKind::Param:
        case ECardExprKind::Local:
        case ECardExprKind::Member:
        {
            if (const UEdGraphPin* const* Found = ParamPins.Find(Expr->Name))
            {
                return Cast<UClass>((*Found)->PinType.PinSubCategoryObject.Get());
            }
            if (FProperty* Property = FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass
                    ? static_cast<UStruct*>(Blueprint->SkeletonGeneratedClass) : static_cast<UStruct*>(Blueprint->ParentClass),
                    FName(*Expr->Name)))
            {
                if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
                {
                    return ObjectProperty->PropertyClass;
                }
                if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
                {
                    return ClassProperty->MetaClass;
                }
            }
            return nullptr;
        }
        case ECardExprKind::Cast:
            return ResolveClass(Expr->Name);
        case ECardExprKind::Literal:
            if (Expr->LiteralType.StartsWith(TEXT("object:"))) return ResolveClass(Expr->LiteralType.RightChop(7));
            return nullptr;
        case ECardExprKind::Call:
        {
            UClass* Owner = Expr->OwnerClass.IsEmpty() ? StaticClassOf(Expr->Target) : ResolveClass(Expr->OwnerClass);
            if (!Owner) return nullptr;
            UFunction* Function = Owner->FindFunctionByName(FName(*Expr->Name));
            if (!Function) return nullptr;
            for (TFieldIterator<FProperty> It(Function); It; ++It)
            {
                FProperty* Property = *It;
                if (!Property->HasAnyPropertyFlags(CPF_Parm)) continue;
                if (!Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm)) continue;
                if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
                {
                    return ObjectProperty->PropertyClass;
                }
            }
            return nullptr;
        }
        default:
            return nullptr;
        }
    }

    UFunction* ResolveCallByName(const FString& FunctionName, const FString& OwnerClass, const FCardExprPtr& Target, UClass*& OutOwner)
    {
        OutOwner = OwnerClass.IsEmpty() ? StaticClassOf(Target) : ResolveClass(OwnerClass);
        if (OutOwner)
        {
            UFunction* Function = OutOwner->FindFunctionByName(FName(*FunctionName));
            if (!Function)
            {
                Fail(TEXT("class ") + OutOwner->GetName() + TEXT(" has no function '") + FunctionName + TEXT("'") + NearMatches(OutOwner, FunctionName));
                return nullptr;
            }
            if (!Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
            {
                Fail(TEXT("function '") + FunctionName + TEXT("' on ") + OutOwner->GetName() + TEXT(" is not BlueprintCallable"));
                return nullptr;
            }
            return Function;
        }
        // No owner given: resolve by unique name across the classes a card spec is
        // allowed to call into. Ambiguity is an error, never a guess.
        TArray<UClass*> Candidates;
        if (Blueprint->ParentClass) Candidates.Add(Blueprint->ParentClass);
        for (const TCHAR* Path : {TEXT("/Script/Engine.KismetMathLibrary"), TEXT("/Script/Engine.KismetArrayLibrary"),
                                  TEXT("/Script/Engine.KismetSystemLibrary"), TEXT("/Script/Engine.KismetStringLibrary"),
                                  TEXT("/Script/kards.BaseCardObject"), TEXT("/Script/kards.CardFunctionsStub")})
        {
            if (UClass* Class = ResolveClass(Path))
            {
                if (!Candidates.Contains(Class)) Candidates.Add(Class);
            }
        }
        UFunction* Hit = nullptr;
        UClass* HitOwner = nullptr;
        int32 Hits = 0;
        TArray<FString> Where;
        for (UClass* Class : Candidates)
        {
            if (UFunction* Function = Class->FindFunctionByName(FName(*FunctionName)))
            {
                if (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable))
                {
                    Where.Add(Class->GetName());
                    if (!Hit) { Hit = Function; HitOwner = Class; }
                    ++Hits;
                }
            }
        }
        if (Hits == 1) { OutOwner = HitOwner; return Hit; }
        if (Hits > 1)
        {
            Fail(TEXT("call '") + FunctionName + TEXT("' is ambiguous; set 'owner' to one of: ") + FString::Join(Where, TEXT(", ")));
            return nullptr;
        }
        Fail(TEXT("cannot resolve function '") + FunctionName + TEXT("' on any known class"));
        return nullptr;
    }

    UFunction* ResolveCall(const FCardExprPtr& Expr, UClass*& OutOwner)
    {
        return ResolveCallByName(Expr->Name, Expr->OwnerClass, Expr->Target, OutOwner);
    }

    UFunction* ResolveCall(const FCardStmtPtr& Stmt, UClass*& OutOwner)
    {
        return ResolveCallByName(Stmt->Name, Stmt->OwnerClass, Stmt->Target, OutOwner);
    }

    bool ValueInto(const FCardExprPtr& Expr, UEdGraphPin* Destination, const FString& What)
    {
        if (!Expr.IsValid()) return Fail(What + TEXT(": missing expression"));
        if (!Destination) return Fail(What + TEXT(": missing destination pin"));
        if (Expr->Kind == ECardExprKind::Literal)
        {
            return ApplyLiteral(Destination, Expr->LiteralType, Expr->LiteralValue, What);
        }
        bool bOk = false;
        UEdGraphPin* Source = EmitExpr(Expr, What, bOk);
        if (!bOk) return false;
        return Connect(Source, Destination, What);
    }

    UEdGraphPin* EmitExpr(const FCardExprPtr& Expr, const FString& What, bool& bOk)
    {
        bOk = false;
        switch (Expr->Kind)
        {
        case ECardExprKind::Self:
        {
            FGraphNodeCreator<UK2Node_Self> Creator(*Graph);
            UK2Node_Self* Node = Creator.CreateNode(false);
            Creator.Finalize();
            Locate(Node, What + TEXT(" self"));
            bOk = true;
            return Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Output);
        }
        case ECardExprKind::Param:
        {
            if (UEdGraphPin** Found = ParamPins.Find(Expr->Name))
            {
                bOk = true;
                return *Found;
            }
            // UE uniquifies parameter pin names ("triggerCard1") when the entry node is
            // rebuilt from an existing signature; accept a unique prefix match.
            {
                UEdGraphPin* Hit = nullptr;
                int32 Hits = 0;
                for (const TPair<FString, UEdGraphPin*>& Pair : ParamPins)
                {
                    if (Pair.Key.StartsWith(Expr->Name)) { Hit = Pair.Value; ++Hits; }
                }
                if (Hits == 1) { bOk = true; return Hit; }
            }
            Fail(What + TEXT(": graph has no parameter '") + Expr->Name + TEXT("'; parameters: ") +
                FString::Join(ParamPinsKeys(), TEXT(", ")));
            return nullptr;
        }
        case ECardExprKind::Local:
        case ECardExprKind::Member:
        {
            FProperty* Property = FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass
                ? static_cast<UStruct*>(Blueprint->SkeletonGeneratedClass) : static_cast<UStruct*>(Blueprint->ParentClass), FName(*Expr->Name));
            if (!Property)
            {
                Fail(What + TEXT(": blueprint has no variable '") + Expr->Name + TEXT("'"));
                return nullptr;
            }
            FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
            UK2Node_VariableGet* Node = Creator.CreateNode(false);
            Node->VariableReference.SetSelfMember(FName(*Expr->Name));
            Creator.Finalize();
            Locate(Node, What + TEXT(" ") + Expr->Name);
            UEdGraphPin* Pin = Node->FindPin(FName(*Expr->Name), EGPD_Output);
            if (!Pin)
            {
                Fail(What + TEXT(": variable get node has no pin '") + Expr->Name + TEXT("'"));
                return nullptr;
            }
            bOk = true;
            return Pin;
        }
        case ECardExprKind::Cast:
        {
            UClass* Target = ResolveClass(Expr->Name);
            if (!Target) { Fail(What + TEXT(": cannot resolve cast class ") + Expr->Name); return nullptr; }
            FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
            UK2Node_DynamicCast* Node = Creator.CreateNode(false);
            Node->TargetType = Target;
            Creator.Finalize();
            Node->SetPurity(true);
            Locate(Node, What + TEXT(" cast ") + Target->GetName());
            if (!ValueInto(Expr->Target, Node->GetCastSourcePin(), What + TEXT(".source"))) return nullptr;
            bOk = true;
            return Node->GetCastResultPin();
        }
        case ECardExprKind::Property:
        {
            UClass* Owner = Expr->OwnerClass.IsEmpty() ? StaticClassOf(Expr->Target) : ResolveClass(Expr->OwnerClass);
            if (!Owner)
            {
                Fail(What + TEXT(": cannot determine the owner of property '") + Expr->Name + TEXT("'"));
                return nullptr;
            }
            FProperty* Property = FindFProperty<FProperty>(Owner, FName(*Expr->Name));
            if (!Property)
            {
                Fail(What + TEXT(": ") + Owner->GetName() + TEXT(" has no property '") + Expr->Name + TEXT("'"));
                return nullptr;
            }
            FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
            UK2Node_VariableGet* Node = Creator.CreateNode(false);
            // 属性属于本蓝图（或它的父类）时用 SetSelfMember —— 这是 UE 自身成员访问的正确写法，
            // 节点不需要连 self 引脚。用 SetExternalMember 会要求 self 连接，否则编译报「无效的目标」。
            UBlueprint* OwningBlueprint = Cast<UBlueprint>(Graph->GetOuter());
            if (OwningBlueprint && OwningBlueprint->ParentClass && Owner->IsChildOf(OwningBlueprint->ParentClass))
            {
                Node->VariableReference.SetSelfMember(FName(*Expr->Name));
            }
            else
            {
                Node->VariableReference.SetExternalMember(FName(*Expr->Name), Owner);
            }
            Creator.Finalize();
            Locate(Node, What + TEXT(" ") + Expr->Name);
            if (Expr->Target.IsValid() && Expr->Target->Kind != ECardExprKind::Self)
            {
                bool bSelfOk = false;
                UEdGraphPin* SelfPin = InputPin(Node, UEdGraphSchema_K2::PN_Self.ToString(), What, bSelfOk);
                if (!bSelfOk) return nullptr;
                if (!ValueInto(Expr->Target, SelfPin, What + TEXT(".self"))) return nullptr;
            }
            UEdGraphPin* Out = Node->FindPin(FName(*Expr->Name), EGPD_Output);
            if (!Out)
            {
                Fail(What + TEXT(": property get node has no pin '") + Expr->Name + TEXT("'"));
                return nullptr;
            }
            bOk = true;
            return Out;
        }
        case ECardExprKind::Call:
            return EmitCall(Expr, What, bOk);
        case ECardExprKind::ArrayMake:
        {
            FEdGraphPinType ElementType;
            FString TypeError;
            if (!PinTypeFromSpec(Expr->LiteralType, ElementType, TypeError))
            {
                Fail(What + TEXT(": arrayMake ") + TypeError);
                return nullptr;
            }
            if (ElementType.ContainerType != EPinContainerType::None)
            {
                Fail(What + TEXT(": arrayMake 'type' must be a single element type"));
                return nullptr;
            }
            if (Expr->Items.Num() == 0)
            {
                Fail(What + TEXT(": arrayMake needs at least one item"));
                return nullptr;
            }
            FGraphNodeCreator<UK2Node_MakeArray> Creator(*Graph);
            UK2Node_MakeArray* Node = Creator.CreateNode(false);
            Creator.Finalize();
            while (Node->NumInputs < Expr->Items.Num()) Node->AddInputPin();
            UEdGraphPin* Out = Node->GetOutputPin();
            if (!Out)
            {
                Fail(What + TEXT(": arrayMake node has no output pin"));
                return nullptr;
            }
            Out->PinType = ElementType;
            Out->PinType.ContainerType = EPinContainerType::Array;
            Locate(Node, What + TEXT(" arrayMake"));
            for (int32 Index = 0; Index < Expr->Items.Num(); ++Index)
            {
                UEdGraphPin* ElementPin = Node->FindPin(Node->GetPinName(Index), EGPD_Input);
                if (!ElementPin)
                {
                    Fail(FString::Printf(TEXT("%s: arrayMake has no element pin [%d]"), *What, Index));
                    return nullptr;
                }
                ElementPin->PinType = ElementType;
                if (!ValueInto(Expr->Items[Index], ElementPin, FString::Printf(TEXT("%s[%d]"), *What, Index))) return nullptr;
            }
            bOk = true;
            return Out;
        }
        default:
            Fail(What + TEXT(": unsupported expression"));
            return nullptr;
        }
    }

    UEdGraphPin* EmitCall(const FCardExprPtr& Expr, const FString& What, bool& bOk)
    {
        bOk = false;
        UClass* Owner = nullptr;
        UFunction* Function = ResolveCall(Expr, Owner);
        if (!Function) return nullptr;

        UK2Node_CallFunction* Call = MakeCallNode(Function);
        Locate(Call, What + TEXT(" ") + Function->GetName());

        if (Expr->Target.IsValid() && Expr->Target->Kind != ECardExprKind::Self)
        {
            bool bTargetOk = false;
            UEdGraphPin* Self = InputPin(Call, UEdGraphSchema_K2::PN_Self.ToString(), What, bTargetOk);
            if (!bTargetOk) return nullptr;
            if (!ValueInto(Expr->Target, Self, What + TEXT(".self"))) return nullptr;
        }
        for (const TPair<FString, FCardExprPtr>& Arg : Expr->Args)
        {
            bool bPinOk = false;
            UEdGraphPin* Pin = InputPin(Call, Arg.Key, What + TEXT(".") + Arg.Key, bPinOk);
            if (!bPinOk) return nullptr;
            if (!ValueInto(Arg.Value, Pin, What + TEXT(".") + Arg.Key)) return nullptr;
        }
        if (!ApplyPositionalArgs(Call, Function, Expr->ArgList, What)) return nullptr;

        // Arguments are evaluated first; only then does this call take over the
        // execution chain. An impure call written as an expression is hoisted here.
        if (!Call->IsNodePure())
        {
            if (!ActiveTails)
            {
                Fail(What + TEXT(": '") + Function->GetName() +
                    TEXT("' has execution pins but the expression is not inside a statement"));
                return nullptr;
            }
            if (!ConnectExec(*ActiveTails, Call->GetExecPin(), What)) return nullptr;
            *ActiveTails = { Call->GetThenPin() };
        }

        UEdGraphPin* Result = nullptr;
        if (!Expr->ResultPin.IsEmpty())
        {
            bool bPinOk = false;
            Result = OutputPin(Call, Expr->ResultPin, What, bPinOk);
            if (!bPinOk) return nullptr;
        }
        else
        {
            Result = Call->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
            if (!Result)
            {
                for (UEdGraphPin* Pin : Call->Pins)
                {
                    if (!Pin || Pin->Direction != EGPD_Output) continue;
                    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
                    if (Result)
                    {
                        Fail(What + TEXT(": '") + Function->GetName() + TEXT("' has several outputs; set 'pin'"));
                        return nullptr;
                    }
                    Result = Pin;
                }
            }
            if (!Result)
            {
                Fail(What + TEXT(": '") + Function->GetName() + TEXT("' produces no value"));
                return nullptr;
            }
        }
        bOk = true;
        return Result;
    }

    // ----------------------------------------------------------- statements

    bool EmitAssignment(const FString& VariableName, UEdGraphPin* Value, const TArray<UEdGraphPin*>& Tails,
        const FString& What, TArray<UEdGraphPin*>& OutTails)
    {
        if (!FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass
                ? static_cast<UStruct*>(Blueprint->SkeletonGeneratedClass) : static_cast<UStruct*>(Blueprint->ParentClass), FName(*VariableName)))
        {
            return Fail(What + TEXT(": blueprint has no variable '") + VariableName + TEXT("'"));
        }
        FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
        UK2Node_VariableSet* Set = Creator.CreateNode(false);
        Set->VariableReference.SetSelfMember(FName(*VariableName));
        Creator.Finalize();
        Locate(Set, What);
        if (!ConnectExec(Tails, Set->GetExecPin(), What)) return false;
        UEdGraphPin* Target = Set->FindPin(FName(*VariableName), EGPD_Input);
        if (!Target) return Fail(What + TEXT(": set node has no input pin"));
        if (!Connect(Value, Target, What)) return false;
        OutTails = { Set->GetThenPin() };
        return true;
    }

    bool Block(const TArray<FCardStmtPtr>& Statements, TArray<UEdGraphPin*>& Tails)
    {
        TArray<UEdGraphPin*>* const PreviousTails = ActiveTails;
        ActiveTails = &Tails;
        for (const FCardStmtPtr& Statement : Statements)
        {
            if (!Statement.IsValid()) { ActiveTails = PreviousTails; return Fail(TEXT("null statement")); }
            if (!Stmt(Statement, Tails)) { ActiveTails = PreviousTails; return false; }
        }
        ActiveTails = PreviousTails;
        return true;
    }

    bool Stmt(const FCardStmtPtr& Statement, TArray<UEdGraphPin*>& Tails)
    {
        switch (Statement->Kind)
        {
        case ECardStmtKind::If:
        {
            FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
            UK2Node_IfThenElse* Branch = Creator.CreateNode(false);
            Creator.Finalize();
            Locate(Branch, TEXT("if"));
            if (!ValueInto(Statement->Value, Branch->GetConditionPin(), TEXT("if.cond"))) return false;
            if (!ConnectExec(Tails, Branch->GetExecPin(), TEXT("if"))) return false;            TArray<UEdGraphPin*> ThenTails = { Branch->GetThenPin() };
            TArray<UEdGraphPin*> ElseTails = { Branch->GetElsePin() };
            if (!Block(Statement->Then, ThenTails)) return false;
            if (!Block(Statement->Else, ElseTails)) return false;
            Tails = MoveTemp(ThenTails);
            Tails.Append(ElseTails);
            return true;
        }
        case ECardStmtKind::Set:
        case ECardStmtKind::LocalSet:
        {
            if (Statement->Name.IsEmpty()) return Fail(TEXT("assignment without a variable name"));
            if (Statement->Value.IsValid() && Statement->Value->Kind == ECardExprKind::Literal)
            {
                // A literal still needs a real set node so the value lands on the object.
                FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
                UK2Node_VariableSet* Set = Creator.CreateNode(false);
                Set->VariableReference.SetSelfMember(FName(*Statement->Name));
                Creator.Finalize();
                Locate(Set, TEXT("set ") + Statement->Name);
                if (!ConnectExec(Tails, Set->GetExecPin(), TEXT("set ") + Statement->Name)) return false;
                UEdGraphPin* Target = Set->FindPin(FName(*Statement->Name), EGPD_Input);
                if (!Target) return Fail(TEXT("set ") + Statement->Name + TEXT(": no input pin"));
                if (!ApplyLiteral(Target, Statement->Value->LiteralType, Statement->Value->LiteralValue, TEXT("set ") + Statement->Name)) return false;
                Tails = { Set->GetThenPin() };
                return true;
            }
            bool bOk = false;
            UEdGraphPin* Value = EmitExpr(Statement->Value, TEXT("set ") + Statement->Name, bOk);
            if (!bOk) return false;
            return EmitAssignment(Statement->Name, Value, Tails, TEXT("set ") + Statement->Name, Tails);
        }
        case ECardStmtKind::LocalDecl:
        {
            // Locals are declared as blueprint variables by the commandlet before the graph is built.
            return true;
        }
        case ECardStmtKind::Cast:
        {
            UClass* Target = ResolveClass(Statement->Name);
            if (!Target) return Fail(TEXT("cast: cannot resolve class ") + Statement->Name);
            FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
            UK2Node_DynamicCast* Node = Creator.CreateNode(false);
            Node->TargetType = Target;
            Creator.Finalize();
            Node->SetPurity(false);
            Locate(Node, TEXT("cast ") + Target->GetName());
            UEdGraphPin* CastExec = Node->GetExecPin();
            UEdGraphPin* CastOk = Node->GetValidCastPin();
            UEdGraphPin* CastBad = Node->GetInvalidCastPin();
            if (!CastExec || !CastOk || !CastBad) return Fail(TEXT("cast: node pins are missing"));
            if (!ConnectExec(Tails, CastExec, TEXT("cast"))) return false;
            if (!ValueInto(Statement->Value, Node->GetCastSourcePin(), TEXT("cast.source"))) return false;
            TArray<UEdGraphPin*> ThenTails = { CastOk };
            TArray<UEdGraphPin*> ElseTails = { CastBad };
            if (!Statement->Target.IsValid() && Statement->Outs.Num() > 0)
            {
                const FCardOutSink& Sink = Statement->Outs[0].Value;
                if (!EmitAssignment(Sink.Name, Node->GetCastResultPin(), ThenTails, TEXT("cast.result"), ThenTails)) return false;
            }
            if (!Block(Statement->Then, ThenTails)) return false;
            if (!Block(Statement->Else, ElseTails)) return false;
            Tails = MoveTemp(ThenTails);
            Tails.Append(ElseTails);
            return true;
        }
        case ECardStmtKind::Seq:
            return Fail(TEXT("sequence nodes are not implemented yet"));
        case ECardStmtKind::Result:
        {
            if (!FunctionEntry)
            {
                return Fail(TEXT("'result' is only valid inside a function graph"));
            }
            // FBlueprintEditorUtils::AddFunctionGraph already drops a result node into
            // the graph and wires the entry to it; reuse it rather than fighting the
            // schema over the entry's single execution output.
            UK2Node_FunctionResult* Result = nullptr;
            for (UEdGraphNode* Candidate : Graph->Nodes)
            {
                UK2Node_FunctionResult* Existing = Cast<UK2Node_FunctionResult>(Candidate);
                if (Existing && !UsedResults.Contains(Existing))
                {
                    Result = Existing;
                    break;
                }
            }
            if (Result)
            {
                Locate(Result, TEXT("result"));
            }
            else
            {
                FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
                Result = Creator.CreateNode(false);
                Result->FunctionReference = FunctionEntry->FunctionReference;
                Creator.Finalize();
                Locate(Result, TEXT("result"));
            }
            UsedResults.Add(Result);
            Result->bIsEditable = true;
            for (const TPair<FString, FCardExprPtr>& Arg : Statement->Args)
            {
                bool bPinOk = false;
                UEdGraphPin* Pin = InputPin(Result, Arg.Key, TEXT("result.") + Arg.Key, bPinOk);
                if (!bPinOk) return false;
                if (!ValueInto(Arg.Value, Pin, TEXT("result.") + Arg.Key)) return false;
            }
            if (!ConnectExec(Tails, Result->GetExecPin(), TEXT("result"))) return false;
            Tails.Reset();
            return true;
        }
        case ECardStmtKind::Foreach:
        {
            const FString MacroPath = Statement->OwnerClass.IsEmpty()
                ? FString(CardMacros::ForEachLoop) : Statement->OwnerClass;
            UEdGraph* MacroGraph = LoadObject<UEdGraph>(nullptr, *MacroPath);
            if (!MacroGraph)
            {
                return Fail(TEXT("cannot load macro graph ") + MacroPath);
            }
            FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
            UK2Node_MacroInstance* Node = Creator.CreateNode(false);
            Node->SetMacroGraph(MacroGraph);
            Creator.Finalize();
            Locate(Node, TEXT("foreach"));
            if (!Statement->Target.IsValid()) return Fail(TEXT("foreach needs an 'array' expression"));

            bool bPinOk = false;
            UEdGraphPin* ArrayPin = InputPin(Node, TEXT("Array"), TEXT("foreach"), bPinOk);
            if (!bPinOk) return false;
            if (!ValueInto(Statement->Target, ArrayPin, TEXT("foreach.array"))) return false;

            UEdGraphPin* ExecIn = InputPin(Node, TEXT("Exec"), TEXT("foreach"), bPinOk);
            if (!bPinOk) return false;
            if (!ConnectExec(Tails, ExecIn, TEXT("foreach"))) return false;

            UEdGraphPin* LoopBody = OutputPin(Node, TEXT("LoopBody"), TEXT("foreach"), bPinOk);
            if (!bPinOk) return false;
            TArray<UEdGraphPin*> BodyTails = { LoopBody };
            for (const TPair<FString, FCardOutSink>& Out : Statement->Outs)
            {
                UEdGraphPin* Value = OutputPin(Node, Out.Key, TEXT("foreach.out.") + Out.Key, bPinOk);
                if (!bPinOk) return false;
                if (!EmitAssignment(Out.Value.Name, Value, BodyTails,
                        TEXT("foreach ") + Out.Key + TEXT(" -> ") + Out.Value.Name, BodyTails)) return false;
            }
            if (!Block(Statement->Then, BodyTails)) return false;

            UEdGraphPin* Completed = OutputPin(Node, TEXT("Completed"), TEXT("foreach"), bPinOk);
            if (!bPinOk) return false;
            Tails = { Completed };
            return true;
        }
        case ECardStmtKind::Call:
            break;
        default:
            return Fail(TEXT("unsupported statement"));
        }

        // Call statement.
        UClass* Owner = nullptr;
        UFunction* Function = ResolveCall(Statement, Owner);
        if (!Function) return false;
        UK2Node_CallFunction* Call = MakeCallNode(Function);
        Locate(Call, TEXT("call ") + Function->GetName());

        // 纯函数当语句写不是错误（不参与执行链），但它的 out 引脚仍然要接到变量上，
        // 所以这里不提前 return —— 处理完 args / outs 再决定要不要连 exec。
        const bool bPureCall = Call->IsNodePure();
        // Evaluate arguments first: an argument that is itself an impure call advances
        // the execution chain before this node runs.
        if (Statement->Target.IsValid() && Statement->Target->Kind != ECardExprKind::Self)
        {
            bool bPinOk = false;
            UEdGraphPin* Self = InputPin(Call, UEdGraphSchema_K2::PN_Self.ToString(), TEXT("call ") + Function->GetName(), bPinOk);
            if (!bPinOk) return false;
            if (!ValueInto(Statement->Target, Self, TEXT("call ") + Function->GetName() + TEXT(".self"))) return false;
        }
        for (const TPair<FString, FCardExprPtr>& Arg : Statement->Args)
        {
            bool bPinOk = false;
            UEdGraphPin* Pin = InputPin(Call, Arg.Key, TEXT("call ") + Function->GetName() + TEXT(".") + Arg.Key, bPinOk);
            if (!bPinOk) return false;
            if (!ValueInto(Arg.Value, Pin, TEXT("call ") + Function->GetName() + TEXT(".") + Arg.Key)) return false;
        }
        if (!ApplyPositionalArgs(Call, Function, Statement->ArgList, TEXT("call ") + Function->GetName())) return false;
        if (bPureCall)
        {
            Diag.Note(TEXT("call ") + Function->GetName() + TEXT(": pure, used as a statement; exec chain unchanged"));
        }
        else
        {
            if (!ConnectExec(Tails, Call->GetExecPin(), TEXT("call ") + Function->GetName())) return false;
            Tails = { Call->GetThenPin() };
        }
        for (const TPair<FString, FCardOutSink>& Out : Statement->Outs)
        {
            bool bPinOk = false;
            UEdGraphPin* Value = OutputPin(Call, Out.Key, TEXT("call ") + Function->GetName() + TEXT(".out.") + Out.Key, bPinOk);
            if (!bPinOk) return false;
            if (!EmitAssignment(Out.Value.Name, Value, Tails,
                    TEXT("call ") + Function->GetName() + TEXT(" -> ") + Out.Value.Name, Tails)) return false;
        }
        return true;
    }
};
}

bool BuildCardGraph(UBlueprint* Blueprint, UEdGraph* Graph, const FCardGraph& Spec, FCardDiagnostics& Diag)
{
    if (!Blueprint || !Graph) return false;
    if (Spec.Kind == ECardGraphKind::RawK2) return BuildRawK2Graph(Blueprint, Graph, Spec, Diag);
    if (Spec.Kind != ECardGraphKind::Event && Spec.Kind != ECardGraphKind::Function)
    {
        Diag.Error(Spec.Function + TEXT(": unsupported graph kind"));
        return false;
    }
    FCardGraphAssembler Assembler(Blueprint, Graph, Spec, Diag);
    return Assembler.Run();
}



// ───────────────────────────────────── 通用反射层（不硬编码节点类）

namespace
{
/** 按属性类型分派写入。这是「任意 UK2Node_* 的任意 UPROPERTY 都能赋」的核心。 */
bool SetValueAt(FProperty* Property, void* Addr, const TSharedPtr<FJsonValue>& Value, FString& Error)
{
    if (!Property) { Error = TEXT("null property"); return false; }
    if (!Value.IsValid()) { Error = TEXT("null value"); return false; }

    if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
    {
        // EdGraphPinType 用引脚类型词表写："array:int" / "object:/Script/kards.BaseCardObject"
        if (StructProperty->Struct == TBaseStructure<FEdGraphPinType>::Get())
        {
            FString Spec;
            if (!Value->TryGetString(Spec)) { Error = TEXT("pin type must be a string such as 'array:int'"); return false; }
            FEdGraphPinType Type;
            if (!PinTypeFromSpec(Spec, Type, Error)) return false;
            *static_cast<FEdGraphPinType*>(Addr) = Type;
            return true;
        }
        if (Value->Type == EJson::Object)
        {
            for (const auto& Pair : Value->AsObject()->Values)
            {
                FProperty* Inner = StructProperty->Struct->FindPropertyByName(FName(*Pair.Key));
                if (!Inner) { Error = TEXT("struct ") + StructProperty->Struct->GetName() + TEXT(" has no field ") + Pair.Key; return false; }
                if (!SetValueAt(Inner, Inner->ContainerPtrToValuePtr<void>(Addr), Pair.Value, Error)) return false;
            }
            return true;
        }
        Error = TEXT("struct value must be an object");
        return false;
    }
    if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
    {
        if (Value->Type != EJson::Array) { Error = TEXT("array property needs a JSON array"); return false; }
        FScriptArrayHelper Helper(ArrayProperty, Addr);
        Helper.EmptyValues();
        for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
        {
            const int32 Index = Helper.AddValue();
            if (!SetValueAt(ArrayProperty->Inner, Helper.GetRawPtr(Index), Element, Error)) return false;
        }
        return true;
    }
    if (FBoolProperty* P = CastField<FBoolProperty>(Property)) { P->SetPropertyValue(Addr, Value->AsBool()); return true; }
    if (FIntProperty* P = CastField<FIntProperty>(Property)) { P->SetPropertyValue(Addr, static_cast<int32>(Value->AsNumber())); return true; }
    if (FInt64Property* P = CastField<FInt64Property>(Property)) { P->SetPropertyValue(Addr, static_cast<int64>(Value->AsNumber())); return true; }
    if (FFloatProperty* P = CastField<FFloatProperty>(Property)) { P->SetPropertyValue(Addr, static_cast<float>(Value->AsNumber())); return true; }
    if (FDoubleProperty* P = CastField<FDoubleProperty>(Property)) { P->SetPropertyValue(Addr, Value->AsNumber()); return true; }
    if (FStrProperty* P = CastField<FStrProperty>(Property)) { P->SetPropertyValue(Addr, Value->AsString()); return true; }
    if (FNameProperty* P = CastField<FNameProperty>(Property)) { P->SetPropertyValue(Addr, FName(*Value->AsString())); return true; }
    if (FTextProperty* P = CastField<FTextProperty>(Property)) { P->SetPropertyValue(Addr, FText::FromString(Value->AsString())); return true; }
    if (FEnumProperty* P = CastField<FEnumProperty>(Property))
    {
        UEnum* Enum = P->GetEnum();
        const int64 Index = Enum->GetValueByNameString(Value->AsString());
        if (Index == INDEX_NONE)
        {
            Error = FString::Printf(TEXT("'%s' is not a value of %s"), *Value->AsString(), *Enum->GetName());
            return false;
        }
        P->GetUnderlyingProperty()->SetIntPropertyValue(Addr, Index);
        return true;
    }
    if (FByteProperty* P = CastField<FByteProperty>(Property))
    {
        int64 Number = static_cast<int64>(Value->AsNumber());
        if (P->Enum && Value->Type == EJson::String)
        {
            const int64 Named = P->Enum->GetValueByNameString(Value->AsString());
            if (Named == INDEX_NONE)
            {
                Error = FString::Printf(TEXT("'%s' is not a value of %s"), *Value->AsString(), *P->Enum->GetName());
                return false;
            }
            Number = Named;
        }
        P->SetIntPropertyValue(Addr, Number);
        return true;
    }
    if (FClassProperty* P = CastField<FClassProperty>(Property))
    {
        UClass* Class = ResolveClass(Value->AsString());
        if (!Class) { Error = TEXT("cannot resolve class ") + Value->AsString(); return false; }
        P->SetPropertyValue(Addr, Class);
        return true;
    }
    if (FObjectPropertyBase* P = CastField<FObjectPropertyBase>(Property))
    {
        const FString Path = Value->AsString();
        UObject* Object = Path.IsEmpty() || Path == TEXT("None") ? nullptr : LoadObject<UObject>(nullptr, *Path);
        if (!Object && !Path.IsEmpty() && Path != TEXT("None")) { Error = TEXT("cannot load ") + Path; return false; }
        P->SetObjectPropertyValue(Addr, Object);
        return true;
    }
    Error = FString::Printf(TEXT("property class %s is not supported by the generic setter"), *Property->GetClass()->GetName());
    return false;
}

/** 反射调用节点上的任意成员函数（AddInputPin / SetMacroGraph / AddOptionPinToNode …）。 */
bool CallNodeApi(UEdGraphNode* Node, const FString& FunctionName, const TArray<TSharedPtr<FJsonValue>>& Args, FString& Error)
{
    // ── 垫片：编辑器自己用的那些「非 UFUNCTION」的 C++ 配置方法，反射看不见，只能直调。
    if (FunctionName == TEXT("AddInputPin"))
    {
        if (IK2Node_AddPinInterface* AddPin = Cast<IK2Node_AddPinInterface>(Node)) { AddPin->AddInputPin(); return true; }
        Error = FString::Printf(TEXT("%s does not implement IK2Node_AddPinInterface"), *Node->GetClass()->GetName());
        return false;
    }
    if (FunctionName == TEXT("RemoveInputPin"))
    {
        if (Args.Num() != 1) { Error = TEXT("RemoveInputPin needs the pin name"); return false; }
        if (IK2Node_AddPinInterface* AddPin = Cast<IK2Node_AddPinInterface>(Node))
        {
            if (UEdGraphPin* Pin = Node->FindPin(FName(*Args[0]->AsString()))) { AddPin->RemoveInputPin(Pin); return true; }
            Error = TEXT("no pin ") + Args[0]->AsString();
            return false;
        }
        Error = TEXT("node does not implement IK2Node_AddPinInterface");
        return false;
    }
    // ReconstructNode：改完决定引脚形状的 UPROPERTY 之后必须调它，引脚才会重建。
    // 它是普通 C++ 方法不是 UFUNCTION，所以只能垫片。
    if (FunctionName == TEXT("ReconstructNode")) { Node->ReconstructNode(); return true; }
    if (FunctionName == TEXT("BreakAllPinLinks") || FunctionName == TEXT("BreakAllNodeLinks"))
    {
        Node->BreakAllNodeLinks();
        return true;
    }
    if (FunctionName == TEXT("SetEnum"))
    {
        UK2Node_Select* Select = Cast<UK2Node_Select>(Node);
        if (!Select) { Error = TEXT("SetEnum only exists on K2Node_Select"); return false; }
        if (Args.Num() != 1) { Error = TEXT("SetEnum needs the enum path"); return false; }
        const FString EnumPath = Args[0]->AsString();
        UEnum* Found = LoadObject<UEnum>(nullptr, *EnumPath);
        if (!Found && !EnumPath.Contains(TEXT("/")))
        {
            for (TObjectIterator<UEnum> It; It; ++It)
            {
                if (It->GetName() == EnumPath) { Found = *It; break; }
            }
        }
        if (!Found) { Error = TEXT("cannot resolve enum ") + EnumPath; return false; }
        Select->SetEnum(Found, /*bForceRegenerate*/ true);
        return true;
    }
    if (FunctionName == TEXT("AddExtraFlags") || FunctionName == TEXT("ClearExtraFlags"))
    {
        UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
        if (!Entry) { Error = FunctionName + TEXT(" only exists on K2Node_FunctionEntry"); return false; }
        const int64 Flags = Args.Num() ? static_cast<int64>(Args[0]->AsNumber()) : 0;
        if (FunctionName == TEXT("AddExtraFlags")) Entry->AddExtraFlags(Flags);
        else Entry->ClearExtraFlags(Flags);
        return true;
    }

    UFunction* Function = Node->GetClass()->FindFunctionByName(FName(*FunctionName));
    if (!Function)
    {
        TArray<FString> Available;
        for (TFieldIterator<UFunction> It(Node->GetClass()); It; ++It)
        {
            if (It->HasAnyFunctionFlags(FUNC_Public | FUNC_Protected)) Available.Add(It->GetName());
        }
        Available.Sort();
        Error = FString::Printf(TEXT("node %s has no function '%s'; available: %s"),
            *Node->GetClass()->GetName(), *FunctionName, *FString::Join(Available, TEXT(", ")));
        return false;
    }
    TArray<uint8> Buffer;
    Buffer.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
    int32 Index = 0;
    for (TFieldIterator<FProperty> It(Function); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property->HasAnyPropertyFlags(CPF_Parm)) continue;
        if (Index >= Args.Num()) break;
        if (!SetValueAt(Property, Property->ContainerPtrToValuePtr<void>(Buffer.GetData()), Args[Index], Error))
        {
            Error = FunctionName + TEXT(".") + Property->GetName() + TEXT(": ") + Error;
            return false;
        }
        ++Index;
    }
    if (Index < Args.Num())
    {
        Error = FString::Printf(TEXT("%s: %d args given but the function takes %d"), *FunctionName, Args.Num(), Index);
        return false;
    }
    Node->ProcessEvent(Function, Buffer.GetData());
    return true;
}
/** JSON 标量转文本（本地版本，CardSpec.cpp 里那个是文件内静态的）。 */
FString RawJsonToText(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid()) return FString();
    if (Value->Type == EJson::String) return Value->AsString();
    if (Value->Type == EJson::Boolean) return Value->AsBool() ? TEXT("true") : TEXT("false");
    if (Value->Type == EJson::Number)
    {
        const double Number = Value->AsNumber();
        if (FMath::FloorToDouble(Number) == Number && FMath::Abs(Number) < 9.0e15)
        {
            return FString::Printf(TEXT("%lld"), static_cast<long long>(Number));
        }
        return FString::SanitizeFloat(Number);
    }
    return FString();
}

/** 执行 raw["api"] 里 when 匹配的成员函数调用。 */
bool RunNodeApi(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Raw, const TCHAR* When,
    const FString& NodeId, FCardDiagnostics& Diag)
{
    const TArray<TSharedPtr<FJsonValue>>* Api = nullptr;
    if (!Raw->TryGetArrayField(TEXT("api"), Api) || !Api) return true;
    for (const TSharedPtr<FJsonValue>& Item : *Api)
    {
        if (!Item.IsValid() || Item->Type != EJson::Object)
        {
            Diag.Error(NodeId + TEXT(".api: entry is not an object"));
            return false;
        }
        const TSharedPtr<FJsonObject> Call = Item->AsObject();
        FString FunctionName;
        FString Phase = TEXT("after");
        Call->TryGetStringField(TEXT("fn"), FunctionName);
        Call->TryGetStringField(TEXT("when"), Phase);
        if (FunctionName.IsEmpty())
        {
            Diag.Error(NodeId + TEXT(".api: entry needs 'fn'"));
            return false;
        }
        if (!Phase.Equals(When, ESearchCase::IgnoreCase)) continue;

        TArray<TSharedPtr<FJsonValue>> Args;
        const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
        if (Call->TryGetArrayField(TEXT("args"), ArgsArray) && ArgsArray) Args = *ArgsArray;

        FString Error;
        if (!CallNodeApi(Node, FunctionName, Args, Error))
        {
            Diag.Error(NodeId + TEXT(".api.") + FunctionName + TEXT(": ") + Error);
            return false;
        }
    }
    return true;
}
}   // namespace

// ───────────────────────────────────── 编辑器会话原语（供 AI 直接操作节点图）

UEdGraphNode* KardsK2CreateNode(UEdGraph* Graph, const FString& ClassPath, int32 PosX, int32 PosY, FString& Error)
{
    return KardsK2CreateNodeEx(Graph, ClassPath, PosX, PosY, FString(), FString(), FString(), FString(), FString(), Error);
}

UEdGraphNode* KardsK2CreateNodeEx(UEdGraph* Graph, const FString& ClassPath, int32 PosX, int32 PosY,
    const FString& Function, const FString& OwnerClass, const FString& Variable,
    const FString& Macro, const FString& TargetClass, FString& Error)
{
    if (!Graph) { Error = TEXT("no graph"); return nullptr; }
    FString Path = ClassPath;
    if (!Path.Contains(TEXT("/"))) Path = TEXT("/Script/BlueprintGraph.") + Path;
    UClass* NodeClass = ResolveClass(Path);
    if (!NodeClass) { Error = TEXT("unknown node class ") + ClassPath; return nullptr; }
    if (!NodeClass->IsChildOf(UK2Node::StaticClass())) { Error = ClassPath + TEXT(" is not a UK2Node"); return nullptr; }
    UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
    if (!Node) { Error = TEXT("cannot instantiate ") + ClassPath; return nullptr; }
    Graph->AddNode(Node, /*bUserAction*/ true, /*bSelectNewNode*/ false);
    Node->CreateNewGuid();
    Node->NodePosX = PosX;
    Node->NodePosY = PosY;

    // 这些必须在 AllocateDefaultPins 之前设，否则引脚形状是错的（甚至没有引脚）
    UBlueprint* Blueprint = Cast<UBlueprint>(Graph->GetOuter());
    UClass* Owner = OwnerClass.IsEmpty()
        ? (Blueprint ? Blueprint->ParentClass.Get() : nullptr)
        : ResolveClass(OwnerClass);
    if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
    {
        if (Function.IsEmpty()) { Error = TEXT("K2Node_CallFunction needs 'function'"); return nullptr; }
        if (!Owner) { Error = TEXT("cannot resolve owner class ") + OwnerClass; return nullptr; }
        UFunction* Found = Owner->FindFunctionByName(FName(*Function));
        if (!Found) { Error = Owner->GetName() + TEXT(" has no function '") + Function + TEXT("'"); return nullptr; }
        Call->SetFromFunction(Found);
    }
    else if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
    {
        if (Function.IsEmpty()) { Error = TEXT("K2Node_Event needs 'function'"); return nullptr; }
        if (!Owner) { Error = TEXT("cannot resolve owner class ") + OwnerClass; return nullptr; }
        UFunction* Found = Owner->FindFunctionByName(FName(*Function));
        if (!Found) { Error = Owner->GetName() + TEXT(" has no event '") + Function + TEXT("'"); return nullptr; }
        Event->EventReference.SetExternalMember(Found->GetFName(), Owner);
        Event->bOverrideFunction = true;
    }
    else if (UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
    {
        if (Variable.IsEmpty()) { Error = TEXT("variable node needs 'variable'"); return nullptr; }
        Var->VariableReference.SetSelfMember(FName(*Variable));
    }
    else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
    {
        UEdGraph* MacroGraph = LoadObject<UEdGraph>(nullptr, *Macro);
        if (!MacroGraph) { Error = TEXT("cannot load macro graph ") + Macro; return nullptr; }
        MacroNode->SetMacroGraph(MacroGraph);
    }
    else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
    {
        UClass* Target = ResolveClass(TargetClass);
        if (!Target) { Error = TEXT("cannot resolve cast class ") + TargetClass; return nullptr; }
        CastNode->TargetType = Target;
    }

    Node->AllocateDefaultPins();
    return Node;
}

bool KardsK2SetProperty(UEdGraphNode* Node, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& Error)
{
    if (!Node) { Error = TEXT("no node"); return false; }
    FProperty* Property = Node->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!Property)
    {
        TArray<FString> Available;
        for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It) Available.Add(It->GetName());
        Available.Sort();
        Error = FString::Printf(TEXT("%s has no property '%s'; available: %s"),
            *Node->GetClass()->GetName(), *PropertyName, *FString::Join(Available, TEXT(", ")));
        return false;
    }
    return SetValueAt(Property, Property->ContainerPtrToValuePtr<void>(Node), Value, Error);
}

bool KardsK2CallApi(UEdGraphNode* Node, const FString& FunctionName, const TArray<TSharedPtr<FJsonValue>>& Args, FString& Error)
{
    return CallNodeApi(Node, FunctionName, Args, Error);
}

FString KardsK2DumpGraph(UEdGraph* Graph)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    if (!Graph) { Root->SetStringField(TEXT("error"), TEXT("no graph")); }
    else
    {
        Root->SetStringField(TEXT("graph"), Graph->GetName());
        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (int32 Index = 0; Index < Graph->Nodes.Num(); ++Index)
        {
            UEdGraphNode* Node = Graph->Nodes[Index];
            if (!Node) continue;
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("id"), FString::Printf(TEXT("n%d"), Index));
            Item->SetStringField(TEXT("class"), Node->GetClass()->GetName());
            Item->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
            Item->SetNumberField(TEXT("x"), Node->NodePosX);
            Item->SetNumberField(TEXT("y"), Node->NodePosY);
            TArray<TSharedPtr<FJsonValue>> Pins;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                auto PinJson = MakeShared<FJsonObject>();
                PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
                PinJson->SetStringField(TEXT("dir"), Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"));
                PinJson->SetStringField(TEXT("type"), PinTypeToString(Pin->PinType));
                if (!Pin->DefaultValue.IsEmpty()) PinJson->SetStringField(TEXT("default"), Pin->DefaultValue);
                TArray<TSharedPtr<FJsonValue>> Links;
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
                    if (!Other) continue;
                    int32 OtherIndex = INDEX_NONE;
                    for (int32 Scan = 0; Scan < Graph->Nodes.Num(); ++Scan)
                    {
                        if (Graph->Nodes[Scan] == Other) { OtherIndex = Scan; break; }
                    }
                    Links.Add(MakeShared<FJsonValueString>(
                        FString::Printf(TEXT("n%d.%s"), OtherIndex, *Linked->PinName.ToString())));
                }
                if (Links.Num()) PinJson->SetArrayField(TEXT("linkedTo"), Links);
                Pins.Add(MakeShared<FJsonValueObject>(PinJson));
            }
            Item->SetArrayField(TEXT("pins"), Pins);
            Nodes.Add(MakeShared<FJsonValueObject>(Item));
        }
        Root->SetArrayField(TEXT("nodes"), Nodes);
    }
    FString Out;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Root, Writer);
    return Out;
}

/** 在可编辑引脚基类节点上加一个用户定义引脚（自定义事件/函数签名靠它）。 */
bool KardsK2AddPin(UEdGraphNode* Node, const FString& PinName, const FString& PinTypeSpec,
    bool bOutput, FString& Error)
{
    if (!Node) { Error = TEXT("no node"); return false; }
    UK2Node_EditablePinBase* Editable = Cast<UK2Node_EditablePinBase>(Node);
    if (!Editable)
    {
        Error = FString::Printf(TEXT("%s is not a UK2Node_EditablePinBase; cannot add pins"), *Node->GetClass()->GetName());
        return false;
    }
    FEdGraphPinType PinType;
    if (!PinTypeFromSpec(PinTypeSpec, PinType, Error)) return false;
    UEdGraphPin* Pin = Editable->CreateUserDefinedPin(FName(*PinName), PinType,
        bOutput ? EGPD_Output : EGPD_Input, /*bUseUniqueName*/ false);
    if (!Pin) { Error = TEXT("CreateUserDefinedPin failed for ") + PinName; return false; }
    return true;
}

/** 新建一张本地函数图（不是覆写父类，是全新的 UFunction）。返回入口节点，OutResult 接结果节点。 */
UEdGraphNode* KardsK2AddFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName, UEdGraphNode*& OutResult, FString& Error)
{
    OutResult = nullptr;
    if (!Blueprint) { Error = TEXT("no blueprint"); return nullptr; }
    for (UEdGraph* Existing : Blueprint->FunctionGraphs)
    {
        if (Existing && Existing->GetName() == FunctionName)
        {
            Error = TEXT("function graph already exists: ") + FunctionName;
            return nullptr;
        }
    }
    UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*FunctionName),
        UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!Graph) { Error = TEXT("CreateNewGraph failed"); return nullptr; }
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated*/ true, /*SignatureFromClass*/ nullptr);
    TArray<UK2Node_FunctionEntry*> Entries;
    Graph->GetNodesOfClass(Entries);
    if (Entries.Num() != 1)
    {
        Error = FString::Printf(TEXT("expected 1 function entry, got %d"), Entries.Num());
        return nullptr;
    }
    UK2Node_FunctionEntry* Entry = Entries[0];
    Entry->ClearExtraFlags(FUNC_BlueprintPure | FUNC_Const | FUNC_Static);

    // AddFunctionGraph<UClass> 不一定给结果节点；没有就自己建一个（出参挂在它上面）
    TArray<UK2Node_FunctionResult*> Results;
    Graph->GetNodesOfClass(Results);
    UK2Node_FunctionResult* Result = Results.Num() > 0 ? Results[0] : nullptr;
    if (!Result)
    {
        FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
        Result = Creator.CreateNode(false);
        Result->FunctionReference = Entry->FunctionReference;
        Creator.Finalize();
        Result->bIsEditable = true;
        Result->NodePosX = Entry->NodePosX + 700;
        Result->NodePosY = Entry->NodePosY;
    }
    OutResult = Result;
    return Entry;
}

/** 在事件图里建一个自定义事件节点。 */
UEdGraphNode* KardsK2AddCustomEvent(UEdGraph* Graph, const FString& EventName, FString& Error)
{
    if (!Graph) { Error = TEXT("no graph"); return nullptr; }
    if (EventName.IsEmpty()) { Error = TEXT("custom event needs a name"); return nullptr; }
    FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
    UK2Node_CustomEvent* Node = Creator.CreateNode(false);
    Node->CustomFunctionName = FName(*EventName);
    Creator.Finalize();
    Node->bIsEditable = true;
    return Node;
}

bool BuildRawK2Graph(UBlueprint* Blueprint, UEdGraph* Graph, const FCardGraph& Spec, FCardDiagnostics& Diag)
{
    if (!Blueprint || !Graph) return false;
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    const FString Tag = Spec.Function.IsEmpty() ? TEXT("k2") : Spec.Function;
    auto Fail = [&](const FString& M) { Diag.Error(Tag + TEXT(": ") + M); return false; };

    TMap<FString, UEdGraphNode*> ById;

    // ── 1) 按官方类名建节点
    for (const FCardNodeSpec& N : Spec.Nodes)
    {
        FString ClassPath = N.Class;
        if (!ClassPath.Contains(TEXT("/"))) ClassPath = TEXT("/Script/BlueprintGraph.") + ClassPath;
        UClass* NodeClass = ResolveClass(ClassPath);
        if (!NodeClass)
        {
            return Fail(TEXT("unknown node class '") + N.Class + TEXT("' (tried ") + ClassPath + TEXT(")"));
        }
        if (!NodeClass->IsChildOf(UK2Node::StaticClass()))
        {
            return Fail(N.Class + TEXT(" is not a UK2Node"));
        }

        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
        if (!Node) return Fail(TEXT("cannot instantiate ") + N.Class);
        Graph->AddNode(Node, /*bUserAction*/ true, /*bSelectNewNode*/ false);
        Node->CreateNewGuid();
        Node->NodePosX = N.PosX;
        Node->NodePosY = N.PosY;
        if (!N.Comment.IsEmpty()) Node->NodeComment = N.Comment;

        // 类特有属性必须在 AllocateDefaultPins 之前设置
        UClass* Owner = N.OwnerClass.IsEmpty() ? Blueprint->ParentClass.Get() : ResolveClass(N.OwnerClass);
        if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
        {
            if (!Owner) return Fail(N.Id + TEXT(": cannot resolve owner class ") + N.OwnerClass);
            UFunction* Function = Owner->FindFunctionByName(FName(*N.Function));
            if (!Function) return Fail(N.Id + TEXT(": ") + Owner->GetName() + TEXT(" has no function '") + N.Function + TEXT("'") + NearMatches(Owner, N.Function));
            Call->SetFromFunction(Function);
        }
        else if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
        {
            if (!Owner) return Fail(N.Id + TEXT(": cannot resolve owner class ") + N.OwnerClass);
            UFunction* Function = Owner->FindFunctionByName(FName(*N.Function));
            if (!Function) return Fail(N.Id + TEXT(": ") + Owner->GetName() + TEXT(" has no event '") + N.Function + TEXT("'") + NearMatches(Owner, N.Function));
            Event->EventReference.SetExternalMember(Function->GetFName(), Owner);
            Event->bOverrideFunction = true;
        }
        else if (UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
        {
            if (N.Variable.IsEmpty()) return Fail(N.Id + TEXT(": variable node needs 'variable'"));
            Variable->VariableReference.SetSelfMember(FName(*N.Variable));
        }
        else if (UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
        {
            UEdGraph* MacroGraph = LoadObject<UEdGraph>(nullptr, *N.Macro);
            if (!MacroGraph) return Fail(N.Id + TEXT(": cannot load macro graph ") + N.Macro);
            Macro->SetMacroGraph(MacroGraph);
        }
        else if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
        {
            UClass* TargetClass = ResolveClass(N.TargetClass);
            if (!TargetClass) return Fail(N.Id + TEXT(": cannot resolve cast class ") + N.TargetClass);
            CastNode->TargetType = TargetClass;
        }

        // ── 通用 props：任意 UPROPERTY（在 AllocateDefaultPins 之前，因为很多节点的
        //    引脚形状由属性决定：K2Node_Select 的 NumOptionPins、Switch 的枚举、Timeline 等）
        FString Error;
        if (N.Raw.IsValid())
        {
            const TSharedPtr<FJsonObject>* Props = nullptr;
            if (N.Raw->TryGetObjectField(TEXT("props"), Props) && Props && Props->IsValid())
            {
                for (const auto& Pair : (*Props)->Values)
                {
                    FProperty* Property = NodeClass->FindPropertyByName(FName(*Pair.Key));
                    if (!Property)
                    {
                        return Fail(N.Id + TEXT(": ") + NodeClass->GetName() + TEXT(" has no property '") + Pair.Key + TEXT("'"));
                    }
                    if (!SetValueAt(Property, Property->ContainerPtrToValuePtr<void>(Node), Pair.Value, Error))
                    {
                        return Fail(N.Id + TEXT(".props.") + Pair.Key + TEXT(": ") + Error);
                    }
                }
            }
            if (!RunNodeApi(Node, N.Raw, TEXT("before"), N.Id, Diag)) return false;
        }

        Node->AllocateDefaultPins();

        if (N.Raw.IsValid() && !RunNodeApi(Node, N.Raw, TEXT("after"), N.Id, Diag)) return false;

        // ── pins：引脚级操作（覆盖引脚类型 / 写默认值），通配符引脚靠这个定型
        if (N.Raw.IsValid())
        {
            const TSharedPtr<FJsonObject>* Pins = nullptr;
            if (N.Raw->TryGetObjectField(TEXT("pins"), Pins) && Pins && Pins->IsValid())
            {
                const TSharedPtr<FJsonObject>* Types = nullptr;
                if ((*Pins)->TryGetObjectField(TEXT("types"), Types) && Types && Types->IsValid())
                {
                    for (const auto& Pair : (*Types)->Values)
                    {
                        UEdGraphPin* Pin = Node->FindPin(FName(*Pair.Key));
                        if (!Pin) return Fail(N.Id + TEXT(".pins.types: no pin '") + Pair.Key + TEXT("'"));
                        FEdGraphPinType Type;
                        if (!PinTypeFromSpec(Pair.Value->AsString(), Type, Error))
                        {
                            return Fail(N.Id + TEXT(".pins.types.") + Pair.Key + TEXT(": ") + Error);
                        }
                        Pin->PinType = Type;
                    }
                }
                const TSharedPtr<FJsonObject>* PinDefaults = nullptr;
                if ((*Pins)->TryGetObjectField(TEXT("defaults"), PinDefaults) && PinDefaults && PinDefaults->IsValid())
                {
                    for (const auto& Pair : (*PinDefaults)->Values)
                    {
                        UEdGraphPin* Pin = Node->FindPin(FName(*Pair.Key));
                        if (!Pin) return Fail(N.Id + TEXT(".pins.defaults: no pin '") + Pair.Key + TEXT("'"));
                        Schema->TrySetDefaultValue(*Pin, RawJsonToText(Pair.Value));
                    }
                }
            }
        }

        for (const TPair<FString, FString>& D : N.Defaults)
        {
            UEdGraphPin* Pin = Node->FindPin(FName(*D.Key));
            if (!Pin)
            {
                TArray<FString> Available;
                for (UEdGraphPin* P : Node->Pins) if (P) Available.Add(P->PinName.ToString());
                return Fail(N.Id + TEXT(": no pin '") + D.Key + TEXT("'; pins: ") + FString::Join(Available, TEXT(", ")));
            }
            Schema->TrySetDefaultValue(*Pin, D.Value);
        }
        ById.Add(N.Id, Node);
    }

    // ── 2) 按引脚名连线
    for (const FCardLinkSpec& L : Spec.Links)
    {
        UEdGraphNode** FromNode = ById.Find(L.FromId);
        UEdGraphNode** ToNode = ById.Find(L.ToId);
        if (!FromNode || !ToNode)
        {
            return Fail(TEXT("link references unknown node id: ") + L.FromId + TEXT(" -> ") + L.ToId);
        }
        UEdGraphPin* From = (*FromNode)->FindPin(FName(*L.FromPin));
        UEdGraphPin* To = (*ToNode)->FindPin(FName(*L.ToPin));
        if (!From || !To)
        {
            auto Pins = [](UEdGraphNode* N)
            {
                TArray<FString> Out;
                for (UEdGraphPin* P : N->Pins) if (P) Out.Add(P->PinName.ToString());
                return FString::Join(Out, TEXT(", "));
            };
            return Fail(FString::Printf(TEXT("link %s.%s -> %s.%s: missing pin (%s pins: %s | %s pins: %s)"),
                *L.FromId, *L.FromPin, *L.ToId, *L.ToPin,
                *L.FromId, *Pins(*FromNode), *L.ToId, *Pins(*ToNode)));
        }
        const FPinConnectionResponse Response = Schema->CanCreateConnection(From, To);
        if (Response.Response != CONNECT_RESPONSE_MAKE)
        {
            return Fail(FString::Printf(TEXT("link %s.%s -> %s.%s refused: %s"),
                *L.FromId, *L.FromPin, *L.ToId, *L.ToPin, *Response.Message.ToString()));
        }
        if (!Schema->TryCreateConnection(From, To))
        {
            return Fail(FString::Printf(TEXT("link %s.%s -> %s.%s did not take"), *L.FromId, *L.FromPin, *L.ToId, *L.ToPin));
        }
    }

    Diag.Note(FString::Printf(TEXT("%s: %d official K2 nodes, %d links"), *Tag, Spec.Nodes.Num(), Spec.Links.Num()));
    return true;
}
