#include "MHGoldenRoot.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Components/SceneComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite::Tests
{
namespace
{
constexpr int32 ReseedSyntheticLeafCount = 300;

struct FMHIncrementalReseedFixture
{
    FAutomationTestBase& Test;
    TArray<UObject*> Assets;
    UMHCompositeAsset* Root = nullptr;

    explicit FMHIncrementalReseedFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    ~FMHIncrementalReseedFixture()
    {
        for (UObject* Object : Assets)
        {
            if (!IsValid(Object)) continue;
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->MarkAsGarbage();
        }
    }

    UStaticMesh* AddMesh(const FString& Name)
    {
        UStaticMesh* Mesh = NewObject<UStaticMesh>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + Name)),
            FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Mesh);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = Name;
        Receipt->SourceRelativePath = Name + TEXT(".mesh.fbx");
        Receipt->SourceHash = TEXT("blake3-160:0123456789012345678901234567890123456789");
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);
        return Mesh;
    }

    UMHCompositeAsset* AddComposite(const FString& Name, const FMHCompositeDocument& Document)
    {
        TArray<uint8> Bytes;
        FString Error;
        if (!MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(Name + TEXT(" canonical write failed: ") + Error);
            return nullptr;
        }
        return AddComposite(Name, Bytes);
    }

    UMHCompositeAsset* AddComposite(const FString& Name, TConstArrayView<uint8> SourceBytes)
    {
        FMHCompositeDocument Document;
        FString Error;
        if (!MHParseCompositeV5(SourceBytes, Document, Error))
        {
            Test.AddError(Name + TEXT(" parse failed: ") + Error);
            return nullptr;
        }
        UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + Name)),
            FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        if (!MHApplyCompositeV5(*Asset, Document, Error))
        {
            Test.AddError(Name + TEXT(" apply failed: ") + Error);
            return nullptr;
        }
        TArray<uint8> AppliedBytes;
        if (!MHWriteCanonicalCompositeV5(Document, AppliedBytes, Error))
        {
            Test.AddError(Name + TEXT(" canonical write failed: ") + Error);
            return nullptr;
        }
        Asset->LogicalName = Name;
        Asset->SourceRelativePath = Name + TEXT(".composite");
        Asset->SourceHash = MHRawPayloadHash(SourceBytes);
        Asset->AppliedHash = MHRawPayloadHash(AppliedBytes);
        return Asset;
    }

    bool BuildSynthetic(const int32 LeafCount)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        const FString RootName = TEXT("s652_reseed_") + Suffix;
        UStaticMesh* MeshA = AddMesh(RootName + TEXT("_a"));
        UStaticMesh* MeshB = AddMesh(RootName + TEXT("_b"));
        if (MeshA == nullptr || MeshB == nullptr) return false;

        FMHCompositeDocument Document;
        FMHCompositeNode& Random = Document.Nodes.AddDefaulted_GetRef();
        Random.Kind = EMHCompositeNodeKind::Random;
        FMHCompositeOption& A = Random.Options.AddDefaulted_GetRef();
        A.Kind = EMHCompositeOptionKind::Mesh;
        A.Resource = MeshA->GetName();
        A.Weight = 1.0f;
        FMHCompositeOption& B = Random.Options.AddDefaulted_GetRef();
        B.Kind = EMHCompositeOptionKind::Mesh;
        B.Resource = MeshB->GetName();
        B.Weight = 1.0f;
        for (int32 Index = 1; Index < LeafCount; ++Index)
        {
            FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshA->GetName();
            Leaf.Transform.TranslationCm = FVector(20.0 * Index, 0.0, 0.0);
        }
        Root = AddComposite(RootName, Document);
        return Root != nullptr;
    }

    bool BuildGaz53()
    {
        FString GoldenRoot;
        if (!ResolveGoldenRoot(Test, GoldenRoot)) return false;
        AddMesh(TEXT("gaz53_b_body"));
        for (const TCHAR* Name : {
            TEXT("gaz53_b_random_cmp"), TEXT("gaz53_b_body_cmp"), TEXT("gaz53_body_bc_random_cmp")})
        {
            TArray<uint8> Bytes;
            const FString Path = FPaths::Combine(
                GoldenRoot, TEXT("v5/gaz53"), FString(Name) + TEXT(".composite"));
            if (!FFileHelper::LoadFileToArray(Bytes, *Path))
            {
                Test.AddError(TEXT("cannot read frozen GAZ-53 document: ") + Path);
                return false;
            }
            UMHCompositeAsset* Asset = AddComposite(Name, Bytes);
            if (Asset == nullptr) return false;
            if (FString(Name) == TEXT("gaz53_b_random_cmp")) Root = Asset;
        }
        for (const TCHAR* Name : {
            TEXT("gaz53_bread_b_cmp"), TEXT("gaz53_wooden_b_cmp"), TEXT("gaz53_wooden_c_cmp")})
        {
            FMHCompositeDocument Document;
            FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = TEXT("gaz53_b_body");
            if (AddComposite(Name, Document) == nullptr) return false;
        }
        return Root != nullptr;
    }

    bool FindChangingSeeds(int32& OutFirst, int32& OutSecond) const
    {
        const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
        if (Root == nullptr || Settings == nullptr) return false;
        FMHRandomSourceGraph Graph;
        TSet<FMHResourceKey> Dependencies;
        FString Error;
        if (!MHBuildAppliedCompositeGraph(*Root, *Settings, Graph, Dependencies, Error))
        {
            Test.AddError(TEXT("cannot build reseed graph: ") + Error);
            return false;
        }
        TMap<int32, int32> FirstSeedByOption;
        for (int32 Seed = 0; Seed < 4096; ++Seed)
        {
            FMHResolvedCompositePlan Plan;
            if (!MHResolveCompositePlan(Graph, Seed, 17, Plan, Error))
            {
                Test.AddError(TEXT("cannot resolve reseed plan: ") + Error);
                return false;
            }
            if (Plan.Decisions.IsEmpty()) continue;
            const int32 Option = Plan.Decisions[0].OptionIndex;
            FirstSeedByOption.FindOrAdd(Option, Seed);
            if (FirstSeedByOption.Num() >= 2)
            {
                TArray<int32> Seeds;
                FirstSeedByOption.GenerateValueArray(Seeds);
                OutFirst = Seeds[0];
                OutSecond = Seeds[1];
                return true;
            }
        }
        Test.AddError(TEXT("could not find two seeds selecting different options"));
        return false;
    }

    AMHCompositeActor* Spawn(UWorld& World, const int32 Seed) const
    {
        AMHCompositeActor* Actor = World.SpawnActor<AMHCompositeActor>();
        if (!Test.TestNotNull(TEXT("reseed placement"), Actor)) return nullptr;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetAppearanceSeed(17);
        Actor->SetSeed(Seed);
        Actor->SetCompositeAsset(Root);
        if (!Test.TestNotNull(*Actor->GetLastPlacementError(), Actor->GetResolvedPlan())) return nullptr;
        return Actor;
    }
};

