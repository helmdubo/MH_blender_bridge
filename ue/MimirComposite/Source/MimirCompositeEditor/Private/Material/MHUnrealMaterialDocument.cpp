#include "Material/MHUnrealMaterialDocument.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture.h"
#include "MaterialShared.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"

#include <charconv>

namespace UE::MimirComposite
{
namespace
{

bool Fail(FString& Error, const FString& Detail)
{
    Error = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: ue_instance: ") + Detail;
    return false;
}

bool Grammar(FString& Error, const FString& Detail)
{
    Error = TEXT("MH_E_MATERIAL_GRAMMAR: ue_instance: ") + Detail;
    return false;
}

bool IsAssetPath(const FString& Value)
{
    const FSoftObjectPath Path(Value);
    return !Value.IsEmpty() && FPackageName::IsValidObjectPath(Value) &&
        Path.IsValid() && Path.GetSubPathString().IsEmpty() &&
        !Path.GetLongPackageName().StartsWith(TEXT("/Engine/Transient")) &&
        !Path.GetLongPackageName().StartsWith(TEXT("/Temp/")) &&
        !Path.GetLongPackageName().StartsWith(TEXT("/Script/")) &&
        Path.ToString() == Value;
}

bool Fields(const TSharedPtr<FJsonObject>& Object, const TSet<FString>& Allowed, FString& Error)
{
    if (!Object.IsValid()) return Grammar(Error, TEXT("expected an object"));
    for (const auto& Pair : Object->Values)
    {
        if (!Allowed.Contains(Pair.Key)) return Grammar(Error, TEXT("unknown field '") + Pair.Key + TEXT("'"));
    }
    return true;
}

bool Number(const TSharedPtr<FJsonValue>& Value, double& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Number) return false;
    return Value->TryGetNumber(Out) && FMath::IsFinite(Out);
}

bool Integer(const TSharedPtr<FJsonValue>& Value, int32& Out)
{
    double Parsed = 0;
    if (!Number(Value, Parsed) || Parsed < MIN_int32 || Parsed > MAX_int32 || FMath::FloorToDouble(Parsed) != Parsed) return false;
    Out = static_cast<int32>(Parsed);
    return true;
}

bool Float(const TSharedPtr<FJsonValue>& Value, float& Out)
{
    double Parsed = 0;
    if (!Number(Value, Parsed)) return false;
    Out = static_cast<float>(Parsed);
    return FMath::IsFinite(Out);
}

bool ReadInfo(const TSharedPtr<FJsonObject>& Row, FMaterialParameterInfo& Out, FString& Error)
{
    FString Name;
    int32 Association = 0;
    int32 Index = INDEX_NONE;
    if (!Row->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty() || Name.Len() >= NAME_SIZE ||
        !Integer(Row->TryGetField(TEXT("association")), Association) ||
        !Integer(Row->TryGetField(TEXT("index")), Index))
        return Grammar(Error, TEXT("parameter requires a nonempty name, integer association and index"));
    for (const TCHAR Character : Name)
        if (Character == 0) return Grammar(Error, TEXT("parameter name cannot contain NUL"));
    const auto Kind = static_cast<EMaterialParameterAssociation>(Association);
    if ((Kind != EMaterialParameterAssociation::GlobalParameter &&
         Kind != EMaterialParameterAssociation::LayerParameter && Kind != EMaterialParameterAssociation::BlendParameter) ||
        (Kind == EMaterialParameterAssociation::GlobalParameter ? Index != INDEX_NONE : Index < 0))
        return Grammar(Error, TEXT("invalid parameter association/index"));
    Out = FMaterialParameterInfo(FName(*Name), Kind, Index);
    if (Out.Name.IsNone()) return Grammar(Error, TEXT("parameter name cannot be None"));
    return true;
}

TSharedPtr<FJsonObject> InfoObject(const FMaterialParameterInfo& Info)
{
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("name"), Info.Name.ToString());
    Row->SetNumberField(TEXT("association"), static_cast<int32>(Info.Association));
    Row->SetNumberField(TEXT("index"), Info.Index);
    return Row;
}

