#include "Source/MHFbxPassport.h"

#include "Codec/MHCompositeCodec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#pragma pack(push, 8)
THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END
#pragma pack(pop)

namespace UE::MimirComposite
{
namespace
{

bool Fail(FString& OutError, const FString& Message)
{
    OutError = FString::Printf(TEXT("MH_E_PASSPORT_INVALID: %s"), *Message);
    return false;
}

bool SerializeCompactObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
{
    OutJson.Reset();
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
    return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
}

bool IsHashString(const FString& Value)
{
    if (Value.Len() != 21 || !Value.StartsWith(TEXT("xxh3:")))
    {
        return false;
    }
    for (int32 Index = 5; Index < Value.Len(); ++Index)
    {
        const TCHAR Char = Value[Index];
        if (!((Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f'))))
        {
            return false;
        }
    }
    return true;
}

bool ValidatePassportDocument(
    const FString& CarrierText,
    FMHFbxPassport& OutPassport,
    FString& OutError)
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(CarrierText);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return Fail(OutError, TEXT("carrier is not valid JSON"));
    }

    static const TCHAR* RequiredFields[] = {
        TEXT("schema"), TEXT("schema_version"), TEXT("resource_uid"), TEXT("kind"),
        TEXT("name"), TEXT("lod_levels"), TEXT("lod_policy"), TEXT("geometry_hash"),
        TEXT("material_slots"), TEXT("properties"), TEXT("exporter")};
    if (Root->Values.Num() != UE_ARRAY_COUNT(RequiredFields))
    {
        return Fail(OutError, TEXT("passport fields differ from mh.fbx_passport v1"));
    }
    for (const TCHAR* Field : RequiredFields)
    {
        if (!Root->Values.Contains(Field))
        {
            return Fail(OutError, FString::Printf(TEXT("missing passport field %s"), Field));
        }
    }

    FString Schema;
    if (!Root->TryGetStringField(TEXT("schema"), Schema) || Schema != TEXT("mh.fbx_passport"))
    {
        return Fail(OutError, TEXT("unknown schema"));
    }
    int64 Version = 0;
    if (!Root->TryGetNumberField(TEXT("schema_version"), Version) || Version != 1)
    {
        return Fail(OutError, TEXT("unsupported schema_version"));
    }

    FString ResourceUid;
    if (!Root->TryGetStringField(TEXT("resource_uid"), ResourceUid) || !MHIsCanonicalUuid(ResourceUid))
    {
        return Fail(OutError, TEXT("resource_uid must be a canonical lowercase UUID"));
    }
    FString Kind;
    if (!Root->TryGetStringField(TEXT("kind"), Kind) || Kind != TEXT("static_mesh"))
    {
        return Fail(OutError, TEXT("kind must be static_mesh"));
    }
    FString Name;
    if (!Root->TryGetStringField(TEXT("name"), Name) || !MHIsValidResourceName(Name))
    {
        return Fail(OutError, TEXT("name must match [A-Za-z0-9_ -]"));
    }

    const TArray<TSharedPtr<FJsonValue>>* LodLevels = nullptr;
    if (!Root->TryGetArrayField(TEXT("lod_levels"), LodLevels) || LodLevels->Num() == 0)
    {
        return Fail(OutError, TEXT("lod_levels must be a non-empty array"));
    }
    TArray<int32> Levels;
    for (const TSharedPtr<FJsonValue>& LevelValue : *LodLevels)
    {
        int64 Level = -1;
        if (!LevelValue.IsValid() || LevelValue->Type != EJson::Number ||
            !LevelValue->TryGetNumber(Level) || Level != Levels.Num())
        {
            return Fail(OutError, TEXT("lod_levels must be contiguous [0..N]"));
        }
        Levels.Add(static_cast<int32>(Level));
    }
    FString LodPolicy;
    if (!Root->TryGetStringField(TEXT("lod_policy"), LodPolicy) ||
        (LodPolicy != TEXT("authored") && LodPolicy != TEXT("generated") && LodPolicy != TEXT("nanite")))
    {
        return Fail(OutError, TEXT("lod_policy must be authored, generated or nanite"));
    }
    if (Levels.Num() > 1 && LodPolicy != TEXT("authored"))
    {
        return Fail(OutError, TEXT("multiple lod_levels require authored lod_policy"));
    }
    FString GeometryHash;
    if (!Root->TryGetStringField(TEXT("geometry_hash"), GeometryHash) || !IsHashString(GeometryHash))
    {
        return Fail(OutError, TEXT("geometry_hash must be xxh3 plus 16 lowercase hex digits"));
    }

    const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
    if (!Root->TryGetArrayField(TEXT("material_slots"), Slots))
    {
        return Fail(OutError, TEXT("material_slots must be an array"));
    }
    TArray<FMHFbxPassportSlot> ParsedSlots;
    TSet<FString> SlotNames;
    for (const TSharedPtr<FJsonValue>& SlotValue : *Slots)
    {
        if (!SlotValue.IsValid() || SlotValue->Type != EJson::Object)
        {
            return Fail(OutError, TEXT("material_slots entries must be objects"));
        }
        const TSharedPtr<FJsonObject> SlotObject = SlotValue->AsObject();
        FMHFbxPassportSlot Slot;
        if (SlotObject->Values.Num() != 3 ||
            !SlotObject->TryGetStringField(TEXT("slot_name"), Slot.SlotName) || Slot.SlotName.IsEmpty() ||
            !SlotObject->TryGetStringField(TEXT("material_uid"), Slot.MaterialUid) ||
            !MHIsCanonicalUuid(Slot.MaterialUid) ||
            !SlotObject->TryGetStringField(TEXT("material_name_hint"), Slot.MaterialNameHint))
        {
            return Fail(OutError, TEXT("material_slots entry has invalid fields"));
        }
        if (SlotNames.Contains(Slot.SlotName))
        {
            return Fail(OutError, FString::Printf(TEXT("duplicate material slot_name %s"), *Slot.SlotName));
        }
        if (ParsedSlots.Num() > 0 && ParsedSlots.Last().SlotName.Compare(Slot.SlotName, ESearchCase::CaseSensitive) > 0)
        {
            return Fail(OutError, TEXT("material_slots must be sorted by slot_name"));
        }
        SlotNames.Add(Slot.SlotName);
        ParsedSlots.Add(MoveTemp(Slot));
    }

    const TSharedPtr<FJsonObject>* Properties = nullptr;
    if (!Root->TryGetObjectField(TEXT("properties"), Properties))
    {
        return Fail(OutError, TEXT("properties must be an object"));
    }
    FString PropertiesJson;
    if (!SerializeCompactObject(*Properties, PropertiesJson))
    {
        return Fail(OutError, TEXT("properties cannot be serialized"));
    }
    FString Exporter;
    if (!Root->TryGetStringField(TEXT("exporter"), Exporter) || Exporter.IsEmpty())
    {
        return Fail(OutError, TEXT("exporter must be a non-empty string"));
    }

    OutPassport.ResourceUid = MoveTemp(ResourceUid);
    OutPassport.Name = MoveTemp(Name);
    OutPassport.LodPolicy = MoveTemp(LodPolicy);
    OutPassport.LodLevels = MoveTemp(Levels);
    OutPassport.GeometryHash = MoveTemp(GeometryHash);
    OutPassport.MaterialSlots = MoveTemp(ParsedSlots);
    OutPassport.PropertiesJson = MoveTemp(PropertiesJson);
    OutPassport.Exporter = MoveTemp(Exporter);
    OutPassport.CarrierText = CarrierText;
    return true;
}

// TODO(C1-parity): the writer additionally guarantees the carrier is the exact
// canonical one-line JSON spelling; byte-level re-serialization parity against
// the Python reference lands with the golden passport fixtures.

} // namespace

bool MHReadFbxPassport(
    const FString& FilePath,
    FMHFbxPassport& OutPassport,
    FString& OutError)
{
    OutPassport = FMHFbxPassport();
    OutError.Reset();

    FbxManager* Manager = FbxManager::Create();
    if (Manager == nullptr)
    {
        return Fail(OutError, TEXT("unable to create FBX manager"));
    }
    FbxIOSettings* IoSettings = FbxIOSettings::Create(Manager, IOSROOT);
    Manager->SetIOSettings(IoSettings);
    FbxImporter* Importer = FbxImporter::Create(Manager, "MHFbxPassportImporter");
    if (!Importer->Initialize(TCHAR_TO_UTF8(*FilePath), -1, IoSettings))
    {
        const FString Message = UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString());
        Manager->Destroy();
        return Fail(OutError, FString::Printf(TEXT("cannot open %s: %s"), *FilePath, *Message));
    }
    FbxScene* Scene = FbxScene::Create(Manager, "MHFbxPassportScene");
    if (Scene == nullptr || !Importer->Import(Scene))
    {
        const FString Message = Importer != nullptr
            ? FString(UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString()))
            : FString();
        Manager->Destroy();
        return Fail(OutError, FString::Printf(TEXT("cannot import %s: %s"), *FilePath, *Message));
    }
    Importer->Destroy();

