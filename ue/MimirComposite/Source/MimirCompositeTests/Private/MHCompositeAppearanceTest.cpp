#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "IO/IoHash.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite::Tests
{
namespace
{

constexpr double AppearanceUint32Scale = 1.0 / 4294967296.0;

void AppearanceAddRawHash(FMHRandomSourceGraph& Graph, const FString& ResourceKey)
{
    // In-memory fixtures hash their own resource key, so the closure receives
    // canonical BLAKE3-160 values without touching any shared golden file.
    const FTCHARToUTF8 Payload(*ResourceKey, ResourceKey.Len());
    const FIoHash Hash = FIoHash::HashBuffer(Payload.Get(), static_cast<uint64>(Payload.Length()));
    Graph.RawHashes.Add(ResourceKey, TEXT("blake3-160:") + LexToString(Hash).ToLower());
}

FMHRandomNode AppearanceMesh(const TCHAR* Resource, const bool bBoundary = false)
{
    FMHRandomNode Node;
    Node.Kind = EMHRandomSemanticKind::Mesh;
    Node.Resource = Resource;
    Node.bAppearanceSeedBoundary = bBoundary;
    return Node;
}

FMHRandomOption AppearanceOption(const EMHRandomSemanticKind Kind, const TCHAR* Resource, const float Weight = 1.0f)
{
    FMHRandomOption Option;
    Option.Kind = Kind;
    Option.Resource = Resource;
    Option.Weight = Weight;
    return Option;
}

void AppearanceAddComposite(FMHRandomSourceGraph& Graph, const TCHAR* Name, TArray<FMHRandomNode> Nodes)
{
    FMHRandomComposite Composite;
    Composite.Name = Name;
    Composite.Nodes = MoveTemp(Nodes);
    Graph.Composites.Add(Composite.Name, Composite);
    AppearanceAddRawHash(Graph, TEXT("composite:") + Composite.Name);
}

FMHRandomSourceGraph AppearanceGraph(TArray<FMHRandomNode> Nodes)
{
    FMHRandomSourceGraph Graph;
    Graph.RootComposite = TEXT("appearance_root");
    AppearanceAddComposite(Graph, TEXT("appearance_root"), MoveTemp(Nodes));
    AppearanceAddRawHash(Graph, TEXT("static_mesh:mesh_a"));
    AppearanceAddRawHash(Graph, TEXT("static_mesh:mesh_b"));
    return Graph;
}

bool AppearanceResolve(FAutomationTestBase& Test, const FMHRandomSourceGraph& Graph,
    const int32 Seed, const int32 AppearanceSeed, FMHResolvedCompositePlan& OutPlan)
{
    FString Error;
    const bool bResolved = MHResolveCompositePlan(Graph, Seed, AppearanceSeed, OutPlan, Error);
    Test.TestTrue(*FString::Printf(TEXT("appearance fixture resolves for seed %d/%d: %s"),
        Seed, AppearanceSeed, *Error), bResolved);
    return bResolved;
}

FString AppearancePreimageText(const FMHResolvedCompositePlan& Plan)
{
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Plan.Appearance.SignaturePreimage.GetData()),
        Plan.Appearance.SignaturePreimage.Num());
    return FString(Text.Length(), Text.Get());
}

FString LayoutPreimageText(const FMHResolvedCompositePlan& Plan)
{
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Plan.SignaturePreimage.GetData()),
        Plan.SignaturePreimage.Num());
    return FString(Text.Length(), Text.Get());
}

FString AppearanceHash160(const FString& Text)
{
    const FTCHARToUTF8 Utf8(*Text, Text.Len());
    const FIoHash Hash = FIoHash::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()));
    return TEXT("blake3-160:") + LexToString(Hash).ToLower();
}

const FMHResolvedCompositeLeaf* AppearanceFindLeaf(const FMHResolvedCompositePlan& Plan, const FString& Origin)
{
    return Plan.Leaves.FindByPredicate([&Origin](const FMHResolvedCompositeLeaf& Leaf) { return Leaf.Origin == Origin; });
}

bool AppearanceChannelsEqual(const FMHResolvedCompositeLeaf& A, const FMHResolvedCompositeLeaf& B)
{
    for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
        if (A.AppearanceChannels[Channel] != B.AppearanceChannels[Channel]) return false;
    return true;
}

