#include "MHCompositeEditFixture.h"

#include "Composite/MHInstancePool.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"

namespace UE::MimirComposite::Tests
{

// Reference fixture coverage shared by the session interaction tests.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHEditModeFixtureTest,
    "Mimir.V5.Composite.EditMode.Characterization.FixtureResolvesBothInvocations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHEditModeFixtureTest::RunTest(const FString& Parameters)
{
    static_cast<void>(Parameters);
    FCompositeEditFixture F(*this);
    if (!F.Build(*this)) return false;
    bool bPassed = true;
    for (AMHCompositeActor* Actor : {F.A, F.B})
    {
        for (int32 Index = 0; Index < 2; ++Index)
        {
            const FMHResolvedCompositeNode* Invocation = FCompositeEditFixture::Invocation(*Actor, Index);
            if (!TestNotNull(*FString::Printf(TEXT("invocation %d"), Index), Invocation)) return false;
            const TArray<FVector> Leaves = FCompositeEditFixture::LeafWorldsUnder(*Actor, Invocation->NodePath + TEXT(">"));
            // plain C + grouped C, plus mesh A when the random node picks it.
            bPassed &= TestTrue(*FString::Printf(TEXT("invocation %d renders two or three leaves"), Index), Leaves.Num() == 2 || Leaves.Num() == 3);
        }
    }
    bPassed &= TestEqual(TEXT("foreign ISM holds its two instances"), F.ForeignWorlds().Num(), 2);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
