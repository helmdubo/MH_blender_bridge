#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceComposition.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

FString BuildPreflightUniqueName(const TCHAR* Prefix)
{
    return FString(Prefix) + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
}

void BuildPreflightRetireAsset(UObject* Asset)
{
    if (Asset == nullptr) return;
    FAssetRegistryModule::AssetDeleted(Asset);
    Asset->ClearFlags(RF_Public | RF_Standalone);
    Asset->MarkAsGarbage();
}

UMHCompositeAsset* BuildPreflightAsset(const FString& Name, const FString& Folder = TEXT("/Game/MH/Generated/Composites/"))
{
    UPackage* Package = CreatePackage(*(Folder + Name));
    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(Package, FName(*Name), RF_Public | RF_Standalone);
    Asset->LogicalName = Name;
    Asset->SourceRelativePath = Name + TEXT(".composite");
    TArray<uint8> Bytes;
    FString Error;
    MHWriteCanonicalCompositeV5(FMHCompositeDocument(), Bytes, Error);
    Asset->SourceHash = MHRawPayloadHash(Bytes);
    Asset->AppliedHash = Asset->SourceHash;
    FAssetRegistryModule::AssetCreated(Asset);
    return Asset;
}

bool BuildPreflightWrite(const FString& Path, const FMHCompositeDocument& Document)
{
    TArray<uint8> Bytes;
    FString Error;
    return MHWriteCanonicalCompositeV5(Document, Bytes, Error) &&
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true) &&
        FFileHelper::SaveArrayToFile(Bytes, *Path);
}

struct FBuildPreflightFixture
{
    FString SourceRoot;
    FDirectoryPath PreviousRoot;
    UWorld* World = nullptr;
    TArray<UObject*> Assets;

    FBuildPreflightFixture()
    {
        SourceRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests"), BuildPreflightUniqueName(TEXT("build_preflight_")));
        IFileManager::Get().MakeDirectory(*SourceRoot, true);
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        PreviousRoot = Settings->SourceRoot;
        Settings->SourceRoot.Path = SourceRoot;
        World = UWorld::CreateWorld(EWorldType::Editor, false);
    }

