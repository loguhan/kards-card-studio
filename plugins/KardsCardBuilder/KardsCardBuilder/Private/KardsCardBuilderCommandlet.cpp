#include "KardsCardBuilderCommandlet.h"

#include "CardGraphBuilder.h"
#include "CardSpec.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetRegistryState.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/EngineVersion.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/BufferArchive.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogKardsCardBuilderCmd, Log, All);

namespace
{
struct FBuildReport
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Errors;
    TArray<TSharedPtr<FJsonValue>> Notes;
    TArray<TSharedPtr<FJsonValue>> Messages;
    int32 CompileErrors = 0;
    int32 CompileWarnings = 0;

    explicit FBuildReport(const TCHAR* Mode)
    {
        Root->SetNumberField(TEXT("report_version"), 1);
        Root->SetStringField(TEXT("mode"), Mode);
        Root->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
        Root->SetBoolField(TEXT("success"), false);
        Root->SetBoolField(TEXT("asset_saved"), false);
    }

    void Error(const FString& Message)
    {
        Errors.Add(MakeShared<FJsonValueString>(Message));
        UE_LOG(LogKardsCardBuilderCmd, Error, TEXT("%s"), *Message);
    }
    void Note(const FString& Message)
    {
        Notes.Add(MakeShared<FJsonValueString>(Message));
        UE_LOG(LogKardsCardBuilderCmd, Display, TEXT("%s"), *Message);
    }
    void Absorb(const FCardDiagnostics& Diag)
    {
        for (const FString& Item : Diag.Errors) Error(Item);
        for (const FString& Item : Diag.Notes) Note(Item);
    }
    void CompileLog(const TCHAR* Phase, const FCompilerResultsLog& Log)
    {
        CompileErrors += Log.NumErrors;
        CompileWarnings += Log.NumWarnings;
        for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
        {
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("phase"), Phase);
            Item->SetStringField(TEXT("severity"), FTokenizedMessage::GetSeverityText(Message->GetSeverity()).ToString());
            Item->SetStringField(TEXT("message"), Message->ToText().ToString());
            Messages.Add(MakeShared<FJsonValueObject>(Item));
        }
    }
    int32 Write(const FString& Filename, bool bSuccess)
    {
        Root->SetBoolField(TEXT("success"), bSuccess && Errors.Num() == 0 && CompileErrors == 0);
        Root->SetArrayField(TEXT("errors"), Errors);
        Root->SetArrayField(TEXT("notes"), Notes);
        Root->SetNumberField(TEXT("compiler_errors"), CompileErrors);
        Root->SetNumberField(TEXT("compiler_warnings"), CompileWarnings);
        Root->SetArrayField(TEXT("compiler_messages"), Messages);
        FString Json;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        if (!FJsonSerializer::Serialize(Root, Writer) ||
            !FFileHelper::SaveStringToFile(Json, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogKardsCardBuilderCmd, Error, TEXT("cannot write report %s"), *Filename);
            return 3;
        }
        UE_LOG(LogKardsCardBuilderCmd, Display, TEXT("report written: %s (success=%s)"),
            *Filename, Root->GetBoolField(TEXT("success")) ? TEXT("true") : TEXT("false"));
        return Root->GetBoolField(TEXT("success")) ? 0 : 1;
    }
};

bool PrepareReportPath(const FString& Params, FString& Path)
{
    if (!FParse::Value(*Params, TEXT("Report="), Path) || FPaths::IsRelative(Path) ||
        !FPaths::GetExtension(Path).Equals(TEXT("json"), ESearchCase::IgnoreCase))
    {
        UE_LOG(LogKardsCardBuilderCmd, Error, TEXT("an absolute -Report=...json path is required"));
        return false;
    }
    Path = FPaths::ConvertRelativePathToFull(Path);
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
    {
        UE_LOG(LogKardsCardBuilderCmd, Error, TEXT("cannot create report directory: %s"), *FPaths::GetPath(Path));
        return false;
    }
    IFileManager::Get().Delete(*Path, false, true, true);
    return true;
}

// --------------------------------------------------------------------- dump

void DumpFunction(const UFunction* Function, TArray<TSharedPtr<FJsonValue>>& Out)
{
    auto Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("name"), Function->GetName());
    Item->SetStringField(TEXT("path"), Function->GetPathName());
    Item->SetStringField(TEXT("owner"), Function->GetOuter()->GetPathName());
    Item->SetStringField(TEXT("flags_hex"), FString::Printf(TEXT("0x%016llx"), static_cast<unsigned long long>(Function->FunctionFlags)));
    Item->SetBoolField(TEXT("blueprint_callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
    Item->SetBoolField(TEXT("blueprint_pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
    Item->SetBoolField(TEXT("blueprint_event"), Function->HasAnyFunctionFlags(FUNC_BlueprintEvent));
    Item->SetBoolField(TEXT("native"), Function->HasAnyFunctionFlags(FUNC_Native));
    Item->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
    Item->SetBoolField(TEXT("const"), Function->HasAnyFunctionFlags(FUNC_Const));

    TArray<TSharedPtr<FJsonValue>> Params;
    TArray<FString> InputNames;
    TArray<FString> OutputNames;
    for (TFieldIterator<FProperty> It(Function); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property->HasAnyPropertyFlags(CPF_Parm)) continue;
        FEdGraphPinType PinType;
        const bool bConverted = GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, PinType);
        auto Param = MakeShared<FJsonObject>();
        Param->SetStringField(TEXT("name"), Property->GetName());
        Param->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
        Param->SetStringField(TEXT("pin_type"), bConverted ? PinTypeToString(PinType) : TEXT("<unconvertible>"));
        const bool bOut = Property->HasAnyPropertyFlags(CPF_OutParm);
        const bool bReturn = Property->HasAnyPropertyFlags(CPF_ReturnParm);
        const bool bReference = Property->HasAnyPropertyFlags(CPF_ReferenceParm);
        Param->SetBoolField(TEXT("out"), bOut);
        Param->SetBoolField(TEXT("return"), bReturn);
        Param->SetBoolField(TEXT("reference"), bReference);
        Params.Add(MakeShared<FJsonValueObject>(Param));
        if (bOut)
        {
            OutputNames.Add(bReturn ? TEXT("ReturnValue") : Property->GetName());
        }
        else
        {
            InputNames.Add(Property->GetName());
        }
    }
    Item->SetArrayField(TEXT("params"), Params);
    Item->SetArrayField(TEXT("input_pins"), TArray<TSharedPtr<FJsonValue>>());
    auto ToValues = [](const TArray<FString>& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FString& Name : Names) Values.Add(MakeShared<FJsonValueString>(Name));
        return Values;
    };
    Item->SetArrayField(TEXT("input_pins"), ToValues(InputNames));
    Item->SetArrayField(TEXT("output_pins"), ToValues(OutputNames));
    Out.Add(MakeShared<FJsonValueObject>(Item));
}

void DumpProperty(const FProperty* Property, TArray<TSharedPtr<FJsonValue>>& Out)
{
    auto Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("name"), Property->GetName());
    Item->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
    Item->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
    Item->SetStringField(TEXT("flags_hex"), FString::Printf(TEXT("0x%016llx"), static_cast<unsigned long long>(Property->GetPropertyFlags())));
    Item->SetBoolField(TEXT("blueprint_visible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
    Item->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
    FEdGraphPinType PinType;
    if (GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, PinType))
    {
        Item->SetStringField(TEXT("pin_type"), PinTypeToString(PinType));
    }
    if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
    {
        FEdGraphPinType Inner;
        if (GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(ArrayProperty->Inner, Inner))
        {
            Item->SetStringField(TEXT("element_pin_type"), PinTypeToString(Inner));
        }
        Item->SetStringField(TEXT("element_class"), ArrayProperty->Inner->GetClass()->GetName());
    }
    Out.Add(MakeShared<FJsonValueObject>(Item));
}

