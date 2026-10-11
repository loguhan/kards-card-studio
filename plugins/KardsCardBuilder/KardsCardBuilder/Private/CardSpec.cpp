#include "CardSpec.h"

#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace
{
bool GetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& Out)
{
    return Obj.IsValid() && Obj->TryGetStringField(Key, Out);
}

FString GetStringOr(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Fallback)
{
    FString Value;
    return GetString(Obj, Key, Value) ? Value : Fallback;
}

ECardExprKind ExprKindFromName(const FString& Name)
{
    if (Name == TEXT("self")) return ECardExprKind::Self;
    if (Name == TEXT("param")) return ECardExprKind::Param;
    if (Name == TEXT("local")) return ECardExprKind::Local;
    if (Name == TEXT("member")) return ECardExprKind::Member;
    if (Name == TEXT("call")) return ECardExprKind::Call;
    if (Name == TEXT("literal")) return ECardExprKind::Literal;
    if (Name == TEXT("cast")) return ECardExprKind::Cast;
    if (Name == TEXT("arrayMake")) return ECardExprKind::ArrayMake;
    if (Name == TEXT("property")) return ECardExprKind::Property;
    return ECardExprKind::Literal;
}

ECardStmtKind StmtKindFromName(const FString& Name)
{
    if (Name == TEXT("call")) return ECardStmtKind::Call;
    if (Name == TEXT("if")) return ECardStmtKind::If;
    if (Name == TEXT("set")) return ECardStmtKind::Set;
    if (Name == TEXT("localSet")) return ECardStmtKind::LocalSet;
    if (Name == TEXT("localDecl")) return ECardStmtKind::LocalDecl;
    if (Name == TEXT("seq")) return ECardStmtKind::Seq;
    if (Name == TEXT("cast")) return ECardStmtKind::Cast;
    if (Name == TEXT("result")) return ECardStmtKind::Result;
    if (Name == TEXT("foreach")) return ECardStmtKind::Foreach;
    return ECardStmtKind::Call;
}

FString JsonValueToText(const TSharedPtr<FJsonValue>& Value)
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

bool ParseExpr(const TSharedPtr<FJsonValue>& Value, FCardExprPtr& Out, FString& Error);