bool InfoLess(const FMaterialParameterInfo& A, const FMaterialParameterInfo& B)
{
    const int32 NameOrder = A.Name.ToString().Compare(B.Name.ToString(), ESearchCase::CaseSensitive);
    if (NameOrder != 0) return NameOrder < 0;
    if (A.Association != B.Association) return static_cast<int32>(A.Association) < static_cast<int32>(B.Association);
    return A.Index < B.Index;
}

// The version-1 base payload is the complete reflected UE 5.7 struct. Only
// booleans, finite numbers, enums and nested value structs are accepted. No
// UObject deserialization, text import or unvalidated reflection writes occur.
bool ReadStruct(UStruct* Type, void* Data, const TSharedPtr<FJsonObject>& Object, FString& Error);
TSharedPtr<FJsonObject> WriteStruct(UStruct* Type, const void* Data, FString& Error);

bool ReadProperty(FProperty* Property, void* Address, const TSharedPtr<FJsonValue>& Value, FString& Error)
{
    if (FBoolProperty* Bool = CastField<FBoolProperty>(Property))
    {
        if (!Value.IsValid() || Value->Type != EJson::Boolean) return Grammar(Error, Property->GetName() + TEXT(" must be boolean"));
        Bool->SetPropertyValue(Address, Value->AsBool());
        return true;
    }
    FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
    UEnum* Enum = nullptr;
    if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        Numeric = EnumProperty->GetUnderlyingProperty();
        Enum = EnumProperty->GetEnum();
    }
    else if (FByteProperty* Byte = CastField<FByteProperty>(Property)) Enum = Byte->Enum;
    if (Numeric != nullptr)
    {
        double Parsed = 0;
        if (!Number(Value, Parsed)) return Grammar(Error, Property->GetName() + TEXT(" must be a finite number"));
        if (Numeric->IsFloatingPoint())
        {
            if (CastField<FFloatProperty>(Numeric) != nullptr && !FMath::IsFinite(static_cast<float>(Parsed)))
                return Grammar(Error, Property->GetName() + TEXT(" overflows float32"));
            Numeric->SetFloatingPointPropertyValue(Address, Parsed);
        }
        else
        {
            int32 Integral = 0;
            if (!Integer(Value, Integral) || (Enum != nullptr &&
                (!Enum->IsValidEnumValue(Integral) ||
                 Enum->GetNameStringByValue(Integral).EndsWith(TEXT("_MAX")) ||
                 Enum->GetNameStringByValue(Integral).EndsWith(TEXT("_NUM")))) ||
                (CastField<FByteProperty>(Numeric) != nullptr && (Integral < 0 || Integral > MAX_uint8)))
                return Grammar(Error, Property->GetName() + TEXT(" is not a valid integer/enum value"));
            Numeric->SetIntPropertyValue(Address, static_cast<uint64>(Integral));
        }
        return true;
    }
    if (FStructProperty* Struct = CastField<FStructProperty>(Property))
    {
        if (!Value.IsValid() || Value->Type != EJson::Object) return Grammar(Error, Property->GetName() + TEXT(" must be an object"));
        return ReadStruct(Struct->Struct, Address, Value->AsObject(), Error);
    }
    return Grammar(Error, TEXT("unsupported base property type: ") + Property->GetName());
}

bool ReadStruct(UStruct* Type, void* Data, const TSharedPtr<FJsonObject>& Object, FString& Error)
{
    if (!Object.IsValid()) return Grammar(Error, TEXT("base_overrides must be an object"));
    int32 Count = 0;
    for (TFieldIterator<FProperty> It(Type); It; ++It)
    {
        FProperty* Property = *It;
        ++Count;
        const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Property->GetName());
        if (Property->ArrayDim != 1 || Value == nullptr ||
            !ReadProperty(Property, Property->ContainerPtrToValuePtr<void>(Data), *Value, Error))
            return Error.IsEmpty() ? Grammar(Error, TEXT("missing/unsupported base property: ") + Property->GetName()) : false;
    }
    return Count == Object->Values.Num() || Grammar(Error, TEXT("unknown base_overrides field"));
}