FString ReseedMetricsLine(const TCHAR* Scenario, const double WallMilliseconds)
{
    const FMHPlacementStageMetrics Stages = MHGetPlacementStageMetrics();
    const FMHPlacementMutationMetrics Mutations = MHGetPlacementMutationMetrics();
    const FMHPlacementReseedMetrics Reseed = MHGetPlacementReseedMetrics();
    FString Result = FString::Printf(
        TEXT("S652_RESEED scenario=%s wall_ms=%.6f previous=%llu candidate=%llu stable=%llu changed=%llu added=%llu removed=%llu incremental=%llu fallback=%llu"),
        Scenario, WallMilliseconds, Reseed.PreviousLeaves, Reseed.CandidateLeaves,
        Reseed.StableLeafIdentities, Reseed.ChangedLeafIdentities,
        Reseed.AddedLeafIdentities, Reseed.RemovedLeafIdentities,
        Reseed.IncrementalApplied, Reseed.FullFallbacks);
    Result += FString::Printf(
        TEXT(" mutations[create=%llu,destroy=%llu,register=%llu,attach=%llu,set_mesh=%llu,world=%llu,appearance=%llu]"),
        Mutations.CreatedComponents, Mutations.DestroyedComponents,
        Mutations.RegisteredComponents, Mutations.Attachments,
        Mutations.StaticMeshAssignments, Mutations.WorldTransformUpdates,
        Mutations.AppearanceUpdates);
    for (int32 Index = 0; Index < FMHPlacementStageMetrics::StageCount; ++Index)
    {
        const EMHPlacementStage Stage = static_cast<EMHPlacementStage>(Index);
        const FMHPlacementStageMetric& Metric = Stages.Get(Stage);
        Result += FString::Printf(TEXT(" %s[calls=%llu,inclusive_ms=%.6f,exclusive_ms=%.6f]"),
            MHPlacementStageLabel(Stage), Metric.Calls,
            FPlatformTime::ToMilliseconds64(Metric.InclusiveCycles),
            FPlatformTime::ToMilliseconds64(Metric.ExclusiveCycles));
    }
    return Result;
}

