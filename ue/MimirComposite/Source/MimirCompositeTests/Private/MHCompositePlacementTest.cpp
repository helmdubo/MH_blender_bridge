#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeActorFactory.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeFactory.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositeProtocol.h"

#include "AssetRegistry/AssetData.h"
#include "Components/ChildActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ThumbnailRendering/ThumbnailRenderer.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositePlacementActorTest,
    "Mimir.V4.Composite.PlacementActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositePlacementActorTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString LogicalName = TEXT("s6_placement_") + Suffix;
    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests"),
        LogicalName);
    IFileManager::Get().MakeDirectory(*SourceRoot, true);

    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const FDirectoryPath PreviousSourceRoot = Settings->SourceRoot;
    Settings->SourceRoot.Path = SourceRoot;

    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(GetTransientPackage());
    Asset->LogicalName = LogicalName;
    FMHCompositeDocument Document;
    FMHCompositeNode Group;
    Group.Kind = EMHCompositeNodeKind::Group;
    Group.Name = TEXT("root_group");
    Group.Transform.TranslationCm = FVector(100.0, 0.0, 0.0);
    Document.Nodes.Add(Group);
    FString Error;
    bool bPassed = TestTrue(
        TEXT("source-shaped asset applies"),
        MHApplyCompositeV4(*Asset, Document, Error));

    UMHCompositeActorFactory* Factory = NewObject<UMHCompositeActorFactory>();
    const FAssetData AssetData(Asset, FAssetData::ECreationFlags::None);
    FText FactoryError;
    bPassed &= TestTrue(
        TEXT("actor factory accepts managed composite class"),
        Factory->CanCreateActorFrom(AssetData, FactoryError));
    UTexture2D* WrongAsset = NewObject<UTexture2D>(GetTransientPackage());
    const FAssetData WrongAssetData(WrongAsset, FAssetData::ECreationFlags::None);
    bPassed &= TestFalse(
        TEXT("actor factory rejects unrelated asset"),
        Factory->CanCreateActorFrom(WrongAssetData, FactoryError));

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    bPassed &= TestNotNull(TEXT("placement test world exists"), World);
    AMHCompositeActor* Actor = nullptr;
    if (World != nullptr)
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = RF_Transient;
        const FTransform Placement(FRotator::ZeroRotator, FVector(1000.0, 0.0, 0.0));
        Actor = World->SpawnActor<AMHCompositeActor>(
            AMHCompositeActor::StaticClass(),
            Placement,
            SpawnParameters);
    }
    bPassed &= TestNotNull(TEXT("placement actor spawns"), Actor);
    if (Actor != nullptr)
    {
        Factory->PostSpawnActor(Asset, Actor);
        const FProperty* CompositeAssetProperty = FindFProperty<FProperty>(
            AMHCompositeActor::StaticClass(),
            TEXT("CompositeAsset"));
        bPassed &= TestNotNull(TEXT("composite asset property exists"), CompositeAssetProperty);
        if (CompositeAssetProperty != nullptr)
        {
            bPassed &= TestTrue(
                TEXT("composite identity is sealed in Details"),
                CompositeAssetProperty->HasAnyPropertyFlags(CPF_EditConst));
        }
        bPassed &= TestEqual(TEXT("factory stores source asset"), Actor->GetCompositeAsset(), Asset);
        bPassed &= TestEqual(
            TEXT("factory reverse lookup"),
            Factory->GetAssetFromActorInstance(Actor),
            static_cast<UObject*>(Asset));
        bPassed &= TestEqual(TEXT("one authored group compiles"), Actor->GetDerivedComponents().Num(), 1);
        bPassed &= TestEqual(
            TEXT("one ordered top-level edit seam"),
            Actor->GetTopLevelPlacementComponents().Num(),
            1);
        if (Actor->GetDerivedComponents().Num() == 1)
        {
            UActorComponent* Component = Actor->GetDerivedComponents()[0];
            bPassed &= TestTrue(TEXT("derived component is transient"), Component->HasAnyFlags(RF_Transient));
            bPassed &= TestTrue(
                TEXT("derived component is duplicate-transient"),
                Component->HasAnyFlags(RF_DuplicateTransient));
            bPassed &= TestTrue(
                TEXT("derived component is text-export-transient"),
                Component->HasAnyFlags(RF_TextExportTransient));
            const USceneComponent* SceneComponent = Cast<USceneComponent>(Component);
            bPassed &= TestNotNull(TEXT("group compiles to scene component"), SceneComponent);
            if (SceneComponent != nullptr)
            {
                bPassed &= TestTrue(
                    TEXT("actor placement transform is composite basis"),
                    SceneComponent->GetComponentLocation().Equals(FVector(1100.0, 0.0, 0.0), 0.01));
            }
        }

        USceneComponent* EditedComponent = Actor->GetTopLevelPlacementComponents().IsEmpty()
            ? nullptr
            : Actor->GetTopLevelPlacementComponents()[0];
        if (EditedComponent != nullptr)
        {
            EditedComponent->SetWorldLocation(FVector(1175.0, 0.0, 0.0));
            Actor->SetPlacementEditMode(true);
        }
        Document.Nodes[0].Transform.TranslationCm = FVector(250.0, 0.0, 0.0);
        bPassed &= TestTrue(TEXT("updated asset applies in place"), MHApplyCompositeV4(*Asset, Document, Error));
        FMHResourceKey RootKey;
        RootKey.Kind = EMHResourceKind::Composite;
        RootKey.LogicalName = LogicalName;
        MHNotifyGeneratedResourceChanged(RootKey);
        bPassed &= TestTrue(TEXT("actor observes its root ResourceKey"), Actor->DependsOnResource(RootKey));
        bPassed &= TestEqual(
            TEXT("notify is deferred while placement edit mode is active"),
            Actor->GetTopLevelPlacementComponents().IsEmpty()
                ? nullptr
                : Actor->GetTopLevelPlacementComponents()[0].Get(),
            EditedComponent);
        if (EditedComponent != nullptr)
        {
            bPassed &= TestTrue(
                TEXT("edit-mode notify preserves local top-level transform"),
                EditedComponent->GetComponentLocation().Equals(FVector(1175.0, 0.0, 0.0), 0.01));
        }
        Actor->SetPlacementEditMode(false);
        Actor->RebuildComposite();
        bPassed &= TestEqual(TEXT("notify keeps one derived group"), Actor->GetDerivedComponents().Num(), 1);
        if (Actor->GetDerivedComponents().Num() == 1)
        {
            USceneComponent* SceneComponent = Cast<USceneComponent>(Actor->GetDerivedComponents()[0]);
            bPassed &= TestNotNull(TEXT("notify rebuild component exists"), SceneComponent);
            if (SceneComponent != nullptr)
            {
                bPassed &= TestTrue(
                    TEXT("in-place notify rebuilds loaded placement"),
                    SceneComponent->GetComponentLocation().Equals(FVector(1250.0, 0.0, 0.0), 0.01));

                SceneComponent->SetWorldLocation(FVector(1600.0, 0.0, 0.0));
                Actor->RebuildComposite();
                const USceneComponent* Rebuilt = Actor->GetTopLevelPlacementComponents().IsEmpty()
                    ? nullptr
                    : Actor->GetTopLevelPlacementComponents()[0];
                bPassed &= TestNotNull(TEXT("sealed rebuild creates replacement component"), Rebuilt);
                if (Rebuilt != nullptr)
                {
                    bPassed &= TestTrue(
                        TEXT("derived transform edit does not survive rebuild"),
                        Rebuilt->GetComponentLocation().Equals(FVector(1250.0, 0.0, 0.0), 0.01));
                }
            }
        }

        Document.Nodes[0].Transform.TranslationCm = FVector(300.0, 0.0, 0.0);
        bPassed &= TestTrue(TEXT("second in-place update applies"), MHApplyCompositeV4(*Asset, Document, Error));
        UPackage* LevelPackage = Actor->GetOutermost();
        LevelPackage->SetDirtyFlag(false);
        MHNotifyGeneratedResourceChanged(RootKey);
        bPassed &= TestFalse(
            TEXT("notify rebuild does not dirty the level package"),
            LevelPackage->IsDirty());
    }

    if (World != nullptr)
    {
        World->DestroyWorld(false);
    }
    Settings->SourceRoot = PreviousSourceRoot;
    IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositePlacementDependencyViewTest,
    "Mimir.V4.Composite.PlacementDependencyView",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositePlacementDependencyViewTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString RootName = TEXT("s6_view_root_") + Suffix;
    const FString NestedName = TEXT("s6_view_nested_") + Suffix;
    const FString MeshName = TEXT("s6_view_mesh_") + Suffix;
    FString Error;
    bool bPassed = true;
    UMHCompositeAsset* RestoredDeadAsset = nullptr;

    const FString NestedPackageName = TEXT("/Game/MH/Generated/Composites/") + NestedName;
    UPackage* NestedPackage = CreatePackage(*NestedPackageName);
    UMHCompositeAsset* NestedAsset = NewObject<UMHCompositeAsset>(
        NestedPackage,
        FName(*NestedName),
        RF_Public | RF_Standalone | RF_Transactional);
    NestedAsset->LogicalName = NestedName;
    FMHCompositeDocument NestedDocument;
    FMHCompositeNode NestedGroup;
    NestedGroup.Kind = EMHCompositeNodeKind::Group;
    NestedGroup.Name = TEXT("nested_group");
    NestedGroup.Transform.TranslationCm = FVector(10.0, 0.0, 0.0);
    NestedDocument.Nodes.Add(NestedGroup);
    bPassed &= TestTrue(
        TEXT("nested generated asset applies"),
        MHApplyCompositeV4(*NestedAsset, NestedDocument, Error));

    UMHCompositeAsset* RootAsset = NewObject<UMHCompositeAsset>(GetTransientPackage());
    RootAsset->LogicalName = RootName;
    FMHCompositeDocument RootDocument;
    FMHCompositeNode NestedPlacement;
    NestedPlacement.Kind = EMHCompositeNodeKind::Composite;
    NestedPlacement.Resource = NestedName;
    NestedPlacement.Transform.TranslationCm = FVector(100.0, 0.0, 0.0);
    RootDocument.Nodes.Add(NestedPlacement);
    bPassed &= TestTrue(
        TEXT("root nested document applies"),
        MHApplyCompositeV4(*RootAsset, RootDocument, Error));

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    bPassed &= TestNotNull(TEXT("dependency-view world exists"), World);
    AMHCompositeActor* Actor = nullptr;
    if (World != nullptr)
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = RF_Transient;
        Actor = World->SpawnActor<AMHCompositeActor>(
            AMHCompositeActor::StaticClass(),
            FTransform(FRotator::ZeroRotator, FVector(1000.0, 0.0, 0.0)),
            SpawnParameters);
    }
    bPassed &= TestNotNull(TEXT("dependency-view actor exists"), Actor);
    if (Actor != nullptr)
    {
        Actor->SetCompositeAsset(RootAsset);
        FMHResourceKey NestedKey;
        NestedKey.Kind = EMHResourceKind::Composite;
        NestedKey.LogicalName = NestedName;
        bPassed &= TestTrue(
            TEXT("nested generated key is observed"),
            Actor->DependsOnResource(NestedKey));
        bPassed &= TestEqual(
            TEXT("nested placement retains one root-document edit handle"),
            Actor->GetTopLevelPlacementComponents().Num(),
            1);
        bPassed &= TestEqual(TEXT("nested resource flattens into two components"), Actor->GetDerivedComponents().Num(), 2);
        if (Actor->GetDerivedComponents().Num() == 2)
        {
            const USceneComponent* NestedComponent = Cast<USceneComponent>(Actor->GetDerivedComponents()[1]);
            bPassed &= TestNotNull(TEXT("nested authored component exists"), NestedComponent);
            if (NestedComponent != nullptr)
            {
                bPassed &= TestTrue(
                    TEXT("nested placement composes document bases"),
                    NestedComponent->GetComponentLocation().Equals(FVector(1110.0, 0.0, 0.0), 0.01));
            }
        }

        NestedDocument.Nodes[0].Transform.TranslationCm = FVector(20.0, 0.0, 0.0);
        bPassed &= TestTrue(
            TEXT("nested asset updates in place"),
            MHApplyCompositeV4(*NestedAsset, NestedDocument, Error));
        MHNotifyGeneratedResourceChanged(NestedKey);
        if (Actor->GetDerivedComponents().Num() == 2)
        {
            const USceneComponent* NestedComponent = Cast<USceneComponent>(Actor->GetDerivedComponents()[1]);
            bPassed &= TestTrue(
                TEXT("nested ResourceKey notify rebuilds parent placement"),
                NestedComponent != nullptr &&
                    NestedComponent->GetComponentLocation().Equals(FVector(1120.0, 0.0, 0.0), 0.01));
        }

        FMHCompositeDocument MissingMeshDocument;
        FMHCompositeNode MissingMesh;
        MissingMesh.Kind = EMHCompositeNodeKind::Mesh;
        MissingMesh.Resource = MeshName;
        MissingMeshDocument.Nodes.Add(MissingMesh);
        bPassed &= TestTrue(
            TEXT("missing-mesh document applies"),
            MHApplyCompositeV4(*RootAsset, MissingMeshDocument, Error));
        FMHResourceKey RootKey;
        RootKey.Kind = EMHResourceKind::Composite;
        RootKey.LogicalName = RootName;
        MHNotifyGeneratedResourceChanged(RootKey);

        FMHResourceKey MeshKey;
        MeshKey.Kind = EMHResourceKind::StaticMesh;
        MeshKey.LogicalName = MeshName;
        bPassed &= TestTrue(TEXT("unresolved mesh key is observed"), Actor->DependsOnResource(MeshKey));
        bPassed &= TestEqual(
            TEXT("unresolved node still has one top-level edit handle"),
            Actor->GetTopLevelPlacementComponents().Num(),
            1);
        bPassed &= TestTrue(
            TEXT("unresolved node produces bbox and label view"),
            Actor->GetDerivedComponents().Num() >= 3);
        bPassed &= TestTrue(
            TEXT("unresolved node reports registered placement warning"),
            Actor->GetLastPlacementWarnings().ContainsByPredicate(
                [&MeshName](const FString& Warning)
                {
                    return Warning.Contains(TEXT("MH_W_UNRESOLVED_PLACEMENT")) &&
                        Warning.Contains(MeshName);
                }));

        const FString MeshPackageName = TEXT("/Game/MH/Generated/Meshes/") + MeshName;
        UPackage* MeshPackage = CreatePackage(*MeshPackageName);
        UStaticMesh* RestoredMesh = NewObject<UStaticMesh>(
            MeshPackage,
            FName(*MeshName),
            RF_Public | RF_Standalone | RF_Transactional);
        MHNotifyGeneratedResourceChanged(MeshKey);
        bPassed &= TestEqual(
            TEXT("same-name mesh notify replaces placeholder with endpoint"),
            Actor->GetDerivedComponents().Num(),
            1);
        bPassed &= TestTrue(
            TEXT("restored endpoint compiles as static mesh component"),
            Actor->GetDerivedComponents().Num() == 1 &&
                Cast<UStaticMeshComponent>(Actor->GetDerivedComponents()[0]) != nullptr);
        bPassed &= TestTrue(
            TEXT("healed placement has no unresolved warning"),
            Actor->GetLastPlacementWarnings().IsEmpty());

        RestoredMesh->ClearFlags(RF_Public | RF_Standalone);
        RestoredMesh->MarkAsGarbage();
    }

    // Exercise the dead-root view directly: it must be visible and retain the
    // key so a later generated-asset notification can heal an actor instance.
    if (World != nullptr)
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = RF_Transient;
        AMHCompositeActor* DeadRootActor = World->SpawnActor<AMHCompositeActor>(SpawnParameters);
        FSoftObjectProperty* AssetProperty = FindFProperty<FSoftObjectProperty>(
            AMHCompositeActor::StaticClass(),
            TEXT("CompositeAsset"));
        bPassed &= TestNotNull(TEXT("soft composite identity property exists"), AssetProperty);
        if (DeadRootActor != nullptr && AssetProperty != nullptr)
        {
            TSoftObjectPtr<UMHCompositeAsset>* StoredPath =
                AssetProperty->ContainerPtrToValuePtr<TSoftObjectPtr<UMHCompositeAsset>>(DeadRootActor);
            const FString DeadName = TEXT("s6_dead_root_") + Suffix;
            *StoredPath = TSoftObjectPtr<UMHCompositeAsset>(FSoftObjectPath(FString::Printf(
                TEXT("/Game/MH/Generated/Composites/%s.%s"),
                *DeadName,
                *DeadName)));
            DeadRootActor->RebuildComposite();
            FMHResourceKey DeadKey;
            DeadKey.Kind = EMHResourceKind::Composite;
            DeadKey.LogicalName = DeadName;
            bPassed &= TestTrue(TEXT("dead-root key remains observable"), DeadRootActor->DependsOnResource(DeadKey));
            bPassed &= TestTrue(
                TEXT("dead root renders bbox and label"),
                DeadRootActor->GetDerivedComponents().Num() >= 3);
            bPassed &= TestTrue(
                TEXT("dead root reports unresolved placement warning"),
                DeadRootActor->GetLastPlacementWarnings().ContainsByPredicate(
                    [&DeadName](const FString& Warning)
                    {
                        return Warning.Contains(TEXT("MH_W_UNRESOLVED_PLACEMENT")) &&
                            Warning.Contains(DeadName);
                    }));

            const FString DeadPackageName = TEXT("/Game/MH/Generated/Composites/") + DeadName;
            UPackage* DeadPackage = CreatePackage(*DeadPackageName);
            RestoredDeadAsset = NewObject<UMHCompositeAsset>(
                DeadPackage,
                FName(*DeadName),
                RF_Public | RF_Standalone | RF_Transactional);
            RestoredDeadAsset->LogicalName = DeadName;
            FMHCompositeDocument HealedDocument;
            FMHCompositeNode HealedGroup;
            HealedGroup.Kind = EMHCompositeNodeKind::Group;
            HealedGroup.Name = TEXT("healed_root");
            HealedDocument.Nodes.Add(HealedGroup);
            bPassed &= TestTrue(
                TEXT("same-name dead-root replacement applies"),
                MHApplyCompositeV4(*RestoredDeadAsset, HealedDocument, Error));
            MHNotifyGeneratedResourceChanged(DeadKey);
            bPassed &= TestEqual(
                TEXT("same-name root notification heals dead placeholder"),
                DeadRootActor->GetCompositeAsset(),
                RestoredDeadAsset);
            bPassed &= TestEqual(
                TEXT("healed dead root compiles authored view"),
                DeadRootActor->GetDerivedComponents().Num(),
                1);
            bPassed &= TestTrue(
                TEXT("healed dead root clears warning"),
                DeadRootActor->GetLastPlacementWarnings().IsEmpty());
        }
    }

    if (World != nullptr)
    {
        World->DestroyWorld(false);
    }
    NestedAsset->ClearFlags(RF_Public | RF_Standalone);
    NestedAsset->MarkAsGarbage();
    if (RestoredDeadAsset != nullptr)
    {
        RestoredDeadAsset->ClearFlags(RF_Public | RF_Standalone);
        RestoredDeadAsset->MarkAsGarbage();
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeThumbnailRegistrationTest,
    "Mimir.V4.Composite.ThumbnailRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeThumbnailRegistrationTest::RunTest(const FString& Parameters)
{
    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(GetTransientPackage());
    Asset->LogicalName = TEXT("s6_thumbnail_registration");
    UE::MimirComposite::FMHCompositeDocument Document;
    FString Error;
    bool bPassed = TestTrue(
        TEXT("thumbnail fixture applies"),
        MHApplyCompositeV4(*Asset, Document, Error));

    UClass* RendererClass = FindObject<UClass>(
        nullptr,
        TEXT("/Script/MimirCompositeEditor.MHCompositeThumbnailRenderer"));
    bPassed &= TestNotNull(TEXT("private thumbnail renderer class is registered"), RendererClass);
    UThumbnailRenderer* Renderer = nullptr;
    if (FApp::CanEverRender())
    {
        FThumbnailRenderingInfo* RenderInfo = UThumbnailManager::Get().GetRenderingInfo(Asset);
        bPassed &= TestNotNull(TEXT("composite class has registered thumbnail info"), RenderInfo);
        if (RenderInfo != nullptr)
        {
            bPassed &= TestNotNull(TEXT("composite thumbnail renderer exists"), RenderInfo->Renderer.Get());
            bPassed &= TestTrue(
                TEXT("composite uses the live placement thumbnail renderer"),
                RenderInfo->Renderer != nullptr &&
                    RendererClass != nullptr &&
                    RenderInfo->Renderer->IsA(RendererClass));
            Renderer = RenderInfo->Renderer;
        }
    }
    else
    {
        // UThumbnailManager intentionally leaves renderer objects null under
        // NullRHI; validate the class contract without requesting a scene.
        if (RendererClass != nullptr)
        {
            Renderer = NewObject<UThumbnailRenderer>(GetTransientPackage(), RendererClass);
        }
    }
    if (Renderer != nullptr)
    {
        bPassed &= TestTrue(TEXT("renderer visualizes a composite asset"), Renderer->CanVisualizeAsset(Asset));
        bPassed &= TestEqual(
            TEXT("thumbnail refreshes on managed asset edits"),
            Renderer->GetThumbnailRenderFrequency(Asset),
            EThumbnailRenderFrequency::OnPropertyChange);
    }
    if (FApp::CanEverRender())
    {
        FObjectThumbnail Thumbnail;
        ThumbnailTools::RenderThumbnail(
            Asset,
            128,
            128,
            ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
            nullptr,
            &Thumbnail);
        bPassed &= TestEqual(TEXT("rendered thumbnail width"), Thumbnail.GetImageWidth(), 128);
        bPassed &= TestEqual(TEXT("rendered thumbnail height"), Thumbnail.GetImageHeight(), 128);
        bPassed &= TestEqual(
            TEXT("rendered thumbnail BGRA byte count"),
            Thumbnail.GetUncompressedImageData().Num(),
            128 * 128 * 4);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeDerivedActorVisibilityTest,
    "Mimir.V4.Composite.DerivedActorVisibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeDerivedActorVisibilityTest::RunTest(const FString& Parameters)
{
    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const TMap<FString, FSoftClassPath> PreviousRegistry = Settings->ActorClassRegistry;
    Settings->ActorClassRegistry.Add(
        TEXT("s6_visibility_actor"),
        FSoftClassPath(AStaticMeshActor::StaticClass()));

    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(GetTransientPackage());
    Asset->LogicalName = TEXT("s6_visibility_root");
    FMHCompositeDocument Document;
    FMHCompositeNode Node;
    Node.Kind = EMHCompositeNodeKind::Actor;
    Node.Resource = TEXT("s6_visibility_actor");
    Node.Name = TEXT("authored_visibility_actor");
    Document.Nodes.Add(Node);
    FString Error;
    bool bPassed = TestTrue(
        TEXT("derived-actor fixture applies"),
        MHApplyCompositeV4(*Asset, Document, Error));

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    bPassed &= TestNotNull(TEXT("derived-actor preview world exists"), World);
    AMHCompositeActor* Actor = World != nullptr
        ? World->SpawnActor<AMHCompositeActor>()
        : nullptr;
    bPassed &= TestNotNull(TEXT("derived-actor composite exists"), Actor);
    if (Actor != nullptr)
    {
        Actor->SetCompositeAsset(Asset);
        UChildActorComponent* ChildComponent = Actor->GetDerivedComponents().Num() == 1
            ? Cast<UChildActorComponent>(Actor->GetDerivedComponents()[0])
            : nullptr;
        bPassed &= TestNotNull(TEXT("actor node compiles to child-actor component"), ChildComponent);
        if (ChildComponent != nullptr)
        {
            bPassed &= TestEqual(
                TEXT("derived child actor is hidden from the Scene Outliner"),
                ChildComponent->GetEditorTreeViewVisualizationMode(),
                EChildActorComponentTreeViewVisualizationMode::Hidden);
            bPassed &= TestNotNull(TEXT("derived child actor is instantiated"), ChildComponent->GetChildActor());
            if (ChildComponent->GetChildActor() != nullptr)
            {
                bPassed &= TestEqual(
                    TEXT("derived child actor receives an authored display label"),
                    ChildComponent->GetChildActor()->GetActorLabel(false),
                    FString(TEXT("authored_visibility_actor")));
            }

            Actor->SetActorLocation(FVector(100.0, 200.0, 300.0));
            Actor->RerunConstructionScripts();
            bPassed &= TestEqual(
                TEXT("moving/rerunning construction preserves the derived component object"),
                Actor->GetDerivedComponents().Num() == 1
                    ? Actor->GetDerivedComponents()[0].Get()
                    : nullptr,
                static_cast<UActorComponent*>(ChildComponent));
        }
    }

    Settings->ActorClassRegistry = PreviousRegistry;
    if (World != nullptr)
    {
        World->DestroyWorld(false);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeManualFileImportTest,
    "Mimir.V4.Composite.ManualFileImport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeManualFileImportTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    const FString LogicalName = TEXT("s6_file_drop_") + Suffix;
    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("MimirCompositeTests"),
        LogicalName);
    const FString SourcePath = FPaths::Combine(SourceRoot, LogicalName + TEXT(".composite"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);

    FMHCompositeDocument Document;
    TArray<uint8> Bytes;
    FString Error;
    bool bPassed = TestTrue(
        TEXT("manual-import fixture canonicalizes"),
        MHWriteCanonicalCompositeV4(Document, Bytes, Error));
    bPassed &= TestTrue(
        TEXT("manual-import source written"),
        FFileHelper::SaveArrayToFile(Bytes, *SourcePath));

    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    const FDirectoryPath PreviousSourceRoot = Settings->SourceRoot;
    Settings->SourceRoot.Path = SourceRoot;
    UMHSourceImporter* Importer = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHSourceImporter>()
        : nullptr;
    bPassed &= TestNotNull(TEXT("source importer subsystem exists"), Importer);

    const FString ExpectedPackage = TEXT("/Game/MH/Generated/Composites/") + LogicalName;
    UMHCompositeAsset* FirstAsset = nullptr;
    UMHCompositeAsset* AdoptedAsset = nullptr;
    FString AdoptedPackage;
    FString AdoptedSourcePath;
    TArray<FString> Warnings;
    if (Importer != nullptr)
    {
        bPassed &= TestTrue(
            TEXT("file inside source root imports manually"),
            Importer->ImportCompositeFile(
                SourcePath,
                ExpectedPackage,
                FirstAsset,
                Warnings,
                Error));
        if (!Error.IsEmpty()) AddError(Error);
        bPassed &= TestNotNull(TEXT("manual import returns managed asset"), FirstAsset);
        if (FirstAsset != nullptr)
        {
            bPassed &= TestEqual(
                TEXT("manual import uses canonical object path"),
                FirstAsset->GetPathName(),
                ExpectedPackage + TEXT(".") + LogicalName);
        }

        UMHCompositeAsset* SecondAsset = nullptr;
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("repeated manual import succeeds"),
            Importer->ImportCompositeFile(
                SourcePath,
                ExpectedPackage,
                SecondAsset,
                Warnings,
                Error));
        bPassed &= TestEqual(TEXT("repeated import preserves exact UObject"), SecondAsset, FirstAsset);

        UMHCompositeAsset* ArbitraryTargetAsset = nullptr;
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("Content Browser target does not affect identity-derived path"),
            Importer->ImportCompositeFile(
                SourcePath,
                TEXT("/Game/Arbitrary/") + LogicalName,
                ArbitraryTargetAsset,
                Warnings,
                Error));
        bPassed &= TestEqual(
            TEXT("arbitrary target returns the same generated UObject"),
            ArbitraryTargetAsset,
            FirstAsset);

        const FString OutsidePath = FPaths::Combine(
            FPaths::ProjectSavedDir(),
            LogicalName + TEXT("_outside.composite"));
        bPassed &= TestTrue(TEXT("outside fixture written"), FFileHelper::SaveArrayToFile(Bytes, *OutsidePath));
        const FString AdoptedName = LogicalName + TEXT("_adopted");
        const FString AdoptedFolder = FPaths::Combine(SourceRoot, TEXT("adopted"));
        AdoptedSourcePath = FPaths::Combine(AdoptedFolder, AdoptedName + TEXT(".composite"));
        AdoptedPackage = TEXT("/Game/MH/Generated/Composites/") + AdoptedName;
        FMHCompositeAdoptTarget AdoptTarget{AdoptedFolder, AdoptedName};
        Error.Reset();
        bPassed &= TestTrue(
            TEXT("external file is atomically adopted and imported"),
            Importer->AdoptCompositeFile(
                OutsidePath,
                AdoptTarget.Folder,
                AdoptTarget.LogicalName,
                AdoptedAsset,
                Warnings,
                Error));
        if (!Error.IsEmpty()) AddError(Error);
        bPassed &= TestNotNull(TEXT("Adopt returns managed asset"), AdoptedAsset);
        if (AdoptedAsset != nullptr)
        {
            bPassed &= TestEqual(
                TEXT("Adopt uses identity-derived generated path"),
                AdoptedAsset->GetPathName(),
                AdoptedPackage + TEXT(".") + AdoptedName);
        }
        TArray<uint8> AdoptedBytes;
        bPassed &= TestTrue(
            TEXT("Adopt preserves payload bytes"),
            FFileHelper::LoadFileToArray(AdoptedBytes, *AdoptedSourcePath));
        bPassed &= TestEqual(TEXT("Adopt byte count"), AdoptedBytes.Num(), Bytes.Num());
        bPassed &= TestTrue(TEXT("Adopt bytes are exact"), AdoptedBytes == Bytes);

        UMHCompositeAsset* DuplicateAsset = nullptr;
        Error.Reset();
        bPassed &= TestFalse(
            TEXT("duplicate Adopt is rejected without overwrite"),
            Importer->AdoptCompositeFile(
                OutsidePath,
                AdoptTarget.Folder,
                AdoptTarget.LogicalName,
                DuplicateAsset,
                Warnings,
                Error));
        bPassed &= TestTrue(
            TEXT("duplicate Adopt emits machine diagnostic"),
            Error.Contains(TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME"), ESearchCase::CaseSensitive));
        TArray<uint8> BytesAfterReject;
        bPassed &= TestTrue(
            TEXT("adopted source remains readable after duplicate reject"),
            FFileHelper::LoadFileToArray(BytesAfterReject, *AdoptedSourcePath));
        bPassed &= TestTrue(TEXT("duplicate reject leaves bytes unchanged"), BytesAfterReject == Bytes);
        IFileManager::Get().Delete(*OutsidePath, false, true, true);
    }

    UMHCompositeFactory* Factory = NewObject<UMHCompositeFactory>();
    bPassed &= TestTrue(TEXT("factory advertises exact .composite"), Factory->FactoryCanImport(SourcePath));
    bPassed &= TestFalse(TEXT("factory rejects uppercase suffix"), Factory->FactoryCanImport(SourcePath + TEXT(".COMPOSITE")));

    Settings->SourceRoot = PreviousSourceRoot;
    MHShutdownProjectIndex();
    if (FirstAsset != nullptr)
    {
        ObjectTools::DeleteSingleObject(FirstAsset, false);
    }
    const FString AssetFilename = FPackageName::LongPackageNameToFilename(
        ExpectedPackage,
        FPackageName::GetAssetPackageExtension());
    IFileManager::Get().Delete(*AssetFilename, false, true, true);
    if (AdoptedAsset != nullptr)
    {
        ObjectTools::DeleteSingleObject(AdoptedAsset, false);
    }
    if (!AdoptedPackage.IsEmpty())
    {
        const FString AdoptedAssetFilename = FPackageName::LongPackageNameToFilename(
            AdoptedPackage,
            FPackageName::GetAssetPackageExtension());
        IFileManager::Get().Delete(*AdoptedAssetFilename, false, true, true);
    }
    if (!AdoptedSourcePath.IsEmpty())
    {
        IFileManager::Get().Delete(*AdoptedSourcePath, false, true, true);
    }
    IFileManager::Get().DeleteDirectory(*SourceRoot, false, true);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
