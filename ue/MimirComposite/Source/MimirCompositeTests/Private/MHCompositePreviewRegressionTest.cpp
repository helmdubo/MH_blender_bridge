#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositePreviewCache.h"
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
struct FPreviewRegressionFixture
{
    TArray<UObject*> Assets;
    FString Name = TEXT("preview_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UStaticMesh* Mesh = nullptr;
    UMHCompositeAsset* Asset = nullptr;

    ~FPreviewRegressionFixture()
    {
        MHInvalidateCompositePreviewCache();
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

UStaticMeshComponent* PreviewRegressionLeaf(AMHCompositeActor& Actor)
{
    for (UActorComponent* Component : Actor.GetDerivedComponents())
        if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component)) return Mesh;
    return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPreviewLifecycleRegression,
    "Mimir.V5.Preview.NativeOwnershipHierarchyAndCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPreviewLifecycleRegression::RunTest(const FString& Parameters)
{
    FPreviewRegressionFixture Fixture;
    if (!Fixture.Build(*this, false)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetCompositeAsset(Fixture.Asset);
    bool bPassed = TestNotNull(*Actor->GetLastPlacementError(), Actor->GetResolvedPlan());
    bPassed &= TestFalse(TEXT("editor placement is not hidden by the editor-only render flag"), Actor->IsEditorOnly());
    bPassed &= TestFalse(TEXT("source placement excluded from cooked client load"), Actor->NeedsLoadForClient());
    bPassed &= TestFalse(TEXT("source placement excluded from cooked server load"), Actor->NeedsLoadForServer());
    UStaticMeshComponent* Leaf = PreviewRegressionLeaf(*Actor);
    if (TestNotNull(TEXT("ordinary static mesh component"), Leaf))
    {
        bPassed &= TestEqual(TEXT("native owner"), Leaf->GetOwner(), static_cast<AActor*>(Actor));
        bPassed &= TestTrue(TEXT("native selection enabled"), Leaf->bSelectable);
        bPassed &= TestTrue(TEXT("group/node hierarchy retained"), Leaf->GetAttachParent() != Actor->GetRootComponent() &&
            Leaf->IsAttachedTo(Actor->GetTopLevelComponents()[0]));
        const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
        Actor->SetCompositeAsset(Fixture.Asset);
        bPassed &= TestTrue(TEXT("second factory assignment does not resolve again"), Actor->GetResolvedPlan() == Plan);
        Leaf->bSelectable = false;
        Leaf->SetVisibility(false);
        Leaf->SetHiddenInGame(true);
        Actor->RebuildComposite();
        bPassed &= TestEqual(TEXT("rebuild reuses the original mesh component"), PreviewRegressionLeaf(*Actor), Leaf);
        bPassed &= TestTrue(TEXT("derived pickability restored"), Leaf->bSelectable);
        bPassed &= TestTrue(TEXT("derived visibility restored"), Leaf->IsVisible() && !Leaf->bHiddenInGame);
    }
    FString Error;
    TSet<FMHResourceKey> Dependencies;
    UStaticMesh* Pending = nullptr;
    const UMHCompositeSettings& Settings = *GetDefault<UMHCompositeSettings>();
    const auto First = MHGetCompositePreviewGraph(*Fixture.Asset, Settings, Dependencies, Error, Pending);
    const auto Second = MHGetCompositePreviewGraph(*Fixture.Asset, Settings, Dependencies, Error, Pending);
    bPassed &= TestTrue(TEXT("placements share one successful admitted seed-free graph"), First.IsValid() && First == Second);
    FPreviewRegressionFixture Unrelated;
    bPassed &= Unrelated.Build(*this, false);
    bPassed &= TestNotNull(TEXT("creating an unrelated definition preserves the sealed plan lease"), Actor->GetResolvedPlan());
    AMHCompositeActor* Equal = World->SpawnActor<AMHCompositeActor>();
    Equal->SetCompositeAsset(Fixture.Asset);
    bPassed &= TestNotNull(TEXT("placing another instance does not revoke the first plan"), Actor->GetResolvedPlan());
    Equal->Destroy();
    UMHStaticMeshImportData* ForeignReceipt = CastChecked<UMHStaticMeshImportData>(Unrelated.Mesh->GetAssetImportData());
    // Exercise the carrier event separately: Modify also stamps the MH receipt
    // through the module's dirty hook, which correctly revokes all live claims.
    FPropertyChangedEvent CarrierChange(nullptr);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Unrelated.Mesh, CarrierChange);
    bPassed &= TestNotNull(TEXT("canonical unrelated mesh edit preserves plan lease"), Actor->GetResolvedPlan());
    ForeignReceipt->LogicalName = Fixture.Mesh->GetName();
    FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Unrelated.Mesh, CarrierChange);
    bPassed &= TestNull(TEXT("foreign live receipt claim revokes the claimed identity, not only the carrier name"), Actor->GetResolvedPlan());
    ForeignReceipt->LogicalName = Unrelated.Mesh->GetName();
    Actor->RebuildComposite(false);
    ForeignReceipt->PostEditChange();
    bPassed &= TestNull(TEXT("receipt edits conservatively revoke live claims without waiting for an AR event"), Actor->GetResolvedPlan());
    Actor->RebuildComposite(false);
    Fixture.Asset->Modify();
    Fixture.Asset->PostEditChange();
    bPassed &= TestNull(TEXT("applied invalidation hides stale plan from Break and inspection without a seed/move"),
        Actor->GetResolvedPlan());
    const auto AfterModification = MHGetCompositePreviewGraph(*Fixture.Asset, Settings, Dependencies, Error, Pending);
    bPassed &= TestTrue(TEXT("in-memory applied edit invalidates the cache"), AfterModification.IsValid() && AfterModification != First);
    Fixture.Asset->Modify();
    FMHCompositeDocument ChangedDocument;
    MHExtractCompositeV5(*Fixture.Asset, ChangedDocument, Error);
    ChangedDocument.Nodes[0].Transform.TranslationCm.X = 125.0;
    MHApplyCompositeV5(*Fixture.Asset, ChangedDocument, Error);
    Fixture.Asset->PostEditChange();
    Actor->SetSeed(123);
    if (TestNotNull(TEXT("seed edit refreshes invalidated actor-local graph"), Actor->GetResolvedPlan()))
        bPassed &= TestEqual(TEXT("seed fast path cannot keep old local geometry"),
            Actor->GetResolvedPlan()->Leaves[0].WorldMatrix.GetOrigin().X, 125.0);
    FMHResourceKey Changed;
    Changed.Kind = EMHResourceKind::StaticMesh;
    Changed.LogicalName = Fixture.Mesh->GetName();
    MHNotifyGeneratedResourceChanged(Changed);
    const auto AfterNotify = MHGetCompositePreviewGraph(*Fixture.Asset, Settings, Dependencies, Error, Pending);
    bPassed &= TestTrue(TEXT("dependency notification invalidates before rebuilding"), AfterNotify != AfterModification);
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPreviewPendingCompileRegression,
    "Mimir.V5.Preview.PendingMeshDoesNotBlock", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPreviewPendingCompileRegression::RunTest(const FString& Parameters)
{
    FPreviewRegressionFixture Fixture;
    if (!Fixture.Build(*this, true)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Existing = World->SpawnActor<AMHCompositeActor>();
    Existing->SetCompositeAsset(Fixture.Asset);
    if (!TestNotNull(*Existing->GetLastPlacementError(), Existing->GetResolvedPlan()))
    {
        World->DestroyWorld(false);
        return false;
    }
    const FMHResolvedCompositePlan OldPlan = *Existing->GetResolvedPlan();
    const auto OldComponents = Existing->GetDerivedComponents();
    FString Error;
    TSet<FMHResourceKey> Dependencies;
    UStaticMesh* Pending = nullptr;
    const UMHCompositeSettings& Settings = *GetDefault<UMHCompositeSettings>();
    const auto Graph = MHGetCompositePreviewGraph(*Fixture.Asset, Settings, Dependencies, Error, Pending);
    IConsoleVariable* Async = IConsoleManager::Get().FindConsoleVariable(TEXT("Editor.AsyncStaticMeshCompilation"));
    if (!TestNotNull(TEXT("mesh async compilation control"), Async)) { World->DestroyWorld(false); return false; }
    const int32 OldAsync = Async->GetInt();
    // Pause the real asset compiler so the test cannot accidentally pass simply
    // because a tiny cube finished before the interactive call was inspected.
    Async->Set(2, ECVF_SetByCode);
    Fixture.Mesh->Build(true);
    bool bPassed = TestTrue(TEXT("real mesh build is pending"), Fixture.Mesh->IsCompiling());
    const FMHCompositePlacementCompileResult Deferred = MHCompileCompositePlacementV5(
        *Existing, OldPlan, Graph->Composites.FindChecked(Graph->RootComposite), Settings, OldComponents);
    bPassed &= TestEqual(TEXT("cached-plan endpoint gate returns typed pending"), Deferred.PendingMesh, Fixture.Mesh);
    bPassed &= TestTrue(TEXT("pending endpoint mutates no components"), Existing->GetDerivedComponents() == OldComponents && Deferred.Components.IsEmpty());
    AMHCompositeActor* New = World->SpawnActor<AMHCompositeActor>();
    New->SetCompositeAsset(Fixture.Asset);
    bPassed &= TestNull(TEXT("pending placement has no usable plan/signature"), New->GetResolvedPlan());
    bPassed &= TestTrue(TEXT("placement did not finish paused compilation"), Fixture.Mesh->IsCompiling());
    bPassed &= TestNull(TEXT("pending placement shows helper, not a blocked mesh component"), PreviewRegressionLeaf(*New));
    Async->Set(1, ECVF_SetByCode);
    FStaticMeshCompilingManager::Get().FinishCompilation({Fixture.Mesh});
    FTSTicker::GetCoreTicker().Tick(0.0f);
    bPassed &= TestNotNull(TEXT("completion callback resumes placement automatically"), New->GetResolvedPlan());
    bPassed &= TestNotNull(TEXT("completed placement materializes mesh"), PreviewRegressionLeaf(*New));
    Async->Set(OldAsync, ECVF_SetByCode);
    Existing->Destroy();
    New->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPreviewRenderedHitProxyRegression,
    "Mimir.V5.Preview.RenderedNativeHitProxy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPreviewRenderedHitProxyRegression::RunTest(const FString& Parameters)
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
    FPreviewRegressionFixture Fixture;
    if (!Fixture.Build(*this, true)) return false;
    UWorld* World = Client->GetWorld();
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    const FVector Origin(20000, 20000, 20000);
    Actor->SetActorLocation(Origin);
    Actor->SetCompositeAsset(Fixture.Asset);
    UStaticMeshComponent* Leaf = PreviewRegressionLeaf(*Actor);
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
