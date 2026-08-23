#include "Ledger/MHImportLedger.h"

#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceResolver.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHImportLedger)

namespace
{

const TCHAR* const LedgerObjectName = TEXT("Ledger");

FString LedgerPackageName(FString ContentRoot)
{
    ContentRoot.TrimStartAndEndInline();
    if (ContentRoot.IsEmpty())
    {
        ContentRoot = TEXT("/Game/MH");
    }
    while (ContentRoot.Len() > 1 && ContentRoot.EndsWith(TEXT("/")))
    {
        ContentRoot.LeftChopInline(1);
    }
    return ContentRoot + TEXT("/_MH/Ledger");
}

bool ParseKey(
    const FString& Serialized,
    UE::MimirComposite::FMHResourceKey& OutKey)
{
    FString KindLabel;
    if (!Serialized.Split(TEXT(":"), &KindLabel, &OutKey.LogicalName) ||
        !UE::MimirComposite::MHResourceKindFromLabel(KindLabel, OutKey.Kind))
    {
        return false;
    }
    return OutKey.IsCanonical() && OutKey.ToString() == Serialized;
}

bool ValidateRow(const FString& SerializedKey, const FMHLedgerRow& Row)
{
    if (Row.SourcePath.IsEmpty() || !FPaths::IsRelative(Row.SourcePath) ||
        Row.SourcePath.Contains(TEXT("\\")) || Row.SourcePath.StartsWith(TEXT("/")) ||
        Row.SourcePath.EndsWith(TEXT("/")) || Row.SourcePath.Contains(TEXT("//")))
    {
        return false;
    }

    TArray<FString> Segments;
    Row.SourcePath.ParseIntoArray(Segments, TEXT("/"), false);
    if (Segments.IsEmpty() || Segments.ContainsByPredicate([](const FString& Segment)
        {
            return Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT("..");
        }))
    {
        return false;
    }

    UE::MimirComposite::FMHResourceKey Key;
    const FString AssetPath = Row.Asset.ToString();
    return ParseKey(SerializedKey, Key) &&
        Key.Kind == Row.Kind && Key.LogicalName == Row.LogicalName &&
        (AssetPath.IsEmpty() ||
            (Row.Asset.IsValid() && FPackageName::IsValidObjectPath(AssetPath))) &&
        UE::MimirComposite::MHIsCanonicalRawPayloadHash(Row.AppliedRawHash) &&
        Row.ImportedAt.GetTicks() > 0 && !Row.ImportStatus.IsEmpty();
}

} // namespace

UMHImportLedger* UMHImportLedger::LoadOrCreate(const FString& ContentRoot)
{
#if WITH_EDITOR
    if (UMHImportLedger* Existing = LoadExisting(ContentRoot))
    {
        return Existing;
    }
    const FString PackageName = LedgerPackageName(ContentRoot);
    if (!FPackageName::IsValidLongPackageName(PackageName))
    {
        return nullptr;
    }
    UPackage* Package = CreatePackage(*PackageName);
    return Package != nullptr
        ? NewObject<UMHImportLedger>(
            Package,
            UMHImportLedger::StaticClass(),
            LedgerObjectName,
            RF_Public | RF_Standalone)
        : nullptr;
#else
    return nullptr;
#endif
}

UMHImportLedger* UMHImportLedger::LoadExisting(const FString& ContentRoot)
{
#if WITH_EDITOR
    const FString PackageName = LedgerPackageName(ContentRoot);
    if (!FPackageName::IsValidLongPackageName(PackageName))
    {
        return nullptr;
    }
    UPackage* Package = FindPackage(nullptr, *PackageName);
    if (Package == nullptr && FPackageName::DoesPackageExist(PackageName))
    {
        Package = LoadPackage(nullptr, *PackageName, LOAD_NoWarn);
    }
    return Package != nullptr ? FindObject<UMHImportLedger>(Package, LedgerObjectName) : nullptr;
#else
    return nullptr;
#endif
}

