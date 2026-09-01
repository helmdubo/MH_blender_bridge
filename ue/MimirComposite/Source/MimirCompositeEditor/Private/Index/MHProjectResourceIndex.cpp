#include "Index/MHProjectResourceIndex.h"

#include "Composite/MHCompositeProtocol.h"
#include "Geometry/MHFbxSceneTranslator.h"
#include "Geometry/MHSceneIR.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Material/MHMaterialProtocol.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Performance/MHPerformanceTrace.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceAnalyzer.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "String/LexFromString.h"

namespace UE::MimirComposite
{
namespace
{

constexpr const TCHAR* ProjectIndexTag = TEXT("mh.project_index:4");

enum class ECandidateParseStatus : uint8
{
    Ok,
    Noncanonical,
    Unreadable,
    InvalidPayload
};

const TCHAR* CandidateStatusLabel(const ECandidateParseStatus Status)
{
    switch (Status)
    {
    case ECandidateParseStatus::Ok: return TEXT("ok");
    case ECandidateParseStatus::Noncanonical: return TEXT("noncanonical");
    case ECandidateParseStatus::Unreadable: return TEXT("unreadable");
    case ECandidateParseStatus::InvalidPayload: return TEXT("invalid_payload");
    }
    return TEXT("invalid_payload");
}

struct FIndexedDependency
{
    FMHResourceKey Owner;
    FMHResourceKey Target;
    FString Role;
    FString OwnerPath;
};

struct FScannedCandidate
{
    FString RelativePath;
    bool bHasKey = false;
    FMHResourceKey Key;
    int64 Size = INDEX_NONE;
    int64 MTimeTicks = 0;
    FString RawHash;
    ECandidateParseStatus ParseStatus = ECandidateParseStatus::Unreadable;
    FString Diagnostic;
    TArray<FIndexedDependency> Dependencies;
};

struct FGeneratedRow
{
    FString ObjectPath;
    FString Kind;
    FString Name;
    FString SourcePath;
    FString SourceHash;
    FString AppliedHash;
    FString CarrierKind;
    bool bKeyValid = false;
    bool bReceiptValid = false;
    FString Diagnostic;
};

struct FSelfPublishToken
{
    FString AbsolutePath;
    FString RawHash;
    int64 Generation = 0;
};

struct FCandidateKeyTransitions
{
    TSet<FMHResourceKey> Disappeared;
    TSet<FMHResourceKey> Appeared;
};

bool ResourceKeyLess(const FMHResourceKey& A, const FMHResourceKey& B)
{
    if (A.Kind != B.Kind)
    {
        return static_cast<uint8>(A.Kind) < static_cast<uint8>(B.Kind);
    }
    return A.LogicalName < B.LogicalName;
}

FString ExtractDiagnosticCode(const FString& Diagnostic)
{
    FString Code;
    FString Unused;
    if (Diagnostic.Split(TEXT(":"), &Code, &Unused) && !Code.IsEmpty())
    {
        return Code;
    }
    return Diagnostic.IsEmpty() ? TEXT("MH_E_SOURCE_INDEX_INVALID") : Diagnostic;
}

bool IsValidRelativeSourcePath(const FString& Path)
{
    if (Path.IsEmpty() || !FPaths::IsRelative(Path) || Path.Contains(TEXT("\\")) ||
        Path.StartsWith(TEXT("/")) || Path.EndsWith(TEXT("/")) || Path.Contains(TEXT("//")))
    {
        return false;
    }
    TArray<FString> Segments;
    Path.ParseIntoArray(Segments, TEXT("/"), false);
    return !Segments.IsEmpty() && !Segments.ContainsByPredicate([](const FString& Segment)
    {
        return Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT("..");
    });
}

bool IsBinaryKind(const EMHResourceKind Kind)
{
    return Kind == EMHResourceKind::Texture || Kind == EMHResourceKind::StaticMesh;
}

FString ExpectedGeneratedObjectPath(
    const EMHResourceKind Kind,
    const FString& LogicalName)
{
    const TCHAR* Folder = nullptr;
    switch (Kind)
    {
    case EMHResourceKind::StaticMesh: Folder = TEXT("Meshes"); break;
    case EMHResourceKind::Material: Folder = TEXT("Materials"); break;
    case EMHResourceKind::Composite: Folder = TEXT("Composites"); break;
    case EMHResourceKind::PlacementProfile: return FString();
    case EMHResourceKind::Texture: Folder = TEXT("Textures"); break;
    }
    return FString::Printf(
        TEXT("/Game/MH/Generated/%s/%s.%s"),
        Folder,
        *LogicalName,
        *LogicalName);
}

bool ValidateNoNestedFilesystemAlias(
    const FString& SourceRoot,
    const FString& AbsolutePath,
    FString& OutError)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    FString Component = AbsolutePath;
    while (!Component.Equals(SourceRoot, ESearchCase::IgnoreCase))
    {
        if (PlatformFile.IsSymlink(*Component) != ESymlinkResult::NonSymlink)
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: nested filesystem alias is forbidden or unavailable: %s"),
                *Component);
            return false;
        }
        FString Parent = FPaths::GetPath(Component);
        FPaths::NormalizeDirectoryName(Parent);
        if (Parent.IsEmpty() || Parent.Equals(Component, ESearchCase::IgnoreCase))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: invalid source path parent chain");
            return false;
        }
        Component = MoveTemp(Parent);
    }
    return true;
}

void CollectCompositeDependencies(
    const TArray<FMHCompositeNode>& Nodes,
    const FMHResourceKey& Owner,
    const FString& OwnerPath,
    TArray<FIndexedDependency>& OutDependencies)
{
    for (const FMHCompositeNode& Node : Nodes)
    {
        FIndexedDependency Dependency;
        Dependency.Owner = Owner;
        Dependency.OwnerPath = OwnerPath;
        if (!Node.Profile.IsEmpty())
        {
            Dependency.Target.Kind = EMHResourceKind::PlacementProfile;
            Dependency.Target.LogicalName = Node.Profile;
            Dependency.Role = TEXT("profile");
            OutDependencies.Add(Dependency);
        }
        if (Node.Kind == EMHCompositeNodeKind::Mesh)
        {
            Dependency.Target.Kind = EMHResourceKind::StaticMesh;
            Dependency.Target.LogicalName = Node.Resource;
            Dependency.Role = TEXT("placement_mesh");
            OutDependencies.Add(MoveTemp(Dependency));
        }
        if (Node.Kind == EMHCompositeNodeKind::Random)
        {
            for (const FMHCompositeOption& Option : Node.Options)
            {
                FIndexedDependency OptionDependency;
                OptionDependency.Owner = Owner;
                OptionDependency.OwnerPath = OwnerPath;
                if (Option.Kind == EMHCompositeOptionKind::Mesh)
                {
                    OptionDependency.Target.Kind = EMHResourceKind::StaticMesh;
                    OptionDependency.Target.LogicalName = Option.Resource;
                    OptionDependency.Role = TEXT("placement_mesh");
                    OutDependencies.Add(MoveTemp(OptionDependency));
                }
                else if (Option.Kind == EMHCompositeOptionKind::Composite)
                {
                    OptionDependency.Target.Kind = EMHResourceKind::Composite;
                    OptionDependency.Target.LogicalName = Option.Resource;
                    OptionDependency.Role = TEXT("placement_composite");
                    OutDependencies.Add(MoveTemp(OptionDependency));
                }
            }
        }
        else if (Node.Kind == EMHCompositeNodeKind::Composite)
        {
            Dependency.Target.Kind = EMHResourceKind::Composite;
            Dependency.Target.LogicalName = Node.Resource;
            Dependency.Role = TEXT("placement_composite");
            OutDependencies.Add(MoveTemp(Dependency));
        }
        CollectCompositeDependencies(Node.Children, Owner, OwnerPath, OutDependencies);
    }
}

void DeduplicateDependencies(TArray<FIndexedDependency>& Dependencies)
{
    Dependencies.Sort([](const FIndexedDependency& A, const FIndexedDependency& B)
    {
        if (A.Target.Kind != B.Target.Kind)
        {
            return static_cast<uint8>(A.Target.Kind) < static_cast<uint8>(B.Target.Kind);
        }
        if (A.Target.LogicalName != B.Target.LogicalName)
        {
            return A.Target.LogicalName < B.Target.LogicalName;
        }
        return A.Role < B.Role;
    });
    for (int32 Index = Dependencies.Num() - 1; Index > 0; --Index)
    {
        const FIndexedDependency& A = Dependencies[Index - 1];
        const FIndexedDependency& B = Dependencies[Index];
        if (A.Target == B.Target && A.Role == B.Role)
        {
            Dependencies.RemoveAt(Index);
        }
    }
}

FString EscapeDumpField(FString Value)
{
    Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
    Value.ReplaceInline(TEXT("\t"), TEXT("\\t"));
    Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
    Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
    return Value;
}

bool ReadNullableString(
    const FSQLitePreparedStatement& Statement,
    const int32 Column,
    FString& OutValue)
{
    ESQLiteColumnType Type = ESQLiteColumnType::Null;
    if (!Statement.GetColumnTypeByIndex(Column, Type))
    {
        return false;
    }
    if (Type == ESQLiteColumnType::Null)
    {
        OutValue = TEXT("~");
        return true;
    }
    return Statement.GetColumnValueByIndex(Column, OutValue);
}

bool ReadCandidateKeys(
    FSQLiteDatabase& Database,
    TSet<FMHResourceKey>& OutKeys,
    FString& OutError)
{
    OutKeys.Reset();
    FSQLitePreparedStatement Statement = Database.PrepareStatement(
        TEXT("SELECT DISTINCT kind,name FROM ResourceCandidates "
             "WHERE kind IS NOT NULL AND name IS NOT NULL ORDER BY kind,name;"));
    if (!Statement.IsValid())
    {
        OutError = Database.GetLastError();
        return false;
    }
    while (true)
    {
        const ESQLitePreparedStatementStepResult Step = Statement.Step();
        if (Step == ESQLitePreparedStatementStepResult::Done)
        {
            return true;
        }
        FString Kind;
        FMHResourceKey Key;
        if (Step != ESQLitePreparedStatementStepResult::Row ||
            !Statement.GetColumnValueByIndex(0, Kind) ||
            !Statement.GetColumnValueByIndex(1, Key.LogicalName) ||
            !MHResourceKindFromLabel(Kind, Key.Kind) ||
            !Key.IsCanonical())
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: malformed candidate key set");
            return false;
        }
        OutKeys.Add(MoveTemp(Key));
    }
}

FCandidateKeyTransitions CandidateKeyTransitions(
    const TSet<FMHResourceKey>& Before,
    const TSet<FMHResourceKey>& After)
{
    FCandidateKeyTransitions Result;
    for (const FMHResourceKey& Key : Before)
    {
        if (!After.Contains(Key))
        {
            Result.Disappeared.Add(Key);
        }
    }
    for (const FMHResourceKey& Key : After)
    {
        if (!Before.Contains(Key))
        {
            Result.Appeared.Add(Key);
        }
    }
    return Result;
}

} // namespace

const TCHAR* MHGeneratedAssetStatusLabel(const EMHGeneratedAssetStatus Status)
{
    switch (Status)
    {
    case EMHGeneratedAssetStatus::Applied: return TEXT("applied");
    case EMHGeneratedAssetStatus::Stale: return TEXT("stale");
    case EMHGeneratedAssetStatus::Orphan: return TEXT("orphan");
    case EMHGeneratedAssetStatus::SourceBlocked: return TEXT("source_blocked");
    case EMHGeneratedAssetStatus::InvalidReceipt: return TEXT("invalid_receipt");
    case EMHGeneratedAssetStatus::DuplicateClaim: return TEXT("duplicate_claim");
    }
    return TEXT("invalid_receipt");
}

class FMHProjectResourceIndex::FImpl
{
public:
    FImpl(FString InSourceRoot, FString InDatabasePath)
        : SourceRoot(MoveTemp(InSourceRoot))
        , DatabasePath(MoveTemp(InDatabasePath))
    {
        SourceRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
        FPaths::NormalizeDirectoryName(SourceRoot);
        if (DatabasePath.IsEmpty())
        {
            DatabasePath = FMHProjectResourceIndex::DefaultDatabasePath();
        }
        DatabasePath = FPaths::ConvertRelativePathToFull(DatabasePath);
        FPaths::NormalizeFilename(DatabasePath);
    }