bool RunDump(const FString& Params, FBuildReport& Report)
{
    FString ClassPath;
    if (!FParse::Value(*Params, TEXT("Class="), ClassPath) || ClassPath.IsEmpty())
    {
        Report.Error(TEXT("-Class=/Script/<module>.<Class> is required for -Mode=Dump"));
        return false;
    }
    UClass* Class = ResolveClass(ClassPath);
    if (!Class)
    {
        Report.Error(TEXT("cannot resolve class ") + ClassPath);
        return false;
    }
    FString Filter;
    FParse::Value(*Params, TEXT("Filter="), Filter);

    Report.Root->SetStringField(TEXT("class"), Class->GetPathName());
    Report.Root->SetStringField(TEXT("super"), GetPathNameSafe(Class->GetSuperClass()));
    TArray<TSharedPtr<FJsonValue>> Functions;
    int32 FunctionCount = 0;
    for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        UFunction* Function = *It;
        if (!Filter.IsEmpty() && !Function->GetName().Contains(Filter)) continue;
        DumpFunction(Function, Functions);
        ++FunctionCount;
    }
    Report.Root->SetNumberField(TEXT("function_count"), FunctionCount);
    Report.Root->SetArrayField(TEXT("functions"), Functions);

    TArray<TSharedPtr<FJsonValue>> Properties;
    int32 PropertyCount = 0;
    for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        if (!Filter.IsEmpty() && !It->GetName().Contains(Filter)) continue;
        DumpProperty(*It, Properties);
        ++PropertyCount;
    }
    Report.Root->SetNumberField(TEXT("property_count"), PropertyCount);
    Report.Root->SetArrayField(TEXT("properties"), Properties);
    return true;
}

bool RunDumpEnum(const FString& Params, FBuildReport& Report)
{
    FString EnumPath;
    if (!FParse::Value(*Params, TEXT("Enum="), EnumPath) || EnumPath.IsEmpty())
    {
        Report.Error(TEXT("-Enum=/Script/<module>.<Enum> is required for -Mode=DumpEnum"));
        return false;
    }
    UEnum* Enum = FindObject<UEnum>(nullptr, *EnumPath);
    if (!Enum) Enum = LoadObject<UEnum>(nullptr, *EnumPath);
    if (!Enum)
    {
        Report.Error(TEXT("cannot resolve enum ") + EnumPath);
        return false;
    }
    Report.Root->SetStringField(TEXT("enum"), Enum->GetPathName());
    Report.Root->SetStringField(TEXT("cpp_type"), Enum->GetCppForm() == UEnum::ECppForm::EnumClass ? TEXT("enum class") : TEXT("plain"));
    TArray<TSharedPtr<FJsonValue>> Values;
    for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
    {
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
        Item->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
        Values.Add(MakeShared<FJsonValueObject>(Item));
    }
    Report.Root->SetArrayField(TEXT("values"), Values);
    return true;
}

bool RunDumpModule(const FString& Params, FBuildReport& Report)
{
    FString ModuleName;
    if (!FParse::Value(*Params, TEXT("Module="), ModuleName) || ModuleName.IsEmpty())
    {
        Report.Error(TEXT("-Module=<name> is required for -Mode=DumpModule"));
        return false;
    }
    TArray<TSharedPtr<FJsonValue>> Classes;
    int32 Count = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        if (!Class || Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
        if (Class->GetOutermost()->GetName() != ModuleName) continue;
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Class->GetName());
        Item->SetStringField(TEXT("path"), Class->GetPathName());
        Item->SetStringField(TEXT("super"), GetPathNameSafe(Class->GetSuperClass()));
        Classes.Add(MakeShared<FJsonValueObject>(Item));
        ++Count;
    }
    Report.Root->SetStringField(TEXT("module"), ModuleName);
    Report.Root->SetNumberField(TEXT("class_count"), Count);
    Report.Root->SetArrayField(TEXT("classes"), Classes);
    return true;
}

// -------------------------------------------------------------------- build

FBPVariableDescription MakeVariable(const FString& Name, const FEdGraphPinType& Type, const FString& DefaultValue)
{
    FBPVariableDescription Description;
    Description.VarName = FName(*Name);
    Description.VarGuid = FGuid::NewGuid();
    Description.VarType = Type;
    Description.PropertyFlags = CPF_BlueprintVisible | CPF_Edit | CPF_DisableEditOnInstance;
    Description.FriendlyName = Name;
    Description.Category = UEdGraphSchema_K2::VR_DefaultCategory;
    Description.DefaultValue = DefaultValue;
    return Description;
}

bool SetArrayDefault(UObject* Target, FArrayProperty* ArrayProperty, const TArray<FString>& Values, FString& Error)
{
    FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Target));
    Helper.EmptyValues();
    if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(ArrayProperty->Inner))
    {
        UEnum* Enum = EnumProperty->GetEnum();
        for (const FString& Value : Values)
        {
            const int64 Index = Enum->GetValueByNameString(Value);
            if (Index == INDEX_NONE)
            {
                Error = FString::Printf(TEXT("'%s' is not a value of %s"), *Value, *Enum->GetName());
                return false;
            }
            const int32 NewIndex = Helper.AddValue();
            EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Helper.GetRawPtr(NewIndex), Index);
        }
        return true;
    }
    if (const FByteProperty* ByteProperty = CastField<FByteProperty>(ArrayProperty->Inner))
    {
        if (UEnum* Enum = ByteProperty->Enum)
        {
            for (const FString& Value : Values)
            {
                const int64 Index = Enum->GetValueByNameString(Value);
                if (Index == INDEX_NONE)
                {
                    Error = FString::Printf(TEXT("'%s' is not a value of %s"), *Value, *Enum->GetName());
                    return false;
                }
                const int32 NewIndex = Helper.AddValue();
                ByteProperty->SetIntPropertyValue(Helper.GetRawPtr(NewIndex), Index);
            }
            return true;
        }
        for (const FString& Value : Values)
        {
            const int32 NewIndex = Helper.AddValue();
            ByteProperty->SetIntPropertyValue(Helper.GetRawPtr(NewIndex), static_cast<int64>(FCString::Atoi(*Value)));
        }
        return true;
    }
    if (const FIntProperty* IntProperty = CastField<FIntProperty>(ArrayProperty->Inner))
    {
        for (const FString& Value : Values)
        {
            const int32 NewIndex = Helper.AddValue();
            IntProperty->SetPropertyValue(Helper.GetRawPtr(NewIndex), FCString::Atoi(*Value));
        }
        return true;
    }
    if (const FStrProperty* StrProperty = CastField<FStrProperty>(ArrayProperty->Inner))
    {
        for (const FString& Value : Values)
        {
            const int32 NewIndex = Helper.AddValue();
            StrProperty->SetPropertyValue(Helper.GetRawPtr(NewIndex), Value);
        }
        return true;
    }
    if (const FNameProperty* NameProperty = CastField<FNameProperty>(ArrayProperty->Inner))
    {
        for (const FString& Value : Values)
        {
            const int32 NewIndex = Helper.AddValue();
            NameProperty->SetPropertyValue(Helper.GetRawPtr(NewIndex), FName(*Value));
        }
        return true;
    }
    if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(ArrayProperty->Inner))
    {
        for (const FString& Value : Values)
        {
            UObject* Object = Value.IsEmpty() || Value == TEXT("None") ? nullptr : LoadObject<UObject>(nullptr, *Value);
            if (!Object)
            {
                Error = FString::Printf(TEXT("cannot resolve object '%s'"), *Value);
                return false;
            }
            const int32 NewIndex = Helper.AddValue();
            ObjectProperty->SetObjectPropertyValue(Helper.GetRawPtr(NewIndex), Object);
        }
        return true;
    }
    Error = FString::Printf(TEXT("array element class %s is not supported"), *ArrayProperty->Inner->GetClass()->GetName());
    return false;
}

