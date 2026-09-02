#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Editor/Transactor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"
#include "ScopedTransaction.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"

namespace UE::MimirComposite::Tests
{

namespace
{

UMHCompositeAsset* MHNewLevelOperationFixture(const FString& Name)
{
    UPackage* Package = CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + Name));
    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(Package, FName(*Name), RF_Public | RF_Standalone);
    Asset->LogicalName = Name;
    return Asset;
}

bool MHApplyLevelOperationFixture(
    UMHCompositeAsset& Asset,
    const FMHCompositeDocument& Document,
    FString& OutError)
{
    TArray<uint8> Bytes;
    if (!MHApplyCompositeV5(Asset, Document, OutError) ||
        !MHWriteCanonicalCompositeV5(Document, Bytes, OutError))
    {
        return false;
    }
    Asset.SourceRelativePath = Asset.LogicalName + TEXT(".composite");
    Asset.SourceHash = MHRawPayloadHash(Bytes);
    Asset.AppliedHash = Asset.SourceHash;
    return true;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeLevelOperationsTest,
    "Mimir.V5.Composite.LevelOperations",
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

    UMHCompositeAsset* Asset = MHNewLevelOperationFixture(TEXT("level_ops_") + Suffix);
    const FString MeshResource = TEXT("level_ops_mesh_") + Suffix;
    const FString NestedResource = TEXT("level_ops_nested_") + Suffix;
    UPackage* MeshPackage = CreatePackage(*FString::Printf(
        TEXT("/Game/MH/Generated/Meshes/%s"),
        *MeshResource));
    UStaticMesh* MeshAsset = NewObject<UStaticMesh>(
        MeshPackage,
        FName(*MeshResource),
        RF_Public | RF_Standalone);
    UMHStaticMeshImportData* MeshReceipt = NewObject<UMHStaticMeshImportData>(MeshAsset);
    MeshReceipt->LogicalName = MeshResource;
    MeshReceipt->SourceRelativePath = MeshResource + TEXT(".mesh.fbx");
    const TArray<uint8> AppliedMeshBytes = {0x4d, 0x48, 0x35};
    MeshReceipt->SourceHash = MHRawPayloadHash(AppliedMeshBytes);
    MeshReceipt->ImporterVersion = MHStaticMeshImporterVersion;
    MeshAsset->SetAssetImportData(MeshReceipt);
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
        MHApplyLevelOperationFixture(*NestedAsset, EmptyNestedDocument, Error));

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
        MHApplyLevelOperationFixture(*Asset, Document, Error));

    AMHCompositeActor* CompositeActor = World->SpawnActor<AMHCompositeActor>(
        AMHCompositeActor::StaticClass(),
        FTransform(FRotator::ZeroRotator, FVector(100.0, 0.0, 0.0)),
        SpawnParameters);
    bPassed &= TestNotNull(TEXT("composite actor spawns"), CompositeActor);
    if (CompositeActor != nullptr)
    {
        CompositeActor->SetCompositeAsset(Asset);
        bPassed &= TestNotNull(TEXT("managed receipts produce the cached preview plan"), CompositeActor->GetResolvedPlan());
        bPassed &= TestEqual(
            TEXT("one top-level placement is compiled"),
            CompositeActor->GetTopLevelComponents().Num(),
            4);

        AMHCompositeActor* MissingPlanActor = World->SpawnActor<AMHCompositeActor>(
            AMHCompositeActor::StaticClass(), FTransform::Identity, SpawnParameters);
        const TArray<TObjectPtr<UActorComponent>> ComponentsBeforeFailedBreak = CompositeActor->GetDerivedComponents();
        TArray<AActor*> RejectedBreakActors;
        Error.Reset();
        bPassed &= TestFalse(
            TEXT("Break rejects the full selection when one placement has no cached plan"),
            Subsystem->BreakComposites({CompositeActor, MissingPlanActor}, RejectedBreakActors, Warnings, Error));
        bPassed &= TestTrue(TEXT("missing-plan rejection names resolved plan"), Error.Contains(TEXT("resolved plan")));
        bPassed &= TestTrue(TEXT("failed selection preflight spawns nothing"), RejectedBreakActors.IsEmpty());
        bPassed &= TestFalse(TEXT("failed selection preflight preserves first actor"), CompositeActor->IsActorBeingDestroyed());
        bPassed &= TestTrue(TEXT("failed selection preflight preserves preview components"),
            ComponentsBeforeFailedBreak == CompositeActor->GetDerivedComponents());
        if (MissingPlanActor != nullptr) MissingPlanActor->Destroy();

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
        }

        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Edit can restart for irreversible Commit boundary gate"),
            Subsystem->BeginEditComposite(CompositeActor, Error));
        if (CompositeActor->GetTopLevelComponents().Num() == 4)
        {
            USceneComponent* EditedComponent = CompositeActor->GetTopLevelComponents()[0];
            {
                const FScopedTransaction UserTransformTransaction(
                    INVTEXT("Automation composite placement edit"));
                EditedComponent->Modify();
                EditedComponent->SetWorldLocation(FVector(126.0, 0.0, 0.0));
                EditedComponent->SetWorldLocation(FVector(125.0, 0.0, 0.0));
            }
        }
        bool bPublisherObservedClosedTransaction = false;
        Subsystem->SetCommitPublisherForTests(
            [&bPublisherObservedClosedTransaction](UMHCompositeAsset&, FString&)
            {
                bPublisherObservedClosedTransaction =
                    GEditor != nullptr &&
                    !GEditor->IsTransactionActive() &&
                    GEditor->Trans != nullptr &&
                    !GEditor->Trans->CanUndo();
                return true;
            });
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Commit succeeds through the source-publish seam"),
            Subsystem->CommitEditComposite(Warnings, Error));
        bPassed &= TestTrue(
            TEXT("Commit clears UE Undo before the source-publish seam"),
            bPublisherObservedClosedTransaction);
        bPassed &= TestFalse(
            TEXT("Ctrl+Z cannot resurrect a pre-Commit placement edit"),
            GEditor->UndoTransaction());
        Subsystem->SetCommitPublisherForTests({});
        if (CompositeActor->GetTopLevelComponents().Num() == 4)
        {
            CompositeActor->GetTopLevelComponents()[0]->SetWorldLocation(FVector(777.0, 0.0, 0.0));
        }
        FMHCompositeDocument BeforeRefreshDocument;
        TArray<uint8> BeforeRefreshBytes;
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Refresh invariant fixture extracts"),
            MHExtractCompositeV5(*Asset, BeforeRefreshDocument, Error) &&
            MHWriteCanonicalCompositeV5(BeforeRefreshDocument, BeforeRefreshBytes, Error));
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
            MHExtractCompositeV5(*Asset, AfterRefreshDocument, Error) &&
            MHWriteCanonicalCompositeV5(AfterRefreshDocument, AfterRefreshBytes, Error));
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

        UMHCompositeAsset* ParentLocalAsset = MHNewLevelOperationFixture(TEXT("parent_local_group_") + Suffix);
        FMHCompositeDocument ParentLocalDocument;
        FMHCompositeNode ParentNode;
        ParentNode.Kind = EMHCompositeNodeKind::Group;
        ParentNode.Name = TEXT("translated_parent");
        ParentNode.Transform.TranslationCm = FVector(100.0, 0.0, 0.0);
        FMHCompositeNode LocalChild;
        LocalChild.Kind = EMHCompositeNodeKind::Mesh;
        LocalChild.Resource = MeshResource;
        LocalChild.Name = TEXT("local_child");
        LocalChild.Transform.TranslationCm = FVector(25.0, 0.0, 0.0);
        ParentNode.Children.Add(LocalChild);
        ParentLocalDocument.Nodes.Add(ParentNode);
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("parent-local group fixture applies"),
            MHApplyLevelOperationFixture(*ParentLocalAsset, ParentLocalDocument, Error));
        AMHCompositeActor* ParentLocalActor = World->SpawnActor<AMHCompositeActor>(
            AMHCompositeActor::StaticClass(),
            FTransform::Identity,
            SpawnParameters);
        ParentLocalActor->SetCompositeAsset(ParentLocalAsset);
        TArray<AActor*> ParentLocalBreakActors;
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Break accumulates and dissolves the parent group"),
            Subsystem->BreakComposites(
                {ParentLocalActor},
                ParentLocalBreakActors,
                Warnings,
                Error));
        bPassed &= TestEqual(
            TEXT("Break emits only the resolved child"),
            ParentLocalBreakActors.Num(),
            1);
        if (ParentLocalBreakActors.Num() == 1)
        {
            bPassed &= TestTrue(
                TEXT("parent 100 plus child local 25 equals world 125"),
                ParentLocalBreakActors[0]->GetActorLocation().Equals(
                    FVector(125.0, 0.0, 0.0),
                    KINDA_SMALL_NUMBER));
            ParentLocalBreakActors[0]->Destroy();
        }

        UMHCompositeAsset* ShearAsset = MHNewLevelOperationFixture(TEXT("break_shear_") + Suffix);
        FMHCompositeDocument ShearDocument;
        FMHCompositeNode RotatedMesh = MeshNode;
        RotatedMesh.Transform.RotationQuat = FQuat(FRotator(0.0, 45.0, 0.0));
        ShearDocument.Nodes.Add(RotatedMesh);
        Error.Reset();
        bPassed &= TestTrue(TEXT("Break shear fixture applies"), MHApplyLevelOperationFixture(*ShearAsset, ShearDocument, Error));
        AMHCompositeActor* ShearActor = World->SpawnActor<AMHCompositeActor>(
            AMHCompositeActor::StaticClass(), FTransform::Identity, SpawnParameters);
        ShearActor->SetCompositeAsset(ShearAsset);
        bPassed &= TestNotNull(TEXT("shear fixture starts with a valid plan"), ShearActor->GetResolvedPlan());
        const TArray<TObjectPtr<UActorComponent>> ComponentsBeforeShear = ShearActor->GetDerivedComponents();
        ShearActor->SetActorScale3D(FVector(2.0, 1.0, 1.0));
        Error.Reset();
        bPassed &= TestFalse(TEXT("Break rejects actor-world shear before mutation"),
            Subsystem->BreakComposites({CompositeActor, ShearActor}, RejectedBreakActors, Warnings, Error));
        bPassed &= TestTrue(TEXT("Break shear uses the registered representability code"),
            Error.StartsWith(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM:")));
        bPassed &= TestTrue(TEXT("shear rejection spawns no partial leaves"), RejectedBreakActors.IsEmpty());
        bPassed &= TestFalse(TEXT("shear rejection preserves earlier selected actor"), CompositeActor->IsActorBeingDestroyed());
        bPassed &= TestTrue(TEXT("shear rejection preserves original derived components"),
            ComponentsBeforeShear == ShearActor->GetDerivedComponents());
        ShearActor->Destroy();

        const FString SelectedNestedName = TEXT("break_selected_nested_") + Suffix;
        UPackage* SelectedNestedPackage = CreatePackage(*FString::Printf(
            TEXT("/Game/MH/Generated/Composites/%s"), *SelectedNestedName));
        UMHCompositeAsset* SelectedNestedAsset = NewObject<UMHCompositeAsset>(
            SelectedNestedPackage, FName(*SelectedNestedName), RF_Public | RF_Standalone);
        SelectedNestedAsset->LogicalName = SelectedNestedName;
        FMHCompositeDocument SelectedNestedDocument;
        FMHCompositeNode SelectedNestedMesh = MeshNode;
        SelectedNestedMesh.Name = TEXT("selected_nested_leaf");
        SelectedNestedMesh.Transform.TranslationCm = FVector(25.0, 0.0, 0.0);
        SelectedNestedDocument.Nodes.Add(SelectedNestedMesh);
        Error.Reset();
        bPassed &= TestTrue(TEXT("selected nested definition fixture applies"),
            MHApplyLevelOperationFixture(*SelectedNestedAsset, SelectedNestedDocument, Error));
        UMHCompositeAsset* RandomRootAsset = MHNewLevelOperationFixture(TEXT("break_random_root_") + Suffix);
        FMHCompositeDocument RandomDocument;
        FMHCompositeNode RandomNode;
        RandomNode.Kind = EMHCompositeNodeKind::Random;
        RandomNode.Name = TEXT("random_wrapper");
        RandomNode.Transform.TranslationCm = FVector(100.0, 0.0, 0.0);
        FMHCompositeOption SelectedOption;
        SelectedOption.Kind = EMHCompositeOptionKind::Composite;
        SelectedOption.Resource = SelectedNestedName;
        SelectedOption.Weight = 1.0f;
        RandomNode.Options.Add(SelectedOption);
        FMHCompositeOption UnselectedOption;
        UnselectedOption.Kind = EMHCompositeOptionKind::Composite;
        UnselectedOption.Resource = NestedResource;
        UnselectedOption.Weight = 0.0f;
        RandomNode.Options.Add(UnselectedOption);
        RandomDocument.Nodes.Add(RandomNode);
        Error.Reset();
        bPassed &= TestTrue(TEXT("random Break definition fixture applies"),
            MHApplyLevelOperationFixture(*RandomRootAsset, RandomDocument, Error));
        AMHCompositeActor* RandomActor = World->SpawnActor<AMHCompositeActor>(
            AMHCompositeActor::StaticClass(), FTransform(FVector(50.0, 0.0, 0.0)), SpawnParameters);
        RandomActor->SetCompositeAsset(RandomRootAsset);
        RandomActor->SetSeed(100);
        const FMHResolvedCompositePlan* RandomPreviewPlan = RandomActor->GetResolvedPlan();
        bPassed &= TestNotNull(TEXT("random nested preview resolves a cached plan"), RandomPreviewPlan);
        if (RandomPreviewPlan != nullptr)
        {
            bPassed &= TestEqual(TEXT("random nested plan records one decision"), RandomPreviewPlan->Decisions.Num(), 1);
            bPassed &= TestEqual(TEXT("random nested plan records one selected leaf"), RandomPreviewPlan->Leaves.Num(), 1);
            // R2b-2: the preview materializes Layout + Appearance and builds no source
            // closure; the unselected option is observed on the proof plane instead.
            bPassed &= TestTrue(TEXT("random nested preview builds no source closure"),
                RandomPreviewPlan->Closure.Resources.IsEmpty());
            FMHRandomSourceGraph ClosureGraph;
            TSet<FMHResourceKey> ClosureDependencies;
            FString ClosureError;
            if (TestTrue(TEXT("random nested root admits an applied graph"),
                    MHBuildAppliedCompositeGraph(*RandomRootAsset, *Settings, ClosureGraph, ClosureDependencies, ClosureError)))
            {
                FMHResourceKey UnselectedKey;
                UnselectedKey.Kind = EMHResourceKind::Composite;
                UnselectedKey.LogicalName = NestedResource;
                bPassed &= TestTrue(TEXT("unselected nested option remains in the applied source closure"),
                    ClosureDependencies.Contains(UnselectedKey));
            }
            else
            {
                bPassed = false;
            }
        }
        const FString RandomSourceHash = RandomRootAsset->SourceHash;
        TArray<AActor*> RandomBreakActors;
        Error.Reset();
        bPassed &= TestTrue(TEXT("Break consumes the nested random result without retaining wrappers"),
            Subsystem->BreakComposites({RandomActor}, RandomBreakActors, Warnings, Error));
        bPassed &= TestEqual(TEXT("nested random Break emits only the selected mesh leaf"), RandomBreakActors.Num(), 1);
        if (RandomBreakActors.Num() == 1)
        {
            bPassed &= TestTrue(TEXT("nested random Break preserves parent-local accumulation plus actor placement"),
                RandomBreakActors[0]->GetActorLocation().Equals(FVector(175.0, 0.0, 0.0), KINDA_SMALL_NUMBER));
            bPassed &= TestEqual(TEXT("nested random Break preserves leaf display identity"),
                RandomBreakActors[0]->GetActorLabel(false), FString(TEXT("selected_nested_leaf")));
            AStaticMeshActor* SelectedMeshActor = Cast<AStaticMeshActor>(RandomBreakActors[0]);
            bPassed &= TestTrue(TEXT("nested random Break preserves selected mesh resource"),
                SelectedMeshActor != nullptr && SelectedMeshActor->GetStaticMeshComponent()->GetStaticMesh() == MeshAsset);
            RandomBreakActors[0]->Destroy();
        }
        bPassed &= TestEqual(TEXT("random Break leaves applied source receipt untouched"), RandomRootAsset->SourceHash, RandomSourceHash);
        SelectedNestedAsset->ClearFlags(RF_Public | RF_Standalone);
        SelectedNestedAsset->MarkAsGarbage();

        TArray<AActor*> BrokenActors;
        TArray<FTransform> ExpectedBreakTransforms;
        if (const FMHResolvedCompositePlan* PreviewPlan = CompositeActor->GetResolvedPlan())
        {
            for (const FMHResolvedCompositeLeaf& Leaf : PreviewPlan->Leaves)
            {
                ExpectedBreakTransforms.Add(FTransform(Leaf.WorldMatrix * CompositeActor->GetActorTransform().ToMatrixWithScale()));
            }
        }
        TArray<TWeakObjectPtr<UActorComponent>> DestroyedPlacementComponents;
        for (UActorComponent* Component : CompositeActor->GetDerivedComponents())
        {
            DestroyedPlacementComponents.Add(Component);
        }
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Break materializes the complete resolved plan"),
            Subsystem->BreakComposites({CompositeActor}, BrokenActors, Warnings, Error));
        bPassed &= TestEqual(TEXT("Break creates only resolved mesh and actor leaves"), BrokenActors.Num(), 3);
        bPassed &= TestEqual(TEXT("Break and cached preview have identical leaf counts"), BrokenActors.Num(), ExpectedBreakTransforms.Num());
        for (int32 Index = 0; Index < BrokenActors.Num() && Index < ExpectedBreakTransforms.Num(); ++Index)
        {
            bPassed &= TestTrue(TEXT("Break consumes cached leaf matrices in plan order"),
                BrokenActors[Index]->GetActorTransform().Equals(ExpectedBreakTransforms[Index], KINDA_SMALL_NUMBER));
        }
        bPassed &= TestEqual(TEXT("Break leaves applied SourceHash untouched"), Asset->SourceHash, BeforeRefreshSourceHash);
        MHRebuildAllLoadedCompositeActors();
        for (const TWeakObjectPtr<UActorComponent>& Component : DestroyedPlacementComponents)
        {
            bPassed &= TestTrue(
                TEXT("destroyed composite leaves no registered placement hit proxy"),
                !Component.IsValid() || !Component->IsRegistered());
        }
        TMap<FString, AActor*> ActorsByLabel;
        for (AActor* BrokenActor : BrokenActors)
        {
            if (BrokenActor != nullptr)
            {
                bPassed &= TestFalse(TEXT("Break leaves no nested composite wrapper"), BrokenActor->IsA<AMHCompositeActor>());
                ActorsByLabel.Add(BrokenActor->GetActorLabel(false), BrokenActor);
            }
        }
        bPassed &= TestTrue(TEXT("Break restores authored mesh name"), ActorsByLabel.Contains(TEXT("authored_mesh")));
        bPassed &= TestFalse(TEXT("empty nested composite has no resolved leaf"), ActorsByLabel.Contains(TEXT("authored_nested")));
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

    TArray<FString> SourceFiles;
    IFileManager::Get().FindFilesRecursive(SourceFiles, *SourceRoot, TEXT("*"), true, false, false);
    bPassed &= TestTrue(TEXT("Break and derived editing tests publish no source files"), SourceFiles.IsEmpty());
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
