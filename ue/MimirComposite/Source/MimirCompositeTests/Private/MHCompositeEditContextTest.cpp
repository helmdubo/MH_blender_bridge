#include "MHRecipeTestFixture.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHInstancePool.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditProjection.h"
#include "UI/MHEditSessionKeys.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace UE::MimirComposite::Tests
{
namespace
{

TArray<TObjectPtr<USceneComponent>> DraftHandles(const UMHCompositeLevelSubsystem& Subsystem)
{
    TArray<TObjectPtr<USceneComponent>> Result;
    const UMHCompositeEditSession* Session = Subsystem.GetEditSession();
    if (Session == nullptr || Session->GetDraft() == nullptr || Session->GetProjection() == nullptr) return Result;
    const UMHCompositeEditDocument* Draft = Session->GetDraft();
    for (int32 Index = 0; Index < Draft->Num(); ++Index)
    {
        if (!Draft->GetParentId(Draft->GetNodeId(Index)).IsValid()) Result.Add(Session->GetProjection()->FindComponentForNodeId(Draft->GetNodeId(Index)));
    }
    return Result;
}

bool MoveDraftNodeToWorld(UMHCompositeLevelSubsystem& Subsystem, const USceneComponent* Handle, const FVector& Location, FString& Error)
{
    UMHCompositeEditSession* Session = Subsystem.GetEditSession();
    if (Session == nullptr || Session->GetProjection() == nullptr || Handle == nullptr) return false;
    FTransform ParentWorld;
    if (!Session->GetProjection()->GetParentWorldForComponent(Handle, ParentWorld)) return false;
    FTransform World = Handle->GetComponentTransform();
    World.SetLocation(Location);
    return Session->SetNodeTransform(Session->GetProjection()->GetNodeIdForComponent(Handle), World.GetRelativeTransform(ParentWorld), Error);
}

/** Root = [mesh A] [composite child(mesh C)] placed twice; the child is the shared definition under edit. */
struct FEditContextFixture
{
    FRecipeFixture Recipe;
    UWorld* World = nullptr;
    AMHCompositeActor* A = nullptr;
    AMHCompositeActor* B = nullptr;
    UMHCompositeAsset* Root = nullptr;
    UMHCompositeAsset* Child = nullptr;
    FString MeshA, MeshC;

    explicit FEditContextFixture(FAutomationTestBase& Test) : Recipe(Test) {}
    ~FEditContextFixture()
    {
        if (UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr)
        {
            FString Ignored;
            if (Subsystem->IsEditingComposite()) Subsystem->CancelEditComposite(Ignored);
        }
        if (World != nullptr) World->DestroyWorld(true);
    }

    bool Build(FAutomationTestBase& Test)
    {
        MeshA = Recipe.Name(TEXT("editctx_mesh_a"));
        MeshC = Recipe.Name(TEXT("editctx_mesh_c"));
        Recipe.Mesh(MeshA);
        Recipe.Mesh(MeshC);
        FMHCompositeDocument ChildDocument;
        {
            FMHCompositeNode& Leaf = ChildDocument.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshC;
            Leaf.Transform.TranslationCm = FVector(0.0, 0.0, 40.0);
        }
        Child = Recipe.Composite(Recipe.Name(TEXT("editctx_child")), ChildDocument, {});
        if (Child == nullptr) return false;
        FMHCompositeDocument RootDocument;
        {
            FMHCompositeNode& Node = RootDocument.Nodes.AddDefaulted_GetRef();
            Node.Kind = EMHCompositeNodeKind::Mesh;
            Node.Resource = MeshA;
        }
        {
            FMHCompositeNode& Nested = RootDocument.Nodes.AddDefaulted_GetRef();
            Nested.Kind = EMHCompositeNodeKind::Composite;
            Nested.Resource = Child->LogicalName;
            Nested.Transform.TranslationCm = FVector(300.0, 0.0, 0.0);
            Nested.Transform.RotationQuat = FQuat(FRotator(0.0, 90.0, 0.0));
        }
        Root = Recipe.Composite(Recipe.Name(TEXT("editctx_root")), RootDocument, {});
        if (Root == nullptr) return false;
        World = UWorld::CreateWorld(EWorldType::Editor, false);
        if (!Test.TestNotNull(TEXT("edit context world"), World)) return false;
        A = Spawn(FTransform(FRotator(0.0, 0.0, 0.0), FVector(100.0, 200.0, 0.0)));
        B = Spawn(FTransform(FVector(0.0, 5000.0, 0.0)));
        return Test.TestNotNull(TEXT("A"), A) && Test.TestNotNull(TEXT("B"), B) &&
            Test.TestTrue(TEXT("A previews: ") + A->GetLastPlacementError(), A->GetResolvedPlan() != nullptr);
    }

    AMHCompositeActor* Spawn(const FTransform& Transform, UMHCompositeAsset* Asset = nullptr)
    {
        FActorSpawnParameters Params;
        Params.ObjectFlags = RF_Transactional;
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), Transform, Params);
        if (Actor == nullptr) return nullptr;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(7);
        Actor->SetAppearanceSeed(11);
        Actor->SetCompositeAsset(Asset != nullptr ? Asset : Root);
        return Actor;
    }

    /** Resident-plan node of the nested composite invocation in Actor, or nullptr. */
    static const FMHResolvedCompositeNode* Invocation(const AMHCompositeActor& Actor)
    {
        const FMHResolvedCompositePlan* Plan = Actor.GetResolvedPlan();
        if (Plan == nullptr) return nullptr;
        return Plan->Nodes.FindByPredicate([](const FMHResolvedCompositeNode& Node) { return Node.SemanticKind == EMHRandomSemanticKind::Composite; });
    }
};

bool CanonicalBytes(const UMHCompositeAsset& Asset, TArray<uint8>& OutBytes)
{
    FMHCompositeDocument Document;
    FString Error;
    return MHExtractCompositeV5(Asset, Document, Error) && MHWriteCanonicalCompositeV5(Document, OutBytes, Error);
}

/** World location of the first pooled leaf of Resource in Actor's resident plan. */
bool LeafWorldLocation(const AMHCompositeActor& Actor, const FString& Resource, FVector& OutLocation)
{
    const UMHCompositeLevelSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
    const UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    const UMHCompositeEditProjection* Projection = Session != nullptr && Session->GetRootPlacement() == &Actor ? Session->GetProjection() : nullptr;
    if (Projection != nullptr && Projection->GetPlan() != nullptr)
    {
        for (const FMHResolvedCompositeLeaf& Leaf : Projection->GetPlan()->Leaves)
        {
            if (Leaf.Resource != Resource) continue;
            if (const USceneComponent* Component = Projection->FindComponentForOrigin(Leaf.Origin))
            {
                OutLocation = Component->GetComponentLocation();
                return true;
            }
        }
    }
    const FMHResolvedCompositePlan* Plan = Actor.GetResolvedPlan();
    const TArray<FMHCompositeLeafMaterialization>& Rows = Actor.GetLeafMaterializations();
    if (Plan == nullptr) return false;
    for (int32 Index = 0; Index < Plan->Leaves.Num() && Index < Rows.Num(); ++Index)
    {
        if (Plan->Leaves[Index].Resource != Resource) continue;
        UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Rows[Index].Component.Get());
        FTransform T;
        if (Bucket == nullptr || !Bucket->GetInstanceTransform(Rows[Index].InstanceIndex, T, true)) return false;
        OutLocation = T.GetLocation();
        return true;
    }
    return false;
}