bool ApplyDefaults(UBlueprint* Blueprint, const FCardSpec& Spec, FBuildReport& Report)
{
    UObject* CDO = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
    if (!CDO)
    {
        Report.Error(TEXT("generated class has no default object"));
        return false;
    }
    TArray<TSharedPtr<FJsonValue>> Applied;
    for (const FCardDefault& Default : Spec.Defaults)
    {
        FProperty* Property = Blueprint->GeneratedClass->FindPropertyByName(FName(*Default.Name));
        if (!Property)
        {
            Report.Error(FString::Printf(TEXT("default '%s': the generated class has no such property"), *Default.Name));
            return false;
        }
        if (Default.bIsArray)
        {
            FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
            if (!ArrayProperty)
            {
                Report.Error(FString::Printf(TEXT("default '%s': not an array property"), *Default.Name));
                return false;
            }
            FString Error;
            if (!SetArrayDefault(CDO, ArrayProperty, Default.Values, Error))
            {
                Report.Error(FString::Printf(TEXT("default '%s': %s"), *Default.Name, *Error));
                return false;
            }
        }
        else if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            FString Error;
            TArray<FString> Single;
            Single.Add(Default.LiteralValue);
            if (!SetArrayDefault(CDO, ArrayProperty, Single, Error))
            {
                Report.Error(FString::Printf(TEXT("default '%s': %s"), *Default.Name, *Error));
                return false;
            }
        }
        else
        {
            void* Address = Property->ContainerPtrToValuePtr<void>(CDO);
            if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
            {
                BoolProperty->SetPropertyValue(Address, Default.LiteralValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Default.LiteralValue == TEXT("1"));
            }
            else if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
            {
                UEnum* Enum = EnumProperty->GetEnum();
                const int64 Index = Enum->GetValueByNameString(Default.LiteralValue);
                if (Index == INDEX_NONE)
                {
                    Report.Error(FString::Printf(TEXT("default '%s': '%s' is not a value of %s"), *Default.Name, *Default.LiteralValue, *Enum->GetName()));
                    return false;
                }
                EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Address, Index);
            }
            else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
            {
                int64 Index = FCString::Atoi64(*Default.LiteralValue);
                if (ByteProperty->Enum)
                {
                    const int64 Named = ByteProperty->Enum->GetValueByNameString(Default.LiteralValue);
                    if (Named != INDEX_NONE) Index = Named;
                }
                ByteProperty->SetIntPropertyValue(Address, Index);
            }
            else if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
            {
                IntProperty->SetPropertyValue(Address, FCString::Atoi(*Default.LiteralValue));
            }
            else if (FInt64Property* Int64Property = CastField<FInt64Property>(Property))
            {
                Int64Property->SetPropertyValue(Address, FCString::Atoi64(*Default.LiteralValue));
            }
            else if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
            {
                FloatProperty->SetPropertyValue(Address, FCString::Atof(*Default.LiteralValue));
            }
            else if (FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
            {
                DoubleProperty->SetPropertyValue(Address, FCString::Atod(*Default.LiteralValue));
            }
            else if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
            {
                StrProperty->SetPropertyValue(Address, Default.LiteralValue);
            }
            else if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
            {
                NameProperty->SetPropertyValue(Address, FName(*Default.LiteralValue));
            }
            else if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
            {
                TextProperty->SetPropertyValue(Address, FText::FromString(Default.LiteralValue));
            }
            else if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
            {
                UObject* Object = Default.LiteralValue.IsEmpty() || Default.LiteralValue == TEXT("None")
                    ? nullptr : LoadObject<UObject>(nullptr, *Default.LiteralValue);
                if (!Object && !Default.LiteralValue.IsEmpty() && Default.LiteralValue != TEXT("None"))
                {
                    Report.Error(FString::Printf(TEXT("default '%s': cannot resolve '%s'"), *Default.Name, *Default.LiteralValue));
                    return false;
                }
                ObjectProperty->SetObjectPropertyValue(Address, Object);
            }
            else
            {
                Report.Error(FString::Printf(TEXT("default '%s': property class %s is not supported"),
                    *Default.Name, *Property->GetClass()->GetName()));
                return false;
            }
        }
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Default.Name);
        Item->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
        FString Exported;
        Property->ExportTextItem_Direct(Exported, Property->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);
        Item->SetStringField(TEXT("value"), Exported);
        Applied.Add(MakeShared<FJsonValueObject>(Item));
    }
    Report.Root->SetArrayField(TEXT("applied_defaults"), Applied);
    return true;
}

bool RunBuild(const FString& Params, FBuildReport& Report)
{
    FString SpecPath;
    if (!FParse::Value(*Params, TEXT("Spec="), SpecPath) || FPaths::IsRelative(SpecPath))
    {
        Report.Error(TEXT("an absolute -Spec=...json path is required for -Mode=Build"));
        return false;
    }
    SpecPath = FPaths::ConvertRelativePathToFull(SpecPath);
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *SpecPath))
    {
        Report.Error(TEXT("cannot read spec ") + SpecPath);
        return false;
    }
    FCardSpec Spec;
    FString Error;
    if (!ParseCardSpec(Text, Spec, Error))
    {
        Report.Error(TEXT("spec: ") + Error);
        return false;
    }
    Report.Root->SetStringField(TEXT("spec"), SpecPath);
    Report.Root->SetStringField(TEXT("package"), Spec.Package);
    Report.Root->SetStringField(TEXT("asset_name"), Spec.AssetName);

    const bool bForce = FParse::Param(*Params, TEXT("Force"));
    FString Filename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(Spec.Package, Filename, FPackageName::GetAssetPackageExtension()))
    {
        Report.Error(TEXT("cannot map ") + Spec.Package + TEXT(" into this project's Content directory"));
        return false;
    }
    Report.Root->SetStringField(TEXT("save_path"), FPaths::ConvertRelativePathToFull(Filename));
    if (!bForce && (FPackageName::DoesPackageExist(Spec.Package) || IFileManager::Get().FileExists(*Filename)))
    {
        Report.Error(TEXT("package already exists; pass -Force to overwrite"));
        return false;
    }
    if (bForce)
    {
        // SavePackage will not silently replace an asset that is already on disk.
        for (const TCHAR* Extension : {TEXT(".uasset"), TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl")})
        {
            const FString Existing = FPaths::ChangeExtension(Filename, Extension);
            if (IFileManager::Get().FileExists(*Existing))
            {
                IFileManager::Get().Delete(*Existing, false, true, true);
                Report.Note(TEXT("removed ") + Existing);
            }
        }
    }

    UClass* ParentClass = ResolveClass(Spec.ParentClass);
    if (!ParentClass)
    {
        Report.Error(TEXT("cannot resolve parent class ") + Spec.ParentClass);
        return false;
    }
    Report.Root->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());

    TStrongObjectPtr<UPackage> Package(CreatePackage(*Spec.Package));
    if (!Package.IsValid())
    {
        Report.Error(TEXT("CreatePackage failed"));
        return false;
    }
    TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(ParentClass, Package.Get(),
        FName(*Spec.AssetName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
    if (!Blueprint.IsValid())
    {
        Report.Error(TEXT("CreateBlueprint failed"));
        return false;
    }

    Blueprint->Status = BS_BeingCreated;

    // Member variables first: every variable node resolves its type out of the skeleton class.
    for (const FCardVariable& Variable : Spec.Variables)
    {
        FEdGraphPinType PinType;
        if (!PinTypeFromSpec(Variable.Type, PinType, Error))
        {
            Report.Error(FString::Printf(TEXT("variable '%s': %s"), *Variable.Name, *Error));
            Package->SetDirtyFlag(false);
            return false;
        }
        Blueprint->NewVariables.Add(MakeVariable(Variable.Name, PinType, Variable.DefaultValue));
    }

    UEdGraph* EventGraph = nullptr;
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (Graph) { EventGraph = Graph; break; }
    }
    if (!EventGraph)
    {
        EventGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(), TEXT("EventGraph"),
            UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        if (!EventGraph)
        {
            Report.Error(TEXT("cannot create the event graph"));
            Package->SetDirtyFlag(false);
            return false;
        }
        FBlueprintEditorUtils::AddUbergraphPage(Blueprint.Get(), EventGraph);
    }
    Report.Root->SetStringField(TEXT("event_graph"), EventGraph->GetName());

    // Skeleton pass so the variable and event signatures exist before wiring.
    Blueprint->Status = BS_Dirty;
    {
        FCompilerResultsLog SkeletonLog;
        FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(),
            EBlueprintCompileOptions::RegenerateSkeletonOnly | EBlueprintCompileOptions::SkipSave, &SkeletonLog);
        Report.CompileLog(TEXT("skeleton"), SkeletonLog);
        if (SkeletonLog.NumErrors > 0)
        {
            Report.Error(TEXT("skeleton compilation failed; nothing was saved"));
            Package->SetDirtyFlag(false);
            return false;
        }
    }
    Blueprint->Status = BS_BeingCreated;

    for (const FCardGraph& GraphSpec : Spec.Graphs)
    {
        // Events all share the ubergraph; a function graph (used to override a
        // BlueprintNativeEvent) gets its own graph and is registered by
        // FBlueprintEditorUtils::AddFunctionGraph inside the builder.
        UEdGraph* TargetGraph = EventGraph;
        if (GraphSpec.Kind == ECardGraphKind::RawK2)
        {
            // k2 图一律进默认事件图：ubergraph 的每个 page 会被编译成「以图名命名的函数」，
            // 再建一个叫 OnOtherCardPlayedFromHand 的 page 会和事件本身重名。
            TargetGraph = EventGraph;
        }
        else if (GraphSpec.Kind == ECardGraphKind::Function)
        {
            TargetGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(), FName(*GraphSpec.Function),
                UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
            if (!TargetGraph)
            {
                Report.Error(FString::Printf(TEXT("cannot create function graph '%s'"), *GraphSpec.Function));
                Package->SetDirtyFlag(false);
                return false;
            }
        }
        FCardDiagnostics Diag;
        if (!BuildCardGraph(Blueprint.Get(), TargetGraph, GraphSpec, Diag))
        {
            Report.Absorb(Diag);
            Report.Error(FString::Printf(TEXT("graph '%s' could not be assembled; nothing was saved"), *GraphSpec.Function));
            Package->SetDirtyFlag(false);
            return false;
        }
        Report.Absorb(Diag);
    }

    Blueprint->Status = BS_Dirty;
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint.Get());
    {
        FCompilerResultsLog CompileLog;
        FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &CompileLog);
        Report.CompileLog(TEXT("implementation"), CompileLog);
        if (CompileLog.NumErrors > 0 || Blueprint->Status == BS_Error)
        {
            Report.Error(TEXT("blueprint compilation failed; nothing was saved"));
            Package->SetDirtyFlag(false);
            return false;
        }
    }

    if (!ApplyDefaults(Blueprint.Get(), Spec, Report))
    {
        Package->SetDirtyFlag(false);
        return false;
    }

    Report.Root->SetStringField(TEXT("object_path"), Blueprint->GetPathName());
    Report.Root->SetStringField(TEXT("generated_class"), GetPathNameSafe(Blueprint->GeneratedClass));
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
    {
        Report.Error(TEXT("cannot create the output directory"));
        return false;
    }

    FSavePackageArgs Args;
    // Build writes the normal editor package (UBlueprint included) so that the asset
    // registry can see it and produce the card's registry entries. -Mode=Strip then
    // rewrites the same package as a class-only, game-loadable package.
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_None;
    FStringOutputDevice SaveErrors;
    Args.Error = &SaveErrors;
    Args.bSlowTask = false;
    Blueprint->MarkPackageDirty();
    if (!UPackage::SavePackage(Package.Get(), Blueprint.Get(), *Filename, Args))
    {
        Report.Error(TEXT("SavePackage failed: ") + SaveErrors);
        return false;
    }
    Report.Root->SetBoolField(TEXT("asset_saved"), true);
    FAssetRegistryModule::AssetCreated(Blueprint.Get());
    return true;
}

