#include "MHRecipeTestFixture.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/StaticMeshComponent.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelEditorViewport.h"
#include "Misc/CommandLine.h"
#include "PrimitiveSceneProxy.h"
#include "RenderingThread.h"
#include "SceneView.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditProjectionGameViewVisibilityTest,
    "Mimir.V5.Composite.EditMode.Viewport.GameViewKeepsNestedProjectionVisible",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditProjectionGameViewVisibilityTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    if (!FParse::Param(FCommandLine::Get(), TEXT("MHPreviewRenderSmoke")))
    {
        AddInfo(TEXT("RHI lane NOT RUN: requires -MHPreviewRenderSmoke in the isolated host without -nullrhi"));
        return true;
    }
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("MimirCompositeV5S6") || GEditor == nullptr)
    {
        AddError(TEXT("edit projection render smoke refuses a non-isolated project"));
        return false;
    }

    FLevelEditorViewportClient* Client = nullptr;
    for (FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
    {
        if (Candidate != nullptr && Candidate->IsPerspective() && Candidate->Viewport != nullptr)
        {
            Client = Candidate;
            break;
        }
    }
    if (!TestNotNull(TEXT("real level viewport"), Client)) return false;
    UWorld* World = Client->GetWorld();
    if (!TestNotNull(TEXT("viewport editor world"), World)) return false;
    const FVector OldLocation = Client->GetViewLocation();
    const FRotator OldRotation = Client->GetViewRotation();
    const EViewModeIndex OldMode = Client->GetViewMode();
    const bool bOldGameView = Client->IsInGameView();
    const FEngineShowFlags OldFlags = Client->EngineShowFlags;
    const FEngineShowFlags OldLastFlags = Client->LastEngineShowFlags;
    const auto RestoreViewport = [Client, OldLocation, OldRotation, OldMode, bOldGameView, OldFlags, OldLastFlags]()
    {
        Client->SetGameView(bOldGameView);
        Client->SetViewMode(OldMode);
        Client->EngineShowFlags = OldFlags;
        Client->LastEngineShowFlags = OldLastFlags;
        Client->SetViewLocation(OldLocation);
        Client->SetViewRotation(OldRotation);
        Client->Invalidate(true, true);
    };

    const FMHCompositeEditBackendScope Backend(true);
    FRecipeFixture Recipe(*this);
    const FString MeshName = Recipe.Name(TEXT("ce_game_view_mesh"));
    if (!TestNotNull(TEXT("mesh receipt"), Recipe.Mesh(MeshName))) return false;

    FMHCompositeDocument ChildDocument;
    FMHCompositeNode& Left = ChildDocument.Nodes.AddDefaulted_GetRef();
    Left.Kind = EMHCompositeNodeKind::Mesh;
    Left.Name = TEXT("left");
    Left.Resource = MeshName;
    Left.Transform.TranslationCm = FVector(0.0, -140.0, 0.0);
    FMHCompositeNode& Right = ChildDocument.Nodes.AddDefaulted_GetRef();
    Right.Kind = EMHCompositeNodeKind::Mesh;
    Right.Name = TEXT("right");
    Right.Resource = MeshName;
    Right.Transform.TranslationCm = FVector(0.0, 140.0, 0.0);
    UMHCompositeAsset* Child = Recipe.Composite(Recipe.Name(TEXT("ce_game_view_child")), ChildDocument, {});

    FMHCompositeDocument RootDocument;
    FMHCompositeNode& Reference = RootDocument.Nodes.AddDefaulted_GetRef();
    Reference.Kind = EMHCompositeNodeKind::Composite;
    Reference.Name = TEXT("offset_child");
    Reference.Resource = Child != nullptr ? Child->LogicalName : FString();
    Reference.Transform.TranslationCm = FVector(250.0, 80.0, 60.0);
    UMHCompositeAsset* Root = Recipe.Composite(Recipe.Name(TEXT("ce_game_view_root")), RootDocument, {});
    if (!TestNotNull(TEXT("child definition"), Child) || !TestNotNull(TEXT("root definition"), Root)) return false;

    FActorSpawnParameters SpawnParams;
    SpawnParams.ObjectFlags = RF_Transactional;
    AMHCompositeActor* Placement = World->SpawnActor<AMHCompositeActor>(
        AMHCompositeActor::StaticClass(), FTransform(FVector(20000.0, 20000.0, 20000.0)), SpawnParams);
    if (!TestNotNull(TEXT("viewport placement"), Placement)) return false;
    Placement->SetAutoSeed(false);
    Placement->SetAutoAppearanceSeed(false);
    Placement->SetSeed(7);
    Placement->SetAppearanceSeed(11);
    Placement->SetCompositeAsset(Root);

    bool bPassed = TestNotNull(TEXT("resolved placement"), Placement->GetResolvedPlan());
    const FMHResolvedCompositeNode* Invocation = nullptr;
    if (Placement->GetResolvedPlan() != nullptr)
    {
        for (const FMHResolvedCompositeNode& Node : Placement->GetResolvedPlan()->Nodes)
        {
            if (Node.SemanticKind == EMHRandomSemanticKind::Composite)
            {
                Invocation = &Node;
                break;
            }
        }
    }
    bPassed &= TestNotNull(TEXT("nonzero nested reference"), Invocation);

    // Reproduce the field sequence: the user is already in Game View when
    // choosing Edit Contents. Entering the mode must not reset their view.
    const FVector ExpectedCenter(20250.0, 20080.0, 20060.0);
    Client->SetViewMode(VMI_Unlit);
    Client->SetViewLocation(ExpectedCenter + FVector(650.0, 0.0, 0.0));
    Client->SetViewRotation(FRotator(0.0, 180.0, 0.0));
    Client->SetGameView(true);
    const FVector EnterLocation = Client->GetViewLocation();
    const FRotator EnterRotation = Client->GetViewRotation();
    FEngineShowFlags ExpectedFlags = Client->EngineShowFlags;
    FEngineShowFlags ExpectedLastFlags = Client->LastEngineShowFlags;
    ExpectedFlags.EditingLevelInstance = true;
    ExpectedLastFlags.EditingLevelInstance = true;

    UMHCompositeLevelSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
    FString Error;
    if (Invocation == nullptr || Subsystem == nullptr ||
        !TestTrue(TEXT("open nested edit: ") + Error, Subsystem->BeginEditNestedComposite(Placement, Invocation->NodePath, Error)))
    {
        RestoreViewport();
        Placement->Destroy();
        return false;
    }
    bPassed &= TestTrue(TEXT("Edit Contents preserves Game View"), Client->IsInGameView());
    bPassed &= TestTrue(TEXT("Edit Contents preserves the viewport camera"),
        Client->GetViewLocation().Equals(EnterLocation, 0.0) && Client->GetViewRotation().Equals(EnterRotation, 0.0));
    bPassed &= TestEqual(TEXT("Edit Contents preserves active Game View flags"),
        Client->EngineShowFlags.ToString(), ExpectedFlags.ToString());
    bPassed &= TestEqual(TEXT("Edit Contents preserves stored editor flags"),
        Client->LastEngineShowFlags.ToString(), ExpectedLastFlags.ToString());

    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    AMHCompositeEditProjectionActor* ProjectionActor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    bPassed &= TestNotNull(TEXT("nested projection"), Projection);
    bPassed &= TestNotNull(TEXT("projection actor"), ProjectionActor);

    TArray<UStaticMeshComponent*> Meshes;
    UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (Projection != nullptr && ProjectionActor != nullptr && Cube != nullptr)
    {
        for (USceneComponent* Component : Projection->GetComponents())
        {
            if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component))
            {
                Mesh->SetStaticMesh(Cube);
                Meshes.Add(Mesh);
            }
        }
    }
    bPassed &= TestNotNull(TEXT("renderable cube"), Cube);
    bPassed &= TestEqual(TEXT("both nested leaves have visuals"), Meshes.Num(), 2);

    TMap<TObjectPtr<UStaticMeshComponent>, FTransform> BeforeTransforms;
    TSet<FGuid> BoundNodeIds;
    FVector Center = FVector::ZeroVector;
    for (UStaticMeshComponent* Mesh : Meshes)
    {
        BeforeTransforms.Add(Mesh, Mesh->GetComponentTransform());
        Center += Mesh->GetComponentLocation();
        const FGuid NodeId = Projection->GetNodeIdForComponent(Mesh);
        BoundNodeIds.Add(NodeId);
        bPassed &= TestTrue(TEXT("visual belongs to the projection actor"), Mesh->GetOwner() == ProjectionActor);
        bPassed &= TestTrue(TEXT("visual retains an authoring-node binding"), NodeId.IsValid());
    }
    bPassed &= TestEqual(TEXT("the two leaves retain distinct bindings"), BoundNodeIds.Num(), 2);
    if (!Meshes.IsEmpty()) Center /= static_cast<double>(Meshes.Num());
    bPassed &= TestTrue(TEXT("nested leaf center keeps the nonzero occurrence transform"), Center.Equals(ExpectedCenter, 1.e-3));
    World->SendAllEndOfFrameUpdates();
    FlushRenderingCommands();
    Client->Invalidate(true, true);
    Client->Viewport->Draw();
    FlushRenderingCommands();

    FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
        Client->Viewport, World->Scene, Client->EngineShowFlags).SetRealtimeUpdate(false));
    const FSceneView* View = Client->CalcSceneView(&ViewFamily);
    bPassed &= TestTrue(TEXT("real viewport uses game show flags"), Client->EngineShowFlags.Game && !Client->EngineShowFlags.Editor);
    bPassed &= TestNotNull(TEXT("game-view scene view"), View);
    bPassed &= TestFalse(TEXT("transient projection actor is visible in game view"), ProjectionActor != nullptr && ProjectionActor->IsHidden());

    for (UStaticMeshComponent* Mesh : Meshes)
    {
        bPassed &= TestTrue(TEXT("game-view toggle preserves the visual transform"),
            Mesh->GetComponentTransform().Equals(BeforeTransforms.FindChecked(Mesh), 0.0));
        bPassed &= TestTrue(TEXT("projection mesh is visible in game view"), !Mesh->bHiddenInGame);
        const FPrimitiveSceneProxy* Proxy = Mesh->GetSceneProxy();
        bPassed &= TestNotNull(TEXT("projection mesh render proxy"), Proxy);
        if (Proxy != nullptr && View != nullptr)
        {
            bPassed &= TestTrue(TEXT("renderer admits every nested leaf in game view"), Proxy->IsShown(View));
        }
        if (View != nullptr)
        {
            FVector2D Pixel;
            const bool bProjected = View->WorldToPixel(Mesh->Bounds.Origin, Pixel);
            bPassed &= TestTrue(TEXT("leaf center projects into the viewport"), bProjected);
            if (bProjected)
            {
                HHitProxy* Hit = Client->Viewport->GetHitProxy(FMath::RoundToInt(Pixel.X), FMath::RoundToInt(Pixel.Y));
                const HActor* ActorHit = Hit != nullptr && Hit->IsA(HActor::StaticGetType()) ? static_cast<HActor*>(Hit) : nullptr;
                bPassed &= TestTrue(TEXT("every visible leaf rasterizes its projection hit proxy"),
                    ActorHit != nullptr && ActorHit->Actor == ProjectionActor && ActorHit->PrimComponent == Mesh);
            }
        }
    }

    bPassed &= TestTrue(TEXT("cancel nested edit: ") + Error, Subsystem->CancelEditComposite(Error));
    RestoreViewport();
    Placement->Destroy();
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