/** World locations of every pooled leaf row of Actor, in row order. */
TArray<FVector> AllLeafWorldLocations(const AMHCompositeActor& Actor)
{
    TArray<FVector> Result;
    for (const FMHCompositeLeafMaterialization& Row : Actor.GetLeafMaterializations())
    {
        UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
        FTransform T;
        if (Bucket != nullptr && Bucket->GetInstanceTransform(Row.InstanceIndex, T, true)) Result.Add(T.GetLocation());
    }
    return Result;
}

} // namespace

// R6-D0 (docs/16 §2.7): a nested composite invocation opens an edit context —
// a draft of the shared child definition under the root placement, with the
// invocation path and the effective parent transform as context. The source
// is untouched until an explicit publish; Cancel discards the draft; the root
// preview is not rebuilt by opening or cancelling.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextNestedInvocationTest,
    "Mimir.V5.Composite.EditContext.NestedInvocationOpensDraft",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextNestedInvocationTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;
    // Freeze the invocation identity before opening its editing context.
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("root placement resolves the nested invocation"), InvocationNode)) return false;
    const FMHResolvedCompositeNode InvocationCopy = *InvocationNode;
    const FMHResolvedCompositeNode* Invocation = &InvocationCopy;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child source bytes"), CanonicalBytes(*F.Child, ChildBefore))) return false;
    const uint32 RevisionA = F.A->GetPreviewRevision();
    const uint32 RebuildsA = F.A->GetPlacementRebuildCount();

    FString Error;
    bool bPassed = TestFalse(TEXT("a mesh leaf is not an editable definition"), Subsystem->BeginEditNestedComposite(F.A, F.A->GetResolvedPlan()->Leaves[0].Origin, Error));
    bPassed &= TestTrue(TEXT("refusal names the reason"), Error.StartsWith(TEXT("MH_E_")));
    bPassed &= TestFalse(TEXT("no session after refusal"), Subsystem->IsEditingComposite());

    bPassed &= TestTrue(TEXT("nested invocation opens an edit context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Invocation->NodePath, Error));
    bPassed &= TestTrue(TEXT("session is active on the root actor"), Subsystem->IsEditingComposite(F.A));
    const FMHCompositeEditContext Context = Subsystem->GetEditContext();
    bPassed &= TestEqual(TEXT("edited definition is the child"), Context.EditedLogicalName, F.Child->LogicalName);
    bPassed &= TestEqual(TEXT("edited source path is the child's"), Context.EditedSourceRelativePath, F.Child->SourceRelativePath);
    bPassed &= TestEqual(TEXT("invocation path is the nested node's"), Context.InvocationPath, Invocation->NodePath);
    bPassed &= TestTrue(TEXT("context names the root placement"), Context.RootPlacement == F.A);
    const FMatrix ExpectedParent = Invocation->WorldMatrix * F.A->GetActorTransform().ToMatrixWithScale();
    bPassed &= TestTrue(TEXT("effective parent transform is the invocation's world matrix under the placement basis"),
        Context.EffectiveParentWorld.Equals(ExpectedParent, 1e-4));
    bPassed &= TestEqual(TEXT("every live placement of the shared definition is counted"), Context.ConsumerPlacements, 2);
    bPassed &= TestEqual(TEXT("save scope is the shared definition"), Context.SaveScope, EMHCompositeEditSaveScope::SharedDefinition);
    FMHCompositeDocument ChildDocument;
    bPassed &= TestTrue(TEXT("child extracts"), MHExtractCompositeV5(*F.Child, ChildDocument, Error));
    bPassed &= TestEqual(TEXT("the draft starts as the child definition"), Subsystem->GetEditingDraft().Nodes.Num(), ChildDocument.Nodes.Num());
    bPassed &= TestEqual(TEXT("the root's own session name reports the edited child"), Subsystem->GetEditingCompositeLogicalName(), F.Child->LogicalName);
    // Opening the context projects the selected occurrence; the published plan stays sealed.
    bPassed &= TestTrue(TEXT("opening the context enters edit mode under the scope"), Subsystem->IsEditingComposite(F.A) && Subsystem->GetEditContext().InvocationPath == Invocation->NodePath);
    bPassed &= TestTrue(TEXT("opening the context keeps a resolved plan"), F.A->GetResolvedPlan() != nullptr && F.A->GetLastPlacementError().IsEmpty());
    bPassed &= TestEqual(TEXT("opening keeps the placement revision"), F.A->GetPreviewRevision(), RevisionA);
    bPassed &= TestFalse(TEXT("a second session is refused while one is active"), Subsystem->BeginEditComposite(F.B, Error));

    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("no session after cancel"), Subsystem->IsEditingComposite());
    bPassed &= TestTrue(TEXT("context is empty after cancel"), Subsystem->GetEditContext().EditedLogicalName.IsEmpty());
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("child source bytes after cancel"), CanonicalBytes(*F.Child, ChildAfter));
    bPassed &= TestTrue(TEXT("cancel leaves the shared definition untouched"), ChildAfter == ChildBefore);
    bPassed &= TestTrue(TEXT("cancel leaves edit mode and the scope"), !Subsystem->IsEditingComposite(F.A) && Subsystem->GetEditContext().InvocationPath.IsEmpty());
    bPassed &= TestEqual(TEXT("begin/cancel does not rebuild the placement"), F.A->GetPlacementRebuildCount(), RebuildsA);
    bPassed &= TestNotNull(TEXT("root still previews"), F.A->GetResolvedPlan());
    return bPassed;
}