// -------------------------------------------------------------------- strip

/**
 * Rewrite an already-built card package so it contains only the generated class,
 * its CDO and its functions. The UBlueprint, its UEdGraphs and every UK2Node_* live
 * in the editor-only BlueprintGraph module; a shipping build has none of those
 * classes, so a package that contains them cannot be linked by the game.
 *
 * ClassGeneratedBy is the only reference from the generated class back to the
 * UBlueprint, and it is editor-only data, so clearing it makes the whole editor
 * graph unreachable from the saved root.
 */
bool RunStrip(const FString& Params, FBuildReport& Report)
{
    FString PackageName;
    if (!FParse::Value(*Params, TEXT("Package="), PackageName) || PackageName.IsEmpty())
    {
        Report.Error(TEXT("-Package=/Game/... is required for -Mode=Strip"));
        return false;
    }
    FString Filename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension()))
    {
        Report.Error(TEXT("cannot map ") + PackageName);
        return false;
    }
    Filename = FPaths::ConvertRelativePathToFull(Filename);
    Report.Root->SetStringField(TEXT("package"), PackageName);
    Report.Root->SetStringField(TEXT("save_path"), Filename);

    const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
    if (!Blueprint)
    {
        Report.Error(TEXT("cannot load ") + ObjectPath + TEXT(" (run -Mode=Build first)"));
        return false;
    }
    UClass* GeneratedClass = Blueprint->GeneratedClass;
    if (!GeneratedClass)
    {
        Report.Error(TEXT("blueprint has no generated class"));
        return false;
    }
    UPackage* Package = Blueprint->GetOutermost();
    Report.Root->SetStringField(TEXT("generated_class"), GeneratedClass->GetPathName());

#if WITH_EDITORONLY_DATA
    GeneratedClass->ClassGeneratedBy = nullptr;
