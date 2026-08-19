#include "Source/MHPayloadHashes.h"

#include "Canonical/MHCanonical.h"
#include "Codec/MHCompositeCodec.h"
#include "Codec/MHMaterialCodec.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::MimirComposite
{
namespace
{

TArray<uint8> PayloadHashesUtf8(const FString& Text)
{
    FTCHARToUTF8 Utf8(*Text, Text.Len());
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return Bytes;
}

/** Integer literal the canonical serializer accepts verbatim. */
TSharedPtr<FJsonValue> PayloadHashesInteger(const int64 Value)
{
    return MakeShared<FJsonValueNumberString>(LexToString(Value));
}

bool PayloadHashesFromCanonical(
    const TSharedPtr<FJsonValue>& Canonical,
    FString& OutHash,
    FString& OutError)
{
    TArray<uint8> CanonicalBytes;
    const FMHCanonicalResult Result = MHCanonicalJsonBytes(Canonical, CanonicalBytes);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }
    OutHash = MHXxh3Hash(CanonicalBytes);
    return true;
}

} // namespace

bool MHCompositeSemanticHash(
    const TConstArrayView<uint8> Bytes,
    FString& OutHash,
    FString& OutError)
{
    OutHash.Reset();
    OutError.Reset();

    FMHCompositeDocument Document;
    FMHCanonicalResult Result = MHParseCompositeV2(Bytes, Document);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }

    TSharedPtr<FJsonValue> Root;
    Result = MHParseJsonUtf8(Bytes, Root);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }
    TSharedPtr<FJsonValue> Canonical;
    Result = MHCompositeCanonicalForm(Root, Canonical);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }
    return PayloadHashesFromCanonical(Canonical, OutHash, OutError);
}

bool MHMaterialSemanticHash(
    const TConstArrayView<uint8> Bytes,
    FString& OutHash,
    FString& OutError)
{
    OutHash.Reset();
    OutError.Reset();

    FMHMaterialDocument Document;
    FMHCanonicalResult Result = MHParseMaterialV1(Bytes, Document);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }

    TSharedPtr<FJsonValue> Root;
    Result = MHParseJsonUtf8(Bytes, Root);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }
    if (!Root.IsValid() || Root->Type != EJson::Object)
    {
        OutError = TEXT("MH_E_INVALID_MATERIAL_VALUE: document must be an object");
        return false;
    }

    const TSharedPtr<FJsonObject> Object = Root->AsObject();
    TSharedPtr<FJsonValue> Disk;
    TSharedPtr<FJsonValue> Canonical;
    Result = MHMaterialForms(
        Document.ShaderClass,
        Object->TryGetField(TEXT("params")),
        Object->TryGetField(TEXT("textures")),
        Disk,
        Canonical);
    if (!Result.bSuccess)
    {
        OutError = Result.Error;
        return false;
    }
    return PayloadHashesFromCanonical(Canonical, OutHash, OutError);
}

FString MHPassportDescriptorHash(const FMHFbxPassport& Passport)
{
    const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("mh.fbx_passport"));
    Root->SetField(TEXT("schema_version"), PayloadHashesInteger(1));
    Root->SetStringField(TEXT("resource_uid"), Passport.ResourceUid);
    Root->SetStringField(TEXT("kind"), TEXT("static_mesh"));
    Root->SetStringField(TEXT("name"), Passport.Name);
    Root->SetStringField(TEXT("lod_policy"), Passport.LodPolicy);

    TArray<TSharedPtr<FJsonValue>> Levels;
    for (const int32 Level : Passport.LodLevels)
    {
        Levels.Add(PayloadHashesInteger(Level));
    }
    Root->SetArrayField(TEXT("lod_levels"), Levels);

    TArray<TSharedPtr<FJsonValue>> Slots;
    for (const FMHFbxPassportSlot& Slot : Passport.MaterialSlots)
    {
        const TSharedPtr<FJsonObject> SlotObject = MakeShared<FJsonObject>();
        SlotObject->SetStringField(TEXT("slot_name"), Slot.SlotName);
        SlotObject->SetStringField(TEXT("material_uid"), Slot.MaterialUid);
        SlotObject->SetStringField(TEXT("material_name_hint"), Slot.MaterialNameHint);
        Slots.Add(MakeShared<FJsonValueObject>(SlotObject));
    }
    Root->SetArrayField(TEXT("material_slots"), Slots);

    const TArray<uint8> PropertiesBytes = PayloadHashesUtf8(
        Passport.PropertiesJson.IsEmpty() ? TEXT("{}") : Passport.PropertiesJson);
    TSharedPtr<FJsonValue> Properties;
    if (!MHParseJsonUtf8(PropertiesBytes, Properties).bSuccess || !Properties.IsValid())
    {
        return FString();
    }
    Root->SetField(TEXT("properties"), Properties);
    Root->SetStringField(TEXT("exporter"), Passport.Exporter);
    // geometry_hash is deliberately absent: docs/05 section 4.3 keeps the two
    // hashes independent so a metadata-only edit never reads as UPDATE_GEOMETRY.

    // MHCompositeCanonicalForm is the exported generic entry point of the frozen
    // canonical library (NFC keys and strings, quantized numbers, no "nodes"
    // array here); reusing it keeps a second canonicalizer out of this module.
    const TSharedPtr<FJsonValue> Document = MakeShared<FJsonValueObject>(Root);
    TSharedPtr<FJsonValue> Canonical;
    if (!MHCompositeCanonicalForm(Document, Canonical).bSuccess)
    {
        return FString();
    }
    FString Hash;
    FString Error;
    if (!PayloadHashesFromCanonical(Canonical, Hash, Error))
    {
        return FString();
    }
    return Hash;
}

FString MHPayloadFingerprint(const TConstArrayView<uint8> Bytes)
{
    return MHXxh3Hash(Bytes);
}

} // namespace UE::MimirComposite
