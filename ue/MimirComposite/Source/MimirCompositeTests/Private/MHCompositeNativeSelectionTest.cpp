#include "MHRecipeTestFixture.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeSelectionAdapter.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Selection.h"
#include "Toolkits/BaseToolkit.h"

namespace UE::MimirComposite::Tests
{

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeNativeSelectionCycleTest,
    "Mimir.V5.Composite.Selection.NativePrimarySecondaryCycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeNativeSelectionCycleTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    if (!TestNotNull(TEXT("editor"), GEditor)) return false;
    FMHCompositeEditBackendScope Backend(true);

    FRecipeFixture Recipe(*this);
    const FString MeshName = Recipe.Name(TEXT("native_selection_mesh"));
    UStaticMesh* Mesh = Recipe.Mesh(MeshName);
    if (!TestNotNull(TEXT("mesh"), Mesh)) return false;

    FMHCompositeDocument ChildDocument;
    FMHCompositeNode& ChildLeaf = ChildDocument.Nodes.AddDefaulted_GetRef();
    ChildLeaf.Kind = EMHCompositeNodeKind::Mesh;
    ChildLeaf.Resource = MeshName;
    FMHCompositeNode SecondLeaf = ChildLeaf;
    SecondLeaf.Transform.TranslationCm = FVector(40.0, 0.0, 0.0);
    ChildDocument.Nodes.Add(SecondLeaf);
    UMHCompositeAsset* Child = Recipe.Composite(Recipe.Name(TEXT("native_selection_child")), ChildDocument, {});
    if (!TestNotNull(TEXT("child"), Child)) return false;

    FMHCompositeDocument RootDocument;
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
    FMHCompositeOption& Option = Random.Options.AddDefaulted_GetRef();
    Option.Kind = EMHCompositeOptionKind::Composite;
    Option.Resource = Child->LogicalName;
    Option.Weight = 1.0f;
    UMHCompositeAsset* Root = Recipe.Composite(Recipe.Name(TEXT("native_selection_root")), RootDocument, {});
    if (!TestNotNull(TEXT("root"), Root)) return false;

    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
    if (!TestNotNull(TEXT("world"), World)) return false;
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
    AMHCompositeActor* Other = World->SpawnActor<AMHCompositeActor>(AMHCompositeActor::StaticClass(), FTransform(FVector(0.0, 1000.0, 0.0)));
    if (!TestNotNull(TEXT("actor"), Actor) || !TestNotNull(TEXT("other"), Other)) return false;
    for (AMHCompositeActor* Placement : {Actor, Other})
    {
        Placement->SetAutoSeed(false);
        Placement->SetAutoAppearanceSeed(false);
        Placement->SetSeed(3);
        Placement->SetAppearanceSeed(5);
        Placement->SetCompositeAsset(Root);
    }

    UTypedElementSelectionSet* Set = GEditor->GetSelectedActors()->GetElementSelectionSet();
    if (!TestNotNull(TEXT("native editor selection set"), Set)) return false;
    if (!TestTrue(TEXT("pool adapter registered"), MHRegisterPoolInstanceSelection(*Set))) return false;
    const FTypedElementSelectionOptions SelectionOptions;
    const FTypedElementHandle OwnerHandle = UEngineElementsLibrary::AcquireEditorActorElementHandle(Actor);

    const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
    if (!TestNotNull(TEXT("plan"), Plan)) return false;
    TArray<FString> Occurrences;
    for (const FMHResolvedCompositeNode& Node : Plan->Nodes)
    {
        if (Node.SemanticKind == EMHRandomSemanticKind::Composite && Node.Resource == Child->LogicalName)
            Occurrences.Add(Node.NodePath);
    }
    if (!TestEqual(TEXT("two explicit occurrences"), Occurrences.Num(), 2)) return false;