// R6-D1b (docs/16 §2.7): inside a nested edit context the child definition's
// nodes get handles under the invocation's effective parent transform. Moving
// a node edits the draft local transform (EditedLocal = EditedWorld *
// inverse(ParentEffectiveWorld)); its projection follows, while the pooled
// placement and other occurrences stay untouched until a
// publish, and Cancel restores the preview from the unchanged definition.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextNestedHandlesTest,
    "Mimir.V5.Composite.EditContext.NestedHandlesEditDraftUnderParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextNestedHandlesTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("nested invocation"), InvocationNode)) return false;
    const FMHResolvedCompositeNode InvocationCopy = *InvocationNode;
    const FMHResolvedCompositeNode* Invocation = &InvocationCopy;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child source bytes"), CanonicalBytes(*F.Child, ChildBefore))) return false;
    UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(F.World);
    if (!TestNotNull(TEXT("pool"), Pool)) return false;

    const auto LeafWorld = &LeafWorldLocation;
    FVector MeshCBeforeA, MeshCBeforeB, MeshABeforeA;
    bool bPassed = TestTrue(TEXT("mesh C renders in A"), LeafWorld(*F.A, F.MeshC, MeshCBeforeA));
    bPassed &= TestTrue(TEXT("mesh C renders in B"), LeafWorld(*F.B, F.MeshC, MeshCBeforeB));
    bPassed &= TestTrue(TEXT("sibling mesh A renders before edit"), LeafWorld(*F.A, F.MeshA, MeshABeforeA));
    const int32 OtherLiveBefore = Pool->NumLiveInstances(*F.B);
    const FMatrix ParentWorld = Invocation->WorldMatrix * F.A->GetActorTransform().ToMatrixWithScale();
    const int32 LiveBefore = Pool->NumLiveInstances(*F.A);

    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Invocation->NodePath, Error))) return false;
    bPassed &= TestTrue(TEXT("root placement is in edit mode for the nested scope"), Subsystem->IsEditingComposite(F.A));
    const TArray<TObjectPtr<USceneComponent>>& Handles = DraftHandles(*Subsystem);
    bPassed &= TestEqual(TEXT("one handle per child definition node"), Handles.Num(), 1);
    if (Handles.Num() != 1 || !IsValid(Handles[0])) return false;
    USceneComponent* Handle = Handles[0];
    // The child node sits at (0,0,40) in the child's space -> under the invocation's world.
    const FVector ExpectedHandle = FTransform(FTransform(FVector(0, 0, 40)).ToMatrixWithScale() * ParentWorld).GetLocation();
    bPassed &= TestTrue(TEXT("handle sits at the node's world under the effective parent"), Handle->GetComponentLocation().Equals(ExpectedHandle, 1e-2));
    bPassed &= TestTrue(TEXT("handle belongs to the edit projection"), Handle->GetOwner() == Subsystem->GetEditSession()->GetProjection()->GetProjectionActor());

    // Drag: +100 along world X.
    bPassed &= TestTrue(TEXT("move node"), MoveDraftNodeToWorld(*Subsystem, Handle, ExpectedHandle + FVector(100, 0, 0), Error));
    bPassed &= TestTrue(TEXT("draft command preserves the sealed preview: ") + F.A->GetLastPlacementError(), F.A->GetLastPlacementError().IsEmpty() && F.A->GetResolvedPlan() != nullptr);
    FMHCompositeDocument Draft;
    bPassed &= TestTrue(TEXT("edited draft is available"), Subsystem->GetEditSession()->GetDraft()->Extract(Draft, Error));
    const FMatrix EditedLocal = FTransform(ExpectedHandle + FVector(100, 0, 0)).ToMatrixWithScale() * ParentWorld.Inverse();
    bPassed &= TestTrue(TEXT("draft node local = edited world * inverse(parent effective world)"),
        Draft.Nodes.Num() == 1 && Draft.Nodes[0].Transform.TranslationCm.Equals(FTransform(EditedLocal).GetLocation(), 1e-2));
    FVector MeshCAfterA;
    bPassed &= TestTrue(TEXT("A renders the moved leaf"), LeafWorld(*F.A, F.MeshC, MeshCAfterA) && MeshCAfterA.Equals(MeshCBeforeA + FVector(100, 0, 0), 1e-2));
    // The selected occurrence leaves the visible pool while its editable mesh
    // replaces it. Count both representations and verify the lease scope.
    const UMHCompositeEditProjection* Projection = Subsystem->GetEditSession()->GetProjection();
    int32 ProjectedMeshes = 0;
    for (const TObjectPtr<USceneComponent>& Component : Projection->GetComponents())
    {
        const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component.Get());
        if (IsValid(Mesh) && Mesh->IsRegistered() && Mesh->IsVisible() && Mesh->GetStaticMesh() != nullptr) ++ProjectedMeshes;
    }
    bPassed &= TestEqual(TEXT("the edited occurrence has one visible mesh in the projection"), ProjectedMeshes, 1);
    bPassed &= TestEqual(TEXT("pool plus projection preserves exactly the original visible leaf count"), Pool->NumLiveInstances(*F.A) + ProjectedMeshes, LiveBefore);
    int32 SuppressedLeaves = 0;
    const FString EditedPrefix = InvocationCopy.NodePath + TEXT(">");
    for (const FMHCompositeLeafMaterialization& Row : F.A->GetLeafMaterializations())
    {
        const bool bEditedLeaf = Row.NodePath.StartsWith(EditedPrefix);
        const bool bSuppressed = Pool->IsSuppressed(Row.Handle);
        bPassed &= TestEqual(TEXT("only leaves of the edited occurrence are suppressed"), bSuppressed, bEditedLeaf);
        if (bSuppressed) ++SuppressedLeaves;
    }
    bPassed &= TestEqual(TEXT("the projected mesh replaces exactly one suppressed leaf"), SuppressedLeaves, ProjectedMeshes);
    FVector MeshAAfterA;
    bPassed &= TestTrue(TEXT("the sibling mesh stays at its published world transform"), LeafWorld(*F.A, F.MeshA, MeshAAfterA) && MeshAAfterA.Equals(MeshABeforeA, 1e-2));
    bPassed &= TestEqual(TEXT("the other placement retains every pooled leaf"), Pool->NumLiveInstances(*F.B), OtherLiveBefore);
    FVector MeshCB;
    bPassed &= TestTrue(TEXT("B is untouched until a publish"), LeafWorld(*F.B, F.MeshC, MeshCB) && MeshCB.Equals(MeshCBeforeB, 1e-2));
    bPassed &= TestEqual(TEXT("the draft is the session's document"), Subsystem->GetEditingDraft().Nodes.Num(), 1);

    // Cancel: the preview comes back from the unchanged definition.
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("edit mode ended"), Subsystem->IsEditingComposite(F.A));
    bPassed &= TestEqual(TEXT("scope handles retired"), DraftHandles(*Subsystem).Num(), 0);
    bPassed &= TestEqual(TEXT("cancel restores the full published pool view"), Pool->NumLiveInstances(*F.A), LiveBefore);
    FVector MeshCRestored;
    bPassed &= TestTrue(TEXT("cancel restores the leaf"), LeafWorld(*F.A, F.MeshC, MeshCRestored) && MeshCRestored.Equals(MeshCBeforeA, 1e-2));
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("child bytes"), CanonicalBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);
    return bPassed;
}