/** Only the explicit isolated host may write maps and generated packages. */
bool AppearanceIsolatedHost(FAutomationTestBase& Test)
{
    if (GEditor != nullptr && FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) == TEXT("MimirCompositeV5S6")) return true;
    Test.AddInfo(TEXT("map lane NOT RUN: requires the isolated MimirCompositeV5S6 editor host"));
    return false;
}

/** One mesh receipt plus a composite of top-level mesh nodes; no shared fixture. */
struct FAppearanceActorFixture
{
    FAutomationTestBase& Test;
    TArray<UObject*> Assets;
    FString Name = TEXT("appearance_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UMHCompositeAsset* Asset = nullptr;

    explicit FAppearanceActorFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    ~FAppearanceActorFixture()
    {
        for (UObject* Object : Assets)
        {
            if (!IsValid(Object)) continue;
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->MarkAsGarbage();
        }
    }

    bool Build(const int32 LeafCount = 2)
    {
        const FString MeshName = Name + TEXT("_mesh");
        UStaticMesh* Mesh = NewObject<UStaticMesh>(CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + MeshName)),
            FName(*MeshName), RF_Public | RF_Standalone);
        Assets.Add(Mesh);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = MeshName;
        Receipt->SourceRelativePath = MeshName + TEXT(".mesh.fbx");
        Receipt->SourceHash = TEXT("blake3-160:0123456789012345678901234567890123456789");
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);

        Asset = NewObject<UMHCompositeAsset>(CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + Name)),
            FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        FMHCompositeDocument Document;
        for (int32 Index = 0; Index < LeafCount; ++Index)
        {
            FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = EMHCompositeNodeKind::Mesh;
            Leaf.Resource = MeshName;
            Leaf.Transform.TranslationCm = FVector(200.0 * Index, 0.0, 0.0);
        }
        FString Error;
        TArray<uint8> Bytes;
        if (!MHApplyCompositeV5(*Asset, Document, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(Error);
            return false;
        }
        Asset->LogicalName = Name;
        Asset->SourceRelativePath = Name + TEXT(".composite");
        Asset->SourceHash = MHRawPayloadHash(Bytes);
        Asset->AppliedHash = Asset->SourceHash;
        return true;
    }
};

/**
 * Rewrites one placement into the exact shape a pre-S6.3 level record has:
 * both AppearanceSeed and its stored marker equal their archetype defaults, so
 * tagged property serialization writes neither and the reloaded actor is
 * indistinguishable from a level saved before this slice existed.
 */
bool AppearanceMakeLegacyRecord(FAutomationTestBase& Test, AMHCompositeActor& Actor)
{
    FIntProperty* SeedProperty = CastField<FIntProperty>(
        AMHCompositeActor::StaticClass()->FindPropertyByName(TEXT("AppearanceSeed")));
    FBoolProperty* StoredProperty = CastField<FBoolProperty>(
        AMHCompositeActor::StaticClass()->FindPropertyByName(TEXT("bAppearanceSeedStored")));
    if (!Test.TestNotNull(TEXT("reflected AppearanceSeed"), SeedProperty) ||
        !Test.TestNotNull(TEXT("reflected bAppearanceSeedStored"), StoredProperty)) return false;
    SeedProperty->SetPropertyValue_InContainer(&Actor, 0);
    StoredProperty->SetPropertyValue_InContainer(&Actor, false);
    return Test.TestFalse(TEXT("fixture is a legacy record before saving"), Actor.HasStoredAppearanceSeed());
}

} // namespace

