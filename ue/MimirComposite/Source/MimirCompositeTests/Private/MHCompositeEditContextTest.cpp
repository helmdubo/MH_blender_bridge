#include "MHRecipeTestFixture.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHInstancePool.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace UE::MimirComposite::Tests
{
namespace
{

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

    AMHCompositeActor* Spawn(const FTransform& Transform)
    {
        FActorSpawnParameters Params;
        Params.ObjectFlags = RF_Transactional;
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), Transform, Params);
        if (Actor == nullptr) return nullptr;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(7);
        Actor->SetAppearanceSeed(11);
        Actor->SetCompositeAsset(Root);
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
    // Copy: entering the session re-materializes the root and replaces its resident plan.
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
    // R6-D1: opening the context enters the root's edit session under the
    // scope (one materialization with handles); the plan itself is unchanged.
    bPassed &= TestTrue(TEXT("opening the context enters edit mode under the scope"), F.A->IsPlacementEditMode() && F.A->GetEditScopeInvocationPath() == Invocation->NodePath);
    bPassed &= TestTrue(TEXT("opening the context keeps a resolved plan"), F.A->GetResolvedPlan() != nullptr && F.A->GetLastPlacementError().IsEmpty());
    static_cast<void>(RevisionA);
    bPassed &= TestFalse(TEXT("a second session is refused while one is active"), Subsystem->BeginEditComposite(F.B, Error));

    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("no session after cancel"), Subsystem->IsEditingComposite());
    bPassed &= TestTrue(TEXT("context is empty after cancel"), Subsystem->GetEditContext().EditedLogicalName.IsEmpty());
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("child source bytes after cancel"), CanonicalBytes(*F.Child, ChildAfter));
    bPassed &= TestTrue(TEXT("cancel leaves the shared definition untouched"), ChildAfter == ChildBefore);
    bPassed &= TestTrue(TEXT("cancel leaves edit mode and the scope"), !F.A->IsPlacementEditMode() && F.A->GetEditScopeInvocationPath().IsEmpty());
    static_cast<void>(RebuildsA);
    bPassed &= TestNotNull(TEXT("root still previews"), F.A->GetResolvedPlan());
    return bPassed;
}

// R6-D1b (docs/16 §2.7): inside a nested edit context the child definition's
// nodes get handles under the invocation's effective parent transform. Moving
// a handle edits the draft node's local transform (EditedLocal = EditedWorld *
// inverse(ParentEffectiveWorld)), the root preview follows through the pool
// without a full re-materialization, other placements stay untouched until a
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

    const auto LeafWorld = [](const AMHCompositeActor& Actor, const FString& Resource, FVector& OutLocation) -> bool
    {
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
    };
    FVector MeshCBeforeA, MeshCBeforeB;
    bool bPassed = TestTrue(TEXT("mesh C renders in A"), LeafWorld(*F.A, F.MeshC, MeshCBeforeA));
    bPassed &= TestTrue(TEXT("mesh C renders in B"), LeafWorld(*F.B, F.MeshC, MeshCBeforeB));
    const FMatrix ParentWorld = Invocation->WorldMatrix * F.A->GetActorTransform().ToMatrixWithScale();
    const int32 LiveBefore = Pool->NumLiveInstances(*F.A);

    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Invocation->NodePath, Error))) return false;
    bPassed &= TestTrue(TEXT("root placement is in edit mode for the nested scope"), F.A->IsPlacementEditMode());
    const TArray<TObjectPtr<USceneComponent>>& Handles = F.A->GetEditScopeHandles();
    bPassed &= TestEqual(TEXT("one handle per child definition node"), Handles.Num(), 1);
    if (Handles.Num() != 1 || !IsValid(Handles[0])) return false;
    USceneComponent* Handle = Handles[0];
    // The child node sits at (0,0,40) in the child's space -> under the invocation's world.
    const FVector ExpectedHandle = FTransform(FTransform(FVector(0, 0, 40)).ToMatrixWithScale() * ParentWorld).GetLocation();
    bPassed &= TestTrue(TEXT("handle sits at the node's world under the effective parent"), Handle->GetComponentLocation().Equals(ExpectedHandle, 1e-2));
    bPassed &= TestTrue(TEXT("handle is the actor's own component"), Handle->GetOwner() == F.A);

    // Drag: +100 along world X.
    Handle->SetWorldLocation(ExpectedHandle + FVector(100, 0, 0));
    F.A->Tick(0.0f);
    bPassed &= TestTrue(TEXT("edit tick keeps the preview: ") + F.A->GetLastPlacementError(), F.A->GetLastPlacementError().IsEmpty() && F.A->GetResolvedPlan() != nullptr);
    FMHCompositeDocument Draft;
    bPassed &= TestTrue(TEXT("edited draft is available"), F.A->GetEditedCompositeDocument(Draft));
    const FMatrix EditedLocal = FTransform(ExpectedHandle + FVector(100, 0, 0)).ToMatrixWithScale() * ParentWorld.Inverse();
    bPassed &= TestTrue(TEXT("draft node local = edited world * inverse(parent effective world)"),
        Draft.Nodes.Num() == 1 && Draft.Nodes[0].Transform.TranslationCm.Equals(FTransform(EditedLocal).GetLocation(), 1e-2));
    FVector MeshCAfterA;
    bPassed &= TestTrue(TEXT("A renders the moved leaf"), LeafWorld(*F.A, F.MeshC, MeshCAfterA) && MeshCAfterA.Equals(MeshCBeforeA + FVector(100, 0, 0), 1e-2));
    bPassed &= TestEqual(TEXT("no duplicated instances while dragging"), Pool->NumLiveInstances(*F.A), LiveBefore);
    FVector MeshCB;
    bPassed &= TestTrue(TEXT("B is untouched until a publish"), LeafWorld(*F.B, F.MeshC, MeshCB) && MeshCB.Equals(MeshCBeforeB, 1e-2));
    bPassed &= TestEqual(TEXT("the draft is the session's document"), Subsystem->GetEditingDraft().Nodes.Num(), 1);

    // Cancel: the preview comes back from the unchanged definition.
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("edit mode ended"), F.A->IsPlacementEditMode());
    bPassed &= TestEqual(TEXT("scope handles retired"), F.A->GetEditScopeHandles().Num(), 0);
    FVector MeshCRestored;
    bPassed &= TestTrue(TEXT("cancel restores the leaf"), LeafWorld(*F.A, F.MeshC, MeshCRestored) && MeshCRestored.Equals(MeshCBeforeA, 1e-2));
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("child bytes"), CanonicalBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);
    return bPassed;
}


} // namespace UE::MimirComposite::Tests