// R6-D2 (docs/16 §2.7): Apply Shared Definition publishes the nested draft as
// the child definition's source and refreshes every placement invoking it.
// Decision (a), 2026-09-06: no revision guard — the file is overwritten as-is.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextApplySharedDefinitionTest,
    "Mimir.V5.Composite.EditContext.ApplySharedDefinitionUpdatesConsumers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextApplySharedDefinitionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("nested invocation"), InvocationNode)) return false;
    const FMHResolvedCompositeNode InvocationCopy = *InvocationNode;
    TArray<uint8> ChildBefore, RootBefore;
    if (!TestTrue(TEXT("child source bytes"), CanonicalBytes(*F.Child, ChildBefore))) return false;
    if (!TestTrue(TEXT("root source bytes"), CanonicalBytes(*F.Root, RootBefore))) return false;
    FVector MeshCBeforeA, MeshCBeforeB;
    bool bPassed = TestTrue(TEXT("mesh C renders in A"), LeafWorldLocation(*F.A, F.MeshC, MeshCBeforeA));
    bPassed &= TestTrue(TEXT("mesh C renders in B"), LeafWorldLocation(*F.B, F.MeshC, MeshCBeforeB));
    const FMatrix ParentWorld = InvocationCopy.WorldMatrix * F.A->GetActorTransform().ToMatrixWithScale();

    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationCopy.NodePath, Error))) return false;
    const TArray<TObjectPtr<USceneComponent>>& Handles = DraftHandles(*Subsystem);
    if (!TestEqual(TEXT("one handle"), Handles.Num(), 1) || !IsValid(Handles[0])) return false;
    const FVector HandleBefore = Handles[0]->GetComponentLocation();
    bPassed &= TestTrue(TEXT("move node"), MoveDraftNodeToWorld(*Subsystem, Handles[0], HandleBefore + FVector(100, 0, 0), Error));
    const FVector ExpectedLocal = FTransform(FTransform(HandleBefore + FVector(100, 0, 0)).ToMatrixWithScale() * ParentWorld.Inverse()).GetLocation();

    // The publish seam stands in for MHPublishCompositeV5: the asset arrives
    // applied, and consumers are notified as the
    // real publisher does after writing the source.
    UMHCompositeAsset* PublishedAsset = nullptr;
    Subsystem->SetCommitPublisherForTests(
        [&PublishedAsset](UMHCompositeAsset& Asset, FString&)
        {
            PublishedAsset = &Asset;
            MHNotifyCompositeAssetChanged(Asset);
            return true;
        });
    TArray<FString> Warnings;
    const bool bCommitted = Subsystem->CommitEditComposite(Warnings, Error);
    Subsystem->SetCommitPublisherForTests({});
    bPassed &= TestTrue(TEXT("apply shared definition: ") + Error, bCommitted);
    bPassed &= TestTrue(TEXT("the shared child definition is what gets published"), PublishedAsset == F.Child);
    bPassed &= TestTrue(TEXT("UE Undo is cleared after a successful publish"), GEditor->Trans != nullptr && !GEditor->Trans->CanUndo());
    bPassed &= TestFalse(TEXT("session ended"), Subsystem->IsEditingComposite());
    bPassed &= TestFalse(TEXT("root left edit mode"), Subsystem->IsEditingComposite(F.A));
    bPassed &= TestEqual(TEXT("scope handles retired"), DraftHandles(*Subsystem).Num(), 0);

    FMHCompositeDocument ChildDocument;
    bPassed &= TestTrue(TEXT("child definition carries the edited node"),
        MHExtractCompositeV5(*F.Child, ChildDocument, Error) && ChildDocument.Nodes.Num() == 1 &&
        ChildDocument.Nodes[0].Transform.TranslationCm.Equals(ExpectedLocal, 1e-2));
    TArray<uint8> ChildAfter, RootAfter;
    bPassed &= TestTrue(TEXT("child bytes changed"), CanonicalBytes(*F.Child, ChildAfter) && ChildAfter != ChildBefore);
    bPassed &= TestTrue(TEXT("root definition untouched"), CanonicalBytes(*F.Root, RootAfter) && RootAfter == RootBefore);
    // Both placements invoke the child with the same rotation (yaw 90°, no
    // actor rotation): the +100 X world edit in A is +100 X in B as well.
    FVector MeshCAfterA, MeshCAfterB;
    bPassed &= TestTrue(TEXT("A renders the published node"), LeafWorldLocation(*F.A, F.MeshC, MeshCAfterA) && MeshCAfterA.Equals(MeshCBeforeA + FVector(100, 0, 0), 1e-2));
    bPassed &= TestTrue(TEXT("B follows the shared definition"), LeafWorldLocation(*F.B, F.MeshC, MeshCAfterB) && MeshCAfterB.Equals(MeshCBeforeB + FVector(100, 0, 0), 1e-2));
    return bPassed;
}

// R6-D2: a refused publish keeps the shared definition and every placement as
// they were; the failed draft stays recoverable until Cancel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextApplySharedDefinitionFailureTest,
    "Mimir.V5.Composite.EditContext.ApplySharedDefinitionFailureKeepsDefinition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextApplySharedDefinitionFailureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("nested invocation"), InvocationNode)) return false;
    const FString InvocationPath = InvocationNode->NodePath;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child source bytes"), CanonicalBytes(*F.Child, ChildBefore))) return false;
    FVector MeshCBeforeA, MeshCBeforeB;
    bool bPassed = TestTrue(TEXT("mesh C renders in A"), LeafWorldLocation(*F.A, F.MeshC, MeshCBeforeA));
    bPassed &= TestTrue(TEXT("mesh C renders in B"), LeafWorldLocation(*F.B, F.MeshC, MeshCBeforeB));

    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationPath, Error))) return false;
    const TArray<TObjectPtr<USceneComponent>>& Handles = DraftHandles(*Subsystem);
    if (!TestEqual(TEXT("one handle"), Handles.Num(), 1) || !IsValid(Handles[0])) return false;
    bPassed &= TestTrue(TEXT("move node"), MoveDraftNodeToWorld(*Subsystem, Handles[0], Handles[0]->GetComponentLocation() + FVector(100, 0, 0), Error));

    Subsystem->SetCommitPublisherForTests(
        [](UMHCompositeAsset&, FString& OutError)
        {
            OutError = TEXT("MH_E_TEST_PUBLISH_REFUSED: source write refused by the test seam");
            return false;
        });
    TArray<FString> Warnings;
    const bool bCommitted = Subsystem->CommitEditComposite(Warnings, Error);
    Subsystem->SetCommitPublisherForTests({});
    bPassed &= TestFalse(TEXT("refused publish fails the apply"), bCommitted);
    bPassed &= TestTrue(TEXT("the refusal is reported"), Error.Contains(TEXT("MH_E_TEST_PUBLISH_REFUSED")));
    bPassed &= TestTrue(TEXT("failed publish keeps the draft session"), Subsystem->IsEditingComposite());
    bPassed &= TestTrue(TEXT("cancel failed draft"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("root left edit mode"), Subsystem->IsEditingComposite(F.A));
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("child definition kept"), CanonicalBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);
    FVector MeshCAfterA, MeshCAfterB;
    bPassed &= TestTrue(TEXT("A renders the kept node"), LeafWorldLocation(*F.A, F.MeshC, MeshCAfterA) && MeshCAfterA.Equals(MeshCBeforeA, 1e-2));
    bPassed &= TestTrue(TEXT("B renders the kept node"), LeafWorldLocation(*F.B, F.MeshC, MeshCAfterB) && MeshCAfterB.Equals(MeshCBeforeB, 1e-2));
    return bPassed;
}