bool UMHImportLedger::Save()
{
#if WITH_EDITOR
    UPackage* Package = GetPackage();
    if (Package == nullptr)
    {
        return false;
    }
    Package->MarkPackageDirty();
    const FString FileName = FPackageName::LongPackageNameToFilename(
        Package->GetName(),
        FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    return UPackage::SavePackage(Package, this, *FileName, SaveArgs);
#else
    return false;
#endif
}

bool MHLedgerSnapshotToJson(const TMap<FString, FMHLedgerRow>& Rows, FString& OutJson)
{
    OutJson.Reset();
    TArray<FString> Keys;
    Rows.GenerateKeyArray(Keys);
    Keys.Sort();

    TArray<TSharedPtr<FJsonValue>> JsonRows;
    for (const FString& Key : Keys)
    {
        const FMHLedgerRow& Row = Rows.FindChecked(Key);
        if (!ValidateRow(Key, Row))
        {
            return false;
        }
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("resource_key"), Key);
        Object->SetStringField(TEXT("kind"), UE::MimirComposite::MHResourceKindLabel(Row.Kind));
        Object->SetStringField(TEXT("logical_name"), Row.LogicalName);
        Object->SetStringField(TEXT("asset"), Row.Asset.ToString());
        Object->SetStringField(TEXT("source_path"), Row.SourcePath);
        Object->SetStringField(TEXT("applied_raw_hash"), Row.AppliedRawHash);
        Object->SetStringField(TEXT("imported_at"), Row.ImportedAt.ToIso8601());
        Object->SetStringField(TEXT("import_status"), Row.ImportStatus);
        JsonRows.Add(MakeShared<FJsonValueObject>(Object));
    }

    const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("tag"), TEXT("mh.legacy_ledger:1"));
    Root->SetArrayField(TEXT("rows"), JsonRows);
    const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
    return FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
}

bool MHLedgerSnapshotFromJson(
    const FString& Json,
    TMap<FString, FMHLedgerRow>& OutRows,
    FString& OutError)
{
    OutRows.Reset();
    OutError.Reset();
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: legacy ledger snapshot is not valid JSON");
        return false;
    }
    FString Tag;
    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (!Root->TryGetStringField(TEXT("tag"), Tag) || Tag != TEXT("mh.legacy_ledger:1") ||
        !Root->TryGetArrayField(TEXT("rows"), Rows) || Rows == nullptr)
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: unsupported legacy ledger snapshot");
        return false;
    }

    for (const TSharedPtr<FJsonValue>& Value : *Rows)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: legacy ledger row must be an object");
            OutRows.Reset();
            return false;
        }
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        FString Key;
        FString KindLabel;
        FString Asset;
        FMHLedgerRow Row;
        FString ImportedAt;
        if (!Object->TryGetStringField(TEXT("resource_key"), Key) ||
            !Object->TryGetStringField(TEXT("kind"), KindLabel) ||
            !UE::MimirComposite::MHResourceKindFromLabel(KindLabel, Row.Kind) ||
            !Object->TryGetStringField(TEXT("logical_name"), Row.LogicalName) ||
            !Object->TryGetStringField(TEXT("asset"), Asset) ||
            !Object->TryGetStringField(TEXT("source_path"), Row.SourcePath) ||
            !Object->TryGetStringField(TEXT("applied_raw_hash"), Row.AppliedRawHash) ||
            !Object->TryGetStringField(TEXT("imported_at"), ImportedAt) ||
            !FDateTime::ParseIso8601(*ImportedAt, Row.ImportedAt) ||
            !Object->TryGetStringField(TEXT("import_status"), Row.ImportStatus))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: invalid or duplicate legacy ledger row");
            OutRows.Reset();
            return false;
        }
        if (!Asset.IsEmpty())
        {
            if (!FPackageName::IsValidObjectPath(Asset))
            {
                OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: invalid or duplicate legacy ledger row");
                OutRows.Reset();
                return false;
            }
            Row.Asset = FSoftObjectPath(Asset);
        }
        if (OutRows.Contains(Key) || !ValidateRow(Key, Row))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: invalid or duplicate legacy ledger row");
            OutRows.Reset();
            return false;
        }
        OutRows.Add(Key, MoveTemp(Row));
    }
    return true;
}