void ResetReseedMeasurements()
{
    MHResetPlacementStageMetrics();
    MHResetPlacementMutationMetrics();
    MHResetPlacementReseedMetrics();
}

void AccumulateMutationMetrics(
    FMHPlacementMutationMetrics& Total, const FMHPlacementMutationMetrics& Sample)
{
    Total.CreatedComponents += Sample.CreatedComponents;
    Total.DestroyedComponents += Sample.DestroyedComponents;
    Total.RegisteredComponents += Sample.RegisteredComponents;
    Total.Attachments += Sample.Attachments;
    Total.StaticMeshAssignments += Sample.StaticMeshAssignments;
    Total.WorldTransformUpdates += Sample.WorldTransformUpdates;
    Total.AppearanceUpdates += Sample.AppearanceUpdates;
}

uint64 MaterializationMutationUnits(const FMHPlacementMutationMetrics& Metrics)
{
    return Metrics.CreatedComponents + Metrics.DestroyedComponents + Metrics.RegisteredComponents +
        Metrics.Attachments + Metrics.StaticMeshAssignments + Metrics.WorldTransformUpdates +
        Metrics.AppearanceUpdates;
}

uint64 MaterializationStageCycles(const FMHPlacementStageMetrics& Metrics)
{
    return Metrics.Get(EMHPlacementStage::CompilePlacement).InclusiveCycles +
        Metrics.Get(EMHPlacementStage::RegisterComponents).InclusiveCycles +
        Metrics.Get(EMHPlacementStage::DestroyRetiredComponents).InclusiveCycles;
}

double TimedReseed(AMHCompositeActor& Actor, const int32 Seed)
{
    const double Start = FPlatformTime::Seconds();
    Actor.SetSeed(Seed);
    return (FPlatformTime::Seconds() - Start) * 1000.0;
}

