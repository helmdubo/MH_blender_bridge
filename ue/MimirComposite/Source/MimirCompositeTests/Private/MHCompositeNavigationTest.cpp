#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Engine/StaticMesh.h"
#include "EditorViewportCommands.h"
#include "Framework/Commands/UICommandList.h"
#include "ILevelEditor.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Modules/ModuleManager.h"
#include "Selection.h"
#include "SLevelViewport.h"
#include "UI/MHCompositeNavigation.h"
#include "UI/MHCompositeOutlinerModel.h"

namespace UE::MimirComposite::Tests
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPlacementNavigationTest,
    "Mimir.V5.Composite.Navigation.PlacementFocusAndBrowse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPlacementNavigationTest::RunTest(const FString& Parameters)
{
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    F.MeshAssetA->SetExtendedBounds(FBoxSphereBounds(FVector::ZeroVector, FVector(40, 50, 60), 90));
    F.MeshAssetC->SetExtendedBounds(FBoxSphereBounds(FVector::ZeroVector, FVector(20, 30, 40), 60));
    const auto Rows = F.A->GetLeafMaterializations();
    if (!TestTrue(TEXT("several pooled leaves"), Rows.Num() > 2)) return false;
    FBox Expected(ForceInit);
    for (const auto& Row : Rows) Expected += MHGetVisualFocusBounds(Row.Component, Row.InstanceIndex);
    const FBox Whole = MHGetPlacementFocusBounds(*F.A);
    TestTrue(TEXT("root uses geometry bounds"), Whole.Equals(Expected));
    TestFalse(TEXT("other owner in same pool excluded"), Whole.IsInside(F.B->GetActorLocation()));
    FMHCompositeOutlinerModel Outliner;
    TestTrue(TEXT("placement outliner built"), Outliner.BuildFromActor(*F.A));
    const auto* Nested = FCompositeEditFixture::Invocation(*F.A, 0);
    if (!TestNotNull(TEXT("nested occurrence"), Nested)) return false;
    FBox NestedExpected(ForceInit);
    for (const auto& Row : Rows)
        if (Row.NodePath.StartsWith(Nested->NodePath + TEXT(">"))) NestedExpected += MHGetVisualFocusBounds(Row.Component, Row.InstanceIndex);
    TestTrue(TEXT("outliner nested reference focuses its geometry only"),
        MHGetOutlinerFocusBounds(Outliner, Outliner.FindByNodePath(Nested->NodePath)).Equals(NestedExpected));

    // Switch leaves, then clear selection; a shared component must not expand
    // focus to its other instances, owners, or the composite pivot.
    for (const int32 Index : {0, 1})
    {
        const auto& Row = Rows[Index];
        TestTrue(TEXT("select exact leaf"), F.A->SelectPlacementLeafByNodePath(Row.NodePath));
        const FBox Leaf = MHGetVisualFocusBounds(Row.Component, Row.InstanceIndex);
        TestTrue(TEXT("focus follows selected instance"), MHGetPlacementFocusBounds(*F.A).Equals(Leaf));
        TArray<UObject*> Assets;
        F.A->GetReferencedContentObjects(Assets);
        const FString& Resource = F.A->GetResolvedPlan()->Leaves[Index].Resource;
        UStaticMesh* ExpectedMesh = Resource == F.MeshA ? F.MeshAssetA : F.MeshAssetC;
        TestEqual(TEXT("one browse target"), Assets.Num(), 1);
        TestTrue(TEXT("browse selected mesh, not root"), Assets.Contains(ExpectedMesh) && !Assets.Contains(F.Root));
    }
    F.A->ClearPlacementLeafSelection();
    TestTrue(TEXT("clear restores whole composite focus"), MHGetPlacementFocusBounds(*F.A).Equals(Whole));
    TArray<UObject*> Assets;
    F.A->GetReferencedContentObjects(Assets);
    TestTrue(TEXT("root browses its definition"), Assets.Contains(F.Root));
    const auto Editor = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor")).GetFirstLevelEditor();
    if (!TestTrue(TEXT("level editor available for native F command"), Editor.IsValid())) return false;
    bool bExercisedViewport = false;
    for (const auto& Viewport : Editor->GetViewports())
    {
        if (!Viewport.IsValid() || !Viewport->GetLevelViewportClient().IsPerspective()) continue;
        auto& Client = Viewport->GetLevelViewportClient();
        TGuardValue<FLevelEditorViewportClient*> CurrentViewport(GCurrentLevelEditingViewportClient, &Client);
        GEditor->SelectNone(false, true, false);
        GEditor->SelectActor(F.A, true, true, true);
        Viewport->GetCommandList()->ExecuteAction(FEditorViewportCommands::Get().FocusViewportToSelection.ToSharedRef());
        TestTrue(TEXT("native F command frames geometry instead of actor icon"), Client.GetLookAtLocation().Equals(Whole.GetCenter(), 0.01));
        F.A->SelectPlacementLeafByNodePath(Rows[1].NodePath);
        Viewport->GetCommandList()->ExecuteAction(FEditorViewportCommands::Get().FocusViewportToSelection.ToSharedRef());
        TestTrue(TEXT("native F command follows leaf selection"), Client.GetLookAtLocation().Equals(MHGetPlacementFocusBounds(*F.A).GetCenter(), 0.01));
        GEditor->SelectNone(false, true, false);
        bExercisedViewport = true;
        break;
    }
    TestTrue(TEXT("perspective viewport F exercised"), bExercisedViewport);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHEditNavigationTargetsTest,
    "Mimir.V5.Composite.Navigation.EditFocusAndBrowse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditNavigationTargetsTest::RunTest(const FString& Parameters)
{
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    auto* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
    FString Error;
    if (!TestTrue(TEXT("open root edit"), Subsystem->BeginEditComposite(F.A, Error))) return false;
    auto* Session = Subsystem->GetEditSession();
    auto* Mode = UMHCompositeEditorMode::GetActive();
    auto* Draft = Session->GetDraft();
    const FGuid MeshId = Draft->GetNodeId(0);
    const FGuid CompositeId = Draft->GetNodeId(1);
    Mode->SelectNodeIds({CompositeId});
    TArray<UObject*> Assets;
    Session->GetProjection()->GetProjectionActor()->GetReferencedContentObjects(Assets);
    TestTrue(TEXT("nested reference browses child definition only"), Assets.Num() == 1 && Assets.Contains(F.Child));
    FBox Expected(ForceInit);
    TestTrue(TEXT("nested geometry available"), Session->GetProjection()->GetNodeBounds(CompositeId, Expected));
    TestTrue(TEXT("nested focus uses descendants"), Mode->ComputeCustomViewportFocus().Equals(Expected));

    Mode->SelectNodeIds({MeshId, CompositeId});
    Assets.Reset();
    MHAppendEditSelectionAssets(*Session, Assets);
    TestTrue(TEXT("multiselect mesh and composite assets"), Assets.Num() == 2 && Assets.Contains(F.MeshAssetA) && Assets.Contains(F.Child));
    FBox MeshBounds(ForceInit);
    Session->GetProjection()->GetNodeBounds(MeshId, MeshBounds);
    Expected += MeshBounds;
    TestTrue(TEXT("multi focus unions selected geometry"), Mode->ComputeCustomViewportFocus().Equals(Expected));

    Subsystem->CancelEditComposite(Error);
    const auto* Invocation = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!Invocation || !Subsystem->BeginEditNestedComposite(F.A, Invocation->NodePath, Error)) return false;
    Session = Subsystem->GetEditSession();
    Mode = UMHCompositeEditorMode::GetActive();
    Mode->SelectNodeIds({Session->GetDraft()->GetNodeId(0)});
    Assets.Reset();
    MHAppendEditSelectionAssets(*Session, Assets);
    TestTrue(TEXT("nested session leaf browses mesh"), Assets.Num() == 1 && Assets.Contains(F.MeshAssetC));
    Mode->SelectNodeIds({});
    Assets.Reset();
    MHAppendEditSelectionAssets(*Session, Assets);
    TestTrue(TEXT("empty selection browses edited composite"), Assets.Num() == 1 && Assets.Contains(F.Child));

    FMHCompositeOutlinerModel Model;
    Model.BuildFromActor(*F.A);
    FMHCompositeOutlinerItem Empty;
    Empty.Kind = EMHRandomSemanticKind::Empty;
    Assets.Reset();
    MHAppendOutlinerAssets(Model, Empty, Assets);
    TestTrue(TEXT("empty option does not fall back to root"), Assets.IsEmpty());
    auto Inactive = MakeShared<FMHCompositeOutlinerItem>();
    Inactive->ItemType = EMHCompositeOutlinerItemType::Option;
    Inactive->Kind = EMHRandomSemanticKind::Mesh;
    Inactive->Resource = F.MeshA;
    MHAppendOutlinerAssets(Model, *Inactive, Assets);
    TestTrue(TEXT("explicit inactive option browses its own resource"), Assets.Num() == 1 && Assets.Contains(F.MeshAssetA));
    TestFalse(TEXT("inactive option has no scene geometry to focus"), MHGetOutlinerFocusBounds(Model, Inactive).IsValid != 0);
    Assets.Reset();
    Inactive->Resource = F.Recipe.Name(TEXT("missing"));
    MHAppendOutlinerAssets(Model, *Inactive, Assets);
    TestTrue(TEXT("missing resource does not browse root"), Assets.IsEmpty());
    return true;
}
}