/**
 * Acceptance 3 core: exactly MH_APPEARANCE_CHANNELS NextU32 draws per leaf,
 * channels 0..N-1 in order, RawU32 as the authority, actor leaves included, and
 * no draws for gameobj/empty options, which produce no leaf at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAppearanceChannelContractTest,
    "Mimir.V5.Random.Appearance.ChannelContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAppearanceChannelContractTest::RunTest(const FString& Parameters)
{
    FMHRandomNode Actor;
    Actor.Kind = EMHRandomSemanticKind::Actor;
    Actor.Resource = TEXT("actor_a");
    FMHRandomNode GameObjChoice;
    GameObjChoice.Kind = EMHRandomSemanticKind::Random;
    GameObjChoice.Options.Add(AppearanceOption(EMHRandomSemanticKind::GameObj, TEXT("marker_a")));
    FMHRandomNode EmptyChoice;
    EmptyChoice.Kind = EMHRandomSemanticKind::Random;
    EmptyChoice.Options.Add(AppearanceOption(EMHRandomSemanticKind::Empty, TEXT("")));
    EmptyChoice.Options[0].Resource.Reset();
    FMHResolvedCompositePlan Plan;
    if (!AppearanceResolve(*this, AppearanceGraph({AppearanceMesh(TEXT("mesh_a")), Actor, GameObjChoice, EmptyChoice}),
            5, 99, Plan)) return false;

    if (!TestEqual(TEXT("mesh and actor leaves only"), Plan.Leaves.Num(), 2)) return false;
    TestEqual(TEXT("MH_APPEARANCE_CHANNELS is the ratified 4"), MH_APPEARANCE_CHANNELS, 4);
    if (!TestEqual(TEXT("four draws per leaf and nothing else"),
            Plan.Appearance.Draws.Num(), Plan.Leaves.Num() * MH_APPEARANCE_CHANNELS)) return false;
    TestEqual(TEXT("plan records the appearance seed"), Plan.Appearance.AppearanceSeed, 99);
    TestEqual(TEXT("actor leaves draw too"), Plan.Leaves[1].Kind, EMHRandomSemanticKind::Actor);

    bool bPassed = true;
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        FMHRandomStream1 Stream = MHMakeNodeRandomStream(99, Leaf.AppearanceBoundaryPath);
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
        {
            const FMHResolvedCompositeAppearanceDraw& Draw = Plan.Appearance.Draws[Index * MH_APPEARANCE_CHANNELS + Channel];
            const uint32 Expected = Stream.NextU32();
            bPassed &= TestEqual(TEXT("draw names its leaf"), Draw.NodePath, Leaf.Origin);
            bPassed &= TestEqual(TEXT("draw names its boundary"), Draw.BoundaryPath, Leaf.AppearanceBoundaryPath);
            bPassed &= TestEqual(TEXT("channels are consecutive from zero"), Draw.Channel, Channel);
            bPassed &= TestEqual(TEXT("RawU32 comes from the unchanged stream primitive"), Draw.RawU32, Expected);
            bPassed &= TestEqual(TEXT("Unit is derived from RawU32"), Draw.Unit,
                static_cast<double>(Expected) * AppearanceUint32Scale);
            bPassed &= TestEqual(TEXT("the leaf channel is the float32 of that Unit"),
                Leaf.AppearanceChannels[Channel], static_cast<float>(static_cast<double>(Expected) * AppearanceUint32Scale));
        }
    }
    // The appearance stage must not consume any layout draw: the gameobj random
    // node still spends exactly its one selection draw and nothing more.
    bPassed &= TestEqual(TEXT("layout draw count is untouched by the appearance stage"), Plan.Draws.Num(), 2);
    return bPassed;
}

/** Acceptance 6: the three boundary scenarios of section 3, all three shapes. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAppearanceBoundaryScenarioTest,
    "Mimir.V5.Random.Appearance.BoundaryScenarios",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAppearanceBoundaryScenarioTest::RunTest(const FString& Parameters)
{
    bool bPassed = true;

    // 1. House with matching windows: no boundary anywhere, one stream keyed by
    //    the placement root, so every leaf receives identical channels.
    FMHRandomNode House;
    House.Children.Add(AppearanceMesh(TEXT("mesh_a")));
    House.Children.Add(AppearanceMesh(TEXT("mesh_b")));
    FMHResolvedCompositePlan Plan;
    if (!AppearanceResolve(*this, AppearanceGraph({House}), 5, 99, Plan)) return false;
    if (!TestEqual(TEXT("house has two leaves"), Plan.Leaves.Num(), 2)) return false;
    bPassed &= TestEqual(TEXT("absent boundary falls back to the placement root"),
        Plan.Leaves[0].AppearanceBoundaryPath, FString(TEXT("appearance_root")));
    bPassed &= TestEqual(TEXT("both windows share the root boundary"),
        Plan.Leaves[1].AppearanceBoundaryPath, Plan.Leaves[0].AppearanceBoundaryPath);
    bPassed &= TestTrue(TEXT("one boundary gives one set of channel values"),
        AppearanceChannelsEqual(Plan.Leaves[0], Plan.Leaves[1]));

    // 2. Fabric shop: a boundary on every leaf, so each leaf keys its own stream.
    FMHRandomNode Shop;
    Shop.Children.Add(AppearanceMesh(TEXT("mesh_a"), true));
    Shop.Children.Add(AppearanceMesh(TEXT("mesh_b"), true));
    FMHResolvedCompositePlan Fabrics;
    if (!AppearanceResolve(*this, AppearanceGraph({Shop}), 5, 99, Fabrics)) return false;
    if (!TestEqual(TEXT("shop has two leaves"), Fabrics.Leaves.Num(), 2)) return false;
    bPassed &= TestEqual(TEXT("a declaring leaf is its own boundary"),
        Fabrics.Leaves[0].AppearanceBoundaryPath, FString(TEXT("appearance_root:nodes[0]/children[0]")));
    bPassed &= TestEqual(TEXT("the second bolt keys its own path"),
        Fabrics.Leaves[1].AppearanceBoundaryPath, FString(TEXT("appearance_root:nodes[0]/children[1]")));
    bPassed &= TestFalse(TEXT("distinct boundaries give distinct channels"),
        AppearanceChannelsEqual(Fabrics.Leaves[0], Fabrics.Leaves[1]));

    // 3. Nested composite carrying the boundary: its whole subtree shares one
    //    stream, while a sibling outside it stays on the placement root.
    FMHRandomNode Nested;
    Nested.Kind = EMHRandomSemanticKind::Composite;
    Nested.Resource = TEXT("appearance_nested");
    Nested.bAppearanceSeedBoundary = true;
    FMHRandomSourceGraph Graph = AppearanceGraph({AppearanceMesh(TEXT("mesh_a")), Nested});
    AppearanceAddComposite(Graph, TEXT("appearance_nested"),
        {AppearanceMesh(TEXT("mesh_a")), AppearanceMesh(TEXT("mesh_b"))});
    FMHResolvedCompositePlan Shared;
    if (!AppearanceResolve(*this, Graph, 5, 99, Shared)) return false;
    if (!TestEqual(TEXT("nested scenario has three leaves"), Shared.Leaves.Num(), 3)) return false;
    bPassed &= TestEqual(TEXT("the sibling outside keeps the root boundary"),
        Shared.Leaves[0].AppearanceBoundaryPath, FString(TEXT("appearance_root")));
    bPassed &= TestEqual(TEXT("the composite node itself is the boundary of its subtree"),
        Shared.Leaves[1].AppearanceBoundaryPath, FString(TEXT("appearance_root:nodes[1]")));
    bPassed &= TestEqual(TEXT("every node behind the composite border shares it"),
        Shared.Leaves[2].AppearanceBoundaryPath, Shared.Leaves[1].AppearanceBoundaryPath);
    bPassed &= TestTrue(TEXT("the shared boundary gives the subtree one appearance"),
        AppearanceChannelsEqual(Shared.Leaves[1], Shared.Leaves[2]));
    bPassed &= TestFalse(TEXT("the subtree does not inherit the root's appearance"),
        AppearanceChannelsEqual(Shared.Leaves[0], Shared.Leaves[1]));
    return bPassed;
}

/**
 * Acceptance 4: the two seeds are independent. Rerolling AppearanceSeed leaves
 * every layout byte alone; rerolling Seed leaves a surviving leaf's channels
 * byte-identical because they are keyed by boundary path, not by draw order.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAppearanceSeedIndependenceTest,
    "Mimir.V5.Random.Appearance.SeedIndependence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAppearanceSeedIndependenceTest::RunTest(const FString& Parameters)
{
    FMHRandomNode Choice;
    Choice.Kind = EMHRandomSemanticKind::Random;
    Choice.Options.Add(AppearanceOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_a")));
    Choice.Options.Add(AppearanceOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_b")));
    const FMHRandomSourceGraph Graph = AppearanceGraph({AppearanceMesh(TEXT("mesh_a")), Choice});

    FMHResolvedCompositePlan Base;
    FMHResolvedCompositePlan Repainted;
    if (!AppearanceResolve(*this, Graph, 42, 7, Base) ||
        !AppearanceResolve(*this, Graph, 42, 8, Repainted)) return false;
    bool bPassed = TestTrue(TEXT("rerolling AppearanceSeed keeps the layout preimage byte-identical"),
        Base.SignaturePreimage == Repainted.SignaturePreimage);
    bPassed &= TestEqual(TEXT("rerolling AppearanceSeed keeps ResolvedSignature"),
        Base.ResolvedSignature, Repainted.ResolvedSignature);
    bPassed &= TestNotEqual(TEXT("rerolling AppearanceSeed changes AppearanceSignature"),
        Base.Appearance.AppearanceSignature, Repainted.Appearance.AppearanceSignature);
    bPassed &= TestNotEqual(TEXT("rerolling AppearanceSeed changes PlacementSignature"),
        Base.PlacementSignature, Repainted.PlacementSignature);
    bPassed &= TestEqual(TEXT("rerolling AppearanceSeed does not touch layout draws"),
        Base.Draws.Num(), Repainted.Draws.Num());
    for (int32 Index = 0; Index < Base.Draws.Num(); ++Index)
        bPassed &= TestEqual(TEXT("layout draw is unchanged"), Base.Draws[Index].RawU32, Repainted.Draws[Index].RawU32);

    // Find a layout seed whose selection differs, so the topology really changes.
    FMHResolvedCompositePlan Relaid;
    int32 OtherSeed = 43;
    for (; OtherSeed < 200; ++OtherSeed)
    {
        if (!AppearanceResolve(*this, Graph, OtherSeed, 7, Relaid)) return false;
        if (Relaid.Decisions[0].OptionIndex != Base.Decisions[0].OptionIndex) break;
    }
    if (!TestNotEqual(TEXT("a differing layout seed exists in the probe range"), OtherSeed, 200)) return false;
    bPassed &= TestNotEqual(TEXT("rerolling Seed changes ResolvedSignature"),
        Base.ResolvedSignature, Relaid.ResolvedSignature);
    const FMHResolvedCompositeLeaf* Survivor = AppearanceFindLeaf(Base, TEXT("appearance_root:nodes[0]"));
    const FMHResolvedCompositeLeaf* SurvivorAfter = AppearanceFindLeaf(Relaid, TEXT("appearance_root:nodes[0]"));
    if (!TestNotNull(TEXT("the fixed mesh survives the topology change"), Survivor) ||
        !TestNotNull(TEXT("the fixed mesh survives in the relaid plan"), SurvivorAfter)) return false;
    bPassed &= TestTrue(TEXT("a leaf that survives a topology change keeps its channels"),
        AppearanceChannelsEqual(*Survivor, *SurvivorAfter));
    bPassed &= TestEqual(TEXT("its boundary is unchanged too"),
        Survivor->AppearanceBoundaryPath, SurvivorAfter->AppearanceBoundaryPath);

    // The other half of the same rule: where the layout seed cannot change the
    // leaf set, it cannot change AppearanceSignature by a single byte either.
    const FMHRandomSourceGraph Fixed = AppearanceGraph({AppearanceMesh(TEXT("mesh_a")), AppearanceMesh(TEXT("mesh_b"))});
    FMHResolvedCompositePlan FixedBase;
    FMHResolvedCompositePlan FixedReseeded;
    if (!AppearanceResolve(*this, Fixed, 42, 7, FixedBase) ||
        !AppearanceResolve(*this, Fixed, 4242, 7, FixedReseeded)) return false;
    bPassed &= TestNotEqual(TEXT("the layout seed still moves ResolvedSignature"),
        FixedBase.ResolvedSignature, FixedReseeded.ResolvedSignature);
    bPassed &= TestTrue(TEXT("a layout reseed leaves the appearance preimage byte-identical"),
        FixedBase.Appearance.SignaturePreimage == FixedReseeded.Appearance.SignaturePreimage);
    bPassed &= TestEqual(TEXT("AppearanceSignature only moves with the leaf set or its own seed"),
        FixedBase.Appearance.AppearanceSignature, FixedReseeded.Appearance.AppearanceSignature);
    bPassed &= TestNotEqual(TEXT("PlacementSignature still tracks both halves"),
        FixedBase.PlacementSignature, FixedReseeded.PlacementSignature);
    return bPassed;
}

/**
 * The AppearanceSignature preimage carries the tag, the seed, the ratified
 * channel count, every boundary and every RawU32; PlacementSignature is the
 * hash of both signature strings concatenated without a separator.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAppearanceSignatureCompositionTest,
    "Mimir.V5.Random.Appearance.SignatureComposition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAppearanceSignatureCompositionTest::RunTest(const FString& Parameters)
{
    FMHResolvedCompositePlan Plan;
    if (!AppearanceResolve(*this, AppearanceGraph({AppearanceMesh(TEXT("mesh_a"), true)}), 5, 12345, Plan)) return false;
    const FString Preimage = AppearancePreimageText(Plan);
    bool bPassed = TestTrue(TEXT("preimage carries the appearance stage tag"),
        Preimage.Contains(TEXT("\"stage\": \"mh.appearance:1\"")));
    bPassed &= TestTrue(TEXT("preimage carries the appearance seed"),
        Preimage.Contains(TEXT("\"appearance_seed\": 12345")));
    bPassed &= TestTrue(TEXT("preimage carries the ratified channel count"), Preimage.Contains(TEXT("\"channels\": 4")));
    bPassed &= TestTrue(TEXT("preimage carries the distinct boundary list"),
        Preimage.Contains(TEXT("\"boundaries\": [\n    \"appearance_root:nodes[0]\"\n  ]")));
    bPassed &= TestTrue(TEXT("preimage carries the per-leaf boundary"),
        Preimage.Contains(TEXT("\"boundary\": \"appearance_root:nodes[0]\"")));
    for (const FMHResolvedCompositeAppearanceDraw& Draw : Plan.Appearance.Draws)
        bPassed &= TestTrue(TEXT("preimage carries every RawU32"), Preimage.Contains(LexToString(Draw.RawU32)));
    bPassed &= TestEqual(TEXT("AppearanceSignature is the hash of its own preimage"),
        Plan.Appearance.AppearanceSignature, AppearanceHash160(Preimage));

    // The layout resolver token is untouched and the appearance tag never
    // leaks into the layout preimage: two stages, two independent domains.
    const FString Layout = LayoutPreimageText(Plan);
    bPassed &= TestTrue(TEXT("layout preimage still names mh.random_resolver:2"),
        Layout.Contains(TEXT("\"resolver\": \"mh.random_resolver:2\"")));
    bPassed &= TestFalse(TEXT("layout preimage does not mention the appearance stage"),
        Layout.Contains(TEXT("mh.appearance")));
    bPassed &= TestEqual(TEXT("PlacementSignature concatenates both signatures without a separator"),
        Plan.PlacementSignature, AppearanceHash160(Plan.ResolvedSignature + Plan.Appearance.AppearanceSignature));
    return bPassed;
}

/**
 * Acceptance 7 tail: the derived plan metadata introduced by this slice is
 * absent from both signature preimages, so it cannot move any parity vector.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAppearanceMetadataOutsideSignaturesTest,
    "Mimir.V5.Random.Appearance.MetadataOutsideSignatures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAppearanceMetadataOutsideSignaturesTest::RunTest(const FString& Parameters)
{
    FMHRandomNode Choice;
    Choice.Kind = EMHRandomSemanticKind::Random;
    Choice.Options.Add(AppearanceOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_a")));
    Choice.Options.Add(AppearanceOption(EMHRandomSemanticKind::Mesh, TEXT("mesh_b")));
    FMHResolvedCompositePlan Plan;
    if (!AppearanceResolve(*this, AppearanceGraph({Choice}), 42, 7, Plan)) return false;
    const TArray<uint8> Layout = Plan.SignaturePreimage;
    const TArray<uint8> Appearance = Plan.Appearance.SignaturePreimage;

    // Perturb every metadata field this slice introduced, then rehash. Neither
    // preimage may move: they are keyed by identity and geometry, not traversal.
    for (FMHResolvedCompositeNode& Node : Plan.Nodes)
    {
        Node.ParentResolvedNodeIndex = 4242;
        Node.SelectedOptionIndex = 4242;
    }
    for (FMHResolvedCompositeLeaf& Leaf : Plan.Leaves) Leaf.OwningResolvedNodeIndex = 4242;
    MHRefreshResolvedCompositeSignature(Plan);
    bool bPassed = TestTrue(TEXT("plan metadata is outside the layout preimage"), Plan.SignaturePreimage == Layout);
    bPassed &= TestTrue(TEXT("plan metadata is outside the appearance preimage"),
        Plan.Appearance.SignaturePreimage == Appearance);
    const FString Text = LayoutPreimageText(Plan) + AppearancePreimageText(Plan);
    bPassed &= TestFalse(TEXT("no signature names a traversal index"), Text.Contains(TEXT("4242")));
    return bPassed;
}

/**
 * Acceptance 5: a level saved before this slice materializes its AppearanceSeed
 * exactly once on load, and later Seed rerolls never move it. The guard against
 * a computed default is the second half: the stored value must stay the value
 * derived from the ORIGINAL Seed, not from whatever Seed holds now.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAppearanceSeedMigrationTest,
    "Mimir.V5.Composite.Seed.AppearanceMigration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAppearanceSeedMigrationTest::RunTest(const FString& Parameters)
{
    if (!AppearanceIsolatedHost(*this)) return true;
    FAppearanceActorFixture Fixture(*this);
    if (!Fixture.Build()) return false;
    UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
    if (!TestNotNull(TEXT("blank editor map"), World)) return false;
    AMHCompositeActor* Authored = World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("authored placement"), Authored)) return false;
    Authored->SetAutoSeed(false);
    Authored->SetAutoAppearanceSeed(false);
    Authored->SetSeed(1234);
    Authored->SetCompositeAsset(Fixture.Asset);
    if (!TestNotNull(*Authored->GetLastPlacementError(), Authored->GetResolvedPlan()) ||
        !AppearanceMakeLegacyRecord(*this, *Authored)) return false;

    TArray<UPackage*> Packages;
    for (UObject* Object : Fixture.Assets) Packages.AddUnique(Object->GetOutermost());
    const FString MapPackage = TEXT("/Game/MimirS6/AppearanceMigration");
    if (!TestTrue(TEXT("fixture asset packages saved"), UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) ||
        !TestTrue(TEXT("legacy-shaped placement map saved"), UEditorLoadingAndSavingUtils::SaveMap(World, MapPackage)))
    {
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        return false;
    }
    UEditorLoadingAndSavingUtils::NewBlankMap(false);
    FString MapFile;
    if (!TestTrue(TEXT("map filename resolves"),
        FPackageName::TryConvertLongPackageNameToFilename(MapPackage, MapFile, FPackageName::GetMapPackageExtension())))
        return false;
    UWorld* Loaded = UEditorLoadingAndSavingUtils::LoadMap(MapFile);
    if (!TestNotNull(TEXT("reopened level"), Loaded))
    {
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        return false;
    }
    AMHCompositeActor* Reloaded = nullptr;
    for (TActorIterator<AMHCompositeActor> It(Loaded); It; ++It) { Reloaded = *It; break; }
    bool bPassed = TestNotNull(TEXT("reopened level still holds the placement"), Reloaded);
    if (Reloaded != nullptr)
    {
        const int32 Expected = MHDeriveAppearanceSeedFromLayoutSeed(1234);
        AddInfo(FString::Printf(TEXT("migrated placement seed=%d appearance=%d expected=%d stored=%d"),
            Reloaded->GetSeed(), Reloaded->GetAppearanceSeed(), Expected,
            Reloaded->HasStoredAppearanceSeed() ? 1 : 0));
        bPassed &= TestEqual(TEXT("legacy placement keeps its layout seed"), Reloaded->GetSeed(), 1234);
        bPassed &= TestTrue(TEXT("load materializes a stored AppearanceSeed"), Reloaded->HasStoredAppearanceSeed());
        bPassed &= TestEqual(TEXT("migration value is Mix(Seed, appearance)"), Reloaded->GetAppearanceSeed(), Expected);
        bPassed &= TestNotNull(*Reloaded->GetLastPlacementError(), Reloaded->GetResolvedPlan());
        if (Reloaded->GetResolvedPlan() != nullptr)
            bPassed &= TestEqual(TEXT("the plan resolves with the migrated seed"),
                Reloaded->GetResolvedPlan()->Appearance.AppearanceSeed, Expected);

        // Guard against a computed default or a getter fallback: three layout
        // rerolls must not move the stored appearance value by a single bit.
        for (const int32 NewSeed : {4321, 0, 77})
        {
            Reloaded->SetSeed(NewSeed);
            bPassed &= TestEqual(TEXT("a layout reseed never rerolls the appearance"),
                Reloaded->GetAppearanceSeed(), Expected);
            bPassed &= TestNotEqual(TEXT("the stored value is not recomputed from the current Seed"),
                Reloaded->GetAppearanceSeed(), MHDeriveAppearanceSeedFromLayoutSeed(NewSeed));
        }
        // A second load of an already migrated record must not migrate again.
        Reloaded->SetSeed(1234);
        Reloaded->PostLoad();
        Reloaded->PostRegisterAllComponents();
        bPassed &= TestEqual(TEXT("migration runs once, never on an already stored seed"),
            Reloaded->GetAppearanceSeed(), Expected);

        // The dirty flag. The editor's own map load clears the map package
        // after every actor has loaded, so it cannot be observed on the line
        // above; drive the same production path once more with the package
        // explicitly clean and the record put back into its legacy shape.
        if (AppearanceMakeLegacyRecord(*this, *Reloaded))
        {
            Reloaded->GetOutermost()->SetDirtyFlag(false);
            Reloaded->PostLoad();
            bPassed &= TestEqual(TEXT("PostLoad materializes the value itself"),
                Reloaded->GetAppearanceSeed(), Expected);
            Reloaded->PostRegisterAllComponents();
            bPassed &= TestTrue(TEXT("the migrating load marks its package dirty"),
                Reloaded->GetOutermost()->IsDirty());
        }
    }
    UEditorLoadingAndSavingUtils::NewBlankMap(false);
    return bPassed;
}

/** The duplicate gate mirrors the existing layout auto-seed, one gate per seed. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAppearanceAutoSeedTest,
    "Mimir.V5.Composite.Seed.AutoAppearanceSeedOnDuplicate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAppearanceAutoSeedTest::RunTest(const FString& Parameters)
{
    FAppearanceActorFixture Fixture(*this);
    if (!Fixture.Build()) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    bool bPassed = TestNotNull(TEXT("appearance seed test actor"), Actor);
    if (Actor == nullptr)
    {
        World->DestroyWorld(false);
        return false;
    }
    Actor->SetCompositeAsset(Fixture.Asset);
    bPassed &= TestTrue(TEXT("a new placement stores its own appearance seed"), Actor->HasStoredAppearanceSeed());
    bPassed &= TestNotEqual(TEXT("auto-created placement gets a non-zero appearance seed"), Actor->GetAppearanceSeed(), 0);
    bPassed &= TestTrue(TEXT("auto appearance seed on duplicate is the default"), Actor->GetAutoAppearanceSeed());
    Actor->SetSeed(100);
    Actor->SetAppearanceSeed(500);

    auto Duplicate = [&](const EDuplicateMode::Type Mode) -> AMHCompositeActor*
    {
        FObjectDuplicationParameters Parameters(Actor, World->PersistentLevel);
        Parameters.DestName = MakeUniqueObjectName(World->PersistentLevel, AMHCompositeActor::StaticClass());
        Parameters.DuplicateMode = Mode;
        return Cast<AMHCompositeActor>(StaticDuplicateObjectEx(Parameters));
    };

    AMHCompositeActor* Copy = Duplicate(EDuplicateMode::Normal);
    if (TestNotNull(TEXT("normal duplicate exists"), Copy))
    {
        bPassed &= TestNotEqual(TEXT("normal duplicate rerolls the appearance seed"), Copy->GetAppearanceSeed(), 500);
        bPassed &= TestNotEqual(TEXT("normal duplicate rerolls the layout seed"), Copy->GetSeed(), 100);
        bPassed &= TestTrue(TEXT("the duplicate stores its own appearance seed"), Copy->HasStoredAppearanceSeed());
        Copy->Destroy();
    }
    bPassed &= TestEqual(TEXT("duplication leaves the source appearance seed alone"), Actor->GetAppearanceSeed(), 500);

    // Each gate is independent: locking only the appearance keeps the painting
    // while the layout still rerolls, and the reverse holds as well.
    Actor->SetAutoAppearanceSeed(false);
    AMHCompositeActor* Locked = Duplicate(EDuplicateMode::Normal);
    if (TestNotNull(TEXT("appearance-locked duplicate exists"), Locked))
    {
        bPassed &= TestEqual(TEXT("locked appearance survives duplication"), Locked->GetAppearanceSeed(), 500);
        bPassed &= TestFalse(TEXT("the lock persists on the duplicate"), Locked->GetAutoAppearanceSeed());
        bPassed &= TestNotEqual(TEXT("the layout gate still rerolls independently"), Locked->GetSeed(), 100);
        Locked->Destroy();
    }
    Actor->SetAutoAppearanceSeed(true);
    Actor->SetAutoSeed(false);
    AMHCompositeActor* Repainted = Duplicate(EDuplicateMode::Normal);
    if (TestNotNull(TEXT("layout-locked duplicate exists"), Repainted))
    {
        bPassed &= TestEqual(TEXT("locked layout survives duplication"), Repainted->GetSeed(), 100);
        bPassed &= TestNotEqual(TEXT("the appearance gate still rerolls independently"), Repainted->GetAppearanceSeed(), 500);
        Repainted->Destroy();
    }
    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