    bool Open(bool& bOutRecreated, FString& OutError)
    {
        Close();
        bOutRecreated = false;
        OutError.Reset();

        const bool bExisted = IFileManager::Get().FileExists(*DatabasePath);
        if (bExisted)
        {
            Database = MakeUnique<FSQLiteDatabase>();
            if (Database->Open(*DatabasePath, ESQLiteDatabaseOpenMode::ReadWrite) &&
                ValidateExistingSchema())
            {
                return true;
            }
            Database->Close();
            Database.Reset();
            if (!IFileManager::Get().Delete(*DatabasePath, false, true, true))
            {
                OutError = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_INVALID: cannot replace invalid ProjectIndex cache: %s"),
                    *DatabasePath);
                return false;
            }
            bOutRecreated = true;
        }
        else
        {
            bOutRecreated = true;
        }

        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(DatabasePath), true))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot create ProjectIndex directory");
            return false;
        }
        Database = MakeUnique<FSQLiteDatabase>();
        if (!Database->Open(*DatabasePath, ESQLiteDatabaseOpenMode::ReadWriteCreate) ||
            !CreateSchema(OutError))
        {
            if (OutError.IsEmpty())
            {
                OutError = Database.IsValid()
                    ? Database->GetLastError()
                    : TEXT("MH_E_SOURCE_INDEX_INVALID: cannot open ProjectIndex.sqlite");
            }
            Close();
            return false;
        }
        Generation = 0;
        return true;
    }

    void Close()
    {
        if (Database.IsValid())
        {
            Database->Close();
            Database.Reset();
        }
        Generation = 0;
        SelfPublishTokens.Reset();
        PendingOrphanRebindDivergences.Reset();
    }

    bool IsOpen() const
    {
        return Database.IsValid() && Database->IsValid();
    }

    bool FullScan(
        const TArray<FMHGeneratedAssetTagClaim>& Claims,
        FMHProjectIndexUpdateResult& OutResult,
        FString& OutError)
    {
        OutResult = FMHProjectIndexUpdateResult();
        TSet<FMHResourceKey> CandidateKeysBefore;
        const bool bPerfTrace = MHIsSourceScanPerfActive();
        const uint64 InitialSQLiteStart = bPerfTrace ? FPlatformTime::Cycles64() : 0;
        const bool bReady = EnsureOpen(OutError) &&
            ReadCandidateKeys(*Database, CandidateKeysBefore, OutError);
        if (bPerfTrace)
        {
            MHRecordSourceScanSQLite(FPlatformTime::Cycles64() - InitialSQLiteStart);
        }
        if (!bReady)
        {
            return false;
        }
        TArray<FScannedCandidate> Candidates;
        if (!ScanFullSnapshot(Candidates, OutError))
        {
            return false;
        }
        const uint64 SQLiteStart = bPerfTrace ? FPlatformTime::Cycles64() : 0;
        ON_SCOPE_EXIT
        {
            if (bPerfTrace)
            {
                MHRecordSourceScanSQLite(FPlatformTime::Cycles64() - SQLiteStart);
            }
        };
        TArray<FGeneratedRow> GeneratedRows;
        BuildGeneratedRows(Claims, GeneratedRows);
        const TSet<FMHResourceKey> PreviousPendingOrphanRebinds =
            PendingOrphanRebindDivergences;
        if (!CaptureOrphanRebindTransitions(Candidates, OutError))
        {
            PendingOrphanRebindDivergences = PreviousPendingOrphanRebinds;
            return false;
        }

        const int64 NextGeneration = Generation + 1;
        if (!BeginTransaction(OutError))
        {
            PendingOrphanRebindDivergences = PreviousPendingOrphanRebinds;
            return false;
        }
        TSet<FMHResourceKey> CandidateKeysAfter;
        if (!Execute(TEXT("DELETE FROM Dependencies;"), OutError) ||
            !Execute(TEXT("DELETE FROM ResourceCandidates;"), OutError) ||
            !Execute(TEXT("DELETE FROM GeneratedAssets;"), OutError) ||
            !InsertCandidates(Candidates, NextGeneration, OutError) ||
            !InsertGeneratedRows(GeneratedRows, NextGeneration, OutError) ||
            !ReadCandidateKeys(*Database, CandidateKeysAfter, OutError) ||
            !RecomputeDerivedState(
                CandidateKeyTransitions(CandidateKeysBefore, CandidateKeysAfter),
                OutError) ||
            !WriteGeneration(NextGeneration, OutError) ||
            !CommitTransaction(OutError))
        {
            RollbackTransaction();
            PendingOrphanRebindDivergences = PreviousPendingOrphanRebinds;
            return false;
        }
        Generation = NextGeneration;
        ++FullScanCount;
        MHRecordFullScanCompleted();
        ConsumeMatchingTokens(Candidates, NextGeneration - 1, OutResult.SessionEvents);
        OutResult.Generation = Generation;
        return true;
    }

    bool UpsertPaths(
        const TArray<FString>& Paths,
        FMHProjectIndexUpdateResult& OutResult,
        FString& OutError)
    {
        OutResult = FMHProjectIndexUpdateResult();
        OutError.Reset();
        if (!EnsureOpen(OutError))
        {
            return false;
        }
        TSet<FMHResourceKey> CandidateKeysBefore;
        if (!ReadCandidateKeys(*Database, CandidateKeysBefore, OutError))
        {
            return false;
        }

        TArray<FString> AbsolutePaths;
        for (const FString& Path : Paths)
        {
            FString Absolute;
            FString Relative;
            if (!NormalizeSourcePath(Path, Absolute, Relative, OutError))
            {
                return false;
            }
            AbsolutePaths.AddUnique(MoveTemp(Absolute));
        }
        AbsolutePaths.Sort();

        TArray<FScannedCandidate> Candidates;
        TArray<FString> RelativePaths;
        for (const FString& Absolute : AbsolutePaths)
        {
            FString Relative;
            FString Ignored;
            if (!NormalizeSourcePath(Absolute, Ignored, Relative, OutError))
            {
                return false;
            }
            RelativePaths.Add(Relative);
            if (IFileManager::Get().FileExists(*Absolute))
            {
                FScannedCandidate Candidate;
                bool bRecognized = false;
                if (!ScanOnePath(Absolute, Candidate, bRecognized, OutError))
                {
                    return false;
                }
                if (bRecognized)
                {
                    Candidates.Add(MoveTemp(Candidate));
                }
            }
        }

        const TSet<FMHResourceKey> PreviousPendingOrphanRebinds =
            PendingOrphanRebindDivergences;
        if (!CaptureOrphanRebindTransitions(Candidates, OutError))
        {
            PendingOrphanRebindDivergences = PreviousPendingOrphanRebinds;
            return false;
        }
        const int64 PreviousGeneration = Generation;
        const int64 NextGeneration = Generation + 1;
        if (!BeginTransaction(OutError))
        {
            PendingOrphanRebindDivergences = PreviousPendingOrphanRebinds;
            return false;
        }
        for (const FString& Relative : RelativePaths)
        {
            if (!DeleteCandidateByPath(Relative, OutError))
            {
                RollbackTransaction();
                PendingOrphanRebindDivergences = PreviousPendingOrphanRebinds;
                return false;
            }
        }
        TSet<FMHResourceKey> CandidateKeysAfter;
        if (!InsertCandidates(Candidates, NextGeneration, OutError) ||
            !ReadCandidateKeys(*Database, CandidateKeysAfter, OutError) ||
            !RecomputeDerivedState(
                CandidateKeyTransitions(CandidateKeysBefore, CandidateKeysAfter),
                OutError) ||
            !WriteGeneration(NextGeneration, OutError) ||
            !CommitTransaction(OutError))
        {
            RollbackTransaction();
            PendingOrphanRebindDivergences = PreviousPendingOrphanRebinds;
            return false;
        }
        Generation = NextGeneration;
        ConsumeMatchingTokens(Candidates, PreviousGeneration, OutResult.SessionEvents);
        OutResult.Generation = Generation;
        return true;
    }

    bool ReplaceGeneratedAssets(
        const TArray<FMHGeneratedAssetTagClaim>& Claims,
        FMHProjectIndexUpdateResult& OutResult,
        FString& OutError)
    {
        OutResult = FMHProjectIndexUpdateResult();
        TArray<FGeneratedRow> Rows;
        BuildGeneratedRows(Claims, Rows);
        const TSet<FMHResourceKey> PreviousPendingOrphanRebinds =
            PendingOrphanRebindDivergences;
        const int64 NextGeneration = Generation + 1;
        if (!BeginTransaction(OutError))
        {
            return false;
        }
        if (!Execute(TEXT("DELETE FROM GeneratedAssets;"), OutError) ||
            !InsertGeneratedRows(Rows, NextGeneration, OutError) ||
            !RecomputeDerivedState(FCandidateKeyTransitions(), OutError) ||
            !WriteGeneration(NextGeneration, OutError) ||
            !CommitTransaction(OutError))
        {
            RollbackTransaction();
            PendingOrphanRebindDivergences = PreviousPendingOrphanRebinds;
            return false;
        }
        Generation = NextGeneration;
        OutResult.Generation = Generation;
        return true;
    }

    bool RegisterSelfPublishAfterReplace(
        const FString& Path,
        const FString& RawHash,
        FString& OutError)
    {
        FString Absolute;
        FString Relative;
        if (!EnsureOpen(OutError) ||
            !NormalizeSourcePath(Path, Absolute, Relative, OutError) ||
            !MHIsCanonicalRawPayloadHash(RawHash))
        {
            if (OutError.IsEmpty())
            {
                OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: self-publish token hash is not canonical");
            }
            return false;
        }
        FSelfPublishToken Token;
        Token.AbsolutePath = MoveTemp(Absolute);
        Token.RawHash = RawHash;
        Token.Generation = Generation;
        SelfPublishTokens.Add(MoveTemp(Token));
        return true;
    }

    bool ConsumeOrphanRebindEvent(
        const FMHResourceKey& Key,
        FString& OutEvent)
    {
        OutEvent.Reset();
        if (!Key.IsCanonical() || PendingOrphanRebindDivergences.Remove(Key) == 0)
        {
            return false;
        }
        OutEvent = FString::Printf(
            TEXT("MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED: %s"),
            *Key.ToString());
        return true;
    }

#if WITH_DEV_AUTOMATION_TESTS
    bool InjectResolutionStatusForTests(
        const FMHResourceKey& Key,
        const FString& Status,
        FString& OutError)
    {
        if (!EnsureOpen(OutError))
        {
            return false;
        }
        FSQLitePreparedStatement Statement = Database->PrepareStatement(
            TEXT("UPDATE ResourceKeys SET resolution_status=?1 WHERE kind=?2 AND name=?3;"));
        if (!Statement.IsValid() ||
            !Statement.SetBindingValueByIndex(1, Status) ||
            !Statement.SetBindingValueByIndex(2, FString(MHResourceKindLabel(Key.Kind))) ||
            !Statement.SetBindingValueByIndex(3, Key.LogicalName) ||
            !Statement.Execute())
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: cannot inject test status: %s"),
                *Database->GetLastError());
            return false;
        }
        return true;
    }

    bool InjectExtraSchemaColumnForTests(FString& OutError)
    {
        return EnsureOpen(OutError) && Execute(
            TEXT("ALTER TABLE ResourceKeys ADD COLUMN extra_col TEXT;"),
            OutError);
    }
#endif

    FMHResolveOutcome Resolve(const FMHResourceKey& Key) const;
    FMHSourceSnapshot GetSnapshot() const;
    bool IsImportBlocked(const FMHResourceKey& Key, FString& OutDiagnostic) const;
    bool GetGeneratedAssets(
        const FMHResourceKey& Key,
        TArray<FMHProjectIndexGeneratedAssetState>& OutAssets,
        FString& OutError) const;
    bool GetPlacementProfileDependencies(
        const FMHResourceKey& CompositeKey,
        TArray<FMHResourceKey>& OutProfiles,
        FString& OutError) const;
    bool GetAllGeneratedAssets(
        TArray<FMHProjectIndexGeneratedAssetState>& OutAssets,
        FString& OutError) const;
    bool GetDiagnostics(
        TArray<FMHProjectIndexDiagnostic>& OutDiagnostics,
        FString& OutError) const;
    bool BuildNormalizedDump(FString& OutDump, FString& OutError) const;

    FString SourceRoot;
    FString DatabasePath;
    TUniquePtr<FSQLiteDatabase> Database;
    int64 Generation = 0;
    int32 FullScanCount = 0;
    TArray<FSelfPublishToken> SelfPublishTokens;
    TSet<FMHResourceKey> PendingOrphanRebindDivergences;

