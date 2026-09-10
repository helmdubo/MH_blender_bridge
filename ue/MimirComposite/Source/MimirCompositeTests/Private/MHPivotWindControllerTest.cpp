#include "Wind/MHPivotWindController.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Misc/AutomationTest.h"
#include "RenderingThread.h"
#include "UObject/StrongObjectPtr.h"

#include <limits>

namespace UE::MimirComposite::Tests
{
namespace
{
const FName DirectionSpeedParameter(TEXT("MH_WindDirectionSpeed"));
const FName NoiseParameter(TEXT("MH_WindNoise"));

struct FWindFixture
{
    TStrongObjectPtr<UMaterialParameterCollection> Collection;
    TArray<UWorld*> Worlds;

    FWindFixture()
        : Collection(NewObject<UMaterialParameterCollection>())
    {
        Collection->PreEditChange(nullptr);
        FCollectionVectorParameter Direction;
        Direction.ParameterName = DirectionSpeedParameter;
        Direction.DefaultValue = FLinearColor(1.0f, 0.0f, 0.0f, 0.0f);
        Collection->VectorParameters.Add(Direction);
        FCollectionVectorParameter Noise;
        Noise.ParameterName = NoiseParameter;
        Noise.DefaultValue = FLinearColor::Transparent;
        Collection->VectorParameters.Add(Noise);
        FCollectionVectorParameter Tree;
        Tree.ParameterName = TEXT("MH_WindTree");
        Tree.DefaultValue = FLinearColor(0.1f,0.1f,0,0);
        Collection->VectorParameters.Add(Tree);
        Collection->PostEditChange();
    }

    ~FWindFixture()
    {
        for (UWorld* World : Worlds)
        {
            World->DestroyWorld(false);
            if (GEngine) GEngine->DestroyWorldContext(World);
        }
        FlushRenderingCommands();
    }

    AMHPivotWindController* AddWorldController()
    {
        UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
        if (World == nullptr) return nullptr;
        if (GEngine) GEngine->CreateNewWorldContext(EWorldType::EditorPreview).SetCurrentWorld(World);
        Worlds.Add(World);
        AMHPivotWindController* Controller = World->SpawnActor<AMHPivotWindController>();
        if (Controller != nullptr) Controller->WindParameters = Collection.Get();
        return Controller;
    }