    ~FBuildPreflightFixture()
    {
        if (World != nullptr) World->DestroyWorld(false);
        for (UObject* Asset : Assets) BuildPreflightRetireAsset(Asset);
        MHShutdownProjectIndex();
        GetMutableDefault<UMHCompositeSettings>()->SourceRoot = PreviousRoot;
        IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    }
};

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeBuildPreflightRejectsBeforeMutationTest,
    "Mimir.V5.Composite.BuildPreflightRejectsBeforeMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeBuildPreflightRejectsBeforeMutationTest::RunTest(const FString& Parameters)
{
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("Build subsystem exists"), Subsystem)) return false;
    bool bPassed = true;
    const TArray<FString> Cases = {
        TEXT("missing_composite"), TEXT("unselected_option"), TEXT("ambiguous_option"),
        TEXT("invalid_option"), TEXT("missing_profile"), TEXT("missing_mesh"), TEXT("unmanaged_mesh")};
    for (const FString& Case : Cases)
    {
        FBuildPreflightFixture Fixture;
        if (!TestNotNull(*Case, Fixture.World)) return false;
        const FString InputName = BuildPreflightUniqueName(TEXT("build_input_"));
        const FString DependencyName = BuildPreflightUniqueName(TEXT("build_dependency_"));
        const FString OutputName = BuildPreflightUniqueName(TEXT("build_output_"));
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = RF_Transactional | RF_Transient;
        AActor* Input = nullptr;
        FString ExpectedCode = TEXT("MH_E_RESOURCE_NOT_FOUND");
        if (Case == TEXT("missing_mesh") || Case == TEXT("unmanaged_mesh"))
        {
            UPackage* Package = CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + InputName));
            UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, FName(*InputName), RF_Public | RF_Standalone);
            Fixture.Assets.Add(Mesh);
            if (Case == TEXT("missing_mesh"))
            {
                UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
                Receipt->LogicalName = InputName;
                Receipt->SourceRelativePath = InputName + TEXT(".mesh.fbx");
                Receipt->SourceHash = MHRawPayloadHash(TArray<uint8>{1, 2, 3});
                Receipt->ImporterVersion = MHStaticMeshImporterVersion;
                Mesh->SetAssetImportData(Receipt);
            }
            else ExpectedCode = TEXT("MH_E_UNREPRESENTABLE_SCENE_OBJECT");
            FAssetRegistryModule::AssetCreated(Mesh);
            AStaticMeshActor* MeshActor = Fixture.World->SpawnActor<AStaticMeshActor>(SpawnParameters);
            if (!TestNotNull(TEXT("mesh input actor exists"), MeshActor)) return false;
            MeshActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
            Input = MeshActor;
        }
        else
        {
            UMHCompositeAsset* Asset = BuildPreflightAsset(InputName);
            Fixture.Assets.Add(Asset);
            // A valid old applied preview is deliberate: admission must inspect
            // the current source closure, not assume this UAsset proves it.
            FMHCompositeDocument SourceDocument;
            if (Case == TEXT("missing_profile"))
            {
                FMHCompositeNode ProfileNode;
                ProfileNode.Kind = EMHCompositeNodeKind::Group;
                ProfileNode.Profile = DependencyName;
                SourceDocument.Nodes.Add(ProfileNode);
            }
            else if (Case != TEXT("missing_composite"))
            {
                FMHCompositeNode Random;
                Random.Kind = EMHCompositeNodeKind::Random;
                FMHCompositeOption Empty;
                Empty.Kind = EMHCompositeOptionKind::Empty;
                Empty.Weight = 1.0f;
                Random.Options.Add(Empty);
                FMHCompositeOption Unselected;
                Unselected.Kind = EMHCompositeOptionKind::Composite;
                Unselected.Resource = DependencyName;
                Unselected.Weight = 0.0f;
                Random.Options.Add(Unselected);
                SourceDocument.Nodes.Add(Random);
            }
            if (Case != TEXT("missing_composite"))
            {
                bPassed &= TestTrue(*FString::Printf(TEXT("%s source is written"), *Case),
                    BuildPreflightWrite(FPaths::Combine(Fixture.SourceRoot, InputName + TEXT(".composite")), SourceDocument));
            }
            if (Case == TEXT("ambiguous_option"))
            {
                ExpectedCode = TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME");
                for (const TCHAR* Folder : {TEXT("one"), TEXT("two")})
                {
                    bPassed &= TestTrue(TEXT("duplicate source fixture is written"), BuildPreflightWrite(
                        FPaths::Combine(Fixture.SourceRoot, Folder, DependencyName + TEXT(".composite")), FMHCompositeDocument()));
                }
            }
            if (Case == TEXT("invalid_option"))
            {
                ExpectedCode = TEXT("MH_E_SOURCE_INDEX_INVALID");
                bPassed &= TestTrue(TEXT("legacy-generation source fixture is written"), FFileHelper::SaveStringToFile(
                    TEXT("{\"nodes\":[]}"), *FPaths::Combine(Fixture.SourceRoot, DependencyName + TEXT(".composite")),
                    FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            }
            AMHCompositeActor* Composite = Fixture.World->SpawnActor<AMHCompositeActor>(SpawnParameters);
            if (!TestNotNull(TEXT("composite input actor exists"), Composite)) return false;
            Composite->SetCompositeAsset(Asset);
            Input = Composite;
        }

        Input->SetActorLocation(FVector(25.0, 50.0, 75.0));
        const FTransform PreviousTransform = Input->GetActorTransform();
        const TSet<UActorComponent*> PreviousComponents = Input->GetComponents();
        TArray<FString> FilesBefore;
        IFileManager::Get().FindFilesRecursive(FilesBefore, *Fixture.SourceRoot, TEXT("*"), true, false, false);
        FilesBefore.Sort();
        const FMHCompositeAdoptTarget Target{FPaths::Combine(Fixture.SourceRoot, TEXT("must_not_be_created")), OutputName};
        AMHCompositeActor* Output = nullptr;
        TArray<FString> Warnings;
        FString Error;
        bPassed &= TestFalse(*FString::Printf(TEXT("%s Build is rejected"), *Case), Subsystem->BuildComposite({Input}, Target, Output, Warnings, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("%s diagnostic: %s"), *Case, *Error), Error.Contains(ExpectedCode));
        bPassed &= TestNull(TEXT("rejected Build returns no output actor"), Output);
        bPassed &= TestFalse(TEXT("input actor is not destroyed"), Input->IsActorBeingDestroyed());
        bPassed &= TestTrue(TEXT("input transform is unchanged"), PreviousTransform.Equals(Input->GetActorTransform()));
        bool bSameComponents = PreviousComponents.Num() == Input->GetComponents().Num();
        for (UActorComponent* Component : PreviousComponents) bSameComponents &= Input->GetComponents().Contains(Component);
        bPassed &= TestTrue(TEXT("input components are unchanged"), bSameComponents);
        TArray<FString> FilesAfter;
        IFileManager::Get().FindFilesRecursive(FilesAfter, *Fixture.SourceRoot, TEXT("*"), true, false, false);
        FilesAfter.Sort();
        bPassed &= TestTrue(TEXT("preflight failure writes no source payloads"), FilesBefore == FilesAfter);
        bPassed &= TestFalse(TEXT("preflight failure creates no destination directory"), IFileManager::Get().DirectoryExists(*Target.Folder));
        const FString PackageName = TEXT("/Game/MH/Generated/Composites/") + OutputName;
        bPassed &= TestNull(TEXT("preflight failure creates no UObject package"), FindPackage(nullptr, *PackageName));
        bPassed &= TestFalse(TEXT("preflight failure creates no UAsset file"), FPackageName::DoesPackageExist(PackageName));
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeAppliedLookupSnapshotTest,
    "Mimir.V5.Composite.AppliedLookupOneRegistrySnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeAppliedLookupSnapshotTest::RunTest(const FString& Parameters)
{
    const FString Name = BuildPreflightUniqueName(TEXT("lookup_unique_"));
    UMHCompositeAsset* Asset = BuildPreflightAsset(Name);
    int32 TagExtractions = 0;
    const FDelegateHandle Handle = UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.AddLambda(
        [Asset, &TagExtractions](FAssetRegistryTagsContext Context)
        {
            if (Context.GetObject() == Asset) ++TagExtractions;
        });
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = Name;
    FString Error;
    FMHAppliedResourceLookup Lookup;
    bool bPassed = TestEqual(TEXT("unique canonical generated claim resolves"), Lookup.Load(Key, Error), static_cast<UObject*>(Asset));
    const int32 FirstExtractions = TagExtractions;
    bPassed &= TestTrue(TEXT("the snapshot includes the live asset"), FirstExtractions > 0);
    bPassed &= TestEqual(TEXT("repeated query returns the same asset"), Lookup.Load(Key, Error), static_cast<UObject*>(Asset));
    bPassed &= TestEqual(TEXT("repeated resource lookup does not re-extract global live tags"), TagExtractions, FirstExtractions);
    UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.Remove(Handle);
    BuildPreflightRetireAsset(Asset);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeAppliedLookupDuplicateClaimsTest,
    "Mimir.V5.Composite.AppliedLookupPreservesGlobalDuplicateClaims",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeAppliedLookupDuplicateClaimsTest::RunTest(const FString& Parameters)
{
    const FString Name = BuildPreflightUniqueName(TEXT("lookup_duplicate_"));
    UMHCompositeAsset* Asset = BuildPreflightAsset(Name);
    const FString WrongName = BuildPreflightUniqueName(TEXT("wrong_carrier_"));
    UPackage* Package = CreatePackage(*(TEXT("/Game/MimirLookupTests/") + WrongName));
    UTexture2D* WrongCarrier = NewObject<UTexture2D>(Package, FName(*WrongName), RF_Public | RF_Standalone);
    const FDelegateHandle Handle = UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.AddLambda(
        [WrongCarrier, Name](FAssetRegistryTagsContext Context)
        {
            if (Context.GetObject() != WrongCarrier) return;
            Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.Kind"), TEXT("composite"), UObject::FAssetRegistryTag::TT_Alphabetical));
            Context.AddTag(UObject::FAssetRegistryTag(TEXT("MH.LogicalName"), Name, UObject::FAssetRegistryTag::TT_Alphabetical));
        });
    FAssetRegistryModule::AssetCreated(WrongCarrier);
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = Name;
    FString Error;
    FMHAppliedResourceLookup Lookup;
    bool bPassed = TestNull(TEXT("wrong-class out-of-folder claim is not hidden by a query filter"), Lookup.Load(Key, Error));
    bPassed &= TestTrue(TEXT("all claimants fail closed without choosing canonical-path winner"), Error.StartsWith(TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET:")));
    UObject::FAssetRegistryTag::OnGetExtraObjectTagsWithContext.Remove(Handle);
    BuildPreflightRetireAsset(WrongCarrier);
    BuildPreflightRetireAsset(Asset);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