#endif
    // The Blueprint must also stop pointing at the class for the harvest to be
    // one-directional; nothing reads it again in this process.
    Blueprint->GeneratedClass = nullptr;
    Package->SetDirtyFlag(false);
    Blueprint->ClearFlags(RF_Standalone);
    Blueprint->ClearFlags(RF_Public);

    for (const TCHAR* Extension : {TEXT(".uasset"), TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl")})
    {
        const FString Existing = FPaths::ChangeExtension(Filename, Extension);
        if (IFileManager::Get().FileExists(*Existing))
        {
            IFileManager::Get().Delete(*Existing, false, true, true);
        }
    }

    // The shipping loader refuses any package that does not carry PKG_FilterEditorOnly:
    //   if (!FPlatformProperties::HasEditorOnlyData() && !(Flags & PKG_FilterEditorOnly))
    //       return LINKER_Failed;   // "Package contains EditorOnly data..."
    // FSaveContext::IsFilterEditorOnly() reads exactly this package flag, so setting it
    // here makes the write (editor-only properties skipped) and the read self-consistent,
    // and it makes the summary flag appear. PKG_Cooked is what every shipped card has.
    Package->SetPackageFlags(PKG_FilterEditorOnly | PKG_Cooked);

    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_NoFlags;
    Args.SaveFlags = SAVE_None;
    FStringOutputDevice SaveErrors;
    Args.Error = &SaveErrors;
    Args.bSlowTask = false;
    Package->SetDirtyFlag(true);
    if (!UPackage::SavePackage(Package, GeneratedClass, *Filename, Args))
    {
        Report.Error(TEXT("SavePackage failed: ") + SaveErrors);
        return false;
    }
    Report.Root->SetNumberField(TEXT("package_flags"), static_cast<double>(Package->GetPackageFlags()));
    Report.Root->SetBoolField(TEXT("filter_editor_only"), true);
    Report.Root->SetBoolField(TEXT("asset_saved"), true);
    return true;
}

// ------------------------------------------------------------ editor session

/**
 * 常驻编辑器会话：AI 直接操作节点图，干活的是编辑器本身，所以覆盖度 = 编辑器能力。
 *
 *   -run=KardsCardBuilder -Mode=Session -Spec=<spec.json> -Bridge=<abs dir>
 *
 * 信箱协议（和 UE4SS_MCP_Bridge 同一套）：
 *   <Bridge>/commands/<seq>.json   操作请求，AI 写
 *   <Bridge>/results/<seq>.json    执行结果，编辑器写
 *   <Bridge>/graph.json            当前图快照，AI 读（每次操作后刷新）
 *
 * 操作：
 *   add_node {class, pos:[x,y], comment}          -> {id, pins:[...]}
 *   set_prop {node, name, value}                  UPROPERTY 反射赋值
 *   call     {node, fn, args:[...]}               成员函数（非 UFUNCTION 走垫片）
 *   connect  {from:{node,pin}, to:{node,pin}}
 *   disconnect {node, pin}
 *   default  {node, pin, value}                   引脚默认值
 *   dump     {}                                   -> {graph:{nodes:[...]}}
 *   compile  {}                                   -> {errors, warnings, messages}
 *   save     {}                                   存盘（自动 Strip 成可进游戏的形态）
 *   stop     {}
 */
bool RunSession(const FString& Params, FBuildReport& Report)
{
    FString SpecPath, Bridge;
    if (!FParse::Value(*Params, TEXT("Spec="), SpecPath) || FPaths::IsRelative(SpecPath))
    {
        Report.Error(TEXT("an absolute -Spec=...json path is required for -Mode=Session"));
        return false;
    }
    if (!FParse::Value(*Params, TEXT("Bridge="), Bridge) || FPaths::IsRelative(Bridge))
    {
        Report.Error(TEXT("an absolute -Bridge=<dir> path is required for -Mode=Session"));
        return false;
    }
    SpecPath = FPaths::ConvertRelativePathToFull(SpecPath);
    Bridge = FPaths::ConvertRelativePathToFull(Bridge);
    const FString CmdDir = Bridge / TEXT("commands");
    const FString ResDir = Bridge / TEXT("results");
    IFileManager::Get().MakeDirectory(*CmdDir, true);
    IFileManager::Get().MakeDirectory(*ResDir, true);

    FString SpecText;
    if (!FFileHelper::LoadFileToString(SpecText, *SpecPath)) { Report.Error(TEXT("cannot read ") + SpecPath); return false; }
    FCardSpec Spec;
    FString ParseError;
    if (!ParseCardSpec(SpecText, Spec, ParseError)) { Report.Error(TEXT("spec: ") + ParseError); return false; }
    UClass* ParentClass = ResolveClass(Spec.ParentClass);
    if (!ParentClass) { Report.Error(TEXT("cannot resolve parent ") + Spec.ParentClass); return false; }

    FString Filename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(Spec.Package, Filename, FPackageName::GetAssetPackageExtension()))
    {
        Report.Error(TEXT("cannot map ") + Spec.Package);
        return false;
    }
    Filename = FPaths::ConvertRelativePathToFull(Filename);
    for (const TCHAR* Extension : {TEXT(".uasset"), TEXT(".uexp")})
    {
        const FString Existing = FPaths::ChangeExtension(Filename, Extension);
        if (IFileManager::Get().FileExists(*Existing)) IFileManager::Get().Delete(*Existing, false, true, true);
    }

    TStrongObjectPtr<UPackage> Package(CreatePackage(*Spec.Package));
    TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(ParentClass, Package.Get(),
        FName(*Spec.AssetName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
    if (!Blueprint.IsValid()) { Report.Error(TEXT("CreateBlueprint failed")); return false; }
    Blueprint->Status = BS_BeingCreated;
    for (const FCardVariable& Variable : Spec.Variables)
    {
        FEdGraphPinType PinType;
        FString Error;
        if (!PinTypeFromSpec(Variable.Type, PinType, Error)) { Report.Error(Variable.Name + TEXT(": ") + Error); return false; }
        FBPVariableDescription Description;
        Description.VarName = FName(*Variable.Name);
        Description.VarGuid = FGuid::NewGuid();
        Description.VarType = PinType;
        Description.PropertyFlags = CPF_BlueprintVisible | CPF_Edit | CPF_DisableEditOnInstance;
        Description.FriendlyName = Variable.Name;
        Description.Category = UEdGraphSchema_K2::VR_DefaultCategory;
        Description.DefaultValue = Variable.DefaultValue;
        Blueprint->NewVariables.Add(Description);
    }
    UEdGraph* Graph = nullptr;
    for (UEdGraph* Existing : Blueprint->UbergraphPages) { if (Existing) { Graph = Existing; break; } }
    if (!Graph)
    {
        Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(), TEXT("EventGraph"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
        FBlueprintEditorUtils::AddUbergraphPage(Blueprint.Get(), Graph);
    }
    Blueprint->Status = BS_Dirty;
    {
        FCompilerResultsLog SkeletonLog;
        FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(),
            EBlueprintCompileOptions::RegenerateSkeletonOnly | EBlueprintCompileOptions::SkipSave, &SkeletonLog);
    }
    Blueprint->Status = BS_BeingCreated;

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    TMap<FString, UEdGraphNode*> Nodes;
    int32 NodeCounter = 0;

    auto WriteResult = [&](const FString& Name, const TSharedRef<FJsonObject>& Payload)
    {
        FString Out;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Payload, Writer);
        FFileHelper::SaveStringToFile(Out, *(ResDir / Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        // 每次都刷新图快照，AI 可以随时回读
        FFileHelper::SaveStringToFile(KardsK2DumpGraph(Graph), *(Bridge / TEXT("graph.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    };
    auto NodeFromJson = [&](const TSharedPtr<FJsonObject>& Op, UEdGraphNode*& Out, FString& Error) -> bool
    {
        FString Id;
        Op->TryGetStringField(TEXT("node"), Id);
        UEdGraphNode** Found = Nodes.Find(Id);
        if (!Found || !*Found) { Error = TEXT("unknown node id ") + Id; return false; }
        Out = *Found;
        return true;
    };
    auto PinOf = [](UEdGraphNode* Node, const FString& PinName) -> UEdGraphPin*
    {
        if (!Node) return nullptr;
        if (UEdGraphPin* Exact = Node->FindPin(FName(*PinName))) return Exact;
        for (UEdGraphPin* Pin : Node->Pins) if (Pin && Pin->PinName.ToString().StartsWith(PinName)) return Pin;
        return nullptr;
    };

    UE_LOG(LogKardsCardBuilderCmd, Display, TEXT("session ready: %s  (bridge %s)"), *Spec.Package, *Bridge);
    Report.Root->SetStringField(TEXT("session"), TEXT("ready"));
    Report.Root->SetStringField(TEXT("bridge"), Bridge);

    bool bStop = false;
    while (!bStop)
    {
        TArray<FString> Files;
        IFileManager::Get().FindFiles(Files, *(CmdDir / TEXT("*.json")), true, false);
        Files.Sort();
        for (const FString& Name : Files)
        {
            const FString CmdPath = CmdDir / Name;
            FString Text;
            if (!FFileHelper::LoadFileToString(Text, *CmdPath)) continue;
            IFileManager::Get().Delete(*CmdPath, false, true, true);

            TSharedPtr<FJsonObject> Op;
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
            if (!FJsonSerializer::Deserialize(Reader, Op) || !Op.IsValid())
            {
                auto R = MakeShared<FJsonObject>();
                R->SetBoolField(TEXT("ok"), false);
                R->SetStringField(TEXT("error"), TEXT("command is not valid JSON"));
                WriteResult(Name, R);
                continue;
            }
            FString Kind;
            Op->TryGetStringField(TEXT("op"), Kind);
            auto Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("op"), Kind);
            bool bOk = true;
            FString Error;

            if (Kind == TEXT("add_node"))
            {
                FString ClassPath, Comment, Function, OwnerClass, Variable, Macro, TargetClass;
                Op->TryGetStringField(TEXT("class"), ClassPath);
                Op->TryGetStringField(TEXT("comment"), Comment);
                Op->TryGetStringField(TEXT("function"), Function);
                Op->TryGetStringField(TEXT("owner"), OwnerClass);
                Op->TryGetStringField(TEXT("variable"), Variable);
                Op->TryGetStringField(TEXT("macro"), Macro);
                Op->TryGetStringField(TEXT("target"), TargetClass);
                int32 X = 0, Y = 0;
                const TArray<TSharedPtr<FJsonValue>>* Pos = nullptr;
                if (Op->TryGetArrayField(TEXT("pos"), Pos) && Pos && Pos->Num() >= 2)
                {
                    X = static_cast<int32>((*Pos)[0]->AsNumber());
                    Y = static_cast<int32>((*Pos)[1]->AsNumber());
                }
                UEdGraphNode* Node = nullptr;
                FString EventName;
                Op->TryGetStringField(TEXT("event"), EventName);
                if (ClassPath.Equals(TEXT("K2Node_CustomEvent"), ESearchCase::IgnoreCase) ||
                    ClassPath.EndsWith(TEXT(".K2Node_CustomEvent"), ESearchCase::IgnoreCase))
                {
                    // 自定义事件：不需要父类签名，名字由 spec 给
                    Node = KardsK2AddCustomEvent(Graph, EventName, Error);
                    if (Node) { Node->NodePosX = X; Node->NodePosY = Y; }
                }
                else
                {
                    Node = KardsK2CreateNodeEx(Graph, ClassPath, X, Y,
                        Function, OwnerClass, Variable, Macro, TargetClass, Error);
                }
                if (Node)
                {
                    if (!Comment.IsEmpty()) Node->NodeComment = Comment;
                    const FString Id = FString::Printf(TEXT("n%d"), NodeCounter++);
                    Nodes.Add(Id, Node);
                    Result->SetStringField(TEXT("id"), Id);
                    TArray<TSharedPtr<FJsonValue>> Pins;
                    for (UEdGraphPin* Pin : Node->Pins)
                    {
                        if (!Pin) continue;
                        Pins.Add(MakeShared<FJsonValueString>(Pin->PinName.ToString() + TEXT("|") +
                            (Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out")) + TEXT("|") + PinTypeToString(Pin->PinType)));
                    }
                    Result->SetArrayField(TEXT("pins"), Pins);
                }
                else bOk = false;
            }
            else if (Kind == TEXT("add_pin"))
            {
                UEdGraphNode* Node = nullptr;
                FString PinName, PinType, Dir;
                Op->TryGetStringField(TEXT("name"), PinName);
                Op->TryGetStringField(TEXT("type"), PinType);
                Op->TryGetStringField(TEXT("dir"), Dir);
                bOk = NodeFromJson(Op, Node, Error) &&
                    KardsK2AddPin(Node, PinName, PinType, !Dir.Equals(TEXT("in"), ESearchCase::IgnoreCase), Error);
            }
            else if (Kind == TEXT("add_function"))
            {
                // 新建本地函数图（全新的 UFunction，不是覆写父类）
                FString FunctionName;
                Op->TryGetStringField(TEXT("name"), FunctionName);
                UEdGraphNode* EntryResult = nullptr;
                UEdGraphNode* Entry = KardsK2AddFunctionGraph(Blueprint.Get(), FunctionName, EntryResult, Error);
                if (Entry)
                {
                    const FString Id = FString::Printf(TEXT("n%d"), NodeCounter++);
                    Nodes.Add(Id, Entry);
                    Result->SetStringField(TEXT("id"), Id);
                    if (EntryResult)
                    {
                        const FString ResultId = FString::Printf(TEXT("n%d"), NodeCounter++);
                        Nodes.Add(ResultId, EntryResult);
                        Result->SetStringField(TEXT("result"), ResultId);
                    }
                    TArray<TSharedPtr<FJsonValue>> GraphNames;
                    for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
                    {
                        if (FunctionGraph) GraphNames.Add(MakeShared<FJsonValueString>(FunctionGraph->GetName()));
                    }
                    Result->SetArrayField(TEXT("graphs"), GraphNames);
                }
                else bOk = false;
            }
            else if (Kind == TEXT("set_prop"))
            {
                UEdGraphNode* Node = nullptr;
                FString PropName;
                Op->TryGetStringField(TEXT("name"), PropName);
                bOk = NodeFromJson(Op, Node, Error) && KardsK2SetProperty(Node, PropName, Op->TryGetField(TEXT("value")), Error);
            }
            else if (Kind == TEXT("call"))
            {
                UEdGraphNode* Node = nullptr;
                FString FunctionName;
                Op->TryGetStringField(TEXT("fn"), FunctionName);
                TArray<TSharedPtr<FJsonValue>> Args;
                const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
                if (Op->TryGetArrayField(TEXT("args"), ArgsArray) && ArgsArray) Args = *ArgsArray;
                bOk = NodeFromJson(Op, Node, Error) && KardsK2CallApi(Node, FunctionName, Args, Error);
            }
            else if (Kind == TEXT("connect"))
            {
                const TSharedPtr<FJsonObject>* FromObj = nullptr;
                const TSharedPtr<FJsonObject>* ToObj = nullptr;
                if (Op->TryGetObjectField(TEXT("from"), FromObj) && Op->TryGetObjectField(TEXT("to"), ToObj))
                {
                    UEdGraphNode* FromNode = nullptr;
                    UEdGraphNode* ToNode = nullptr;
                    FString FromPin, ToPin;
                    (*FromObj)->TryGetStringField(TEXT("pin"), FromPin);
                    (*ToObj)->TryGetStringField(TEXT("pin"), ToPin);
                    bOk = NodeFromJson(*FromObj, FromNode, Error) && NodeFromJson(*ToObj, ToNode, Error);
                    if (bOk)
                    {
                        UEdGraphPin* A = PinOf(FromNode, FromPin);
                        UEdGraphPin* B = PinOf(ToNode, ToPin);
                        if (!A || !B) { bOk = false; Error = TEXT("pin not found"); }
                        else
                        {
                            const FPinConnectionResponse Response = Schema->CanCreateConnection(A, B);
                            if (Response.Response != CONNECT_RESPONSE_MAKE) { bOk = false; Error = Response.Message.ToString(); }
                            else bOk = Schema->TryCreateConnection(A, B);
                        }
                    }
                }
                else { bOk = false; Error = TEXT("connect needs 'from' and 'to' objects"); }
            }
            else if (Kind == TEXT("disconnect"))
            {
                UEdGraphNode* Node = nullptr;
                FString PinName;
                Op->TryGetStringField(TEXT("pin"), PinName);
                bOk = NodeFromJson(Op, Node, Error);
                if (bOk)
                {
                    UEdGraphPin* Pin = PinOf(Node, PinName);
                    if (Pin) Pin->BreakAllPinLinks();
                    else { bOk = false; Error = TEXT("pin not found"); }
                }
            }
            else if (Kind == TEXT("default"))
            {
                UEdGraphNode* Node = nullptr;
                FString PinName;
                Op->TryGetStringField(TEXT("pin"), PinName);
                bOk = NodeFromJson(Op, Node, Error);
                if (bOk)
                {
                    UEdGraphPin* Pin = PinOf(Node, PinName);
                    if (!Pin) { bOk = false; Error = TEXT("pin not found"); }
                    else
                    {
                        FString Value;
                        if (Op->TryGetField(TEXT("value")).IsValid() && Op->TryGetField(TEXT("value"))->Type == EJson::String)
                            Value = Op->GetStringField(TEXT("value"));
                        else if (Op->TryGetField(TEXT("value")).IsValid() && Op->TryGetField(TEXT("value"))->Type == EJson::Boolean)
                            Value = Op->GetBoolField(TEXT("value")) ? TEXT("true") : TEXT("false");
                        else if (Op->TryGetField(TEXT("value")).IsValid())
                            Value = FString::Printf(TEXT("%lld"), static_cast<long long>(Op->GetNumberField(TEXT("value"))));
                        Schema->TrySetDefaultValue(*Pin, Value);
                    }
                }
            }
            else if (Kind == TEXT("reset"))
            {
                // 清空事件图 + 删掉本轮新建的函数图，让重试从干净状态开始
                TArray<UEdGraphNode*> Doomed;
                for (UEdGraphNode* Node : Graph->Nodes) { if (Node) Doomed.Add(Node); }
                for (UEdGraphNode* Node : Doomed) { Node->BreakAllNodeLinks(); Graph->RemoveNode(Node); }
                for (int32 Index = Blueprint->FunctionGraphs.Num() - 1; Index >= 0; --Index)
                {
                    UEdGraph* FunctionGraph = Blueprint->FunctionGraphs[Index];
                    if (!FunctionGraph) continue;
                    FBlueprintEditorUtils::RemoveGraph(Blueprint.Get(), FunctionGraph);
                }
                Nodes.Empty();
                NodeCounter = 0;
                Result->SetStringField(TEXT("reset"), TEXT("ok"));
            }
            else if (Kind == TEXT("dump"))
            {
                FString Dump;
                if (FFileHelper::LoadFileToString(Dump, *(Bridge / TEXT("graph.json")))) Result->SetStringField(TEXT("graph"), Dump);
            }
            else if (Kind == TEXT("compile"))
            {
                Blueprint->Status = BS_Dirty;
                FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint.Get());
                FCompilerResultsLog Log;
                FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Log);
                Result->SetNumberField(TEXT("errors"), Log.NumErrors);
                Result->SetNumberField(TEXT("warnings"), Log.NumWarnings);
                TArray<TSharedPtr<FJsonValue>> Messages;
                for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
                {
                    Messages.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
                }
                Result->SetArrayField(TEXT("messages"), Messages);
                bOk = Log.NumErrors == 0;
            }
            else if (Kind == TEXT("save"))
            {
                Blueprint->Status = BS_Dirty;
                FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint.Get());
                FCompilerResultsLog Log;
                FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(), EBlueprintCompileOptions::SkipSave, &Log);
                if (Log.NumErrors > 0)
                {
                    bOk = false;
                    Error = FString::Printf(TEXT("compilation failed with %d error(s); not saved"), Log.NumErrors);
                }
                else
                {
                    // CDO 默认值（Name/title/Text/Type/faction/kredits/cardSet/isInPermanentPool/usedTriggers…）
                    // 必须在这里应用，否则卡属性全空。用和 -Mode=Build 完全相同的实现。
                    if (!ApplyDefaults(Blueprint.Get(), Spec, Report))
                    {
                        bOk = false;
                        Error = TEXT("应用 CDO 默认值失败");
                    }
                    else
                    {
                    // 只做**正常存盘**。不要在这里设 PKG_FilterEditorOnly —— 那是 Strip 阶段的活。
                    // UBlueprint 是编辑器对象，提前打上 FilterEditorOnly 会让下游 Verify 读到 0 个节点。
                    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
                    for (const TCHAR* Extension : {TEXT(".uasset"), TEXT(".uexp")})
                    {
                        const FString Existing = FPaths::ChangeExtension(Filename, Extension);
                        if (IFileManager::Get().FileExists(*Existing)) IFileManager::Get().Delete(*Existing, false, true, true);
                    }
                    Blueprint->MarkPackageDirty();
                    Package->SetDirtyFlag(true);
                    FSavePackageArgs Args;
                    Args.TopLevelFlags = RF_NoFlags;
                    Args.SaveFlags = SAVE_None;
                    FStringOutputDevice SaveErrors;
                    Args.Error = &SaveErrors;
                    Args.bSlowTask = false;
                    bOk = UPackage::SavePackage(Package.Get(), Blueprint.Get(), *Filename, Args);
                    if (!bOk) Error = TEXT("SavePackage failed: ") + SaveErrors;
                    else Result->SetStringField(TEXT("saved"), Filename);
                    }
                }
            }
            else if (Kind == TEXT("stop"))
            {
                bStop = true;
            }
            else
            {
                bOk = false;
                Error = TEXT("unknown op '") + Kind + TEXT("'");
            }

            Result->SetBoolField(TEXT("ok"), bOk);
            if (!bOk) Result->SetStringField(TEXT("error"), Error);
            WriteResult(Name, Result);
        }
        FPlatformProcess::Sleep(0.4f);
    }
    Report.Root->SetStringField(TEXT("session"), TEXT("stopped"));
    return true;
}

// ------------------------------------------------------------ scan registry

/**
 * Build a small "donor" AssetRegistry.bin containing only the assets under
 * -FilterPath, serialized with UE's own FAssetRegistryState writer. This is the
 * same serializer AssetRegistryTool uses for its convert step, so the name hash
 * table stays intact; the merge into the host registry is done by
 * AssetRegistryTool Merge.
 */
bool RunScanRegistry(const FString& Params, FBuildReport& Report)
{
    FString FilterPath, OutFile;
    if (!FParse::Value(*Params, TEXT("FilterPath="), FilterPath) || FilterPath.IsEmpty())
    {
        Report.Error(TEXT("-FilterPath=/Game/... is required for -Mode=ScanRegistry"));
        return false;
    }
    if (!FParse::Value(*Params, TEXT("Out="), OutFile) || FPaths::IsRelative(OutFile))
    {
        Report.Error(TEXT("an absolute -Out=...bin path is required for -Mode=ScanRegistry"));
        return false;
    }
    OutFile = FPaths::ConvertRelativePathToFull(OutFile);

    FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& Registry = Module.Get();
    Registry.SearchAllAssets(true);

    TArray<FAssetData> Assets;
    Registry.GetAssetsByPath(FName(*FilterPath), Assets, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ true);

    // -Include=name1,name2,... —— 只保留指定资产名。
    // 多卡合并时必须用它：filterPath 是整个 Mods 目录，不筛的话注册表会混进
    // 没进 pak 的卡，游戏建卡池时会去找不存在的包。
    FString IncludeList;
    // bShouldStopOnComma=false —— 默认行为会在逗号处截断，多卡列表就只剩第一个。
    FParse::Value(*Params, TEXT("Include="), IncludeList, /*bShouldStopOnComma*/ false);
    if (!IncludeList.IsEmpty())
    {
        TArray<FString> Wanted;
        IncludeList.ParseIntoArray(Wanted, TEXT(","), /*bCullEmpty*/ true);
        for (FString& Token : Wanted) { Token.TrimStartAndEndInline(); }
        Assets.RemoveAll([&Wanted](const FAssetData& Asset)
        {
            const FString Name = Asset.AssetName.ToString();
            for (const FString& Token : Wanted)
            {
                if (Token.IsEmpty()) continue;
                // Strip 之后包里只剩生成类，注册表里的名字是 "<card>_C"，所以两个都认。
                // 用精确匹配而不是包含匹配 —— 否则叫 "card_event" 的卡会匹配到所有卡。
                if (Name.Equals(Token, ESearchCase::IgnoreCase) ||
                    Name.Equals(Token + TEXT("_C"), ESearchCase::IgnoreCase)) return false;
            }
            return true;
        });
    }
    Report.Root->SetStringField(TEXT("filter_path"), FilterPath);
    if (!IncludeList.IsEmpty()) Report.Root->SetStringField(TEXT("include"), IncludeList);
    Report.Root->SetNumberField(TEXT("asset_count"), Assets.Num());

    FAssetRegistryState State;
    TArray<TSharedPtr<FJsonValue>> Listed;
    for (const FAssetData& Asset : Assets)
    {
        FAssetDataTagMap Tags;
        for (const auto& Pair : Asset.TagsAndValues)
        {
            Tags.Add(Pair.Key, Pair.Value.AsString());
        }
        const FSoftObjectPath ObjectPath = Asset.GetSoftObjectPath();
        State.AddAssetData(new FAssetData(Asset));
        State.AddTagsToAssetData(ObjectPath, MoveTemp(Tags));

        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("object_path"), ObjectPath.ToString());
        Item->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.ToString());
        Listed.Add(MakeShared<FJsonValueObject>(Item));

        // Cooked card packages carry the generated class as a second registry entry
        // ("<name>_C"). The on-disk scan only reports the Blueprint, so synthesize the
        // class entry from the Blueprint's own tags. The shipped package is class-only,
        // which means only this entry can actually resolve to an object at runtime.
        if (Asset.AssetClassPath != FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("Blueprint")))
        {
            continue;
        }
        UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
        UClass* GeneratedClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
        if (!GeneratedClass) continue;

        FAssetData ClassAsset;
        ClassAsset.PackageName = Asset.PackageName;
        ClassAsset.PackagePath = Asset.PackagePath;
        ClassAsset.AssetName = FName(*(Asset.AssetName.ToString() + TEXT("_C")));
        ClassAsset.AssetClassPath = FTopLevelAssetPath(TEXT("/Script/Engine"), TEXT("BlueprintGeneratedClass"));
        const FSoftObjectPath ClassPath = ClassAsset.GetSoftObjectPath();

        FAssetDataTagMap ClassTags;
        for (const TCHAR* Key : {TEXT("BlueprintType"), TEXT("NativeParentClass"), TEXT("ParentClass"),
                                 TEXT("PrimaryAssetName"), TEXT("PrimaryAssetType")})
        {
            FString Value;
            if (Asset.GetTagValue(FName(Key), Value)) ClassTags.Add(FName(Key), Value);
        }
        if (!ClassTags.Contains(FName(TEXT("PrimaryAssetName"))))
        {
            ClassTags.Add(FName(TEXT("PrimaryAssetName")), Asset.AssetName.ToString());
        }
        ClassTags.Add(FName(TEXT("ModuleName")), Asset.AssetName.ToString());
        ClassTags.Add(FName(TEXT("ClassFlags")), FString::FromInt(static_cast<int32>(GeneratedClass->GetClassFlags())));
        ClassTags.Add(FName(TEXT("NumReplicatedProperties")), TEXT("0"));

        State.AddAssetData(new FAssetData(ClassAsset));
        State.AddTagsToAssetData(ClassPath, MoveTemp(ClassTags));
        Report.Note(TEXT("synthesized registry entry ") + ClassPath.ToString());
    }
    Report.Root->SetArrayField(TEXT("assets"), Listed);

    FBufferArchive Archive;
    FAssetRegistrySerializationOptions Options;
    Options.bSerializeAssetRegistry = true;
    Options.bSerializeDependencies = true;
    Options.bSerializePackageData = true;
    Options.bSerializeSearchableNameDependencies = true;
    Options.bSerializeManageDependencies = true;
    Archive.SetFilterEditorOnly(true);
    if (!State.Save(Archive, Options))
    {
        Report.Error(TEXT("FAssetRegistryState::Save failed"));
        return false;
    }
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutFile), true))
    {
        Report.Error(TEXT("cannot create ") + FPaths::GetPath(OutFile));
        return false;
    }
    if (!FFileHelper::SaveArrayToFile(Archive, *OutFile))
    {
        Report.Error(TEXT("cannot write ") + OutFile);
        return false;
    }
    Report.Root->SetStringField(TEXT("out_file"), OutFile);
    Report.Root->SetNumberField(TEXT("bytes"), Archive.Num());
    return true;
}