// R6-U1 (docs/16 §2.7): Save Unique, procedural variant. "Make Unique for This
// Placement" copies the invocation chain up to this placement's root and
// switches only this placement to the new root; its root-level streams stay
// keyed by the original root through the placement's call context.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextMakeUniqueForPlacementTest,
    "Mimir.V5.Composite.EditContext.MakeUniqueForPlacementSwitchesOnlyThisPlacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextMakeUniqueForPlacementTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("nested invocation"), InvocationNode)) return false;
    const FMHResolvedCompositeNode InvocationCopy = *InvocationNode;
    TArray<uint8> ChildBefore, RootBefore;
    if (!TestTrue(TEXT("child source bytes"), CanonicalBytes(*F.Child, ChildBefore))) return false;
    if (!TestTrue(TEXT("root source bytes"), CanonicalBytes(*F.Root, RootBefore))) return false;
    FVector MeshCBeforeA, MeshCBeforeB;
    bool bPassed = TestTrue(TEXT("mesh C renders in A"), LeafWorldLocation(*F.A, F.MeshC, MeshCBeforeA));
    bPassed &= TestTrue(TEXT("mesh C renders in B"), LeafWorldLocation(*F.B, F.MeshC, MeshCBeforeB));
    const FMatrix ParentWorld = InvocationCopy.WorldMatrix * F.A->GetActorTransform().ToMatrixWithScale();

    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationCopy.NodePath, Error))) return false;
    const TArray<TObjectPtr<USceneComponent>>& Handles = DraftHandles(*Subsystem);
    if (!TestEqual(TEXT("one handle"), Handles.Num(), 1) || !IsValid(Handles[0])) return false;
    const FVector HandleBefore = Handles[0]->GetComponentLocation();
    bPassed &= TestTrue(TEXT("move node"), MoveDraftNodeToWorld(*Subsystem, Handles[0], HandleBefore + FVector(100, 0, 0), Error));
    const FVector ExpectedLocal = FTransform(FTransform(HandleBefore + FVector(100, 0, 0)).ToMatrixWithScale() * ParentWorld.Inverse()).GetLocation();

    FMHCompositeSaveUniquePlan Plan;
    bPassed &= TestTrue(TEXT("describe: ") + Error, Subsystem->DescribeSaveUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, Plan, Error));
    bPassed &= TestTrue(TEXT("copies: the edited child, then the placement's root"),
        Plan.Copies.Num() == 2 && Plan.Copies[0] == F.Child->LogicalName && Plan.Copies[1] == F.Root->LogicalName);
    bPassed &= TestTrue(TEXT("no shared definition is overwritten for this scope"), Plan.OverwrittenDefinition.IsEmpty());
    bPassed &= TestTrue(TEXT("no re-roll warning without randomization"), Plan.Warnings.IsEmpty());
    if (Plan.Copies.Num() != 2) return false;

    // The creation seam stands in for validate/publish/import (shared with Build).
    TArray<FString> Created;
    Subsystem->SetDefinitionCreatorForTests(
        [&F, &Created](const FMHCompositeDocument& Document, const FMHCompositeAdoptTarget& Target, FString&) -> UMHCompositeAsset*
        {
            Created.Add(Target.LogicalName);
            return F.Recipe.Composite(Target.LogicalName, Document, {});
        });
    TArray<FMHCompositeAdoptTarget> Targets;
    for (const FString& Copy : Plan.Copies)
    {
        FMHCompositeAdoptTarget& Target = Targets.AddDefaulted_GetRef();
        Target.LogicalName = Copy + TEXT("_u");
    }
    TArray<FString> Warnings;
    const bool bSaved = Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, Targets, Warnings, Error);
    Subsystem->SetDefinitionCreatorForTests({});
    bPassed &= TestTrue(TEXT("save unique: ") + Error, bSaved);
    bPassed &= TestTrue(TEXT("definitions are created innermost first"),
        Created.Num() == 2 && Created[0] == Targets[0].LogicalName && Created[1] == Targets[1].LogicalName);
    bPassed &= TestFalse(TEXT("session ended"), Subsystem->IsEditingComposite());
    bPassed &= TestFalse(TEXT("root left edit mode"), Subsystem->IsEditingComposite(F.A));
    bPassed &= TestEqual(TEXT("scope handles retired"), DraftHandles(*Subsystem).Num(), 0);

    const UMHCompositeAsset* NewRoot = F.A->GetCompositeAsset();
    bPassed &= TestTrue(TEXT("this placement now invokes the unique root"), NewRoot != nullptr && NewRoot->LogicalName == Targets[1].LogicalName);
    FMHCompositeDocument NewRootDocument;
    bPassed &= TestTrue(TEXT("the unique root invokes the unique child at the same slot"),
        NewRoot != nullptr && MHExtractCompositeV5(*NewRoot, NewRootDocument, Error) && NewRootDocument.Nodes.Num() == 2 &&
        NewRootDocument.Nodes[1].Kind == EMHCompositeNodeKind::Composite && NewRootDocument.Nodes[1].Resource == Targets[0].LogicalName);
    const UMHCompositeAsset* NewChild = F.Recipe.Composites.FindRef(Targets[0].LogicalName);
    FMHCompositeDocument NewChildDocument;
    bPassed &= TestTrue(TEXT("the unique child carries the edited node"),
        NewChild != nullptr && MHExtractCompositeV5(*NewChild, NewChildDocument, Error) && NewChildDocument.Nodes.Num() == 1 &&
        NewChildDocument.Nodes[0].Transform.TranslationCm.Equals(ExpectedLocal, 1e-2));
    bPassed &= TestEqual(TEXT("root-level streams stay keyed by the original root"), F.A->GetCallContext().StreamNamespace, F.Root->LogicalName);
    TArray<uint8> ChildAfter, RootAfter;
    bPassed &= TestTrue(TEXT("shared child untouched"), CanonicalBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);
    bPassed &= TestTrue(TEXT("shared root untouched"), CanonicalBytes(*F.Root, RootAfter) && RootAfter == RootBefore);
    FVector MeshCAfterA, MeshCAfterB;
    bPassed &= TestTrue(TEXT("A renders the unique node"), LeafWorldLocation(*F.A, F.MeshC, MeshCAfterA) && MeshCAfterA.Equals(MeshCBeforeA + FVector(100, 0, 0), 1e-2));
    bPassed &= TestTrue(TEXT("B keeps the shared definition"), LeafWorldLocation(*F.B, F.MeshC, MeshCAfterB) && MeshCAfterB.Equals(MeshCBeforeB, 1e-2));
    bPassed &= TestTrue(TEXT("B still invokes the shared root"), F.B->GetCompositeAsset() == F.Root);
    return bPassed;
}

