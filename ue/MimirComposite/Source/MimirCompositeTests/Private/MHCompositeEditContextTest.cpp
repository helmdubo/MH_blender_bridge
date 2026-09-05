#include "MHRecipeTestFixture.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeProtocol.h"
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
    const FMHResolvedCompositeNode* Invocation = FEditContextFixture::Invocation(*F.A);
    if (!TestNotNull(TEXT("root placement resolves the nested invocation"), Invocation)) return false;
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
    bPassed &= TestEqual(TEXT("opening the context does not rebuild the root"), F.A->GetPlacementRebuildCount(), RebuildsA);
    bPassed &= TestEqual(TEXT("opening the context does not advance the preview"), F.A->GetPreviewRevision(), RevisionA);
    bPassed &= TestFalse(TEXT("a second session is refused while one is active"), Subsystem->BeginEditComposite(F.B, Error));

    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("no session after cancel"), Subsystem->IsEditingComposite());
    bPassed &= TestTrue(TEXT("context is empty after cancel"), Subsystem->GetEditContext().EditedLogicalName.IsEmpty());
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("child source bytes after cancel"), CanonicalBytes(*F.Child, ChildAfter));
    bPassed &= TestTrue(TEXT("cancel leaves the shared definition untouched"), ChildAfter == ChildBefore);
    bPassed &= TestEqual(TEXT("cancel does not rebuild the root"), F.A->GetPlacementRebuildCount(), RebuildsA);
    bPassed &= TestNotNull(TEXT("root still previews"), F.A->GetResolvedPlan());
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
