#include "MHRecipeTestFixture.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeSelectionAdapter.h"
#include "Composite/MHInstancePool.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
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

} // namespace UE::MimirComposite::Tests
