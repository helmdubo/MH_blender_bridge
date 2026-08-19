#include "Ledger/MHImportLedger.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHImportLedger)

namespace
{

const TCHAR* const LedgerSnapshotSchema = TEXT("mh.import_ledger");
constexpr int32 LedgerSnapshotVersion = 1;
const TCHAR* const LedgerObjectName = TEXT("Ledger");

FString LedgerNormalizeContentRoot(const FString& ContentRoot)
{
    FString Root = ContentRoot;
    Root.TrimStartAndEndInline();
    if (Root.IsEmpty())
    {
        Root = TEXT("/Game/MH");
    }
    while (Root.Len() > 1 && Root.EndsWith(TEXT("/")))
    {
        Root.LeftChopInline(1);
    }
    return Root;
}

FString LedgerFieldString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    FString Value;
    return Object->TryGetStringField(Field, Value) ? Value : FString();
}

} // namespace

const TCHAR* MHResourceKindLabel(const EMHResourceKind Kind)
{
    switch (Kind)
    {
    case EMHResourceKind::StaticMesh: return TEXT("static_mesh");
    case EMHResourceKind::Material: return TEXT("material");
    default: return TEXT("composite");
    }
}

bool MHResourceKindFromLabel(const FString& Label, EMHResourceKind& OutKind)
{
    if (Label == TEXT("static_mesh"))
    {
        OutKind = EMHResourceKind::StaticMesh;
        return true;
    }
    if (Label == TEXT("material"))
    {
        OutKind = EMHResourceKind::Material;
        return true;
    }
    if (Label == TEXT("composite"))
    {
        OutKind = EMHResourceKind::Composite;
        return true;
    }
    return false;
}

UMHImportLedger* UMHImportLedger::LoadOrCreate(const FString& ContentRoot)
{
#if WITH_EDITOR
    const FString PackageName = LedgerNormalizeContentRoot(ContentRoot) + TEXT("/_MH/Ledger");
    if (!FPackageName::IsValidLongPackageName(PackageName))
    {
        return nullptr;
    }

    UPackage* Package = nullptr;
    if (FPackageName::DoesPackageExist(PackageName))
    {
        Package = LoadPackage(nullptr, *PackageName, LOAD_NoWarn);
        if (Package != nullptr)
        {
            if (UMHImportLedger* Existing = FindObject<UMHImportLedger>(Package, LedgerObjectName))
            {
                return Existing;
            }
        }
    }
    if (Package == nullptr)
    {
        Package = CreatePackage(*PackageName);
    }
    if (Package == nullptr)
    {
        return nullptr;
    }
    Package->FullyLoad();

    return NewObject<UMHImportLedger>(
        Package,
        UMHImportLedger::StaticClass(),
        LedgerObjectName,
        RF_Public | RF_Standalone);
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
    SaveArgs.SaveFlags = SAVE_None;
    return UPackage::SavePackage(Package, this, *FileName, SaveArgs);
#else
    return false;
#endif
}

bool MHLedgerSnapshotToJson(const TMap<FString, FMHLedgerRow>& Rows, FString& OutJson)
{
    OutJson.Reset();

    TArray<FString> Uids;
    Rows.GenerateKeyArray(Uids);
    Uids.Sort();

    const TSharedPtr<FJsonObject> RowsObject = MakeShared<FJsonObject>();
    for (const FString& Uid : Uids)
    {
        const FMHLedgerRow& Row = Rows[Uid];
        const TSharedPtr<FJsonObject> RowObject = MakeShared<FJsonObject>();
        RowObject->SetStringField(TEXT("kind"), MHResourceKindLabel(Row.Kind));
        RowObject->SetStringField(TEXT("asset"), Row.Asset.ToString());
        RowObject->SetStringField(TEXT("source_path"), Row.SourcePath);
        RowObject->SetStringField(TEXT("applied_geometry_hash"), Row.AppliedGeometryHash);
        RowObject->SetStringField(TEXT("applied_descriptor_hash"), Row.AppliedDescriptorHash);
        RowObject->SetStringField(TEXT("payload_fingerprint"), Row.PayloadFingerprint);
        RowObject->SetStringField(TEXT("imported_at"), Row.ImportedAt.ToIso8601());
        RowObject->SetStringField(TEXT("import_status"), Row.ImportStatus);
        RowsObject->SetObjectField(Uid, RowObject);
    }

    const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), LedgerSnapshotSchema);
    Root->SetNumberField(TEXT("schema_version"), static_cast<double>(LedgerSnapshotVersion));
    Root->SetObjectField(TEXT("rows"), RowsObject);

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
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: ledger snapshot is not valid JSON");
        return false;
    }

    FString Schema;
    if (!Root->TryGetStringField(TEXT("schema"), Schema) || Schema != LedgerSnapshotSchema)
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: ledger snapshot schema must be mh.import_ledger");
        return false;
    }
    double Version = 0.0;
    if (!Root->TryGetNumberField(TEXT("schema_version"), Version) ||
        Version != static_cast<double>(LedgerSnapshotVersion))
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: unsupported ledger snapshot version");
        return false;
    }

    const TSharedPtr<FJsonObject>* RowsObject = nullptr;
    if (!Root->TryGetObjectField(TEXT("rows"), RowsObject) || RowsObject == nullptr)
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: ledger snapshot has no rows object");
        return false;
    }

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*RowsObject)->Values)
    {
        if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s is not an object"),
                *Pair.Key);
            return false;
        }
        const TSharedPtr<FJsonObject> RowObject = Pair.Value->AsObject();

        FMHLedgerRow Row;
        const FString KindLabel = LedgerFieldString(RowObject, TEXT("kind"));
        if (!MHResourceKindFromLabel(KindLabel, Row.Kind))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s has unknown kind %s"),
                *Pair.Key,
                *KindLabel);
            return false;
        }
        const FString AssetPath = LedgerFieldString(RowObject, TEXT("asset"));
        if (!AssetPath.IsEmpty())
        {
            Row.Asset = FSoftObjectPath(*AssetPath);
        }
        Row.SourcePath = LedgerFieldString(RowObject, TEXT("source_path"));
        Row.AppliedGeometryHash = LedgerFieldString(RowObject, TEXT("applied_geometry_hash"));
        Row.AppliedDescriptorHash = LedgerFieldString(RowObject, TEXT("applied_descriptor_hash"));
        Row.PayloadFingerprint = LedgerFieldString(RowObject, TEXT("payload_fingerprint"));
        Row.ImportStatus = LedgerFieldString(RowObject, TEXT("import_status"));

        const FString ImportedAt = LedgerFieldString(RowObject, TEXT("imported_at"));
        if (!ImportedAt.IsEmpty() && !FDateTime::ParseIso8601(*ImportedAt, Row.ImportedAt))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s has an invalid imported_at"),
                *Pair.Key);
            return false;
        }
        OutRows.Add(Pair.Key, MoveTemp(Row));
    }
    return true;
}
