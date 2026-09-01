#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "LevelEditorViewport.h"
#include "Misc/PackageName.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PrimitiveSceneProxy.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UnrealClient.h"

namespace UE::MimirComposite::Tests
{
namespace
{
struct FReviewPreviewRegressionFixture
{
    TArray<UObject*> Assets;
    FString Name = TEXT("preview_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UStaticMesh* Mesh = nullptr;
    UMHCompositeAsset* Asset = nullptr;

    ~FReviewPreviewRegressionFixture()
    {
        for (UObject* Object : Assets)
        {
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->MarkAsGarbage();
        }
    }

    bool Build(FAutomationTestBase& Test, const bool bGeometry)
    {
        const FString MeshName = Name + TEXT("_mesh");
        UPackage* MeshPackage = CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + MeshName));
        if (bGeometry)
        {
            UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
            if (!Test.TestNotNull(TEXT("stock cube"), Cube)) return false;
            FStaticMeshCompilingManager::Get().FinishCompilation({Cube});
            Mesh = DuplicateObject<UStaticMesh>(Cube, MeshPackage, FName(*MeshName));
            Mesh->SetStaticMaterials({});
            for (int32 Lod = 0; Lod < Mesh->GetNumSourceModels(); ++Lod)
            {
                FMeshDescription* Description = Mesh->GetMeshDescription(Lod);
                FStaticMeshAttributes Attributes(*Description);
                TPolygonGroupAttributesRef<FName> Slots = Attributes.GetPolygonGroupMaterialSlotNames();
                for (const FPolygonGroupID Group : Description->PolygonGroups().GetElementIDs()) Slots[Group] = NAME_None;
                Mesh->CommitMeshDescription(Lod);
            }
            // Only the test fixture may wait. Interactive placement must not.
            Mesh->Build(true);
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
        }
        else Mesh = NewObject<UStaticMesh>(MeshPackage, FName(*MeshName), RF_Public | RF_Standalone);
        Assets.Add(Mesh);
        Mesh->SetFlags(RF_Public | RF_Standalone);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = MeshName;
        Receipt->SourceRelativePath = MeshName + TEXT(".mesh.fbx");
        Receipt->SourceHash = TEXT("blake3-160:0123456789012345678901234567890123456789");
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);
        Asset = NewObject<UMHCompositeAsset>(CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + Name)),
            FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        FMHCompositeDocument Document;
        FMHCompositeNode& Group = Document.Nodes.AddDefaulted_GetRef();
        Group.Kind = EMHCompositeNodeKind::Group;
        Group.Name = TEXT("Parent group");
        FMHCompositeNode& Leaf = Group.Children.AddDefaulted_GetRef();
        Leaf.Kind = EMHCompositeNodeKind::Mesh;
        Leaf.Resource = MeshName;
        FString Error;
        TArray<uint8> Bytes;
        if (!MHApplyCompositeV5(*Asset, Document, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(Error);
            return false;
        }
        Asset->LogicalName = Name;
        Asset->SourceRelativePath = Name + TEXT(".composite");
        Asset->SourceHash = MHRawPayloadHash(Bytes);
        Asset->AppliedHash = Asset->SourceHash;
        return true;
    }
};

UStaticMeshComponent* ReviewPreviewRegressionLeaf(AMHCompositeActor& Actor)
{
    for (UActorComponent* Component : Actor.GetDerivedComponents())
        if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component)) return Mesh;
    return nullptr;
}

