#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeCompiler.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHRuntimeCompositeActor.h"
#include "Composite/MHRuntimeCompositeInput.h"
#include "Components/ChildActorComponent.h"
#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Info.h"
#include "HAL/FileManager.h"
#include "Index/MHProjectResourceIndex.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceResolver.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{

TArray<uint8> MarkerTestUtf8(const TCHAR* Text)
{
    const FTCHARToUTF8 Utf8(Text);
    return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

class FMarkerNoSourceResolver final : public IMHSourceResolver
{
public:
    int32 ResolveCalls = 0;
    virtual FMHSourceSnapshot GetSnapshot() const override { return {}; }
    virtual FMHResolveOutcome Resolve(const FMHResourceKey& Key) override
    {
        ++ResolveCalls;
        return {};
    }
};

struct FMarkerFixture
{
    FAutomationTestBase& Test;
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    TMap<FString, FSoftClassPath> PreviousRegistry = Settings->ActorClassRegistry;
    TArray<UMHCompositeAsset*> Assets;

    explicit FMarkerFixture(FAutomationTestBase& InTest) : Test(InTest)
    {
        if (World != nullptr && GEngine != nullptr)
            GEngine->CreateNewWorldContext(EWorldType::EditorPreview).SetCurrentWorld(World);
    }

    ~FMarkerFixture()
    {
        if (World != nullptr)
        {
            World->DestroyWorld(false);
            if (GEngine != nullptr) GEngine->DestroyWorldContext(World);
        }
        Settings->ActorClassRegistry = PreviousRegistry;
        for (UMHCompositeAsset* Asset : Assets)
        {
            Asset->ClearFlags(RF_Public | RF_Standalone);
            Asset->MarkAsGarbage();
        }
    }

    FString Name(const TCHAR* Stem) const { return FString(Stem) + TEXT("_") + Suffix; }

    UMHCompositeAsset* Asset(const FMHCompositeDocument& Document, const TCHAR* Stem)
    {
        const FString LogicalName = Name(Stem);
        UMHCompositeAsset* Result = NewObject<UMHCompositeAsset>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + LogicalName)),
            FName(*LogicalName), RF_Public | RF_Standalone);
        Assets.Add(Result);
        Result->LogicalName = LogicalName;
        TArray<uint8> Bytes;
        FString Error;
        if (!MHApplyCompositeV5(*Result, Document, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(Error);
            return nullptr;
        }
        Result->SourceRelativePath = LogicalName + TEXT(".composite");
        Result->SourceHash = MHRawPayloadHash(Bytes);
        Result->AppliedHash = Result->SourceHash;
        return Result;
    }
};

int32 MarkerSpawnedChildCount(const AActor& Actor)
{
    TInlineComponentArray<UChildActorComponent*> Components;
    Actor.GetComponents(Components);
    int32 Result = 0;
    for (const UChildActorComponent* Component : Components)
        if (Component->GetChildActor() != nullptr) ++Result;
    return Result;
}

