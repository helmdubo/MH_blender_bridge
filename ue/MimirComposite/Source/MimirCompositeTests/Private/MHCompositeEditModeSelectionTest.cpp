#include "MHCompositeEditFixture.h"

#include "Components/PrimitiveComponent.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "EngineUtils.h"
#include "Selection.h"
#include "Settings/MHCompositeSettings.h"
#include "UI/MHEditSessionKeys.h"
#include "UI/MHSourceOverwritePolicy.h"

namespace UE::MimirComposite::Tests
{
namespace
{

struct FSelectionV2Scope
{
    bool bPrevious = false;
    FSelectionV2Scope()
    {
        UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
        bPrevious = Settings->bCompositeEditModeV2;
        Settings->bCompositeEditModeV2 = true;
    }
    ~FSelectionV2Scope()
    {
        GetMutableDefault<UMHCompositeSettings>()->bCompositeEditModeV2 = bPrevious;
        UMHCompositeEditorMode::SetDiscardConfirmForTests({});
        MHSetSourceOverwritePolicyTestHooks(FMHSourceOverwritePolicyTestHooks());
    }
};

bool ComponentSelected(const USceneComponent* Component)
{
    return GEditor != nullptr && Component != nullptr && GEditor->GetSelectedComponents()->IsSelected(Component);
}

} // namespace

// CE-3b (spec CE-3): a Composite Outliner row and a viewport click grab the
// node's projection component — the gizmo sits on the node. Everything
// outside the projection is locked; gizmo axes and empty space keep their
// meaning.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeClickSelectionTest,
    "Mimir.V5.Composite.EditMode.Selection.ClicksGrabTheProjectionNode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeClickSelectionTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FSelectionV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    const FString SecondPath = Second->NodePath;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, SecondPath, Error))) return false;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    AActor* ProjectionActor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    if (!TestNotNull(TEXT("mode"), Mode) || !TestNotNull(TEXT("projection actor"), ProjectionActor)) return false;
    const FString Prefix = SecondPath + TEXT(">") + F.Child->LogicalName + TEXT(":");
    USceneComponent* Plain = Projection->FindComponentForOrigin(Prefix + TEXT("nodes[0]"));
    USceneComponent* Grouped = Projection->FindComponentForOrigin(Prefix + TEXT("nodes[1]/children[0]"));
    bool bPassed = TestNotNull(TEXT("plain leaf by origin"), Plain);
    bPassed &= TestNotNull(TEXT("grouped leaf by origin"), Grouped);
    bPassed &= TestNull(TEXT("unknown origin"), Projection->FindComponentForOrigin(Prefix + TEXT("nodes[9]")));
    if (Plain == nullptr || Grouped == nullptr) return false;

    // Outliner row: the projection actor exclusively, then the node's component.
    bPassed &= TestTrue(TEXT("row click grabs the node"), Mode->SelectComponent(Plain));
    bPassed &= TestTrue(TEXT("projection actor selected"), ProjectionActor->IsSelected());
    bPassed &= TestTrue(TEXT("plain component selected"), ComponentSelected(Plain));
    bPassed &= TestFalse(TEXT("a foreign component is refused"), Mode->SelectComponent(F.A->GetRootComponent()));
    bPassed &= TestTrue(TEXT("plain still selected"), ComponentSelected(Plain));

    // Viewport click on projection geometry: the hit component.
    TRefCountPtr<HHitProxy> HitGrouped = new HActor(ProjectionActor, Cast<UPrimitiveComponent>(Grouped));
    bPassed &= TestTrue(TEXT("viewport click grabs the node"), Mode->HandleHitProxy(HitGrouped));
    bPassed &= TestTrue(TEXT("grouped selected"), ComponentSelected(Grouped));
    bPassed &= TestFalse(TEXT("plain deselected"), ComponentSelected(Plain));