// ------------------------------------------------------------------- verify

bool RunVerify(const FString& Params, FBuildReport& Report){
    FString PackageName;
    if (!FParse::Value(*Params, TEXT("Package="), PackageName) || PackageName.IsEmpty())
    {
        Report.Error(TEXT("-Package=/Game/... is required for -Mode=Verify"));
        return false;
    }
    const FString ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
    // A shipped card package contains only the generated class (no UBlueprint), so
    // verification must accept either shape.
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
    UClass* GeneratedClass = nullptr;
    if (Blueprint)
    {
        GeneratedClass = Blueprint->GeneratedClass;
        Report.Root->SetStringField(TEXT("object_path"), Blueprint->GetPathName());
        Report.Root->SetStringField(TEXT("parent_class"), GetPathNameSafe(Blueprint->ParentClass));
        Report.Root->SetBoolField(TEXT("status_ok"), Blueprint->Status != BS_Error);
    }
    else
    {
        GeneratedClass = LoadObject<UClass>(nullptr, *(ObjectPath + TEXT("_C")));
        if (!GeneratedClass)
        {
            Report.Error(TEXT("cannot load ") + ObjectPath + TEXT(" nor ") + ObjectPath + TEXT("_C"));
            return false;
        }
        Report.Root->SetStringField(TEXT("object_path"), GeneratedClass->GetPathName());
        Report.Root->SetStringField(TEXT("parent_class"), GetPathNameSafe(GeneratedClass->GetSuperClass()));
        Report.Root->SetBoolField(TEXT("status_ok"), true);
        Report.Note(TEXT("package contains no UBlueprint (shipped, class-only form)"));
    }
    Report.Root->SetStringField(TEXT("generated_class"), GetPathNameSafe(GeneratedClass));

    UObject* CDO = GeneratedClass->GetDefaultObject();
    TArray<TSharedPtr<FJsonValue>> Properties;
    int32 Count = 0;
    if (CDO)
    {
        for (TFieldIterator<FProperty> It(GeneratedClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_Edit)) continue;
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), Property->GetName());
            Item->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
            FString Exported;
            Property->ExportTextItem_Direct(Exported, Property->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);
            Item->SetStringField(TEXT("value"), Exported);
            Properties.Add(MakeShared<FJsonValueObject>(Item));
            ++Count;
        }
    }
    Report.Root->SetNumberField(TEXT("property_count"), Count);
    Report.Root->SetArrayField(TEXT("properties"), Properties);

    TArray<TSharedPtr<FJsonValue>> Graphs;
    for (UEdGraph* Graph : (Blueprint ? Blueprint->UbergraphPages : TArray<TObjectPtr<UEdGraph>>()))
    {
        if (!Graph) continue;
        auto Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), Graph->GetName());
        Item->SetNumberField(TEXT("nodes"), Graph->Nodes.Num());
        TArray<TSharedPtr<FJsonValue>> NodeNames;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;
            NodeNames.Add(MakeShared<FJsonValueString>(Node->GetClass()->GetName() + TEXT(":") + Node->GetNodeTitle(ENodeTitleType::ListView).ToString()));
        }
        Item->SetArrayField(TEXT("node_titles"), NodeNames);
        Graphs.Add(MakeShared<FJsonValueObject>(Item));
    }
    Report.Root->SetArrayField(TEXT("graphs"), Graphs);

    TArray<TSharedPtr<FJsonValue>> Variables;
    if (Blueprint)
    {
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            auto Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), Variable.VarName.ToString());
            Item->SetStringField(TEXT("type"), PinTypeToString(Variable.VarType));
            Variables.Add(MakeShared<FJsonValueObject>(Item));
        }
    }
    Report.Root->SetArrayField(TEXT("variables"), Variables);
    return true;
}
}

