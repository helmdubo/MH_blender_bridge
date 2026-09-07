#include "MHRecipeTestFixture.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeSelectionAdapter.h"
#include "Composite/MHInstancePool.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Selection.h"

namespace UE::MimirComposite::Tests
{

// R5b-2b (KICKOFF §5 R5b-2): a hit on a pooled ISM instance resolves through
// the pool to the owner composite actor - never to the service pool actor -
// on the level editor's element selection set, without the Composite Outliner;
// the owner records which leaf was hit. Stock ISM instances keep the level
// editor's behaviour (instance -> owning component / actor).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPoolInstanceSelectionResolvesOwnerTest,
    "Mimir.V5.Composite.Selection.PoolInstanceResolvesToOwnerActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPoolInstanceSelectionResolvesOwnerTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FRecipeFixture Recipe(*this);
    const FString MeshName = Recipe.Name(TEXT("sel_mesh"));
    UStaticMesh* Mesh = Recipe.Mesh(MeshName);
    if (!TestNotNull(TEXT("mesh"), Mesh)) return false;
    FMHCompositeDocument Document;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FMHCompositeNode& Node = Document.Nodes.AddDefaulted_GetRef();
        Node.Kind = EMHCompositeNodeKind::Mesh;
        Node.Resource = MeshName;
        Node.Transform.TranslationCm = FVector(Index * 100.0, 0.0, 0.0);
    }
    UMHCompositeAsset* Root = Recipe.Composite(Recipe.Name(TEXT("sel_root")), Document, {});
    if (Root == nullptr) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    if (!TestNotNull(TEXT("world"), World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(true); };
    AMHCompositeActor* A = World->SpawnActor<AMHCompositeActor>();
    AMHCompositeActor* B = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), FTransform(FVector(0, 1000, 0)));
    if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B)) return false;
    for (AMHCompositeActor* Actor : {A, B})
    {
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(3);
        Actor->SetAppearanceSeed(5);
        Actor->SetCompositeAsset(Root);
    }
    // A stock ISM actor, outside the pool.
    AActor* Stock = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FVector(0, -1000, 0)));
    UInstancedStaticMeshComponent* StockISM = NewObject<UInstancedStaticMeshComponent>(Stock);
    StockISM->SetStaticMesh(Mesh);
    // A synthetic mesh has no body setup; the stock ISM must not create instance bodies.
    StockISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Stock->SetRootComponent(StockISM);
    StockISM->RegisterComponent();
    const int32 StockIndex = StockISM->AddInstance(FTransform::Identity, true);

    UTypedElementSelectionSet* Set = NewObject<UTypedElementSelectionSet>(GetTransientPackage());
    if (!TestNotNull(TEXT("selection set"), Set)) return false;
    bool bPassed = TestTrue(TEXT("adapter registers on a selection set"), MHRegisterPoolInstanceSelection(*Set));
    bPassed &= TestTrue(TEXT("registration is idempotent"), MHRegisterPoolInstanceSelection(*Set) && MHIsPoolInstanceSelectionRegistered(*Set));

    const auto Resolve = [&](const AMHCompositeActor& Actor, const int32 Row, const ETypedElementSelectionMethod Method, FTypedElementHandle& OutInstance) -> FTypedElementHandle
    {
        const TArray<FMHCompositeLeafMaterialization>& Rows = Actor.GetLeafMaterializations();
        if (!Rows.IsValidIndex(Row)) return FTypedElementHandle();
        UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Rows[Row].Component.Get());
        if (Bucket == nullptr || Rows[Row].InstanceIndex == INDEX_NONE) return FTypedElementHandle();
        OutInstance = UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(Bucket, Rows[Row].InstanceIndex);
        return OutInstance ? Set->GetSelectionElement(OutInstance, Method) : FTypedElementHandle();
    };
    FTypedElementHandle Instance;
    const FTypedElementHandle HandleA = UEngineElementsLibrary::AcquireEditorActorElementHandle(A);
    const FTypedElementHandle HandleB = UEngineElementsLibrary::AcquireEditorActorElementHandle(B);
    bPassed &= TestTrue(TEXT("A's first instance resolves to A"), Resolve(*A, 0, ETypedElementSelectionMethod::Primary, Instance) == HandleA);
    bPassed &= TestEqual(TEXT("A learned which leaf was hit"), A->GetSelectedPlacementLeafPath(), A->GetLeafMaterializations()[0].NodePath);
    bPassed &= TestTrue(TEXT("A's second instance resolves to A"), Resolve(*A, 1, ETypedElementSelectionMethod::Primary, Instance) == HandleA);
    bPassed &= TestEqual(TEXT("A's selected leaf follows the hit"), A->GetSelectedPlacementLeafPath(), A->GetLeafMaterializations()[1].NodePath);
    bPassed &= TestTrue(TEXT("B's instance on the shared bucket resolves to B"), Resolve(*B, 0, ETypedElementSelectionMethod::Primary, Instance) == HandleB);
    bPassed &= TestTrue(TEXT("a second click still resolves to the owner, never the instance"), Resolve(*A, 0, ETypedElementSelectionMethod::Secondary, Instance) == HandleA);
    // Both placements share one bucket; the pool actor is never the answer.
    UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(A->GetLeafMaterializations()[0].Component.Get());
    bPassed &= TestTrue(TEXT("both placements render in one pool bucket"), Bucket != nullptr && Bucket == B->GetLeafMaterializations()[0].Component.Get());
    if (Bucket != nullptr)
    {
        const FTypedElementHandle PoolActorHandle = UEngineElementsLibrary::AcquireEditorActorElementHandle(Bucket->GetOwner());
        bPassed &= TestTrue(TEXT("the pool actor is not the selection element"), Set->GetSelectionElement(Instance, ETypedElementSelectionMethod::Primary) != PoolActorHandle);
        // A stale index (beyond the live instances) selects nothing.
        const FTypedElementHandle Stale = UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(Bucket, Bucket->GetInstanceCount() + 5);
        bPassed &= TestFalse(TEXT("a stale pooled index resolves to nothing"), Stale && Set->GetSelectionElement(Stale, ETypedElementSelectionMethod::Primary).IsSet());
    }
    // Stock instance keeps the engine route: owning component (or its actor when the level editor's component customization is present).
    const FTypedElementHandle StockInstance = UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(StockISM, StockIndex);
    const FTypedElementHandle StockResolved = Set->GetSelectionElement(StockInstance, ETypedElementSelectionMethod::Primary);
    bPassed &= TestTrue(TEXT("stock instance resolves to its component or actor"),
        StockResolved == UEngineElementsLibrary::AcquireEditorComponentElementHandle(StockISM) || StockResolved == UEngineElementsLibrary::AcquireEditorActorElementHandle(Stock));
    bPassed &= TestTrue(TEXT("stock instance never resolves to a composite"), StockResolved != HandleA && StockResolved != HandleB);

    // Integration: the editor module registered the adapter on the level editor's set at startup.
    if (GEditor != nullptr)
    {
        if (UTypedElementSelectionSet* EditorSet = GEditor->GetSelectedActors()->GetElementSelectionSet())
        {
            bPassed &= TestTrue(TEXT("adapter is registered on the level editor's selection set"), MHIsPoolInstanceSelectionRegistered(*EditorSet));
            FTypedElementHandle EditorInstance;
            const TArray<FMHCompositeLeafMaterialization>& Rows = A->GetLeafMaterializations();
            UInstancedStaticMeshComponent* BucketA = Cast<UInstancedStaticMeshComponent>(Rows[0].Component.Get());
            if (BucketA != nullptr)
            {
                EditorInstance = UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(BucketA, Rows[0].InstanceIndex);
                bPassed &= TestTrue(TEXT("level editor set resolves a pooled instance to the owner"), EditorInstance && EditorSet->GetSelectionElement(EditorInstance, ETypedElementSelectionMethod::Primary) == HandleA);
            }
        }
        else
        {
            AddInfo(TEXT("no level editor selection set in this host; integration check skipped"));
        }
    }
    return bPassed;
}