private:
    bool EnsureOpen(FString& OutError) const
    {
        if (IsOpen())
        {
            return true;
        }
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: ProjectIndex is not open");
        return false;
    }

    bool Execute(const TCHAR* Sql, FString& OutError) const
    {
        if (Database->Execute(Sql))
        {
            return true;
        }
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: SQLite failure: %s"),
            *Database->GetLastError());
        return false;
    }

    bool BeginTransaction(FString& OutError) const
    {
        return EnsureOpen(OutError) && Execute(TEXT("BEGIN IMMEDIATE TRANSACTION;"), OutError);
    }

    bool CommitTransaction(FString& OutError) const
    {
        return Execute(TEXT("COMMIT TRANSACTION;"), OutError);
    }

    void RollbackTransaction() const
    {
        if (IsOpen())
        {
            Database->Execute(TEXT("ROLLBACK TRANSACTION;"));
        }
    }

    bool CreateSchema(FString& OutError)
    {
        const TCHAR* Statements[] = {
            TEXT("CREATE TABLE Meta(key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL);"),
            TEXT("CREATE TABLE ResourceCandidates(path TEXT PRIMARY KEY NOT NULL, kind TEXT, name TEXT, size INTEGER NOT NULL, mtime INTEGER NOT NULL, raw_hash TEXT, parse_status TEXT NOT NULL, diagnostic TEXT NOT NULL, last_seen_generation INTEGER NOT NULL);"),
            TEXT("CREATE INDEX ResourceCandidates_Key ON ResourceCandidates(kind, name);"),
            TEXT("CREATE TABLE ResourceKeys(kind TEXT NOT NULL, name TEXT NOT NULL, resolution_status TEXT NOT NULL, PRIMARY KEY(kind, name));"),
            TEXT("CREATE TABLE Dependencies(owner_kind TEXT NOT NULL, owner_name TEXT NOT NULL, target_kind TEXT NOT NULL, target_name TEXT NOT NULL, role TEXT NOT NULL, owner_path TEXT NOT NULL, PRIMARY KEY(owner_kind, owner_name, target_kind, target_name, role, owner_path));"),
            TEXT("CREATE INDEX Dependencies_Owner ON Dependencies(owner_kind, owner_name);"),
            TEXT("CREATE INDEX Dependencies_Target ON Dependencies(target_kind, target_name);"),
            TEXT("CREATE TABLE GeneratedAssets(object_path TEXT PRIMARY KEY NOT NULL, kind TEXT NOT NULL, name TEXT NOT NULL, source_path TEXT NOT NULL, source_hash TEXT NOT NULL, applied_hash TEXT NOT NULL, carrier_kind TEXT NOT NULL, key_valid INTEGER NOT NULL, receipt_valid INTEGER NOT NULL, status TEXT NOT NULL, diagnostic TEXT NOT NULL, last_seen_generation INTEGER NOT NULL);"),
            TEXT("CREATE INDEX GeneratedAssets_Key ON GeneratedAssets(kind, name);"),
            TEXT("CREATE TABLE Diagnostics(id INTEGER PRIMARY KEY AUTOINCREMENT, severity TEXT NOT NULL, code TEXT NOT NULL, owner_kind TEXT NOT NULL, owner_name TEXT NOT NULL, path TEXT NOT NULL, target_kind TEXT NOT NULL, target_name TEXT NOT NULL, message TEXT NOT NULL);"),
            TEXT("INSERT INTO Meta(key, value) VALUES('tag', 'mh.project_index:4');"),
            TEXT("INSERT INTO Meta(key, value) VALUES('generation', '0');")};
        for (const TCHAR* Statement : Statements)
        {
            if (!Execute(Statement, OutError))
            {
                return false;
            }
        }
        return true;
    }

    bool ValidateExistingSchema()
    {
        if (!Database->PerformQuickIntegrityCheck())
        {
            return false;
        }

        TMap<FString, FString> ExpectedSchema;
        auto AddExpectedSchema = [&ExpectedSchema](
            const TCHAR* Type,
            const TCHAR* Name,
            const TCHAR* Table,
            const TCHAR* Sql)
        {
            ExpectedSchema.Add(
                Name,
                FString::Printf(TEXT("%s|%s|%s"), Type, Table, Sql));
        };
        AddExpectedSchema(
            TEXT("table"), TEXT("Meta"), TEXT("Meta"),
            TEXT("CREATE TABLE Meta(key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL)"));
        AddExpectedSchema(
            TEXT("table"), TEXT("ResourceCandidates"), TEXT("ResourceCandidates"),
            TEXT("CREATE TABLE ResourceCandidates(path TEXT PRIMARY KEY NOT NULL, kind TEXT, name TEXT, size INTEGER NOT NULL, mtime INTEGER NOT NULL, raw_hash TEXT, parse_status TEXT NOT NULL, diagnostic TEXT NOT NULL, last_seen_generation INTEGER NOT NULL)"));
        AddExpectedSchema(
            TEXT("index"), TEXT("ResourceCandidates_Key"), TEXT("ResourceCandidates"),
            TEXT("CREATE INDEX ResourceCandidates_Key ON ResourceCandidates(kind, name)"));
        AddExpectedSchema(
            TEXT("table"), TEXT("ResourceKeys"), TEXT("ResourceKeys"),
            TEXT("CREATE TABLE ResourceKeys(kind TEXT NOT NULL, name TEXT NOT NULL, resolution_status TEXT NOT NULL, PRIMARY KEY(kind, name))"));
        AddExpectedSchema(
            TEXT("table"), TEXT("Dependencies"), TEXT("Dependencies"),
            TEXT("CREATE TABLE Dependencies(owner_kind TEXT NOT NULL, owner_name TEXT NOT NULL, target_kind TEXT NOT NULL, target_name TEXT NOT NULL, role TEXT NOT NULL, owner_path TEXT NOT NULL, PRIMARY KEY(owner_kind, owner_name, target_kind, target_name, role, owner_path))"));
        AddExpectedSchema(
            TEXT("index"), TEXT("Dependencies_Owner"), TEXT("Dependencies"),
            TEXT("CREATE INDEX Dependencies_Owner ON Dependencies(owner_kind, owner_name)"));
        AddExpectedSchema(
            TEXT("index"), TEXT("Dependencies_Target"), TEXT("Dependencies"),
            TEXT("CREATE INDEX Dependencies_Target ON Dependencies(target_kind, target_name)"));
        AddExpectedSchema(
            TEXT("table"), TEXT("GeneratedAssets"), TEXT("GeneratedAssets"),
            TEXT("CREATE TABLE GeneratedAssets(object_path TEXT PRIMARY KEY NOT NULL, kind TEXT NOT NULL, name TEXT NOT NULL, source_path TEXT NOT NULL, source_hash TEXT NOT NULL, applied_hash TEXT NOT NULL, carrier_kind TEXT NOT NULL, key_valid INTEGER NOT NULL, receipt_valid INTEGER NOT NULL, status TEXT NOT NULL, diagnostic TEXT NOT NULL, last_seen_generation INTEGER NOT NULL)"));
        AddExpectedSchema(
            TEXT("index"), TEXT("GeneratedAssets_Key"), TEXT("GeneratedAssets"),
            TEXT("CREATE INDEX GeneratedAssets_Key ON GeneratedAssets(kind, name)"));
        AddExpectedSchema(
            TEXT("table"), TEXT("Diagnostics"), TEXT("Diagnostics"),
            TEXT("CREATE TABLE Diagnostics(id INTEGER PRIMARY KEY AUTOINCREMENT, severity TEXT NOT NULL, code TEXT NOT NULL, owner_kind TEXT NOT NULL, owner_name TEXT NOT NULL, path TEXT NOT NULL, target_kind TEXT NOT NULL, target_name TEXT NOT NULL, message TEXT NOT NULL)"));

        bool bSchemaSignatureValid = true;
        int64 SchemaRows = 0;
        FSQLitePreparedStatement Schema = Database->PrepareStatement(
            TEXT("SELECT type,name,tbl_name,sql FROM sqlite_master "
                 "WHERE name NOT LIKE 'sqlite_%' ORDER BY name;"));
        if (!Schema.IsValid())
        {
            return false;
        }
        while (true)
        {
            const ESQLitePreparedStatementStepResult Step = Schema.Step();
            if (Step == ESQLitePreparedStatementStepResult::Done)
            {
                break;
            }
            if (Step != ESQLitePreparedStatementStepResult::Row)
            {
                return false;
            }
            FString Type;
            FString Name;
            FString Table;
            FString Sql;
            if (!Schema.GetColumnValueByIndex(0, Type) ||
                !Schema.GetColumnValueByIndex(1, Name) ||
                !Schema.GetColumnValueByIndex(2, Table) ||
                !Schema.GetColumnValueByIndex(3, Sql))
            {
                return false;
            }
            const FString* Expected = ExpectedSchema.Find(Name);
            const FString Actual = FString::Printf(TEXT("%s|%s|%s"), *Type, *Table, *Sql);
            if (Expected == nullptr || *Expected != Actual)
            {
                bSchemaSignatureValid = false;
            }
            else
            {
                ExpectedSchema.Remove(Name);
            }
            ++SchemaRows;
        }
        if (!bSchemaSignatureValid || SchemaRows != 10 || !ExpectedSchema.IsEmpty())
        {
            return false;
        }

        int64 TableCount = INDEX_NONE;
        const int64 Rows = Database->Execute(
            TEXT("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';"),
            [&TableCount](const FSQLitePreparedStatement& Statement)
            {
                return Statement.GetColumnValueByIndex(0, TableCount)
                    ? ESQLitePreparedStatementExecuteRowResult::Continue
                    : ESQLitePreparedStatementExecuteRowResult::Error;
            });
        if (Rows != 1 || TableCount != 6)
        {
            return false;
        }
        for (const TCHAR* Query : {
            TEXT("SELECT path,kind,name,size,mtime,raw_hash,parse_status,diagnostic,last_seen_generation FROM ResourceCandidates LIMIT 0;"),
            TEXT("SELECT kind,name,resolution_status FROM ResourceKeys LIMIT 0;"),
            TEXT("SELECT owner_kind,owner_name,target_kind,target_name,role,owner_path FROM Dependencies LIMIT 0;"),
            TEXT("SELECT object_path,kind,name,source_path,source_hash,applied_hash,carrier_kind,key_valid,receipt_valid,status,diagnostic,last_seen_generation FROM GeneratedAssets LIMIT 0;"),
            TEXT("SELECT severity,code,owner_kind,owner_name,path,target_kind,target_name,message FROM Diagnostics LIMIT 0;")})
        {
            if (!Database->PrepareStatement(Query).IsValid())
            {
                return false;
            }
        }

        auto HasInvalidRows = [this](const TCHAR* Query)
        {
            int64 InvalidCount = INDEX_NONE;
            const int64 ResultRows = Database->Execute(
                Query,
                [&InvalidCount](const FSQLitePreparedStatement& Statement)
                {
                    return Statement.GetColumnValueByIndex(0, InvalidCount)
                        ? ESQLitePreparedStatementExecuteRowResult::Continue
                        : ESQLitePreparedStatementExecuteRowResult::Error;
                });
            return ResultRows != 1 || InvalidCount != 0;
        };
        if (HasInvalidRows(
                TEXT("SELECT COUNT(*) FROM ResourceCandidates WHERE "
                     "parse_status NOT IN ('ok','noncanonical','unreadable','invalid_payload') "
                     "OR ((kind IS NULL) <> (name IS NULL)) "
                     "OR (kind IS NOT NULL AND kind NOT IN ('static_mesh','material','composite','placement_profile','texture'));")) ||
            HasInvalidRows(
                TEXT("SELECT COUNT(*) FROM ResourceKeys WHERE "
                     "kind NOT IN ('static_mesh','material','composite','placement_profile','texture') "
                     "OR resolution_status NOT IN ('unique','ambiguous','invalid','missing');")) ||
            HasInvalidRows(
                TEXT("WITH CandidateStates(kind,name,resolution_status) AS ("
                     "SELECT kind,name,CASE WHEN COUNT(*)>=2 THEN 'ambiguous' "
                     "WHEN MIN(parse_status)='ok' AND MAX(parse_status)='ok' THEN 'unique' "
                     "ELSE 'invalid' END FROM ResourceCandidates "
                     "WHERE kind IS NOT NULL AND name IS NOT NULL GROUP BY kind,name), "
                     "Referenced(kind,name) AS ("
                     "SELECT target_kind,target_name FROM Dependencies UNION "
                     "SELECT kind,name FROM GeneratedAssets WHERE key_valid=1), "
                     "Expected(kind,name,resolution_status) AS ("
                     "SELECT kind,name,resolution_status FROM CandidateStates UNION ALL "
                     "SELECT r.kind,r.name,'missing' FROM Referenced r WHERE NOT EXISTS ("
                     "SELECT 1 FROM CandidateStates c WHERE c.kind=r.kind AND c.name=r.name)), "
                     "Mismatches AS ("
                     "SELECT e.kind,e.name FROM Expected e LEFT JOIN ResourceKeys a "
                     "ON a.kind=e.kind AND a.name=e.name "
                     "WHERE a.kind IS NULL OR a.resolution_status<>e.resolution_status UNION ALL "
                     "SELECT a.kind,a.name FROM ResourceKeys a LEFT JOIN Expected e "
                     "ON e.kind=a.kind AND e.name=a.name WHERE e.kind IS NULL) "
                     "SELECT COUNT(*) FROM Mismatches;")) ||
            HasInvalidRows(
                TEXT("SELECT COUNT(*) FROM Dependencies WHERE "
                     "NOT ((owner_kind='material' AND target_kind='texture' AND role='texture') "
                     "OR (owner_kind='composite' AND target_kind='static_mesh' AND role='placement_mesh') "
                     "OR (owner_kind='composite' AND target_kind='composite' AND role='placement_composite') "
                     "OR (owner_kind='composite' AND target_kind='placement_profile' AND role='profile') "
                     "OR (owner_kind='static_mesh' AND target_kind='material' AND role='slot'));")) ||
            HasInvalidRows(
                TEXT("SELECT COUNT(*) FROM GeneratedAssets WHERE "
                     "key_valid NOT IN (0,1) OR receipt_valid NOT IN (0,1) "
                     "OR status NOT IN ('applied','stale','orphan','source_blocked','invalid_receipt','duplicate_claim') "
                     "OR (key_valid=1 AND kind NOT IN ('static_mesh','material','composite','texture')) "
                     "OR (receipt_valid=1 AND carrier_kind NOT IN ('static_mesh','material','composite','texture'));")) ||
            HasInvalidRows(
                TEXT("SELECT COUNT(*) FROM Diagnostics WHERE severity NOT IN ('error','warning');")))
        {
            return false;
        }

        FString Tag;
        FString GenerationText;
        FSQLitePreparedStatement Statement = Database->PrepareStatement(
            TEXT("SELECT key,value FROM Meta ORDER BY key;"));
        if (!Statement.IsValid())
        {
            return false;
        }
        int32 Count = 0;
        while (Statement.Step() == ESQLitePreparedStatementStepResult::Row)
        {
            FString Key;
            FString Value;
            if (!Statement.GetColumnValueByIndex(0, Key) ||
                !Statement.GetColumnValueByIndex(1, Value))
            {
                return false;
            }
            if (Key == TEXT("tag")) Tag = Value;
            else if (Key == TEXT("generation")) GenerationText = Value;
            else return false;
            ++Count;
        }
        int64 ParsedGeneration = 0;
        if (Count != 2 || Tag != ProjectIndexTag ||
            !LexTryParseString(ParsedGeneration, *GenerationText) || ParsedGeneration < 0)
        {
            return false;
        }
        Generation = ParsedGeneration;
        return true;
    }

    bool CaptureOrphanRebindTransitions(
        const TArray<FScannedCandidate>& Candidates,
        FString& OutError)
    {
        FSQLitePreparedStatement Statement = Database->PrepareStatement(
            TEXT("SELECT source_hash FROM GeneratedAssets WHERE kind=?1 AND name=?2 AND status='orphan' AND receipt_valid=1 ORDER BY object_path;"));
        if (!Statement.IsValid())
        {
            OutError = Database->GetLastError();
            return false;
        }

        TSet<FMHResourceKey> InspectedKeys;
        for (const FScannedCandidate& Candidate : Candidates)
        {
            if (!Candidate.bHasKey || Candidate.ParseStatus != ECandidateParseStatus::Ok ||
                Candidate.RawHash.IsEmpty() || InspectedKeys.Contains(Candidate.Key))
            {
                continue;
            }
            InspectedKeys.Add(Candidate.Key);

            if (!Statement.SetBindingValueByIndex(
                    1, FString(MHResourceKindLabel(Candidate.Key.Kind))) ||
                !Statement.SetBindingValueByIndex(2, Candidate.Key.LogicalName))
            {
                OutError = Database->GetLastError();
                return false;
            }
            FString OrphanSourceHash;
            int32 OrphanCount = 0;
            while (true)
            {
                const ESQLitePreparedStatementStepResult Step = Statement.Step();
                if (Step == ESQLitePreparedStatementStepResult::Done)
                {
                    break;
                }
                FString SourceHash;
                if (Step != ESQLitePreparedStatementStepResult::Row ||
                    !Statement.GetColumnValueByIndex(0, SourceHash))
                {
                    OutError = Database->GetLastError();
                    return false;
                }
                OrphanSourceHash = MoveTemp(SourceHash);
                ++OrphanCount;
            }
            Statement.Reset();
            Statement.ClearBindings();

            if (OrphanCount == 1)
            {
                if (Candidate.RawHash == OrphanSourceHash)
                {
                    PendingOrphanRebindDivergences.Remove(Candidate.Key);
                }
                else
                {
                    PendingOrphanRebindDivergences.Add(Candidate.Key);
                }
            }
        }
        return true;
    }

    bool WriteGeneration(const int64 Value, FString& OutError) const
    {
        FSQLitePreparedStatement Statement = Database->PrepareStatement(
            TEXT("UPDATE Meta SET value=?1 WHERE key='generation';"));
        if (!Statement.IsValid() || !Statement.SetBindingValueByIndex(1, LexToString(Value)) ||
            !Statement.Execute())
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: cannot persist generation: %s"),
                *Database->GetLastError());
            return false;
        }
        return true;
    }

    bool NormalizeSourcePath(
        const FString& Input,
        FString& OutAbsolute,
        FString& OutRelative,
        FString& OutError) const
    {
        OutAbsolute = FPaths::IsRelative(Input)
            ? FPaths::ConvertRelativePathToFull(SourceRoot, Input)
            : FPaths::ConvertRelativePathToFull(Input);
        FPaths::NormalizeFilename(OutAbsolute);
        FPaths::CollapseRelativeDirectories(OutAbsolute);
        if (!FPaths::IsUnderDirectory(OutAbsolute, SourceRoot))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: path escapes source_root: %s"),
                *Input);
            return false;
        }
        FString RootWithSlash = SourceRoot + TEXT("/");
        OutRelative = OutAbsolute;
        if (!FPaths::MakePathRelativeTo(OutRelative, *RootWithSlash) ||
            !IsValidRelativeSourcePath(OutRelative))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: cannot make canonical relative source path");
            return false;
        }
        FPaths::NormalizeFilename(OutRelative);
        return true;
    }

    bool EnumerateKnownPaths(TArray<FString>& OutPaths, FString& OutError) const
    {
        OutPaths.Reset();
        OutError.Reset();
        if (!IFileManager::Get().DirectoryExists(*SourceRoot))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: source_root does not exist: %s"),
                *SourceRoot);
            return false;
        }
        const bool bTraversed = IFileManager::Get().IterateDirectoryRecursively(
            *SourceRoot,
            [this, &OutPaths, &OutError](const TCHAR* Path, const bool bIsDirectory)
            {
                if (bIsDirectory)
                {
                    FString Directory = FPaths::ConvertRelativePathToFull(Path);
                    FPaths::NormalizeDirectoryName(Directory);
                    if (!Directory.Equals(SourceRoot, ESearchCase::IgnoreCase) &&
                        (!FPaths::IsUnderDirectory(Directory, SourceRoot) ||
                         !ValidateNoNestedFilesystemAlias(SourceRoot, Directory, OutError)))
                    {
                        if (OutError.IsEmpty())
                        {
                            OutError = FString::Printf(
                                TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: source directory escapes source_root: %s"),
                                *Directory);
                        }
                        return false;
                    }
                    return true;
                }
                FString Absolute = FPaths::ConvertRelativePathToFull(Path);
                FPaths::NormalizeFilename(Absolute);
                FMHResourceKey Key;
                FString ClassificationError;
                const bool bKnown = MHResourceKeyFromSourceFile(Absolute, Key, ClassificationError);
                if (!bKnown && ClassificationError.IsEmpty())
                {
                    return true;
                }
                if (!FPaths::IsUnderDirectory(Absolute, SourceRoot) ||
                    !ValidateNoNestedFilesystemAlias(SourceRoot, Absolute, OutError))
                {
                    if (OutError.IsEmpty())
                    {
                        OutError = TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: discovered path escapes source_root");
                    }
                    return false;
                }
                OutPaths.Add(MoveTemp(Absolute));
                return true;
            });
        OutPaths.Sort();
        if (!bTraversed)
        {
            if (OutError.IsEmpty())
            {
                OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source tree traversal failed");
            }
            return false;
        }
        return true;
    }

    bool ScanOnePath(
        const FString& AbsolutePath,
        FScannedCandidate& OutCandidate,
        bool& bOutRecognized,
        FString& OutError) const
    {
        OutCandidate = FScannedCandidate();
        bOutRecognized = false;
        FString IgnoredAbsolute;
        if (!NormalizeSourcePath(
                AbsolutePath,
                IgnoredAbsolute,
                OutCandidate.RelativePath,
                OutError))
        {
            return false;
        }
        FMHResourceKey Key;
        FString ClassificationError;
        if (!MHResourceKeyFromSourceFile(AbsolutePath, Key, ClassificationError))
        {
            if (ClassificationError.IsEmpty())
            {
                return true;
            }
            bOutRecognized = true;
            OutCandidate.ParseStatus = ECandidateParseStatus::Noncanonical;
            OutCandidate.Diagnostic = ClassificationError;
            OutCandidate.Size = IFileManager::Get().FileSize(*AbsolutePath);
            OutCandidate.MTimeTicks = IFileManager::Get().GetTimeStamp(*AbsolutePath).GetTicks();
            return true;
        }
        bOutRecognized = true;
        OutCandidate.bHasKey = true;
        OutCandidate.Key = Key;

        const int64 SizeBefore = IFileManager::Get().FileSize(*AbsolutePath);
        const FDateTime TimeBefore = IFileManager::Get().GetTimeStamp(*AbsolutePath);
        TArray<uint8> Bytes;
        const bool bPerfTrace = MHIsSourceScanPerfActive();
        const uint64 IOHashStart = bPerfTrace ? FPlatformTime::Cycles64() : 0;
        const bool bRead = SizeBefore >= 0 && FFileHelper::LoadFileToArray(Bytes, *AbsolutePath);
        const int64 SizeAfter = IFileManager::Get().FileSize(*AbsolutePath);
        const FDateTime TimeAfter = IFileManager::Get().GetTimeStamp(*AbsolutePath);
        OutCandidate.Size = SizeAfter;
        OutCandidate.MTimeTicks = TimeAfter.GetTicks();
        if (!bRead)
        {
            if (bPerfTrace)
                MHRecordSourceScanIOHash(FPlatformTime::Cycles64() - IOHashStart, false);
            OutCandidate.ParseStatus = ECandidateParseStatus::Unreadable;
            OutCandidate.Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot read source payload");
            return true;
        }
        if (SizeBefore != SizeAfter || TimeBefore != TimeAfter ||
            static_cast<int64>(Bytes.Num()) != SizeAfter)
        {
            if (bPerfTrace)
                MHRecordSourceScanIOHash(FPlatformTime::Cycles64() - IOHashStart, false);
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: source payload changed during scan: %s"),
                *AbsolutePath);
            return false;
        }
        OutCandidate.RawHash = MHRawPayloadHash(Bytes);
        if (bPerfTrace)
            MHRecordSourceScanIOHash(FPlatformTime::Cycles64() - IOHashStart, true);
        OutCandidate.ParseStatus = ECandidateParseStatus::Ok;

        FString ParseError;
        const uint64 ParseStart = bPerfTrace ? FPlatformTime::Cycles64() : 0;
        if (Key.Kind == EMHResourceKind::StaticMesh)
        {
            FMHSceneIR Scene;
            FMHFbxSceneTranslator Translator;
            if (!Translator.Translate(Key.LogicalName, Bytes, Scene, ParseError))
            {
                OutCandidate.ParseStatus = ECandidateParseStatus::InvalidPayload;
                OutCandidate.Diagnostic = ParseError;
            }
            else
            {
                TArray<FString> SortedNames = Scene.MaterialNames;
                SortedNames.Sort();
                for (const FString& MaterialName : SortedNames)
                {
                    FIndexedDependency& Dependency = OutCandidate.Dependencies.AddDefaulted_GetRef();
                    Dependency.Owner = Key;
                    Dependency.Target.Kind = EMHResourceKind::Material;
                    Dependency.Target.LogicalName = MaterialName;
                    Dependency.Role = TEXT("slot");
                    Dependency.OwnerPath = OutCandidate.RelativePath;
                }
            }
        }
        else if (Key.Kind == EMHResourceKind::Material)
        {
            FMHMaterialDocument Document;
            if (!MHParseMaterialV4(Bytes, Document, ParseError))
            {
                OutCandidate.ParseStatus = ECandidateParseStatus::InvalidPayload;
                OutCandidate.Diagnostic = ParseError;
            }
            else
            {
                TSet<FString> TextureNames;
                for (const TPair<int32, FString>& Pair : Document.Textures)
                {
                    TextureNames.Add(Pair.Value);
                }
                TArray<FString> SortedNames = TextureNames.Array();
                SortedNames.Sort();
                for (const FString& TextureName : SortedNames)
                {
                    FIndexedDependency& Dependency = OutCandidate.Dependencies.AddDefaulted_GetRef();
                    Dependency.Owner = Key;
                    Dependency.Target.Kind = EMHResourceKind::Texture;
                    Dependency.Target.LogicalName = TextureName;
                    Dependency.Role = TEXT("texture");
                    Dependency.OwnerPath = OutCandidate.RelativePath;
                }
            }
        }
        else if (Key.Kind == EMHResourceKind::Composite)
        {
            FMHCompositeDocument Document;
            if (!MHParseCompositeV5(Bytes, Document, ParseError))
            {
                OutCandidate.ParseStatus = ECandidateParseStatus::InvalidPayload;
                OutCandidate.Diagnostic = ParseError;
            }
            else
            {
                CollectCompositeDependencies(
                    Document.Nodes,
                    Key,
                    OutCandidate.RelativePath,
                    OutCandidate.Dependencies);
                DeduplicateDependencies(OutCandidate.Dependencies);
            }
        }
        else if (Key.Kind == EMHResourceKind::PlacementProfile)
        {
            FMHPlacementProfile Profile;
            Profile.LogicalName = Key.LogicalName;
            if (!MHParsePlacementProfileV1(Bytes, Profile, ParseError))
            {
                OutCandidate.ParseStatus = ECandidateParseStatus::InvalidPayload;
                OutCandidate.Diagnostic = ParseError;
            }
        }
        if (bPerfTrace && (Key.Kind == EMHResourceKind::StaticMesh ||
            Key.Kind == EMHResourceKind::Material ||
            Key.Kind == EMHResourceKind::Composite ||
            Key.Kind == EMHResourceKind::PlacementProfile))
        {
            MHRecordSourceScanParse(
                Key,
                OutCandidate.RelativePath,
                FPlatformTime::Cycles64() - ParseStart);
        }
        return true;
    }

    bool ScanFullSnapshot(TArray<FScannedCandidate>& OutCandidates, FString& OutError) const
    {
        TArray<FString> FirstPaths;
        TArray<FString> ConfirmedPaths;
        const bool bPerfTrace = MHIsSourceScanPerfActive();
        const uint64 FirstEnumerationStart = bPerfTrace ? FPlatformTime::Cycles64() : 0;
        const bool bFirstEnumerated = EnumerateKnownPaths(FirstPaths, OutError);
        if (bPerfTrace)
        {
            MHRecordSourceScanEnumeration(
                FPlatformTime::Cycles64() - FirstEnumerationStart,
                FirstPaths.Num());
        }
        if (!bFirstEnumerated)
        {
            return false;
        }
        TArray<FScannedCandidate> First;
        if (bPerfTrace) MHRecordSourceScanPass();
        for (const FString& Path : FirstPaths)
        {
            FScannedCandidate Candidate;
            bool bRecognized = false;
            if (!ScanOnePath(Path, Candidate, bRecognized, OutError))
            {
                return false;
            }
            if (bRecognized)
            {
                if (bPerfTrace && Candidate.Size > 0)
                {
                    MHRecordSourceScanEnumeratedBytes(static_cast<uint64>(Candidate.Size));
                }
                First.Add(MoveTemp(Candidate));
            }
        }
        const uint64 ConfirmedEnumerationStart = bPerfTrace ? FPlatformTime::Cycles64() : 0;
        const bool bConfirmedEnumerated = EnumerateKnownPaths(ConfirmedPaths, OutError);
        if (bPerfTrace)
        {
            MHRecordSourceScanEnumeration(
                FPlatformTime::Cycles64() - ConfirmedEnumerationStart,
                ConfirmedPaths.Num());
        }
        if (!bConfirmedEnumerated || ConfirmedPaths != FirstPaths)
        {
            if (OutError.IsEmpty())
            {
                OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: source payload set changed during scan");
            }
            return false;
        }

        OutCandidates.Reset();
        if (bPerfTrace) MHRecordSourceScanPass();
        for (const FString& Path : ConfirmedPaths)
        {
            FScannedCandidate Candidate;
            bool bRecognized = false;
            if (!ScanOnePath(Path, Candidate, bRecognized, OutError) || !bRecognized)
            {
                if (OutError.IsEmpty())
                {
                    OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: source classification changed during scan");
                }
                return false;
            }
            OutCandidates.Add(MoveTemp(Candidate));
        }
        if (First.Num() != OutCandidates.Num())
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: candidate count changed during scan");
            return false;
        }
        for (int32 Index = 0; Index < First.Num(); ++Index)
        {
            const FScannedCandidate& A = First[Index];
            const FScannedCandidate& B = OutCandidates[Index];
            if (A.RelativePath != B.RelativePath || A.bHasKey != B.bHasKey ||
                (A.bHasKey && !(A.Key == B.Key)) || A.Size != B.Size ||
                A.MTimeTicks != B.MTimeTicks || A.RawHash != B.RawHash ||
                A.ParseStatus != B.ParseStatus)
            {
                OutError = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: source payload changed during scan: %s"),
                    *B.RelativePath);
                return false;
            }
        }
        return true;
    }

    void BuildGeneratedRows(
        const TArray<FMHGeneratedAssetTagClaim>& Claims,
        TArray<FGeneratedRow>& OutRows) const
    {
        OutRows.Reset();
        TSet<FString> SeenObjectPaths;
        for (const FMHGeneratedAssetTagClaim& Claim : Claims)
        {
            if (SeenObjectPaths.Contains(Claim.UEObjectPath))
            {
                continue;
            }
            SeenObjectPaths.Add(Claim.UEObjectPath);
            FGeneratedRow Row;
            Row.ObjectPath = Claim.UEObjectPath;
            Row.Kind = Claim.Kind;
            Row.Name = Claim.LogicalName;
            Row.SourcePath = Claim.SourcePath;
            Row.SourceHash = Claim.SourceHash;
            Row.AppliedHash = Claim.AppliedHash;

            EMHResourceKind ClaimedKind = EMHResourceKind::StaticMesh;
            FMHResourceKey Key;
            Key.LogicalName = Claim.LogicalName;
            Row.bKeyValid = MHResourceKindFromLabel(Claim.Kind, ClaimedKind) &&
                ClaimedKind != EMHResourceKind::PlacementProfile;
            if (Row.bKeyValid)
            {
                Key.Kind = ClaimedKind;
                Row.bKeyValid = Key.IsCanonical();
            }
            Row.CarrierKind = Claim.bHasCarrierKind
                ? MHResourceKindLabel(Claim.CarrierKind)
                : FString();

            FMHResourceKey PathKey;
            FString PathError;
            const bool bPathMatches = IsValidRelativeSourcePath(Claim.SourcePath) &&
                MHResourceKeyFromSourceFile(Claim.SourcePath, PathKey, PathError) &&
                Row.bKeyValid && PathKey == Key;
            const bool bGeneratedPathMatches = Row.bKeyValid &&
                Claim.UEObjectPath == ExpectedGeneratedObjectPath(ClaimedKind, Claim.LogicalName);
            Row.bReceiptValid = Row.bKeyValid && Claim.Managed == TEXT("True") &&
                Claim.MHTagCount == 6 &&
                Claim.bHasCarrierKind && Claim.CarrierKind == ClaimedKind &&
                FPackageName::IsValidObjectPath(Claim.UEObjectPath) &&
                bGeneratedPathMatches && bPathMatches &&
                MHIsCanonicalRawPayloadHash(Claim.SourceHash) &&
                MHIsCanonicalRawPayloadHash(Claim.AppliedHash) &&
                (!IsBinaryKind(ClaimedKind) || Claim.AppliedHash == Claim.SourceHash);
            if (!Row.bReceiptValid)
            {
                Row.Diagnostic = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_INVALID: invalid managed receipt at %s"),
                    *Claim.UEObjectPath);
            }
            OutRows.Add(MoveTemp(Row));
        }
        OutRows.Sort([](const FGeneratedRow& A, const FGeneratedRow& B)
        {
            return A.ObjectPath < B.ObjectPath;
        });
    }

    bool DeleteCandidateByPath(const FString& RelativePath, FString& OutError) const;
    bool InsertCandidates(
        const TArray<FScannedCandidate>& Candidates,
        int64 SeenGeneration,
        FString& OutError) const;
    bool InsertGeneratedRows(
        const TArray<FGeneratedRow>& Rows,
        int64 SeenGeneration,
        FString& OutError) const;
    bool RecomputeDerivedState(
        const FCandidateKeyTransitions& CandidateTransitions,
        FString& OutError);
    void ConsumeMatchingTokens(
        const TArray<FScannedCandidate>& Candidates,
        int64 TokenGeneration,
        TArray<FString>& OutEvents);
};

