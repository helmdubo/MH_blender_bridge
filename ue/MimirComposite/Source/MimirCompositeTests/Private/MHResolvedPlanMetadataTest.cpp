#include "MHGoldenRoot.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "IO/IoHash.h"
#include "Math/Matrix.h"
#include "Math/Quat.h"
#include "Math/Transform.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"

namespace UE::MimirComposite::Tests
{
namespace
{

void MetadataAddRawHash(FMHRandomSourceGraph& Graph, const FString& ResourceKey)
{
    // These in-memory fixtures use the UTF-8 resource key as their raw payload.
    // The closure therefore receives real, canonical BLAKE3-160 hashes without
    // reading or modifying any shared golden/source fixture.
    const FTCHARToUTF8 Payload(*ResourceKey, ResourceKey.Len());
    const FIoHash Hash = FIoHash::HashBuffer(Payload.Get(), static_cast<uint64>(Payload.Length()));
    Graph.RawHashes.Add(ResourceKey, TEXT("blake3-160:") + LexToString(Hash).ToLower());
}

FMHRandomNode MetadataMesh(const TCHAR* Resource, const float TranslationX = 0.0f)
{
    FMHRandomNode Node;
    Node.Kind = EMHRandomSemanticKind::Mesh;
    Node.Resource = Resource;
    Node.Transform.TranslationCm.X = TranslationX;
    return Node;
}

FMHRandomOption MetadataOption(
    const EMHRandomSemanticKind Kind, const TCHAR* Resource, const float Weight = 1.0f)
{
    FMHRandomOption Option;
    Option.Kind = Kind;
    Option.Resource = Resource;
    Option.Weight = Weight;
    return Option;
}

void MetadataAddComposite(FMHRandomSourceGraph& Graph, const TCHAR* Name, TArray<FMHRandomNode> Nodes)
{
    FMHRandomComposite Composite;
    Composite.Name = Name;
    Composite.Nodes = MoveTemp(Nodes);
    Graph.Composites.Add(Composite.Name, Composite);
    MetadataAddRawHash(Graph, TEXT("composite:") + Composite.Name);
}

FMHRandomSourceGraph MetadataGraph(FMHRandomNode Node)
{
    FMHRandomSourceGraph Graph;
    Graph.RootComposite = TEXT("metadata_root");
    MetadataAddComposite(Graph, TEXT("metadata_root"), {MoveTemp(Node)});
    MetadataAddRawHash(Graph, TEXT("static_mesh:mesh_a"));
    MetadataAddRawHash(Graph, TEXT("static_mesh:mesh_b"));
    return Graph;
}

void MetadataAddOffsetProfile(FMHRandomSourceGraph& Graph, const TCHAR* Name, const float Deviation)
{
    FMHRandomPlacementProfile Profile;
    Profile.Name = Name;
    Profile.bHasOffsetCm = true;
    Profile.OffsetCm[0].Base = 10.0f;
    Profile.OffsetCm[0].Deviation = Deviation;
    Graph.Profiles.Add(Profile.Name, Profile);
    MetadataAddRawHash(Graph, TEXT("placement_profile:") + Profile.Name);
}

FMatrix MetadataMatrix(const FMHRandomTrs& Trs)
{
    return FTransform(FQuat(Trs.RotationQuat), FVector(Trs.TranslationCm), FVector(Trs.Scale)).ToMatrixWithScale();
}

bool MetadataResolve(
    FAutomationTestBase& Test,
    const FMHRandomSourceGraph& Graph,
    const int32 Seed,
    FMHResolvedCompositePlan& OutPlan,
    const int32 AppearanceSeed = 777)
{
    FString Error;
    const bool bResolved = MHResolveCompositePlan(Graph, Seed, AppearanceSeed, OutPlan, Error);
    Test.TestTrue(*FString::Printf(TEXT("synthetic graph resolves for seed %d: %s"), Seed, *Error), bResolved);
    return bResolved;
}

EMHRandomSemanticKind MetadataNodeKind(const EMHCompositeNodeKind Kind)
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

EMHRandomSemanticKind MetadataOptionKind(const EMHCompositeOptionKind Kind)
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

FMHRandomNode MetadataConvertNode(const FMHCompositeNode& Node)
{
    FMHRandomNode Result;
    Result.Kind = MetadataNodeKind(Node.Kind);
    Result.Resource = Node.Resource;
    Result.DisplayName = Node.Name;
    Result.Profile = Node.Profile;
    Result.bAppearanceSeedBoundary = Node.bAppearanceSeedBoundary;
    Result.Transform.TranslationCm = FVector3f(Node.Transform.TranslationCm);
    Result.Transform.RotationQuat = FQuat4f(Node.Transform.RotationQuat);
    Result.Transform.Scale = FVector3f(Node.Transform.Scale);
    for (const FMHCompositeOption& Option : Node.Options)
        Result.Options.Add({MetadataOptionKind(Option.Kind), Option.Resource, Option.Weight});
    for (const FMHCompositeNode& Child : Node.Children) Result.Children.Add(MetadataConvertNode(Child));
    return Result;
}

/**
 * Loads the ratified GAZ-53 documents from `golden/` unchanged and terminates
 * the three option payloads the fixture manifest itself declares unresolved
 * (`unresolved_random_option_payloads`). The terminators are declared here, in
 * the test, and nothing is written back to `golden/`.
 */
bool MetadataLoadGaz53(FAutomationTestBase& Test, FMHRandomSourceGraph& OutGraph)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(Test, GoldenRoot)) return false;
    OutGraph.RootComposite = TEXT("gaz53_b_random_cmp");
    for (const TCHAR* Name : {TEXT("gaz53_b_random_cmp"), TEXT("gaz53_b_body_cmp"), TEXT("gaz53_body_bc_random_cmp")})
    {
        TArray<uint8> Bytes;
        const FString Path = FPaths::Combine(GoldenRoot, TEXT("v5/gaz53"), FString(Name) + TEXT(".composite"));
        if (!FFileHelper::LoadFileToArray(Bytes, *Path))
        {
            Test.AddError(TEXT("cannot read frozen GAZ-53 document: ") + Path);
            return false;
        }
        FMHCompositeDocument Document;
        FString Error;
        if (!MHParseCompositeV5(Bytes, Document, Error))
        {
            Test.AddError(TEXT("frozen GAZ-53 document rejected: ") + Error);
            return false;
        }
        FMHRandomComposite Composite;
        Composite.Name = Name;
        for (const FMHCompositeNode& Node : Document.Nodes) Composite.Nodes.Add(MetadataConvertNode(Node));
        OutGraph.Composites.Add(Composite.Name, MoveTemp(Composite));
        MetadataAddRawHash(OutGraph, TEXT("composite:") + FString(Name));
    }
    for (const TCHAR* Name : {TEXT("gaz53_bread_b_cmp"), TEXT("gaz53_wooden_b_cmp"), TEXT("gaz53_wooden_c_cmp")})
    {
        MetadataAddComposite(OutGraph, Name, {MetadataMesh(TEXT("gaz53_b_body"))});
    }
    MetadataAddRawHash(OutGraph, TEXT("static_mesh:gaz53_b_body"));
    return true;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanParentLocalMetadataTest,
    "Mimir.V5.Random.PlanMetadata.ParentLocalMatrices",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanParentLocalMetadataTest::RunTest(const FString& Parameters)
{
    FMHRandomNode Parent;
    Parent.Transform.TranslationCm.X = 100.0f;
    Parent.Children.Add(MetadataMesh(TEXT("mesh_a"), 25.0f));
    const FMHRandomSourceGraph Graph = MetadataGraph(Parent);
    FMHResolvedCompositePlan Plan;
    if (!MetadataResolve(*this, Graph, 100, Plan)) return false;
    if (!TestEqual(TEXT("parent and child metadata"), Plan.Nodes.Num(), 2) ||
        !TestEqual(TEXT("one mesh leaf"), Plan.Leaves.Num(), 1)) return false;

    TestEqual(TEXT("authored child remains parent-local"), Plan.Nodes[1].AuthoredLocalTrs.TranslationCm.X, 25.0f);
    TestEqual(TEXT("sampled child remains parent-local"), Plan.Nodes[1].LocalTrs.TranslationCm.X, 25.0f);
    TestEqual(TEXT("parent world translation"), Plan.Nodes[0].WorldMatrix.M[3][0], 100.0);
    TestEqual(TEXT("parent 100 plus local 25 gives world 125"), Plan.Leaves[0].WorldMatrix.M[3][0], 125.0);
    TestEqual(TEXT("frozen reference TRS translation"), Plan.Leaves[0].WorldTrs.TranslationCm.X, 125.0f);
    TestTrue(TEXT("leaf uses the child full matrix"), Plan.Leaves[0].WorldMatrix.Equals(Plan.Nodes[1].WorldMatrix, 0.0));

    FString Error;
    TestTrue(TEXT("representable plan admitted"), MHValidateResolvedPlacementTransforms(Plan, FTransform::Identity, Error));
    const FTransform MovedPlacement(FVector(1000.0, 0.0, 0.0));
    TestTrue(TEXT("moved placement admitted"), MHValidateResolvedPlacementTransforms(Plan, MovedPlacement, Error));
    const FMatrix MovedWorld = Plan.Leaves[0].WorldMatrix * MovedPlacement.ToMatrixWithScale();
    TestEqual(TEXT("placement basis is applied once"), MovedWorld.M[3][0], 1125.0);
    TestEqual(TEXT("moving placement does not mutate plan matrix"), Plan.Leaves[0].WorldMatrix.M[3][0], 125.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanShearMetadataTest,
    "Mimir.V5.Random.PlanMetadata.ShearRemainsVisibleToAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanShearMetadataTest::RunTest(const FString& Parameters)
{
    FMHRandomNode Parent;
    Parent.Transform.Scale = FVector3f(2.0f, 1.0f, 1.0f);
    FMHRandomNode Child = MetadataMesh(TEXT("mesh_a"), 5.0f);
    Child.Transform.RotationQuat = FQuat4f(FVector3f(0.0f, 0.0f, 1.0f), FMath::DegreesToRadians(45.0f));
    Parent.Children.Add(Child);
    FMHResolvedCompositePlan Plan;
    if (!MetadataResolve(*this, MetadataGraph(Parent), 100, Plan)) return false;
    if (!TestEqual(TEXT("two source nodes"), Plan.Nodes.Num(), 2) ||
        !TestEqual(TEXT("one resolved leaf"), Plan.Leaves.Num(), 1)) return false;

    const FMatrix Expected = MetadataMatrix(Plan.Nodes[1].LocalTrs) * MetadataMatrix(Plan.Nodes[0].LocalTrs);
    const FMatrix FrozenTrsMatrix = MetadataMatrix(Plan.Leaves[0].WorldTrs);
    TestTrue(TEXT("full local-parent matrix product retained"), Plan.Leaves[0].WorldMatrix.Equals(Expected, 0.0));
    TestFalse(TEXT("componentwise WorldTrs cannot replace the full product"),
        MHMatrixElementsWithinTrsTolerance(Plan.Leaves[0].WorldMatrix, FrozenTrsMatrix));
    TestTrue(TEXT("parent by itself is representable"), MHIsRepresentableTransformMatrix(Plan.Nodes[0].WorldMatrix));
    TestFalse(TEXT("child world retains non-representable shear"), MHIsRepresentableTransformMatrix(Plan.Leaves[0].WorldMatrix));
    TestTrue(TEXT("componentwise signature TRS would hide the shear"), MHIsRepresentableTransformMatrix(FrozenTrsMatrix));

    FString Error;
    TestFalse(TEXT("consumer rejects the full plan before mutation"),
        MHValidateResolvedPlacementTransforms(Plan, FTransform::Identity, Error));
    TestTrue(TEXT("rejection names registered diagnostic"), Error.Contains(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM")));
    TestTrue(TEXT("rejection identifies the child source path"), Error.Contains(TEXT("metadata_root:nodes[0]/children[0]")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanRootNodeMetadataTest,
    "Mimir.V5.Random.PlanMetadata.NestedRootNodeProvenance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanRootNodeMetadataTest::RunTest(const FString& Parameters)
{
    FMHRandomSourceGraph Graph = MetadataGraph(MetadataMesh(TEXT("mesh_a")));
    FMHRandomNode Random;
    Random.Kind = EMHRandomSemanticKind::Random;
    Random.Transform.TranslationCm.X = 100.0f;
    Random.Options.Add(MetadataOption(EMHRandomSemanticKind::Composite, TEXT("metadata_nested")));
    Random.Children.Add(MetadataMesh(TEXT("mesh_b"), 2.0f));
    Graph.Composites.FindChecked(Graph.RootComposite).Nodes.Add(Random);

    FMHRandomNode NestedGroup;
    NestedGroup.Transform.TranslationCm.X = 25.0f;
    NestedGroup.Children.Add(MetadataMesh(TEXT("mesh_b"), 5.0f));
    FMHRandomNode NestedComposite;
    NestedComposite.Kind = EMHRandomSemanticKind::Composite;
    NestedComposite.Resource = TEXT("metadata_deep");
    NestedComposite.Transform.TranslationCm.X = 50.0f;
    MetadataAddComposite(Graph, TEXT("metadata_nested"), {NestedGroup, NestedComposite});
    MetadataAddComposite(Graph, TEXT("metadata_deep"), {MetadataMesh(TEXT("mesh_a"), 10.0f)});

    FMHResolvedCompositePlan Plan;
    if (!MetadataResolve(*this, Graph, 100, Plan)) return false;
    const TArray<FString> ExpectedPaths{
        TEXT("metadata_root:nodes[0]"),
        TEXT("metadata_root:nodes[1]"),
        TEXT("metadata_root:nodes[1]/options[0]>metadata_nested:nodes[0]"),
        TEXT("metadata_root:nodes[1]/options[0]>metadata_nested:nodes[0]/children[0]"),
        TEXT("metadata_root:nodes[1]/options[0]>metadata_nested:nodes[1]"),
        TEXT("metadata_root:nodes[1]/options[0]>metadata_nested:nodes[1]>metadata_deep:nodes[0]"),
        TEXT("metadata_root:nodes[1]/children[0]")};
    if (!TestEqual(TEXT("all source nodes recorded"), Plan.Nodes.Num(), ExpectedPaths.Num()) ||
        !TestEqual(TEXT("nested composites are flattened to four leaves"), Plan.Leaves.Num(), 4)) return false;
    for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
    {
        TestEqual(*FString::Printf(TEXT("DFS node path %d"), Index), Plan.Nodes[Index].NodePath, ExpectedPaths[Index]);
        TestEqual(*FString::Printf(TEXT("root index retained at node %d"), Index), Plan.Nodes[Index].RootNodeIndex, Index == 0 ? 0 : 1);
    }
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        TestEqual(*FString::Printf(TEXT("root index retained at leaf %d"), Index), Plan.Leaves[Index].RootNodeIndex, Index == 0 ? 0 : 1);
    }
    TestEqual(TEXT("group descendant world"), Plan.Leaves[1].WorldMatrix.M[3][0], 130.0);
    TestEqual(TEXT("deep composite descendant world"), Plan.Leaves[2].WorldMatrix.M[3][0], 160.0);
    TestEqual(TEXT("random direct child follows nested traversal"), Plan.Leaves[3].WorldMatrix.M[3][0], 102.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanDisplayMetadataTest,
    "Mimir.V5.Random.PlanMetadata.DisplayNamesDoNotEnterSignature",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanDisplayMetadataTest::RunTest(const FString& Parameters)
{
    FMHRandomNode Parent;
    Parent.DisplayName = TEXT("Original group");
    FMHRandomNode Random;
    Random.Kind = EMHRandomSemanticKind::Random;
    Random.DisplayName = TEXT("Original choice");
    Random.Options.Add(MetadataOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_a")));
    Random.Options.Add(MetadataOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_b")));
    Parent.Children.Add(Random);
    FMHRandomSourceGraph Graph = MetadataGraph(Parent);
    FMHResolvedCompositePlan Before;
    if (!MetadataResolve(*this, Graph, 100, Before)) return false;

    // Isolate presentation metadata: the source receipt/closure hashes are fixed.
    FMHRandomNode& Renamed = Graph.Composites.FindChecked(Graph.RootComposite).Nodes[0];
    Renamed.DisplayName = TEXT("Renamed group");
    Renamed.Children[0].DisplayName = TEXT("Renamed choice");
    FMHResolvedCompositePlan After;
    if (!MetadataResolve(*this, Graph, 100, After)) return false;
    if (!TestEqual(TEXT("group and choice metadata"), After.Nodes.Num(), 2) ||
        !TestEqual(TEXT("one selected mesh"), After.Leaves.Num(), 1) ||
        !TestEqual(TEXT("one decision"), After.Decisions.Num(), 1) ||
        !TestEqual(TEXT("one original decision"), Before.Decisions.Num(), 1)) return false;
    TestEqual(TEXT("group display name follows presentation"), After.Nodes[0].DisplayName, FString(TEXT("Renamed group")));
    TestEqual(TEXT("leaf receives choice display name"), After.Leaves[0].DisplayName, FString(TEXT("Renamed choice")));
    TestEqual(TEXT("display names do not key the random stream"), Before.Decisions[0].RawU32, After.Decisions[0].RawU32);
    TestTrue(TEXT("signature bytes exclude presentation metadata"), Before.SignaturePreimage == After.SignaturePreimage);
    TestEqual(TEXT("signature remains identical"), Before.ResolvedSignature, After.ResolvedSignature);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanSignatureRefreshTest,
    "Mimir.V5.Random.PlanMetadata.NoDrawSignatureRefresh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanSignatureRefreshTest::RunTest(const FString& Parameters)
{
    const FMHRandomSourceGraph Graph = MetadataGraph(MetadataMesh(TEXT("mesh_a"), 25.0f));
    FMHResolvedCompositePlan Before;
    FMHResolvedCompositePlan Fresh;
    if (!MetadataResolve(*this, Graph, 100, Before) || !MetadataResolve(*this, Graph, 200, Fresh)) return false;
    TestEqual(TEXT("no visual seed effect"), MHClassifyCompositeGraph(Graph), EMHCompositeSeedEffect::None);
    TestTrue(TEXT("no decision or profile draws"), Before.Decisions.IsEmpty() && Before.Draws.IsEmpty());
    FMHResolvedCompositePlan Refreshed = Before;
    Refreshed.Seed = 200;
    MHRefreshResolvedCompositeSignature(Refreshed);
    TestTrue(TEXT("refresh bytes equal full resolve"), Refreshed.SignaturePreimage == Fresh.SignaturePreimage);
    TestEqual(TEXT("refresh signature equals full resolve"), Refreshed.ResolvedSignature, Fresh.ResolvedSignature);
    TestNotEqual(TEXT("seed remains part of signature without random nodes"), Before.ResolvedSignature, Fresh.ResolvedSignature);
    TestEqual(TEXT("refresh preserves source closure"), Refreshed.Closure.ClosureHash, Before.Closure.ClosureHash);
    TestTrue(TEXT("refresh does not manufacture draws"), Refreshed.Decisions.IsEmpty() && Refreshed.Draws.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanSeedClassificationTest,
    "Mimir.V5.Random.PlanMetadata.SeedEffectClassification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanSeedClassificationTest::RunTest(const FString& Parameters)
{
    FMHRandomSourceGraph Graph = MetadataGraph(MetadataMesh(TEXT("mesh_a")));
    TestEqual(TEXT("fixed mesh is None"), MHClassifyCompositeGraph(Graph), EMHCompositeSeedEffect::None);
    Graph.Composites.FindChecked(Graph.RootComposite).Nodes[0].Profile = TEXT("varying_offset");
    MetadataAddOffsetProfile(Graph, TEXT("varying_offset"), 5.0f);
    TestEqual(TEXT("varying local profile is Transform"), MHClassifyCompositeGraph(Graph), EMHCompositeSeedEffect::Transform);

    FMHRandomNode Random;
    Random.Kind = EMHRandomSemanticKind::Random;
    Random.Options.Add(MetadataOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_a")));
    Random.Options.Add(MetadataOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_b")));
    Graph = MetadataGraph(Random);
    TestEqual(TEXT("different positive options are Topology"), MHClassifyCompositeGraph(Graph), EMHCompositeSeedEffect::Topology);

    FMHRandomNode Nested;
    Nested.Kind = EMHRandomSemanticKind::Composite;
    Nested.Resource = TEXT("metadata_nested");
    Graph = MetadataGraph(Nested);
    MetadataAddComposite(Graph, TEXT("metadata_nested"), {Random});
    TestEqual(TEXT("variation only inside referenced composite is ChildSeedsOnly"),
        MHClassifyCompositeGraph(Graph), EMHCompositeSeedEffect::ChildSeedsOnly);
    Graph.Composites.FindChecked(Graph.RootComposite).Nodes[0].Profile = TEXT("varying_offset");
    MetadataAddOffsetProfile(Graph, TEXT("varying_offset"), 5.0f);
    TestEqual(TEXT("local Transform dominates ChildSeedsOnly"), MHClassifyCompositeGraph(Graph), EMHCompositeSeedEffect::Transform);
    Random.Profile = TEXT("varying_offset");
    Graph.Composites.FindChecked(Graph.RootComposite).Nodes.Add(Random);
    TestEqual(TEXT("local Topology dominates Transform"), MHClassifyCompositeGraph(Graph), EMHCompositeSeedEffect::Topology);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanSingleOptionTraceTest,
    "Mimir.V5.Random.PlanMetadata.ConstantSelectionStillDraws",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanSingleOptionTraceTest::RunTest(const FString& Parameters)
{
    FMHRandomNode Random;
    Random.Kind = EMHRandomSemanticKind::Random;
    Random.Options.Add(MetadataOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_a")));
    Random.Options.Add(MetadataOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_b"), 0.0f));
    const FMHRandomSourceGraph Graph = MetadataGraph(Random);
    TestEqual(TEXT("one positive option has no visual seed effect"), MHClassifyCompositeGraph(Graph), EMHCompositeSeedEffect::None);
    FMHResolvedCompositePlan Before;
    FMHResolvedCompositePlan After;
    if (!MetadataResolve(*this, Graph, 100, Before) || !MetadataResolve(*this, Graph, 200, After)) return false;
    if (!TestEqual(TEXT("seed 100 always consumes one selection draw"), Before.Draws.Num(), 1) ||
        !TestEqual(TEXT("seed 200 always consumes one selection draw"), After.Draws.Num(), 1) ||
        !TestEqual(TEXT("seed 100 has one decision"), Before.Decisions.Num(), 1) ||
        !TestEqual(TEXT("seed 200 has one decision"), After.Decisions.Num(), 1) ||
        !TestEqual(TEXT("seed 100 has one leaf"), Before.Leaves.Num(), 1) ||
        !TestEqual(TEXT("seed 200 has one leaf"), After.Leaves.Num(), 1)) return false;
    TestEqual(TEXT("seed 100 selects the only positive option"), Before.Decisions[0].OptionIndex, 0);
    TestEqual(TEXT("seed 200 selects the only positive option"), After.Decisions[0].OptionIndex, 0);
    TestEqual(TEXT("draw has selection role"), Before.Draws[0].Role, FString(TEXT("selection")));
    TestNotEqual(TEXT("visually constant selection raw draw changes"), Before.Draws[0].RawU32, After.Draws[0].RawU32);
    TestEqual(TEXT("selected resource unchanged"), Before.Leaves[0].Resource, After.Leaves[0].Resource);
    TestTrue(TEXT("selected geometry unchanged"), Before.Leaves[0].WorldMatrix.Equals(After.Leaves[0].WorldMatrix, 0.0));
    TestTrue(TEXT("zero-weight dependency still belongs to full closure"), After.Closure.Resources.Contains(TEXT("static_mesh:mesh_b")));
    FMHResolvedCompositePlan IncorrectShortcut = Before;
    IncorrectShortcut.Seed = 200;
    MHRefreshResolvedCompositeSignature(IncorrectShortcut);
    TestNotEqual(TEXT("classification None alone cannot authorize signature-only reuse"),
        IncorrectShortcut.ResolvedSignature, After.ResolvedSignature);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanConstantProfileTraceTest,
    "Mimir.V5.Random.PlanMetadata.ConstantProfileStillDraws",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanConstantProfileTraceTest::RunTest(const FString& Parameters)
{
    FMHRandomNode Mesh = MetadataMesh(TEXT("mesh_a"));
    Mesh.Profile = TEXT("constant_offset");
    FMHRandomSourceGraph Graph = MetadataGraph(Mesh);
    MetadataAddOffsetProfile(Graph, TEXT("constant_offset"), 0.0f);
    TestEqual(TEXT("zero-deviation profile has no visual seed effect"), MHClassifyCompositeGraph(Graph), EMHCompositeSeedEffect::None);
    FMHResolvedCompositePlan Before;
    FMHResolvedCompositePlan After;
    if (!MetadataResolve(*this, Graph, 100, Before) || !MetadataResolve(*this, Graph, 200, After)) return false;
    if (!TestEqual(TEXT("seed 100 consumes all present offset parameters"), Before.Draws.Num(), 3) ||
        !TestEqual(TEXT("seed 200 consumes all present offset parameters"), After.Draws.Num(), 3) ||
        !TestEqual(TEXT("profile node metadata remains available"), After.Nodes.Num(), 1) ||
        !TestEqual(TEXT("seed 100 has one mesh"), Before.Leaves.Num(), 1) ||
        !TestEqual(TEXT("seed 200 has one mesh"), After.Leaves.Num(), 1)) return false;
    TestTrue(TEXT("profile-only node has no option decisions"), Before.Decisions.IsEmpty() && After.Decisions.IsEmpty());
    TestEqual(TEXT("first present parameter is offset x"), Before.Draws[0].Role, FString(TEXT("offset_x")));
    TestEqual(TEXT("offset x sample equals fixed base"), Before.Draws[0].Sample, 10.0);
    TestEqual(TEXT("profile metadata preserves authored local transform"), After.Nodes[0].AuthoredLocalTrs.TranslationCm.X, 0.0f);
    TestEqual(TEXT("profile metadata separately records sampled local transform"), After.Nodes[0].LocalTrs.TranslationCm.X, 10.0f);
    TestNotEqual(TEXT("zero deviation does not suppress raw draws"), Before.Draws[0].RawU32, After.Draws[0].RawU32);
    for (int32 Index = 0; Index < Before.Draws.Num(); ++Index)
    {
        TestEqual(*FString::Printf(TEXT("fixed profile sample %d"), Index), Before.Draws[Index].Sample, After.Draws[Index].Sample);
    }
    TestTrue(TEXT("zero-deviation geometry is unchanged"), Before.Leaves[0].WorldMatrix.Equals(After.Leaves[0].WorldMatrix, 0.0));
    FMHResolvedCompositePlan IncorrectShortcut = Before;
    IncorrectShortcut.Seed = 200;
    MHRefreshResolvedCompositeSignature(IncorrectShortcut);
    TestEqual(TEXT("fixed profile can mask stale trace behind an equal signature"), IncorrectShortcut.ResolvedSignature, After.ResolvedSignature);
    TestNotEqual(TEXT("draw-aware guard is required even when refreshed signature matches"),
        IncorrectShortcut.Draws[0].RawU32, After.Draws[0].RawU32);
    return true;
}

/**
 * Acceptance 7: the pre-order parent invariant, the nested-composite border,
 * the random-option owner, and SelectedOptionIndex agreeing with Decisions.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanParentOwnerMetadataTest,
    "Mimir.V5.Random.PlanMetadata.ParentAndOwnerIndices",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanParentOwnerMetadataTest::RunTest(const FString& Parameters)
{
    FMHRandomSourceGraph Graph = MetadataGraph(MetadataMesh(TEXT("mesh_a")));
    FMHRandomNode Random;
    Random.Kind = EMHRandomSemanticKind::Random;
    Random.Options.Add(MetadataOption(EMHRandomSemanticKind::Composite, TEXT("metadata_nested")));
    Random.Children.Add(MetadataMesh(TEXT("mesh_b"), 2.0f));
    Graph.Composites.FindChecked(Graph.RootComposite).Nodes.Add(Random);
    FMHRandomNode NestedGroup;
    NestedGroup.Children.Add(MetadataMesh(TEXT("mesh_b"), 5.0f));
    FMHRandomNode NestedComposite;
    NestedComposite.Kind = EMHRandomSemanticKind::Composite;
    NestedComposite.Resource = TEXT("metadata_deep");
    MetadataAddComposite(Graph, TEXT("metadata_nested"), {NestedGroup, NestedComposite});
    MetadataAddComposite(Graph, TEXT("metadata_deep"), {MetadataMesh(TEXT("mesh_a"), 10.0f)});

    FMHResolvedCompositePlan Plan;
    if (!MetadataResolve(*this, Graph, 100, Plan)) return false;
    if (!TestEqual(TEXT("all source nodes recorded"), Plan.Nodes.Num(), 7) ||
        !TestEqual(TEXT("nested composites flatten to four leaves"), Plan.Leaves.Num(), 4)) return false;

    // Indices follow the frozen DFS order already asserted by the sibling test.
    bool bPassed = TestEqual(TEXT("first root node has no parent"), Plan.Nodes[0].ParentResolvedNodeIndex, INDEX_NONE);
    bPassed &= TestEqual(TEXT("second root node has no parent"), Plan.Nodes[1].ParentResolvedNodeIndex, INDEX_NONE);
    bPassed &= TestEqual(TEXT("the option composite's first node hangs off the random node"),
        Plan.Nodes[2].ParentResolvedNodeIndex, 1);
    bPassed &= TestEqual(TEXT("a group child keeps its own group as parent"),
        Plan.Nodes[3].ParentResolvedNodeIndex, 2);
    bPassed &= TestEqual(TEXT("the second node of the option composite is not chained to the previous subtree"),
        Plan.Nodes[4].ParentResolvedNodeIndex, 1);
    bPassed &= TestEqual(TEXT("a node behind a nested composite border names the referencing node"),
        Plan.Nodes[5].ParentResolvedNodeIndex, 4);
    bPassed &= TestEqual(TEXT("a direct child of the random node returns to it after the option subtree"),
        Plan.Nodes[6].ParentResolvedNodeIndex, 1);
    for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
        bPassed &= TestTrue(TEXT("pre-order invariant Parent < Index"),
            Plan.Nodes[Index].ParentResolvedNodeIndex < Index);

    bPassed &= TestEqual(TEXT("only the random node records a selected option"),
        Plan.Nodes[0].SelectedOptionIndex, INDEX_NONE);
    if (TestEqual(TEXT("one decision"), Plan.Decisions.Num(), 1))
    {
        bPassed &= TestEqual(TEXT("SelectedOptionIndex equals the decision of the same NodePath"),
            Plan.Nodes[1].SelectedOptionIndex, Plan.Decisions[0].OptionIndex);
        bPassed &= TestEqual(TEXT("the decision belongs to that node"),
            Plan.Nodes[1].NodePath, Plan.Decisions[0].NodePath);
    }
    for (const FMHResolvedCompositeNode& Node : Plan.Nodes)
        if (Node.SemanticKind != EMHRandomSemanticKind::Random)
            bPassed &= TestEqual(TEXT("non-random nodes carry no selected option"), Node.SelectedOptionIndex, INDEX_NONE);

    for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
    {
        if (!TestTrue(TEXT("every leaf names an owning node"),
            Plan.Nodes.IsValidIndex(Leaf.OwningResolvedNodeIndex))) { bPassed = false; continue; }
        bPassed &= TestTrue(TEXT("the owning node's path prefixes the leaf origin"),
            Leaf.Origin.StartsWith(Plan.Nodes[Leaf.OwningResolvedNodeIndex].NodePath, ESearchCase::CaseSensitive));
    }
    bPassed &= TestEqual(TEXT("a plain mesh leaf is owned by its own node"), Plan.Leaves[0].OwningResolvedNodeIndex, 0);
    bPassed &= TestEqual(TEXT("a leaf inside the selected option is owned by its own node"),
        Plan.Leaves[1].OwningResolvedNodeIndex, 3);
    return bPassed;
}

/**
 * The same invariants on the ratified GAZ-53 documents, which exercise both a
 * nested composite border and a random composite option in one graph.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHResolvedPlanGaz53MetadataTest,
    "Mimir.V5.Random.PlanMetadata.Gaz53ParentAndOwnerIndices",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHResolvedPlanGaz53MetadataTest::RunTest(const FString& Parameters)
{
    FMHRandomSourceGraph Graph;
    if (!MetadataLoadGaz53(*this, Graph)) return false;
    bool bPassed = true;
    for (const int32 Seed : {0, 1, 2, 42, 123, 1024, 2147483647})
    {
        FMHResolvedCompositePlan Plan;
        if (!MetadataResolve(*this, Graph, Seed, Plan, Seed)) return false;
        for (int32 Index = 0; Index < Plan.Nodes.Num(); ++Index)
        {
            const FMHResolvedCompositeNode& Node = Plan.Nodes[Index];
            bPassed &= TestTrue(*FString::Printf(TEXT("GAZ-53 seed %d node %d keeps Parent < Index"), Seed, Index),
                Node.ParentResolvedNodeIndex < Index);
            if (Node.ParentResolvedNodeIndex != INDEX_NONE)
                bPassed &= TestTrue(TEXT("GAZ-53 parent path prefixes the child path"),
                    Node.NodePath.StartsWith(Plan.Nodes[Node.ParentResolvedNodeIndex].NodePath, ESearchCase::CaseSensitive));
            bPassed &= TestEqual(TEXT("GAZ-53 selected option only on random nodes"),
                Node.SelectedOptionIndex != INDEX_NONE, Node.SemanticKind == EMHRandomSemanticKind::Random);
        }
        for (const FMHResolvedCompositeDecision& Decision : Plan.Decisions)
        {
            const FMHResolvedCompositeNode* Owner = Plan.Nodes.FindByPredicate(
                [&Decision](const FMHResolvedCompositeNode& Node) { return Node.NodePath == Decision.NodePath; });
            if (!TestNotNull(TEXT("GAZ-53 decision has a node"), Owner)) { bPassed = false; continue; }
            bPassed &= TestEqual(TEXT("GAZ-53 SelectedOptionIndex mirrors the decision"),
                Owner->SelectedOptionIndex, Decision.OptionIndex);
        }
        for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
        {
            if (!TestTrue(TEXT("GAZ-53 leaf names an owning node"),
                Plan.Nodes.IsValidIndex(Leaf.OwningResolvedNodeIndex))) { bPassed = false; continue; }
            bPassed &= TestTrue(TEXT("GAZ-53 owning node path prefixes the leaf origin"),
                Leaf.Origin.StartsWith(Plan.Nodes[Leaf.OwningResolvedNodeIndex].NodePath, ESearchCase::CaseSensitive));
            bPassed &= TestEqual(TEXT("GAZ-53 has no declared boundary, so every leaf keys the placement root"),
                Leaf.AppearanceBoundaryPath, Graph.RootComposite);
        }
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
