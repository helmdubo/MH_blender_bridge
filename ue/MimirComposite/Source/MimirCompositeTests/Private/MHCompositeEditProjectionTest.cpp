#include "MHCompositeEditFixture.h"

#include "Components/StaticMeshComponent.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/** Runs the test under the CE backend (spec: the flag picks one backend wholesale). */
struct FEditModeV2Scope
{
    bool bPrevious = false;
    FEditModeV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FEditModeV2Scope() { GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious; }
};

TArray<FVector> MeshComponentWorlds(const UMHCompositeEditProjection& Projection)
{
    TArray<FVector> Result;
    for (const TObjectPtr<USceneComponent>& Component : Projection.GetComponents())
    {
        const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component.Get());
        if (IsValid(Mesh) && Mesh->GetStaticMesh() != nullptr) Result.Add(Mesh->GetComponentLocation());
    }
    return Result;
}

USceneComponent* ComponentAtOrigin(const UMHCompositeEditProjection& Projection, const FString& Origin)
{
    for (const TObjectPtr<USceneComponent>& Component : Projection.GetComponents())
    {
        if (IsValid(Component) && Projection.GetOriginForComponent(Component) == Origin) return Component;
    }
    return nullptr;
}

} // namespace

// CE-2b (spec §5.4–6.1, CE-ADR-4): under the CE backend a nested session shows
// the selected occurrence as components of one transient projection actor,
// suppresses that occurrence's pooled instances, and previews the draft
// locally — the other invocation, the other placement and foreign ISMs keep
// the published view. The root placement never enters the legacy edit mode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditProjectionOccurrenceTest,
    "Mimir.V5.Composite.EditMode.Projection.ShowsTheOccurrenceAndSuppressesItsInstances",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditProjectionOccurrenceTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FEditModeV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* First = FCompositeEditFixture::Invocation(*F.A, 0);
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("first"), First) || !TestNotNull(TEXT("second"), Second)) return false;
    const FString FirstPrefix = First->NodePath + TEXT(">");
    const FString SecondPath = Second->NodePath;
    const FString SecondPrefix = SecondPath + TEXT(">");
    const TArray<FVector> FirstBefore = FCompositeEditFixture::LeafWorldsUnder(*F.A, FirstPrefix);
    const TArray<FVector> SecondBefore = FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix);
    const TArray<FVector> BBefore = FCompositeEditFixture::LeafWorldsUnder(*F.B, F.Root->LogicalName);
    const TArray<FVector> ForeignBefore = F.ForeignWorlds();
    const auto OthersUntouched = [&]()
    {
        return FCompositeEditFixture::SameLocations(FirstBefore, FCompositeEditFixture::LeafWorldsUnder(*F.A, FirstPrefix)) &&
            FCompositeEditFixture::SameLocations(BBefore, FCompositeEditFixture::LeafWorldsUnder(*F.B, F.Root->LogicalName)) &&
            FCompositeEditFixture::SameLocations(ForeignBefore, F.ForeignWorlds());
    };

    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    if (!TestNotNull(TEXT("session"), Session) || !TestNotNull(TEXT("projection"), Projection)) return false;
    bool bPassed = TestTrue(TEXT("projection is open"), Projection->IsOpen());
    bPassed &= TestFalse(TEXT("the root never enters the legacy edit mode"), F.A->IsPlacementEditMode());
    bPassed &= TestEqual(TEXT("no legacy scope handles"), F.A->GetEditScopeHandles().Num(), 0);
    const AMHCompositeEditProjectionActor* Actor = Projection->GetProjectionActor();
    if (!TestNotNull(TEXT("projection actor"), Actor)) return false;
    bPassed &= TestTrue(TEXT("projection actor is transient and editor-only, in the root's level"),
        Actor->HasAnyFlags(RF_Transient) && Actor->IsEditorOnly() && Actor->GetLevel() == F.A->GetLevel() && Actor->IsHidden());

    // The occurrence renders through the projection, not through the pool.
    const TArray<FVector> ProjectedMeshes = MeshComponentWorlds(*Projection);
    bPassed &= TestTrue(TEXT("one mesh component per pooled leaf, at the same world position"), FCompositeEditFixture::SameLocations(SecondBefore, ProjectedMeshes));
    bPassed &= TestEqual(TEXT("the occurrence's pooled instances are suppressed"), FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix).Num(), 0);
    bPassed &= TestTrue(TEXT("the lease names exactly those instances"), Projection->GetLease().IsSet() && Projection->GetLease().Handles.Num() == SecondBefore.Num());
    bPassed &= TestTrue(TEXT("the other invocation, the other placement and the foreign ISM keep the published view"), OthersUntouched());

    // A draft command moves the projection only (local preview, shared save).
    UMHCompositeEditDocument* Draft = Session->GetDraft();
    if (!TestNotNull(TEXT("draft"), Draft)) return false;
    const FGuid PlainId = Draft->GetNodeId(0);
    USceneComponent* Plain = Projection->FindComponentForNodeId(PlainId);
    if (!TestNotNull(TEXT("plain mesh component"), Plain)) return false;
    const FVector PlainBefore = Plain->GetComponentLocation();
    bPassed &= TestTrue(TEXT("command: ") + Error, Session->SetNodeTransform(PlainId, FTransform(FVector(100.0, 0.0, 40.0)), Error));
    Plain = Projection->FindComponentForNodeId(PlainId);
    bPassed &= TestTrue(TEXT("the projection follows the draft (+100 X under an unrotated invocation)"), Plain != nullptr && Plain->GetComponentLocation().Equals(PlainBefore + FVector(100.0, 0.0, 0.0), 1e-2));
    bPassed &= TestTrue(TEXT("the other invocation of the same definition does not follow the draft"), OthersUntouched());
    bPassed &= TestEqual(TEXT("still suppressed while editing"), FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix).Num(), 0);
    bPassed &= TestTrue(TEXT("dirty"), Session->IsDirty());

    // Cancel: projection gone, the published occurrence is back where it was.
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("projection closed"), Projection->IsOpen());
    bPassed &= TestNull(TEXT("projection actor destroyed"), Projection->GetProjectionActor());
    bPassed &= TestTrue(TEXT("the occurrence renders where it did"), FCompositeEditFixture::SameLocations(SecondBefore, FCompositeEditFixture::LeafWorldsUnder(*F.A, SecondPrefix)));
    bPassed &= TestTrue(TEXT("others still untouched"), OthersUntouched());
    return bPassed;
}

