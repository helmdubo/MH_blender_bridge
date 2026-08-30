#include "MHGoldenRoot.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "IO/IoHash.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"
#include "UI/MHCompositeOutlinerModel.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

void OutlinerAddRawHash(FMHRandomSourceGraph& Graph, const FString& ResourceKey)
{
    const FTCHARToUTF8 Payload(*ResourceKey, ResourceKey.Len());
    const FIoHash Hash = FIoHash::HashBuffer(Payload.Get(), static_cast<uint64>(Payload.Length()));
    Graph.RawHashes.Add(ResourceKey, TEXT("blake3-160:") + LexToString(Hash).ToLower());
}

EMHRandomSemanticKind OutlinerNodeKind(const EMHCompositeNodeKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeNodeKind::Mesh: return EMHRandomSemanticKind::Mesh;
    case EMHCompositeNodeKind::Actor: return EMHRandomSemanticKind::Actor;
    case EMHCompositeNodeKind::Composite: return EMHRandomSemanticKind::Composite;
    case EMHCompositeNodeKind::Random: return EMHRandomSemanticKind::Random;
    case EMHCompositeNodeKind::GameObj: return EMHRandomSemanticKind::GameObj;
    default: return EMHRandomSemanticKind::Group;
    }
}

EMHRandomSemanticKind OutlinerOptionKind(const EMHCompositeOptionKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeOptionKind::Mesh: return EMHRandomSemanticKind::Mesh;
    case EMHCompositeOptionKind::Actor: return EMHRandomSemanticKind::Actor;
    case EMHCompositeOptionKind::Composite: return EMHRandomSemanticKind::Composite;
    case EMHCompositeOptionKind::GameObj: return EMHRandomSemanticKind::GameObj;
    default: return EMHRandomSemanticKind::Empty;
    }
}

FMHRandomNode OutlinerConvertNode(const FMHCompositeNode& Node)
{
    FMHRandomNode Result;
    Result.Kind = OutlinerNodeKind(Node.Kind);
    Result.Resource = Node.Resource;
    Result.DisplayName = Node.Name;
    Result.Profile = Node.Profile;
    Result.bAppearanceSeedBoundary = Node.bAppearanceSeedBoundary;
    Result.Transform.TranslationCm = FVector3f(Node.Transform.TranslationCm);
    Result.Transform.RotationQuat = FQuat4f(Node.Transform.RotationQuat);
    Result.Transform.Scale = FVector3f(Node.Transform.Scale);
    for (const FMHCompositeOption& Option : Node.Options)
        Result.Options.Add({OutlinerOptionKind(Option.Kind), Option.Resource, Option.Weight});
    for (const FMHCompositeNode& Child : Node.Children)
        Result.Children.Add(OutlinerConvertNode(Child));
    return Result;
}

struct FOutlinerFixture
{
    FAutomationTestBase& Test;
    TMap<FString, TObjectPtr<UMHCompositeAsset>> Assets;
    FMHRandomSourceGraph Graph;

    explicit FOutlinerFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    UMHCompositeAsset* AddComposite(const FString& Name, const FMHCompositeDocument& Document)
    {
        UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
            GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), UMHCompositeAsset::StaticClass(), FName(*Name)));
        FString Error;
        if (!MHApplyCompositeV5(*Asset, Document, Error))
        {
            Test.AddError(Name + TEXT(" apply failed: ") + Error);
            return nullptr;
        }
        Asset->LogicalName = Name;
        Asset->SourceRelativePath = Name + TEXT(".composite");
        Assets.Add(Name, Asset);

        FMHRandomComposite Definition;
        Definition.Name = Name;
        for (const FMHCompositeNode& Node : Document.Nodes)
            Definition.Nodes.Add(OutlinerConvertNode(Node));
        Graph.Composites.Add(Name, MoveTemp(Definition));
        OutlinerAddRawHash(Graph, TEXT("composite:") + Name);
        return Asset;
    }

    bool AddGolden(const FString& GoldenRoot, const FString& Name)
    {
        TArray<uint8> Bytes;
        const FString Path = FPaths::Combine(
            GoldenRoot, TEXT("v5/gaz53"), Name + TEXT(".composite"));
        if (!FFileHelper::LoadFileToArray(Bytes, *Path))
        {
            Test.AddError(TEXT("cannot read frozen GAZ-53 document: ") + Path);
            return false;
        }
        FMHCompositeDocument Document;
        FString Error;
        if (!MHParseCompositeV5(Bytes, Document, Error))
        {
            Test.AddError(Name + TEXT(" parse failed: ") + Error);
            return false;
        }
        return AddComposite(Name, Document) != nullptr;
    }

    bool BuildGaz53()
    {
        FString GoldenRoot;
        if (!ResolveGoldenRoot(Test, GoldenRoot)) return false;
        Graph.RootComposite = TEXT("gaz53_b_random_cmp");
        for (const TCHAR* Name : {
            TEXT("gaz53_b_random_cmp"), TEXT("gaz53_b_body_cmp"),
            TEXT("gaz53_body_bc_random_cmp")})
        {
            if (!AddGolden(GoldenRoot, Name)) return false;
        }
        for (const TCHAR* Name : {
            TEXT("gaz53_bread_b_cmp"), TEXT("gaz53_wooden_b_cmp"),
            TEXT("gaz53_wooden_c_cmp")})
        {
            FMHCompositeDocument Document;
            FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = TEXT("gaz53_b_body");
            if (AddComposite(Name, Document) == nullptr) return false;
        }
        OutlinerAddRawHash(Graph, TEXT("static_mesh:gaz53_b_body"));
        return true;
    }

    bool Resolve(const int32 Seed, FMHResolvedCompositePlan& OutPlan)
    {
        FString Error;
        if (!MHResolveCompositePlan(Graph, Seed, 17, OutPlan, Error))
        {
            Test.AddError(TEXT("outliner fixture did not resolve: ") + Error);
            return false;
        }
        return true;
    }

    FMHCompositeOutlinerModel MakeModel()
    {
        return FMHCompositeOutlinerModel(
            [this](const EMHRandomSemanticKind Kind, const FString& Resource, FString& OutError) -> UObject*
            {
                if (Kind == EMHRandomSemanticKind::Composite)
                {
                    if (const TObjectPtr<UMHCompositeAsset>* Found = Assets.Find(Resource))
                        return Found->Get();
                }
                OutError = TEXT("unresolved test resource: ") + Resource;
                return nullptr;
            });
    }
};

