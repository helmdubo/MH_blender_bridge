#include "MHCompositeEditFixture.h"

#include "Composite/MHCompositeImporter.h"
#include "Editing/MHCompositeEditDocument.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"

namespace UE::MimirComposite::Tests
{
namespace
{

TArray<FMHCompositeAdoptTarget> TargetsFor(const FMHCompositeSaveUniquePlan& Plan, const TCHAR* Suffix)
{
    TArray<FMHCompositeAdoptTarget> Targets;
    for (const FString& Copy : Plan.Copies)
    {
        FMHCompositeAdoptTarget& Target = Targets.AddDefaulted_GetRef();
        Target.LogicalName = Copy + Suffix;
    }
    return Targets;
}

} // namespace

// CE-5b (spec §10.3, A25/A27): Save As Unique from a CE-backend session —
// a failure before any copy exists keeps the session and the draft
// (NoExternalChange, retry works); a failure on a later copy is a partial
// batch: the copies that exist are named, the session stays for recovery.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditUniqueFailureTest,
    "Mimir.V5.Composite.EditMode.Publish.UniqueFailureKeepsTheSessionAndNamesTheOrphans",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditUniqueFailureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);

    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("level subsystem"), Subsystem)) return false;
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    const FMHResolvedCompositeNode* Second = FCompositeEditFixture::Invocation(*F.A, 1);
    if (!TestNotNull(TEXT("second"), Second)) return false;
    FString Error;
    TArray<uint8> ChildBefore;
    if (!TestTrue(TEXT("child bytes"), FCompositeEditFixture::AssetBytes(*F.Child, ChildBefore))) return false;
    if (!TestTrue(TEXT("nested context: ") + Error, Subsystem->BeginEditNestedComposite(F.A, Second->NodePath, Error))) return false;
    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditDocument* Draft = Session != nullptr ? Session->GetDraft() : nullptr;
    if (!TestNotNull(TEXT("draft"), Draft)) return false;
    const uint32 Epoch = Subsystem->GetEditSessionEpoch();
    bool bPassed = TestTrue(TEXT("edit"), Session->SetNodeTransform(Draft->GetNodeId(0), FTransform(FVector(100.0, 0.0, 40.0)), Error));
    FMHCompositeSaveUniquePlan Plan;
    if (!TestTrue(TEXT("describe: ") + Error, Subsystem->DescribeSaveUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, Plan, Error)) || Plan.Copies.Num() != 2) return false;

    // Nothing created: the first copy is refused.
    Subsystem->SetDefinitionCreatorForTests([](const FMHCompositeDocument&, const FMHCompositeAdoptTarget&, FString& OutError) -> UMHCompositeAsset*
    {
        OutError = TEXT("MH_E_SOURCE_WRITE: simulated failure before the first copy");
        return nullptr;
    });
    TArray<FString> Warnings;
    bPassed &= TestFalse(TEXT("the batch fails"), Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, TargetsFor(Plan, TEXT("_u1")), Warnings, Error));
    bPassed &= TestEqual(TEXT("outcome: nothing external changed"), Subsystem->GetLastPublishOutcome(), EMHCompositePublishOutcome::NoExternalChange);
    bPassed &= TestTrue(TEXT("the session survived"), Subsystem->IsEditingComposite() && Subsystem->GetEditSession() == Session && Session->IsOpen() && UMHCompositeEditorMode::IsActive());
    bPassed &= TestEqual(TEXT("the same session"), Subsystem->GetEditSessionEpoch(), Epoch);
    bPassed &= TestTrue(TEXT("the draft keeps the edit"), Session->IsDirty());
    TArray<uint8> ChildAfter;
    bPassed &= TestTrue(TEXT("the shared child is untouched"), FCompositeEditFixture::AssetBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);

    // The second copy fails: the first exists and is named.
    TArray<FString> CreatedNames;
    Subsystem->SetDefinitionCreatorForTests([&F, &CreatedNames](const FMHCompositeDocument& Document, const FMHCompositeAdoptTarget& Target, FString& OutError) -> UMHCompositeAsset*
    {
        if (!CreatedNames.IsEmpty())
        {
            OutError = TEXT("MH_E_SOURCE_WRITE: simulated failure on the second copy");
            return nullptr;
        }
        CreatedNames.Add(Target.LogicalName);
        return F.Recipe.Composite(Target.LogicalName, Document, {});
    });
    Warnings.Reset();
    const TArray<FMHCompositeAdoptTarget> Targets2 = TargetsFor(Plan, TEXT("_u2"));
    bPassed &= TestFalse(TEXT("the batch fails on the second copy"), Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, Targets2, Warnings, Error));
    bPassed &= TestEqual(TEXT("outcome: a partial batch"), Subsystem->GetLastPublishOutcome(), EMHCompositePublishOutcome::PartialBatch);
    bPassed &= TestTrue(TEXT("the orphan is named"), CreatedNames.Num() == 1 && Warnings.ContainsByPredicate([&CreatedNames](const FString& Warning) { return Warning.Contains(CreatedNames[0]); }));
    bPassed &= TestTrue(TEXT("the session survived the partial batch"), Subsystem->IsEditingComposite() && Subsystem->GetEditSession() == Session && Session->IsOpen());
    bPassed &= TestTrue(TEXT("the draft still keeps the edit"), Session->IsDirty());
    bPassed &= TestTrue(TEXT("the shared child is still untouched"), FCompositeEditFixture::AssetBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);

    // Retry: both copies land, the session closes.
    Subsystem->SetDefinitionCreatorForTests([&F](const FMHCompositeDocument& Document, const FMHCompositeAdoptTarget& Target, FString&) -> UMHCompositeAsset*
    {
        return F.Recipe.Composite(Target.LogicalName, Document, {});
    });
    Warnings.Reset();
    Error.Reset();
    bPassed &= TestTrue(TEXT("retry: ") + Error, Subsystem->SaveEditAsUnique(EMHCompositeUniqueScope::ForThisPlacement, EMHCompositeUniqueVariant::Procedural, TargetsFor(Plan, TEXT("_u3")), Warnings, Error));
    Subsystem->SetDefinitionCreatorForTests({});
    bPassed &= TestEqual(TEXT("retry: outcome"), Subsystem->GetLastPublishOutcome(), EMHCompositePublishOutcome::Succeeded);
    bPassed &= TestFalse(TEXT("retry: the session closed"), Subsystem->IsEditingComposite() || UMHCompositeEditorMode::IsActive());
    bPassed &= TestTrue(TEXT("retry: the shared child is untouched (this placement went unique)"), FCompositeEditFixture::AssetBytes(*F.Child, ChildAfter) && ChildAfter == ChildBefore);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
