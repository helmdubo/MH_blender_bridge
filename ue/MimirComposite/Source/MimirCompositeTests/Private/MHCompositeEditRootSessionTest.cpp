#include "MHCompositeEditFixture.h"

#include "Components/StaticMeshComponent.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Settings/MHCompositeSettings.h"
#include "UI/MHSourceOverwritePolicy.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FRootV2Scope
{
    bool bPrevious = false;
    FRootV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FRootV2Scope()
    {
        GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious;
        MHSetSourceOverwritePolicyTestHooks(FMHSourceOverwritePolicyTestHooks());
    }
};

TArray<FVector> RootProjectedWorlds(const UMHCompositeEditProjection& Projection)
{
    TArray<FVector> Result;
    for (const TObjectPtr<USceneComponent>& Component : Projection.GetComponents())
    {
        const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Component.Get());
        if (IsValid(Mesh) && Mesh->GetStaticMesh() != nullptr) Result.Add(Mesh->GetComponentLocation());
    }
    return Result;
}

} // namespace

// CE-3c (spec §5.4, A04): under the CE backend the root definition is edited
// the same way as a nested one — one session, the whole placement projected,
// nested references atomic (their geometry maps to the reference node, no
// handles inside), every instance of this placement suppressed, the other
// placement and the foreign ISM untouched; Cancel restores the view.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditRootSessionTest,
    "Mimir.V5.Composite.EditMode.Root.SessionProjectsTheWholePlacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditRootSessionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FRootV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    // Every row of a placement starts with its root definition prefix.
    const FString Prefix = F.Root->LogicalName + TEXT(":");
    const TArray<FVector> ABefore = FCompositeEditFixture::LeafWorldsUnder(*F.A, Prefix);
    const TArray<FVector> BBefore = FCompositeEditFixture::LeafWorldsUnder(*F.B, Prefix);
    if (!TestTrue(TEXT("the placement has pooled leaves"), ABefore.Num() > 0)) return false;
    const TArray<FVector> ForeignBefore = F.ForeignWorlds();
    FString Error;
    if (!TestTrue(TEXT("root session: ") + Error, Subsystem->BeginEditComposite(F.A, Error))) return false;
    bool bPassed = TestFalse(TEXT("the root never enters the legacy edit mode"), F.A->IsPlacementEditMode());
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("session"), Session) || !TestNotNull(TEXT("projection"), Projection) || !TestNotNull(TEXT("draft"), Draft)) return false;
    bPassed &= TestFalse(TEXT("a root session is not nested"), Session->IsNested());
    bPassed &= TestTrue(TEXT("the root definition is edited"), Session->GetEditedAsset() == F.Root);
    bPassed &= TestTrue(TEXT("the mode follows the root session"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestTrue(TEXT("projection actor"), Projection->GetProjectionActor() != nullptr);

    USceneComponent* Own = Projection->FindComponentForOrigin(Prefix + TEXT("nodes[0]"));
    USceneComponent* First = Projection->FindComponentForOrigin(Prefix + TEXT("nodes[1]"));
    USceneComponent* NestedLeaf = Projection->FindComponentForOrigin(Prefix + TEXT("nodes[1]>") + F.Child->LogicalName + TEXT(":nodes[0]"));
    USceneComponent* NestedGroup = Projection->FindComponentForOrigin(Prefix + TEXT("nodes[1]>") + F.Child->LogicalName + TEXT(":nodes[1]"));
    bPassed &= TestTrue(TEXT("the root's own leaf is a mesh component"), Own != nullptr && Own->IsA<UStaticMeshComponent>());
    bPassed &= TestTrue(TEXT("a nested reference has a handle"), First != nullptr && !First->IsA<UStaticMeshComponent>());
    bPassed &= TestTrue(TEXT("nested geometry is projected"), NestedLeaf != nullptr && NestedLeaf->IsA<UStaticMeshComponent>());
    bPassed &= TestNull(TEXT("a nested reference is atomic: no handle inside it"), NestedGroup);
    bPassed &= TestTrue(TEXT("nested geometry maps to the reference node (A04)"), NestedLeaf != nullptr && Projection->GetNodeIdForComponent(NestedLeaf) == Draft->GetNodeId(1));
    bPassed &= TestTrue(TEXT("own leaf -> node 0"), Own != nullptr && Projection->GetNodeIdForComponent(Own) == Draft->GetNodeId(0));

    bPassed &= TestTrue(TEXT("one mesh component per pooled leaf of the placement, at the same world position"), FCompositeEditFixture::SameLocations(ABefore, RootProjectedWorlds(*Projection)));
    bPassed &= TestTrue(TEXT("every instance of this placement is suppressed"), Projection->GetLease().IsSet() && Projection->GetLease().Handles.Num() == ABefore.Num());
    bPassed &= TestTrue(TEXT("the other placement keeps its view"), FCompositeEditFixture::SameLocations(BBefore, FCompositeEditFixture::LeafWorldsUnder(*F.B, Prefix)));
    bPassed &= TestTrue(TEXT("the foreign ISM keeps its view"), FCompositeEditFixture::SameLocations(ForeignBefore, F.ForeignWorlds()));

    // A local edit moves the projection only.
    if (Own == nullptr) return false;
    const FVector OwnBefore = Own->GetComponentLocation();
    bPassed &= TestTrue(TEXT("edit"), Session->SetNodeTransform(Draft->GetNodeId(0), FTransform(FVector(100.0, 0.0, 0.0)), Error));
    USceneComponent* OwnAfter = Projection->FindComponentForOrigin(Prefix + TEXT("nodes[0]"));
    bPassed &= TestTrue(TEXT("the projection follows the draft"), OwnAfter != nullptr && OwnAfter->GetComponentLocation().Equals(OwnBefore + FVector(100.0, 0.0, 0.0), 1e-2));
    bPassed &= TestTrue(TEXT("dirty"), Session->IsDirty());

    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    bPassed &= TestFalse(TEXT("mode gone"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestTrue(TEXT("the placement renders where it did"), FCompositeEditFixture::SameLocations(ABefore, FCompositeEditFixture::LeafWorldsUnder(*F.A, Prefix)));
    bPassed &= TestFalse(TEXT("still no legacy edit mode"), F.A->IsPlacementEditMode());
    return bPassed;
}

// CE-3c: Save on a root session publishes the root definition from the draft.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditRootSaveTest,
    "Mimir.V5.Composite.EditMode.Root.SaveAppliesTheDraft",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditRootSaveTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FRootV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    FString Error;
    if (!TestTrue(TEXT("root session: ") + Error, Subsystem->BeginEditComposite(F.A, Error))) return false;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    if (!TestNotNull(TEXT("mode"), Mode) || !TestNotNull(TEXT("session"), Session) || Session->GetDraft() == nullptr) return false;
    bool bPassed = TestTrue(TEXT("edit"), Session->SetNodeTransform(Session->GetDraft()->GetNodeId(0), FTransform(FVector(100.0, 0.0, 0.0)), Error));
    int32 Confirmations = 0;
    FMHSourceOverwritePolicyTestHooks Hooks;
    Hooks.Confirm = [&Confirmations](const FText&) { ++Confirmations; return false; };
    Hooks.Notify = [](const FText&) {};
    Hooks.MessageLog = [](const FText&) {};
    MHSetSourceOverwritePolicyTestHooks(Hooks);
    UMHCompositeAsset* Published = nullptr;
    Subsystem->SetCommitPublisherForTests([&Published](UMHCompositeAsset& Asset, FString&) { Published = &Asset; MHNotifyCompositeAssetChanged(Asset); return true; });
    Mode->RequestSave();
    Subsystem->SetCommitPublisherForTests({});
    bPassed &= TestEqual(TEXT("explicit root Save needs no confirmation"), Confirmations, 0);
    bPassed &= TestTrue(TEXT("the root definition was published"), Published == F.Root);
    bPassed &= TestFalse(TEXT("session gone"), Subsystem->IsEditingComposite());
    bPassed &= TestFalse(TEXT("mode gone"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestFalse(TEXT("no legacy edit mode"), F.A->IsPlacementEditMode());
    FMHCompositeDocument RootDocument;
    bPassed &= TestTrue(TEXT("the root carries the edit"), MHExtractCompositeV5(*F.Root, RootDocument, Error) && RootDocument.Nodes.Num() == 3 && RootDocument.Nodes[0].Transform.TranslationCm.Equals(FVector(100.0, 0.0, 0.0), 1e-2));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