TSharedPtr<FMHCompositeOutlinerItem> FindOption(
    const TSharedPtr<FMHCompositeOutlinerItem>& Random,
    const int32 OptionIndex)
{
    if (!Random.IsValid()) return nullptr;
    for (const TSharedPtr<FMHCompositeOutlinerItem>& Child : Random->Children)
        if (Child.IsValid() && Child->IsOption() && Child->OptionIndex == OptionIndex)
            return Child;
    return nullptr;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeOutlinerGaz53Test,
    "Mimir.V5.Composite.OutlinerModel.Gaz53TreeAndOverlay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeOutlinerGaz53Test::RunTest(const FString& Parameters)
{
    FOutlinerFixture Fixture(*this);
    if (!Fixture.BuildGaz53()) return false;
    FMHResolvedCompositePlan Plan;
    if (!Fixture.Resolve(42, Plan)) return false;

    FMHCompositeOutlinerModel Model = Fixture.MakeModel();
    UMHCompositeAsset* Root = Fixture.Assets.FindRef(TEXT("gaz53_b_random_cmp"));
    bool bPassed = TestTrue(TEXT("GAZ model builds"), Root != nullptr && Model.Build(*Root, &Plan));
    bPassed &= TestEqual(TEXT("root mirrors two source nodes"), Model.GetRoots().Num(), 2);
    if (Model.GetRoots().Num() != 2) return false;
    bPassed &= TestEqual(TEXT("first root path"), Model.GetRoots()[0]->NodePath,
        FString(TEXT("gaz53_b_random_cmp:nodes[0]")));
    bPassed &= TestEqual(TEXT("second root path"), Model.GetRoots()[1]->NodePath,
        FString(TEXT("gaz53_b_random_cmp:nodes[1]")));

    bPassed &= TestTrue(TEXT("body composite expands lazily"), Model.ExpandItem(Model.GetRoots()[0]));
    if (!Model.GetRoots()[0]->Children.IsEmpty())
    {
        const TSharedPtr<FMHCompositeOutlinerItem> Group = Model.GetRoots()[0]->Children[0];
        bPassed &= TestEqual(TEXT("nested group path"), Group->NodePath,
            FString(TEXT("gaz53_b_random_cmp:nodes[0]>gaz53_b_body_cmp:nodes[0]")));
        if (!Group->Children.IsEmpty())
        {
            const TSharedPtr<FMHCompositeOutlinerItem> Mesh = Group->Children[0];
            bPassed &= TestTrue(TEXT("nested mesh has resolved overlay"), Mesh->bHasResolvedOverlay);
            bPassed &= TestEqual(TEXT("nested mesh path"), Mesh->NodePath,
                FString(TEXT("gaz53_b_random_cmp:nodes[0]>gaz53_b_body_cmp:nodes[0]/children[0]")));
            bPassed &= TestTrue(TEXT("sampled TRS is exposed"), Mesh->SampledLocalTrs.IsSet());
            if (Mesh->SampledLocalTrs.IsSet())
                bPassed &= TestEqual(TEXT("sampled local X"), Mesh->SampledLocalTrs->TranslationCm.X, 25.0f);
        }
        else
        {
            AddError(TEXT("nested GAZ group has no mesh child"));
            bPassed = false;
        }
    }
    else
    {
        AddError(TEXT("body composite did not expose its nested root"));
        bPassed = false;
    }

    bPassed &= TestTrue(TEXT("random composite expands lazily"), Model.ExpandItem(Model.GetRoots()[1]));
    const TSharedPtr<FMHCompositeOutlinerItem> Random = Model.FindByNodePath(
        TEXT("gaz53_b_random_cmp:nodes[1]>gaz53_body_bc_random_cmp:nodes[0]"));
    bPassed &= TestTrue(TEXT("nested random is present"), Random.IsValid());
    if (Random.IsValid())
    {
        bPassed &= TestEqual(TEXT("random lists every source option"), Random->Children.Num(), 3);
        const TSharedPtr<FMHCompositeOutlinerItem> Selected = FindOption(Random, Plan.Decisions[0].OptionIndex);
        bPassed &= TestTrue(TEXT("selected option mirrors Decisions"),
            Selected.IsValid() && Selected->bSelectedOption);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeOutlinerEmptyNavigationTest,
    "Mimir.V5.Composite.OutlinerModel.EmptyAndNavigation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeOutlinerEmptyNavigationTest::RunTest(const FString& Parameters)
{
    FOutlinerFixture Fixture(*this);
    const FString RootName = TEXT("outliner_root");
    const FString NestedName = TEXT("outliner_nested");
    Fixture.Graph.RootComposite = RootName;

    FMHCompositeDocument NestedDocument;
    NestedDocument.Nodes.AddDefaulted_GetRef().Kind = EMHCompositeNodeKind::Group;
    UMHCompositeAsset* Nested = Fixture.AddComposite(NestedName, NestedDocument);
    if (Nested == nullptr) return false;
    Nested->SourceRelativePath = TEXT("folder/outliner_nested.composite");

    FMHCompositeDocument RootDocument;
    FMHCompositeNode& RandomNode = RootDocument.Nodes.AddDefaulted_GetRef();
    RandomNode.Kind = EMHCompositeNodeKind::Random;
    RandomNode.Name = TEXT("variant");
    FMHCompositeOption& Empty = RandomNode.Options.AddDefaulted_GetRef();
    Empty.Kind = EMHCompositeOptionKind::Empty;
    Empty.Weight = 1.0f;
    FMHCompositeOption& Composite = RandomNode.Options.AddDefaulted_GetRef();
    Composite.Kind = EMHCompositeOptionKind::Composite;
    Composite.Resource = NestedName;
    Composite.Weight = 1.0f;
    UMHCompositeAsset* Root = Fixture.AddComposite(RootName, RootDocument);
    if (Root == nullptr) return false;

    FMHResolvedCompositePlan Plan;
    if (!Fixture.Resolve(0, Plan)) return false;
    FMHCompositeOutlinerModel Model = Fixture.MakeModel();
    bool bPassed = TestTrue(TEXT("synthetic model builds"), Model.Build(*Root, &Plan));
    if (Model.GetRoots().IsEmpty()) return false;
    const TSharedPtr<FMHCompositeOutlinerItem> Random = Model.GetRoots()[0];
    const TSharedPtr<FMHCompositeOutlinerItem> EmptyRow = FindOption(Random, 0);
    const TSharedPtr<FMHCompositeOutlinerItem> CompositeRow = FindOption(Random, 1);
    bPassed &= TestTrue(TEXT("empty option row exists"), EmptyRow.IsValid());
    if (EmptyRow.IsValid()) bPassed &= TestEqual(TEXT("empty option uses Dagor label"), EmptyRow->Label, FString(TEXT("--")));

    FMHCompositeOutlinerNavigation Navigation;
    bPassed &= TestTrue(TEXT("composite option has navigation"),
        CompositeRow.IsValid() && Model.GetNavigation(*CompositeRow, TEXT("C:/MHSource"), Navigation));
    bPassed &= TestEqual(TEXT("navigation name"), Navigation.Name, NestedName);
    bPassed &= TestEqual(TEXT("navigation source path"),
        FPaths::ConvertRelativePathToFull(Navigation.SourceFilepath),
        FPaths::ConvertRelativePathToFull(TEXT("C:/MHSource/folder/outliner_nested.composite")));
    bPassed &= TestEqual(TEXT("copy name is the referenced resource"),
        CompositeRow.IsValid() ? Model.GetCopyName(*CompositeRow) : FString(), NestedName);

    FMHCompositeOutlinerModel MissingPlanModel = Fixture.MakeModel();
    bPassed &= TestTrue(TEXT("source tree remains available without a plan"),
        MissingPlanModel.Build(*Root, nullptr, TEXT("missing endpoint")));
    bPassed &= TestEqual(TEXT("missing-plan reason is retained"),
        MissingPlanModel.GetOverlayStatus(), FString(TEXT("missing endpoint")));
    if (!MissingPlanModel.GetRoots().IsEmpty())
        bPassed &= TestFalse(TEXT("missing plan creates no false overlay"),
            MissingPlanModel.GetRoots()[0]->bHasResolvedOverlay);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeOutlinerReseedRefreshTest,
    "Mimir.V5.Composite.OutlinerModel.ReseedRefresh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeOutlinerReseedRefreshTest::RunTest(const FString& Parameters)
{
    FOutlinerFixture Fixture(*this);
    if (!Fixture.BuildGaz53()) return false;
    FMHResolvedCompositePlan First;
    FMHResolvedCompositePlan Second;
    if (!Fixture.Resolve(0, First)) return false;
    int32 SecondSeed = 1;
    while (SecondSeed < 4096)
    {
        if (!Fixture.Resolve(SecondSeed, Second)) return false;
        if (Second.Decisions[0].OptionIndex != First.Decisions[0].OptionIndex) break;
        ++SecondSeed;
    }
    if (!TestTrue(TEXT("fixture exposes two different random choices"), SecondSeed < 4096)) return false;

    FMHCompositeOutlinerModel Model = Fixture.MakeModel();
    UMHCompositeAsset* Root = Fixture.Assets.FindRef(TEXT("gaz53_b_random_cmp"));
    if (Root == nullptr || !Model.Build(*Root, &First) || Model.GetRoots().Num() != 2) return false;
    if (!Model.ExpandItem(Model.GetRoots()[1])) return false;
    const FString RandomPath = TEXT("gaz53_b_random_cmp:nodes[1]>gaz53_body_bc_random_cmp:nodes[0]");
    const TSharedPtr<FMHCompositeOutlinerItem> Random = Model.FindByNodePath(RandomPath);
    if (!TestTrue(TEXT("random row exists before refresh"), Random.IsValid())) return false;
    const TSharedPtr<FMHCompositeOutlinerItem> StablePointer = Random;
    bool bPassed = TestTrue(TEXT("overlay refresh succeeds"), Model.RefreshOverlay(&Second));
    bPassed &= TestTrue(TEXT("source row identity survives reseed"), Model.FindByNodePath(RandomPath) == StablePointer);
    bPassed &= TestTrue(TEXT("old option is no longer selected"),
        !FindOption(Random, First.Decisions[0].OptionIndex)->bSelectedOption);
    bPassed &= TestTrue(TEXT("new option mirrors new Decisions"),
        FindOption(Random, Second.Decisions[0].OptionIndex)->bSelectedOption);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeOutlinerOverlayCostTest,
    "Mimir.V5.Composite.OutlinerModel.OverlayRefreshCost300",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeOutlinerOverlayCostTest::RunTest(const FString& Parameters)
{
    FOutlinerFixture Fixture(*this);
    const FString RootName = TEXT("outliner_cost_root");
    Fixture.Graph.RootComposite = RootName;
    FMHCompositeDocument Document;
    for (int32 Index = 0; Index < 300; ++Index)
    {
        FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
        Leaf.Kind = EMHCompositeNodeKind::Mesh;
        Leaf.Resource = TEXT("outliner_cost_mesh");
        Leaf.Name = FString::Printf(TEXT("leaf_%d"), Index);
    }
    UMHCompositeAsset* Root = Fixture.AddComposite(RootName, Document);
    OutlinerAddRawHash(Fixture.Graph, TEXT("static_mesh:outliner_cost_mesh"));
    if (Root == nullptr) return false;
    FMHResolvedCompositePlan Plan;
    if (!Fixture.Resolve(123, Plan)) return false;
    FMHCompositeOutlinerModel Model = Fixture.MakeModel();
    if (!Model.Build(*Root, &Plan)) return false;

    constexpr int32 Iterations = 200;
    const double Start = FPlatformTime::Seconds();
    for (int32 Index = 0; Index < Iterations; ++Index)
        if (!Model.RefreshOverlay(&Plan)) return false;
    const double AverageMs = (FPlatformTime::Seconds() - Start) * 1000.0 / Iterations;
    AddInfo(FString::Printf(
        TEXT("MH_OUTLINER_PROFILE nodes=300 iterations=%d average_overlay_refresh_ms=%.6f deferred_from_reseed=1"),
        Iterations,
        AverageMs));
    return TestTrue(
        *FString::Printf(TEXT("300-node overlay refresh %.6f ms stays below 1 ms"), AverageMs),
        AverageMs < 1.0);
}

} // namespace UE::MimirComposite::Tests
