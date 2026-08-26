#include "Index/MHProjectResourceIndex.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "MHGoldenRoot.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceComposition.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "UObject/Package.h"

#pragma pack(push, 8)
THIRD_PARTY_INCLUDES_START
#include <fbxsdk.h>
THIRD_PARTY_INCLUDES_END
#pragma pack(pop)

namespace UE::MimirComposite::Tests
{
namespace
{

struct FIndexFixture
{
    FString Root;
    FString DatabasePath;

    FIndexFixture()
    {
        Root = FPaths::Combine(
            FPaths::ProjectSavedDir(),
            TEXT("MimirCompositeTests"),
            FString::Printf(TEXT("ProjectIndex_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        DatabasePath = FPaths::Combine(Root, TEXT("cache/ProjectIndex.sqlite"));
        IFileManager::Get().MakeDirectory(*Root, true);
    }

    ~FIndexFixture()
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }
};

bool WriteProjectIndexUtf8(const FString& Path, const FString& Contents)
{
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    return FFileHelper::SaveStringToFile(
        Contents,
        *Path,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString FileHash(const FString& Path)
{
    TArray<uint8> Bytes;
    return FFileHelper::LoadFileToArray(Bytes, *Path)
        ? MHRawPayloadHash(Bytes)
        : FString();
}

FMHResourceKey ProjectIndexTestKey(const EMHResourceKind Kind, const FString& Name)
{
    FMHResourceKey Result;
    Result.Kind = Kind;
    Result.LogicalName = Name;
    return Result;
}

FMHGeneratedAssetTagClaim Claim(
    const EMHResourceKind Kind,
    const FString& Name,
    const FString& RelativePath,
    const FString& SourceHash,
    const FString& AppliedHash = FString())
{
    FMHGeneratedAssetTagClaim Result;
    Result.UEObjectPath = FString::Printf(
        TEXT("/Game/MH/Generated/%s/%s.%s"),
        Kind == EMHResourceKind::Material ? TEXT("Materials") :
        Kind == EMHResourceKind::Composite ? TEXT("Composites") :
        Kind == EMHResourceKind::Texture ? TEXT("Textures") : TEXT("Meshes"),
        *Name,
        *Name);
    Result.Kind = MHResourceKindLabel(Kind);
    Result.LogicalName = Name;
    Result.SourcePath = RelativePath;
    Result.SourceHash = SourceHash;
    Result.AppliedHash = AppliedHash.IsEmpty() ? SourceHash : AppliedHash;
    Result.Managed = TEXT("True");
    Result.MHTagCount = 6;
    Result.bHasCarrierKind = true;
    Result.CarrierKind = Kind;
    return Result;
}

bool OpenIndex(FMHProjectResourceIndex& Index, FAutomationTestBase& Test)
{
    bool bRecreated = false;
    FString Error;
    const bool bOpened = Index.Open(bRecreated, Error);
    if (!bOpened) Test.AddError(Error);
    return bOpened;
}

FbxNode* FindFirstMeshNode(FbxNode* Node)
{
    if (Node == nullptr)
    {
        return nullptr;
    }
    if (Node->GetMesh() != nullptr)
    {
        return Node;
    }
    for (int32 ChildIndex = 0; ChildIndex < Node->GetChildCount(); ++ChildIndex)
    {
        if (FbxNode* MeshNode = FindFirstMeshNode(Node->GetChild(ChildIndex)))
        {
            return MeshNode;
        }
    }
    return nullptr;
}

bool CopyFbxWithMaterialSlot(
    const FString& SourcePath,
    const FString& DestinationPath,
    const FString& MaterialName,
    FString& OutError)
{
    OutError.Reset();
    FbxManager* Manager = FbxManager::Create();
    if (Manager == nullptr)
    {
        OutError = TEXT("FbxManager::Create failed");
        return false;
    }
    FbxIOSettings* IOSettings = FbxIOSettings::Create(Manager, IOSROOT);
    Manager->SetIOSettings(IOSettings);
    FbxScene* Scene = FbxScene::Create(Manager, "MHProjectIndexSlotFixture");
    FbxImporter* Importer = FbxImporter::Create(Manager, "MHProjectIndexSlotImporter");
    if (!Importer->Initialize(TCHAR_TO_UTF8(*SourcePath), -1, IOSettings) ||
        !Importer->Import(Scene))
    {
        OutError = UTF8_TO_TCHAR(Importer->GetStatus().GetErrorString());
        Manager->Destroy();
        return false;
    }

    FbxNode* MeshNode = FindFirstMeshNode(Scene->GetRootNode());
    FbxMesh* Mesh = MeshNode != nullptr ? MeshNode->GetMesh() : nullptr;
    if (Mesh == nullptr)
    {
        OutError = TEXT("golden FBX contains no mesh node");
        Manager->Destroy();
        return false;
    }
    FbxSurfacePhong* Material = FbxSurfacePhong::Create(Scene, TCHAR_TO_UTF8(*MaterialName));
    MeshNode->AddMaterial(Material);
    FbxGeometryElementMaterial* MaterialLayer = Mesh->CreateElementMaterial();
    MaterialLayer->SetMappingMode(FbxGeometryElement::eAllSame);
    MaterialLayer->SetReferenceMode(FbxGeometryElement::eIndexToDirect);
    MaterialLayer->GetIndexArray().Add(0);

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestinationPath), true);
    FbxExporter* Exporter = FbxExporter::Create(Manager, "MHProjectIndexSlotExporter");
    if (!Exporter->Initialize(TCHAR_TO_UTF8(*DestinationPath), -1, IOSettings) ||
        !Exporter->Export(Scene))
    {
        OutError = UTF8_TO_TCHAR(Exporter->GetStatus().GetErrorString());
        Manager->Destroy();
        return false;
    }
    Manager->Destroy();
    return true;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexPartialTagDiscoveryTest,
    "Mimir.V4.ProjectIndex.PartialTagDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexPartialTagDiscoveryTest::RunTest(const FString& Parameters)
{
    const FString AssetName = TEXT("s4_partial_tags_") +
        FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UPackage* Package = CreatePackage(*(TEXT("/Game/MH/Generated/Tests/") + AssetName));
    UTexture2D* Asset = NewObject<UTexture2D>(
        Package,
        FName(*AssetName),
        RF_Public | RF_Standalone);
    const FDelegateHandle ExtraTagsHandle =
        UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.AddLambda(
            [Asset](FAssetRegistryTagsContext Context)
            {
                if (Context.GetObject() == Asset)
                {
                    Context.AddTag(UObject::FAssetRegistryTag(
                        TEXT("MH.SourceHash"),
                        TEXT("blake3-160:af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9"),
                        UObject::FAssetRegistryTag::TT_Alphabetical));
                }
            });
    FAssetRegistryModule::AssetCreated(Asset);

    TArray<FMHGeneratedAssetTagClaim> Claims;
    MHGatherGeneratedAssetClaimsForTests(Claims);
    const FString ObjectPath = FSoftObjectPath::ConstructFromObject(Asset).ToString();
    const FMHGeneratedAssetTagClaim* Claim = Claims.FindByPredicate(
        [&ObjectPath](const FMHGeneratedAssetTagClaim& Candidate)
        {
            return Candidate.UEObjectPath == ObjectPath;
        });
    bool bPassed = TestNotNull(TEXT("asset missing MH.Managed remains discoverable"), Claim);
    if (Claim != nullptr)
    {
        bPassed &= TestTrue(TEXT("missing managed tag remains empty"), Claim->Managed.IsEmpty());
        bPassed &= TestEqual(TEXT("partial MH tag count is retained"), Claim->MHTagCount, 1);
    }

    FAssetRegistryModule::AssetDeleted(Asset);
    UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.Remove(ExtraTagsHandle);
    Asset->ClearFlags(RF_Public | RF_Standalone);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexRebuildTest,
    "Mimir.V4.ProjectIndex.RebuildAndNormalizedDump",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexRebuildTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString TexturePath = FPaths::Combine(Fixture.Root, TEXT("textures/brick_d.png"));
    const FString MaterialPath = FPaths::Combine(Fixture.Root, TEXT("materials/wall.material"));
    bool bPassed = WriteProjectIndexUtf8(TexturePath, TEXT("texture-bytes"));
    bPassed &= WriteProjectIndexUtf8(
        MaterialPath,
        TEXT("{\n  \"class\": \"simple\",\n  \"textures\": {\n    \"tex0\": \"brick_d\"\n  }\n}\n"));

    const FMHGeneratedAssetTagClaim MaterialClaim = Claim(
        EMHResourceKind::Material,
        TEXT("wall"),
        TEXT("materials/wall.material"),
        FileHash(MaterialPath),
        FileHash(MaterialPath));
    const TArray<FMHResourceKey> ExpectedKeys = {
        ProjectIndexTestKey(EMHResourceKind::Material, TEXT("wall")),
        ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("brick_d"))};
    TArray<FMHResolveOutcome> FirstOutcomes;
    FString FirstDump;
    {
        FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
        if (!OpenIndex(Index, *this)) return false;
        FMHProjectIndexUpdateResult Update;
        FString Error;
        bPassed &= TestTrue(
            TEXT("full scan succeeds"),
            Index.FullScan({MaterialClaim}, Update, Error));
        if (!Error.IsEmpty()) AddError(Error);
        bPassed &= TestEqual(TEXT("generation one"), Update.Generation, static_cast<int64>(1));
        bPassed &= TestEqual(
            TEXT("material resolves through present texture dependency"),
            Index.Resolve(ProjectIndexTestKey(EMHResourceKind::Material, TEXT("wall"))).Status,
            EMHResolveStatus::Resolved);
        for (const FMHResourceKey& ResourceKey : ExpectedKeys)
        {
            FirstOutcomes.Add(Index.Resolve(ResourceKey));
        }
        bPassed &= TestTrue(TEXT("normalized dump builds"), Index.BuildNormalizedDump(FirstDump, Error));
        bPassed &= TestFalse(TEXT("dump excludes generation"), FirstDump.Contains(TEXT("generation")));
        Index.Close();
    }

    bPassed &= TestTrue(
        TEXT("cache deletion"),
        IFileManager::Get().Delete(*Fixture.DatabasePath, false, true, true));
    FString SecondDump;
    {
        FMHProjectResourceIndex Rebuilt(Fixture.Root, Fixture.DatabasePath);
        if (!OpenIndex(Rebuilt, *this)) return false;
        FMHProjectIndexUpdateResult Update;
        FString Error;
        bPassed &= TestTrue(
            TEXT("rebuild scan succeeds"),
            Rebuilt.FullScan({MaterialClaim}, Update, Error));
        if (!Error.IsEmpty()) AddError(Error);
        bPassed &= TestTrue(TEXT("rebuilt dump builds"), Rebuilt.BuildNormalizedDump(SecondDump, Error));
        for (int32 KeyIndex = 0; KeyIndex < ExpectedKeys.Num(); ++KeyIndex)
        {
            const FMHResolveOutcome RebuiltOutcome = Rebuilt.Resolve(ExpectedKeys[KeyIndex]);
            const FMHResolveOutcome& FirstOutcome = FirstOutcomes[KeyIndex];
            const FString Label = ExpectedKeys[KeyIndex].ToString();
            bPassed &= TestEqual(*(Label + TEXT(" status after rebuild")), RebuiltOutcome.Status, FirstOutcome.Status);
            bPassed &= TestEqual(*(Label + TEXT(" path after rebuild")), RebuiltOutcome.PayloadPath, FirstOutcome.PayloadPath);
            bPassed &= TestEqual(*(Label + TEXT(" hash after rebuild")), RebuiltOutcome.RawHash, FirstOutcome.RawHash);
            bPassed &= TestTrue(
                *(Label + TEXT(" candidates after rebuild")),
                RebuiltOutcome.CandidatePaths == FirstOutcome.CandidatePaths);
            bPassed &= TestEqual(
                *(Label + TEXT(" diagnostic after rebuild")),
                RebuiltOutcome.Diagnostic,
                FirstOutcome.Diagnostic);
        }
    }
    bPassed &= TestEqual(TEXT("rebuild logical dump is identical"), SecondDump, FirstDump);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexStaticMeshSlotDependencyTest,
    "Mimir.V4.ProjectIndex.StaticMeshSlotDependency",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexStaticMeshSlotDependencyTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    const FString SourceFbx = FPaths::Combine(
        GoldenRoot,
        TEXT("fixtures/axis/axis_probe.fbx"));
    const FString MeshPath = FPaths::Combine(Fixture.Root, TEXT("meshes/crate.mesh.fbx"));
    const FString MaterialPath = FPaths::Combine(Fixture.Root, TEXT("materials/crate_surface.material"));
    FString Error;
    bool bPassed = TestTrue(
        TEXT("create FBX with one material slot"),
        CopyFbxWithMaterialSlot(SourceFbx, MeshPath, TEXT("crate_surface"), Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= WriteProjectIndexUtf8(MaterialPath, TEXT("{\n  \"class\": \"simple\"\n}\n"));

    const FMHResourceKey MeshKey = ProjectIndexTestKey(EMHResourceKind::StaticMesh, TEXT("crate"));
    FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(Index, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    Error.Reset();
    bPassed &= TestTrue(TEXT("scan FBX material slot"), Index.FullScan({}, Update, Error));
    if (!Error.IsEmpty()) AddError(Error);
    const FMHResolveOutcome MeshOutcome = Index.Resolve(MeshKey);
    if (MeshOutcome.Status != EMHResolveStatus::Resolved)
    {
        AddError(FString::Printf(
            TEXT("static mesh resolution failed: %s"),
            *MeshOutcome.Diagnostic));
    }
    bPassed &= TestEqual(
        TEXT("mesh resolves while slot material exists"),
        MeshOutcome.Status,
        EMHResolveStatus::Resolved);

    FString Dump;
    bPassed &= TestTrue(TEXT("dependency dump builds"), Index.BuildNormalizedDump(Dump, Error));
    const bool bHasSlotDependency = Dump.Contains(TEXT(
        "Dependencies\tstatic_mesh\tcrate\tmaterial\tcrate_surface\tslot\tmeshes/crate.mesh.fbx\n"));
    if (!bHasSlotDependency)
    {
        AddError(FString::Printf(TEXT("slot dependency missing from dump:\n%s"), *Dump));
    }
    bPassed &= TestTrue(TEXT("FBX slot is projected as the closed dependency tuple"), bHasSlotDependency);
    Index.Close();

    bool bRecreated = false;
    bPassed &= TestTrue(TEXT("reopen index containing slot tuple"), Index.Open(bRecreated, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= TestFalse(
        TEXT("static_mesh to material slot tuple is accepted by dictionary validation"),
        bRecreated);

    bPassed &= TestTrue(TEXT("remove slot material"), IFileManager::Get().Delete(*MaterialPath));
    bPassed &= TestTrue(TEXT("upsert removed slot material"), Index.UpsertPaths({MaterialPath}, Update, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= TestEqual(
        TEXT("missing slot material transitively blocks static mesh"),
        Index.Resolve(MeshKey).Status,
        EMHResolveStatus::Invalid);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexPlacementProfileDependencyTest,
    "Mimir.V5.ProjectIndex.PlacementProfileDependency",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexPlacementProfileDependencyTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString ProfilePath = FPaths::Combine(Fixture.Root, TEXT("profiles/scatter.placement"));
    const FString CompositePath = FPaths::Combine(Fixture.Root, TEXT("composites/root.composite"));
    bool bPassed = WriteProjectIndexUtf8(
        ProfilePath,
        TEXT("{\n  \"v\": 1,\n  \"kind\": \"placement_profile\",\n  \"uniform_scale\": [\n    1,\n    0.1\n  ]\n}\n"));
    bPassed &= WriteProjectIndexUtf8(
        CompositePath,
        TEXT("{\n  \"v\": 5,\n  \"nodes\": [\n    {\n      \"kind\": \"group\",\n      \"profile\": \"scatter\"\n    }\n  ]\n}\n"));
    const FMHResourceKey ProfileKey =
        ProjectIndexTestKey(EMHResourceKind::PlacementProfile, TEXT("scatter"));
    const FMHResourceKey CompositeKey =
        ProjectIndexTestKey(EMHResourceKind::Composite, TEXT("root"));
    const FMHGeneratedAssetTagClaim CompositeClaim = Claim(
        EMHResourceKind::Composite,
        TEXT("root"),
        TEXT("composites/root.composite"),
        FileHash(CompositePath));
    FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(Index, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    FString Error;
    bPassed &= TestTrue(
        TEXT("profile/composite scan succeeds"),
        Index.FullScan({}, Update, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= TestEqual(
        TEXT("placement profile resolves as source-only resource"),
        Index.Resolve(ProfileKey).Status,
        EMHResolveStatus::Resolved);
    bPassed &= TestEqual(
        TEXT("composite resolves through profile"),
        Index.Resolve(CompositeKey).Status,
        EMHResolveStatus::Resolved);
    FMHProjectIndexResolver Resolver(Index);
    FMHProjectIndexChangeDetector Detector(Index);
    FMHSourceAnalysis Analysis;
    Detector.DetectChanges(Resolver, Fixture.Root, Analysis);
    const FMHSourceAnalysisEntry* ProfileEntry = Analysis.Find(ProfileKey);
    bPassed &= TestNotNull(TEXT("source-only profile analysis entry"), ProfileEntry);
    if (ProfileEntry != nullptr)
    {
        bPassed &= TestEqual(
            TEXT("resolved source-only profile has no phantom create"),
            ProfileEntry->Change,
            EMHSourceChange::NoChange);
    }
    bPassed &= TestTrue(
        TEXT("register applied profile carrier"),
        Index.ReplaceGeneratedAssets({CompositeClaim}, Update, Error));
    if (!Error.IsEmpty()) AddError(Error);
    TArray<FMHProjectIndexGeneratedAssetState> CompositeAssets;
    bPassed &= TestTrue(
        TEXT("read applied profile carrier"),
        Index.GetGeneratedAssets(CompositeKey, CompositeAssets, Error));
    bPassed &= TestEqual(TEXT("one managed profile carrier"), CompositeAssets.Num(), 1);
    if (CompositeAssets.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("profile carrier starts applied"),
            CompositeAssets[0].Status,
            EMHGeneratedAssetStatus::Applied);
    }
    FString Dump;
    bPassed &= TestTrue(TEXT("profile dependency dump builds"), Index.BuildNormalizedDump(Dump, Error));
    bPassed &= TestTrue(
        TEXT("index stores exact composite to placement_profile profile edge"),
        Dump.Contains(TEXT("Dependencies\tcomposite\troot\tplacement_profile\tscatter\tprofile\tcomposites/root.composite\n")));
    TArray<FMHResourceKey> ProfileDependencies;
    bPassed &= TestTrue(
        TEXT("profile dependency reader succeeds"),
        Index.GetPlacementProfileDependencies(CompositeKey, ProfileDependencies, Error));
    bPassed &= TestTrue(
        TEXT("profile dependency reader returns the exact indexed edge"),
        ProfileDependencies.Num() == 1 && ProfileDependencies[0] == ProfileKey);
    bPassed &= TestFalse(
        TEXT("placement profile has no generated asset path"),
        Dump.Contains(TEXT("GeneratedAssets\tplacement_profile")));
    bPassed &= WriteProjectIndexUtf8(
        ProfilePath,
        TEXT("{\n  \"v\": 1,\n  \"kind\": \"placement_profile\",\n  \"uniform_scale\": [\n    1,\n    0.2\n  ]\n}\n"));
    bPassed &= TestTrue(
        TEXT("changed valid placement profile upsert succeeds"),
        Index.UpsertPaths({ProfilePath}, Update, Error));
    if (!Error.IsEmpty()) AddError(Error);
    CompositeAssets.Reset();
    bPassed &= TestTrue(
        TEXT("read carrier after profile source change"),
        Index.GetGeneratedAssets(CompositeKey, CompositeAssets, Error));
    bPassed &= TestEqual(TEXT("one carrier remains after profile change"), CompositeAssets.Num(), 1);
    if (CompositeAssets.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("profile freshness stays outside the pure index projection"),
            CompositeAssets[0].Status,
            EMHGeneratedAssetStatus::Applied);
    }
    bPassed &= TestTrue(
        TEXT("refresh carrier receipt before profile ambiguity probe"),
        Index.ReplaceGeneratedAssets({CompositeClaim}, Update, Error));
    const FString DuplicateProfilePath =
        FPaths::Combine(Fixture.Root, TEXT("duplicate/scatter.placement"));
    bPassed &= WriteProjectIndexUtf8(
        DuplicateProfilePath,
        TEXT("{\n  \"v\": 1,\n  \"kind\": \"placement_profile\",\n  \"uniform_scale\": [\n    1,\n    0.3\n  ]\n}\n"));
    bPassed &= TestTrue(
        TEXT("duplicate profile upsert succeeds"),
        Index.UpsertPaths({DuplicateProfilePath}, Update, Error));
    bPassed &= TestEqual(
        TEXT("duplicate profile is ambiguous"),
        Index.Resolve(ProfileKey).Status,
        EMHResolveStatus::Ambiguous);
    CompositeAssets.Reset();
    bPassed &= TestTrue(
        TEXT("read carrier after unique profile removal"),
        Index.GetGeneratedAssets(CompositeKey, CompositeAssets, Error));
    if (CompositeAssets.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("profile ambiguity does not synthesize composite receipt staleness"),
            CompositeAssets[0].Status,
            EMHGeneratedAssetStatus::Applied);
    }
    bPassed &= TestTrue(
        TEXT("refresh carrier receipt before profile recovery probe"),
        Index.ReplaceGeneratedAssets({CompositeClaim}, Update, Error));
    bPassed &= TestTrue(
        TEXT("remove duplicate profile"),
        IFileManager::Get().Delete(*DuplicateProfilePath, false, true, true));
    bPassed &= TestTrue(
        TEXT("upsert recovered unique profile"),
        Index.UpsertPaths({DuplicateProfilePath}, Update, Error));
    bPassed &= TestEqual(
        TEXT("profile recovers to unique"),
        Index.Resolve(ProfileKey).Status,
        EMHResolveStatus::Resolved);
    CompositeAssets.Reset();
    bPassed &= TestTrue(
        TEXT("read carrier after profile recovery"),
        Index.GetGeneratedAssets(CompositeKey, CompositeAssets, Error));
    bPassed &= TestEqual(TEXT("one carrier remains after recovery"), CompositeAssets.Num(), 1);
    if (CompositeAssets.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("profile recovery leaves the pure composite projection applied"),
            CompositeAssets[0].Status,
            EMHGeneratedAssetStatus::Applied);
    }
    Index.Close();
    bool bRecreated = false;
    bPassed &= TestTrue(TEXT("index with profile edge reopens"), Index.Open(bRecreated, Error));
    bPassed &= TestFalse(TEXT("profile dictionary does not force cache recreation"), bRecreated);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexStatusTest,
    "Mimir.V4.ProjectIndex.StatusPrecedenceAndClosure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexStatusTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString TextureA = FPaths::Combine(Fixture.Root, TEXT("a/shared_d.png"));
    const FString TextureB = FPaths::Combine(Fixture.Root, TEXT("b/shared_d.tga"));
    const FString Material = FPaths::Combine(Fixture.Root, TEXT("uses_shared.material"));
    bool bPassed = WriteProjectIndexUtf8(TextureA, TEXT("one"));
    bPassed &= WriteProjectIndexUtf8(TextureB, TEXT("two"));
    bPassed &= WriteProjectIndexUtf8(
        Material,
        TEXT("{\n  \"class\": \"simple\",\n  \"textures\": {\n    \"tex0\": \"shared_d\"\n  }\n}\n"));

    FMHGeneratedAssetTagClaim TextureClaim = Claim(
        EMHResourceKind::Texture,
        TEXT("shared_d"),
        TEXT("a/shared_d.png"),
        FileHash(TextureA));
    FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(Index, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    FString Error;
    bPassed &= TestTrue(TEXT("scan duplicate texture"), Index.FullScan({TextureClaim}, Update, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= TestEqual(
        TEXT("duplicate texture is ambiguous"),
        Index.Resolve(ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("shared_d"))).Status,
        EMHResolveStatus::Ambiguous);
    bPassed &= TestEqual(
        TEXT("dependent material is blocked"),
        Index.Resolve(ProjectIndexTestKey(EMHResourceKind::Material, TEXT("uses_shared"))).Status,
        EMHResolveStatus::Invalid);

    TArray<FMHProjectIndexGeneratedAssetState> Assets;
    bPassed &= TestTrue(
        TEXT("read source-blocked generated state"),
        Index.GetGeneratedAssets(
            ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("shared_d")), Assets, Error));
    bPassed &= TestEqual(TEXT("one claim"), Assets.Num(), 1);
    if (Assets.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("ambiguous source produces source_blocked"),
            Assets[0].Status,
            EMHGeneratedAssetStatus::SourceBlocked);
    }

    FMHGeneratedAssetTagClaim InvalidBinary = TextureClaim;
    InvalidBinary.AppliedHash = MHRawPayloadHash(TArray<uint8>{1, 2, 3});
    bPassed &= TestTrue(
        TEXT("replace invalid binary receipt"),
        Index.ReplaceGeneratedAssets({InvalidBinary}, Update, Error));
    Assets.Reset();
    bPassed &= Index.GetGeneratedAssets(
        ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("shared_d")), Assets, Error);
    if (Assets.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("binary hash mismatch invalidates receipt"),
            Assets[0].Status,
            EMHGeneratedAssetStatus::InvalidReceipt);
    }

    FMHGeneratedAssetTagClaim ExtraTag = TextureClaim;
    ExtraTag.MHTagCount = 7;
    bPassed &= TestTrue(
        TEXT("replace non-exact MH tag set"),
        Index.ReplaceGeneratedAssets({ExtraTag}, Update, Error));
    Assets.Reset();
    bPassed &= Index.GetGeneratedAssets(
        ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("shared_d")), Assets, Error);
    if (Assets.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("extra MH tag invalidates receipt"),
            Assets[0].Status,
            EMHGeneratedAssetStatus::InvalidReceipt);
    }

    FMHGeneratedAssetTagClaim DuplicateClaim = TextureClaim;
    DuplicateClaim.UEObjectPath = TEXT("/Game/MH/Generated/Textures/shared_d_copy.shared_d_copy");
    bPassed &= TestTrue(
        TEXT("replace duplicate claims"),
        Index.ReplaceGeneratedAssets({InvalidBinary, DuplicateClaim}, Update, Error));
    Assets.Reset();
    bPassed &= Index.GetGeneratedAssets(
        ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("shared_d")), Assets, Error);
    bPassed &= TestEqual(TEXT("two claims"), Assets.Num(), 2);
    for (const FMHProjectIndexGeneratedAssetState& Asset : Assets)
    {
        bPassed &= TestEqual(
            TEXT("duplicate_claim precedes invalid_receipt"),
            Asset.Status,
            EMHGeneratedAssetStatus::DuplicateClaim);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexIncrementalTest,
    "Mimir.V4.ProjectIndex.IncrementalPublishMoveRename",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexIncrementalTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString OldPath = FPaths::Combine(Fixture.Root, TEXT("old/brick_d.png"));
    bool bPassed = WriteProjectIndexUtf8(OldPath, TEXT("same-binary-content"));
    const FString Hash = FileHash(OldPath);
    FMHGeneratedAssetTagClaim TextureClaim = Claim(
        EMHResourceKind::Texture,
        TEXT("brick_d"),
        TEXT("old/brick_d.png"),
        Hash);

    FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(Index, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    FString Error;
    bPassed &= TestTrue(TEXT("initial scan"), Index.FullScan({TextureClaim}, Update, Error));

    bPassed &= TestTrue(
        TEXT("register publish token"),
        Index.RegisterSelfPublishAfterReplace(OldPath, Hash, Error));
    bPassed &= TestTrue(TEXT("published path upsert"), Index.UpsertPaths({OldPath}, Update, Error));
    bPassed &= TestEqual(TEXT("single self-publish event"), Update.SessionEvents.Num(), 1);
    if (Update.SessionEvents.Num() == 1)
    {
        bPassed &= TestTrue(
            TEXT("event classification"),
            Update.SessionEvents[0].StartsWith(TEXT("SELF_PUBLISHED")));
    }
    bPassed &= TestTrue(TEXT("second path upsert"), Index.UpsertPaths({OldPath}, Update, Error));
    bPassed &= TestTrue(TEXT("token is single-shot"), Update.SessionEvents.IsEmpty());

    const FString MovedPath = FPaths::Combine(Fixture.Root, TEXT("moved/brick_d.png"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(MovedPath), true);
    bPassed &= TestTrue(TEXT("move source"), IFileManager::Get().Move(*MovedPath, *OldPath));
    bPassed &= TestTrue(
        TEXT("upsert old and new path"),
        Index.UpsertPaths({OldPath, MovedPath}, Update, Error));
    FMHProjectIndexResolver Resolver(Index);
    FMHProjectIndexChangeDetector Detector(Index);
    FMHSourceAnalysis Analysis;
    Detector.DetectChanges(Resolver, Fixture.Root, Analysis);
    const FMHSourceAnalysisEntry* Entry = Analysis.Find(
        ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("brick_d")));
    bPassed &= TestNotNull(TEXT("move analysis entry"), Entry);
    if (Entry != nullptr)
    {
        bPassed &= TestEqual(
            TEXT("move preserves identity without asset mutation"),
            Entry->Change,
            EMHSourceChange::NoChange);
    }

    const FString RenamedPath = FPaths::Combine(Fixture.Root, TEXT("moved/brick_new.png"));
    bPassed &= TestTrue(TEXT("rename source"), IFileManager::Get().Move(*RenamedPath, *MovedPath));
    bPassed &= TestTrue(
        TEXT("upsert rename pair"),
        Index.UpsertPaths({MovedPath, RenamedPath}, Update, Error));
    FString Dump;
    bPassed &= TestTrue(TEXT("dump after rename"), Index.BuildNormalizedDump(Dump, Error));
    bPassed &= TestTrue(
        TEXT("bijective same-hash rename diagnostic"),
        Dump.Contains(TEXT("MH_W_PROBABLE_RESOURCE_RENAME")));
    const FMHResourceKey OldKey = ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("brick_d"));
    const FMHResourceKey NewKey = ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("brick_new"));
    bPassed &= TestEqual(
        TEXT("renamed old key has no source candidate"),
        Index.Resolve(OldKey).Status,
        EMHResolveStatus::Unresolved);
    bPassed &= TestEqual(
        TEXT("renamed new key is a unique resource"),
        Index.Resolve(NewKey).Status,
        EMHResolveStatus::Resolved);
    TArray<FMHProjectIndexGeneratedAssetState> OldAssets;
    bPassed &= TestTrue(
        TEXT("old generated claim remains queryable"),
        Index.GetGeneratedAssets(OldKey, OldAssets, Error));
    bPassed &= TestEqual(TEXT("one old generated claim"), OldAssets.Num(), 1);
    if (OldAssets.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("old generated claim becomes orphan"),
            OldAssets[0].Status,
            EMHGeneratedAssetStatus::Orphan);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexClaimedRenameEndpointTest,
    "Mimir.V4.ProjectIndex.ProbableRenameExcludesClaimedEndpoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexClaimedRenameEndpointTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString PathA = FPaths::Combine(Fixture.Root, TEXT("asset_a.png"));
    const FString PathB = FPaths::Combine(Fixture.Root, TEXT("asset_b.png"));
    bool bPassed = WriteProjectIndexUtf8(PathA, TEXT("shared-binary-content"));
    bPassed &= WriteProjectIndexUtf8(PathB, TEXT("shared-binary-content"));
    const FString Hash = FileHash(PathA);
    const FMHGeneratedAssetTagClaim ClaimA = Claim(
        EMHResourceKind::Texture, TEXT("asset_a"), TEXT("asset_a.png"), Hash);
    const FMHGeneratedAssetTagClaim ClaimB = Claim(
        EMHResourceKind::Texture, TEXT("asset_b"), TEXT("asset_b.png"), Hash);

    FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(Index, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    FString Error;
    bPassed &= TestTrue(
        TEXT("scan two pre-existing managed resources"),
        Index.FullScan({ClaimA, ClaimB}, Update, Error));
    bPassed &= TestTrue(TEXT("remove A source"), IFileManager::Get().Delete(*PathA));
    bPassed &= TestTrue(TEXT("upsert removed A"), Index.UpsertPaths({PathA}, Update, Error));
    FString Dump;
    bPassed &= TestTrue(TEXT("dump after A removal"), Index.BuildNormalizedDump(Dump, Error));
    bPassed &= TestFalse(
        TEXT("pre-existing claimed B is not a probable rename endpoint"),
        Dump.Contains(TEXT("MH_W_PROBABLE_RESOURCE_RENAME")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexPreexistingUnmanagedRenameEndpointTest,
    "Mimir.V4.ProjectIndex.ProbableRenameRequiresAppearedEndpoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexPreexistingUnmanagedRenameEndpointTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString PathA = FPaths::Combine(Fixture.Root, TEXT("old_managed.png"));
    const FString PathB = FPaths::Combine(Fixture.Root, TEXT("preexisting_unmanaged.png"));
    bool bPassed = WriteProjectIndexUtf8(PathA, TEXT("shared-binary-content"));
    bPassed &= WriteProjectIndexUtf8(PathB, TEXT("shared-binary-content"));
    const FMHGeneratedAssetTagClaim ClaimA = Claim(
        EMHResourceKind::Texture,
        TEXT("old_managed"),
        TEXT("old_managed.png"),
        FileHash(PathA));

    FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(Index, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    FString Error;
    bPassed &= TestTrue(
        TEXT("scan managed A and pre-existing unmanaged B"),
        Index.FullScan({ClaimA}, Update, Error));
    bPassed &= TestTrue(TEXT("remove A source"), IFileManager::Get().Delete(*PathA));
    bPassed &= TestTrue(TEXT("upsert removed A"), Index.UpsertPaths({PathA}, Update, Error));
    FString Dump;
    bPassed &= TestTrue(TEXT("dump after A removal"), Index.BuildNormalizedDump(Dump, Error));
    bPassed &= TestFalse(
        TEXT("pre-existing unmanaged B is not Appeared in this generation"),
        Dump.Contains(TEXT("MH_W_PROBABLE_RESOURCE_RENAME")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexInvalidDictionaryRebuildTest,
    "Mimir.V4.ProjectIndex.InvalidDictionaryRowRebuildsCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexInvalidDictionaryRebuildTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString TexturePath = FPaths::Combine(Fixture.Root, TEXT("semantic_probe.png"));
    bool bPassed = WriteProjectIndexUtf8(TexturePath, TEXT("semantic-probe"));
    const FMHResourceKey ProbeKey = ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("semantic_probe"));
    {
        FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
        if (!OpenIndex(Index, *this)) return false;
        FMHProjectIndexUpdateResult Update;
        FString Error;
        bPassed &= TestTrue(TEXT("seed valid cache"), Index.FullScan({}, Update, Error));
        bPassed &= TestEqual(
            TEXT("probe starts unique"),
            Index.Resolve(ProbeKey).Status,
            EMHResolveStatus::Resolved);
        bPassed &= TestTrue(
            TEXT("inject invalid closed-dictionary value"),
            Index.InjectResolutionStatusForTests(ProbeKey, TEXT("garbage"), Error));
        Index.Close();
    }

    FMHProjectResourceIndex Reopened(Fixture.Root, Fixture.DatabasePath);
    bool bRecreated = false;
    FString Error;
    bPassed &= TestTrue(
        TEXT("invalid cache is replaced on open"),
        Reopened.Open(bRecreated, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= TestTrue(TEXT("semantic corruption forces rebuild"), bRecreated);
    bPassed &= TestEqual(
        TEXT("recreated empty cache has generation zero"),
        Reopened.GetGeneration(),
        static_cast<int64>(0));
    bPassed &= TestEqual(
        TEXT("corrupt ResourceKeys row is gone"),
        Reopened.Resolve(ProbeKey).Status,
        EMHResolveStatus::Unresolved);
    FMHProjectIndexUpdateResult Update;
    bPassed &= TestTrue(TEXT("reseed valid candidate projection"), Reopened.FullScan({}, Update, Error));
    bPassed &= TestEqual(
        TEXT("reseeded probe is unique"),
        Reopened.Resolve(ProbeKey).Status,
        EMHResolveStatus::Resolved);
    bPassed &= TestTrue(
        TEXT("inject closed but derived-inconsistent status"),
        Reopened.InjectResolutionStatusForTests(ProbeKey, TEXT("missing"), Error));
    Reopened.Close();

    FMHProjectResourceIndex DerivedReopened(Fixture.Root, Fixture.DatabasePath);
    bRecreated = false;
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("derived-inconsistent cache is replaced on open"),
        DerivedReopened.Open(bRecreated, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= TestTrue(TEXT("closed but wrong derived status forces rebuild"), bRecreated);
    bPassed &= TestEqual(
        TEXT("derived rebuild has generation zero"),
        DerivedReopened.GetGeneration(),
        static_cast<int64>(0));
    bPassed &= TestTrue(
        TEXT("inject extra schema column"),
        DerivedReopened.InjectExtraSchemaColumnForTests(Error));
    DerivedReopened.Close();

    FMHProjectResourceIndex SchemaReopened(Fixture.Root, Fixture.DatabasePath);
    bRecreated = false;
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("extra-column cache is replaced on open"),
        SchemaReopened.Open(bRecreated, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= TestTrue(TEXT("schema anomaly forces rebuild"), bRecreated);
    bPassed &= TestEqual(
        TEXT("schema rebuild has generation zero"),
        SchemaReopened.GetGeneration(),
        static_cast<int64>(0));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexOrdinaryStaleWarningTest,
    "Mimir.V4.ProjectIndex.OrdinaryStaleHasNoOrphanRebindWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexOrdinaryStaleWarningTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString TexturePath = FPaths::Combine(Fixture.Root, TEXT("ordinary_edit.png"));
    bool bPassed = WriteProjectIndexUtf8(TexturePath, TEXT("before-edit"));
    const FMHGeneratedAssetTagClaim TextureClaim = Claim(
        EMHResourceKind::Texture,
        TEXT("ordinary_edit"),
        TEXT("ordinary_edit.png"),
        FileHash(TexturePath));

    FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(Index, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    FString Error;
    bPassed &= TestTrue(TEXT("initial scan"), Index.FullScan({TextureClaim}, Update, Error));
    bPassed &= WriteProjectIndexUtf8(TexturePath, TEXT("ordinary-source-edit"));
    bPassed &= TestTrue(TEXT("edited source upsert"), Index.UpsertPaths({TexturePath}, Update, Error));

    FMHProjectIndexResolver Resolver(Index);
    FMHProjectIndexChangeDetector Detector(Index);
    FMHSourceAnalysis Analysis;
    Detector.DetectChanges(Resolver, Fixture.Root, Analysis);
    const FMHSourceAnalysisEntry* Entry = Analysis.Find(
        ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("ordinary_edit")));
    bPassed &= TestNotNull(TEXT("ordinary stale entry"), Entry);
    if (Entry != nullptr)
    {
        bPassed &= TestEqual(
            TEXT("ordinary edit remains an ordinary reimport"),
            Entry->Change,
            EMHSourceChange::Reimport);
    }
    const auto HasOrphanRebindWarning = [](const TArray<FString>& Warnings)
    {
        return Warnings.ContainsByPredicate([](const FString& Warning)
        {
            return Warning.Contains(TEXT("MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED"));
        });
    };
    bPassed &= TestFalse(
        TEXT("ordinary stale emits no orphan-rebind warning"),
        HasOrphanRebindWarning(Analysis.Warnings) ||
        (Entry != nullptr && HasOrphanRebindWarning(Entry->Warnings)));
    FString Event;
    bPassed &= TestFalse(
        TEXT("ordinary stale has no consumable orphan-rebind event"),
        Index.ConsumeOrphanRebindEvent(
            ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("ordinary_edit")), Event));
    bPassed &= TestTrue(TEXT("ordinary stale event is empty"), Event.IsEmpty());
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexOrphanRebindWarningTest,
    "Mimir.V4.ProjectIndex.DivergentOrphanRebindWarningLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexOrphanRebindWarningTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString TexturePath = FPaths::Combine(Fixture.Root, TEXT("orphan_rebind.png"));
    bool bPassed = WriteProjectIndexUtf8(TexturePath, TEXT("original-content"));
    const FString OriginalHash = FileHash(TexturePath);
    FMHGeneratedAssetTagClaim TextureClaim = Claim(
        EMHResourceKind::Texture,
        TEXT("orphan_rebind"),
        TEXT("orphan_rebind.png"),
        OriginalHash);

    FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(Index, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    FString Error;
    bPassed &= TestTrue(TEXT("initial scan"), Index.FullScan({TextureClaim}, Update, Error));
    bPassed &= TestTrue(
        TEXT("remove source"),
        IFileManager::Get().Delete(*TexturePath, false, true, true));
    bPassed &= TestTrue(TEXT("orphan transition upsert"), Index.UpsertPaths({TexturePath}, Update, Error));

    TArray<FMHProjectIndexGeneratedAssetState> Assets;
    bPassed &= TestTrue(
        TEXT("read orphan state"),
        Index.GetGeneratedAssets(
            ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("orphan_rebind")), Assets, Error));
    bPassed &= TestEqual(TEXT("one orphan claim"), Assets.Num(), 1);
    if (Assets.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("claim is proven orphan before rebound"),
            Assets[0].Status,
            EMHGeneratedAssetStatus::Orphan);
    }

    bPassed &= WriteProjectIndexUtf8(TexturePath, TEXT("divergent-rebound-content"));
    const FString ReboundHash = FileHash(TexturePath);
    bPassed &= TestTrue(TEXT("rebound source upsert"), Index.UpsertPaths({TexturePath}, Update, Error));

    FString Dump;
    bPassed &= TestTrue(TEXT("dump with pending rebound"), Index.BuildNormalizedDump(Dump, Error));
    bPassed &= TestFalse(
        TEXT("session-only rebound is absent from normalized dump"),
        Dump.Contains(TEXT("MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED")));

    const FMHResourceKey ReboundKey =
        ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("orphan_rebind"));
    FString Event;
    bPassed &= TestTrue(
        TEXT("divergent orphan rebound produces one consumable event"),
        Index.ConsumeOrphanRebindEvent(ReboundKey, Event));
    bPassed &= TestEqual(
        TEXT("event has canonical category and key"),
        Event,
        FString(TEXT("MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED: texture:orphan_rebind")));
    bPassed &= TestFalse(
        TEXT("orphan-rebind event is single-shot"),
        Index.ConsumeOrphanRebindEvent(ReboundKey, Event));
    bPassed &= TestTrue(TEXT("consumed event output is reset"), Event.IsEmpty());

    TextureClaim.SourceHash = ReboundHash;
    TextureClaim.AppliedHash = ReboundHash;
    bPassed &= TestTrue(
        TEXT("successful import receipt refresh"),
        Index.ReplaceGeneratedAssets({TextureClaim}, Update, Error));
    Dump.Reset();
    bPassed &= TestTrue(TEXT("dump after import"), Index.BuildNormalizedDump(Dump, Error));
    bPassed &= TestFalse(
        TEXT("successful import leaves no persisted rebound event"),
        Dump.Contains(TEXT("MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexOrphanRebindSessionLossTest,
    "Mimir.V4.ProjectIndex.OrphanRebindPendingStateIsSessionOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexOrphanRebindSessionLossTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    const FString TexturePath = FPaths::Combine(Fixture.Root, TEXT("session_rebind.png"));
    bool bPassed = WriteProjectIndexUtf8(TexturePath, TEXT("original-content"));
    const FMHGeneratedAssetTagClaim TextureClaim = Claim(
        EMHResourceKind::Texture,
        TEXT("session_rebind"),
        TEXT("session_rebind.png"),
        FileHash(TexturePath));
    const FMHResourceKey RebindKey =
        ProjectIndexTestKey(EMHResourceKind::Texture, TEXT("session_rebind"));

    FMHProjectResourceIndex Index(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(Index, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    FString Error;
    bPassed &= TestTrue(TEXT("initial scan"), Index.FullScan({TextureClaim}, Update, Error));
    bPassed &= TestTrue(TEXT("remove source"), IFileManager::Get().Delete(*TexturePath));
    bPassed &= TestTrue(TEXT("orphan upsert"), Index.UpsertPaths({TexturePath}, Update, Error));
    bPassed &= WriteProjectIndexUtf8(TexturePath, TEXT("first-divergent-rebound"));
    bPassed &= TestTrue(TEXT("first rebound upsert"), Index.UpsertPaths({TexturePath}, Update, Error));
    Index.Close();

    bool bRecreated = false;
    bPassed &= TestTrue(TEXT("reopen valid cache"), Index.Open(bRecreated, Error));
    bPassed &= TestFalse(TEXT("ordinary reopen does not rebuild cache"), bRecreated);
    FString Event;
    bPassed &= TestFalse(
        TEXT("restart loses pending rebound event"),
        Index.ConsumeOrphanRebindEvent(RebindKey, Event));

    bPassed &= TestTrue(TEXT("remove source again"), IFileManager::Get().Delete(*TexturePath));
    bPassed &= TestTrue(TEXT("second orphan upsert"), Index.UpsertPaths({TexturePath}, Update, Error));
    bPassed &= WriteProjectIndexUtf8(TexturePath, TEXT("second-divergent-rebound"));
    bPassed &= TestTrue(TEXT("second rebound upsert"), Index.UpsertPaths({TexturePath}, Update, Error));
    Index.Close();
    bPassed &= TestTrue(
        TEXT("delete cache"),
        IFileManager::Get().Delete(*Fixture.DatabasePath, false, true, true));

    FMHProjectResourceIndex Rebuilt(Fixture.Root, Fixture.DatabasePath);
    bRecreated = false;
    bPassed &= TestTrue(TEXT("open deleted cache"), Rebuilt.Open(bRecreated, Error));
    bPassed &= TestTrue(TEXT("deleted cache is recreated"), bRecreated);
    bPassed &= TestTrue(TEXT("rebuild source projection"), Rebuilt.FullScan({TextureClaim}, Update, Error));
    bPassed &= TestFalse(
        TEXT("delete and rebuild loses pending rebound event"),
        Rebuilt.ConsumeOrphanRebindEvent(RebindKey, Event));
    FString Dump;
    bPassed &= TestTrue(TEXT("rebuilt dump"), Rebuilt.BuildNormalizedDump(Dump, Error));
    bPassed &= TestFalse(
        TEXT("rebuilt dump contains no session event"),
        Dump.Contains(TEXT("MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHProjectIndexTenThousandResolveTest,
    "Mimir.V4.ProjectIndex.TenThousandIndexedResolve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHProjectIndexTenThousandResolveTest::RunTest(const FString& Parameters)
{
    FIndexFixture Fixture;
    constexpr int32 ResourceCount = 10000;
    bool bPassed = true;
    for (int32 Index = 0; Index < ResourceCount; ++Index)
    {
        const FString Name = FString::Printf(TEXT("texture_%05d"), Index);
        bPassed &= WriteProjectIndexUtf8(FPaths::Combine(Fixture.Root, Name + TEXT(".png")), Name);
    }
    FMHProjectResourceIndex ProjectIndex(Fixture.Root, Fixture.DatabasePath);
    if (!OpenIndex(ProjectIndex, *this)) return false;
    FMHProjectIndexUpdateResult Update;
    FString Error;
    bPassed &= TestTrue(TEXT("10k full scan"), ProjectIndex.FullScan({}, Update, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= TestEqual(TEXT("one full scan"), ProjectIndex.GetFullScanCountForTests(), 1);
    FMHProjectIndexResolver Resolver(ProjectIndex);
    for (int32 Index = 0; Index < ResourceCount; ++Index)
    {
        const FString Name = FString::Printf(TEXT("texture_%05d"), Index);
        if (Resolver.Resolve(ProjectIndexTestKey(EMHResourceKind::Texture, Name)).Status != EMHResolveStatus::Resolved)
        {
            AddError(FString::Printf(TEXT("indexed resolve failed for %s"), *Name));
            bPassed = false;
            break;
        }
    }
    bPassed &= TestEqual(
        TEXT("resolves never trigger another source scan"),
        ProjectIndex.GetFullScanCountForTests(),
        1);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
