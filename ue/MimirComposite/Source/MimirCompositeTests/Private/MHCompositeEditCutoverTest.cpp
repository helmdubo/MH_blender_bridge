#include "MHCompositeEditFixture.h"

#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"

namespace UE::MimirComposite::Tests
{

// Root and nested editing both use the session projection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditCutoverDefaultBackendTest,
    "Mimir.V5.Composite.EditMode.Cutover.TheModeIsTheProductionBackendByDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditCutoverDefaultBackendTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    bool bPassed = true;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;

    // Edit Contents on a nested definition.
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    bPassed &= TestTrue(TEXT("nested: the mode is active"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestTrue(TEXT("nested: the session projects the occurrence"), Session != nullptr && Session->GetProjection() != nullptr && Session->GetProjection()->GetProjectionActor() != nullptr);
    bPassed &= TestTrue(TEXT("cancel nested"), Subsystem->CancelEditComposite(Error));

    // Edit on the placement itself.
    if (!TestTrue(TEXT("root context: ") + Error, Subsystem->BeginEditComposite(F.A, Error))) return false;
    Session = Subsystem->GetEditSession();
    bPassed &= TestTrue(TEXT("root: the mode is active"), UMHCompositeEditorMode::IsActive());
    bPassed &= TestTrue(TEXT("root: the session projects the placement"), Session != nullptr && Session->GetProjection() != nullptr);
    bPassed &= TestTrue(TEXT("cancel root"), Subsystem->CancelEditComposite(Error));

    return bPassed;
}

} // namespace UE::MimirComposite::Tests
