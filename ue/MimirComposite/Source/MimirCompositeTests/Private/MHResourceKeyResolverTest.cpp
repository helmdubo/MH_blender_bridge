#include "Source/MHPayloadScanResolver.h"

#include "HAL/FileManager.h"
#include "Ledger/MHImportLedger.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHPayloadHashes.h"

namespace UE::MimirComposite::Tests
{
namespace
{

FString MakeTempRoot()
{
    return FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests"),
        FString::Printf(TEXT("ResourceKey_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

bool WriteBytes(const FString& Path, const FString& Contents)
{
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    return FFileHelper::SaveStringToFile(
        Contents,
        *Path,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FMHResourceKey Key(const EMHResourceKind Kind, const TCHAR* LogicalName)
{
    FMHResourceKey Result;
    Result.Kind = Kind;
    Result.LogicalName = LogicalName;
    return Result;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResourceKeyClassifierTest,
    "Mimir.V4.ResourceKey.Classifier",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResourceKeyClassifierTest::RunTest(const FString& Parameters)
{
    FMHResourceKey Parsed;
    FString Error;
    bool bPassed = true;
    bPassed &= TestTrue(
        TEXT("compound mesh extension is stripped in full"),
        MHResourceKeyFromSourceFile(TEXT("C:/source/garage_a.mesh.fbx"), Parsed, Error));
    bPassed &= TestEqual(TEXT("mesh kind"), Parsed.Kind, EMHResourceKind::StaticMesh);
    bPassed &= TestEqual(TEXT("mesh logical name"), Parsed.LogicalName, TEXT("garage_a"));

    bPassed &= TestTrue(
        TEXT("texture extension is classified"),
        MHResourceKeyFromSourceFile(TEXT("C:/source/brick_a_tex_d.tiff"), Parsed, Error));
    bPassed &= TestEqual(TEXT("texture kind"), Parsed.Kind, EMHResourceKind::Texture);
    bPassed &= TestEqual(TEXT("texture logical name"), Parsed.LogicalName, TEXT("brick_a_tex_d"));

    for (const TCHAR* Extension : {
        TEXT("png"), TEXT("tga"), TEXT("tif"), TEXT("tiff"), TEXT("exr"),
        TEXT("jpg"), TEXT("jpeg"), TEXT("dds"), TEXT("hdr")})
    {
        Error.Reset();
        bPassed &= TestTrue(
            *FString::Printf(TEXT("texture extension %s"), Extension),
            MHResourceKeyFromSourceFile(
                FString::Printf(TEXT("C:/source/texture_name.%s"), Extension),
                Parsed,
                Error));
        bPassed &= TestEqual(TEXT("texture kind"), Parsed.Kind, EMHResourceKind::Texture);
    }
    bPassed &= TestTrue(
        TEXT("material extension is classified"),
        MHResourceKeyFromSourceFile(TEXT("C:/source/material_name.material"), Parsed, Error));
    bPassed &= TestEqual(TEXT("material kind"), Parsed.Kind, EMHResourceKind::Material);
    bPassed &= TestTrue(
        TEXT("composite extension is classified"),
        MHResourceKeyFromSourceFile(TEXT("C:/source/composite_name.composite"), Parsed, Error));
    bPassed &= TestEqual(TEXT("composite kind"), Parsed.Kind, EMHResourceKind::Composite);

    bPassed &= TestFalse(
        TEXT("embedded dot is rejected without normalization"),
        MHResourceKeyFromSourceFile(TEXT("C:/source/foo.bar.material"), Parsed, Error));
    bPassed &= TestTrue(
        TEXT("logical-name diagnostic is registered code"),
        Error.StartsWith(TEXT("MH_E_NONCANONICAL_RESOURCE_NAME")));

    Error.Reset();
    bPassed &= TestFalse(
        TEXT("unknown extension is ignored"),
        MHResourceKeyFromSourceFile(TEXT("C:/source/readme.txt"), Parsed, Error));
    bPassed &= TestTrue(TEXT("unknown extension has no diagnostic"), Error.IsEmpty());

    Error.Reset();
    bPassed &= TestFalse(
        TEXT("mixed-case logical name is rejected"),
        MHResourceKeyFromSourceFile(TEXT("C:/source/Garage.material"), Parsed, Error));
    bPassed &= TestTrue(TEXT("mixed-case name diagnostic"), Error.StartsWith(TEXT("MH_E_NONCANONICAL_RESOURCE_NAME")));
    Error.Reset();
    bPassed &= TestFalse(
        TEXT("mixed-case extension is rejected"),
        MHResourceKeyFromSourceFile(TEXT("C:/source/garage.Material"), Parsed, Error));
    bPassed &= TestTrue(TEXT("mixed-case extension diagnostic"), Error.StartsWith(TEXT("MH_E_NONCANONICAL_RESOURCE_NAME")));

    Parsed.Kind = static_cast<EMHResourceKind>(255);
    Parsed.LogicalName = TEXT("valid_name");
    bPassed &= TestFalse(TEXT("unknown resource kind is not canonical"), Parsed.IsCanonical());

    TSet<FMHResourceKey> DistinctKeys;
    DistinctKeys.Add(Key(EMHResourceKind::StaticMesh, TEXT("shared")));
    DistinctKeys.Add(Key(EMHResourceKind::Material, TEXT("shared")));
    DistinctKeys.Add(Key(EMHResourceKind::StaticMesh, TEXT("shared")));
    bPassed &= TestEqual(TEXT("kind participates in equality and hashing"), DistinctKeys.Num(), 2);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResourceKeyResolverTest,
    "Mimir.V4.ResourceKey.Resolver",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResourceKeyResolverTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeTempRoot();
    bool bPassed = true;
    bPassed &= TestTrue(
        TEXT("write mesh"),
        WriteBytes(FPaths::Combine(Root, TEXT("a/shared.mesh.fbx")), TEXT("mesh")));
    bPassed &= TestTrue(
        TEXT("write cross-kind material"),
        WriteBytes(FPaths::Combine(Root, TEXT("b/shared.material")), TEXT("{}")));
    bPassed &= TestTrue(
        TEXT("write texture candidate one"),
        WriteBytes(FPaths::Combine(Root, TEXT("a/brick_d.png")), TEXT("one")));
    bPassed &= TestTrue(
        TEXT("write texture candidate two"),
        WriteBytes(FPaths::Combine(Root, TEXT("b/brick_d.tga")), TEXT("two")));

    FMHPayloadScanResolver Resolver(Root);
    FString Error;
    bPassed &= TestTrue(TEXT("scan initializes"), Resolver.Initialize(Error));
    if (!Error.IsEmpty())
    {
        AddError(Error);
    }
    bPassed &= TestEqual(TEXT("all candidates counted"), Resolver.GetCandidateFileCount(), 4);

    bPassed &= TestEqual(
        TEXT("cross-kind mesh resolves"),
        Resolver.Resolve(Key(EMHResourceKind::StaticMesh, TEXT("shared"))).Status,
        EMHResolveStatus::Resolved);
    bPassed &= TestEqual(
        TEXT("cross-kind material resolves"),
        Resolver.Resolve(Key(EMHResourceKind::Material, TEXT("shared"))).Status,
        EMHResolveStatus::Resolved);
    const FMHResolveOutcome Texture = Resolver.Resolve(Key(EMHResourceKind::Texture, TEXT("brick_d")));
    bPassed &= TestEqual(
        TEXT("same texture stem across extensions is ambiguous"),
        Texture.Status,
        EMHResolveStatus::Ambiguous);
    bPassed &= TestTrue(
        TEXT("ambiguity diagnostic"),
        Texture.Diagnostic.StartsWith(TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME")));
    bPassed &= TestTrue(
        TEXT("scan emits duplicate warning"),
        Resolver.GetSnapshot().Warnings.ContainsByPredicate([](const FString& Warning)
        {
            return Warning.StartsWith(TEXT("MH_W_DUPLICATE_RESOURCE_NAME"));
        }));

    const FMHResolveOutcome Missing = Resolver.Resolve(
        Key(EMHResourceKind::Composite, TEXT("missing")));
    bPassed &= TestEqual(TEXT("missing key is unresolved"), Missing.Status, EMHResolveStatus::Unresolved);
    bPassed &= TestTrue(TEXT("missing diagnostic"), Missing.Diagnostic.StartsWith(TEXT("MH_E_RESOURCE_NOT_FOUND")));
    const FMHResolveOutcome Invalid = Resolver.Resolve(
        Key(EMHResourceKind::Composite, TEXT("Not_Canonical")));
    bPassed &= TestEqual(TEXT("invalid key is rejected"), Invalid.Status, EMHResolveStatus::Invalid);

    WriteBytes(FPaths::Combine(Root, TEXT("c/identical.mesh.fbx")), TEXT("same"));
    WriteBytes(FPaths::Combine(Root, TEXT("d/identical.mesh.fbx")), TEXT("same"));
    FMHPayloadScanResolver IdenticalResolver(Root);
    bPassed &= TestTrue(TEXT("rescan with identical duplicates"), IdenticalResolver.Initialize(Error));
    bPassed &= TestEqual(
        TEXT("byte-identical same-kind duplicates remain ambiguous"),
        IdenticalResolver.Resolve(Key(EMHResourceKind::StaticMesh, TEXT("identical"))).Status,
        EMHResolveStatus::Ambiguous);

    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResourceKeyBlockedAndRemoveTest,
    "Mimir.V4.ResourceKey.BlockedAndRemove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResourceKeyBlockedAndRemoveTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeTempRoot();
    WriteBytes(FPaths::Combine(Root, TEXT("invalid/Bad.material")), TEXT("{}"));
    WriteBytes(FPaths::Combine(Root, TEXT("a/duplicate.mesh.fbx")), TEXT("one"));
    WriteBytes(FPaths::Combine(Root, TEXT("b/duplicate.mesh.fbx")), TEXT("two"));

    FMHPayloadScanResolver Resolver(Root);
    FString Error;
    bool bPassed = TestTrue(TEXT("scan initializes with quarantined name"), Resolver.Initialize(Error));
    const FMHSourceSnapshot Snapshot = Resolver.GetSnapshot();
    bPassed &= TestEqual(TEXT("invalid known payload is quarantined"), Snapshot.Quarantined.Num(), 1);
    bPassed &= TestTrue(TEXT("quarantine is blocking"), Snapshot.Errors.ContainsByPredicate([](const FString& Value)
    {
        return Value.Contains(TEXT("MH_E_NONCANONICAL_RESOURCE_NAME"));
    }));

    FMHLedgerChangeDetector EmptyDetector({});
    FMHSourceAnalysis Blocked;
    EmptyDetector.DetectChanges(Resolver, Root, Blocked);
    bPassed &= TestEqual(TEXT("ambiguous resource is blocked"), Blocked.CountOf(EMHSourceChange::Blocked), 1);
    bPassed &= TestTrue(TEXT("quarantine propagates to analysis errors"), Blocked.HasErrors());

    const FString EmptyRoot = MakeTempRoot();
    IFileManager::Get().MakeDirectory(*EmptyRoot, true);
    FMHPayloadScanResolver EmptyResolver(EmptyRoot);
    bPassed &= TestTrue(TEXT("empty scan initializes"), EmptyResolver.Initialize(Error));
    FMHLedgerRow MissingRow;
    MissingRow.Kind = EMHResourceKind::StaticMesh;
    MissingRow.LogicalName = TEXT("removed");
    MissingRow.SourcePath = TEXT("old/removed.mesh.fbx");
    const TArray<uint8> EmptyBytes;
    MissingRow.AppliedRawHash = MHRawPayloadHash(EmptyBytes);
    MissingRow.ImportedAt = FDateTime::UtcNow();
    MissingRow.ImportStatus = TEXT("NO_CHANGE");
    TMap<FString, FMHLedgerRow> State;
    State.Add(TEXT("static_mesh:removed"), MissingRow);
    FMHLedgerChangeDetector RemoveDetector(State);
    FMHSourceAnalysis Removed;
    RemoveDetector.DetectChanges(EmptyResolver, EmptyRoot, Removed);
    bPassed &= TestEqual(TEXT("missing applied key is remove"), Removed.CountOf(EMHSourceChange::Remove), 1);

    IFileManager::Get().DeleteDirectory(*Root, false, true);
    IFileManager::Get().DeleteDirectory(*EmptyRoot, false, true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRawHashAndLedgerValidationTest,
    "Mimir.V4.ResourceKey.RawHashAndLedger",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRawHashAndLedgerValidationTest::RunTest(const FString& Parameters)
{
    bool bPassed = true;
    const TArray<uint8> EmptyBytes;
    const FString EmptyHash = MHRawPayloadHash(EmptyBytes);
    bPassed &= TestEqual(
        TEXT("BLAKE3-160 empty vector"),
        EmptyHash,
        TEXT("blake3-160:af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9"));
    bPassed &= TestTrue(TEXT("emitted hash is canonical"), MHIsCanonicalRawPayloadHash(EmptyHash));
    bPassed &= TestFalse(
        TEXT("uppercase digest is not canonical"),
        MHIsCanonicalRawPayloadHash(TEXT("blake3-160:AF1349B9F5F9A1A6A0404DEA36DCC9499BCB25C9")));

    FMHLedgerRow Row;
    Row.Kind = EMHResourceKind::StaticMesh;
    Row.LogicalName = TEXT("garage");
    Row.SourcePath = TEXT("models..old/garage.mesh.fbx");
    Row.AppliedRawHash = EmptyHash;
    Row.ImportedAt = FDateTime::UtcNow();
    Row.ImportStatus = TEXT("NO_CHANGE");
    TMap<FString, FMHLedgerRow> Rows;
    Rows.Add(TEXT("static_mesh:garage"), Row);
    FString Json;
    bPassed &= TestTrue(TEXT("legal dotted directory round-trips"), MHLedgerSnapshotToJson(Rows, Json));
    TMap<FString, FMHLedgerRow> Parsed;
    FString Error;
    bPassed &= TestTrue(TEXT("ledger snapshot parses"), MHLedgerSnapshotFromJson(Json, Parsed, Error));
    bPassed &= TestEqual(TEXT("ledger row count"), Parsed.Num(), 1);

    const FString InvalidAssetJson = Json.Replace(
        TEXT("\"asset\": \"\""),
        TEXT("\"asset\": \"not a valid object path\""));
    bPassed &= TestNotEqual(TEXT("negative asset fixture changed"), InvalidAssetJson, Json);
    bPassed &= TestFalse(
        TEXT("invalid nonempty asset path is rejected on parse"),
        MHLedgerSnapshotFromJson(InvalidAssetJson, Parsed, Error));

    for (const FString& InvalidPath : {
        FString(TEXT("/outside/garage.mesh.fbx")),
        FString(TEXT("C:/outside/garage.mesh.fbx")),
        FString(TEXT("folder/../garage.mesh.fbx")),
        FString(TEXT("folder\\garage.mesh.fbx"))})
    {
        Rows[TEXT("static_mesh:garage")].SourcePath = InvalidPath;
        bPassed &= TestFalse(*FString::Printf(TEXT("reject path %s"), *InvalidPath), MHLedgerSnapshotToJson(Rows, Json));
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResourceKeyAnalyzerTest,
    "Mimir.V4.ResourceKey.Analyzer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResourceKeyAnalyzerTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeTempRoot();
    const FString InitialPath = FPaths::Combine(Root, TEXT("a/garage.mesh.fbx"));
    WriteBytes(InitialPath, TEXT("first"));

    FMHPayloadScanResolver FirstResolver(Root);
    FString Error;
    if (!FirstResolver.Initialize(Error))
    {
        AddError(Error);
        return false;
    }
    FMHLedgerChangeDetector EmptyDetector({});
    FMHSourceAnalysis First;
    EmptyDetector.DetectChanges(FirstResolver, Root, First);
    bool bPassed = TestEqual(TEXT("first scan creates"), First.CountOf(EMHSourceChange::Create), 1);
    bPassed &= TestEqual(TEXT("source path is root-relative"), First.Entries[0].SourcePath, TEXT("a/garage.mesh.fbx"));

    FMHLedgerRow Row;
    bPassed &= TestTrue(TEXT("create produces deprecated Ledger row"), MHLedgerRowFromAnalysis(First.Entries[0], Row));
    TMap<FString, FMHLedgerRow> ReaderState;
    ReaderState.Add(First.Entries[0].Key.ToString(), Row);

    FMHLedgerChangeDetector NoChangeDetector(ReaderState);
    FMHSourceAnalysis NoChange;
    NoChangeDetector.DetectChanges(FirstResolver, Root, NoChange);
    bPassed &= TestEqual(TEXT("same path and bytes"), NoChange.CountOf(EMHSourceChange::NoChange), 1);

    const FString MovedPath = FPaths::Combine(Root, TEXT("b/garage.mesh.fbx"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(MovedPath), true);
    IFileManager::Get().Move(*MovedPath, *InitialPath);
    FMHPayloadScanResolver MovedResolver(Root);
    bPassed &= TestTrue(TEXT("moved scan initializes"), MovedResolver.Initialize(Error));
    FMHLedgerChangeDetector MoveDetector(ReaderState);
    FMHSourceAnalysis Moved;
    MoveDetector.DetectChanges(MovedResolver, Root, Moved);
    bPassed &= TestEqual(TEXT("move preserves identity"), Moved.CountOf(EMHSourceChange::Move), 1);

    WriteBytes(MovedPath, TEXT("second"));
    FMHPayloadScanResolver ChangedResolver(Root);
    bPassed &= TestTrue(TEXT("changed scan initializes"), ChangedResolver.Initialize(Error));
    FMHLedgerChangeDetector ChangedDetector(ReaderState);
    FMHSourceAnalysis Changed;
    ChangedDetector.DetectChanges(ChangedResolver, Root, Changed);
    bPassed &= TestEqual(TEXT("raw byte change reimports"), Changed.CountOf(EMHSourceChange::Reimport), 1);

    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