bool CompareMaterializedViews(
    FAutomationTestBase& Test, const AMHCompositeActor& Incremental, const AMHCompositeActor& Full)
{
    const FMHResolvedCompositePlan* LeftPlan = Incremental.GetResolvedPlan();
    const FMHResolvedCompositePlan* RightPlan = Full.GetResolvedPlan();
    if (!Test.TestNotNull(TEXT("incremental plan"), LeftPlan) ||
        !Test.TestNotNull(TEXT("full plan"), RightPlan)) return false;
    bool bPassed = true;
    bPassed &= Test.TestEqual(TEXT("resolved signature parity"), LeftPlan->ResolvedSignature, RightPlan->ResolvedSignature);
    bPassed &= Test.TestEqual(TEXT("appearance signature parity"),
        LeftPlan->Appearance.AppearanceSignature, RightPlan->Appearance.AppearanceSignature);
    bPassed &= Test.TestEqual(TEXT("placement signature parity"), LeftPlan->PlacementSignature, RightPlan->PlacementSignature);
    bPassed &= Test.TestTrue(TEXT("resolved preimage parity"), LeftPlan->SignaturePreimage == RightPlan->SignaturePreimage);
    bPassed &= Test.TestTrue(TEXT("appearance preimage parity"),
        LeftPlan->Appearance.SignaturePreimage == RightPlan->Appearance.SignaturePreimage);
    bPassed &= Test.TestEqual(TEXT("leaf plan count parity"), LeftPlan->Leaves.Num(), RightPlan->Leaves.Num());
    const TArray<TObjectPtr<USceneComponent>>& Left = Incremental.GetLeafPlacementComponents();
    const TArray<TObjectPtr<USceneComponent>>& Right = Full.GetLeafPlacementComponents();
    const TArray<FMHCompositeLeafMaterialization>& LeftRows = Incremental.GetLeafMaterializations();
    const TArray<FMHCompositeLeafMaterialization>& RightRows = Full.GetLeafMaterializations();
    bPassed &= Test.TestEqual(TEXT("materialized leaf count parity"), Left.Num(), Right.Num());
    bPassed &= Test.TestEqual(TEXT("materialization mapping count parity"),
        LeftRows.Num(), RightRows.Num());
    const int32 Count = FMath::Min(
        FMath::Min(LeftPlan->Leaves.Num(), RightPlan->Leaves.Num()),
        FMath::Min(Left.Num(), Right.Num()));
    const int32 Base = GetDefault<UMHCompositeSettings>()->AppearanceCustomDataBaseIndex;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const FMHResolvedCompositeLeaf& LeftLeaf = LeftPlan->Leaves[Index];
        const FMHResolvedCompositeLeaf& RightLeaf = RightPlan->Leaves[Index];
        bPassed &= Test.TestTrue(TEXT("leaf identity parity"),
            LeftLeaf.Origin == RightLeaf.Origin && LeftLeaf.Kind == RightLeaf.Kind &&
            LeftLeaf.Resource == RightLeaf.Resource);
        bPassed &= Test.TestTrue(TEXT("plan world matrix byte parity"),
            FMemory::Memcmp(&LeftLeaf.WorldMatrix, &RightLeaf.WorldMatrix, sizeof(FMatrix)) == 0);
        if (!IsValid(Left[Index]) || !IsValid(Right[Index]) ||
            !LeftRows.IsValidIndex(Index) || !RightRows.IsValidIndex(Index))
        {
            bPassed = false;
            continue;
        }
        bPassed &= Test.TestEqual(TEXT("component class parity"), Left[Index]->GetClass(), Right[Index]->GetClass());
        bPassed &= Test.TestTrue(TEXT("component tag parity"), Left[Index]->ComponentTags == Right[Index]->ComponentTags);
        FTransform LeftTransform;
        FTransform RightTransform;
        const UInstancedStaticMeshComponent* LeftBucket =
            Cast<UInstancedStaticMeshComponent>(Left[Index]);
        const UInstancedStaticMeshComponent* RightBucket =
            Cast<UInstancedStaticMeshComponent>(Right[Index]);
        if (LeftRows[Index].IsInstanced() || RightRows[Index].IsInstanced())
        {
            if (LeftBucket == nullptr || RightBucket == nullptr ||
                !LeftRows[Index].IsInstanced() || !RightRows[Index].IsInstanced() ||
                !LeftBucket->GetInstanceTransform(
                    LeftRows[Index].InstanceIndex, LeftTransform, false) ||
                !RightBucket->GetInstanceTransform(
                    RightRows[Index].InstanceIndex, RightTransform, false))
            {
                bPassed = false;
                continue;
            }
        }
        else
        {
            LeftTransform = Left[Index]->GetComponentTransform();
            RightTransform = Right[Index]->GetComponentTransform();
        }
        const FMatrix LeftWorld = LeftTransform.ToMatrixWithScale();
        const FMatrix RightWorld = RightTransform.ToMatrixWithScale();
        bPassed &= Test.TestTrue(TEXT("materialized world matrix byte parity"),
            FMemory::Memcmp(&LeftWorld, &RightWorld, sizeof(FMatrix)) == 0);
        const UStaticMeshComponent* LeftMesh = Cast<UStaticMeshComponent>(Left[Index]);
        const UStaticMeshComponent* RightMesh = Cast<UStaticMeshComponent>(Right[Index]);
        if (LeftMesh != nullptr || RightMesh != nullptr)
        {
            if (LeftMesh == nullptr || RightMesh == nullptr)
            {
                bPassed = false;
                continue;
            }
            bPassed &= Test.TestEqual(TEXT("mesh endpoint identity parity"),
                LeftMesh->GetStaticMesh(), RightMesh->GetStaticMesh());
            for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
            {
                if (LeftBucket != nullptr && RightBucket != nullptr)
                {
                    const int32 LeftDataIndex = LeftRows[Index].InstanceIndex *
                        LeftBucket->NumCustomDataFloats + Base + Channel;
                    const int32 RightDataIndex = RightRows[Index].InstanceIndex *
                        RightBucket->NumCustomDataFloats + Base + Channel;
                    bPassed &= Test.TestTrue(TEXT("appearance CPD parity"),
                        LeftBucket->PerInstanceSMCustomData.IsValidIndex(LeftDataIndex) &&
                        RightBucket->PerInstanceSMCustomData.IsValidIndex(RightDataIndex) &&
                        LeftBucket->PerInstanceSMCustomData[LeftDataIndex] ==
                            RightBucket->PerInstanceSMCustomData[RightDataIndex]);
                }
                else
                {
                    const TArray<float>& LeftData = LeftMesh->GetCustomPrimitiveData().Data;
                    const TArray<float>& RightData = RightMesh->GetCustomPrimitiveData().Data;
                    const int32 DataIndex = Base + Channel;
                    bPassed &= Test.TestTrue(TEXT("appearance CPD parity"),
                        LeftData.IsValidIndex(DataIndex) && RightData.IsValidIndex(DataIndex) &&
                        LeftData[DataIndex] == RightData[DataIndex]);
                }
            }
        }
    }
    return bPassed;
}

