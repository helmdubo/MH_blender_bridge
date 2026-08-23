#include "Misc/AutomationTest.h"

#include "Ledger/MHImportLedger.h"
#include "Misc/Guid.h"
#include "Source/MHSourceImporter.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace UE::MimirComposite;

namespace
{

class FPlanFakeResolver final : public IMHSourceResolver
{
public:
    virtual FMHSourceSnapshot GetSnapshot() const override { return FMHSourceSnapshot(); }
    virtual FMHResolveOutcome Resolve(const FString&, EMHResourceKind) override
    {
        return FMHResolveOutcome();
    }
};

class FPlanFakeDetector final : public IMHChangeDetector
{
public:
    virtual void DetectChanges(
        IMHSourceResolver&,
        const FString&,
        FMHSourceAnalysis& OutAnalysis) override
    {
        FMHSourceAnalysisEntry First;
        First.ResourceUid = TEXT("00000000-0000-4000-8000-000000000001");
        First.Change = EMHSourceChange::Create;
        OutAnalysis.Entries.Add(MoveTemp(First));

        FMHSourceAnalysisEntry Second;
        Second.ResourceUid = TEXT("00000000-0000-4000-8000-000000000002");
        Second.Change = EMHSourceChange::Move;
        OutAnalysis.Entries.Add(MoveTemp(Second));
    }
};

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHSourceImporterScopeTest,
    "Mimir.C1.SourceImporterScope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHSourceImporterScopeTest::RunTest(const FString& Parameters)
{
    (void)Parameters;

    FMHSourceAnalysis Analysis;
    FMHSourceAnalysisEntry A;
    A.ResourceUid = TEXT("00000000-0000-4000-8000-000000000001");
    Analysis.Entries.Add(A);
    FMHSourceAnalysisEntry B;
    B.ResourceUid = TEXT("00000000-0000-4000-8000-000000000002");
    Analysis.Entries.Add(B);

    FMHImportSourcesScope Scope;
    Scope.ResourceUids.Add(B.ResourceUid);
    MHFilterAnalysisToScope(Scope, Analysis);

    bool bPassed = TestEqual(TEXT("one scoped entry remains"), Analysis.Entries.Num(), 1);
    if (Analysis.Entries.Num() == 1)
    {
        bPassed &= TestEqual(TEXT("requested UID remains"), Analysis.Entries[0].ResourceUid, B.ResourceUid);
    }

    FMHSourceAnalysis AllAnalysis;
    AllAnalysis.Entries.Add(A);
    AllAnalysis.Entries.Add(B);
    MHFilterAnalysisToScope(FMHImportSourcesScope::All(), AllAnalysis);
    bPassed &= TestEqual(TEXT("empty scope means all"), AllAnalysis.Entries.Num(), 2);

    FPlanFakeResolver Resolver;
    FPlanFakeDetector Detector;
    FMHSourceAnalysis Planned;
    bool bExecuted = true;
    bPassed &= TestTrue(
        TEXT("pure coordinator builds a scoped plan"),
        MHBuildSourceImportPlan(
            Detector,
            Resolver,
            TEXT("X:/read-only-source"),
            Scope,
            Planned,
            bExecuted));
    bPassed &= TestFalse(TEXT("C1 coordinator never executes"), bExecuted);
    bPassed &= TestEqual(TEXT("coordinator applies scope once"), Planned.Entries.Num(), 1);

    FMHImportSourcesScope MissingScope;
    const FString MissingUid = TEXT("00000000-0000-4000-8000-000000000099");
    MissingScope.ResourceUids.Add(MissingUid);
    FMHSourceAnalysis MissingPlan;
    bExecuted = true;
    bPassed &= TestFalse(
        TEXT("unknown requested scope UID blocks the plan"),
        MHBuildSourceImportPlan(
            Detector,
            Resolver,
            TEXT("X:/read-only-source"),
            MissingScope,
            MissingPlan,
            bExecuted));
    bPassed &= TestFalse(TEXT("blocked scope never executes"), bExecuted);
    bPassed &= TestEqual(TEXT("blocked UID has one plan entry"), MissingPlan.Entries.Num(), 1);
    const FMHSourceAnalysisEntry* MissingEntry = MissingPlan.Find(MissingUid);
    bPassed &= TestTrue(TEXT("blocked UID is represented"), MissingEntry != nullptr);
    if (MissingEntry != nullptr)
    {
        bPassed &= TestEqual(
            TEXT("unknown UID classification"),
            static_cast<int32>(MissingEntry->Change),
            static_cast<int32>(EMHSourceChange::Blocked));
        bPassed &= TestTrue(
            TEXT("unknown UID diagnostic"),
            MissingEntry->Errors.ContainsByPredicate([](const FString& Error)
            {
                return Error.StartsWith(TEXT("MH_E_RESOURCE_NOT_FOUND"));
            }));
        bPassed &= TestFalse(TEXT("unknown UID cannot advance Ledger"), MissingEntry->bLedgerAdvanceAllowed);
    }

    FMHImportSourcesScope MisCasedScope;
    const FString MisCasedUid = FString(TEXT("abcdef00-0000-4000-8000-000000000001")).ToUpper();
    MisCasedScope.ResourceUids.Add(MisCasedUid);
    FMHSourceAnalysis MisCasedPlan;
    bExecuted = true;
    bPassed &= TestFalse(
        TEXT("mis-cased requested scope UID blocks the plan"),
        MHBuildSourceImportPlan(
            Detector,
            Resolver,
            TEXT("X:/read-only-source"),
            MisCasedScope,
            MisCasedPlan,
            bExecuted));
    bPassed &= TestEqual(TEXT("mis-cased scope has one blocked entry"), MisCasedPlan.Entries.Num(), 1);
    if (MisCasedPlan.Entries.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("mis-cased request does not alias canonical UID"),
            MisCasedPlan.Entries[0].ResourceUid,
            MisCasedUid);
        bPassed &= TestTrue(
            TEXT("mis-cased request is source-index invalid"),
            MisCasedPlan.Entries[0].Errors.ContainsByPredicate([](const FString& Error)
            {
                return Error.StartsWith(TEXT("MH_E_SOURCE_INDEX_INVALID"));
            }));
    }

    const FString MissingContentRoot = FString::Printf(
        TEXT("/Game/MH_C1_NoCreate_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString MissingPackageName = MissingContentRoot + TEXT("/_MH/Ledger");
    bPassed &= TestTrue(
        TEXT("test package starts absent"),
        FindPackage(nullptr, *MissingPackageName) == nullptr);
    bPassed &= TestTrue(
        TEXT("read-only Ledger lookup returns absent"),
        UMHImportLedger::LoadExisting(MissingContentRoot) == nullptr);
    bPassed &= TestTrue(
        TEXT("read-only Ledger lookup creates no package"),
        FindPackage(nullptr, *MissingPackageName) == nullptr);
    return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