bool FMHProjectResourceIndex::FImpl::DeleteCandidateByPath(
    const FString& RelativePath,
    FString& OutError) const
{
    for (const TCHAR* Sql : {
        TEXT("DELETE FROM Dependencies WHERE owner_path=?1;"),
        TEXT("DELETE FROM ResourceCandidates WHERE path=?1;")})
    {
        FSQLitePreparedStatement Statement = Database->PrepareStatement(Sql);
        if (!Statement.IsValid() ||
            !Statement.SetBindingValueByIndex(1, RelativePath) ||
            !Statement.Execute())
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: cannot remove candidate %s: %s"),
                *RelativePath,
                *Database->GetLastError());
            return false;
        }
    }
    return true;
}

bool FMHProjectResourceIndex::FImpl::InsertCandidates(
    const TArray<FScannedCandidate>& Candidates,
    const int64 SeenGeneration,
    FString& OutError) const
{
    FSQLitePreparedStatement CandidateStatement = Database->PrepareStatement(
        TEXT("INSERT INTO ResourceCandidates(path,kind,name,size,mtime,raw_hash,parse_status,diagnostic,last_seen_generation) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);"));
    FSQLitePreparedStatement DependencyStatement = Database->PrepareStatement(
        TEXT("INSERT OR REPLACE INTO Dependencies(owner_kind,owner_name,target_kind,target_name,role,owner_path) VALUES(?1,?2,?3,?4,?5,?6);"));
    if (!CandidateStatement.IsValid() || !DependencyStatement.IsValid())
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: cannot prepare candidate statements: %s"),
            *Database->GetLastError());
        return false;
    }

    for (const FScannedCandidate& Candidate : Candidates)
    {
        bool bBound = CandidateStatement.SetBindingValueByIndex(1, Candidate.RelativePath);
        if (Candidate.bHasKey)
        {
            bBound &= CandidateStatement.SetBindingValueByIndex(
                2, FString(MHResourceKindLabel(Candidate.Key.Kind)));
            bBound &= CandidateStatement.SetBindingValueByIndex(3, Candidate.Key.LogicalName);
        }
        else
        {
            bBound &= CandidateStatement.SetBindingValueByIndex(2);
            bBound &= CandidateStatement.SetBindingValueByIndex(3);
        }
        bBound &= CandidateStatement.SetBindingValueByIndex(4, Candidate.Size);
        bBound &= CandidateStatement.SetBindingValueByIndex(5, Candidate.MTimeTicks);
        bBound &= Candidate.RawHash.IsEmpty()
            ? CandidateStatement.SetBindingValueByIndex(6)
            : CandidateStatement.SetBindingValueByIndex(6, Candidate.RawHash);
        bBound &= CandidateStatement.SetBindingValueByIndex(
            7, FString(CandidateStatusLabel(Candidate.ParseStatus)));
        bBound &= CandidateStatement.SetBindingValueByIndex(8, Candidate.Diagnostic);
        bBound &= CandidateStatement.SetBindingValueByIndex(9, SeenGeneration);
        if (!bBound || !CandidateStatement.Execute())
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: cannot insert candidate %s: %s"),
                *Candidate.RelativePath,
                *Database->GetLastError());
            return false;
        }
        CandidateStatement.Reset();
        CandidateStatement.ClearBindings();

        for (const FIndexedDependency& Dependency : Candidate.Dependencies)
        {
            const bool bDependencyBound =
                DependencyStatement.SetBindingValueByIndex(
                    1, FString(MHResourceKindLabel(Dependency.Owner.Kind))) &&
                DependencyStatement.SetBindingValueByIndex(2, Dependency.Owner.LogicalName) &&
                DependencyStatement.SetBindingValueByIndex(
                    3, FString(MHResourceKindLabel(Dependency.Target.Kind))) &&
                DependencyStatement.SetBindingValueByIndex(4, Dependency.Target.LogicalName) &&
                DependencyStatement.SetBindingValueByIndex(5, Dependency.Role) &&
                DependencyStatement.SetBindingValueByIndex(6, Dependency.OwnerPath);
            if (!bDependencyBound || !DependencyStatement.Execute())
            {
                OutError = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_INVALID: cannot insert dependency for %s: %s"),
                    *Candidate.RelativePath,
                    *Database->GetLastError());
                return false;
            }
            DependencyStatement.Reset();
            DependencyStatement.ClearBindings();
        }
    }
    return true;
}