    // Locked context: another placement swallows the click, selection unchanged.
    TRefCountPtr<HHitProxy> HitOther = new HActor(F.B, nullptr);
    bPassed &= TestTrue(TEXT("a click on another placement is swallowed"), Mode->HandleHitProxy(HitOther));
    bPassed &= TestFalse(TEXT("the other placement is not selected"), F.B->IsSelected());
    bPassed &= TestTrue(TEXT("grouped still selected"), ComponentSelected(Grouped));
    bPassed &= TestFalse(TEXT("empty space passes through"), Mode->HandleHitProxy(nullptr));

    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

// CE-3b: the projection renders as the edited thing — its primitives carry
// the Level Instance editing state, so the `EditingLevelInstance` show flag
// dims everything else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeTintTest,
    "Mimir.V5.Composite.EditMode.Selection.ProjectionCarriesTheEditingTint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeTintTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FSelectionV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    AActor* ProjectionActor = Projection != nullptr ? Projection->GetProjectionActor() : nullptr;
    if (!TestNotNull(TEXT("projection actor"), ProjectionActor)) return false;
    bool bPassed = TestTrue(TEXT("the projection actor is in the editing hierarchy"), ProjectionActor->IsInEditLevelInstanceHierarchy());
    int32 Primitives = 0;
    for (const TObjectPtr<USceneComponent>& Component : Projection->GetComponents())
    {
        const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component.Get());
        if (Primitive == nullptr) continue;
        ++Primitives;
        bPassed &= TestTrue(TEXT("primitive carries the editing state: ") + Projection->GetOriginForComponent(Primitive), Primitive->GetLevelInstanceEditingState());
    }
    bPassed &= TestTrue(TEXT("the projection has primitives"), Primitives > 0);
    bPassed &= TestFalse(TEXT("the root placement stays dimmed"), F.A->IsInEditLevelInstanceHierarchy());
    bPassed &= TestTrue(TEXT("cancel"), Subsystem->CancelEditComposite(Error));
    return bPassed;
}

// CE-3b: under the mode the keys follow the Level Instance Edit contract —
// Escape is the mode's Cancel (asks when dirty), Enter publishes nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeKeysTest,
    "Mimir.V5.Composite.EditMode.Selection.KeysFollowTheMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeKeysTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    const FSelectionV2Scope V2;
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    if (!TestNotNull(TEXT("session"), Session) || Session->GetDraft() == nullptr || !UMHCompositeEditorMode::IsActive()) return false;
    bool bPassed = TestTrue(TEXT("edit"), Session->SetNodeTransform(Session->GetDraft()->GetNodeId(0), FTransform(FVector(100.0, 0.0, 40.0)), Error));

    // Enter: no commit, no confirmation, the session stays.
    int32 Confirmations = 0;
    FMHSourceOverwritePolicyTestHooks Hooks;
    Hooks.Confirm = [&Confirmations](const FText&) { ++Confirmations; return false; };
    Hooks.Notify = [](const FText&) {};
    Hooks.MessageLog = [](const FText&) {};
    MHSetSourceOverwritePolicyTestHooks(Hooks);
    bPassed &= TestFalse(TEXT("Enter means nothing under the mode"), MHHandleEditSessionKey(EKeys::Enter, false));
    bPassed &= TestEqual(TEXT("Enter asked for no overwrite"), Confirmations, 0);
    bPassed &= TestTrue(TEXT("session still open after Enter"), Subsystem->IsEditingComposite() && UMHCompositeEditorMode::IsActive());

    // Escape: the mode's Cancel — asks when dirty, staying keeps everything.
    int32 Asked = 0;
    UMHCompositeEditorMode::SetDiscardConfirmForTests([&Asked]() { ++Asked; return false; });
    bPassed &= TestTrue(TEXT("Escape is the mode's Cancel"), MHHandleEditSessionKey(EKeys::Escape, false));
    bPassed &= TestEqual(TEXT("Escape asked once"), Asked, 1);
    bPassed &= TestTrue(TEXT("staying keeps the session and the mode"), Subsystem->IsEditingComposite() && UMHCompositeEditorMode::IsActive());
    UMHCompositeEditorMode::SetDiscardConfirmForTests([&Asked]() { ++Asked; return true; });
    bPassed &= TestTrue(TEXT("Escape again"), MHHandleEditSessionKey(EKeys::Escape, false));
    bPassed &= TestEqual(TEXT("Escape asked again"), Asked, 2);
    bPassed &= TestFalse(TEXT("discarding leaves"), Subsystem->IsEditingComposite() || UMHCompositeEditorMode::IsActive());
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