bool SetSeedPropertyWithoutRebuild(FAutomationTestBase& Test, AMHCompositeActor& Actor, const int32 Seed)
{
    FIntProperty* Property = CastField<FIntProperty>(
        AMHCompositeActor::StaticClass()->FindPropertyByName(TEXT("Seed")));
    if (!Test.TestNotNull(TEXT("reflected layout seed"), Property)) return false;
    Property->SetPropertyValue_InContainer(&Actor, Seed);
    return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHIncrementalReseedSyntheticInstrumentationTest,
    "Mimir.V5.Composite.IncrementalReseed.InstrumentationSynthetic300",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHIncrementalReseedSyntheticInstrumentationTest::RunTest(const FString& Parameters)
{
    FMHIncrementalReseedFixture Fixture(*this);
    if (!Fixture.BuildSynthetic(ReseedSyntheticLeafCount)) return false;
    int32 FirstSeed = 0;
    int32 SecondSeed = 0;
    if (!Fixture.FindChangingSeeds(FirstSeed, SecondSeed)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = Fixture.Spawn(*World, FirstSeed);
    if (Actor == nullptr)
    {
        World->DestroyWorld(false);
        return false;
    }
    ResetReseedMeasurements();
    const double Wall = TimedReseed(*Actor, SecondSeed);
    AddInfo(ReseedMetricsLine(TEXT("synthetic300_k1"), Wall));
    const FMHPlacementReseedMetrics Metrics = MHGetPlacementReseedMetrics();
    bool bPassed = TestEqual(TEXT("synthetic leaf count"), Actor->GetLeafPlacementComponents().Num(), ReseedSyntheticLeafCount);
    bPassed &= TestEqual(TEXT("selected option introduces one leaf path"), Metrics.AddedLeafIdentities, 1ull);
    bPassed &= TestEqual(TEXT("previous option removes one leaf path"), Metrics.RemovedLeafIdentities, 1ull);
    bPassed &= TestEqual(TEXT("all other leaf identities stay stable"),
        Metrics.StableLeafIdentities, static_cast<uint64>(ReseedSyntheticLeafCount - 1));
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHIncrementalReseedGazInstrumentationTest,
    "Mimir.V5.Composite.IncrementalReseed.InstrumentationGaz53",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHIncrementalReseedGazInstrumentationTest::RunTest(const FString& Parameters)
{
    FMHIncrementalReseedFixture Fixture(*this);
    if (!Fixture.BuildGaz53()) return false;
    int32 FirstSeed = 0;
    int32 SecondSeed = 0;
    if (!Fixture.FindChangingSeeds(FirstSeed, SecondSeed)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = Fixture.Spawn(*World, FirstSeed);
    if (Actor == nullptr)
    {
        World->DestroyWorld(false);
        return false;
    }
    ResetReseedMeasurements();
    const double Wall = TimedReseed(*Actor, SecondSeed);
    AddInfo(ReseedMetricsLine(TEXT("gaz53_frozen_reseed"), Wall));
    const FMHPlacementReseedMetrics Metrics = MHGetPlacementReseedMetrics();
    bool bPassed = TestEqual(TEXT("GAZ reseed is measured once"), Metrics.Attempts, 1ull);
    bPassed &= TestTrue(TEXT("GAZ reseed has a materialized plan"), Actor->GetResolvedPlan() != nullptr);
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHIncrementalReseedSelectiveMutationTest,
    "Mimir.V5.Composite.IncrementalReseed.SelectiveMutationAndIdentity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHIncrementalReseedSelectiveMutationTest::RunTest(const FString& Parameters)
{
    FMHIncrementalReseedFixture Fixture(*this);
    if (!Fixture.BuildSynthetic(ReseedSyntheticLeafCount)) return false;
    int32 FirstSeed = 0;
    int32 SecondSeed = 0;
    if (!Fixture.FindChangingSeeds(FirstSeed, SecondSeed)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = Fixture.Spawn(*World, FirstSeed);
    if (Actor == nullptr)
    {
        World->DestroyWorld(false);
        return false;
    }
    const FMHResolvedCompositePlan BeforePlan = *Actor->GetResolvedPlan();
    TMap<FString, TObjectPtr<USceneComponent>> BeforeByPath;
    for (int32 Index = 0; Index < BeforePlan.Leaves.Num(); ++Index)
        BeforeByPath.Add(BeforePlan.Leaves[Index].Origin, Actor->GetLeafPlacementComponents()[Index]);

    ResetReseedMeasurements();
    const double Wall = TimedReseed(*Actor, SecondSeed);
    AddInfo(ReseedMetricsLine(TEXT("selective_mutation"), Wall));
    const FMHPlacementReseedMetrics Reseed = MHGetPlacementReseedMetrics();
    const FMHPlacementMutationMetrics Mutations = MHGetPlacementMutationMetrics();
    const FMHResolvedCompositePlan* AfterPlan = Actor->GetResolvedPlan();
    bool bPassed = TestNotNull(TEXT("reseeded plan"), AfterPlan);
    if (AfterPlan != nullptr)
    {
        for (int32 Index = 0; Index < AfterPlan->Leaves.Num(); ++Index)
        {
            const FMHResolvedCompositeLeaf& Leaf = AfterPlan->Leaves[Index];
            const FMHResolvedCompositeLeaf* BeforeLeaf = BeforePlan.Leaves.FindByPredicate(
                [&Leaf](const FMHResolvedCompositeLeaf& Candidate) { return Candidate.Origin == Leaf.Origin; });
            if (BeforeLeaf != nullptr && BeforeLeaf->Kind == Leaf.Kind && BeforeLeaf->Resource == Leaf.Resource)
            {
                bPassed &= TestEqual(TEXT("stable leaf keeps exact component object"),
                    Actor->GetLeafPlacementComponents()[Index], BeforeByPath.FindRef(Leaf.Origin));
            }
        }
    }
    const uint64 K = Reseed.ChangedLeafIdentities +
        FMath::Max(Reseed.AddedLeafIdentities, Reseed.RemovedLeafIdentities);
    const uint64 ExpensiveTouches = Mutations.CreatedComponents + Mutations.DestroyedComponents +
        Mutations.RegisteredComponents + Mutations.StaticMeshAssignments;
    bPassed &= TestEqual(TEXT("incremental reseed path is used"), Reseed.IncrementalApplied, 1ull);
    bPassed &= TestTrue(TEXT("expensive component work scales with changed leaves"), ExpensiveTouches <= 6 * K);
    bPassed &= TestTrue(TEXT("attachment work scales with changed leaves"), Mutations.Attachments <= K);
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHIncrementalReseedParityTest,
    "Mimir.V5.Composite.IncrementalReseed.FullRebuildParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHIncrementalReseedParityTest::RunTest(const FString& Parameters)
{
    FMHIncrementalReseedFixture Fixture(*this);
    if (!Fixture.BuildSynthetic(ReseedSyntheticLeafCount)) return false;
    int32 FirstSeed = 0;
    int32 SecondSeed = 0;
    if (!Fixture.FindChangingSeeds(FirstSeed, SecondSeed)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Incremental = Fixture.Spawn(*World, FirstSeed);
    AMHCompositeActor* Full = Fixture.Spawn(*World, SecondSeed);
    if (Incremental == nullptr || Full == nullptr)
    {
        World->DestroyWorld(false);
        return false;
    }
    Incremental->SetSeed(SecondSeed);
    const bool bPassed = CompareMaterializedViews(*this, *Incremental, *Full);
    Incremental->Destroy();
    Full->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHIncrementalReseedDesyncFallbackTest,
    "Mimir.V5.Composite.IncrementalReseed.DesyncFallsBackToFull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHIncrementalReseedDesyncFallbackTest::RunTest(const FString& Parameters)
{
    FMHIncrementalReseedFixture Fixture(*this);
    if (!Fixture.BuildSynthetic(ReseedSyntheticLeafCount)) return false;
    int32 FirstSeed = 0;
    int32 SecondSeed = 0;
    if (!Fixture.FindChangingSeeds(FirstSeed, SecondSeed)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = Fixture.Spawn(*World, FirstSeed);
    if (Actor == nullptr)
    {
        World->DestroyWorld(false);
        return false;
    }
    FArrayProperty* Leaves = CastField<FArrayProperty>(
        AMHCompositeActor::StaticClass()->FindPropertyByName(TEXT("LeafPlacementComponents")));
    if (!TestNotNull(TEXT("reflected leaf view"), Leaves))
    {
        Actor->Destroy();
        World->DestroyWorld(false);
        return false;
    }
    FScriptArrayHelper Helper(Leaves, Leaves->ContainerPtrToValuePtr<void>(Actor));
    if (!TestTrue(TEXT("leaf view can be desynchronized"), Helper.Num() > 0))
    {
        Actor->Destroy();
        World->DestroyWorld(false);
        return false;
    }
    Helper.RemoveValues(Helper.Num() - 1, 1);
    ResetReseedMeasurements();
    Actor->SetSeed(SecondSeed);
    AddInfo(ReseedMetricsLine(TEXT("forced_desync"), 0.0));
    const FMHPlacementReseedMetrics Metrics = MHGetPlacementReseedMetrics();
    bool bPassed = TestEqual(TEXT("desync rejects incremental path"), Metrics.IncrementalApplied, 0ull);
    bPassed &= TestEqual(TEXT("desync records one full fallback"), Metrics.FullFallbacks, 1ull);
    bPassed &= TestNotNull(*Actor->GetLastPlacementError(), Actor->GetResolvedPlan());
    if (Actor->GetResolvedPlan() != nullptr)
        bPassed &= TestEqual(TEXT("fallback restores complete leaf view"),
            Actor->GetLeafPlacementComponents().Num(), Actor->GetResolvedPlan()->Leaves.Num());

    UStaticMeshComponent* StableMesh = Actor->GetLeafPlacementComponents().Num() > 1
        ? Cast<UStaticMeshComponent>(Actor->GetLeafPlacementComponents()[1]) : nullptr;
    UStaticMesh* ExpectedMesh = StableMesh != nullptr ? StableMesh->GetStaticMesh() : nullptr;
    UStaticMesh* WrongMesh = nullptr;
    for (UObject* Object : Fixture.Assets)
    {
        UStaticMesh* Mesh = Cast<UStaticMesh>(Object);
        if (Mesh != nullptr && Mesh != ExpectedMesh)
        {
            WrongMesh = Mesh;
            break;
        }
    }
    if (!TestNotNull(TEXT("stable mesh component"), StableMesh) ||
        !TestNotNull(TEXT("alternate mesh endpoint"), WrongMesh))
    {
        Actor->Destroy();
        World->DestroyWorld(false);
        return false;
    }
    StableMesh->SetStaticMesh(WrongMesh);
    ResetReseedMeasurements();
    Actor->SetSeed(FirstSeed);
    const FMHPlacementReseedMetrics EndpointMetrics = MHGetPlacementReseedMetrics();
    bPassed &= TestEqual(TEXT("endpoint desync rejects incremental path"),
        EndpointMetrics.IncrementalApplied, 0ull);
    bPassed &= TestEqual(TEXT("endpoint desync records one full fallback"),
        EndpointMetrics.FullFallbacks, 1ull);
    UStaticMeshComponent* RepairedMesh = Actor->GetLeafPlacementComponents().Num() > 1
        ? Cast<UStaticMeshComponent>(Actor->GetLeafPlacementComponents()[1]) : nullptr;
    bPassed &= TestNotNull(TEXT("full fallback restores a mesh component"), RepairedMesh);
    if (RepairedMesh != nullptr)
        bPassed &= TestEqual<UStaticMesh*>(TEXT("full fallback restores the endpoint"),
            RepairedMesh->GetStaticMesh().Get(), ExpectedMesh);
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHIncrementalReseedWallRatioTest,
    "Mimir.V5.Composite.IncrementalReseed.WallRatioSynthetic300",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHIncrementalReseedWallRatioTest::RunTest(const FString& Parameters)
{
    FMHIncrementalReseedFixture Fixture(*this);
    if (!Fixture.BuildSynthetic(ReseedSyntheticLeafCount)) return false;
    int32 FirstSeed = 0;
    int32 SecondSeed = 0;
    if (!Fixture.FindChangingSeeds(FirstSeed, SecondSeed)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Incremental = Fixture.Spawn(*World, FirstSeed);
    AMHCompositeActor* Full = Fixture.Spawn(*World, FirstSeed);
    if (Incremental == nullptr || Full == nullptr)
    {
        World->DestroyWorld(false);
        return false;
    }

    constexpr int32 Iterations = 5;
    double IncrementalMilliseconds = 0.0;
    double FullMilliseconds = 0.0;
    uint64 IncrementalStageCycles = 0;
    uint64 FullStageCycles = 0;
    uint64 IncrementalResolverCycles = 0;
    uint64 FullResolverCycles = 0;
    FMHPlacementMutationMetrics IncrementalMutations;
    FMHPlacementMutationMetrics FullMutations;
    int32 NextSeed = SecondSeed;
    for (int32 Index = 0; Index < Iterations; ++Index)
    {
        ResetReseedMeasurements();
        IncrementalMilliseconds += TimedReseed(*Incremental, NextSeed);
        const FMHPlacementStageMetrics IncrementalStages = MHGetPlacementStageMetrics();
        IncrementalStageCycles += MaterializationStageCycles(IncrementalStages);
        IncrementalResolverCycles +=
            IncrementalStages.Get(EMHPlacementStage::ResolveCompositePlan).InclusiveCycles;
        AccumulateMutationMetrics(IncrementalMutations, MHGetPlacementMutationMetrics());
        if (!SetSeedPropertyWithoutRebuild(*this, *Full, NextSeed))
        {
            Incremental->Destroy();
            Full->Destroy();
            World->DestroyWorld(false);
            return false;
        }
        ResetReseedMeasurements();
        const double Start = FPlatformTime::Seconds();
        Full->RebuildComposite();
        FullMilliseconds += (FPlatformTime::Seconds() - Start) * 1000.0;
        const FMHPlacementStageMetrics FullStages = MHGetPlacementStageMetrics();
        FullStageCycles += MaterializationStageCycles(FullStages);
        FullResolverCycles += FullStages.Get(EMHPlacementStage::ResolveCompositePlan).InclusiveCycles;
        AccumulateMutationMetrics(FullMutations, MHGetPlacementMutationMetrics());
        NextSeed = NextSeed == FirstSeed ? SecondSeed : FirstSeed;
    }
    const uint64 IncrementalUnits = MaterializationMutationUnits(IncrementalMutations);
    const uint64 FullUnits = MaterializationMutationUnits(FullMutations);
    const double MaterializationRatio = FullUnits > 0
        ? static_cast<double>(IncrementalUnits) / static_cast<double>(FullUnits) : 1.0;
    const double IncrementalStageMilliseconds = FPlatformTime::ToMilliseconds64(IncrementalStageCycles);
    const double FullStageMilliseconds = FPlatformTime::ToMilliseconds64(FullStageCycles);
    const double IncrementalResolverMilliseconds = FPlatformTime::ToMilliseconds64(IncrementalResolverCycles);
    const double FullResolverMilliseconds = FPlatformTime::ToMilliseconds64(FullResolverCycles);
    const double StageRatio = FullStageMilliseconds > 0.0
        ? IncrementalStageMilliseconds / FullStageMilliseconds : 1.0;
    const double WallRatio = FullMilliseconds > 0.0 ? IncrementalMilliseconds / FullMilliseconds : 1.0;
    AddInfo(FString::Printf(
        TEXT("S652_MATERIALIZATION synthetic300 iterations=%d incremental_units=%llu full_units=%llu ratio=%.6f")
        TEXT(" incremental[create=%llu,destroy=%llu,register=%llu,attach=%llu,set_mesh=%llu,world=%llu,appearance=%llu]")
        TEXT(" full[create=%llu,destroy=%llu,register=%llu,attach=%llu,set_mesh=%llu,world=%llu,appearance=%llu]"),
        Iterations, IncrementalUnits, FullUnits, MaterializationRatio,
        IncrementalMutations.CreatedComponents, IncrementalMutations.DestroyedComponents,
        IncrementalMutations.RegisteredComponents, IncrementalMutations.Attachments,
        IncrementalMutations.StaticMeshAssignments, IncrementalMutations.WorldTransformUpdates,
        IncrementalMutations.AppearanceUpdates,
        FullMutations.CreatedComponents, FullMutations.DestroyedComponents,
        FullMutations.RegisteredComponents, FullMutations.Attachments,
        FullMutations.StaticMeshAssignments, FullMutations.WorldTransformUpdates,
        FullMutations.AppearanceUpdates));
    AddInfo(FString::Printf(
        TEXT("S652_MATERIALIZATION_STAGE_INFO synthetic300 iterations=%d incremental_ms=%.6f full_ms=%.6f ratio=%.6f"),
        Iterations, IncrementalStageMilliseconds, FullStageMilliseconds, StageRatio));
    AddInfo(FString::Printf(
        TEXT("S652_WALL_INFO synthetic300 iterations=%d incremental_ms=%.6f full_ms=%.6f ratio=%.6f")
        TEXT(" incremental_resolve_ms=%.6f full_resolve_ms=%.6f"),
        Iterations, IncrementalMilliseconds, FullMilliseconds, WallRatio,
        IncrementalResolverMilliseconds, FullResolverMilliseconds));
    bool bPassed = TestTrue(
        TEXT("incremental reseed materialization work is at most fifteen percent of full rebuild"),
        MaterializationRatio <= 0.15);
    bPassed &= CompareMaterializedViews(*this, *Incremental, *Full);
    Incremental->Destroy();
    Full->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}
} // namespace UE::MimirComposite::Tests
