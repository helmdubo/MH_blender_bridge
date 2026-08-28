#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditorViewport.h"
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
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHReviewMainHierarchy,
    "Mimir.Audit.MainBaseline.SemanticHierarchy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMHReviewMainHierarchy::RunTest(const FString& Parameters)
{
    FReviewPreviewRegressionFixture Fixture;
    if (!Fixture.Build(*this, false)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetCompositeAsset(Fixture.Asset);
    bool bPassed = TestNotNull(TEXT("applied plan is usable"), Actor->GetResolvedPlan());
    UStaticMeshComponent* Leaf = ReviewPreviewRegressionLeaf(*Actor);
    if (TestNotNull(TEXT("mesh leaf exists"), Leaf) && Actor->GetTopLevelComponents().Num() > 0)
    {
        bPassed &= TestTrue(TEXT("leaf is attached under its semantic group, not flattened under actor root"),
            Leaf->GetAttachParent() != Actor->GetRootComponent() &&
            Leaf->IsAttachedTo(Actor->GetTopLevelComponents()[0]));
        AddInfo(FString::Printf(TEXT("hierarchy leaf-parent=%s actor-root=%s editor-only=%d"),
            *GetNameSafe(Leaf->GetAttachParent()), *GetNameSafe(Actor->GetRootComponent()), Actor->IsEditorOnly()));
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
}