FMHCompositeDocument MarkerTestDocument(const FString& MarkerName)
{
    FMHCompositeDocument Document;
    FMHCompositeNode& Parent = Document.Nodes.AddDefaulted_GetRef();
    Parent.Name = TEXT("Marker parent");
    Parent.Transform.TranslationCm.X = 100.0;
    FMHCompositeNode& Marker = Parent.Children.AddDefaulted_GetRef();
    Marker.Kind = EMHCompositeNodeKind::Marker;
    Marker.Resource = MarkerName;
    Marker.Name = TEXT("Authored marker name");
    Marker.Transform.TranslationCm.X = 25.0;
    FMHCompositeNode& Random = Marker.Children.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Name = TEXT("Authored random name");
    Random.Transform.TranslationCm.Y = 10.0;
    FMHCompositeOption& Empty = Random.Options.AddDefaulted_GetRef();
    Empty.Kind = EMHCompositeOptionKind::Empty;
    Empty.Weight = 0.0f;
    FMHCompositeOption& Option = Random.Options.AddDefaulted_GetRef();
    Option.Kind = EMHCompositeOptionKind::Marker;
    Option.Resource = MarkerName;
    Option.Weight = 1.0f;
    return Document;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeMarkerCodecTest,
    "Mimir.V5.Composite.Marker.CodecRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeMarkerCodecTest::RunTest(const FString& Parameters)
{
    const TCHAR* Documents[] = {
        TEXT(R"({"v":5,"nodes":[{"kind":"marker","resource":"dummy_volumetric_box","name":"Authored marker","transform":{"translation_cm":[100,20,30]}},{"kind":"group","children":[{"kind":"marker","resource":"loot_box","transform":{"translation_cm":[25,0,0]}}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","name":"Authored choice","options":[{"kind":"marker","resource":"loot_box","weight":1},{"kind":"empty","weight":0}]}]})")
    };
    bool bPassed = true;
    for (const TCHAR* Json : Documents)
    {
        FMHCompositeDocument Document;
        FString Error;
        const bool bParsed = MHParseCompositeV5(MarkerTestUtf8(Json), Document, Error);
        bPassed &= TestTrue(TEXT("marker is admitted as an ordinary node or weighted option"), bParsed);
        if (!bParsed)
        {
            AddInfo(TEXT("MARKER_BASELINE_PARSE: ") + Error);
            continue;
        }
        TArray<uint8> Canonical;
        if (!TestTrue(TEXT("marker canonical writer succeeds"), MHWriteCanonicalCompositeV5(Document, Canonical, Error)))
        {
            AddError(Error);
            bPassed = false;
            continue;
        }
        FMHCompositeDocument Reparsed;
        TArray<uint8> Rewritten;
        bPassed &= TestTrue(TEXT("canonical marker parses"), MHParseCompositeV5(Canonical, Reparsed, Error));
        bPassed &= TestTrue(TEXT("canonical marker rewrites"), MHWriteCanonicalCompositeV5(Reparsed, Rewritten, Error));
        bPassed &= TestTrue(TEXT("marker canonical bytes round-trip exactly"), Canonical == Rewritten);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeMarkerClosedGrammarTest,
    "Mimir.V5.Composite.Marker.ClosedGrammar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeMarkerClosedGrammarTest::RunTest(const FString& Parameters)
{
    const TCHAR* Invalid[] = {
        TEXT(R"({"v":5,"nodes":[{"kind":"marker"}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"marker","resource":"Invalid-Name"}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"marker","resource":"one","resource":"two"}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"marker","weight":1}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"marker","resource":"marker","weight":1,"transform":{}}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"marker","resource":"marker","weight":1,"profile":"p"}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"marker","resource":"marker","weight":-1}]}]})")
    };
    for (const TCHAR* Json : Invalid)
    {
        FMHCompositeDocument Document;
        FString Error;
        TestFalse(TEXT("marker cannot weaken closed resource/option grammar"), MHParseCompositeV5(MarkerTestUtf8(Json), Document, Error));
        TestFalse(TEXT("invalid marker produces a diagnostic"), Error.IsEmpty());
    }
    FMHCompositeDocument Document = MarkerTestDocument(TEXT("marker"));
    Document.Nodes[0].Children[0].Resource.Reset();
    TArray<uint8> Bytes;
    FString Error;
    TestFalse(TEXT("writer rejects marker without canonical resource"), MHWriteCanonicalCompositeV5(Document, Bytes, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeMarkerAdmissionTest,
    "Mimir.V5.Composite.Marker.AdmissionIgnoresActorRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeMarkerAdmissionTest::RunTest(const FString& Parameters)
{
    FMarkerFixture Fixture(*this);
    const FString Token = Fixture.Name(TEXT("marker_identity"));
    const FMHCompositeDocument Document = MarkerTestDocument(Token);
    UMHCompositeAsset* Asset = Fixture.Asset(Document, TEXT("marker_admission"));
    if (Asset == nullptr) return false;
    FMHCompositeDocument Extracted;
    TArray<uint8> OriginalBytes;
    TArray<uint8> ExtractedBytes;
    FString Error;
    if (!TestTrue(TEXT("applied marker extracts"), MHExtractCompositeV5(*Asset, Extracted, Error)) ||
        !MHWriteCanonicalCompositeV5(Document, OriginalBytes, Error) || !MHWriteCanonicalCompositeV5(Extracted, ExtractedBytes, Error)) return false;
    TestTrue(TEXT("source-shaped apply/extract preserves marker identity/name/TRS exactly"), OriginalBytes == ExtractedBytes);

    // Unknown, invalid, and spawnable registry entries must be equally irrelevant.
    for (int32 RegistryCase = 0; RegistryCase < 3; ++RegistryCase)
    {
        Fixture.Settings->ActorClassRegistry.Remove(Token);
        if (RegistryCase == 1) Fixture.Settings->ActorClassRegistry.Add(Token, FSoftClassPath(AInfo::StaticClass()));
        if (RegistryCase == 2) Fixture.Settings->ActorClassRegistry.Add(Token, FSoftClassPath(AStaticMeshActor::StaticClass()));
        FMarkerNoSourceResolver Resolver;
        if (!TestTrue(TEXT("marker source admission is independent of ActorClassRegistry"),
            MHProbeCompositeBuildV5(Asset->LogicalName, Document, Resolver, *Fixture.Settings, Error)))
        {
            AddError(Error);
            return false;
        }
        TestEqual(TEXT("marker tokens never resolve source payloads"), Resolver.ResolveCalls, 0);
        FMHRandomSourceGraph Graph;
        TSet<FMHResourceKey> Dependencies;
        if (!TestTrue(TEXT("marker applied admission succeeds without any marker asset"),
            MHBuildAppliedCompositeGraph(*Asset, *Fixture.Settings, Graph, Dependencies, Error)))
        {
            AddError(Error);
            return false;
        }
        TestEqual(TEXT("only the root composite is a dependency"), Dependencies.Num(), 1);
        FMHResolvedCompositePlan Plan;
        if (!TestTrue(TEXT("marker plan resolves"), MHResolveCompositePlan(Graph, 100, Plan, Error))) return false;
        TestTrue(TEXT("ordinary and selected markers emit no leaves"), Plan.Leaves.IsEmpty());
        TestEqual(TEXT("marker tokens do not enter source closure"), Plan.Closure.Resources.Num(), 1);
        TestTrue(TEXT("marker-only graph has no SelectedDependencies"), Plan.SelectedDependencies.IsEmpty());
        if (!TestEqual(TEXT("group/ordinary marker/random/selected marker metadata"), Plan.Nodes.Num(), 4) ||
            !TestEqual(TEXT("random marker is selected once"), Plan.Decisions.Num(), 1)) return false;
        TestEqual(TEXT("zero-weight empty is not selected"), Plan.Decisions[0].OptionIndex, 1);
        TestTrue(TEXT("ordinary marker retains semantic kind"), Plan.Nodes[1].SemanticKind == EMHRandomSemanticKind::Marker);
        TestEqual(TEXT("ordinary marker retains resource token"), Plan.Nodes[1].Resource, Token);
        TestEqual(TEXT("ordinary marker retains display name"), Plan.Nodes[1].DisplayName, FString(TEXT("Authored marker name")));
        TestEqual(TEXT("parent100 plus marker local25 gives world125"), Plan.Nodes[1].WorldMatrix.GetOrigin().X, 125.0);
        TestEqual(TEXT("selected marker has the option NodePath"), Plan.Nodes[3].NodePath,
            Plan.Decisions[0].NodePath + TEXT("/options[1]"));
        TestTrue(TEXT("selected marker retains semantic kind"), Plan.Nodes[3].SemanticKind == EMHRandomSemanticKind::Marker);
        TestEqual(TEXT("selected marker retains identity independently of random name"), Plan.Nodes[3].Resource, Token);
        TestEqual(TEXT("selected marker retains random display name"), Plan.Nodes[3].DisplayName, FString(TEXT("Authored random name")));
        TestTrue(TEXT("option world equals resolved random-node world"), Plan.Nodes[3].WorldMatrix.Equals(Plan.Nodes[2].WorldMatrix, 0.0));
        TestTrue(TEXT("option carries no authored local transform"), Plan.Nodes[3].AuthoredLocalTrs.TranslationCm.IsZero());
        TestEqual(TEXT("marker does not consume an extra draw"), Plan.Draws.Num(), 1);
        FMHRandomStream1 Stream = MHMakeNodeRandomStream(100, Plan.Decisions[0].NodePath);
        TestEqual(TEXT("selection draw stays path-derived"), Plan.Decisions[0].RawU32, Stream.NextU32());
        const TArray<uint8> Preimage = Plan.SignaturePreimage;
        const FString Signature = Plan.ResolvedSignature;
        Plan.Nodes[1].Resource = TEXT("derived_metadata_only");
        Plan.Nodes[3].DisplayName = TEXT("Presentation only");
        MHRefreshResolvedCompositeSignature(Plan);
        TestTrue(TEXT("derived marker metadata never changes signature bytes"), Plan.SignaturePreimage == Preimage);
        TestEqual(TEXT("signature hash is unchanged by derived metadata"), Plan.ResolvedSignature, Signature);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeMarkerRuntimeTransportTest,
    "Mimir.V5.Composite.Marker.RuntimeTransportAndProfile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeMarkerRuntimeTransportTest::RunTest(const FString& Parameters)
{
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Mesh) == 0);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Actor) == 1);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Composite) == 2);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Group) == 3);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Random) == 4);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Empty) == 5);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Marker) == 6);
    FMarkerFixture Fixture(*this);
    UMHCompositeAsset* Asset = Fixture.Asset(MarkerTestDocument(TEXT("runtime_marker")), TEXT("marker_runtime"));
    if (Asset == nullptr) return false;
    FMHRandomSourceGraph Graph;
    TSet<FMHResourceKey> Dependencies;
    FString Error;
    if (!MHBuildAppliedCompositeGraph(*Asset, *Fixture.Settings, Graph, Dependencies, Error)) return false;
    FMHRandomNode& Marker = Graph.Composites.FindChecked(Graph.RootComposite).Nodes[0].Children[0];
    Marker.Profile = TEXT("marker_profile");
    FMHRandomPlacementProfile Profile;
    Profile.Name = Marker.Profile;
    Profile.bHasOffsetCm = true;
    Profile.OffsetCm[0] = {2.0f, 1.0f};
    Graph.Profiles.Add(Profile.Name, Profile);
    Graph.RawHashes.Add(TEXT("placement_profile:") + Profile.Name, MHRawPayloadHash(MarkerTestUtf8(TEXT("profile fixture"))));

    FMHRuntimeCompositeInput Input;
    if (!TestTrue(TEXT("marker runtime input encodes"), MHEncodeRuntimeCompositeGraph(Graph, Input.GraphBytes, Error))) return false;
    FMHRandomSourceGraph Decoded;
    TArray<uint8> Reencoded;
    if (!TestTrue(TEXT("marker runtime input decodes"), MHDecodeRuntimeCompositeGraph(Input.GraphBytes, Decoded, Error)) ||
        !MHEncodeRuntimeCompositeGraph(Decoded, Reencoded, Error)) return false;
    TestTrue(TEXT("runtime bytes are deterministic and lossless"), Input.GraphBytes == Reencoded);
    TArray<FString> BindingKeys;
    TestTrue(TEXT("marker graph collects cook bindings"), MHCollectRuntimeCompositeBindingKeys(Decoded, BindingKeys, Error));
    TestTrue(TEXT("marker and source-only profile need no runtime endpoint bindings"), BindingKeys.IsEmpty());
    TestTrue(TEXT("empty binding set is complete for marker-only graph"), MHValidateRuntimeCompositeBindings(Decoded, Input.Bindings, Error));
    FMHRuntimeCompositeBinding Phantom;
    Phantom.ResourceKey = TEXT("actor:runtime_marker");
    Phantom.Object = AStaticMeshActor::StaticClass();
    const TArray<FMHRuntimeCompositeBinding> PhantomBindings = {Phantom};
    TestFalse(TEXT("phantom actor binding for marker is rejected"), MHValidateRuntimeCompositeBindings(Decoded, PhantomBindings, Error));

    // A node resource is serialized as Kind + little-endian byte-count + UTF-8.
    // Corrupt the first marker's kind in valid bytes to exercise the reader,
    // not only the semantic validation that precedes writing.
    const TArray<uint8> Needle = MarkerTestUtf8(TEXT("runtime_marker"));
    int32 TokenOffset = INDEX_NONE;
    for (int32 Offset = 5; Offset + Needle.Num() <= Input.GraphBytes.Num(); ++Offset)
    {
        bool bMatches = true;
        for (int32 Index = 0; Index < Needle.Num(); ++Index)
            bMatches &= Input.GraphBytes[Offset + Index] == Needle[Index];
        if (bMatches) { TokenOffset = Offset; break; }
    }
    if (!TestTrue(TEXT("marker transport token is located"), TokenOffset >= 5)) return false;
    TestEqual(TEXT("marker uses appended ordinal six"), Input.GraphBytes[TokenOffset - 5], static_cast<uint8>(6));
    for (const uint8 InvalidKind : {static_cast<uint8>(5), static_cast<uint8>(255)})
    {
        TArray<uint8> Corrupt = Input.GraphBytes;
        Corrupt[TokenOffset - 5] = InvalidKind;
        FMHRandomSourceGraph Refused;
        TestFalse(TEXT("reader rejects ordinary Empty and unknown kind bytes"), MHDecodeRuntimeCompositeGraph(Corrupt, Refused, Error));
    }

    FMHResolvedCompositePlan Before;
    FMHResolvedCompositePlan After;
    if (!MHResolveCompositePlan(Graph, 100, Before, Error) || !MHResolveCompositePlan(Decoded, 100, After, Error)) return false;
    TestEqual(TEXT("runtime transport preserves signature"), After.ResolvedSignature, Before.ResolvedSignature);
    TestTrue(TEXT("runtime transport preserves signature preimage"), After.SignaturePreimage == Before.SignaturePreimage);
    TestEqual(TEXT("marker profile samples XYZ; random selection consumes one"), After.Draws.Num(), 4);
    TestTrue(TEXT("profiled markers still emit no leaves"), After.Leaves.IsEmpty());
    FMHRandomStream1 ProfileStream = MHMakeNodeRandomStream(100, Before.Nodes[1].NodePath);
    for (int32 Index = 0; Index < 3; ++Index)
        TestEqual(TEXT("profile draws use the ordinary marker's independent stream"), Before.Draws[Index].RawU32, ProfileStream.NextU32());

    FMHRandomSourceGraph Invalid = Decoded;
    Invalid.Composites.FindChecked(Invalid.RootComposite).Nodes[0].Kind = EMHRandomSemanticKind::Empty;
    TestFalse(TEXT("appending marker must not admit ordinary Empty"), MHEncodeRuntimeCompositeGraph(Invalid, Reencoded, Error));
    Invalid = Decoded;
    Invalid.Composites.FindChecked(Invalid.RootComposite).Nodes[0].Children[0].Children[0].Options[1].Kind = EMHRandomSemanticKind::Group;
    TestFalse(TEXT("appending marker must not admit Group as an option"), MHEncodeRuntimeCompositeGraph(Invalid, Reencoded, Error));
    Invalid = Decoded;
    Invalid.Composites.FindChecked(Invalid.RootComposite).Nodes[0].Children[0].Resource.Reset();
    TestFalse(TEXT("runtime marker still requires a canonical token"), MHEncodeRuntimeCompositeGraph(Invalid, Reencoded, Error));

    if (!TestNotNull(TEXT("runtime marker test world"), Fixture.World)) return false;
    AMHRuntimeCompositeActor* Runtime = Fixture.World->SpawnActor<AMHRuntimeCompositeActor>();
    if (!TestNotNull(TEXT("runtime marker placement exists"), Runtime)) return false;
    if (!TestTrue(TEXT("runtime marker-only placement configures"), Runtime->Configure(Input, 100, Error)))
    {
        AddError(Error);
        return false;
    }
    TestEqual(TEXT("runtime marker produces no components"), Runtime->GetMaterializedComponents().Num(), 0);
    TestEqual(TEXT("runtime marker produces no child actor"), MarkerSpawnedChildCount(*Runtime), 0);
    if (!TestNotNull(TEXT("runtime exposes the same semantic plan"), Runtime->GetResolvedPlan())) return false;
    TestEqual(TEXT("runtime result signature agrees with reference entrypoint"), Runtime->GetResolvedPlan()->ResolvedSignature, Before.ResolvedSignature);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeMarkerPreviewNoSpawnTest,
    "Mimir.V5.Composite.Marker.PreviewNoSpawnAndActorUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeMarkerPreviewNoSpawnTest::RunTest(const FString& Parameters)
{
    FMarkerFixture Fixture(*this);
    if (!TestNotNull(TEXT("preview marker test world"), Fixture.World)) return false;
    const FString Token = Fixture.Name(TEXT("marker_collision"));
    Fixture.Settings->ActorClassRegistry.Add(Token, FSoftClassPath(AStaticMeshActor::StaticClass()));
    UMHCompositeAsset* MarkerAsset = Fixture.Asset(MarkerTestDocument(Token), TEXT("marker_preview"));
    if (MarkerAsset == nullptr) return false;
    AMHCompositeActor* Preview = Fixture.World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("preview placement exists"), Preview)) return false;
    Preview->SetSeed(100);
    Preview->SetCompositeAsset(MarkerAsset);
    if (!TestNotNull(TEXT("preview admits marker-only composite"), Preview->GetResolvedPlan()))
    {
        AddError(Preview->GetLastPlacementError());
        return false;
    }
    TestEqual(TEXT("preview never spawns actor for a colliding marker token"), MarkerSpawnedChildCount(*Preview), 0);
    TestEqual(TEXT("marker has no leaf components; root authoring handle remains"), Preview->GetDerivedComponents().Num(), 1);

    FMHCompositeDocument ActorDocument;
    FMHCompositeNode& ActorNode = ActorDocument.Nodes.AddDefaulted_GetRef();
    ActorNode.Kind = EMHCompositeNodeKind::Actor;
    ActorNode.Resource = Token;
    UMHCompositeAsset* ActorAsset = Fixture.Asset(ActorDocument, TEXT("marker_actor_control"));
    if (ActorAsset == nullptr) return false;
    Preview->SetCompositeAsset(ActorAsset);
    if (!TestNotNull(TEXT("native actor kind remains admitted"), Preview->GetResolvedPlan())) return false;
    TestEqual(TEXT("native actor retains one leaf"), Preview->GetResolvedPlan()->Leaves.Num(), 1);
    TestEqual(TEXT("native actor still spawns the registered class"), MarkerSpawnedChildCount(*Preview), 1);

    FMHRandomSourceGraph Graph;
    TSet<FMHResourceKey> Dependencies;
    FString Error;
    if (!MHBuildAppliedCompositeGraph(*ActorAsset, *Fixture.Settings, Graph, Dependencies, Error)) return false;
    FMHRuntimeCompositeInput Input;
    if (!MHEncodeRuntimeCompositeGraph(Graph, Input.GraphBytes, Error)) return false;
    FMHRuntimeCompositeBinding& Binding = Input.Bindings.AddDefaulted_GetRef();
    Binding.ResourceKey = TEXT("actor:") + Token;
    Binding.Object = AStaticMeshActor::StaticClass();
    AMHRuntimeCompositeActor* Runtime = Fixture.World->SpawnActor<AMHRuntimeCompositeActor>();
    if (!TestNotNull(TEXT("actor-control runtime placement exists"), Runtime) ||
        !TestTrue(TEXT("native actor runtime remains admitted"), Runtime->Configure(Input, 100, Error))) return false;
    TestEqual(TEXT("native actor still spawns at runtime"), MarkerSpawnedChildCount(*Runtime), 1);

    Fixture.Settings->ActorClassRegistry.Remove(Token);
    FMarkerNoSourceResolver Resolver;
    TestFalse(TEXT("unregistered native actor still fails source admission"),
        MHProbeCompositeBuildV5(ActorAsset->LogicalName, ActorDocument, Resolver, *Fixture.Settings, Error));
    TestTrue(TEXT("native actor rejection remains explicit"), Error.Contains(TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeMarkerIndexTest,
    "Mimir.V5.Composite.Marker.IndexHasNoMarkerEdges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeMarkerIndexTest::RunTest(const FString& Parameters)
{
    struct FMarkerIndexFiles
    {
        FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests"),
            TEXT("MarkerIndex_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
        ~FMarkerIndexFiles() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
    } Files;
    if (!TestTrue(TEXT("isolated marker index directory exists"), IFileManager::Get().MakeDirectory(*Files.Root, true))) return false;
    FMHCompositeDocument Document = MarkerTestDocument(TEXT("not_a_resource_key"));
    FMHCompositeNode& Marker = Document.Nodes[0].Children[0];
    Marker.Profile = TEXT("marker_profile");
    FMHCompositeOption& Unselected = Marker.Children[0].Options.AddDefaulted_GetRef();
    Unselected.Kind = EMHCompositeOptionKind::Composite;
    Unselected.Resource = TEXT("marker_child");
    Unselected.Weight = 0.0f;
    FString Error;
    TArray<uint8> Bytes;
    if (!MHWriteCanonicalCompositeV5(Document, Bytes, Error) ||
        !FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(Files.Root, TEXT("marker_index_root.composite")))) return false;
    if (!FFileHelper::SaveArrayToFile(MarkerTestUtf8(TEXT(R"({"v":5,"nodes":[]})")),
        *FPaths::Combine(Files.Root, TEXT("marker_child.composite"))) ||
        !FFileHelper::SaveArrayToFile(MarkerTestUtf8(TEXT(R"({"v":1,"kind":"placement_profile"})")),
        *FPaths::Combine(Files.Root, TEXT("marker_profile.placement")))) return false;
    FMHProjectResourceIndex Index(Files.Root, FPaths::Combine(Files.Root, TEXT("cache/ProjectIndex.sqlite")));
    bool bRecreated = false;
    FMHProjectIndexUpdateResult Update;
    if (!TestTrue(TEXT("marker test index opens"), Index.Open(bRecreated, Error)) ||
        !TestTrue(TEXT("full scan accepts marker grammar"), Index.FullScan({}, Update, Error)))
    {
        AddError(Error);
        return false;
    }
    FMHResourceKey RootKey;
    RootKey.Kind = EMHResourceKind::Composite;
    RootKey.LogicalName = TEXT("marker_index_root");
    TestFalse(TEXT("marker token never creates an unresolved dependency"), Index.IsImportBlocked(RootKey, Error));
    FString Dump;
    if (!TestTrue(TEXT("index normalized projection is available"), Index.BuildNormalizedDump(Dump, Error))) return false;
    TestFalse(TEXT("marker identity is neither ResourceKey, edge nor diagnostic"), Dump.Contains(TEXT("not_a_resource_key")));
    TestTrue(TEXT("zero-weight real composite dependency below marker remains in closure"),
        Dump.Contains(TEXT("Dependencies\tcomposite\tmarker_index_root\tcomposite\tmarker_child\tplacement_composite")));
    TestTrue(TEXT("marker profile remains a real dependency"),
        Dump.Contains(TEXT("Dependencies\tcomposite\tmarker_index_root\tplacement_profile\tmarker_profile\tprofile")));
    TestTrue(TEXT("remove only the test's referenced child file"),
        IFileManager::Get().Delete(*FPaths::Combine(Files.Root, TEXT("marker_child.composite")), false, true));
    if (!Index.FullScan({}, Update, Error)) return false;
    TestTrue(TEXT("unselected missing real dependency below marker still blocks root"), Index.IsImportBlocked(RootKey, Error));
    TestTrue(TEXT("real dependency refusal names the absent composite"), Error.Contains(TEXT("marker_child")));
    return true;
}

} // namespace UE::MimirComposite::Tests