    bool Read(UWorld* World, const FName Name, FLinearColor& Value) const
    {
        UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection.Get());
        return Instance != nullptr && Instance->GetVectorParameterValue(Name, Value);
    }
};

bool Finite(const FLinearColor& Value)
{
    return FMath::IsFinite(Value.R) && FMath::IsFinite(Value.G) &&
        FMath::IsFinite(Value.B) && FMath::IsFinite(Value.A);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPivotWindWorldIsolationTest,
    "Mimir.Wind.Controller.WorldIsolationAndOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPivotWindWorldIsolationTest::RunTest(const FString& Parameters)
{
    FWindFixture Fixture;
    AMHPivotWindController* First = Fixture.AddWorldController();
    AMHPivotWindController* Second = Fixture.AddWorldController();
    if (!TestNotNull(TEXT("first controller"), First) || !TestNotNull(TEXT("second controller"), Second)) return false;
    First->DirectionDegrees = 90.0f;
    First->StrengthBeaufort = 4.0f;
    First->NoiseSpeedBeaufort = 1.0f;
    First->NoiseScaleMeters = 50.0f;
    First->NoisePerpendicular = 0.5f;
    First->NoiseStrength = 0.75f;
    Second->DirectionDegrees = 180.0f;
    Second->StrengthBeaufort = 2.0f;
    bool bOk = TestTrue(TEXT("first world applies"), First->ApplySettings());
    bOk &= TestTrue(TEXT("second world applies independently"), Second->ApplySettings());
    bOk &= TestFalse(TEXT("controller never ticks"), First->PrimaryActorTick.bCanEverTick);
    FLinearColor FirstValue, SecondValue, Noise;
    bOk &= TestTrue(TEXT("first world value exists"), Fixture.Read(First->GetWorld(), DirectionSpeedParameter, FirstValue));
    bOk &= TestTrue(TEXT("second world value exists"), Fixture.Read(Second->GetWorld(), DirectionSpeedParameter, SecondValue));
    bOk &= TestTrue(TEXT("Dagor 90 degrees maps to UE -Y"), FMath::IsNearlyZero(FirstValue.R, 1.e-6f) && FMath::IsNearlyEqual(FirstValue.G, -1.0f, 1.e-6f));
    bOk &= TestTrue(TEXT("Beaufort four speed"), FMath::IsNearlyEqual(FirstValue.B, 6.688f));
    bOk &= TestEqual(TEXT("enabled flag"), FirstValue.A, 1.0f);
    bOk &= TestTrue(TEXT("180 degrees is -X in second world"), FMath::IsNearlyEqual(SecondValue.R, -1.0f, 1.e-6f) && FMath::IsNearlyZero(SecondValue.G, 1.e-6f));
    bOk &= TestTrue(TEXT("noise vector exists"), Fixture.Read(First->GetWorld(), NoiseParameter, Noise));
    bOk &= TestTrue(TEXT("Dagor raw noise speed and inverse scale"), Noise.R == 1.0f && FMath::IsNearlyEqual(Noise.G, 0.02f));
    FLinearColor Tree;
    Fixture.Read(First->GetWorld(), TEXT("MH_WindTree"), Tree);
    bOk &= TestTrue(TEXT("CDK branch/detail amplitudes"), Tree.Equals(FLinearColor(.1f,.1f,0,0)));
    First->BranchAmplitude = .3f;
    First->DetailAmplitude = .7f;
    First->ApplySettings();
    Fixture.Read(First->GetWorld(), TEXT("MH_WindTree"), Tree);
    bOk &= TestTrue(TEXT("leaf response is published per world"), Tree.Equals(FLinearColor(.3f,.7f,0,0)));
    bOk &= TestEqual(TEXT("fractional perpendicular multiplier"), Noise.B, 0.5f);
    bOk &= TestEqual(TEXT("noise multiplier"), Noise.A, 0.75f);
    bOk &= TestEqual(TEXT("collection defaults remain unchanged"), Fixture.Collection->VectorParameters[0].DefaultValue.A, 0.0f);

    AMHPivotWindController* Duplicate = First->GetWorld()->SpawnActor<AMHPivotWindController>();
    if (!TestNotNull(TEXT("duplicate controller"), Duplicate)) return false;
    Duplicate->WindParameters = Fixture.Collection.Get();
    Duplicate->DirectionDegrees = 270.0f;
    bOk &= TestFalse(TEXT("duplicate cannot overwrite owner"), Duplicate->ApplySettings());
    FLinearColor AfterDuplicate;
    Fixture.Read(First->GetWorld(), DirectionSpeedParameter, AfterDuplicate);
    bOk &= TestTrue(TEXT("rejected duplicate leaves original values"), AfterDuplicate.Equals(FirstValue));
    First->Enabled = false;
    bOk &= TestTrue(TEXT("owner can disable wind"), First->ApplySettings());
    bOk &= TestFalse(TEXT("disabled owner still owns the world"), Duplicate->ApplySettings());
    UWorld* FirstWorld = First->GetWorld();
    bOk &= TestTrue(TEXT("owner can be removed"), FirstWorld->DestroyActor(First));
    FLinearColor RemovedValue;
    Fixture.Read(FirstWorld, DirectionSpeedParameter, RemovedValue);
    bOk &= TestEqual(TEXT("normal owner removal leaves wind disabled"), RemovedValue.A, 0.0f);
    bOk &= TestTrue(TEXT("duplicate can explicitly claim released ownership"), Duplicate->ApplySettings());
    FLinearColor SecondAfter;
    Fixture.Read(Second->GetWorld(), DirectionSpeedParameter, SecondAfter);
    bOk &= TestTrue(TEXT("another world's controller is unaffected"), SecondAfter.Equals(SecondValue));
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPivotWindFiniteLifecycleTest,
    "Mimir.Wind.Controller.FiniteDisabledAndTeardown",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPivotWindFiniteLifecycleTest::RunTest(const FString& Parameters)
{
    FWindFixture Fixture;
    AMHPivotWindController* Controller = Fixture.AddWorldController();
    if (!TestNotNull(TEXT("controller"), Controller)) return false;
    Controller->WindParameters = nullptr;
    bool bOk = TestFalse(TEXT("null collection is safe"), Controller->ApplySettings());
    Controller->WindParameters = Fixture.Collection.Get();
    Controller->Enabled = false;
    Controller->DirectionDegrees = std::numeric_limits<float>::infinity();
    Controller->StrengthBeaufort = std::numeric_limits<float>::quiet_NaN();
    Controller->NoiseSpeedBeaufort = -4.0f;
    Controller->NoiseScaleMeters = 0.0f;
    Controller->NoiseStrength = std::numeric_limits<float>::infinity();
    Controller->NoisePerpendicular = std::numeric_limits<float>::quiet_NaN();
    bOk &= TestTrue(TEXT("non-finite settings are sanitized"), Controller->ApplySettings());
    FLinearColor Direction, Noise;
    Fixture.Read(Controller->GetWorld(), DirectionSpeedParameter, Direction);
    Fixture.Read(Controller->GetWorld(), NoiseParameter, Noise);
    bOk &= TestTrue(TEXT("all published values are finite"), Finite(Direction) && Finite(Noise));
    bOk &= TestEqual(TEXT("disabled flag is zero"), Direction.A, 0.0f);
    bOk &= TestEqual(TEXT("invalid strength becomes calm"), Direction.B, 0.0f);
    bOk &= TestTrue(TEXT("invalid angle falls back to +X"), Direction.R == 1.0f && Direction.G == 0.0f);
    bOk &= TestEqual(TEXT("negative noise speed becomes calm"), Noise.R, 0.0f);
    bOk &= TestEqual(TEXT("zero noise scale selects constant noise coordinates like Dagor"), Noise.G, 0.0f);
    bOk &= TestEqual(TEXT("non-finite noise multiplier becomes zero"), Noise.A, 0.0f);
    bOk &= TestEqual(TEXT("non-finite perpendicular multiplier becomes zero"), Noise.B, 0.0f);
    bOk &= TestEqual(TEXT("negative Beaufort clamps to calm"), AMHPivotWindController::BeaufortToMetersPerSecond(-1.0f), 0.0f);
    Controller->Enabled = true;
    Controller->StrengthBeaufort = 99.0f;
    Controller->PostEditUndo();
    Fixture.Read(Controller->GetWorld(), DirectionSpeedParameter, Direction);
    bOk &= TestTrue(TEXT("Undo hook republishes and clamps to Beaufort twelve"),
        FMath::IsNearlyEqual(Direction.B, AMHPivotWindController::BeaufortToMetersPerSecond(12.0f)) && Direction.A == 1.0f);

    UWorld* World = Controller->GetWorld();
    World->bIsTearingDown = true;
    Controller->DirectionDegrees = 135.0f;
    bOk &= TestFalse(TEXT("teardown rejects writes"), Controller->ApplySettings());
    FLinearColor DuringTeardown;
    Fixture.Read(World, DirectionSpeedParameter, DuringTeardown);
    bOk &= TestTrue(TEXT("teardown leaves existing overrides unchanged"), DuringTeardown.Equals(Direction));
    World->bIsTearingDown = false;
    Controller->WindParameters = nullptr;
    bOk &= TestFalse(TEXT("clearing collection releases controller"), Controller->ApplySettings());
    Fixture.Read(World, DirectionSpeedParameter, Direction);
    bOk &= TestEqual(TEXT("clearing collection disables previous wind"), Direction.A, 0.0f);
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPivotWindWorldReinitializationTest,
    "Mimir.Wind.Controller.WorldReinitializationAndRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPivotWindWorldReinitializationTest::RunTest(const FString& Parameters)
{
    FWindFixture Fixture;
    AMHPivotWindController* Controller = Fixture.AddWorldController();
    if (!TestNotNull(TEXT("controller"), Controller)) return false;
    UWorld* World = Controller->GetWorld();
    Controller->DirectionDegrees = 72.0f;
    Controller->StrengthBeaufort = 5.0f;
    Controller->NoiseSpeedBeaufort = 2.0f;
    Controller->NoiseScaleMeters = 70.0f;
    Controller->NoisePerpendicular = 0.4f;
    Controller->NoiseStrength = 1.8f;
    bool bOk = TestTrue(TEXT("owner publishes initial settings"), Controller->ApplySettings());
    FLinearColor ExpectedDirection, ExpectedNoise;
    Fixture.Read(World, DirectionSpeedParameter, ExpectedDirection);
    Fixture.Read(World, NoiseParameter, ExpectedNoise);
    const TStrongObjectPtr<UMaterialParameterCollectionInstance> OriginalInstance(
        World->GetParameterCollectionInstance(Fixture.Collection.Get()));
    if (!TestNotNull(TEXT("original collection instance"), OriginalInstance.Get())) return false;

    AMHPivotWindController* Duplicate = World->SpawnActor<AMHPivotWindController>();
    if (!TestNotNull(TEXT("duplicate controller"), Duplicate)) return false;
    Duplicate->WindParameters = Fixture.Collection.Get();
    Duplicate->DirectionDegrees = 180.0f;
    bOk &= TestFalse(TEXT("duplicate starts rejected"), Duplicate->ApplySettings());

    // Actual engine path: destroys/recreates MPC instances, then registers
    // components without construction scripts or BeginPlay. No manual apply.
    World->ReInitWorld();
    bOk &= TestTrue(TEXT("world reinitialization replaces MPC instance"),
        World->GetParameterCollectionInstance(Fixture.Collection.Get()) != OriginalInstance.Get());
    FLinearColor Direction, Noise;
    bOk &= TestTrue(TEXT("direction exists after reinitialization"), Fixture.Read(World, DirectionSpeedParameter, Direction));
    bOk &= TestTrue(TEXT("noise exists after reinitialization"), Fixture.Read(World, NoiseParameter, Noise));
    bOk &= TestTrue(TEXT("reinitialization restores owner direction and speed"), Direction.Equals(ExpectedDirection));
    bOk &= TestTrue(TEXT("reinitialization restores owner noise"), Noise.Equals(ExpectedNoise));
    bOk &= TestFalse(TEXT("duplicate remains rejected after reinitialization"), Duplicate->ApplySettings());

    Controller->UnregisterAllComponents();
    Duplicate->UnregisterAllComponents();
    Controller->Enabled = false;
    Controller->NoiseStrength = 0.25f;
    // Register the rejected controller first; temporary unregistration must not
    // allow it to take over this world's owner.
    Duplicate->RegisterAllComponents();
    Fixture.Read(World, DirectionSpeedParameter, Direction);
    bOk &= TestTrue(TEXT("reversed registration order preserves ownership"), Direction.Equals(ExpectedDirection));
    Controller->RegisterAllComponents();
    Fixture.Read(World, DirectionSpeedParameter, Direction);
    Fixture.Read(World, NoiseParameter, Noise);
    bOk &= TestEqual(TEXT("registration republishes disabled setting"), Direction.A, 0.0f);
    bOk &= TestEqual(TEXT("registration republishes changed noise"), Noise.A, 0.25f);

    Controller->UnregisterAllComponents();
    World->bIsTearingDown = true;
    Controller->Enabled = true;
    Controller->NoiseStrength = 9.0f;
    Controller->RegisterAllComponents();
    FLinearColor TeardownDirection, TeardownNoise;
    Fixture.Read(World, DirectionSpeedParameter, TeardownDirection);
    Fixture.Read(World, NoiseParameter, TeardownNoise);
    bOk &= TestTrue(TEXT("registration during teardown does not write direction"), TeardownDirection.Equals(Direction));
    bOk &= TestTrue(TEXT("registration during teardown does not write noise"), TeardownNoise.Equals(Noise));
    World->bIsTearingDown = false;
    return bOk;
}
}