// R6-U1: "Make Child Unique in This Definition" copies the edited definition
// and points the invoking (shared) definition at the copy: every placement of
// that definition follows; the original child definition is untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextMakeUniqueInDefinitionTest,
    "Mimir.V5.Composite.EditContext.MakeUniqueInDefinitionRewiresSharedParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextMakeUniqueInDefinitionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("nested invocation"), InvocationNode)) return false;
    const FString InvocationPath = InvocationNode->NodePath;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child source bytes"), CanonicalBytes(*F.Child, ChildBefore))) return false;
    FVector MeshCBeforeA, MeshCBeforeB;
    bool bPassed = TestTrue(TEXT("mesh C renders in A"), LeafWorldLocation(*F.A, F.MeshC, MeshCBeforeA));
    bPassed &= TestTrue(TEXT("mesh C renders in B"), LeafWorldLocation(*F.B, F.MeshC, MeshCBeforeB));

    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationPath, Error))) return false;
    const TArray<TObjectPtr<USceneComponent>>& Handles = DraftHandles(*Subsystem);
    if (!TestEqual(TEXT("one handle"), Handles.Num(), 1) || !IsValid(Handles[0])) return false;
    bPassed &= TestTrue(TEXT("move node"), MoveDraftNodeToWorld(*Subsystem, Handles[0], Handles[0]->GetComponentLocation() + FVector(100, 0, 0), Error));

    FMHCompositeSaveUniquePlan Plan;
    bPassed &= TestTrue(TEXT("describe: ") + Error, Subsystem->DescribeSaveUnique(EMHCompositeUniqueScope::InParentDefinition, EMHCompositeUniqueVariant::Procedural, Plan, Error));
    bPassed &= TestTrue(TEXT("only the edited child is copied"), Plan.Copies.Num() == 1 && Plan.Copies[0] == F.Child->LogicalName);
    bPassed &= TestEqual(TEXT("the invoking definition is overwritten"), Plan.OverwrittenDefinition, F.Root->LogicalName);
    if (Plan.Copies.Num() != 1) return false;

    Subsystem->SetDefinitionCreatorForTests(
        [&F](const FMHCompositeDocument& Document, const FMHCompositeAdoptTarget& Target, FString&) -> UMHCompositeAsset*
        {
            return F.Recipe.Composite(Target.LogicalName, Document, {});
        });
    UMHCompositeAsset* PublishedAsset = nullptr;
    Subsystem->SetCommitPublisherForTests(
        [&PublishedAsset](UMHCompositeAsset& Asset, FString&)
        {
            PublishedAsset = &Asset;
            MHNotifyCompositeAssetChanged(Asset);
            return true;
        });
    TArray<FMHCompositeAdoptTarget> Targets;
    Targets.AddDefaulted_GetRef().LogicalName = F.Child->LogicalName + TEXT("_u");
    TArray<FString> Warnings;
    const bool bSaved = Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::InParentDefinition, EMHCompositeUniqueVariant::Procedural, Targets, Warnings, Error);
    Subsystem->SetDefinitionCreatorForTests({});
    Subsystem->SetCommitPublisherForTests({});
    bPassed &= TestTrue(TEXT("save unique: ") + Error, bSaved);
    bPassed &= TestTrue(TEXT("the shared root is what gets published"), PublishedAsset == F.Root);
    bPassed &= TestFalse(TEXT("session ended"), Subsystem->IsEditingComposite());
    FMHCompositeDocument RootDocument;
    bPassed &= TestTrue(TEXT("the root now invokes the unique child"),
        MHExtractCompositeV5(*F.Root, RootDocument, Error) && RootDocument.Nodes.Num() == 2 && RootDocument.Nodes[1].Resource == Targets[0].LogicalName);
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("original child untouched"), CanonicalBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);
    bPassed &= TestTrue(TEXT("this placement keeps its root"), F.A->GetCompositeAsset() == F.Root);
    bPassed &= TestTrue(TEXT("call context stays empty"), F.A->GetCallContext().IsEmpty());
    FVector MeshCAfterA, MeshCAfterB;
    bPassed &= TestTrue(TEXT("A renders the unique node"), LeafWorldLocation(*F.A, F.MeshC, MeshCAfterA) && MeshCAfterA.Equals(MeshCBeforeA + FVector(100, 0, 0), 1e-2));
    bPassed &= TestTrue(TEXT("B follows the rewired root"), LeafWorldLocation(*F.B, F.MeshC, MeshCAfterB) && MeshCAfterB.Equals(MeshCBeforeB + FVector(100, 0, 0), 1e-2));
    return bPassed;
}

// R6-U1: Save Unique validates its targets before anything is written, and the
// re-roll warning names exactly the copies that carry randomization.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextSaveUniqueValidationTest,
    "Mimir.V5.Composite.EditContext.SaveUniqueValidatesTargetsAndWarnsOnRandom",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextSaveUniqueValidationTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    {
        FMHCompositeDocument Plain;
        Plain.Nodes.AddDefaulted_GetRef().Kind = EMHCompositeNodeKind::Mesh;
        FMHCompositeDocument WithRandom = Plain;
        FMHCompositeNode& Random = WithRandom.Nodes.AddDefaulted_GetRef();
        Random.Kind = EMHCompositeNodeKind::Random;
        Random.Options.AddDefaulted_GetRef().Kind = EMHCompositeOptionKind::Mesh;
        FMHCompositeDocument WithProfile = Plain;
        WithProfile.Nodes[0].Children.AddDefaulted_GetRef().Profile = TEXT("some_profile");
        if (!TestFalse(TEXT("plain document has no randomization"), MHCompositeDocumentHasRandomization(Plain))) return false;
        if (!TestTrue(TEXT("random node is randomization"), MHCompositeDocumentHasRandomization(WithRandom))) return false;
        if (!TestTrue(TEXT("nested placement profile is randomization"), MHCompositeDocumentHasRandomization(WithProfile))) return false;
    }
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("nested invocation"), InvocationNode)) return false;
    const FString InvocationPath = InvocationNode->NodePath;
    FString Error;
    TArray<FString> Warnings;
    FMHCompositeSaveUniquePlan Plan;
    bool bPassed = TestFalse(TEXT("no session: describe refuses"), Subsystem->DescribeSaveUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, Plan, Error));
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationPath, Error))) return false;

    bool bCreatorCalled = false;
    Subsystem->SetDefinitionCreatorForTests(
        [&bCreatorCalled](const FMHCompositeDocument&, const FMHCompositeAdoptTarget&, FString&) -> UMHCompositeAsset*
        {
            bCreatorCalled = true;
            return nullptr;
        });
    TArray<FMHCompositeAdoptTarget> TooFew;
    bPassed &= TestFalse(TEXT("wrong target count refused"), Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, TooFew, Warnings, Error));
    TArray<FMHCompositeAdoptTarget> BadName;
    BadName.AddDefaulted_GetRef().LogicalName = TEXT("Not Canonical");
    bPassed &= TestFalse(TEXT("non-canonical name refused"), Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::InParentDefinition, EMHCompositeUniqueVariant::Procedural, BadName, Warnings, Error));
    bPassed &= TestTrue(TEXT("the refusal names the token rule"), Error.Contains(TEXT("MH_E_NONCANONICAL_RESOURCE_NAME")));
    TArray<FMHCompositeAdoptTarget> SameAsOriginal;
    SameAsOriginal.AddDefaulted_GetRef().LogicalName = F.Child->LogicalName;
    bPassed &= TestFalse(TEXT("a name already on the chain is refused"), Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::InParentDefinition, EMHCompositeUniqueVariant::Procedural, SameAsOriginal, Warnings, Error));
    TArray<FMHCompositeAdoptTarget> Duplicate;
    Duplicate.AddDefaulted_GetRef().LogicalName = F.Child->LogicalName + TEXT("_same");
    Duplicate.AddDefaulted_GetRef().LogicalName = F.Child->LogicalName + TEXT("_same");
    bPassed &= TestFalse(TEXT("duplicate target names are refused"), Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, Duplicate, Warnings, Error));
    Subsystem->SetDefinitionCreatorForTests({});
    bPassed &= TestFalse(TEXT("nothing was created"), bCreatorCalled);
    bPassed &= TestTrue(TEXT("the session survives refused saves"), Subsystem->IsEditingComposite() && Subsystem->IsEditingComposite(F.A));
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