bool FMHProjectResourceIndex::FImpl::InsertGeneratedRows(
    const TArray<FGeneratedRow>& Rows,
    const int64 SeenGeneration,
    FString& OutError) const
{
    FSQLitePreparedStatement Statement = Database->PrepareStatement(
        TEXT("INSERT INTO GeneratedAssets(object_path,kind,name,source_path,source_hash,applied_hash,carrier_kind,key_valid,receipt_valid,status,diagnostic,last_seen_generation) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,'invalid_receipt',?10,?11);"));
    if (!Statement.IsValid())
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: cannot prepare generated-asset insert: %s"),
            *Database->GetLastError());
        return false;
    }
    for (const FGeneratedRow& Row : Rows)
    {
        const bool bBound = Statement.SetBindingValueByIndex(1, Row.ObjectPath) &&
            Statement.SetBindingValueByIndex(2, Row.Kind) &&
            Statement.SetBindingValueByIndex(3, Row.Name) &&
            Statement.SetBindingValueByIndex(4, Row.SourcePath) &&
            Statement.SetBindingValueByIndex(5, Row.SourceHash) &&
            Statement.SetBindingValueByIndex(6, Row.AppliedHash) &&
            Statement.SetBindingValueByIndex(7, Row.CarrierKind) &&
            Statement.SetBindingValueByIndex(8, Row.bKeyValid ? 1 : 0) &&
            Statement.SetBindingValueByIndex(9, Row.bReceiptValid ? 1 : 0) &&
            Statement.SetBindingValueByIndex(10, Row.Diagnostic) &&
            Statement.SetBindingValueByIndex(11, SeenGeneration);
        if (!bBound || !Statement.Execute())
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: cannot insert generated asset %s: %s"),
                *Row.ObjectPath,
                *Database->GetLastError());
            return false;
        }
        Statement.Reset();
        Statement.ClearBindings();
    }
    return true;
}

bool FMHProjectResourceIndex::FImpl::RecomputeDerivedState(
    const FCandidateKeyTransitions& CandidateTransitions,
    FString& OutError)
{
    if (!Execute(TEXT("DELETE FROM ResourceKeys;"), OutError) ||
        !Execute(TEXT("DELETE FROM Diagnostics;"), OutError) ||
        !Execute(
            TEXT("INSERT INTO ResourceKeys(kind,name,resolution_status) "
                 "SELECT kind,name,CASE WHEN COUNT(*)>=2 THEN 'ambiguous' "
                 "WHEN MIN(parse_status)='ok' AND MAX(parse_status)='ok' THEN 'unique' "
                 "ELSE 'invalid' END FROM ResourceCandidates "
                 "WHERE kind IS NOT NULL AND name IS NOT NULL GROUP BY kind,name;"),
            OutError) ||
        !Execute(
            TEXT("INSERT OR IGNORE INTO ResourceKeys(kind,name,resolution_status) "
                 "SELECT target_kind,target_name,'missing' FROM Dependencies;"),
            OutError) ||
        !Execute(
            TEXT("INSERT OR IGNORE INTO ResourceKeys(kind,name,resolution_status) "
                 "SELECT kind,name,'missing' FROM GeneratedAssets WHERE key_valid=1;"),
            OutError))
    {
        return false;
    }

    struct FGeneratedStatusWork
    {
        FString ObjectPath;
        FString Kind;
        FString Name;
        FString SourceHash;
        bool bKeyValid = false;
        bool bReceiptValid = false;
    };
    TArray<FGeneratedStatusWork> Work;
    TMap<FString, int32> ClaimCounts;
    FSQLitePreparedStatement ReadAssets = Database->PrepareStatement(
        TEXT("SELECT object_path,kind,name,source_hash,key_valid,receipt_valid FROM GeneratedAssets ORDER BY object_path;"));
    if (!ReadAssets.IsValid())
    {
        OutError = Database->GetLastError();
        return false;
    }
    while (true)
    {
        const ESQLitePreparedStatementStepResult Step = ReadAssets.Step();
        if (Step == ESQLitePreparedStatementStepResult::Done) break;
        if (Step != ESQLitePreparedStatementStepResult::Row)
        {
            OutError = Database->GetLastError();
            return false;
        }
        FGeneratedStatusWork& Item = Work.AddDefaulted_GetRef();
        int32 KeyValid = 0;
        int32 ReceiptValid = 0;
        if (!ReadAssets.GetColumnValueByIndex(0, Item.ObjectPath) ||
            !ReadAssets.GetColumnValueByIndex(1, Item.Kind) ||
            !ReadAssets.GetColumnValueByIndex(2, Item.Name) ||
            !ReadAssets.GetColumnValueByIndex(3, Item.SourceHash) ||
            !ReadAssets.GetColumnValueByIndex(4, KeyValid) ||
            !ReadAssets.GetColumnValueByIndex(5, ReceiptValid))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: malformed generated-asset row");
            return false;
        }
        Item.bKeyValid = KeyValid != 0;
        Item.bReceiptValid = ReceiptValid != 0;
        if (Item.bKeyValid)
        {
            ++ClaimCounts.FindOrAdd(Item.Kind + TEXT(":") + Item.Name);
        }
    }

    FSQLitePreparedStatement ReadResource = Database->PrepareStatement(
        TEXT("SELECT resolution_status FROM ResourceKeys WHERE kind=?1 AND name=?2;"));
    FSQLitePreparedStatement ReadRawHash = Database->PrepareStatement(
        TEXT("SELECT raw_hash FROM ResourceCandidates WHERE kind=?1 AND name=?2 LIMIT 1;"));
    FSQLitePreparedStatement WriteStatus = Database->PrepareStatement(
        TEXT("UPDATE GeneratedAssets SET status=?1 WHERE object_path=?2;"));
    if (!ReadResource.IsValid() || !ReadRawHash.IsValid() || !WriteStatus.IsValid())
    {
        OutError = Database->GetLastError();
        return false;
    }

    TSet<FMHResourceKey> StaleKeys;
    for (const FGeneratedStatusWork& Item : Work)
    {
        EMHGeneratedAssetStatus Status = EMHGeneratedAssetStatus::InvalidReceipt;
        const FString SerializedKey = Item.Kind + TEXT(":") + Item.Name;
        if (Item.bKeyValid && ClaimCounts.FindRef(SerializedKey) > 1)
        {
            Status = EMHGeneratedAssetStatus::DuplicateClaim;
        }
        else if (!Item.bReceiptValid)
        {
            Status = EMHGeneratedAssetStatus::InvalidReceipt;
        }
        else
        {
            FString SourceStatus;
            if (!ReadResource.SetBindingValueByIndex(1, Item.Kind) ||
                !ReadResource.SetBindingValueByIndex(2, Item.Name))
            {
                OutError = Database->GetLastError();
                return false;
            }
            const ESQLitePreparedStatementStepResult ResourceStep = ReadResource.Step();
            if (ResourceStep != ESQLitePreparedStatementStepResult::Row ||
                !ReadResource.GetColumnValueByIndex(0, SourceStatus))
            {
                OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: generated key lacks ResourceKeys row");
                return false;
            }
            ReadResource.Reset();
            ReadResource.ClearBindings();
            if (SourceStatus == TEXT("missing"))
            {
                Status = EMHGeneratedAssetStatus::Orphan;
            }
            else if (SourceStatus == TEXT("ambiguous") || SourceStatus == TEXT("invalid"))
            {
                Status = EMHGeneratedAssetStatus::SourceBlocked;
            }
            else if (SourceStatus == TEXT("unique"))
            {
                FString CandidateHash;
                if (!ReadRawHash.SetBindingValueByIndex(1, Item.Kind) ||
                    !ReadRawHash.SetBindingValueByIndex(2, Item.Name) ||
                    ReadRawHash.Step() != ESQLitePreparedStatementStepResult::Row ||
                    !ReadRawHash.GetColumnValueByIndex(0, CandidateHash))
                {
                    OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: unique key lacks raw hash");
                    return false;
                }
                ReadRawHash.Reset();
                ReadRawHash.ClearBindings();
                Status = CandidateHash == Item.SourceHash
                    ? EMHGeneratedAssetStatus::Applied
                    : EMHGeneratedAssetStatus::Stale;
            }
            else
            {
                OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: unknown source resolution status");
                return false;
            }
        }
        if (!WriteStatus.SetBindingValueByIndex(
                1, FString(MHGeneratedAssetStatusLabel(Status))) ||
            !WriteStatus.SetBindingValueByIndex(2, Item.ObjectPath) ||
            !WriteStatus.Execute())
        {
            OutError = Database->GetLastError();
            return false;
        }
        WriteStatus.Reset();
        WriteStatus.ClearBindings();
        if (Status == EMHGeneratedAssetStatus::Stale)
        {
            FMHResourceKey StaleKey;
            StaleKey.LogicalName = Item.Name;
            if (MHResourceKindFromLabel(Item.Kind, StaleKey.Kind) && StaleKey.IsCanonical())
            {
                StaleKeys.Add(MoveTemp(StaleKey));
            }
        }
    }
    for (auto Iterator = PendingOrphanRebindDivergences.CreateIterator(); Iterator; ++Iterator)
    {
        if (!StaleKeys.Contains(*Iterator))
        {
            Iterator.RemoveCurrent();
        }
    }

    auto InsertDiagnostic = [this, &OutError](
        const FString& Severity,
        const FString& Code,
        const FString& OwnerKind,
        const FString& OwnerName,
        const FString& Path,
        const FString& TargetKind,
        const FString& TargetName,
        const FString& Message)
    {
        FSQLitePreparedStatement Statement = Database->PrepareStatement(
            TEXT("INSERT INTO Diagnostics(severity,code,owner_kind,owner_name,path,target_kind,target_name,message) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);"));
        const bool bOk = Statement.IsValid() &&
            Statement.SetBindingValueByIndex(1, Severity) &&
            Statement.SetBindingValueByIndex(2, Code) &&
            Statement.SetBindingValueByIndex(3, OwnerKind) &&
            Statement.SetBindingValueByIndex(4, OwnerName) &&
            Statement.SetBindingValueByIndex(5, Path) &&
            Statement.SetBindingValueByIndex(6, TargetKind) &&
            Statement.SetBindingValueByIndex(7, TargetName) &&
            Statement.SetBindingValueByIndex(8, Message) && Statement.Execute();
        if (!bOk)
        {
            OutError = Database->GetLastError();
        }
        return bOk;
    };

    FSQLitePreparedStatement CandidateDiagnostics = Database->PrepareStatement(
        TEXT("SELECT COALESCE(kind,''),COALESCE(name,''),path,diagnostic FROM ResourceCandidates WHERE parse_status<>'ok' ORDER BY path;"));
    if (!CandidateDiagnostics.IsValid())
    {
        OutError = Database->GetLastError();
        return false;
    }
    while (CandidateDiagnostics.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        FString Kind;
        FString Name;
        FString Path;
        FString Message;
        if (!CandidateDiagnostics.GetColumnValueByIndex(0, Kind) ||
            !CandidateDiagnostics.GetColumnValueByIndex(1, Name) ||
            !CandidateDiagnostics.GetColumnValueByIndex(2, Path) ||
            !CandidateDiagnostics.GetColumnValueByIndex(3, Message) ||
            !InsertDiagnostic(
                TEXT("error"), ExtractDiagnosticCode(Message), Kind, Name, Path,
                FString(), FString(), Message))
        {
            return false;
        }
    }

    FSQLitePreparedStatement DuplicateKeys = Database->PrepareStatement(
        TEXT("SELECT kind,name FROM ResourceKeys WHERE resolution_status='ambiguous' ORDER BY kind,name;"));
    while (DuplicateKeys.IsValid() && DuplicateKeys.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        FString Kind;
        FString Name;
        if (!DuplicateKeys.GetColumnValueByIndex(0, Kind) ||
            !DuplicateKeys.GetColumnValueByIndex(1, Name))
        {
            OutError = Database->GetLastError();
            return false;
        }
        const FString Message = FString::Printf(
            TEXT("MH_W_DUPLICATE_RESOURCE_NAME: %s:%s has multiple candidates"),
            *Kind, *Name);
        if (!InsertDiagnostic(
                TEXT("warning"), TEXT("MH_W_DUPLICATE_RESOURCE_NAME"),
                Kind, Name, FString(), FString(), FString(), Message))
        {
            return false;
        }
    }

    FSQLitePreparedStatement GeneratedDiagnostics = Database->PrepareStatement(
        TEXT("SELECT kind,name,object_path,status,diagnostic FROM GeneratedAssets WHERE status IN ('invalid_receipt','duplicate_claim') ORDER BY object_path;"));
    while (GeneratedDiagnostics.IsValid() &&
        GeneratedDiagnostics.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        FString Kind;
        FString Name;
        FString Path;
        FString Status;
        FString ExistingDiagnostic;
        if (!GeneratedDiagnostics.GetColumnValueByIndex(0, Kind) ||
            !GeneratedDiagnostics.GetColumnValueByIndex(1, Name) ||
            !GeneratedDiagnostics.GetColumnValueByIndex(2, Path) ||
            !GeneratedDiagnostics.GetColumnValueByIndex(3, Status) ||
            !GeneratedDiagnostics.GetColumnValueByIndex(4, ExistingDiagnostic))
        {
            OutError = Database->GetLastError();
            return false;
        }
        const FString Code = Status == TEXT("duplicate_claim")
            ? TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET")
            : TEXT("MH_E_SOURCE_INDEX_INVALID");
        const FString Message = Status == TEXT("duplicate_claim")
            ? FString::Printf(
                TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: multiple managed assets claim %s:%s"),
                *Kind, *Name)
            : ExistingDiagnostic;
        if (!InsertDiagnostic(
                TEXT("error"), Code, Kind, Name, Path,
                FString(), FString(), Message))
        {
            return false;
        }
    }

    struct FRenameEndpoint
    {
        FString Kind;
        FString Name;
        FString Hash;
    };
    TMultiMap<FString, FRenameEndpoint> OrphansByDomain;
    TMultiMap<FString, FRenameEndpoint> SourcesByDomain;
    FSQLitePreparedStatement Orphans = Database->PrepareStatement(
        TEXT("SELECT kind,name,source_hash FROM GeneratedAssets WHERE status='orphan' ORDER BY kind,name;"));
    while (Orphans.IsValid() && Orphans.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        FRenameEndpoint Value;
        if (!Orphans.GetColumnValueByIndex(0, Value.Kind) ||
            !Orphans.GetColumnValueByIndex(1, Value.Name) ||
            !Orphans.GetColumnValueByIndex(2, Value.Hash))
        {
            OutError = Database->GetLastError();
            return false;
        }
        FMHResourceKey Key;
        Key.LogicalName = Value.Name;
        if (MHResourceKindFromLabel(Value.Kind, Key.Kind) &&
            CandidateTransitions.Disappeared.Contains(Key))
        {
            OrphansByDomain.Add(Value.Kind + TEXT("|") + Value.Hash, Value);
        }
    }
    FSQLitePreparedStatement Sources = Database->PrepareStatement(
        TEXT("SELECT k.kind,k.name,c.raw_hash FROM ResourceKeys k "
             "JOIN ResourceCandidates c ON c.kind=k.kind AND c.name=k.name "
             "WHERE k.resolution_status='unique' AND NOT EXISTS ("
             "SELECT 1 FROM GeneratedAssets g WHERE g.key_valid=1 "
             "AND g.kind=k.kind AND g.name=k.name) ORDER BY k.kind,k.name;"));
    while (Sources.IsValid() && Sources.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        FRenameEndpoint Value;
        if (!Sources.GetColumnValueByIndex(0, Value.Kind) ||
            !Sources.GetColumnValueByIndex(1, Value.Name) ||
            !Sources.GetColumnValueByIndex(2, Value.Hash))
        {
            OutError = Database->GetLastError();
            return false;
        }
        FMHResourceKey Key;
        Key.LogicalName = Value.Name;
        if (MHResourceKindFromLabel(Value.Kind, Key.Kind) &&
            CandidateTransitions.Appeared.Contains(Key))
        {
            SourcesByDomain.Add(Value.Kind + TEXT("|") + Value.Hash, Value);
        }
    }
    TArray<FString> Domains;
    {
        TSet<FString> UniqueDomains;
        for (const TPair<FString, FRenameEndpoint>& Pair : OrphansByDomain)
        {
            UniqueDomains.Add(Pair.Key);
        }
        Domains = UniqueDomains.Array();
    }
    Domains.Sort();
    for (const FString& Domain : Domains)
    {
        TArray<FRenameEndpoint> Old;
        TArray<FRenameEndpoint> New;
        OrphansByDomain.MultiFind(Domain, Old);
        SourcesByDomain.MultiFind(Domain, New);
        New.RemoveAll([&Old](const FRenameEndpoint& Candidate)
        {
            return Old.ContainsByPredicate([&Candidate](const FRenameEndpoint& Orphan)
            {
                return Orphan.Kind == Candidate.Kind && Orphan.Name == Candidate.Name;
            });
        });
        if (Old.Num() == 1 && New.Num() == 1)
        {
            const FString Message = FString::Printf(
                TEXT("MH_W_PROBABLE_RESOURCE_RENAME: %s:%s -> %s:%s has matching raw hash"),
                *Old[0].Kind, *Old[0].Name, *New[0].Kind, *New[0].Name);
            if (!InsertDiagnostic(
                    TEXT("warning"), TEXT("MH_W_PROBABLE_RESOURCE_RENAME"),
                    Old[0].Kind, Old[0].Name, FString(),
                    New[0].Kind, New[0].Name, Message))
            {
                return false;
            }
        }
    }
    return true;
}