UKardsCardBuilderCommandlet::UKardsCardBuilderCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
    ShowErrorCount = true;
}

int32 UKardsCardBuilderCommandlet::Main(const FString& Params)
{
    FString Mode = TEXT("Build");
    FParse::Value(*Params, TEXT("Mode="), Mode);
    FString ReportFile;
    if (Mode.Equals(TEXT("Session"), ESearchCase::IgnoreCase))
    {
        // 会话是常驻的，-Report 可选；默认写到 bridge 目录里。
        FString Bridge;
        FParse::Value(*Params, TEXT("Bridge="), Bridge);
        ReportFile = Bridge.IsEmpty()
            ? (FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("KardsCardBuilderSession.json")))
            : (FPaths::ConvertRelativePathToFull(Bridge) / TEXT("session-report.json"));
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportFile), true))
        {
            UE_LOG(LogKardsCardBuilderCmd, Error, TEXT("cannot create report directory"));
            return 2;
        }
        IFileManager::Get().Delete(*ReportFile, false, true, true);
    }
    else if (!PrepareReportPath(Params, ReportFile))
    {
        return 2;
    }

    FBuildReport Report(*Mode);
    bool bSuccess = false;
    if (Mode.Equals(TEXT("Dump"), ESearchCase::IgnoreCase))
    {
        bSuccess = RunDump(Params, Report);
    }
    else if (Mode.Equals(TEXT("DumpEnum"), ESearchCase::IgnoreCase))
    {
        bSuccess = RunDumpEnum(Params, Report);
    }
    else if (Mode.Equals(TEXT("DumpModule"), ESearchCase::IgnoreCase))
    {
        bSuccess = RunDumpModule(Params, Report);
    }
    else if (Mode.Equals(TEXT("Verify"), ESearchCase::IgnoreCase))
    {
        bSuccess = RunVerify(Params, Report);
    }
    else if (Mode.Equals(TEXT("ScanRegistry"), ESearchCase::IgnoreCase))
    {
        bSuccess = RunScanRegistry(Params, Report);
    }
    else if (Mode.Equals(TEXT("Strip"), ESearchCase::IgnoreCase))
    {
        bSuccess = RunStrip(Params, Report);
    }
    else if (Mode.Equals(TEXT("Session"), ESearchCase::IgnoreCase))
    {
        bSuccess = RunSession(Params, Report);
    }
    else if (Mode.Equals(TEXT("Build"), ESearchCase::IgnoreCase))
    {
        bSuccess = RunBuild(Params, Report);
    }
    else
    {
        Report.Error(TEXT("unknown -Mode=") + Mode + TEXT("; expected Dump, DumpEnum, DumpModule, Build, Verify, ScanRegistry, Strip or Session"));
    }
    return Report.Write(ReportFile, bSuccess);
}
