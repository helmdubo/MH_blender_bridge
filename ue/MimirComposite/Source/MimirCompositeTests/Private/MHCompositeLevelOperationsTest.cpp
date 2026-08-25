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
    FMHCompositeDocument Document;
    FMHCompositeNode Node;
    Node.Kind = EMHCompositeNodeKind::Actor;
    Node.Resource = TEXT("s6_actor");
    Node.Name = TEXT("authored_actor");
    Node.Transform.TranslationCm = FVector(25.0, 0.0, 0.0);
    Document.Nodes.Add(Node);
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
            1);
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Edit unlocks top-level transforms"),
            Subsystem->BeginEditComposite(CompositeActor, Error));
        bPassed &= TestTrue(TEXT("actor records active edit mode"), CompositeActor->IsPlacementEditMode());
        if (CompositeActor->GetTopLevelComponents().Num() == 1)
        {
            CompositeActor->GetTopLevelComponents()[0]->SetWorldLocation(FVector(999.0, 0.0, 0.0));
        }
        Error.Reset();
        bPassed &= TestTrue(TEXT("Cancel closes edit mode"), Subsystem->CancelEditComposite(Error));
        bPassed &= TestFalse(TEXT("Cancel reseals the actor"), CompositeActor->IsPlacementEditMode());
        if (CompositeActor->GetTopLevelComponents().Num() == 1)
        {
            bPassed &= TestTrue(
                TEXT("Cancel rebuild restores authored placement"),
                CompositeActor->GetTopLevelComponents()[0]->GetComponentLocation().Equals(
                    FVector(125.0, 0.0, 0.0),
                    KINDA_SMALL_NUMBER));
            CompositeActor->GetTopLevelComponents()[0]->SetWorldLocation(FVector(777.0, 0.0, 0.0));
        }
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("explicit Rebuild succeeds"),
            Subsystem->RebuildComposites({CompositeActor}, Warnings, Error));
        if (CompositeActor->GetTopLevelComponents().Num() == 1)
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
        bPassed &= TestEqual(TEXT("Break creates one registry actor"), BrokenActors.Num(), 1);
        if (BrokenActors.Num() == 1)
        {
            bPassed &= TestTrue(
                TEXT("Break preserves authored world placement"),
                BrokenActors[0]->GetActorLocation().Equals(
                    FVector(125.0, 0.0, 0.0),
                    KINDA_SMALL_NUMBER));
        }
    }

    World->DestroyWorld(false);
    Settings->SourceRoot = PreviousRoot;
    Settings->ActorClassRegistry = PreviousRegistry;
    IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
