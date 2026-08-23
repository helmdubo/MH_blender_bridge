#include "Ledger/MHImportLedger.h"

#include "Codec/MHCompositeCodec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
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

FString LedgerPackageName(const FString& ContentRoot)
{
    return LedgerNormalizeContentRoot(ContentRoot) + TEXT("/_MH/Ledger");
}

template <int32 FieldCount>
bool LedgerHasExactFields(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* const (&Expected)[FieldCount],
    FString& OutDetail)
{
    if (!Object.IsValid() || Object->Values.Num() != FieldCount)
    {
        OutDetail = TEXT("field count differs from schema");
        return false;
    }
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
    {
        bool bExpected = false;
        for (const TCHAR* Field : Expected)
        {
            bExpected |= Pair.Key.Equals(Field, ESearchCase::CaseSensitive);
        }
        if (!bExpected)
        {
            OutDetail = FString::Printf(TEXT("unexpected or mis-cased field %s"), *Pair.Key);
            return false;
        }
    }
    return true;
}

bool LedgerTryRequiredString(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* Field,
    FString& OutValue)
{
    return Object.IsValid() && Object->TryGetStringField(Field, OutValue);
}

bool LedgerIsCanonicalSourcePath(const FString& Path)
{
    if (Path.IsEmpty() || !FPaths::IsRelative(Path) || Path.Contains(TEXT("\\")) ||
        Path.StartsWith(TEXT("/")) || Path.EndsWith(TEXT("/")) ||
        Path.Contains(TEXT("//")))
    {
        return false;
    }

    TArray<FString> Segments;
    Path.ParseIntoArray(Segments, TEXT("/"), false);
    for (const FString& Segment : Segments)
    {
        if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT(".."))
        {
            return false;
        }
    }
    return !Segments.IsEmpty();
}

bool LedgerHasLowerHexFormat(
    const FString& Value,
    const TCHAR* Prefix,
    const int32 HexDigitCount)
{
    const int32 PrefixLength = FCString::Strlen(Prefix);
    if (Value.Len() != PrefixLength + HexDigitCount ||
        !Value.StartsWith(Prefix, ESearchCase::CaseSensitive))
    {
        return false;
    }
    for (int32 Index = PrefixLength; Index < Value.Len(); ++Index)
    {
        const TCHAR Char = Value[Index];
        if (!((Char >= TEXT('0') && Char <= TEXT('9')) ||
              (Char >= TEXT('a') && Char <= TEXT('f'))))
        {
            return false;
        }
    }
    return true;
}

bool LedgerValidateRow(
    const FString& Uid,
    const FMHLedgerRow& Row,
    FString& OutDetail)
{
    if (!UE::MimirComposite::MHIsCanonicalUuid(Uid) ||
        !LedgerIsCanonicalSourcePath(Row.SourcePath) ||
        (!Row.Asset.ToString().IsEmpty() && !Row.Asset.IsValid()) ||
        !LedgerHasLowerHexFormat(Row.PayloadFingerprint, TEXT("sha256:"), 64) ||
        Row.ImportedAt.GetTicks() <= 0 || Row.ImportStatus.IsEmpty())
    {
        OutDetail = TEXT("has invalid identity, path, fingerprint, timestamp, or status");
        return false;
    }

    if (Row.Kind == EMHResourceKind::StaticMesh)
    {
        if (!LedgerHasLowerHexFormat(Row.AppliedGeometryHash, TEXT("xxh3:"), 16) ||
            !LedgerHasLowerHexFormat(Row.AppliedDescriptorHash, TEXT("sha256:"), 64))
        {
            OutDetail = TEXT("has invalid static-mesh applied hashes");
            return false;
        }
    }
    else if (Row.Kind == EMHResourceKind::Material || Row.Kind == EMHResourceKind::Composite)
    {
        if (!Row.AppliedGeometryHash.IsEmpty() ||
            !LedgerHasLowerHexFormat(Row.AppliedDescriptorHash, TEXT("xxh3:"), 16))
        {
            OutDetail = TEXT("has invalid JSON-payload applied hashes");
            return false;
        }
    }
    else
    {
        OutDetail = TEXT("has an invalid resource kind");
        return false;
    }
    return true;
}

bool LedgerRejectDuplicateObjectKeys(const FString& Json, FString& OutError)
{
    struct FScope
    {
        bool bObject = false;
        TSet<FString> FoldedKeys;
    };

    TArray<FScope> Scopes;
    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
    EJsonNotation Notation = EJsonNotation::Error;
    while (Reader->ReadNext(Notation))
    {
        if (Notation == EJsonNotation::ObjectEnd || Notation == EJsonNotation::ArrayEnd)
        {
            const bool bExpectedObject = Notation == EJsonNotation::ObjectEnd;
            if (Scopes.IsEmpty() || Scopes.Last().bObject != bExpectedObject)
            {
                OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: ledger snapshot has invalid JSON nesting");
                return false;
            }
            Scopes.Pop();
            continue;
        }

        if (!Scopes.IsEmpty() && Scopes.Last().bObject)
        {
            const FString FoldedKey = Reader->GetIdentifier().ToLower();
            if (Scopes.Last().FoldedKeys.Contains(FoldedKey))
            {
                OutError = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_INVALID: ledger snapshot has duplicate or case-aliased key %s"),
                    *Reader->GetIdentifier());
                return false;
            }
            Scopes.Last().FoldedKeys.Add(FoldedKey);
        }

        if (Notation == EJsonNotation::ObjectStart || Notation == EJsonNotation::ArrayStart)
        {
            FScope& Scope = Scopes.AddDefaulted_GetRef();
            Scope.bObject = Notation == EJsonNotation::ObjectStart;
        }
    }

    if (!Reader->GetErrorMessage().IsEmpty() || !Scopes.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: ledger snapshot is not valid JSON");
        return false;
    }
    return true;
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