/** The perspective level viewport of the isolated host, or nullptr. */
FLevelEditorViewportClient* ReviewPreviewLevelViewport()
{
    if (GEditor == nullptr) return nullptr;
    for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
        if (Candidate != nullptr && Candidate->IsPerspective() && Candidate->Viewport != nullptr) return Candidate;
    return nullptr;
}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHReviewMainTopLevelGrouping,
    "Mimir.Audit.MainBaseline.TopLevelGrouping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMHReviewMainTopLevelGrouping::RunTest(const FString& Parameters)
{
    FReviewPreviewRegressionFixture Fixture;
    if (!Fixture.Build(*this, false)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetCompositeAsset(Fixture.Asset);
    bool bPassed = TestNotNull(TEXT("applied plan is usable"), Actor->GetResolvedPlan());
    UStaticMeshComponent* Leaf = ReviewPreviewRegressionLeaf(*Actor);
    const TArray<FMHCompositeLeafMaterialization>& Materializations =
        Actor->GetLeafMaterializations();
    if (TestNotNull(TEXT("mesh leaf exists"), Leaf) &&
        TestEqual(TEXT("one plan-aligned leaf mapping exists"), Materializations.Num(), 1) &&
        Actor->GetTopLevelComponents().Num() > 0)
    {
        const FMHCompositeLeafMaterialization& Mapping = Materializations[0];
        UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Leaf);
        bPassed &= TestNotNull(TEXT("static leaf is materialized by an ISM bucket"), Bucket);
        bPassed &= TestTrue(TEXT("leaf mapping identifies an ISM instance"), Mapping.IsInstanced());
        bPassed &= TestEqual(TEXT("leaf mapping points at the bucket"), Mapping.Component.Get(),
            static_cast<USceneComponent*>(Bucket));
        bPassed &= TestTrue(TEXT("leaf mapping retains its source hierarchy path"),
            Mapping.NodePath.Contains(TEXT("/children[0]")));
        bPassed &= TestTrue(TEXT("instance selection maps back to the leaf path"),
            Actor->SelectPlacementLeaf(Bucket, Mapping.InstanceIndex));
        bPassed &= TestEqual(TEXT("selected instance preserves logical top-level grouping"),
            Actor->GetSelectedPlacementLeafPath(), Mapping.NodePath);
        AddInfo(FString::Printf(
            TEXT("top-level grouping bucket-parent=%s actor-root=%s instance=%d path=%s editor-only=%d"),
            *GetNameSafe(Leaf->GetAttachParent()), *GetNameSafe(Actor->GetRootComponent()),
            Mapping.InstanceIndex, *Mapping.NodePath, Actor->IsEditorOnly()));
    }
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHReviewRenderedHitProxyRegression,
    "Mimir.Audit.MainBaseline.RenderedNativeHitProxy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHReviewRenderedHitProxyRegression::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("MHPreviewRenderSmoke")))
    {
        AddInfo(TEXT("RHI lane NOT RUN: requires -MHPreviewRenderSmoke in the isolated host without -nullrhi"));
        return true;
    }
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("MimirCompositeV5S6") || GEditor == nullptr)
    {
        AddError(TEXT("render smoke refuses a non-isolated project"));
        return false;
    }
    FLevelEditorViewportClient* Client = nullptr;
    for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
        if (Candidate != nullptr && Candidate->IsPerspective() && Candidate->Viewport != nullptr) { Client = Candidate; break; }
    if (!TestNotNull(TEXT("real level viewport"), Client)) return false;
    FReviewPreviewRegressionFixture Fixture;
    if (!Fixture.Build(*this, true)) return false;
    UWorld* World = Client->GetWorld();
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    const FVector Origin(20000, 20000, 20000);
    Actor->SetActorLocation(Origin);
    Actor->SetCompositeAsset(Fixture.Asset);
    UStaticMeshComponent* Leaf = ReviewPreviewRegressionLeaf(*Actor);
    if (!TestNotNull(TEXT("rendered leaf"), Leaf)) { Actor->Destroy(); return false; }
    const FVector OldLocation = Client->GetViewLocation();
    const FRotator OldRotation = Client->GetViewRotation();
    const FEngineShowFlags OldFlags = Client->EngineShowFlags;
    const EViewModeIndex OldMode = Client->GetViewMode();
    Client->SetViewMode(VMI_Unlit);
    Client->SetViewLocation(Origin + FVector(500, 0, 0));
    Client->SetViewRotation(FRotator(0, 180, 0));
    bool bPassed = true;
    for (int32 Pass = 0; Pass < 4; ++Pass)
    {
        const bool bGameView = (Pass % 2) != 0;
        Client->SetGameView(bGameView);
        if (Pass == 2)
        {
            Actor->RebuildComposite();
            Actor->SetActorLocation(Origin + FVector(0, 0, 10));
            Actor->SetSeed(42);
        }
        World->SendAllEndOfFrameUpdates();
        FlushRenderingCommands();
        Client->Invalidate(true, true);
        Client->Viewport->Draw();
        FlushRenderingCommands();
        FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
            Client->Viewport, World->Scene, Client->EngineShowFlags).SetRealtimeUpdate(false));
        const FSceneView* View = Client->CalcSceneView(&ViewFamily);
        FPrimitiveSceneProxy* Proxy = Leaf->GetSceneProxy();
        bPassed &= TestNotNull(*FString::Printf(TEXT("pass %d registered render proxy"), Pass), Proxy);
        if (Proxy != nullptr)
        {
            bPassed &= TestTrue(*FString::Printf(TEXT("pass %d actual proxy shown (G=%d)"), Pass, bGameView), Proxy->IsShown(View));
            bPassed &= TestTrue(TEXT("render proxy selectable"), Proxy->IsSelectable());
        }
        const FIntPoint Size = Client->Viewport->GetSizeXY();
        HHitProxy* Hit = Client->Viewport->GetHitProxy(Size.X / 2, Size.Y / 2);
        bPassed &= TestTrue(*FString::Printf(TEXT("pass %d rasterized native HActor at mesh pixel"), Pass),
            Hit != nullptr && Hit->IsA(HActor::StaticGetType()) && static_cast<HActor*>(Hit)->Actor == Actor);
        AddInfo(FString::Printf(TEXT("RHI native-pick pass=%d G=%d size=%dx%d hit=%s"), Pass, bGameView,
            Size.X, Size.Y, Hit != nullptr ? Hit->GetType()->GetName() : TEXT("none")));
    }
    Client->SetViewMode(OldMode);
    Client->EngineShowFlags = OldFlags;
    Client->SetViewLocation(OldLocation);
    Client->SetViewRotation(OldRotation);
    Actor->Destroy();
    Client->Invalidate(true, true);
    return bPassed;
}