TSharedPtr<FJsonObject> WriteStruct(UStruct* Type, const void* Data, FString& Error)
{
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Type); It; ++It)
    {
        FProperty* Property = *It;
        if (Property->ArrayDim != 1) { Grammar(Error, TEXT("unsupported base property array")); return nullptr; }
        const void* Address = Property->ContainerPtrToValuePtr<void>(Data);
        if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property))
        {
            Object->SetBoolField(Property->GetName(), Bool->GetPropertyValue(Address));
            continue;
        }
        const FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
        if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property)) Numeric = Enum->GetUnderlyingProperty();
        if (Numeric != nullptr)
        {
            const double Value = Numeric->IsFloatingPoint()
                ? Numeric->GetFloatingPointPropertyValue(Address)
                : static_cast<double>(Numeric->GetSignedIntPropertyValue(Address));
            if (!FMath::IsFinite(Value)) { Grammar(Error, TEXT("nonfinite base property: ") + Property->GetName()); return nullptr; }
            Object->SetNumberField(Property->GetName(), Value);
        }
        else if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
        {
            TSharedPtr<FJsonObject> Nested = WriteStruct(Struct->Struct, Address, Error);
            if (!Nested.IsValid()) return nullptr;
            Object->SetObjectField(Property->GetName(), Nested);
        }
        else { Grammar(Error, TEXT("unsupported base property type: ") + Property->GetName()); return nullptr; }
    }
    return Object;
}

void JsonString(const FString& Value, FString& Out)
{
    Out += TEXT("\"");
    for (TCHAR C : Value)
    {
        if (C == TEXT('"')) Out += TEXT("\\\"");
        else if (C == TEXT('\\')) Out += TEXT("\\\\");
        else if (C < TEXT(' ')) Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(C));
        else Out.AppendChar(C);
    }
    Out += TEXT("\"");
}

bool Json(const TSharedPtr<FJsonValue>& Value, const int32 Depth, FString& Out, FString& Error)
{
    const FString Indent = FString::ChrN(Depth * 2, TEXT(' '));
    if (!Value.IsValid()) return Grammar(Error, TEXT("invalid JSON value"));
    if (Value->Type == EJson::String) JsonString(Value->AsString(), Out);
    else if (Value->Type == EJson::Boolean) Out += Value->AsBool() ? TEXT("true") : TEXT("false");
    else if (Value->Type == EJson::Number)
    {
        const double NumberValue = Value->AsNumber();
        if (!FMath::IsFinite(NumberValue)) return Grammar(Error, TEXT("nonfinite number"));
        if (NumberValue == 0) Out += TEXT("0");
        else
        {
            ANSICHAR Buffer[128];
            const float FloatValue = static_cast<float>(NumberValue);
            const auto Result = static_cast<double>(FloatValue) == NumberValue
                ? std::to_chars(Buffer, Buffer + UE_ARRAY_COUNT(Buffer), FloatValue)
                : std::to_chars(Buffer, Buffer + UE_ARRAY_COUNT(Buffer), NumberValue);
            if (Result.ec != std::errc()) return Grammar(Error, TEXT("number serialization failed"));
            const FUTF8ToTCHAR Converted(Buffer, static_cast<int32>(Result.ptr - Buffer));
            Out.AppendChars(Converted.Get(), Converted.Length());
        }
    }
    else if (Value->Type == EJson::Object)
    {
        const auto Object = Value->AsObject();
        TArray<FString> Keys;
        Object->Values.GenerateKeyArray(Keys);
        Keys.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
        Out += TEXT("{");
        for (int32 I = 0; I < Keys.Num(); ++I)
        {
            Out += TEXT("\n") + Indent + TEXT("  ");
            JsonString(Keys[I], Out);
            Out += TEXT(": ");
            if (!Json(Object->Values.FindChecked(Keys[I]), Depth + 1, Out, Error)) return false;
            if (I + 1 < Keys.Num()) Out += TEXT(",");
        }
        if (!Keys.IsEmpty()) Out += TEXT("\n") + Indent;
        Out += TEXT("}");
    }
    else if (Value->Type == EJson::Array)
    {
        const auto& Array = Value->AsArray();
        Out += TEXT("[");
        for (int32 I = 0; I < Array.Num(); ++I)
        {
            Out += TEXT("\n") + Indent + TEXT("  ");
            if (!Json(Array[I], Depth + 1, Out, Error)) return false;
            if (I + 1 < Array.Num()) Out += TEXT(",");
        }
        if (!Array.IsEmpty()) Out += TEXT("\n") + Indent;
        Out += TEXT("]");
    }
    else return Grammar(Error, TEXT("null or unsupported JSON value"));
    return true;
}