bool ParseExprObject(const TSharedPtr<FJsonObject>& Obj, FCardExprPtr& Out, FString& Error)
{
    if (!Obj.IsValid()) { Error = TEXT("expression is not an object"); return false; }
    FString KindName;
    if (!Obj->TryGetStringField(TEXT("kind"), KindName))
    {
        // Shorthand: a bare string means a literal, an object with "function" means a call.
        Error = TEXT("expression is missing 'kind'");
        return false;
    }
    TSharedPtr<FCardExpr> Expr = MakeShared<FCardExpr>();
    Expr->Kind = ExprKindFromName(KindName);
    Obj->TryGetStringField(TEXT("name"), Expr->Name);
    Obj->TryGetStringField(TEXT("owner"), Expr->OwnerClass);
    Obj->TryGetStringField(TEXT("pin"), Expr->ResultPin);
    Obj->TryGetStringField(TEXT("type"), Expr->LiteralType);
    Obj->TryGetStringField(TEXT("function"), Expr->Name);
    if (Expr->Kind == ECardExprKind::Cast && Expr->Name.IsEmpty())
    {
        Obj->TryGetStringField(TEXT("class"), Expr->Name);
    }
    if (Obj->HasField(TEXT("value")))
    {
        const TSharedPtr<FJsonValue> Raw = Obj->TryGetField(TEXT("value"));
        if (Raw.IsValid() && Raw->Type == EJson::String && Expr->Kind == ECardExprKind::Literal)
        {
            Expr->LiteralValue = Raw->AsString();
        }
        else if (Raw.IsValid() && Raw->Type == EJson::Object)
        {
            if (!ParseExpr(Raw, Expr->Target, Error)) return false;
        }
        else
        {
            Expr->LiteralValue = JsonValueToText(Raw);
        }
    }
    if (Obj->HasField(TEXT("target")))
    {
        if (!ParseExpr(Obj->TryGetField(TEXT("target")), Expr->Target, Error)) return false;
    }
    const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
    if (Obj->TryGetObjectField(TEXT("args"), ArgsObj) && ArgsObj && ArgsObj->IsValid())
    {
        for (const auto& Pair : (*ArgsObj)->Values)
        {
            FCardExprPtr Arg;
            if (!ParseExpr(Pair.Value, Arg, Error)) { Error = Pair.Key + TEXT(": ") + Error; return false; }
            Expr->Args.Emplace(Pair.Key, Arg);
        }
    }
    // args 写成 JSON 数组时按位置绑定（LLM 经常这么写）
    const TArray<TSharedPtr<FJsonValue>>* ArgsArr = nullptr;
    if (Obj->TryGetArrayField(TEXT("args"), ArgsArr) && ArgsArr)
    {
        for (const TSharedPtr<FJsonValue>& Item : *ArgsArr)
        {
            FCardExprPtr Arg;
            if (!ParseExpr(Item, Arg, Error)) return false;
            Expr->ArgList.Add(Arg);
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* ItemsArray = nullptr;
    if (Obj->TryGetArrayField(TEXT("items"), ItemsArray) && ItemsArray)
    {
        for (const TSharedPtr<FJsonValue>& Item : *ItemsArray)
        {
            FCardExprPtr Element;
            if (!ParseExpr(Item, Element, Error)) return false;
            Expr->Items.Add(Element);
        }
    }
    Out = Expr;
    return true;
}

bool ParseExpr(const TSharedPtr<FJsonValue>& Value, FCardExprPtr& Out, FString& Error)
{
    if (!Value.IsValid()) { Error = TEXT("null expression"); return false; }
    if (Value->Type == EJson::Object)
    {
        return ParseExprObject(Value->AsObject(), Out, Error);
    }
    // Bare scalars are literals; the type is inferred from the JSON type.
    TSharedPtr<FCardExpr> Expr = MakeShared<FCardExpr>();
    Expr->Kind = ECardExprKind::Literal;
    if (Value->Type == EJson::Boolean) Expr->LiteralType = TEXT("bool");
    else if (Value->Type == EJson::Number) Expr->LiteralType = TEXT("int");
    else if (Value->Type == EJson::String) Expr->LiteralType = TEXT("string");
    else { Error = TEXT("unsupported bare literal"); return false; }
    Expr->LiteralValue = JsonValueToText(Value);
    Out = Expr;
    return true;
}

bool ParseStmtList(const TArray<TSharedPtr<FJsonValue>>& Array, TArray<FCardStmtPtr>& Out, FString& Error);

bool ParseStmt(const TSharedPtr<FJsonObject>& Obj, FCardStmtPtr& Out, FString& Error)
{
    if (!Obj.IsValid()) { Error = TEXT("statement is not an object"); return false; }
    FString KindName;
    if (!Obj->TryGetStringField(TEXT("kind"), KindName))
    {
        Error = TEXT("statement is missing 'kind'");
        return false;
    }
    TSharedPtr<FCardStmt> Stmt = MakeShared<FCardStmt>();
    Stmt->Kind = StmtKindFromName(KindName);
    Obj->TryGetStringField(TEXT("name"), Stmt->Name);
    Obj->TryGetStringField(TEXT("owner"), Stmt->OwnerClass);
    Obj->TryGetStringField(TEXT("var"), Stmt->Name);
    Obj->TryGetStringField(TEXT("function"), Stmt->Name);
    Obj->TryGetStringField(TEXT("type"), Stmt->DeclType);
    if (Obj->HasField(TEXT("target")) && !ParseExpr(Obj->TryGetField(TEXT("target")), Stmt->Target, Error)) return false;
    if (Obj->HasField(TEXT("array")) && !ParseExpr(Obj->TryGetField(TEXT("array")), Stmt->Target, Error)) return false;
    if (Obj->HasField(TEXT("cond")) && !ParseExpr(Obj->TryGetField(TEXT("cond")), Stmt->Value, Error)) return false;
    if (Obj->HasField(TEXT("value")) && !ParseExpr(Obj->TryGetField(TEXT("value")), Stmt->Value, Error)) return false;
    if (Obj->HasField(TEXT("macro")))
    {
        FString Macro;
        Obj->TryGetStringField(TEXT("macro"), Macro);
        Stmt->OwnerClass = Macro;   // Foreach reuses OwnerClass for the macro graph path
    }
    const TSharedPtr<FJsonObject>* ArgsObj = nullptr;
    if (Obj->TryGetObjectField(TEXT("args"), ArgsObj) && ArgsObj && ArgsObj->IsValid())
    {
        for (const auto& Pair : (*ArgsObj)->Values)
        {
            FCardExprPtr Arg;
            if (!ParseExpr(Pair.Value, Arg, Error)) { Error = Pair.Key + TEXT(": ") + Error; return false; }
            Stmt->Args.Emplace(Pair.Key, Arg);
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* ArgsArr = nullptr;
    if (Obj->TryGetArrayField(TEXT("args"), ArgsArr) && ArgsArr)
    {
        for (const TSharedPtr<FJsonValue>& Item : *ArgsArr)
        {
            FCardExprPtr Arg;
            if (!ParseExpr(Item, Arg, Error)) return false;
            Stmt->ArgList.Add(Arg);
        }
    }
    const TSharedPtr<FJsonObject>* OutsObj = nullptr;
    if (Obj->TryGetObjectField(TEXT("outs"), OutsObj) && OutsObj && OutsObj->IsValid())
    {
        for (const auto& Pair : (*OutsObj)->Values)
        {
            FCardOutSink Sink;
            FString SinkText;
            if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
            {
                SinkText = Pair.Value->AsString();
            }
            else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> SinkObj = Pair.Value->AsObject();
                FString VarName;
                if (SinkObj->TryGetStringField(TEXT("var"), VarName)) { Sink.bToLocal = false; Sink.Name = VarName; }
                else if (SinkObj->TryGetStringField(TEXT("local"), VarName)) { Sink.bToLocal = true; Sink.Name = VarName; }
            }
            if (Sink.Name.IsEmpty() && !SinkText.IsEmpty())
            {
                Sink.Name = SinkText.StartsWith(TEXT("local:")) ? SinkText.RightChop(6) : SinkText;
                Sink.bToLocal = SinkText.StartsWith(TEXT("local:"));
            }
            if (Sink.Name.IsEmpty()) { Error = TEXT("outs entry for ") + Pair.Key + TEXT(" has no destination"); return false; }
            Stmt->Outs.Emplace(Pair.Key, Sink);
        }
    }
    // Shorthand: "result" captures the function's return value into a variable.
    if (Stmt->Outs.Num() == 0 && Obj->HasField(TEXT("result")))
    {
        FCardOutSink Sink;
        const FString Raw = JsonValueToText(Obj->TryGetField(TEXT("result")));
        Sink.bToLocal = Raw.StartsWith(TEXT("local:"));
        Sink.Name = Sink.bToLocal ? Raw.RightChop(6) : Raw;
        if (Sink.Name.IsEmpty()) { Error = TEXT("'result' has no destination variable"); return false; }
        Stmt->Outs.Emplace(TEXT("ReturnValue"), Sink);
    }
    const TArray<TSharedPtr<FJsonValue>>* ThenArray = nullptr;
    if (Obj->TryGetArrayField(TEXT("then"), ThenArray) && ThenArray)
    {
        if (!ParseStmtList(*ThenArray, Stmt->Then, Error)) return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* ElseArray = nullptr;
    if (Obj->TryGetArrayField(TEXT("else"), ElseArray) && ElseArray)
    {
        if (!ParseStmtList(*ElseArray, Stmt->Else, Error)) return false;
    }
    Out = Stmt;
    return true;
}

bool ParseStmtList(const TArray<TSharedPtr<FJsonValue>>& Array, TArray<FCardStmtPtr>& Out, FString& Error)
{
    for (const TSharedPtr<FJsonValue>& Item : Array)
    {
        if (!Item.IsValid() || Item->Type != EJson::Object)
        {
            Error = TEXT("statement list contains a non-object");
            return false;
        }
        FCardStmtPtr Stmt;
        if (!ParseStmt(Item->AsObject(), Stmt, Error)) return false;
        Out.Add(Stmt);
    }
    return true;
}
}

FString PinTypeToString(const FEdGraphPinType& Type)
{
    FString Base;
    const FName Category = Type.PinCategory;
    if (Category == UEdGraphSchema_K2::PC_Boolean) Base = TEXT("bool");
    else if (Category == UEdGraphSchema_K2::PC_Byte)
    {
        if (const UEnum* Enum = Cast<UEnum>(Type.PinSubCategoryObject.Get()))
        {
            Base = TEXT("enum:") + Enum->GetPathName();
        }
        else
        {
            Base = TEXT("byte");
        }
    }
    else if (Category == UEdGraphSchema_K2::PC_Int) Base = TEXT("int");
    else if (Category == UEdGraphSchema_K2::PC_Int64) Base = TEXT("int64");
    else if (Category == UEdGraphSchema_K2::PC_Real)
    {
        Base = Type.PinSubCategory == UEdGraphSchema_K2::PC_Double ? TEXT("double") : TEXT("float");
    }
    else if (Category == UEdGraphSchema_K2::PC_Float) Base = TEXT("float");
    else if (Category == UEdGraphSchema_K2::PC_Double) Base = TEXT("double");
    else if (Category == UEdGraphSchema_K2::PC_String) Base = TEXT("string");
    else if (Category == UEdGraphSchema_K2::PC_Name) Base = TEXT("name");
    else if (Category == UEdGraphSchema_K2::PC_Text) Base = TEXT("text");
    else if (Category == UEdGraphSchema_K2::PC_Object) Base = TEXT("object:") + GetPathNameSafe(Type.PinSubCategoryObject.Get());
    else if (Category == UEdGraphSchema_K2::PC_Class) Base = TEXT("class:") + GetPathNameSafe(Type.PinSubCategoryObject.Get());
    else if (Category == UEdGraphSchema_K2::PC_SoftObject) Base = TEXT("softobject:") + GetPathNameSafe(Type.PinSubCategoryObject.Get());
    else if (Category == UEdGraphSchema_K2::PC_SoftClass) Base = TEXT("softclass:") + GetPathNameSafe(Type.PinSubCategoryObject.Get());
    else if (Category == UEdGraphSchema_K2::PC_Struct) Base = TEXT("struct:") + GetPathNameSafe(Type.PinSubCategoryObject.Get());
    else if (Category == UEdGraphSchema_K2::PC_Interface) Base = TEXT("interface:") + GetPathNameSafe(Type.PinSubCategoryObject.Get());
    else Base = Category.ToString();

    if (Type.ContainerType == EPinContainerType::Array) return TEXT("array:") + Base;
    if (Type.ContainerType == EPinContainerType::Set) return TEXT("set:") + Base;
    if (Type.ContainerType == EPinContainerType::Map) return TEXT("map:") + Base;
    return Base;
}

UClass* ResolveClass(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    if (UClass* Found = FindObject<UClass>(nullptr, *Path)) return Found;
    if (UClass* Found = LoadObject<UClass>(nullptr, *Path)) return Found;
    // Allow a bare name resolved against /Script/kards.
    const FString Qualified = Path.Contains(TEXT("/")) ? Path : FString::Printf(TEXT("/Script/kards.%s"), *Path);
    if (UClass* Found = FindObject<UClass>(nullptr, *Qualified)) return Found;
    return LoadObject<UClass>(nullptr, *Qualified);
}

bool PinTypeFromSpec(const FString& Spec, FEdGraphPinType& Out, FString& Error)
{
    FString Body = Spec;
    EPinContainerType Container = EPinContainerType::None;
    if (Body.StartsWith(TEXT("array:"))) { Container = EPinContainerType::Array; Body.RightChopInline(6); }
    else if (Body.StartsWith(TEXT("set:"))) { Container = EPinContainerType::Set; Body.RightChopInline(4); }
    else if (Body.StartsWith(TEXT("map:"))) { Error = TEXT("map pin types are not supported"); return false; }

    FEdGraphPinType Type;
    Type.ContainerType = Container;
    if (Body == TEXT("bool")) Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    else if (Body == TEXT("byte")) Type.PinCategory = UEdGraphSchema_K2::PC_Byte;
    else if (Body == TEXT("int")) Type.PinCategory = UEdGraphSchema_K2::PC_Int;
    else if (Body == TEXT("int64")) Type.PinCategory = UEdGraphSchema_K2::PC_Int64;
    else if (Body == TEXT("float"))
    {
        Type.PinCategory = UEdGraphSchema_K2::PC_Real;
        Type.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    }
    else if (Body == TEXT("double"))
    {
        Type.PinCategory = UEdGraphSchema_K2::PC_Real;
        Type.PinSubCategory = UEdGraphSchema_K2::PC_Double;
    }
    else if (Body == TEXT("string")) Type.PinCategory = UEdGraphSchema_K2::PC_String;
    else if (Body == TEXT("name")) Type.PinCategory = UEdGraphSchema_K2::PC_Name;
    else if (Body == TEXT("text")) Type.PinCategory = UEdGraphSchema_K2::PC_Text;
    else if (Body.StartsWith(TEXT("enum:")))
    {
        UEnum* Enum = FindObject<UEnum>(nullptr, *Body.RightChop(5));
        if (!Enum) Enum = LoadObject<UEnum>(nullptr, *Body.RightChop(5));
        if (!Enum) { Error = TEXT("unknown enum ") + Body.RightChop(5); return false; }
        Type.PinCategory = UEdGraphSchema_K2::PC_Byte;
        Type.PinSubCategoryObject = Enum;
    }
    else if (Body.StartsWith(TEXT("struct:")))
    {
        UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *Body.RightChop(7));
        if (!Struct) Struct = LoadObject<UScriptStruct>(nullptr, *Body.RightChop(7));
        if (!Struct) { Error = TEXT("unknown struct ") + Body.RightChop(7); return false; }
        Type.PinCategory = UEdGraphSchema_K2::PC_Struct;
        Type.PinSubCategoryObject = Struct;
    }
    else if (Body.StartsWith(TEXT("object:")))
    {
        UClass* Class = ResolveClass(Body.RightChop(7));
        if (!Class) { Error = TEXT("unknown object class ") + Body.RightChop(7); return false; }
        Type.PinCategory = UEdGraphSchema_K2::PC_Object;
        Type.PinSubCategoryObject = Class;
    }
    else if (Body.StartsWith(TEXT("class:")))
    {
        UClass* Class = ResolveClass(Body.RightChop(6));
        if (!Class) { Error = TEXT("unknown meta class ") + Body.RightChop(6); return false; }
        Type.PinCategory = UEdGraphSchema_K2::PC_Class;
        Type.PinSubCategoryObject = Class;
    }
    else if (Body.StartsWith(TEXT("softobject:")))
    {
        UClass* Class = ResolveClass(Body.RightChop(11));
        if (!Class) { Error = TEXT("unknown soft object class ") + Body.RightChop(11); return false; }
        Type.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
        Type.PinSubCategoryObject = Class;
    }
    else if (Body.StartsWith(TEXT("interface:")))
    {
        UClass* Class = ResolveClass(Body.RightChop(10));
        if (!Class) { Error = TEXT("unknown interface ") + Body.RightChop(10); return false; }
        Type.PinCategory = UEdGraphSchema_K2::PC_Interface;
        Type.PinSubCategoryObject = Class;
    }
    else
    {
        Error = TEXT("unknown pin type '") + Body + TEXT("'");
        return false;
    }
    Out = Type;
    return true;
}

bool ParseCardSpec(const FString& Text, FCardSpec& Out, FString& Error)
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        Error = TEXT("spec is not valid JSON");
        return false;
    }
    if (!Root->TryGetStringField(TEXT("package"), Out.Package) || Out.Package.IsEmpty())
    {
        Error = TEXT("spec.package is required");
        return false;
    }
    Out.AssetName = GetStringOr(Root, TEXT("assetName"), FPackageName::GetShortName(Out.Package));
    Out.ParentClass = GetStringOr(Root, TEXT("parentClass"), Out.ParentClass);

    const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
    if (Root->TryGetArrayField(TEXT("variables"), Variables) && Variables)
    {
        for (const TSharedPtr<FJsonValue>& Item : *Variables)
        {
            if (!Item.IsValid() || Item->Type != EJson::Object) { Error = TEXT("variables entry is not an object"); return false; }
            const TSharedPtr<FJsonObject> Obj = Item->AsObject();
            FCardVariable Variable;
            if (!Obj->TryGetStringField(TEXT("name"), Variable.Name) || Variable.Name.IsEmpty())
            {
                Error = TEXT("variables entry is missing 'name'");
                return false;
            }
            if (!Obj->TryGetStringField(TEXT("type"), Variable.Type) || Variable.Type.IsEmpty())
            {
                Error = Variable.Name + TEXT(": variables entry is missing 'type'");
                return false;
            }
            Variable.DefaultValue = GetStringOr(Obj, TEXT("value"), FString());
            Out.Variables.Add(Variable);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Defaults = nullptr;
    if (Root->TryGetArrayField(TEXT("defaults"), Defaults) && Defaults)
    {
        for (const TSharedPtr<FJsonValue>& Item : *Defaults)
        {
            if (!Item.IsValid() || Item->Type != EJson::Object) { Error = TEXT("defaults entry is not an object"); return false; }
            const TSharedPtr<FJsonObject> Obj = Item->AsObject();
            FCardDefault Default;
            if (!Obj->TryGetStringField(TEXT("name"), Default.Name) || Default.Name.IsEmpty())
            {
                Error = TEXT("defaults entry is missing 'name'");
                return false;
            }
            Obj->TryGetStringField(TEXT("type"), Default.LiteralType);
            Obj->TryGetStringField(TEXT("enum"), Default.EnumType);
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (Obj->TryGetArrayField(TEXT("values"), Values) && Values)
            {
                Default.bIsArray = true;
                for (const TSharedPtr<FJsonValue>& Entry : *Values)
                {
                    Default.Values.Add(JsonValueToText(Entry));
                }
                if (Default.EnumType.IsEmpty() && Default.LiteralType.StartsWith(TEXT("enum:")))
                {
                    Default.EnumType = Default.LiteralType.RightChop(5);
                }
                if (Default.EnumType.IsEmpty())
                {
                    Error = Default.Name + TEXT(": array default needs 'enum' or type 'enum:<path>'");
                    return false;
                }
            }
            else if (Obj->HasField(TEXT("value")))
            {
                Default.LiteralValue = JsonValueToText(Obj->TryGetField(TEXT("value")));
                if (Default.LiteralType.IsEmpty())
                {
                    const TSharedPtr<FJsonValue> Raw = Obj->TryGetField(TEXT("value"));
                    if (Raw.IsValid())
                    {
                        if (Raw->Type == EJson::Boolean) Default.LiteralType = TEXT("bool");
                        else if (Raw->Type == EJson::Number) Default.LiteralType = TEXT("int");
                        else Default.LiteralType = TEXT("string");
                    }
                }
            }
            else
            {
                Error = Default.Name + TEXT(": default needs 'value' or 'values'");
                return false;
            }
            Out.Defaults.Add(Default);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
    if (Root->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs)
    {
        for (const TSharedPtr<FJsonValue>& Item : *Graphs)
        {
            if (!Item.IsValid() || Item->Type != EJson::Object) { Error = TEXT("graphs entry is not an object"); return false; }
            const TSharedPtr<FJsonObject> Obj = Item->AsObject();
            FCardGraph Graph;
            FString KindName;
            Obj->TryGetStringField(TEXT("kind"), KindName);
            if (KindName == TEXT("function")) Graph.Kind = ECardGraphKind::Function;
            else if (KindName == TEXT("k2")) Graph.Kind = ECardGraphKind::RawK2;
            else Graph.Kind = ECardGraphKind::Event;
            if (!Obj->TryGetStringField(TEXT("function"), Graph.Function)) Graph.Function = FString();
            Graph.OwnerClass = GetStringOr(Obj, TEXT("owner"), FString());
            if (Graph.Function.IsEmpty() && Graph.Kind != ECardGraphKind::RawK2)
            {
                Error = TEXT("graphs entry is missing 'function'");
                return false;
            }

            // kind:"k2" —— 原样实例化官方节点图
            const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
            if (Obj->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
            {
                for (const TSharedPtr<FJsonValue>& NodeItem : *Nodes)
                {
                    if (!NodeItem.IsValid() || NodeItem->Type != EJson::Object) { Error = TEXT("nodes entry is not an object"); return false; }
                    const TSharedPtr<FJsonObject> NO = NodeItem->AsObject();
                    FCardNodeSpec Node;
                    if (!NO->TryGetStringField(TEXT("id"), Node.Id) || Node.Id.IsEmpty()) { Error = TEXT("node is missing 'id'"); return false; }
                    if (!NO->TryGetStringField(TEXT("class"), Node.Class) || Node.Class.IsEmpty()) { Error = Node.Id + TEXT(": node is missing 'class'"); return false; }
                    NO->TryGetStringField(TEXT("function"), Node.Function);
                    NO->TryGetStringField(TEXT("owner"), Node.OwnerClass);
                    NO->TryGetStringField(TEXT("variable"), Node.Variable);
                    NO->TryGetStringField(TEXT("macro"), Node.Macro);
                    NO->TryGetStringField(TEXT("target"), Node.TargetClass);
                    NO->TryGetStringField(TEXT("comment"), Node.Comment);
                    const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
                    if (NO->TryGetArrayField(TEXT("pos"), Pos) && Pos && Pos->Num() >= 2)
                    {
                        Node.PosX = static_cast<int32>((*Pos)[0]->AsNumber());
                        Node.PosY = static_cast<int32>((*Pos)[1]->AsNumber());
                    }
                    const TSharedPtr<FJsonObject>* Defs = nullptr;
                    if (NO->TryGetObjectField(TEXT("defaults"), Defs) && Defs && Defs->IsValid())
                    {
                        for (const auto& Pair : (*Defs)->Values)
                        {
                            Node.Defaults.Emplace(Pair.Key, JsonValueToText(Pair.Value));
                        }
                    }
                    Node.Raw = NO;
                    Graph.Nodes.Add(Node);
                }
            }
            const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
            if (Obj->TryGetArrayField(TEXT("links"), Links) && Links)
            {
                for (const TSharedPtr<FJsonValue>& LinkItem : *Links)
                {
                    if (!LinkItem.IsValid() || LinkItem->Type != EJson::Array) { Error = TEXT("link is not an array"); return false; }
                    const TArray<TSharedPtr<FJsonValue>>& L = LinkItem->AsArray();
                    if (L.Num() < 4) { Error = TEXT("link needs [fromId, fromPin, toId, toPin]"); return false; }
                    FCardLinkSpec Link;
                    Link.FromId = JsonValueToText(L[0]);
                    Link.FromPin = JsonValueToText(L[1]);
                    Link.ToId = JsonValueToText(L[2]);
                    Link.ToPin = JsonValueToText(L[3]);
                    Graph.Links.Add(Link);
                }
            }
            if (Graph.Kind == ECardGraphKind::RawK2 && Graph.Nodes.IsEmpty())
            {
                Error = TEXT("kind:'k2' graph needs a non-empty 'nodes' array");
                return false;
            }
            const TArray<TSharedPtr<FJsonValue>>* Body = nullptr;
            if (Obj->TryGetArrayField(TEXT("body"), Body) && Body)
            {
                if (!ParseStmtList(*Body, Graph.Body, Error)) { Error = Graph.Function + TEXT(": ") + Error; return false; }
            }
            Out.Graphs.Add(Graph);
        }
    }
    if (Out.Graphs.IsEmpty())
    {
        Error = TEXT("spec.graphs is empty");
        return false;
    }
    return true;
}