void FMHProjectResourceIndex::FImpl::ConsumeMatchingTokens(
    const TArray<FScannedCandidate>& Candidates,
    const int64 TokenGeneration,
    TArray<FString>& OutEvents)
{
    TArray<int32> Consumed;
    for (const FScannedCandidate& Candidate : Candidates)
    {
        if (Candidate.RawHash.IsEmpty()) continue;
        const FString Absolute = FPaths::ConvertRelativePathToFull(
            SourceRoot, Candidate.RelativePath);
        const int32 Match = SelfPublishTokens.IndexOfByPredicate(
            [&Absolute, &Candidate, TokenGeneration](const FSelfPublishToken& Token)
            {
                return Token.Generation == TokenGeneration &&
                    Token.AbsolutePath.Equals(Absolute, ESearchCase::IgnoreCase) &&
                    Token.RawHash == Candidate.RawHash;
            });
        if (Match != INDEX_NONE)
        {
            OutEvents.Add(FString::Printf(
                TEXT("SELF_PUBLISHED: %s"), *Candidate.RelativePath));
            Consumed.AddUnique(Match);
        }
    }
    Consumed.Sort([](const int32 A, const int32 B)
    {
        return A > B;
    });
    for (const int32 Index : Consumed)
    {
        SelfPublishTokens.RemoveAt(Index);
    }
}

FMHResolveOutcome FMHProjectResourceIndex::FImpl::Resolve(const FMHResourceKey& Key) const
{
    FMHResolveOutcome Outcome;
    if (!IsOpen())
    {
        Outcome.Status = EMHResolveStatus::Invalid;
        Outcome.Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: ProjectIndex is not open");
        return Outcome;
    }
    if (!Key.IsCanonical())
    {
        Outcome.Status = EMHResolveStatus::Invalid;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: noncanonical resource key %s"),
            *Key.ToString());
        return Outcome;
    }

    const FString Kind = MHResourceKindLabel(Key.Kind);
    FString ResolutionStatus;
    FSQLitePreparedStatement ReadKey = Database->PrepareStatement(
        TEXT("SELECT resolution_status FROM ResourceKeys WHERE kind=?1 AND name=?2;"));
    if (!ReadKey.IsValid() || !ReadKey.SetBindingValueByIndex(1, Kind) ||
        !ReadKey.SetBindingValueByIndex(2, Key.LogicalName))
    {
        Outcome.Status = EMHResolveStatus::Invalid;
        Outcome.Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot query ProjectIndex key");
        return Outcome;
    }
    const ESQLitePreparedStatementStepResult KeyStep = ReadKey.Step();
    if (KeyStep == ESQLitePreparedStatementStepResult::Done)
    {
        Outcome.Status = EMHResolveStatus::Unresolved;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: no source payload for %s"),
            *Key.ToString());
        return Outcome;
    }
    if (KeyStep != ESQLitePreparedStatementStepResult::Row ||
        !ReadKey.GetColumnValueByIndex(0, ResolutionStatus))
    {
        Outcome.Status = EMHResolveStatus::Invalid;
        Outcome.Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot read ProjectIndex key status");
        return Outcome;
    }

    FSQLitePreparedStatement ReadCandidates = Database->PrepareStatement(
        TEXT("SELECT path,raw_hash,parse_status,diagnostic FROM ResourceCandidates WHERE kind=?1 AND name=?2 ORDER BY path;"));
    if (!ReadCandidates.IsValid() ||
        !ReadCandidates.SetBindingValueByIndex(1, Kind) ||
        !ReadCandidates.SetBindingValueByIndex(2, Key.LogicalName))
    {
        Outcome.Status = EMHResolveStatus::Invalid;
        Outcome.Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot query resource candidates");
        return Outcome;
    }
    FString CandidateParseStatus;
    FString CandidateDiagnostic;
    while (true)
    {
        const ESQLitePreparedStatementStepResult Step = ReadCandidates.Step();
        if (Step == ESQLitePreparedStatementStepResult::Done) break;
        if (Step != ESQLitePreparedStatementStepResult::Row)
        {
            Outcome.Status = EMHResolveStatus::Invalid;
            Outcome.Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot enumerate resource candidates");
            return Outcome;
        }
        FString RelativePath;
        FString RawHash;
        if (!ReadCandidates.GetColumnValueByIndex(0, RelativePath) ||
            !ReadNullableString(ReadCandidates, 1, RawHash) ||
            !ReadCandidates.GetColumnValueByIndex(2, CandidateParseStatus) ||
            !ReadCandidates.GetColumnValueByIndex(3, CandidateDiagnostic))
        {
            Outcome.Status = EMHResolveStatus::Invalid;
            Outcome.Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: malformed resource candidate row");
            return Outcome;
        }
        FString Absolute = FPaths::ConvertRelativePathToFull(SourceRoot, RelativePath);
        FPaths::NormalizeFilename(Absolute);
        Outcome.CandidatePaths.Add(Absolute);
        if (Outcome.CandidatePaths.Num() == 1)
        {
            Outcome.PayloadPath = Absolute;
            Outcome.RawHash = RawHash == TEXT("~") ? FString() : RawHash;
        }
    }

    if (ResolutionStatus == TEXT("missing"))
    {
        Outcome.Status = EMHResolveStatus::Unresolved;
        Outcome.PayloadPath.Reset();
        Outcome.RawHash.Reset();
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: no source payload for %s"),
            *Key.ToString());
        return Outcome;
    }
    if (ResolutionStatus == TEXT("ambiguous"))
    {
        Outcome.Status = EMHResolveStatus::Ambiguous;
        Outcome.PayloadPath.Reset();
        Outcome.RawHash.Reset();
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME: %s has %d candidates"),
            *Key.ToString(), Outcome.CandidatePaths.Num());
        return Outcome;
    }
    if (ResolutionStatus == TEXT("invalid"))
    {
        Outcome.Status = EMHResolveStatus::Invalid;
        Outcome.PayloadPath.Reset();
        Outcome.RawHash.Reset();
        Outcome.Diagnostic = CandidateDiagnostic.IsEmpty()
            ? FString::Printf(TEXT("MH_E_SOURCE_INDEX_INVALID: invalid source payload for %s"), *Key.ToString())
            : CandidateDiagnostic;
        return Outcome;
    }
    if (ResolutionStatus != TEXT("unique") || Outcome.CandidatePaths.Num() != 1 ||
        CandidateParseStatus != TEXT("ok") || Outcome.RawHash.IsEmpty())
    {
        Outcome.Status = EMHResolveStatus::Invalid;
        Outcome.Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: inconsistent unique resource key");
        return Outcome;
    }

    FString BlockingDiagnostic;
    if (IsImportBlocked(Key, BlockingDiagnostic))
    {
        Outcome.Status = EMHResolveStatus::Invalid;
        Outcome.PayloadPath.Reset();
        Outcome.RawHash.Reset();
        Outcome.Diagnostic = MoveTemp(BlockingDiagnostic);
        return Outcome;
    }
    Outcome.Status = EMHResolveStatus::Resolved;
    return Outcome;
}