bool Guid(const TSharedPtr<FJsonObject>& Row, FGuid& Out, FString& Error)
{
    FString Value;
    if (!Row->TryGetStringField(TEXT("expression_guid"), Value) ||
        !FGuid::ParseExact(Value, EGuidFormats::DigitsWithHyphens, Out) ||
        Out.ToString(EGuidFormats::DigitsWithHyphens).ToLower() != Value)
        return Grammar(Error, TEXT("expression_guid must be a lowercase hyphenated GUID"));
    return true;
}

} // namespace

bool MHParseUnrealMaterialV1(const TSharedPtr<FJsonObject>& Payload, FMHMaterialDocument& OutDocument, FString& OutError)
{
    OutDocument = FMHMaterialDocument();
    OutError.Reset();
    const TSet<FString> Allowed = {TEXT("version"), TEXT("parent"), TEXT("scalars"), TEXT("vectors"),
        TEXT("textures"), TEXT("static_switches"), TEXT("static_masks"), TEXT("base_overrides")};
    if (!Fields(Payload, Allowed, OutError)) return false;
    int32 Version = 0;
    FString Parent;
    if (!Integer(Payload->TryGetField(TEXT("version")), Version) || Version != 1 ||
        !Payload->TryGetStringField(TEXT("parent"), Parent) || !IsAssetPath(Parent))
        return Grammar(OutError, TEXT("version must be 1 and parent must be a persistent UE asset object path"));
    auto Data = MakeShared<FMHUnrealMaterialInstanceData>();
    Data->Parent = FSoftObjectPath(Parent);
    for (const TCHAR* Category : {TEXT("scalars"), TEXT("vectors"), TEXT("textures"), TEXT("static_switches"), TEXT("static_masks")})
    {
        const TSharedPtr<FJsonValue> CategoryValue = Payload->TryGetField(Category);
        if (!CategoryValue.IsValid()) continue;
        if (CategoryValue->Type != EJson::Array) return Grammar(OutError, FString(Category) + TEXT(" must be an array"));
        const FString Kind(Category);
        const bool bStatic = Kind.StartsWith(TEXT("static_"));
        TSet<FString> RowFields = {TEXT("name"), TEXT("association"), TEXT("index"), TEXT("value")};
        if (bStatic) RowFields.Add(TEXT("expression_guid"));
        TArray<FMaterialParameterInfo> Seen;
        for (const TSharedPtr<FJsonValue>& RowValue : CategoryValue->AsArray())
        {
            if (!RowValue.IsValid() || RowValue->Type != EJson::Object) return Grammar(OutError, Kind + TEXT(" row must be an object"));
            const auto Row = RowValue->AsObject();
            FMaterialParameterInfo Info;
            if (!Fields(Row, RowFields, OutError) || !ReadInfo(Row, Info, OutError)) return false;
            if (Seen.Contains(Info)) return Grammar(OutError, Kind + TEXT(" has duplicate parameter identity: ") + Info.Name.ToString());
            Seen.Add(Info);
            const auto Value = Row->TryGetField(TEXT("value"));
            if (Kind == TEXT("scalars"))
            {
                float Scalar = 0;
                if (!Float(Value, Scalar)) return Grammar(OutError, TEXT("scalar must be finite float32"));
                Data->Scalars.Emplace(Info, Scalar);
            }
            else if (Kind == TEXT("vectors"))
            {
                if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 4) return Grammar(OutError, TEXT("vector must have four numbers"));
                FLinearColor Color;
                for (int32 I = 0; I < 4; ++I) if (!Float(Value->AsArray()[I], Color.Component(I))) return Grammar(OutError, TEXT("vector component must be finite float32"));
                Data->Vectors.Emplace(Info, Color);
            }
            else if (Kind == TEXT("textures"))
            {
                FString Path;
                if (!Value.IsValid() || !Value->TryGetString(Path) || !IsAssetPath(Path)) return Grammar(OutError, TEXT("texture must be a persistent UE asset object path"));
                Data->Textures.Emplace(Info, FSoftObjectPath(Path));
            }
            else
            {
                FGuid Expression;
                if (!Guid(Row, Expression, OutError)) return false;
                if (Kind == TEXT("static_switches"))
                {
                    if (!Value.IsValid() || Value->Type != EJson::Boolean) return Grammar(OutError, TEXT("static switch value must be boolean"));
                    Data->StaticSwitches.Add(FStaticSwitchParameter(Info, Value->AsBool(), true, Expression));
                }
                else
                {
                    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 4) return Grammar(OutError, TEXT("static mask must have four booleans"));
                    for (const auto& Bit : Value->AsArray()) if (!Bit.IsValid() || Bit->Type != EJson::Boolean) return Grammar(OutError, TEXT("static mask component must be boolean"));
                    FStaticComponentMaskParameter Mask;
                    Mask.ParameterInfo = Info;
                    Mask.ExpressionGUID = Expression;
                    Mask.bOverride = true;
                    Mask.R = Value->AsArray()[0]->AsBool(); Mask.G = Value->AsArray()[1]->AsBool();
                    Mask.B = Value->AsArray()[2]->AsBool(); Mask.A = Value->AsArray()[3]->AsBool();
                    Data->StaticComponentMasks.Add(Mask);
                }
            }
        }
    }
    if (const auto Base = Payload->TryGetField(TEXT("base_overrides")); Base.IsValid())
    {
        if (Base->Type != EJson::Object || !ReadStruct(FMaterialInstanceBasePropertyOverrides::StaticStruct(), &Data->BaseOverrides, Base->AsObject(), OutError))
            return OutError.IsEmpty() ? Grammar(OutError, TEXT("base_overrides must be an object")) : false;
    }
    OutDocument.Mode = EMHMaterialMode::UnrealInstance;
    OutDocument.Parent = Parent;
    OutDocument.UnrealInstance = MoveTemp(Data);
    return true;
}