// R6-U2 (docs/16 §2.7): Bake Current Result — the unique copy is the resolved
// subtree of the edited definition under this placement, as concrete mesh and
// actor nodes: no random draws, so nothing re-rolls under the new name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextBakeCurrentResultTest,
    "Mimir.V5.Composite.EditContext.BakeCurrentResultKeepsResolvedLeaves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextBakeCurrentResultTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;

    // A definition that draws: a random mesh (C or A) at z=40 and a group carrying mesh C.
    FMHCompositeDocument RandomChildDocument;
    {
        FMHCompositeNode& Random = RandomChildDocument.Nodes.AddDefaulted_GetRef();
        Random.Kind = EMHCompositeNodeKind::Random;
        Random.Name = TEXT("pick");
        Random.Transform.TranslationCm = FVector(0.0, 0.0, 40.0);
        FMHCompositeOption& OptionC = Random.Options.AddDefaulted_GetRef();
        OptionC.Kind = EMHCompositeOptionKind::Mesh;
        OptionC.Resource = F.MeshC;
        OptionC.Weight = 1.0f;
        FMHCompositeOption& OptionA = Random.Options.AddDefaulted_GetRef();
        OptionA.Kind = EMHCompositeOptionKind::Mesh;
        OptionA.Resource = F.MeshA;
        OptionA.Weight = 1.0f;
        FMHCompositeNode& Group = RandomChildDocument.Nodes.AddDefaulted_GetRef();
        Group.Kind = EMHCompositeNodeKind::Group;
        Group.Transform.TranslationCm = FVector(10.0, 0.0, 0.0);
        FMHCompositeNode& Grouped = Group.Children.AddDefaulted_GetRef();
        Grouped.Kind = EMHCompositeNodeKind::Mesh;
        Grouped.Resource = F.MeshC;
        Grouped.Transform.TranslationCm = FVector(0.0, 0.0, 5.0);
    }
    UMHCompositeAsset* RandomChild = F.Recipe.Composite(F.Recipe.Name(TEXT("editctx_child_rnd")), RandomChildDocument, {});
    FMHCompositeDocument RandomRootDocument;
    {
        FMHCompositeNode& Node = RandomRootDocument.Nodes.AddDefaulted_GetRef();
        Node.Kind = EMHCompositeNodeKind::Mesh;
        Node.Resource = F.MeshA;
        FMHCompositeNode& Nested = RandomRootDocument.Nodes.AddDefaulted_GetRef();
        Nested.Kind = EMHCompositeNodeKind::Composite;
        Nested.Resource = RandomChild != nullptr ? RandomChild->LogicalName : FString();
        Nested.Transform.TranslationCm = FVector(300.0, 0.0, 0.0);
        Nested.Transform.RotationQuat = FQuat(FRotator(0.0, 90.0, 0.0));
    }
    UMHCompositeAsset* RandomRoot = RandomChild != nullptr ? F.Recipe.Composite(F.Recipe.Name(TEXT("editctx_root_rnd")), RandomRootDocument, {}) : nullptr;
    if (!TestNotNull(TEXT("random child"), RandomChild) || !TestNotNull(TEXT("random root"), RandomRoot)) return false;
    AMHCompositeActor* P = F.Spawn(FTransform(FVector(0.0, -3000.0, 0.0)), RandomRoot);
    if (!TestNotNull(TEXT("P"), P) || !TestTrue(TEXT("P previews: ") + P->GetLastPlacementError(), P->GetResolvedPlan() != nullptr)) return false;
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*P);
    if (!TestNotNull(TEXT("nested invocation"), InvocationNode)) return false;
    const FMHResolvedCompositeNode InvocationCopy = *InvocationNode;
    TArray<FVector> LeavesBefore = AllLeafWorldLocations(*P);
    bool bPassed = TestEqual(TEXT("P renders mesh A, the random pick and the grouped mesh"), LeavesBefore.Num(), 3);

    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(P, InvocationCopy.NodePath, Error))) return false;
    UMHCompositeEditSession* BakeSession = Subsystem->GetEditSession();
    const int32 GroupIndex = BakeSession->GetDraft()->FindNodeIndexBySelector(TEXT("nodes[1]"));
    if (!TestTrue(TEXT("group is editable"), GroupIndex != INDEX_NONE)) return false;
    FTransform GroupTransform = BakeSession->GetDraft()->GetNodes()[GroupIndex].Transform;
    GroupTransform.AddToTranslation(FVector(0, 0, 25));
    bPassed &= TestTrue(TEXT("edit the group before baking"), BakeSession->SetNodeTransform(BakeSession->GetDraft()->GetNodeId(GroupIndex), GroupTransform, Error));
    // The copied definition must include the current draft, not the sealed placement.
    LeavesBefore.Reset();
    for (const FMHResolvedCompositeLeaf& Leaf : BakeSession->GetProjection()->GetPlan()->Leaves)
    {
        LeavesBefore.Add(FTransform(Leaf.WorldMatrix * P->GetActorTransform().ToMatrixWithScale()).GetLocation());
    }
    const FString Prefix = InvocationCopy.NodePath + TEXT(">") + RandomChild->LogicalName + TEXT(":");
    TArray<TPair<FString, FMatrix>> ExpectedLeaves;
    for (const FMHResolvedCompositeLeaf& Leaf : BakeSession->GetProjection()->GetPlan()->Leaves)
    {
        if (Leaf.Origin.StartsWith(Prefix) && Leaf.Kind == EMHRandomSemanticKind::Mesh) ExpectedLeaves.Emplace(Leaf.Resource, Leaf.WorldMatrix);
    }
    bPassed &= TestEqual(TEXT("two resolved leaves under the child"), ExpectedLeaves.Num(), 2);
    const FMatrix InvocationInverse = InvocationCopy.WorldMatrix.Inverse();

    // Procedural would re-roll the child's draws; the bake keeps them.
    FMHCompositeSaveUniquePlan ProceduralPlan, BakedPlan;
    bPassed &= TestTrue(TEXT("describe procedural: ") + Error, Subsystem->DescribeSaveUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, ProceduralPlan, Error));
    bPassed &= TestTrue(TEXT("procedural warns about the child's random draws"),
        ProceduralPlan.Warnings.Num() == 1 && ProceduralPlan.Warnings[0].Contains(RandomChild->LogicalName));
    bPassed &= TestTrue(TEXT("describe bake: ") + Error, Subsystem->DescribeSaveUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::BakeCurrentResult, BakedPlan, Error));
    bPassed &= TestTrue(TEXT("bake has nothing to re-roll"), BakedPlan.Warnings.IsEmpty());
    bPassed &= TestTrue(TEXT("bake copies the child and the root"),
        BakedPlan.Copies.Num() == 2 && BakedPlan.Copies[0] == RandomChild->LogicalName && BakedPlan.Copies[1] == RandomRoot->LogicalName);
    if (BakedPlan.Copies.Num() != 2) return false;

    Subsystem->SetDefinitionCreatorForTests(
        [&F](const FMHCompositeDocument& Document, const FMHCompositeAdoptTarget& Target, FString&) -> UMHCompositeAsset*
        {
            return F.Recipe.Composite(Target.LogicalName, Document, {});
        });
    TArray<FMHCompositeAdoptTarget> Targets;
    for (const FString& Copy : BakedPlan.Copies) Targets.AddDefaulted_GetRef().LogicalName = Copy + TEXT("_b");
    TArray<FString> Warnings;
    const bool bSaved = Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::BakeCurrentResult, Targets, Warnings, Error);
    Subsystem->SetDefinitionCreatorForTests({});
    bPassed &= TestTrue(TEXT("bake: ") + Error, bSaved);
    bPassed &= TestTrue(TEXT("no warnings for a bake under the root boundary"), Warnings.IsEmpty());
    bPassed &= TestFalse(TEXT("session ended"), Subsystem->IsEditingComposite());

    const UMHCompositeAsset* BakedChild = F.Recipe.Composites.FindRef(Targets[0].LogicalName);
    FMHCompositeDocument BakedDocument;
    if (!TestTrue(TEXT("baked child extracts"), BakedChild != nullptr && MHExtractCompositeV5(*BakedChild, BakedDocument, Error))) return false;
    bPassed &= TestEqual(TEXT("one concrete node per resolved leaf"), BakedDocument.Nodes.Num(), ExpectedLeaves.Num());
    for (int32 Index = 0; Index < BakedDocument.Nodes.Num() && Index < ExpectedLeaves.Num(); ++Index)
    {
        const FMHCompositeNode& Node = BakedDocument.Nodes[Index];
        bPassed &= TestTrue(*FString::Printf(TEXT("baked node %d is a plain mesh node"), Index),
            Node.Kind == EMHCompositeNodeKind::Mesh && Node.Options.IsEmpty() && Node.Children.IsEmpty() && !Node.bHasInlinePlacement && Node.Profile.IsEmpty());
        bPassed &= TestEqual(*FString::Printf(TEXT("baked node %d keeps the resolved resource"), Index), Node.Resource, ExpectedLeaves[Index].Key);
        const FVector ExpectedLocal = FTransform(ExpectedLeaves[Index].Value * InvocationInverse).GetLocation();
        bPassed &= TestTrue(*FString::Printf(TEXT("baked node %d sits where the leaf resolved, relative to the invocation"), Index),
            Node.Transform.TranslationCm.Equals(ExpectedLocal, 1e-2));
    }
    bPassed &= TestTrue(TEXT("this placement now invokes the baked root"), P->GetCompositeAsset() != nullptr && P->GetCompositeAsset()->LogicalName == Targets[1].LogicalName);
    bPassed &= TestEqual(TEXT("root-level streams stay keyed by the original root"), P->GetCallContext().StreamNamespace, RandomRoot->LogicalName);
    const TArray<FVector> LeavesAfter = AllLeafWorldLocations(*P);
    bPassed &= TestEqual(TEXT("the same number of leaves renders"), LeavesAfter.Num(), LeavesBefore.Num());
    for (const FVector& Before : LeavesBefore)
    {
        bPassed &= TestTrue(*FString::Printf(TEXT("leaf at %s renders where it did"), *Before.ToString()),
            LeavesAfter.ContainsByPredicate([&Before](const FVector& After) { return After.Equals(Before, 1e-2); }));
    }
    bPassed &= TestTrue(TEXT("the shared placements are untouched"), F.A->GetCompositeAsset() == F.Root && F.B->GetCompositeAsset() == F.Root);
    return bPassed;
}