    auto RowFor = [&](const FString& Prefix) -> const FMHCompositeLeafMaterialization*
    {
        return Actor->GetLeafMaterializations().FindByPredicate([&Prefix](const FMHCompositeLeafMaterialization& Row)
        {
            return Row.NodePath.StartsWith(Prefix + TEXT(">"));
        });
    };
    auto HighlightedOnly = [&](const FString& Prefix) -> bool
    {
        for (const FMHCompositeLeafMaterialization& Row : Actor->GetLeafMaterializations())
        {
            const UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
            if (Bucket == nullptr || Row.InstanceIndex == INDEX_NONE) return false;
            const bool bExpected = Prefix.IsEmpty() || Row.NodePath.StartsWith(Prefix + TEXT(">"));
            if (Bucket->IsInstanceSelected(Row.InstanceIndex) != bExpected) return false;
        }
        for (const FMHCompositeLeafMaterialization& Row : Other->GetLeafMaterializations())
        {
            const UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row.Component.Get());
            if (Bucket == nullptr || Bucket->IsInstanceSelected(Row.InstanceIndex)) return false;
        }
        return true;
    };
    auto NativeHit = [&](const FString& Prefix, const ETypedElementSelectionMethod Method, const bool bClear) -> bool
    {
        const FMHCompositeLeafMaterialization* Row = RowFor(Prefix);
        if (Row == nullptr) return false;
        UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row->Component.Get());
        if (Bucket == nullptr) return false;
        const FTypedElementHandle Raw = UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(Bucket, Row->InstanceIndex);
        const FTypedElementHandle Resolved = Set->GetSelectionElement(Raw, Method);
        if (!Resolved || Resolved != OwnerHandle) return false;
        if (bClear) Set->ClearSelection(SelectionOptions);
        Set->SelectElement(Resolved, SelectionOptions);
        Set->NotifyPendingChanges();
        // RMB opens the menu after notifying, even when the selection did not change.
        if (!bClear) MHSelectCompositeContextHit(Resolved, *Actor);
        return true;
    };

    GEditor->SelectNone(false, true, false);
    Set->ClearSelection(SelectionOptions);
    Set->NotifyPendingChanges();
    bool bPassed = NativeHit(Occurrences[0], ETypedElementSelectionMethod::Primary, true);
    bPassed &= TestTrue(TEXT("primary hit selects owner"), Actor->IsSelected());
    bPassed &= TestTrue(TEXT("primary hit starts at root context"), Actor->GetSelectedPlacementLeafPath().IsEmpty());
    bPassed &= TestTrue(TEXT("primary hit highlights whole owner"), HighlightedOnly(FString()));
    bPassed &= NativeHit(Occurrences[0], ETypedElementSelectionMethod::Primary, false);
    bPassed &= TestTrue(TEXT("RMB from root preserves root context"), Actor->GetSelectedPlacementLeafPath().IsEmpty());

    UMHCompositeLevelSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
    if (!TestNotNull(TEXT("edit subsystem"), Subsystem)) return false;
    FString Error;
    bPassed &= TestTrue(TEXT("root RMB Edit opens root"), Subsystem->BeginEditComposite(Actor, Error));
    bPassed &= TestTrue(TEXT("root Edit has empty invocation path"),
        Subsystem->GetEditSession() != nullptr && Subsystem->GetEditSession()->GetInvocationPath().IsEmpty());
    if (UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive(); Mode != nullptr && Mode->UsesToolkits())
    {
        const TSharedPtr<FModeToolkit> Toolkit = Mode->GetToolkit().Pin();
        bPassed &= TestTrue(TEXT("Edit toolkit contains the inline Composite Outliner"),
            Toolkit.IsValid() && Toolkit->GetInlineContent().IsValid());
    }
    bPassed &= TestFalse(TEXT("standalone Composite Outliner tab is removed"),
        FGlobalTabmanager::Get()->HasTabSpawner(FName(TEXT("MHCompositeOutliner"))));
    bPassed &= TestTrue(TEXT("root edit cancels"), Subsystem->CancelEditComposite(Error));
    bPassed &= NativeHit(Occurrences[0], ETypedElementSelectionMethod::Primary, true);

    // Secondary (double click) enters the nearest occurrence through the same
    // typed selection route; no helper or direct actor-selection shortcut is used.
    bPassed &= NativeHit(Occurrences[0], ETypedElementSelectionMethod::Secondary, true);
    bPassed &= TestEqual(TEXT("secondary enters occurrence context"), Actor->GetSelectedPlacementOccurrencePath(), Occurrences[0]);
    bPassed &= TestTrue(TEXT("secondary highlights first occurrence"), HighlightedOnly(Occurrences[0]));

    bPassed &= NativeHit(Occurrences[1], ETypedElementSelectionMethod::Primary, false);
    bPassed &= TestEqual(TEXT("RMB in nested selection retargets sibling"), Actor->GetSelectedPlacementOccurrencePath(), Occurrences[1]);
    const FString PickedLeaf = Actor->GetSelectedPlacementLeafPath();
    bPassed &= TestTrue(TEXT("nested RMB Edit opens clicked occurrence"), MHBeginEditPickedComposite(*Actor, PickedLeaf, Error));
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    const USceneComponent* Component = Projection != nullptr ? Projection->FindComponentForOrigin(PickedLeaf) : nullptr;
    bPassed &= TestTrue(TEXT("nested Edit selects the clicked leaf's authored node"),
        Session != nullptr && Session->GetInvocationPath() == Occurrences[1] && Component != nullptr &&
        Session->GetActiveNodeId() == Projection->GetNodeIdForComponent(Component));
    bPassed &= TestTrue(TEXT("nested edit cancels"), Subsystem->CancelEditComposite(Error));
    bPassed &= NativeHit(Occurrences[0], ETypedElementSelectionMethod::Primary, true);
    bPassed &= NativeHit(Occurrences[0], ETypedElementSelectionMethod::Secondary, true);

    // While nested, a primary hit selects the clicked sibling occurrence; a
    // primary hit with the owner already selected must not force root context.
    bPassed &= NativeHit(Occurrences[1], ETypedElementSelectionMethod::Primary, true);
    bPassed &= TestEqual(TEXT("primary nested hit retargets sibling"), Actor->GetSelectedPlacementOccurrencePath(), Occurrences[1]);
    bPassed &= TestTrue(TEXT("sibling occurrence highlighted"), HighlightedOnly(Occurrences[1]));

    // Secondary toggles nested state back to the owner root.
    bPassed &= NativeHit(Occurrences[1], ETypedElementSelectionMethod::Secondary, true);
    bPassed &= TestTrue(TEXT("secondary returns to root context"), Actor->GetSelectedPlacementLeafPath().IsEmpty());
    bPassed &= TestTrue(TEXT("root context highlights whole owner"), HighlightedOnly(FString()));

    bPassed &= NativeHit(Occurrences[0], ETypedElementSelectionMethod::Secondary, true);
    const FTypedElementHandle DirectActor = Set->GetSelectionElement(OwnerHandle, ETypedElementSelectionMethod::Primary);
    Set->ClearSelection(SelectionOptions);
    Set->SelectElement(DirectActor, SelectionOptions);
    Set->NotifyPendingChanges();
    bPassed &= TestTrue(TEXT("direct actor selection returns to root"), Actor->GetSelectedPlacementLeafPath().IsEmpty());
    bPassed &= NativeHit(Occurrences[0], ETypedElementSelectionMethod::Secondary, true);

    // A different actor clears the first owner's native and logical state.
    const FTypedElementHandle OtherActorHandle = UEngineElementsLibrary::AcquireEditorActorElementHandle(Other);
    Set->SelectElement(OtherActorHandle, SelectionOptions); // Native Shift-add.
    Set->NotifyPendingChanges();
    bPassed &= TestTrue(TEXT("multi-actor selection clears logical subcomponent scope"),
        Actor->GetSelectedPlacementLeafPath().IsEmpty() && Other->GetSelectedPlacementLeafPath().IsEmpty());
    Set->ClearSelection(SelectionOptions);
    Set->SelectElement(OwnerHandle, SelectionOptions);
    Set->NotifyPendingChanges();
    bPassed &= NativeHit(Occurrences[0], ETypedElementSelectionMethod::Secondary, true);
    const TArray<FMHCompositeLeafMaterialization>& OtherRows = Other->GetLeafMaterializations();
    if (OtherRows.Num() > 0)
    {
        UInstancedStaticMeshComponent* OtherBucket = Cast<UInstancedStaticMeshComponent>(OtherRows[0].Component.Get());
        const FTypedElementHandle OtherRaw = OtherBucket != nullptr
            ? UEngineElementsLibrary::AcquireEditorSMInstanceElementHandle(OtherBucket, OtherRows[0].InstanceIndex)
            : FTypedElementHandle();
        const FTypedElementHandle OtherResolved = Set->GetSelectionElement(OtherRaw, ETypedElementSelectionMethod::Primary);
        if (OtherResolved)
        {
            Set->ClearSelection(SelectionOptions);
            Set->SelectElement(OtherResolved, SelectionOptions);
            Set->NotifyPendingChanges();
        }
    }
    bPassed &= TestFalse(TEXT("other actor deselects first owner"), Actor->IsSelected());
    bPassed &= TestTrue(TEXT("other actor clears stale first owner context"), Actor->GetSelectedPlacementLeafPath().IsEmpty());
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