// CE-2b: every projection component resolves to the session node it stands
// for — a leaf to its node, a leaf picked by a random node to that random
// node, a group handle to the group. The random node always has a handle;
// its picked leaf exists only when the pick (fixture names carry a per-run
// suffix, so the pick varies between runs) lands on the mesh option.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditProjectionMappingTest,
    "Mimir.V5.Composite.EditMode.Projection.ComponentsMapToDraftNodes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditProjectionMappingTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FEditModeV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    const FString SecondPath = Second->NodePath;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("projection"), Projection) || !TestNotNull(TEXT("draft"), Draft)) return false;
    const FString Prefix = SecondPath + TEXT(">") + F.Child->LogicalName + TEXT(":");
    USceneComponent* Plain = ComponentAtOrigin(*Projection, Prefix + TEXT("nodes[0]"));
    USceneComponent* Group = ComponentAtOrigin(*Projection, Prefix + TEXT("nodes[1]"));
    USceneComponent* Grouped = ComponentAtOrigin(*Projection, Prefix + TEXT("nodes[1]/children[0]"));
    USceneComponent* Random = ComponentAtOrigin(*Projection, Prefix + TEXT("nodes[2]"));
    USceneComponent* Picked = ComponentAtOrigin(*Projection, Prefix + TEXT("nodes[2]/options[0]"));
    bool bPassed = TestTrue(TEXT("plain leaf is a mesh component"), Plain != nullptr && Plain->IsA<UStaticMeshComponent>());
    bPassed &= TestTrue(TEXT("group is a handle"), Group != nullptr && !Group->IsA<UStaticMeshComponent>());
    bPassed &= TestTrue(TEXT("grouped leaf is a mesh component"), Grouped != nullptr && Grouped->IsA<UStaticMeshComponent>());
    bPassed &= TestTrue(TEXT("the random node has a handle"), Random != nullptr && !Random->IsA<UStaticMeshComponent>());
    bPassed &= TestTrue(TEXT("a picked mesh option is a mesh component"), Picked == nullptr || Picked->IsA<UStaticMeshComponent>());
    bPassed &= TestTrue(TEXT("plain -> node 0"), Projection->GetNodeIdForComponent(Plain) == Draft->GetNodeId(0));
    bPassed &= TestTrue(TEXT("group -> node 1"), Projection->GetNodeIdForComponent(Group) == Draft->GetNodeId(1));
    bPassed &= TestTrue(TEXT("grouped -> node 2"), Projection->GetNodeIdForComponent(Grouped) == Draft->GetNodeId(2));
    bPassed &= TestTrue(TEXT("random handle -> node 3"), Projection->GetNodeIdForComponent(Random) == Draft->GetNodeId(3));
    bPassed &= TestTrue(TEXT("picked leaf -> node 3"), Picked == nullptr || Projection->GetNodeIdForComponent(Picked) == Draft->GetNodeId(3));
    bPassed &= TestTrue(TEXT("node 0 -> plain"), Projection->FindComponentForNodeId(Draft->GetNodeId(0)) == Plain);
    bPassed &= TestTrue(TEXT("a foreign component maps to nothing"), !Projection->GetNodeIdForComponent(F.A->GetRootComponent()).IsValid());
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
