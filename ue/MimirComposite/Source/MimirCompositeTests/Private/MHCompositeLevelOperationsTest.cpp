#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeProtocol.h"

#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeLevelOperationsTest,
    "Mimir.V4.Composite.LevelOperations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeLevelOperationsTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests"),
        TEXT("s6_level_ops_") + Suffix);
    IFileManager::Get().MakeDirectory(*SourceRoot, true);

    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const FDirectoryPath PreviousRoot = Settings->SourceRoot;
    const TMap<FString, FSoftClassPath> PreviousRegistry = Settings->ActorClassRegistry;
    Settings->SourceRoot.Path = SourceRoot;
    Settings->ActorClassRegistry.Reset();
    Settings->ActorClassRegistry.Add(
        TEXT("s6_actor"),
        FSoftClassPath(AStaticMeshActor::StaticClass()));

    bool bPassed = true;
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    bPassed &= TestNotNull(TEXT("level-operation editor world exists"), World);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>()
        : nullptr;
    bPassed &= TestNotNull(TEXT("level-operation subsystem exists"), Subsystem);
    if (World == nullptr || Subsystem == nullptr)
    {
        Settings->SourceRoot = PreviousRoot;
        Settings->ActorClassRegistry = PreviousRegistry;
        if (World != nullptr) World->DestroyWorld(false);
        IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
        return false;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transactional | RF_Transient;
    AActor* Unsupported = World->SpawnActor<AActor>(
        AActor::StaticClass(),
        FTransform::Identity,
        SpawnParameters);
    FMHCompositeAdoptTarget RejectTarget;
    RejectTarget.Folder = SourceRoot;
    RejectTarget.LogicalName = TEXT("unrepresentable_") + Suffix;
    AMHCompositeActor* RejectedResult = nullptr;
    TArray<FString> Warnings;
    FString Error;
    bPassed &= TestFalse(
        TEXT("Build rejects an unregistered level actor before publication"),
        Subsystem->BuildComposite(
            {Unsupported},
            RejectTarget,
            RejectedResult,
            Warnings,
            Error));
    bPassed &= TestTrue(
        TEXT("Build rejection is machine-readable and names the object"),
        Error.StartsWith(TEXT("MH_E_UNREPRESENTABLE_SCENE_OBJECT:")) &&
        Error.Contains(Unsupported->GetPathName()));
    bPassed &= TestFalse(
        TEXT("failed Build leaves no source document"),
        IFileManager::Get().FileExists(*FPaths::Combine(
            SourceRoot,
            RejectTarget.LogicalName + TEXT(".composite"))));

    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(GetTransientPackage());
    Asset->LogicalName = TEXT("level_ops_") + Suffix;
    const FString MeshResource = TEXT("level_ops_mesh_") + Suffix;
    const FString NestedResource = TEXT("level_ops_nested_") + Suffix;
    UPackage* MeshPackage = CreatePackage(*FString::Printf(
        TEXT("/Game/MH/Generated/Meshes/%s"),
        *MeshResource));
    UStaticMesh* MeshAsset = NewObject<UStaticMesh>(
        MeshPackage,
        FName(*MeshResource),
        RF_Public | RF_Standalone);
    UPackage* NestedPackage = CreatePackage(*FString::Printf(
        TEXT("/Game/MH/Generated/Composites/%s"),
        *NestedResource));
    UMHCompositeAsset* NestedAsset = NewObject<UMHCompositeAsset>(
        NestedPackage,
        FName(*NestedResource),
        RF_Public | RF_Standalone);
    NestedAsset->LogicalName = NestedResource;
    FMHCompositeDocument EmptyNestedDocument;
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("nested Break fixture applies"),
        MHApplyCompositeV4(*NestedAsset, EmptyNestedDocument, Error));

    FMHCompositeDocument Document;
    FMHCompositeNode Node;
    Node.Kind = EMHCompositeNodeKind::Actor;
    Node.Resource = TEXT("s6_actor");
    Node.Name = TEXT("authored_actor");
    Node.Transform.TranslationCm = FVector(25.0, 0.0, 0.0);
    Document.Nodes.Add(Node);
    FMHCompositeNode MeshNode;
    MeshNode.Kind = EMHCompositeNodeKind::Mesh;
    MeshNode.Resource = MeshResource;
    MeshNode.Name = TEXT("authored_mesh");
    Document.Nodes.Add(MeshNode);
    FMHCompositeNode NestedNode;
    NestedNode.Kind = EMHCompositeNodeKind::Composite;
    NestedNode.Resource = NestedResource;
    NestedNode.Name = TEXT("authored_nested");
    Document.Nodes.Add(NestedNode);
    FMHCompositeNode FallbackMeshNode;
    FallbackMeshNode.Kind = EMHCompositeNodeKind::Mesh;
    FallbackMeshNode.Resource = MeshResource;
    Document.Nodes.Add(FallbackMeshNode);
    Error.Reset();
    bPassed &= TestTrue(
        TEXT("level-operation fixture applies"),
        MHApplyCompositeV4(*Asset, Document, Error));

    AMHCompositeActor* CompositeActor = World->SpawnActor<AMHCompositeActor>(
        AMHCompositeActor::StaticClass(),
        FTransform(FRotator::ZeroRotator, FVector(100.0, 0.0, 0.0)),
        SpawnParameters);
    bPassed &= TestNotNull(TEXT("composite actor spawns"), CompositeActor);
    if (CompositeActor != nullptr)
    {
        CompositeActor->SetCompositeAsset(Asset);
        bPassed &= TestEqual(
            TEXT("one top-level placement is compiled"),
            CompositeActor->GetTopLevelComponents().Num(),
            4);
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Edit unlocks top-level transforms"),
            Subsystem->BeginEditComposite(CompositeActor, Error));
        bPassed &= TestTrue(TEXT("actor records active edit mode"), CompositeActor->IsPlacementEditMode());
        TArray<AActor*> UnsafeBreakActors;
        Error.Reset();
        bPassed &= TestFalse(
            TEXT("Refresh cannot invalidate the active edit component snapshot"),
            Subsystem->RebuildComposites({CompositeActor}, Warnings, Error));
        bPassed &= TestTrue(TEXT("blocked Refresh names the active edit"), Error.Contains(TEXT("active composite edit")));
        Error.Reset();
        bPassed &= TestFalse(
            TEXT("Break cannot destroy the actor in an active edit session"),
            Subsystem->BreakComposites({CompositeActor}, UnsafeBreakActors, Warnings, Error));
        bPassed &= TestTrue(TEXT("blocked Break names the active edit"), Error.Contains(TEXT("active composite edit")));
        Error.Reset();
        AMHCompositeActor* UnsafeBuildResult = nullptr;
        bPassed &= TestFalse(
            TEXT("Build is unavailable while a composite edit session is active"),
            Subsystem->BuildComposite(
                {Unsupported},
                RejectTarget,
                UnsafeBuildResult,
                Warnings,
                Error));
        bPassed &= TestTrue(TEXT("blocked Build names the active edit"), Error.Contains(TEXT("active composite edit")));
        bPassed &= TestTrue(TEXT("blocked operations preserve edit mode"), CompositeActor->IsPlacementEditMode());
        if (CompositeActor->GetTopLevelComponents().Num() == 4)
        {
            CompositeActor->GetTopLevelComponents()[0]->SetWorldLocation(FVector(999.0, 0.0, 0.0));
        }
        Error.Reset();
        bPassed &= TestTrue(TEXT("Cancel closes edit mode"), Subsystem->CancelEditComposite(Error));
        bPassed &= TestFalse(TEXT("Cancel reseals the actor"), CompositeActor->IsPlacementEditMode());
        if (CompositeActor->GetTopLevelComponents().Num() == 4)
        {
            bPassed &= TestTrue(
                TEXT("Cancel rebuild restores authored placement"),
                CompositeActor->GetTopLevelComponents()[0]->GetComponentLocation().Equals(
                    FVector(125.0, 0.0, 0.0),
                    KINDA_SMALL_NUMBER));
            CompositeActor->GetTopLevelComponents()[0]->SetWorldLocation(FVector(777.0, 0.0, 0.0));
        }
        FMHCompositeDocument BeforeRefreshDocument;
        TArray<uint8> BeforeRefreshBytes;
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Refresh invariant fixture extracts"),
            MHExtractCompositeV4(*Asset, BeforeRefreshDocument, Error) &&
            MHWriteCanonicalCompositeV4(BeforeRefreshDocument, BeforeRefreshBytes, Error));
        const FString BeforeRefreshSourceHash = Asset->SourceHash;
        const FString BeforeRefreshAppliedHash = Asset->AppliedHash;
        const bool bPackageWasDirty = Asset->GetOutermost()->IsDirty();
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("explicit Rebuild succeeds"),
            Subsystem->RebuildComposites({CompositeActor}, Warnings, Error));
        FMHCompositeDocument AfterRefreshDocument;
        TArray<uint8> AfterRefreshBytes;
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Refresh leaves the managed document extractable"),
            MHExtractCompositeV4(*Asset, AfterRefreshDocument, Error) &&
            MHWriteCanonicalCompositeV4(AfterRefreshDocument, AfterRefreshBytes, Error));
        bPassed &= TestTrue(TEXT("Refresh does not mutate canonical asset data"), BeforeRefreshBytes == AfterRefreshBytes);
        bPassed &= TestEqual(TEXT("Refresh preserves SourceHash"), Asset->SourceHash, BeforeRefreshSourceHash);
        bPassed &= TestEqual(TEXT("Refresh preserves AppliedHash"), Asset->AppliedHash, BeforeRefreshAppliedHash);
        bPassed &= TestEqual(TEXT("Refresh preserves package dirty state"), Asset->GetOutermost()->IsDirty(), bPackageWasDirty);
        if (CompositeActor->GetTopLevelComponents().Num() == 4)
        {
            bPassed &= TestTrue(
                TEXT("derived edits outside Edit do not survive Rebuild"),
                CompositeActor->GetTopLevelComponents()[0]->GetComponentLocation().Equals(
                    FVector(125.0, 0.0, 0.0),
                    KINDA_SMALL_NUMBER));
        }

        TArray<AActor*> BrokenActors;
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Break removes one composite layer"),
            Subsystem->BreakComposites({CompositeActor}, BrokenActors, Warnings, Error));
        bPassed &= TestEqual(TEXT("Break creates every authored placement actor"), BrokenActors.Num(), 4);
        TMap<FString, AActor*> ActorsByLabel;
        for (AActor* BrokenActor : BrokenActors)
        {
            if (BrokenActor != nullptr)
            {
                ActorsByLabel.Add(BrokenActor->GetActorLabel(false), BrokenActor);
            }
        }
        bPassed &= TestTrue(TEXT("Break restores authored mesh name"), ActorsByLabel.Contains(TEXT("authored_mesh")));
        bPassed &= TestTrue(TEXT("Break restores authored nested-composite name"), ActorsByLabel.Contains(TEXT("authored_nested")));
        bPassed &= TestTrue(TEXT("Break falls back to mesh resource name"), ActorsByLabel.Contains(MeshResource));
        if (AActor** AuthoredActor = ActorsByLabel.Find(TEXT("authored_actor")))
        {
            bPassed &= TestTrue(
                TEXT("Break preserves authored world placement"),
                (*AuthoredActor)->GetActorLocation().Equals(
                    FVector(125.0, 0.0, 0.0),
                    KINDA_SMALL_NUMBER));
        }
        else
        {
            bPassed &= TestTrue(TEXT("Break restores the authored actor display label"), false);
        }
    }

    World->DestroyWorld(false);
    Settings->SourceRoot = PreviousRoot;
    Settings->ActorClassRegistry = PreviousRegistry;
    MeshAsset->ClearFlags(RF_Public | RF_Standalone);
    MeshAsset->MarkAsGarbage();
    NestedAsset->ClearFlags(RF_Public | RF_Standalone);
    NestedAsset->MarkAsGarbage();
    IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