    TArray<FString> CarrierTexts;
    const int32 NodeCount = Scene->GetNodeCount();
    for (int32 NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
    {
        FbxNode* Node = Scene->GetNode(NodeIndex);
        if (Node == nullptr || Node->GetMesh() == nullptr)
        {
            continue;
        }
        int32 CarriersOnNode = 0;
        FString CarrierText;
        for (FbxProperty Property = Node->GetFirstProperty(); Property.IsValid();
             Property = Node->GetNextProperty(Property))
        {
            if (FString(UTF8_TO_TCHAR(Property.GetNameAsCStr())) != TEXT("mh_fbx_passport"))
            {
                continue;
            }
            ++CarriersOnNode;
            if (Property.GetPropertyDataType().GetType() != eFbxString)
            {
                Manager->Destroy();
                return Fail(OutError, FString::Printf(
                    TEXT("MESH Model %s has a non-string passport property"),
                    UTF8_TO_TCHAR(Node->GetName())));
            }
            CarrierText = UTF8_TO_TCHAR(Property.Get<FbxString>().Buffer());
        }
        if (CarriersOnNode != 1)
        {
            const FString NodeName = UTF8_TO_TCHAR(Node->GetName());
            Manager->Destroy();
            return Fail(OutError, FString::Printf(
                TEXT("MESH Model %s must contain exactly one mh_fbx_passport; found %d"),
                *NodeName,
                CarriersOnNode));
        }
        CarrierTexts.Add(MoveTemp(CarrierText));
    }
    Manager->Destroy();

    if (CarrierTexts.Num() == 0)
    {
        return Fail(OutError, TEXT("FBX has no MESH Models"));
    }
    for (int32 Index = 1; Index < CarrierTexts.Num(); ++Index)
    {
        if (!CarrierTexts[Index].Equals(CarrierTexts[0], ESearchCase::CaseSensitive))
        {
            return Fail(OutError, TEXT("Carrier B copies differ byte-for-byte across MESH Models"));
        }
    }

    if (!ValidatePassportDocument(CarrierTexts[0], OutPassport, OutError))
    {
        return false;
    }
    OutPassport.CopyCount = CarrierTexts.Num();
    return true;
}

} // namespace UE::MimirComposite
