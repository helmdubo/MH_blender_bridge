#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite::Tests
{

// CE-6b (spec CE-6 "Новый mode — единственный production Edit entry"): the
// Composite Edit Mode is the backend a project gets without touching any
// setting; the legacy actor-handle path is reachable only by turning the
// setting off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditCutoverDefaultBackendTest,
    "Mimir.V5.Composite.EditMode.Cutover.TheModeIsTheProductionBackendByDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditCutoverDefaultBackendTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    // No scope here on purpose: this is the setting a project ships with.
    bool bPassed = TestTrue(TEXT("the setting is on by default"), GetDefault<UMHCompositeSettings>()->bCompositeEditModeV2);
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;

    // Edit Contents on a nested definition.
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    bPassed &= TestTrue(TEXT("nested: the mode is active"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestFalse(TEXT("nested: no legacy edit mode on the placement"), F.A->IsPlacementEditMode());
    bPassed &= TestTrue(TEXT("nested: the session projects the occurrence"), Session != nullptr && Session->GetProjection() != nullptr && Session->GetProjection()->GetProjectionActor() != nullptr);
    bPassed &= TestTrue(TEXT("cancel nested"), Subsystem->CancelEditComposite(Error));

    // Edit on the placement itself.
    if (!TestTrue(TEXT("root context: ") + Error, Subsystem->BeginEditComposite(F.A, Error))) return false;
    Session = Subsystem->GetEditSession();
    bPassed &= TestTrue(TEXT("root: the mode is active"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestFalse(TEXT("root: no legacy edit mode on the placement"), F.A->IsPlacementEditMode());
    bPassed &= TestTrue(TEXT("root: the session projects the placement"), Session != nullptr && Session->GetProjection() != nullptr);
    bPassed &= TestTrue(TEXT("cancel root"), Subsystem->CancelEditComposite(Error));

    // The legacy path is still there for whoever turns the setting off.
    {
        const FMHCompositeEditBackendScope Legacy(false);
        if (!TestTrue(TEXT("legacy context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
        bPassed &= TestFalse(TEXT("legacy: the mode stays out"), UMHCompositeEditorMode::IsActive());
        bPassed &= TestTrue(TEXT("legacy: the placement holds the handles"), F.A->IsPlacementEditMode());
        bPassed &= TestTrue(TEXT("cancel legacy"), Subsystem->CancelEditComposite(Error));
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