// A viewport hit keeps the native composite actor as the selected element, but
// its visual selection is the nearest enclosing resolved composite occurrence.
// Repeated occurrences may share one pool bucket; changing the hit while the
// actor is already selected must retarget that per-instance highlight. A
// selected composite option is an occurrence at owner/options[index].
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHPoolLeafSelectsNearestCompositeOccurrenceTest,
    "Mimir.V5.Composite.Selection.PoolLeafSelectsNearestCompositeOccurrence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPoolLeafSelectsNearestCompositeOccurrenceTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    if (!TestNotNull(TEXT("editor"), GEditor)) return false;
    FMHCompositeEditBackendScope Backend(true);

    FRecipeFixture Recipe(*this);
    const FString MeshName = Recipe.Name(TEXT("occurrence_selection_mesh"));
    UStaticMesh* Mesh = Recipe.Mesh(MeshName);
    if (!TestNotNull(TEXT("shared occurrence mesh"), Mesh)) return false;

    FMHCompositeDocument ChildDocument;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FMHCompositeNode& Leaf = ChildDocument.Nodes.AddDefaulted_GetRef();
        Leaf.Kind = EMHCompositeNodeKind::Mesh;
        Leaf.Resource = MeshName;
        Leaf.Transform.TranslationCm = FVector(Index * 40.0, 0.0, 0.0);
    }
    UMHCompositeAsset* Child = Recipe.Composite(
        Recipe.Name(TEXT("occurrence_selection_child")), ChildDocument, {});
    if (!TestNotNull(TEXT("nested child"), Child)) return false;

    FMHCompositeDocument RootDocument;
    FMHCompositeNode& RootLeaf = RootDocument.Nodes.AddDefaulted_GetRef();
    RootLeaf.Kind = EMHCompositeNodeKind::Mesh;
    RootLeaf.Resource = MeshName;
    for (int32 Index = 0; Index < 2; ++Index)
    {
        FMHCompositeNode& Invocation = RootDocument.Nodes.AddDefaulted_GetRef();
        Invocation.Kind = EMHCompositeNodeKind::Composite;
        Invocation.Resource = Child->LogicalName;
        Invocation.Transform.TranslationCm = FVector(200.0 + Index * 300.0, 0.0, 0.0);
    }
    FMHCompositeNode& Random = RootDocument.Nodes.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Transform.TranslationCm = FVector(800.0, 0.0, 0.0);
    FMHCompositeOption& SelectedChild = Random.Options.AddDefaulted_GetRef();
    SelectedChild.Kind = EMHCompositeOptionKind::Composite;
    SelectedChild.Resource = Child->LogicalName;
    SelectedChild.Weight = 1.0f;
    UMHCompositeAsset* Root = Recipe.Composite(
        Recipe.Name(TEXT("occurrence_selection_root")), RootDocument, {});
    if (!TestNotNull(TEXT("occurrence root"), Root)) return false;

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    if (!TestNotNull(TEXT("occurrence selection world"), World)) return false;
    ON_SCOPE_EXIT
    {
        if (UMHCompositeLevelSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
            Subsystem != nullptr && Subsystem->IsEditingComposite())
        {
            FString Ignored;
            Subsystem->CancelEditComposite(Ignored);
        }
        GEditor->SelectNone(false, true, false);
        World->DestroyWorld(true);
    };
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    AMHCompositeActor* Other = World->SpawnActor<AMHCompositeActor>(
        AMHCompositeActor::StaticClass(), FTransform(FVector(0.0, 1000.0, 0.0)));
    if (!TestNotNull(TEXT("occurrence actor"), Actor) ||
        !TestNotNull(TEXT("other occurrence actor"), Other)) return false;
    for (AMHCompositeActor* Placement : {Actor, Other})
    {
        Placement->SetAutoSeed(false);
        Placement->SetAutoAppearanceSeed(false);
        Placement->SetSeed(3);
        Placement->SetAppearanceSeed(5);
        Placement->SetCompositeAsset(Root);
    }
    const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
    if (!TestNotNull(TEXT("resolved occurrence plan"), Plan)) return false;

    TArray<FString> ExplicitOccurrences;
    FString RandomOccurrence;
    for (const FMHResolvedCompositeNode& Node : Plan->Nodes)
    {
        if (Node.SemanticKind == EMHRandomSemanticKind::Composite &&
            Node.Resource == Child->LogicalName)
        {
            ExplicitOccurrences.Add(Node.NodePath);
        }
        else if (Node.SemanticKind == EMHRandomSemanticKind::Random &&
                 Node.SelectedOptionIndex == 0)
        {
            RandomOccurrence = Node.NodePath + TEXT("/options[0]");
        }
    }
    if (!TestEqual(TEXT("two explicit child occurrences"), ExplicitOccurrences.Num(), 2) ||
        !TestFalse(TEXT("selected composite option occurrence exists"), RandomOccurrence.IsEmpty())) return false;

    UTypedElementSelectionSet* Set = NewObject<UTypedElementSelectionSet>(GetTransientPackage());
    if (!TestNotNull(TEXT("occurrence selection set"), Set) ||
        !TestTrue(TEXT("occurrence adapter registers"), MHRegisterPoolInstanceSelection(*Set))) return false;
    const FTypedElementHandle OwnerHandle = UEngineElementsLibrary::AcquireEditorActorElementHandle(Actor);
    auto HitUnder = [&](const FString& Occurrence, const ETypedElementSelectionMethod Method) -> FString
    {
        const FMHCompositeLeafMaterialization* Row = Actor->GetLeafMaterializations().FindByPredicate(
            [&Occurrence](const FMHCompositeLeafMaterialization& Candidate)
            {
                return Candidate.NodePath.StartsWith(Occurrence + TEXT(">"));
            });
        if (Row == nullptr) return FString();
        UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row->Component.Get());
        if (Bucket == nullptr || Row->InstanceIndex == INDEX_NONE) return FString();
        const FTypedElementHandle Instance =
            UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(Bucket, Row->InstanceIndex);
        if (!Instance || Set->GetSelectionElement(Instance, Method) != OwnerHandle) return FString();
        return Row->NodePath;
    };
    const auto OnlyOccurrenceHighlighted = [&](const FString& Occurrence) -> bool
    {
        for (const FMHCompositeLeafMaterialization& Row : Actor->GetLeafMaterializations())
        {
            const UInstancedStaticMeshComponent* Bucket =
                Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
            if (Bucket == nullptr || Row.InstanceIndex == INDEX_NONE) return false;
            const bool bExpected = Row.NodePath.StartsWith(Occurrence + TEXT(">"));
            if (Bucket->IsInstanceSelected(Row.InstanceIndex) != bExpected) return false;
        }
        for (const FMHCompositeLeafMaterialization& Row : Other->GetLeafMaterializations())
        {
            const UInstancedStaticMeshComponent* Bucket =
                Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
            if (Bucket == nullptr || Row.InstanceIndex == INDEX_NONE ||
                Bucket->IsInstanceSelected(Row.InstanceIndex)) return false;
        }
        return true;
    };

    GEditor->SelectNone(false, true, false);
    const FString FirstHit = HitUnder(ExplicitOccurrences[0], ETypedElementSelectionMethod::Primary);
    bool bPassed = TestFalse(TEXT("first nested occurrence has a hittable pooled leaf"), FirstHit.IsEmpty());
    GEditor->SelectActor(Actor, true, true, true);
    bPassed &= TestEqual(TEXT("native selection remains the owner actor"), GEditor->GetSelectedActorCount(), 1);
    bPassed &= TestTrue(TEXT("owner actor is selected"), Actor->IsSelected());
    bPassed &= TestEqual(TEXT("exact hit path remains available for authored-node preselection"),
        Actor->GetSelectedPlacementLeafPath(), FirstHit);
    bPassed &= TestTrue(TEXT("only the first enclosing occurrence is highlighted"),
        OnlyOccurrenceHighlighted(ExplicitOccurrences[0]));

    // Secondary resolution models the right-click that opens the actor context
    // menu. It must retain the clicked leaf/occurrence while the owner is
    // already selected.
    const FString ContextHit = HitUnder(ExplicitOccurrences[0], ETypedElementSelectionMethod::Secondary);
    bPassed &= TestEqual(TEXT("right-click keeps the exact clicked leaf for Edit Contents"),
        Actor->GetSelectedPlacementLeafPath(), ContextHit);
    bPassed &= TestTrue(TEXT("right-click keeps the enclosing occurrence highlight"),
        OnlyOccurrenceHighlighted(ExplicitOccurrences[0]));

    const FString SecondHit = HitUnder(ExplicitOccurrences[1], ETypedElementSelectionMethod::Primary);
    bPassed &= TestFalse(TEXT("second occurrence has a hittable pooled leaf"), SecondHit.IsEmpty());
    bPassed &= TestEqual(TEXT("a hit retargets logical selection while actor stays selected"),
        Actor->GetSelectedPlacementLeafPath(), SecondHit);
    bPassed &= TestTrue(TEXT("highlight retargets to the second occurrence without actor reselection"),
        OnlyOccurrenceHighlighted(ExplicitOccurrences[1]));

    const FString OptionHit = HitUnder(RandomOccurrence, ETypedElementSelectionMethod::Primary);
    bPassed &= TestFalse(TEXT("selected composite option has a hittable descendant"), OptionHit.IsEmpty());
    bPassed &= TestEqual(TEXT("selected option keeps its exact clicked descendant"),
        Actor->GetSelectedPlacementLeafPath(), OptionHit);
    bPassed &= TestTrue(TEXT("selected composite option is highlighted as one occurrence"),
        OnlyOccurrenceHighlighted(RandomOccurrence));

    // Once selection leaves the actor, a later direct actor selection has no
    // stale leaf context and therefore keeps the established whole-placement
    // actor highlight contract.
    GEditor->SelectActor(Actor, false, true, true);
    bPassed &= TestTrue(TEXT("deselect clears stale logical leaf selection"),
        Actor->GetSelectedPlacementLeafPath().IsEmpty());
    GEditor->SelectActor(Actor, true, true, true);
    for (const FMHCompositeLeafMaterialization& Row : Actor->GetLeafMaterializations())
    {
        const UInstancedStaticMeshComponent* Bucket =
            Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
        bPassed &= TestTrue(TEXT("direct actor selection highlights the whole placement"),
            Bucket != nullptr && Row.InstanceIndex != INDEX_NONE &&
            Bucket->IsInstanceSelected(Row.InstanceIndex));
    }

    UMHCompositeLevelSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
    if (!TestNotNull(TEXT("edit subsystem"), Subsystem)) return false;
    FString EditError;
    bPassed &= TestFalse(TEXT("a stale captured leaf cannot open an edit session"),
        MHBeginEditPickedComposite(*Actor, FirstHit + TEXT("/stale"), EditError));
    bPassed &= TestTrue(TEXT("stale leaf refusal is diagnosed"),
        EditError.Contains(TEXT("MH_E_INVALID_RESOURCE_SOURCE")));
    bPassed &= TestNull(TEXT("stale leaf leaves no session"), Subsystem->GetEditSession());

    EditError.Reset();
    bPassed &= TestTrue(TEXT("Edit Contents opens the clicked explicit occurrence"),
        MHBeginEditPickedComposite(*Actor, FirstHit, EditError));
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    USceneComponent* PickedProjection = Projection != nullptr ? Projection->FindComponentForOrigin(FirstHit) : nullptr;
    bPassed &= TestNotNull(TEXT("explicit occurrence session"), Session);
    bPassed &= TestEqual(TEXT("explicit occurrence is the edit scope"),
        Session != nullptr ? Session->GetInvocationPath() : FString(), ExplicitOccurrences[0]);
    bPassed &= TestTrue(TEXT("clicked explicit leaf's authored owner is selected"),
        Session != nullptr && Projection != nullptr && PickedProjection != nullptr &&
        Session->GetSelectedNodeIds().Num() == 1 &&
        Session->GetActiveNodeId() == Projection->GetNodeIdForComponent(PickedProjection));
    FString CancelError;
    bPassed &= TestTrue(TEXT("explicit picked edit cancels"), Subsystem->CancelEditComposite(CancelError));

    EditError.Reset();
    const bool bOptionOpened = MHBeginEditPickedComposite(*Actor, OptionHit, EditError);
    bPassed &= TestTrue(TEXT("Edit Contents opens the selected composite option occurrence: ") + EditError,
        bOptionOpened);
    Session = Subsystem->GetEditSession();
    Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    PickedProjection = Projection != nullptr ? Projection->FindComponentForOrigin(OptionHit) : nullptr;
    bPassed &= TestEqual(TEXT("selected option is the edit scope"),
        Session != nullptr ? Session->GetInvocationPath() : FString(), RandomOccurrence);
    bPassed &= TestTrue(TEXT("clicked selected-option leaf's authored owner is selected"),
        Session != nullptr && Projection != nullptr && PickedProjection != nullptr &&
        Session->GetSelectedNodeIds().Num() == 1 &&
        Session->GetActiveNodeId() == Projection->GetNodeIdForComponent(PickedProjection));
    CancelError.Reset();
    bPassed &= TestTrue(TEXT("selected-option picked edit cancels"),
        Subsystem->CancelEditComposite(CancelError));

    // A successful rebuild may reuse the same pool bucket while replacing all
    // semantic paths. The stale occurrence must be pruned and the still
    // selected owner must return to whole-placement highlighting.
    bPassed &= TestTrue(TEXT("restore a scoped leaf before replacement"),
        Actor->SelectPlacementLeafByNodePath(OptionHit));
    UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(World);
    if (!TestNotNull(TEXT("selection pool"), Pool)) return false;
    Pool->SetOwnerSelected(*Actor, true);
    Actor->SetCompositeAsset(Child);
    bPassed &= TestTrue(TEXT("replacement prunes the obsolete clicked leaf"),
        Actor->GetSelectedPlacementLeafPath().IsEmpty() &&
        Actor->GetSelectedPlacementOccurrencePath().IsEmpty());
    for (const FMHCompositeLeafMaterialization& Row : Actor->GetLeafMaterializations())
    {
        const UInstancedStaticMeshComponent* Bucket =
            Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
        bPassed &= TestTrue(TEXT("replacement applies whole-placement highlight to reused slots"),
            Bucket != nullptr && Row.InstanceIndex != INDEX_NONE &&
            Bucket->IsInstanceSelected(Row.InstanceIndex));
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