bool MHWriteUnrealMaterialV1(const FMHMaterialDocument& Document, TArray<uint8>& OutBytes, FString& OutError)
{
    OutBytes.Reset(); OutError.Reset();
    if (Document.Mode != EMHMaterialMode::UnrealInstance || !Document.UnrealInstance.IsValid() ||
        Document.bHasTwoSided || !Document.Textures.IsEmpty() || !Document.Params.IsEmpty() ||
        Document.Parent != Document.UnrealInstance->Parent.ToString())
        return Grammar(OutError, TEXT("inconsistent UE instance document"));
    const auto& Data = *Document.UnrealInstance;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("version"), 1);
    Payload->SetStringField(TEXT("parent"), Data.Parent.ToString());
    auto Scalars = Data.Scalars;
    Scalars.Sort([](const auto& A, const auto& B) { return InfoLess(A.Key, B.Key); });
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const auto& Pair : Scalars)
    {
        auto Row = InfoObject(Pair.Key); Row->SetNumberField(TEXT("value"), Pair.Value);
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Payload->SetArrayField(TEXT("scalars"), Rows);
    Rows.Reset();
    auto Vectors = Data.Vectors;
    Vectors.Sort([](const auto& A, const auto& B) { return InfoLess(A.Key, B.Key); });
    for (const auto& Pair : Vectors)
    {
        auto Row = InfoObject(Pair.Key);
        TArray<TSharedPtr<FJsonValue>> Values;
        for (int32 I = 0; I < 4; ++I) Values.Add(MakeShared<FJsonValueNumber>(Pair.Value.Component(I)));
        Row->SetArrayField(TEXT("value"), Values); Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Payload->SetArrayField(TEXT("vectors"), Rows);
    Rows.Reset();
    auto Textures = Data.Textures;
    Textures.Sort([](const auto& A, const auto& B) { return InfoLess(A.Key, B.Key); });
    for (const auto& Pair : Textures)
    {
        auto Row = InfoObject(Pair.Key); Row->SetStringField(TEXT("value"), Pair.Value.ToString());
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Payload->SetArrayField(TEXT("textures"), Rows);
    Rows.Reset();
    auto Switches = Data.StaticSwitches;
    Switches.Sort([](const auto& A, const auto& B) { return InfoLess(A.ParameterInfo, B.ParameterInfo); });
    for (const auto& Switch : Switches)
    {
        if (!Switch.bOverride) return Grammar(OutError, TEXT("static switch snapshot must contain only local overrides"));
        auto Row = InfoObject(Switch.ParameterInfo);
        Row->SetBoolField(TEXT("value"), Switch.Value);
        Row->SetStringField(TEXT("expression_guid"), Switch.ExpressionGUID.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Payload->SetArrayField(TEXT("static_switches"), Rows);
    Rows.Reset();
    auto Masks = Data.StaticComponentMasks;
    Masks.Sort([](const auto& A, const auto& B) { return InfoLess(A.ParameterInfo, B.ParameterInfo); });
    for (const auto& Mask : Masks)
    {
        if (!Mask.bOverride) return Grammar(OutError, TEXT("static mask snapshot must contain only local overrides"));
        auto Row = InfoObject(Mask.ParameterInfo);
        Row->SetStringField(TEXT("expression_guid"), Mask.ExpressionGUID.ToString(EGuidFormats::DigitsWithHyphens).ToLower());
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const bool Bit : {Mask.R, Mask.G, Mask.B, Mask.A}) Values.Add(MakeShared<FJsonValueBoolean>(Bit));
        Row->SetArrayField(TEXT("value"), Values); Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Payload->SetArrayField(TEXT("static_masks"), Rows);
    auto Base = WriteStruct(FMaterialInstanceBasePropertyOverrides::StaticStruct(), &Data.BaseOverrides, OutError);
    if (!Base.IsValid()) return false;
    Payload->SetObjectField(TEXT("base_overrides"), Base);
    FMHMaterialDocument Validated;
    if (!MHParseUnrealMaterialV1(Payload, Validated, OutError)) return false;
    auto Root = MakeShared<FJsonObject>(); Root->SetObjectField(TEXT("ue_instance"), Payload);
    FString Text;
    if (!Json(MakeShared<FJsonValueObject>(Root), 0, Text, OutError)) return false;
    Text += TEXT("\n");
    const FTCHARToUTF8 Bytes(*Text, Text.Len());
    OutBytes.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
    return true;
}

bool MHExtractUnrealMaterialV1(const UMaterialInstanceConstant& Material, FMHMaterialDocument& OutDocument, FString& OutError)
{
    OutDocument = FMHMaterialDocument(); OutError.Reset();
    if (Material.Parent == nullptr || !IsAssetPath(Material.Parent->GetPathName()) || Material.Parent->HasAnyFlags(RF_Transient))
        return Fail(OutError, TEXT("parent must be a persistent UE material asset"));
    const auto Unsupported = [&](const bool Present, const TCHAR* Name)
    {
        return Present ? Fail(OutError, FString(TEXT("unsupported local state: ")) + Name) : true;
    };
    if (!Unsupported(!Material.DoubleVectorParameterValues.IsEmpty(), TEXT("double vector parameters")) ||
        !Unsupported(!Material.FontParameterValues.IsEmpty(), TEXT("font parameters")) ||
        !Unsupported(!Material.TextureCollectionParameterValues.IsEmpty(), TEXT("texture collection parameters")) ||
        !Unsupported(!Material.RuntimeVirtualTextureParameterValues.IsEmpty(), TEXT("runtime virtual texture parameters")) ||
        !Unsupported(!Material.SparseVolumeTextureParameterValues.IsEmpty(), TEXT("sparse volume texture parameters")) ||
        !Unsupported(!Material.ParameterCollectionParameterValues.IsEmpty(), TEXT("parameter collection parameters"))) return false;
    const FStaticParameterSet Static = Material.GetStaticParameters();
    if (!Unsupported(Static.bHasMaterialLayers, TEXT("material layer stack")) ||
        !Unsupported(!Static.EditorOnly.TerrainLayerWeightParameters.IsEmpty(), TEXT("terrain layer weight parameters"))) return false;
    auto Data = MakeShared<FMHUnrealMaterialInstanceData>();
    Data->Parent = FSoftObjectPath(Material.Parent);
    Data->BaseOverrides = Material.BasePropertyOverrides;
    for (const FScalarParameterValue& Value : Material.ScalarParameterValues)
    {
#if WITH_EDITORONLY_DATA
        if (Value.AtlasData.bIsUsedAsAtlasPosition || !Value.AtlasData.Atlas.IsNull() || !Value.AtlasData.Curve.IsNull())
            return Fail(OutError, TEXT("unsupported scalar atlas override: ") + Value.ParameterInfo.Name.ToString());
#endif
        Data->Scalars.Emplace(Value.ParameterInfo, Value.ParameterValue);
    }
    for (const FVectorParameterValue& Value : Material.VectorParameterValues) Data->Vectors.Emplace(Value.ParameterInfo, Value.ParameterValue);
    for (const FTextureParameterValue& Value : Material.TextureParameterValues)
    {
        if (Value.ParameterValue == nullptr || Value.ParameterValue->HasAnyFlags(RF_Transient) || !IsAssetPath(Value.ParameterValue->GetPathName()))
            return Fail(OutError, TEXT("texture override is null or nonpersistent: ") + Value.ParameterInfo.Name.ToString());
        Data->Textures.Emplace(Value.ParameterInfo, FSoftObjectPath(Value.ParameterValue));
    }
    for (const auto& Value : Static.StaticSwitchParameters) if (Value.bOverride) Data->StaticSwitches.Add(Value);
    for (const auto& Value : Static.EditorOnly.StaticComponentMaskParameters) if (Value.bOverride) Data->StaticComponentMasks.Add(Value);
    OutDocument.Mode = EMHMaterialMode::UnrealInstance;
    OutDocument.Parent = Data->Parent.ToString();
    OutDocument.UnrealInstance = MoveTemp(Data);
    TArray<uint8> Bytes;
    if (!MHWriteUnrealMaterialV1(OutDocument, Bytes, OutError)) { OutDocument = FMHMaterialDocument(); return false; }
    return true;
}

bool MHApplyUnrealMaterialV1(UMaterialInstanceConstant& Material, UMaterialInterface& Parent,
    const FMHMaterialDocument& Document, FString& OutError)
{
    TArray<uint8> Bytes;
    if (!MHWriteUnrealMaterialV1(Document, Bytes, OutError)) return false;
    const auto& Data = *Document.UnrealInstance;
    if (Data.Parent.TryLoad() != &Parent || Parent.GetPathName() != Document.Parent || Parent.HasAnyFlags(RF_Transient))
        return Fail(OutError, TEXT("parent does not resolve to the exact source asset: ") + Document.Parent);
    TSet<const UMaterialInterface*> Seen;
    for (const UMaterialInterface* Current = &Parent; Current != nullptr;)
    {
        if (Current == &Material || Seen.Contains(Current)) return Fail(OutError, TEXT("parent cycle would be created"));
        Seen.Add(Current);
        const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current);
        Current = Instance != nullptr ? Instance->Parent.Get() : nullptr;
    }
    TArray<UTexture*> Textures;
    for (const auto& Pair : Data.Textures)
    {
        UTexture* Texture = Cast<UTexture>(Pair.Value.TryLoad());
        if (Texture == nullptr || Texture->GetPathName() != Pair.Value.ToString() || Texture->HasAnyFlags(RF_Transient))
            return Fail(OutError, TEXT("texture does not resolve to the exact source asset: ") + Pair.Value.ToString());
        Textures.Add(Texture);
    }
    Material.Modify();
    {
        FMaterialUpdateContext UpdateContext;
        Material.SetParentEditorOnly(&Parent);
        {
            FMaterialInstanceParameterUpdateContext Parameters(&Material, EMaterialInstanceClearParameterFlag::All);
            Parameters.GetStaticParameters().StaticSwitchParameters = Data.StaticSwitches;
            Parameters.GetStaticParameters().EditorOnly.StaticComponentMaskParameters = Data.StaticComponentMasks;
            Parameters.SetBasePropertyOverrides(Data.BaseOverrides);
            for (const auto& Pair : Data.Scalars) Material.SetScalarParameterValueEditorOnly(Pair.Key, Pair.Value);
            for (const auto& Pair : Data.Vectors) Material.SetVectorParameterValueEditorOnly(Pair.Key, Pair.Value);
            for (int32 I = 0; I < Data.Textures.Num(); ++I) Material.SetTextureParameterValueEditorOnly(Data.Textures[I].Key, Textures[I]);
        }
        Material.PostEditChange();
        UpdateContext.AddMaterialInstance(&Material);
    }
    Material.MarkPackageDirty();
    return true;
}

} // namespace UE::MimirComposite
