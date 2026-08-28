#include "Source/MHSourceImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Index/MHProjectResourceIndex.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceComposition.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "Texture/MHTextureSourceData.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

FMHResourceKey WatcherScopeKey(const EMHResourceKind Kind, const FString& Name)
{
    FMHResourceKey Key;
    Key.Kind = Kind;
    Key.LogicalName = Name;
    return Key;
}

bool WatcherScopeWrite(const FString& Path, const FString& Text)
{
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString WatcherScopeHash(const FString& Path)
{
    TArray<uint8> Bytes;
    return FFileHelper::LoadFileToArray(Bytes, *Path) ? MHRawPayloadHash(Bytes) : FString();
}

struct FWatcherScopeFixture
{
    FString Base;
    FString Root;
    TArray<UObject*> Assets;

    FWatcherScopeFixture()
    {
        Base = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(),
            TEXT("MimirCompositeTests"), TEXT("watcher_scope_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()));
        Root = FPaths::Combine(Base, TEXT("source"));
        IFileManager::Get().MakeDirectory(*Root, true);
    }
    ~FWatcherScopeFixture()
    {
        MHSetNoChangeAssetLoadObserverForTests({});
        MHSetProfileFreshnessAssetLoadObserverForTests({});
        MHShutdownProjectIndex();
        for (UObject* Asset : Assets)
        {
            FAssetRegistryModule::AssetDeleted(Asset);
            Asset->ClearFlags(RF_Public | RF_Standalone);
            Asset->MarkAsGarbage();
        }
        IFileManager::Get().DeleteDirectory(*Base, false, true);
    }
};

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHWatcherAffectedSourceClosureTest,
    "Mimir.V5.Watcher.AffectedOldAndNewSourceClosure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHWatcherAffectedSourceClosureTest::RunTest(const FString& Parameters)
{
    FWatcherScopeFixture Fixture;
    const FString RootPath = FPaths::Combine(Fixture.Root, TEXT("root.composite"));
    const FString BranchPath = FPaths::Combine(Fixture.Root, TEXT("branch.composite"));
    const FString ProfilePath = FPaths::Combine(Fixture.Root, TEXT("scatter.placement"));
    const FString NewProfilePath = FPaths::Combine(Fixture.Root, TEXT("scatter_new.placement"));
    const FString Empty = TEXT("{\"v\":5,\"nodes\":[]}");
    const FString Profile = TEXT("{\"v\":1,\"kind\":\"placement_profile\",\"uniform_scale\":[1,0.25]}");
    const FString Branch = TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"group\",\"profile\":\"scatter\",\"children\":[{\"kind\":\"composite\",\"resource\":\"old_leaf\"}]}]}");
    bool bPassed = WatcherScopeWrite(RootPath,
        TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"random\",\"options\":[{\"kind\":\"empty\",\"weight\":1},{\"kind\":\"composite\",\"resource\":\"branch\",\"weight\":0}]},{\"kind\":\"composite\",\"resource\":\"sibling\"}]}"));
    bPassed &= WatcherScopeWrite(BranchPath, Branch);
    bPassed &= WatcherScopeWrite(ProfilePath, Profile);
    for (const TCHAR* Name : {TEXT("old_leaf"), TEXT("sibling"), TEXT("unrelated")})
        bPassed &= WatcherScopeWrite(FPaths::Combine(Fixture.Root, FString(Name) + TEXT(".composite")), Empty);
    FMHProjectResourceIndex Index(Fixture.Root, FPaths::Combine(Fixture.Base, TEXT("index.sqlite")));
    bool bRecreated = false;
    FString Error;
    if (!TestTrue(TEXT("isolated watcher index opens"), Index.Open(bRecreated, Error))) return false;
    FMHProjectIndexUpdateResult Update;
    bPassed &= TestTrue(TEXT("initial source projection"), Index.FullScan({}, Update, Error));
    const auto Contains = [&](const EMHResourceKind Kind, const TCHAR* Name)
    {
        return Update.AffectedResourceKeys.Contains(WatcherScopeKey(Kind, Name));
    };
    const auto CheckBranchClosure = [&]()
    {
        bPassed &= TestTrue(TEXT("unselected zero-weight branch remains affected"), Contains(EMHResourceKind::Composite, TEXT("branch")));
        bPassed &= TestTrue(TEXT("reverse parent is affected"), Contains(EMHResourceKind::Composite, TEXT("root")));
        bPassed &= TestTrue(TEXT("parent's forward sibling is admitted"), Contains(EMHResourceKind::Composite, TEXT("sibling")));
        bPassed &= TestFalse(TEXT("unrelated source island is excluded"), Contains(EMHResourceKind::Composite, TEXT("unrelated")));
    };
    bPassed &= TestTrue(TEXT("profile event upserts"), Index.UpsertPaths({ProfilePath}, Update, Error));
    CheckBranchClosure();
    bPassed &= TestTrue(TEXT("profile closure contains old descendant"), Contains(EMHResourceKind::Composite, TEXT("old_leaf")));

    bPassed &= WatcherScopeWrite(BranchPath, Empty);
    bPassed &= TestTrue(TEXT("edge-removing edit upserts"), Index.UpsertPaths({BranchPath}, Update, Error));
    CheckBranchClosure();
    bPassed &= TestTrue(TEXT("old forward edge survives in ephemeral scope"), Contains(EMHResourceKind::Composite, TEXT("old_leaf")));
    bPassed &= TestTrue(TEXT("old profile edge survives in ephemeral scope"), Contains(EMHResourceKind::PlacementProfile, TEXT("scatter")));
    bPassed &= WatcherScopeWrite(BranchPath, Branch);
    bPassed &= TestTrue(TEXT("restoring branch edges upserts"), Index.UpsertPaths({BranchPath}, Update, Error));

    bPassed &= TestTrue(TEXT("profile file removed"), IFileManager::Get().Delete(*ProfilePath));
    bPassed &= TestTrue(TEXT("profile removal upserts"), Index.UpsertPaths({ProfilePath}, Update, Error));
    CheckBranchClosure();
    bPassed &= WatcherScopeWrite(ProfilePath, Profile);
    bPassed &= TestTrue(TEXT("profile restoration upserts"), Index.UpsertPaths({ProfilePath}, Update, Error));
    CheckBranchClosure();
    bPassed &= TestTrue(TEXT("rename removes old file"), IFileManager::Get().Delete(*ProfilePath));
    bPassed &= WatcherScopeWrite(NewProfilePath, Profile);
    bPassed &= TestTrue(TEXT("profile rename batch upserts"), Index.UpsertPaths({ProfilePath, NewProfilePath}, Update, Error));
    CheckBranchClosure();
    bPassed &= TestTrue(TEXT("rename retains old key"), Contains(EMHResourceKind::PlacementProfile, TEXT("scatter")));
    bPassed &= TestTrue(TEXT("rename includes new key"), Contains(EMHResourceKind::PlacementProfile, TEXT("scatter_new")));
    bPassed &= TestTrue(TEXT("empty upsert succeeds"), Index.UpsertPaths({}, Update, Error));
    bPassed &= TestTrue(TEXT("empty affected set stays empty"), Update.AffectedResourceKeys.IsEmpty());
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHWatcherScopedPolicyLoadsTest,
    "Mimir.V5.Watcher.UnrelatedNoChangeAssetsAreNotLoadedOrReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHWatcherScopedPolicyLoadsTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    FWatcherScopeFixture Fixture;
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString CompositeName = TEXT("watcher_changed_") + Token;
    const FString MeshName = TEXT("watcher_unrelated_mesh_") + Token;
    const FString TextureName = TEXT("watcher_unrelated_texture_") + Token;
    const FString InvalidName = TEXT("watcher_unrelated_invalid_") + Token;
    const FString CompositePath = FPaths::Combine(Fixture.Root, CompositeName + TEXT(".composite"));
    const FString MeshPath = FPaths::Combine(Fixture.Root, MeshName + TEXT(".mesh.fbx"));
    const FString TexturePath = FPaths::Combine(Fixture.Root, TextureName + TEXT(".png"));
    bool bPassed = WatcherScopeWrite(CompositePath, TEXT("{\"v\":5,\"nodes\":[]}"));
    bPassed &= TestTrue(TEXT("unrelated real FBX copied into isolated source"),
        IFileManager::Get().Copy(*MeshPath, *FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"))) == COPY_OK);
    bPassed &= WatcherScopeWrite(TexturePath, TEXT("not imported by this unrelated watcher event"));
    bPassed &= WatcherScopeWrite(FPaths::Combine(Fixture.Root, InvalidName + TEXT(".composite")), TEXT("{\"nodes\":[]}"));

    UPackage* CompositePackage = CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + CompositeName));
    UMHCompositeAsset* Composite = NewObject<UMHCompositeAsset>(CompositePackage, FName(*CompositeName), RF_Public | RF_Standalone);
    Composite->LogicalName = CompositeName;
    Composite->SourceRelativePath = CompositeName + TEXT(".composite");
    Composite->SourceHash = WatcherScopeHash(CompositePath);
    Composite->AppliedHash = Composite->SourceHash;
    Fixture.Assets.Add(Composite);
    FAssetRegistryModule::AssetCreated(Composite);
    UPackage* MeshPackage = CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + MeshName));
    UStaticMesh* Mesh = NewObject<UStaticMesh>(MeshPackage, FName(*MeshName), RF_Public | RF_Standalone);
    UMHStaticMeshImportData* MeshReceipt = NewObject<UMHStaticMeshImportData>(Mesh);
    MeshReceipt->LogicalName = MeshName;
    MeshReceipt->SourceRelativePath = MeshName + TEXT(".mesh.fbx");
    MeshReceipt->SourceHash = WatcherScopeHash(MeshPath);
    MeshReceipt->ImporterVersion = MHStaticMeshImporterVersion;
    Mesh->SetAssetImportData(MeshReceipt);
    Fixture.Assets.Add(Mesh);
    FAssetRegistryModule::AssetCreated(Mesh);
    UPackage* TexturePackage = CreatePackage(*(TEXT("/Game/MH/Generated/Textures/") + TextureName));
    UTexture2D* Texture = NewObject<UTexture2D>(TexturePackage, FName(*TextureName), RF_Public | RF_Standalone);
    UMHTextureSourceData* TextureReceipt = NewObject<UMHTextureSourceData>(Texture);
    TextureReceipt->LogicalName = TextureName;
    TextureReceipt->SourceRelativePath = TextureName + TEXT(".png");
    TextureReceipt->SourceHash = WatcherScopeHash(TexturePath);
    Texture->AddAssetUserData(TextureReceipt);
    Fixture.Assets.Add(Texture);
    FAssetRegistryModule::AssetCreated(Texture);

    MHShutdownProjectIndex();
    FMHSourceAnalysisServices Services;
    FString Error;
    if (!TestTrue(TEXT("initial full projection does not import"), MHCreateDefaultSourceAnalysisServices(Fixture.Root, Services, Error))) return false;
    const FMHResourceKey MeshKey = WatcherScopeKey(EMHResourceKind::StaticMesh, MeshName);
    const FMHResourceKey TextureKey = WatcherScopeKey(EMHResourceKind::Texture, TextureName);
    FMHSourceAnalysis InitialAnalysis;
    bool bExecuted = false;
    MHBuildSourceImportPlan(*Services.ChangeDetector, *Services.Resolver, Fixture.Root,
        FMHImportSourcesScope::Only({MeshKey, TextureKey}), InitialAnalysis, bExecuted);
    bPassed &= TestTrue(TEXT("unrelated mesh is actually NO_CHANGE before event"), InitialAnalysis.Find(MeshKey) != nullptr && InitialAnalysis.Find(MeshKey)->Change == EMHSourceChange::NoChange);
    bPassed &= TestTrue(TEXT("unrelated texture is actually NO_CHANGE before event"), InitialAnalysis.Find(TextureKey) != nullptr && InitialAnalysis.Find(TextureKey)->Change == EMHSourceChange::NoChange);

    TArray<FMHResourceKey> PolicyLoads;
    MHSetNoChangeAssetLoadObserverForTests([&PolicyLoads](const FMHResourceKey& Key) { PolicyLoads.Add(Key); });
    FMHSourceAnalysis Analysis;
    FMHProjectIndexUpdateResult Update;
    bool bUsedFullScan = false;
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    MHImportChangedSourcesHeadless(Fixture.Root, {CompositePath}, *Settings, Analysis, Update, bUsedFullScan, bExecuted);
    bPassed &= TestFalse(TEXT("warm watcher event does not full-scan"), bUsedFullScan);
    bPassed &= TestTrue(TEXT("unrelated NO_CHANGE policy loads never run"), PolicyLoads.IsEmpty());
    bPassed &= TestNull(TEXT("unrelated mesh omitted from presented plan"), Analysis.Find(MeshKey));
    bPassed &= TestNull(TEXT("unrelated texture omitted from presented plan"), Analysis.Find(TextureKey));
    bPassed &= TestEqual(TEXT("one affected entry only"), Analysis.Entries.Num(), 1);
    bPassed &= TestFalse(TEXT("unchanged event requires no import"), bExecuted);
    bPassed &= TestFalse(TEXT("unchanged scoped plan needs no watcher presentation"), MHShouldPresentWatcherAnalysis({CompositePath}, Analysis));
    TArray<FMHProjectIndexDiagnostic> Diagnostics;
    bPassed &= TestTrue(TEXT("typed diagnostics remain in the index"), Services.Index->GetDiagnostics(Diagnostics, Error));
    bool bFoundUnrelatedInvalid = false;
    for (const FMHProjectIndexDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.OwnerName != InvalidName) continue;
        bFoundUnrelatedInvalid = true;
        bPassed &= TestFalse(TEXT("unrelated per-resource errors are not repeated in watcher report"), Analysis.Errors.Contains(Diagnostic.Message));
    }
    bPassed &= TestTrue(TEXT("fixture has an unrelated per-resource error to filter"), bFoundUnrelatedInvalid);

    const FString IgnoredPath = FPaths::Combine(Fixture.Root, TEXT("artist_notes.txt"));
    bPassed &= WatcherScopeWrite(IgnoredPath, TEXT("not a source payload"));
    MHImportChangedSourcesHeadless(Fixture.Root, {IgnoredPath}, *Settings, Analysis, Update, bUsedFullScan, bExecuted);
    bPassed &= TestTrue(TEXT("unrecognized event is explicit empty scope"), Analysis.Entries.IsEmpty());
    bPassed &= TestTrue(TEXT("explicit empty scope causes no policy loads"), PolicyLoads.IsEmpty());
    MHImportChangedSourcesHeadless(Fixture.Root, {}, *Settings, Analysis, Update, bUsedFullScan, bExecuted);
    bPassed &= TestTrue(TEXT("empty batch never becomes All"), Analysis.Entries.IsEmpty() && Update.AffectedResourceKeys.IsEmpty() && !bExecuted);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHWatcherEmptyScopeIntegrityTest,
    "Mimir.V5.Watcher.ExplicitEmptyScopePreservesGlobalIntegrityErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHWatcherEmptyScopeIntegrityTest::RunTest(const FString& Parameters)
{
    class FEmptyScopeResolver final : public IMHSourceResolver
    {
    public:
        virtual FMHSourceSnapshot GetSnapshot() const override
        {
            FMHSourceSnapshot Snapshot;
            Snapshot.ResourceKeys.Add(WatcherScopeKey(EMHResourceKind::StaticMesh, TEXT("unrelated")));
            Snapshot.Errors.Add(TEXT("MH_E_SOURCE_INDEX_INVALID: global integrity failure"));
            return Snapshot;
        }
        virtual FMHResolveOutcome Resolve(const FMHResourceKey& Key) override { return FMHResolveOutcome(); }
    } Resolver;
    class FEmptyScopeDetector final : public IMHChangeDetector
    {
    public:
        virtual void DetectChanges(IMHSourceResolver& SourceResolver, const FString& SourceRoot, FMHSourceAnalysis& Analysis) override
        {
            const FMHSourceSnapshot Snapshot = SourceResolver.GetSnapshot();
            Analysis.Errors = Snapshot.Errors;
            for (const FMHResourceKey& Key : Snapshot.ResourceKeys)
            {
                FMHSourceAnalysisEntry& Entry = Analysis.Entries.AddDefaulted_GetRef();
                Entry.Key = Key;
            }
        }
    } Detector;
    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    bool bPassed = TestFalse(TEXT("global integrity failure remains fail-closed"),
        MHBuildSourceImportPlan(Detector, Resolver, FString(), FMHImportSourcesScope::Only({}), Analysis, bExecuted));
    bPassed &= TestTrue(TEXT("explicit empty selection plans no unrelated resources"), Analysis.Entries.IsEmpty());
    bPassed &= TestTrue(TEXT("global integrity diagnostic is preserved verbatim"),
        Analysis.Errors.Contains(TEXT("MH_E_SOURCE_INDEX_INVALID: global integrity failure")));
    bPassed &= TestFalse(TEXT("pure planning executes no imports"), bExecuted);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