// Escape discards the session; Enter stays available to editor widgets.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextSessionKeysTest,
    "Mimir.V5.Composite.EditContext.EscapeCancelsEnterPassesThrough",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextSessionKeysTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    bool bPassed = TestEqual(TEXT("Esc in a session cancels"), MHEditSessionKeyAction(EKeys::Escape, true), EMHEditSessionKeyAction::Cancel);
    bPassed &= TestEqual(TEXT("Enter in a session passes through"), MHEditSessionKeyAction(EKeys::Enter, true), EMHEditSessionKeyAction::None);
    bPassed &= TestEqual(TEXT("other keys are not session keys"), MHEditSessionKeyAction(EKeys::A, true), EMHEditSessionKeyAction::None);
    bPassed &= TestEqual(TEXT("without a session nothing is intercepted"), MHEditSessionKeyAction(EKeys::Escape, false), EMHEditSessionKeyAction::None);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("nested invocation"), InvocationNode)) return false;
    const FString InvocationPath = InvocationNode->NodePath;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child source bytes"), CanonicalBytes(*F.Child, ChildBefore))) return false;
    FVector MeshCBeforeA;
    bPassed &= TestTrue(TEXT("mesh C renders in A"), LeafWorldLocation(*F.A, F.MeshC, MeshCBeforeA));

    bPassed &= TestFalse(TEXT("no session: keys pass through"), MHHandleEditSessionKey(EKeys::Escape, false));

    // Esc: the draft is discarded, the preview comes back.
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationPath, Error))) return false;
    const TArray<TObjectPtr<USceneComponent>>& Handles = DraftHandles(*Subsystem);
    if (!TestEqual(TEXT("one handle"), Handles.Num(), 1) || !IsValid(Handles[0])) return false;
    bPassed &= TestTrue(TEXT("move node"), MoveDraftNodeToWorld(*Subsystem, Handles[0], Handles[0]->GetComponentLocation() + FVector(100, 0, 0), Error));
    bPassed &= TestTrue(TEXT("Esc is handled"), MHHandleEditSessionKey(EKeys::Escape, false));
    bPassed &= TestFalse(TEXT("Esc ended the session"), Subsystem->IsEditingComposite());
    bPassed &= TestFalse(TEXT("Esc left edit mode"), Subsystem->IsEditingComposite(F.A));
    FVector MeshCAfterEsc;
    bPassed &= TestTrue(TEXT("Esc restored the leaf"), LeafWorldLocation(*F.A, F.MeshC, MeshCAfterEsc) && MeshCAfterEsc.Equals(MeshCBeforeA, 1e-2));
    TArray<uint8> ChildAfterEsc;
    bPassed &= TestTrue(TEXT("Esc never touches the source"), CanonicalBytes(*F.Child, ChildAfterEsc) && ChildAfterEsc == ChildBefore);

    if (!TestTrue(TEXT("nested context again: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationPath, Error))) return false;
    bPassed &= TestFalse(TEXT("Enter passes through"), MHHandleEditSessionKey(EKeys::Enter, false));
    bPassed &= TestTrue(TEXT("Enter keeps the session open"), Subsystem->IsEditingComposite());
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

// A deferred Escape belongs to one session and cannot cancel a later one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditContextStaleCancelTest,
    "Mimir.V5.Composite.EditContext.QueuedCancelIgnoresLaterSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditContextStaleCancelTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FEditContextFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* InvocationNode = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("nested invocation"), InvocationNode)) return false;
    const FString InvocationPath = InvocationNode->NodePath;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child source bytes"), CanonicalBytes(*F.Child, ChildBefore))) return false;

    FString Error;
    bool bPassed = TestTrue(TEXT("session A: ") + Error, Subsystem->BeginEditNestedComposite(F.A, InvocationPath, Error));
    const uint32 EpochA = Subsystem->GetEditSessionEpoch();
    bPassed &= TestTrue(TEXT("cancel A"), Subsystem->CancelEditComposite(Error));
    const FMHResolvedCompositeNode* InvocationB = FEditContextFixture::Invocation(*F.B);
    if (!TestNotNull(TEXT("B invocation"), InvocationB)) return false;
    bPassed &= TestTrue(TEXT("session B: ") + Error, Subsystem->BeginEditNestedComposite(F.B, InvocationB->NodePath, Error));
    bPassed &= TestTrue(TEXT("a new session has a new epoch"), Subsystem->GetEditSessionEpoch() != EpochA);

    // The stale callback fires: nothing may happen to B.
    bPassed &= TestFalse(TEXT("stale Cancel is a no-op"), MHRunDeferredEditSessionCancel(EpochA));
    bPassed &= TestTrue(TEXT("B is still being edited"), Subsystem->IsEditingComposite() && Subsystem->IsEditingComposite(F.B));

    // The current epoch cancels B.
    bPassed &= TestTrue(TEXT("current Cancel runs"), MHRunDeferredEditSessionCancel(Subsystem->GetEditSessionEpoch()));
    bPassed &= TestFalse(TEXT("B session ended"), Subsystem->IsEditingComposite());
    Subsystem->SetCommitPublisherForTests({});
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("no external change from the stale callback"), CanonicalBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