bool FMHProjectResourceIndex::FImpl::IsImportBlocked(
    const FMHResourceKey& Key,
    FString& OutDiagnostic) const
{
    OutDiagnostic.Reset();
    if (!IsOpen() || !Key.IsCanonical())
    {
        OutDiagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: invalid ProjectIndex resolve request");
        return true;
    }

    TSet<FMHResourceKey> Visiting;
    TMap<FMHResourceKey, bool> Memo;
    TMap<FMHResourceKey, FString> MemoDiagnostics;
    TFunction<bool(const FMHResourceKey&, FString&)> Visit;
    Visit = [this, &Visiting, &Memo, &MemoDiagnostics, &Visit](
            const FMHResourceKey& Current,
            FString& Diagnostic)
    {
        if (const bool* Cached = Memo.Find(Current))
        {
            Diagnostic = MemoDiagnostics.FindRef(Current);
            return *Cached;
        }
        if (Visiting.Contains(Current))
        {
            Diagnostic = FString::Printf(
                TEXT("MH_E_COMPOSITE_CYCLE: dependency cycle reaches %s"),
                *Current.ToString());
            return true;
        }
        Visiting.Add(Current);
        const FString Kind = MHResourceKindLabel(Current.Kind);

        FString Status;
        FSQLitePreparedStatement KeyStatement = Database->PrepareStatement(
            TEXT("SELECT resolution_status FROM ResourceKeys WHERE kind=?1 AND name=?2;"));
        if (!KeyStatement.IsValid() ||
            !KeyStatement.SetBindingValueByIndex(1, Kind) ||
            !KeyStatement.SetBindingValueByIndex(2, Current.LogicalName) ||
            KeyStatement.Step() != ESQLitePreparedStatementStepResult::Row ||
            !KeyStatement.GetColumnValueByIndex(0, Status))
        {
            Diagnostic = FString::Printf(
                TEXT("MH_E_RESOURCE_NOT_FOUND: no source payload for %s"),
                *Current.ToString());
        }
        else if (Status == TEXT("ambiguous"))
        {
            Diagnostic = FString::Printf(
                TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME: %s has multiple candidates"),
                *Current.ToString());
        }
        else if (Status == TEXT("invalid"))
        {
            Diagnostic = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: invalid source payload for %s"),
                *Current.ToString());
        }
        else if (Status == TEXT("missing"))
        {
            Diagnostic = FString::Printf(
                TEXT("MH_E_RESOURCE_NOT_FOUND: no source payload for %s"),
                *Current.ToString());
        }
        else if (Status != TEXT("unique"))
        {
            Diagnostic = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: unknown resolution status for %s"),
                *Current.ToString());
        }

        if (Diagnostic.IsEmpty())
        {
            FSQLitePreparedStatement GeneratedStatement = Database->PrepareStatement(
                TEXT("SELECT status FROM GeneratedAssets WHERE kind=?1 AND name=?2 ORDER BY object_path;"));
            if (!GeneratedStatement.IsValid() ||
                !GeneratedStatement.SetBindingValueByIndex(1, Kind) ||
                !GeneratedStatement.SetBindingValueByIndex(2, Current.LogicalName))
            {
                Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot inspect generated asset claims");
            }
            else
            {
                while (GeneratedStatement.Step() == ESQLitePreparedStatementStepResult::Row)
                {
                    FString GeneratedStatus;
                    if (!GeneratedStatement.GetColumnValueByIndex(0, GeneratedStatus))
                    {
                        Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: malformed generated asset status");
                        break;
                    }
                    if (GeneratedStatus == TEXT("duplicate_claim"))
                    {
                        Diagnostic = FString::Printf(
                            TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: multiple managed assets claim %s"),
                            *Current.ToString());
                        break;
                    }
                    if (GeneratedStatus == TEXT("invalid_receipt"))
                    {
                        Diagnostic = FString::Printf(
                            TEXT("MH_E_SOURCE_INDEX_INVALID: invalid managed asset receipt for %s"),
                            *Current.ToString());
                        break;
                    }
                }
            }
        }

        if (Diagnostic.IsEmpty())
        {
            FSQLitePreparedStatement DependencyStatement = Database->PrepareStatement(
                TEXT("SELECT DISTINCT target_kind,target_name,role FROM Dependencies WHERE owner_kind=?1 AND owner_name=?2 ORDER BY target_kind,target_name,role;"));
            if (!DependencyStatement.IsValid() ||
                !DependencyStatement.SetBindingValueByIndex(1, Kind) ||
                !DependencyStatement.SetBindingValueByIndex(2, Current.LogicalName))
            {
                Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot inspect source dependencies");
            }
            else
            {
                while (DependencyStatement.Step() == ESQLitePreparedStatementStepResult::Row)
                {
                    FString TargetKindLabel;
                    FString Role;
                    FMHResourceKey Target;
                    if (!DependencyStatement.GetColumnValueByIndex(0, TargetKindLabel) ||
                        !DependencyStatement.GetColumnValueByIndex(1, Target.LogicalName) ||
                        !DependencyStatement.GetColumnValueByIndex(2, Role) ||
                        !MHResourceKindFromLabel(TargetKindLabel, Target.Kind))
                    {
                        Diagnostic = TEXT("MH_E_SOURCE_INDEX_INVALID: malformed dependency target");
                        break;
                    }
                    FString TargetDiagnostic;
                    if (Visit(Target, TargetDiagnostic))
                    {
                        if (Current.Kind == EMHResourceKind::StaticMesh && Role == TEXT("slot"))
                        {
                            Diagnostic = FString::Printf(
                                TEXT("MH_E_UNRESOLVED_MATERIAL_REFERENCE: slot '%s' required by %s is blocked (%s)"),
                                *Target.LogicalName,
                                *Current.ToString(),
                                *TargetDiagnostic);
                        }
                        else
                        {
                            Diagnostic = FString::Printf(
                                TEXT("%s; dependent %s is blocked"),
                                *TargetDiagnostic,
                                *Current.ToString());
                        }
                        break;
                    }
                }
            }
        }

        Visiting.Remove(Current);
        const bool bBlocked = !Diagnostic.IsEmpty();
        Memo.Add(Current, bBlocked);
        MemoDiagnostics.Add(Current, Diagnostic);
        return bBlocked;
    };

    return Visit(Key, OutDiagnostic);
}

FMHSourceSnapshot FMHProjectResourceIndex::FImpl::GetSnapshot() const
{
    FMHSourceSnapshot Snapshot;
    if (!IsOpen())
    {
        Snapshot.Errors.Add(TEXT("MH_E_SOURCE_INDEX_INVALID: ProjectIndex is not open"));
        return Snapshot;
    }
    FSQLitePreparedStatement Keys = Database->PrepareStatement(
        TEXT("SELECT kind,name FROM ResourceKeys ORDER BY kind,name;"));
    while (Keys.IsValid() && Keys.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        FString Kind;
        FMHResourceKey Key;
        if (Keys.GetColumnValueByIndex(0, Kind) &&
            Keys.GetColumnValueByIndex(1, Key.LogicalName) &&
            MHResourceKindFromLabel(Kind, Key.Kind))
        {
            Snapshot.ResourceKeys.Add(MoveTemp(Key));
        }
    }
    Snapshot.ResourceKeys.Sort(ResourceKeyLess);

    FSQLitePreparedStatement Diagnostics = Database->PrepareStatement(
        TEXT("SELECT severity,message FROM Diagnostics ORDER BY severity,code,owner_kind,owner_name,path,target_kind,target_name,message;"));
    while (Diagnostics.IsValid() &&
        Diagnostics.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        FString Severity;
        FString Message;
        if (Diagnostics.GetColumnValueByIndex(0, Severity) &&
            Diagnostics.GetColumnValueByIndex(1, Message))
        {
            (Severity == TEXT("warning") ? Snapshot.Warnings : Snapshot.Errors).Add(Message);
        }
    }
    FSQLitePreparedStatement Quarantine = Database->PrepareStatement(
        TEXT("SELECT path,diagnostic FROM ResourceCandidates WHERE parse_status<>'ok' ORDER BY path;"));
    while (Quarantine.IsValid() &&
        Quarantine.Step() == ESQLitePreparedStatementStepResult::Row)
    {
        FMHSourceQuarantine& Entry = Snapshot.Quarantined.AddDefaulted_GetRef();
        FString RelativePath;
        if (Quarantine.GetColumnValueByIndex(0, RelativePath) &&
            Quarantine.GetColumnValueByIndex(1, Entry.Diagnostic))
        {
            Entry.PayloadPath = FPaths::ConvertRelativePathToFull(SourceRoot, RelativePath);
            FPaths::NormalizeFilename(Entry.PayloadPath);
        }
    }
    Snapshot.Warnings.Sort();
    Snapshot.Errors.Sort();
    return Snapshot;
}

bool FMHProjectResourceIndex::FImpl::GetGeneratedAssets(
    const FMHResourceKey& Key,
    TArray<FMHProjectIndexGeneratedAssetState>& OutAssets,
    FString& OutError) const
{
    OutAssets.Reset();
    OutError.Reset();
    if (!EnsureOpen(OutError) || !Key.IsCanonical())
    {
        if (OutError.IsEmpty()) OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: invalid generated-asset query");
        return false;
    }
    FSQLitePreparedStatement Statement = Database->PrepareStatement(
        TEXT("SELECT kind,name,object_path,source_path,source_hash,applied_hash,status,key_valid,receipt_valid FROM GeneratedAssets WHERE kind=?1 AND name=?2 ORDER BY object_path;"));
    if (!Statement.IsValid() ||
        !Statement.SetBindingValueByIndex(1, FString(MHResourceKindLabel(Key.Kind))) ||
        !Statement.SetBindingValueByIndex(2, Key.LogicalName))
    {
        OutError = Database->GetLastError();
        return false;
    }
    while (true)
    {
        const ESQLitePreparedStatementStepResult Step = Statement.Step();
        if (Step == ESQLitePreparedStatementStepResult::Done) break;
        if (Step != ESQLitePreparedStatementStepResult::Row)
        {
            OutError = Database->GetLastError();
            return false;
        }
        FMHProjectIndexGeneratedAssetState& State = OutAssets.AddDefaulted_GetRef();
        FString Status;
        int32 KeyValid = 0;
        int32 ReceiptValid = 0;
        if (!Statement.GetColumnValueByIndex(0, State.KindLabel) ||
            !Statement.GetColumnValueByIndex(1, State.LogicalName) ||
            !Statement.GetColumnValueByIndex(2, State.UEObjectPath) ||
            !Statement.GetColumnValueByIndex(3, State.SourcePath) ||
            !Statement.GetColumnValueByIndex(4, State.SourceHash) ||
            !Statement.GetColumnValueByIndex(5, State.AppliedHash) ||
            !Statement.GetColumnValueByIndex(6, Status) ||
            !Statement.GetColumnValueByIndex(7, KeyValid) ||
            !Statement.GetColumnValueByIndex(8, ReceiptValid))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: malformed GeneratedAssets row");
            return false;
        }
        State.Key = Key;
        State.bKeyValid = KeyValid != 0;
        State.bReceiptValid = ReceiptValid != 0;
        if (Status == TEXT("applied")) State.Status = EMHGeneratedAssetStatus::Applied;
        else if (Status == TEXT("stale")) State.Status = EMHGeneratedAssetStatus::Stale;
        else if (Status == TEXT("orphan")) State.Status = EMHGeneratedAssetStatus::Orphan;
        else if (Status == TEXT("source_blocked")) State.Status = EMHGeneratedAssetStatus::SourceBlocked;
        else if (Status == TEXT("duplicate_claim")) State.Status = EMHGeneratedAssetStatus::DuplicateClaim;
        else State.Status = EMHGeneratedAssetStatus::InvalidReceipt;
    }
    return true;
}

bool FMHProjectResourceIndex::FImpl::GetPlacementProfileDependencies(
    const FMHResourceKey& CompositeKey,
    TArray<FMHResourceKey>& OutProfiles,
    FString& OutError) const
{
    OutProfiles.Reset();
    OutError.Reset();
    if (!EnsureOpen(OutError) || CompositeKey.Kind != EMHResourceKind::Composite ||
        !CompositeKey.IsCanonical())
    {
        if (OutError.IsEmpty())
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: invalid composite profile-dependency query");
        }
        return false;
    }
    FSQLitePreparedStatement Statement = Database->PrepareStatement(
        TEXT("SELECT DISTINCT target_name FROM Dependencies ")
        TEXT("WHERE owner_kind='composite' AND owner_name=?1 ")
        TEXT("AND target_kind='placement_profile' AND role='profile' ")
        TEXT("ORDER BY target_name;"));
    if (!Statement.IsValid() ||
        !Statement.SetBindingValueByIndex(1, CompositeKey.LogicalName))
    {
        OutError = Database->GetLastError();
        return false;
    }
    while (true)
    {
        const ESQLitePreparedStatementStepResult Step = Statement.Step();
        if (Step == ESQLitePreparedStatementStepResult::Done)
        {
            return true;
        }
        FMHResourceKey& Profile = OutProfiles.AddDefaulted_GetRef();
        Profile.Kind = EMHResourceKind::PlacementProfile;
        if (Step != ESQLitePreparedStatementStepResult::Row ||
            !Statement.GetColumnValueByIndex(0, Profile.LogicalName) ||
            !Profile.IsCanonical())
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: malformed composite profile dependency");
            return false;
        }
    }
}

bool FMHProjectResourceIndex::FImpl::GetAllGeneratedAssets(
    TArray<FMHProjectIndexGeneratedAssetState>& OutAssets,
    FString& OutError) const
{
    OutAssets.Reset();
    OutError.Reset();
    if (!EnsureOpen(OutError))
    {
        return false;
    }
    FSQLitePreparedStatement Statement = Database->PrepareStatement(
        TEXT("SELECT kind,name,object_path,source_path,source_hash,applied_hash,status,key_valid,receipt_valid FROM GeneratedAssets ORDER BY object_path;"));
    if (!Statement.IsValid())
    {
        OutError = Database->GetLastError();
        return false;
    }
    while (true)
    {
        const ESQLitePreparedStatementStepResult Step = Statement.Step();
        if (Step == ESQLitePreparedStatementStepResult::Done) break;
        if (Step != ESQLitePreparedStatementStepResult::Row)
        {
            OutError = Database->GetLastError();
            return false;
        }

        FMHProjectIndexGeneratedAssetState& State = OutAssets.AddDefaulted_GetRef();
        FString Status;
        int32 KeyValid = 0;
        int32 ReceiptValid = 0;
        if (!Statement.GetColumnValueByIndex(0, State.KindLabel) ||
            !Statement.GetColumnValueByIndex(1, State.LogicalName) ||
            !Statement.GetColumnValueByIndex(2, State.UEObjectPath) ||
            !Statement.GetColumnValueByIndex(3, State.SourcePath) ||
            !Statement.GetColumnValueByIndex(4, State.SourceHash) ||
            !Statement.GetColumnValueByIndex(5, State.AppliedHash) ||
            !Statement.GetColumnValueByIndex(6, Status) ||
            !Statement.GetColumnValueByIndex(7, KeyValid) ||
            !Statement.GetColumnValueByIndex(8, ReceiptValid))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: malformed GeneratedAssets row");
            return false;
        }
        State.bKeyValid = KeyValid != 0;
        State.bReceiptValid = ReceiptValid != 0;
        State.Key.LogicalName = State.LogicalName;
        const bool bParsedKind = MHResourceKindFromLabel(State.KindLabel, State.Key.Kind);
        if (State.bKeyValid && (!bParsedKind || !State.Key.IsCanonical()))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: GeneratedAssets valid key cannot be decoded");
            return false;
        }
        if (Status == TEXT("applied")) State.Status = EMHGeneratedAssetStatus::Applied;
        else if (Status == TEXT("stale")) State.Status = EMHGeneratedAssetStatus::Stale;
        else if (Status == TEXT("orphan")) State.Status = EMHGeneratedAssetStatus::Orphan;
        else if (Status == TEXT("source_blocked")) State.Status = EMHGeneratedAssetStatus::SourceBlocked;
        else if (Status == TEXT("duplicate_claim")) State.Status = EMHGeneratedAssetStatus::DuplicateClaim;
        else if (Status == TEXT("invalid_receipt")) State.Status = EMHGeneratedAssetStatus::InvalidReceipt;
        else
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: unknown GeneratedAssets status");
            return false;
        }
    }
    return true;
}