UMHImportLedger* UMHImportLedger::LoadExisting(const FString& ContentRoot)
{
#if WITH_EDITOR
    const FString PackageName = LedgerPackageName(ContentRoot);
    if (!FPackageName::IsValidLongPackageName(PackageName) ||
        PackageName.IsEmpty())
    {
        return nullptr;
    }

    UPackage* Package = FindPackage(nullptr, *PackageName);
    if (Package == nullptr && FPackageName::DoesPackageExist(PackageName))
    {
        Package = LoadPackage(nullptr, *PackageName, LOAD_NoWarn);
    }
    return Package != nullptr
        ? FindObject<UMHImportLedger>(Package, LedgerObjectName)
        : nullptr;
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
        FString RowError;
        if (!LedgerValidateRow(Uid, Row, RowError))
        {
            OutJson.Reset();
            return false;
        }
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

    if (!LedgerRejectDuplicateObjectKeys(Json, OutError))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: ledger snapshot is not valid JSON");
        return false;
    }

    static const TCHAR* const RootFields[] = {
        TEXT("schema"), TEXT("schema_version"), TEXT("rows")};
    FString FieldError;
    if (!LedgerHasExactFields(Root, RootFields, FieldError))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: ledger snapshot %s"),
            *FieldError);
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

    TMap<FString, FMHLedgerRow> ParsedRows;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*RowsObject)->Values)
    {
        if (!UE::MimirComposite::MHIsCanonicalUuid(Pair.Key))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row key %s is not a canonical UUID"),
                *Pair.Key);
            return false;
        }
        if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s is not an object"),
                *Pair.Key);
            return false;
        }
        const TSharedPtr<FJsonObject> RowObject = Pair.Value->AsObject();

        static const TCHAR* const RowFields[] = {
            TEXT("kind"), TEXT("asset"), TEXT("source_path"),
            TEXT("applied_geometry_hash"), TEXT("applied_descriptor_hash"),
            TEXT("payload_fingerprint"), TEXT("imported_at"), TEXT("import_status")};
        if (!LedgerHasExactFields(RowObject, RowFields, FieldError))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s %s"),
                *Pair.Key,
                *FieldError);
            return false;
        }

        FString KindLabel;
        FString AssetPath;
        FString SourcePath;
        FString GeometryHash;
        FString DescriptorHash;
        FString PayloadFingerprint;
        FString ImportedAt;
        FString ImportStatus;
        if (!LedgerTryRequiredString(RowObject, TEXT("kind"), KindLabel) ||
            !LedgerTryRequiredString(RowObject, TEXT("asset"), AssetPath) ||
            !LedgerTryRequiredString(RowObject, TEXT("source_path"), SourcePath) ||
            !LedgerTryRequiredString(RowObject, TEXT("applied_geometry_hash"), GeometryHash) ||
            !LedgerTryRequiredString(RowObject, TEXT("applied_descriptor_hash"), DescriptorHash) ||
            !LedgerTryRequiredString(RowObject, TEXT("payload_fingerprint"), PayloadFingerprint) ||
            !LedgerTryRequiredString(RowObject, TEXT("imported_at"), ImportedAt) ||
            !LedgerTryRequiredString(RowObject, TEXT("import_status"), ImportStatus))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s has a non-string field"),
                *Pair.Key);
            return false;
        }

        FMHLedgerRow Row;
        if (!MHResourceKindFromLabel(KindLabel, Row.Kind))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s has unknown kind %s"),
                *Pair.Key,
                *KindLabel);
            return false;
        }
        if (!AssetPath.IsEmpty())
        {
            Row.Asset = FSoftObjectPath(*AssetPath);
            if (!Row.Asset.IsValid())
            {
                OutError = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s has an invalid asset path"),
                    *Pair.Key);
                return false;
            }
        }
        if (ImportedAt.IsEmpty() || !FDateTime::ParseIso8601(*ImportedAt, Row.ImportedAt))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s has an invalid imported_at"),
                *Pair.Key);
            return false;
        }
        Row.SourcePath = MoveTemp(SourcePath);
        Row.AppliedGeometryHash = MoveTemp(GeometryHash);
        Row.AppliedDescriptorHash = MoveTemp(DescriptorHash);
        Row.PayloadFingerprint = MoveTemp(PayloadFingerprint);
        Row.ImportStatus = MoveTemp(ImportStatus);
        FString RowError;
        if (!LedgerValidateRow(Pair.Key, Row, RowError))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: ledger row %s %s"),
                *Pair.Key,
                *RowError);
            return false;
        }
        ParsedRows.Add(Pair.Key, MoveTemp(Row));
    }
    OutRows = MoveTemp(ParsedRows);
    return true;
}