/**
 * V5-S6.2 acceptance 1. Not a component-existence check: reopen a saved level
 * and route a real viewport click through the leaf pixel, then assert the
 * editor selection actually holds the composite actor.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHLoadedPlacementClickSelection,
    "Mimir.Audit.MainBaseline.LoadedPlacementClickSelectsActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHLoadedPlacementClickSelection::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("MHPreviewRenderSmoke")))
    {
        AddInfo(TEXT("RHI lane NOT RUN: requires -MHPreviewRenderSmoke in the isolated host without -nullrhi"));
        return true;
    }
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("MimirCompositeV5S6") || GEditor == nullptr)
    {
        AddError(TEXT("click selection smoke refuses a non-isolated project"));
        return false;
    }
    FReviewPreviewRegressionFixture Fixture;
    if (!Fixture.Build(*this, true)) return false;

    const FVector Origin(20000, 20000, 20000);
    UWorld* Authoring = UEditorLoadingAndSavingUtils::NewBlankMap(false);
    if (!TestNotNull(TEXT("blank authoring map"), Authoring)) return false;
    AMHCompositeActor* Authored = Authoring->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("authored placement"), Authored)) return false;
    Authored->SetActorLocation(Origin);
    Authored->SetCompositeAsset(Fixture.Asset);
    if (!TestNotNull(*Authored->GetLastPlacementError(), Authored->GetResolvedPlan())) return false;

    TArray<UPackage*> Packages;
    for (UObject* Object : Fixture.Assets) Packages.AddUnique(Object->GetOutermost());
    const FString MapPackage = TEXT("/Game/MimirS6/PlacementClick");
    FString MapFile;
    const bool bSaved = TestTrue(TEXT("fixture asset packages saved"),
            UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) &&
        TestTrue(TEXT("authored placement map saved"), UEditorLoadingAndSavingUtils::SaveMap(Authoring, MapPackage)) &&
        TestTrue(TEXT("map filename resolves"), FPackageName::TryConvertLongPackageNameToFilename(
            MapPackage, MapFile, FPackageName::GetMapPackageExtension()));
    if (!bSaved)
    {
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        return false;
    }
    UEditorLoadingAndSavingUtils::NewBlankMap(false);

    // Open the level exactly as the owner does, then click what is on screen.
    UWorld* World = UEditorLoadingAndSavingUtils::LoadMap(MapFile);
    if (!TestNotNull(TEXT("reopened level"), World))
    {
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        return false;
    }
    AMHCompositeActor* Actor = nullptr;
    for (TActorIterator<AMHCompositeActor> It(World); It; ++It) { Actor = *It; break; }
    FLevelEditorViewportClient* Client = ReviewPreviewLevelViewport();
    if (!TestNotNull(TEXT("reopened level holds the placement"), Actor) ||
        !TestNotNull(TEXT("real level viewport"), Client) ||
        !TestTrue(TEXT("viewport shows the reopened level"), Client->GetWorld() == World))
    {
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        return false;
    }
    UStaticMeshComponent* Leaf = ReviewPreviewRegressionLeaf(*Actor);
    bool bPassed = TestNotNull(TEXT("reopened placement has a rendered leaf"), Leaf);
    bPassed &= TestEqual(TEXT("no placement build ran before the actor was registered"),
        Actor->GetPlacementUnregisteredBuildCount(), 0u);
    if (Leaf == nullptr)
    {
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        return false;
    }

    const FVector OldLocation = Client->GetViewLocation();
    const FRotator OldRotation = Client->GetViewRotation();
    const EViewModeIndex OldMode = Client->GetViewMode();
    Client->SetViewMode(VMI_Unlit);
    Client->SetViewLocation(Origin + FVector(500, 0, 0));
    Client->SetViewRotation(FRotator(0, 180, 0));
    Client->SetGameView(false);
    World->SendAllEndOfFrameUpdates();
    FlushRenderingCommands();
    Client->Invalidate(true, true);
    Client->Viewport->Draw();
    FlushRenderingCommands();

    GEditor->SelectNone(false, true, false);
    const FIntPoint Size = Client->Viewport->GetSizeXY();
    const uint32 HitX = Size.X / 2;
    const uint32 HitY = Size.Y / 2;
    HHitProxy* Hit = Client->Viewport->GetHitProxy(HitX, HitY);
    bPassed &= TestTrue(TEXT("reopened leaf rasterizes a native HActor of the composite actor"),
        Hit != nullptr && Hit->IsA(HActor::StaticGetType()) && static_cast<HActor*>(Hit)->Actor == Actor);
    FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
        Client->Viewport, World->Scene, Client->EngineShowFlags).SetRealtimeUpdate(false));
    FSceneView* View = Client->CalcSceneView(&ViewFamily);
    if (View != nullptr)
    {
        Client->ProcessClick(*View, Hit, EKeys::LeftMouseButton, IE_Released, HitX, HitY);
    }
    const bool bSelected = GEditor->GetSelectedActors() != nullptr &&
        GEditor->GetSelectedActors()->IsSelected(Actor);
    AddInfo(FString::Printf(TEXT("reopened-level click at %dx%d hit=%s selected=%d selection-count=%d"),
        HitX, HitY, Hit != nullptr ? Hit->GetType()->GetName() : TEXT("none"), bSelected ? 1 : 0,
        GEditor->GetSelectedActors() != nullptr ? GEditor->GetSelectedActors()->Num() : -1));
    bPassed &= TestTrue(TEXT("clicking the leaf selects the composite actor"), bSelected);

    GEditor->SelectNone(false, true, false);
    Client->SetViewMode(OldMode);
    Client->SetViewLocation(OldLocation);
    Client->SetViewRotation(OldRotation);
    Client->Invalidate(true, true);
    UEditorLoadingAndSavingUtils::NewBlankMap(false);
    return bPassed;
}
}