bool FMHProjectResourceIndex::FImpl::GetDiagnostics(
    TArray<FMHProjectIndexDiagnostic>& OutDiagnostics,
    FString& OutError) const
{
    OutDiagnostics.Reset();
    OutError.Reset();
    if (!EnsureOpen(OutError))
    {
        return false;
    }
    FSQLitePreparedStatement Statement = Database->PrepareStatement(
        TEXT("SELECT severity,code,owner_kind,owner_name,path,target_kind,target_name,message FROM Diagnostics ORDER BY severity,code,owner_kind,owner_name,path,target_kind,target_name,message;"));
    if (!Statement.IsValid())
    {
        OutError = Database->GetLastError();
        return false;
    }
    while (true)
    {
        const ESQLitePreparedStatementStepResult Step = Statement.Step();
        if (Step == ESQLitePreparedStatementStepResult::Done) break;
        if (Step != ESQLitePreparedStatementStepResult::Row)
        {
            OutError = Database->GetLastError();
            return false;
        }

        FMHProjectIndexDiagnostic& Diagnostic = OutDiagnostics.AddDefaulted_GetRef();
        FString Severity;
        if (!Statement.GetColumnValueByIndex(0, Severity) ||
            !Statement.GetColumnValueByIndex(1, Diagnostic.Code) ||
            !ReadNullableString(Statement, 2, Diagnostic.OwnerKind) ||
            !ReadNullableString(Statement, 3, Diagnostic.OwnerName) ||
            !ReadNullableString(Statement, 4, Diagnostic.Path) ||
            !ReadNullableString(Statement, 5, Diagnostic.TargetKind) ||
            !ReadNullableString(Statement, 6, Diagnostic.TargetName) ||
            !Statement.GetColumnValueByIndex(7, Diagnostic.Message))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: malformed Diagnostics row");
            return false;
        }
        if (Severity == TEXT("warning"))
        {
            Diagnostic.Severity = EMHProjectDiagnosticSeverity::Warning;
        }
        else if (Severity == TEXT("error"))
        {
            Diagnostic.Severity = EMHProjectDiagnosticSeverity::Error;
        }
        else
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: unknown Diagnostics severity");
            return false;
        }
    }
    return true;
}

bool FMHProjectResourceIndex::FImpl::BuildNormalizedDump(
    FString& OutDump,
    FString& OutError) const
{
    OutDump.Reset();
    OutError.Reset();
    if (!EnsureOpen(OutError))
    {
        return false;
    }
    struct FDumpQuery
    {
        const TCHAR* Table;
        const TCHAR* Sql;
        int32 Columns;
    };
    const FDumpQuery Queries[] = {
        {TEXT("ResourceCandidates"), TEXT("SELECT path,kind,name,CAST(size AS TEXT),raw_hash,parse_status,diagnostic FROM ResourceCandidates ORDER BY path;"), 7},
        {TEXT("ResourceKeys"), TEXT("SELECT kind,name,resolution_status FROM ResourceKeys ORDER BY kind,name;"), 3},
        {TEXT("Dependencies"), TEXT("SELECT owner_kind,owner_name,target_kind,target_name,role,owner_path FROM Dependencies ORDER BY owner_kind,owner_name,target_kind,target_name,role,owner_path;"), 6},
        {TEXT("GeneratedAssets"), TEXT("SELECT object_path,kind,name,source_path,source_hash,applied_hash,carrier_kind,CAST(key_valid AS TEXT),CAST(receipt_valid AS TEXT),status,diagnostic FROM GeneratedAssets ORDER BY object_path;"), 11},
        {TEXT("Diagnostics"), TEXT("SELECT severity,code,owner_kind,owner_name,path,target_kind,target_name,message FROM Diagnostics ORDER BY severity,code,owner_kind,owner_name,path,target_kind,target_name,message;"), 8}};
    for (const FDumpQuery& Query : Queries)
    {
        FSQLitePreparedStatement Statement = Database->PrepareStatement(Query.Sql);
        if (!Statement.IsValid())
        {
            OutError = Database->GetLastError();
            return false;
        }
        while (true)
        {
            const ESQLitePreparedStatementStepResult Step = Statement.Step();
            if (Step == ESQLitePreparedStatementStepResult::Done) break;
            if (Step != ESQLitePreparedStatementStepResult::Row)
            {
                OutError = Database->GetLastError();
                return false;
            }
            OutDump += Query.Table;
            for (int32 Column = 0; Column < Query.Columns; ++Column)
            {
                FString Value;
                if (!ReadNullableString(Statement, Column, Value))
                {
                    OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot build normalized dump");
                    return false;
                }
                OutDump += TEXT("\t") + EscapeDumpField(MoveTemp(Value));
            }
            OutDump += TEXT("\n");
        }
    }
    return true;
}

FString FMHProjectResourceIndex::DefaultDatabasePath()
{
    return FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirBridge"),
        TEXT("ProjectIndex.sqlite"));
}

FMHProjectResourceIndex::FMHProjectResourceIndex(
    FString InSourceRoot,
    FString InDatabasePath)
    : Impl(MakeUnique<FImpl>(MoveTemp(InSourceRoot), MoveTemp(InDatabasePath)))
{
}

FMHProjectResourceIndex::~FMHProjectResourceIndex()
{
    Impl->Close();
}

bool FMHProjectResourceIndex::Open(bool& bOutRecreated, FString& OutError)
{
    return Impl->Open(bOutRecreated, OutError);
}

void FMHProjectResourceIndex::Close()
{
    Impl->Close();
}

bool FMHProjectResourceIndex::IsOpen() const
{
    return Impl->IsOpen();
}

bool FMHProjectResourceIndex::FullScan(
    const TArray<FMHGeneratedAssetTagClaim>& GeneratedAssets,
    FMHProjectIndexUpdateResult& OutResult,
    FString& OutError)
{
    return Impl->FullScan(GeneratedAssets, OutResult, OutError);
}

bool FMHProjectResourceIndex::UpsertPaths(
    const TArray<FString>& Paths,
    FMHProjectIndexUpdateResult& OutResult,
    FString& OutError)
{
    return Impl->UpsertPaths(Paths, OutResult, OutError);
}

bool FMHProjectResourceIndex::ReplaceGeneratedAssets(
    const TArray<FMHGeneratedAssetTagClaim>& GeneratedAssets,
    FMHProjectIndexUpdateResult& OutResult,
    FString& OutError)
{
    return Impl->ReplaceGeneratedAssets(GeneratedAssets, OutResult, OutError);
}

bool FMHProjectResourceIndex::RegisterSelfPublishAfterReplace(
    const FString& Path,
    const FString& RawHash,
    FString& OutError)
{
    return Impl->RegisterSelfPublishAfterReplace(Path, RawHash, OutError);
}

bool FMHProjectResourceIndex::ConsumeOrphanRebindEvent(
    const FMHResourceKey& Key,
    FString& OutEvent)
{
    return Impl->ConsumeOrphanRebindEvent(Key, OutEvent);
}

FMHResolveOutcome FMHProjectResourceIndex::Resolve(const FMHResourceKey& Key) const
{
    return Impl->Resolve(Key);
}

FMHSourceSnapshot FMHProjectResourceIndex::GetSnapshot() const
{
    return Impl->GetSnapshot();
}

bool FMHProjectResourceIndex::IsImportBlocked(
    const FMHResourceKey& Key,
    FString& OutDiagnostic) const
{
    return Impl->IsImportBlocked(Key, OutDiagnostic);
}

bool FMHProjectResourceIndex::GetGeneratedAssets(
    const FMHResourceKey& Key,
    TArray<FMHProjectIndexGeneratedAssetState>& OutAssets,
    FString& OutError) const
{
    return Impl->GetGeneratedAssets(Key, OutAssets, OutError);
}

bool FMHProjectResourceIndex::GetPlacementProfileDependencies(
    const FMHResourceKey& CompositeKey,
    TArray<FMHResourceKey>& OutProfiles,
    FString& OutError) const
{
    return Impl->GetPlacementProfileDependencies(CompositeKey, OutProfiles, OutError);
}

bool FMHProjectResourceIndex::GetAllGeneratedAssets(
    TArray<FMHProjectIndexGeneratedAssetState>& OutAssets,
    FString& OutError) const
{
    return Impl->GetAllGeneratedAssets(OutAssets, OutError);
}

bool FMHProjectResourceIndex::GetDiagnostics(
    TArray<FMHProjectIndexDiagnostic>& OutDiagnostics,
    FString& OutError) const
{
    return Impl->GetDiagnostics(OutDiagnostics, OutError);
}

bool FMHProjectResourceIndex::BuildNormalizedDump(
    FString& OutDump,
    FString& OutError) const
{
    return Impl->BuildNormalizedDump(OutDump, OutError);
}

int64 FMHProjectResourceIndex::GetGeneration() const
{
    return Impl->Generation;
}

int32 FMHProjectResourceIndex::GetFullScanCountForTests() const
{
    return Impl->FullScanCount;
}

const FString& FMHProjectResourceIndex::GetSourceRoot() const
{
    return Impl->SourceRoot;
}

const FString& FMHProjectResourceIndex::GetDatabasePath() const
{
    return Impl->DatabasePath;
}

#if WITH_DEV_AUTOMATION_TESTS
bool FMHProjectResourceIndex::InjectResolutionStatusForTests(
    const FMHResourceKey& Key,
    const FString& Status,
    FString& OutError)
{
    return Impl->InjectResolutionStatusForTests(Key, Status, OutError);
}

bool FMHProjectResourceIndex::InjectExtraSchemaColumnForTests(FString& OutError)
{
    return Impl->InjectExtraSchemaColumnForTests(OutError);
}
#endif

FMHSourceSnapshot FMHProjectIndexResolver::GetSnapshot() const
{
    return Index.GetSnapshot();
}

FMHResolveOutcome FMHProjectIndexResolver::Resolve(const FMHResourceKey& Key)
{
    return Index.Resolve(Key);
}

void FMHProjectIndexChangeDetector::DetectChanges(
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis)
{
    OutAnalysis = FMHSourceAnalysis();
    const FMHSourceSnapshot Snapshot = Resolver.GetSnapshot();
    OutAnalysis.Warnings = Snapshot.Warnings;
    OutAnalysis.Errors = Snapshot.Errors;

    TArray<FMHResourceKey> Keys = Snapshot.ResourceKeys;
    Keys.Sort(ResourceKeyLess);
    FString NormalizedRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(NormalizedRoot);
    NormalizedRoot += TEXT("/");

    for (const FMHResourceKey& Key : Keys)
    {
        FMHSourceAnalysisEntry& Entry = OutAnalysis.Entries.AddDefaulted_GetRef();
        Entry.Key = Key;
        const FMHResolveOutcome Outcome = Resolver.Resolve(Key);
        TArray<FMHProjectIndexGeneratedAssetState> Assets;
        FString AssetError;
        if (!Index.GetGeneratedAssets(Key, Assets, AssetError))
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(AssetError);
            continue;
        }
        if (Assets.ContainsByPredicate([](const FMHProjectIndexGeneratedAssetState& Asset)
            {
                return Asset.Status == EMHGeneratedAssetStatus::DuplicateClaim;
            }))
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: multiple managed assets claim %s"),
                *Key.ToString()));
            continue;
        }
        if (Assets.ContainsByPredicate([](const FMHProjectIndexGeneratedAssetState& Asset)
            {
                return Asset.Status == EMHGeneratedAssetStatus::InvalidReceipt;
            }))
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: invalid managed asset receipt for %s"),
                *Key.ToString()));
            continue;
        }

        if (Outcome.Status != EMHResolveStatus::Resolved)
        {
            if (Assets.Num() == 1 &&
                Assets[0].Status == EMHGeneratedAssetStatus::Orphan)
            {
                Entry.Change = EMHSourceChange::Remove;
            }
            else
            {
                Entry.Change = EMHSourceChange::Blocked;
                Entry.Errors.Add(Outcome.Diagnostic);
            }
            continue;
        }

        Entry.PayloadPath = Outcome.PayloadPath;
        Entry.RawHash = Outcome.RawHash;
        Entry.SourcePath = Outcome.PayloadPath;
        if (!FPaths::MakePathRelativeTo(Entry.SourcePath, *NormalizedRoot) ||
            !IsValidRelativeSourcePath(Entry.SourcePath))
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: resolved source path escapes source_root"));
            continue;
        }
        FPaths::NormalizeFilename(Entry.SourcePath);

        if (Assets.IsEmpty())
        {
            Entry.Change = Key.Kind == EMHResourceKind::PlacementProfile
                ? EMHSourceChange::NoChange
                : EMHSourceChange::Create;
            continue;
        }
        if (Assets.Num() != 1)
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: multiple managed assets claim %s"),
                *Key.ToString()));
            continue;
        }
        const FMHProjectIndexGeneratedAssetState& Asset = Assets[0];
        switch (Asset.Status)
        {
        case EMHGeneratedAssetStatus::Applied:
            // A path-only source move changes the index candidate path, not
            // the applied UObject or its receipt.
            Entry.Change = EMHSourceChange::NoChange;
            break;
        case EMHGeneratedAssetStatus::Stale:
            Entry.Change = EMHSourceChange::Reimport;
            break;
        case EMHGeneratedAssetStatus::Orphan:
            Entry.Change = EMHSourceChange::Create;
            break;
        case EMHGeneratedAssetStatus::SourceBlocked:
        case EMHGeneratedAssetStatus::InvalidReceipt:
        case EMHGeneratedAssetStatus::DuplicateClaim:
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: generated asset state %s blocks %s"),
                MHGeneratedAssetStatusLabel(Asset.Status),
                *Key.ToString()));
            break;
        }
    }
}

} // namespace UE::MimirComposite
