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

TArray<uint8> GameObjTestUtf8(const TCHAR* Text)
{
    const FTCHARToUTF8 Utf8(Text);
    return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

class FGameObjNoSourceResolver final : public IMHSourceResolver
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

struct FGameObjFixture
{
    FAutomationTestBase& Test;
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    TMap<FString, FSoftClassPath> PreviousRegistry = Settings->ActorClassRegistry;
    TArray<UMHCompositeAsset*> Assets;

    explicit FGameObjFixture(FAutomationTestBase& InTest) : Test(InTest)
    {
        if (World != nullptr && GEngine != nullptr)
            GEngine->CreateNewWorldContext(EWorldType::EditorPreview).SetCurrentWorld(World);
    }

    ~FGameObjFixture()
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

int32 GameObjSpawnedChildCount(const AActor& Actor)
{
    TInlineComponentArray<UChildActorComponent*> Components;
    Actor.GetComponents(Components);
    int32 Result = 0;
    for (const UChildActorComponent* Component : Components)
        if (Component->GetChildActor() != nullptr) ++Result;
    return Result;
}

FMHCompositeDocument GameObjTestDocument(const FString& GameObjName)
{
    FMHCompositeDocument Document;
    FMHCompositeNode& Parent = Document.Nodes.AddDefaulted_GetRef();
    Parent.Name = TEXT("GameObj parent");
    Parent.Transform.TranslationCm.X = 100.0;
    FMHCompositeNode& GameObj = Parent.Children.AddDefaulted_GetRef();
    GameObj.Kind = EMHCompositeNodeKind::GameObj;
    GameObj.Resource = GameObjName;
    GameObj.Name = TEXT("Authored gameobj name");
    GameObj.Transform.TranslationCm.X = 25.0;
    FMHCompositeNode& Random = GameObj.Children.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Name = TEXT("Authored random name");
    Random.Transform.TranslationCm.Y = 10.0;
    FMHCompositeOption& Empty = Random.Options.AddDefaulted_GetRef();
    Empty.Kind = EMHCompositeOptionKind::Empty;
    Empty.Weight = 0.0f;
    FMHCompositeOption& Option = Random.Options.AddDefaulted_GetRef();
    Option.Kind = EMHCompositeOptionKind::GameObj;
    Option.Resource = GameObjName;
    Option.Weight = 1.0f;
    return Document;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeGameObjCodecTest,
    "Mimir.V5.Composite.GameObj.CodecRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeGameObjCodecTest::RunTest(const FString& Parameters)
{
    const TCHAR* Documents[] = {
        TEXT(R"({"v":5,"nodes":[{"kind":"gameobj","resource":"dummy_volumetric_box","name":"Authored gameobj","transform":{"translation_cm":[100,20,30]}},{"kind":"group","children":[{"kind":"gameobj","resource":"loot_box","transform":{"translation_cm":[25,0,0]}}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","name":"Authored choice","options":[{"kind":"gameobj","resource":"loot_box","weight":1},{"kind":"empty","weight":0}]}]})")
    };
    bool bPassed = true;
    for (const TCHAR* Json : Documents)
    {
        FMHCompositeDocument Document;
        FString Error;
        const bool bParsed = MHParseCompositeV5(GameObjTestUtf8(Json), Document, Error);
        bPassed &= TestTrue(TEXT("gameobj is admitted as an ordinary node or weighted option"), bParsed);
        if (!bParsed)
        {
            AddInfo(TEXT("GAMEOBJ_BASELINE_PARSE: ") + Error);
            continue;
        }
        TArray<uint8> Canonical;
        if (!TestTrue(TEXT("gameobj canonical writer succeeds"), MHWriteCanonicalCompositeV5(Document, Canonical, Error)))
        {
            AddError(Error);
            bPassed = false;
            continue;
        }
        FMHCompositeDocument Reparsed;
        TArray<uint8> Rewritten;
        bPassed &= TestTrue(TEXT("canonical gameobj parses"), MHParseCompositeV5(Canonical, Reparsed, Error));
        bPassed &= TestTrue(TEXT("canonical gameobj rewrites"), MHWriteCanonicalCompositeV5(Reparsed, Rewritten, Error));
        bPassed &= TestTrue(TEXT("gameobj canonical bytes round-trip exactly"), Canonical == Rewritten);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeGameObjClosedGrammarTest,
    "Mimir.V5.Composite.GameObj.ClosedGrammar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeGameObjClosedGrammarTest::RunTest(const FString& Parameters)
{
    const TCHAR* Invalid[] = {
        TEXT(R"({"v":5,"nodes":[{"kind":"gameobj"}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"gameobj","resource":"Invalid-Name"}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"gameobj","resource":"one","resource":"two"}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"gameobj","weight":1}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"gameobj","resource":"gameobj","weight":1,"transform":{}}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"gameobj","resource":"gameobj","weight":1,"profile":"p"}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"gameobj","resource":"gameobj","weight":-1}]}]})")
    };
    for (const TCHAR* Json : Invalid)
    {
        FMHCompositeDocument Document;
        FString Error;
        TestFalse(TEXT("gameobj cannot weaken closed resource/option grammar"), MHParseCompositeV5(GameObjTestUtf8(Json), Document, Error));
        TestFalse(TEXT("invalid gameobj produces a diagnostic"), Error.IsEmpty());
    }
    FMHCompositeDocument Document = GameObjTestDocument(TEXT("gameobj"));
    Document.Nodes[0].Children[0].Resource.Reset();
    TArray<uint8> Bytes;
    FString Error;
    TestFalse(TEXT("writer rejects gameobj without canonical resource"), MHWriteCanonicalCompositeV5(Document, Bytes, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeGameObjAdmissionTest,
    "Mimir.V5.Composite.GameObj.AdmissionIgnoresActorRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeGameObjAdmissionTest::RunTest(const FString& Parameters)
{
    FGameObjFixture Fixture(*this);
    const FString Token = Fixture.Name(TEXT("gameobj_identity"));
    const FMHCompositeDocument Document = GameObjTestDocument(Token);
    UMHCompositeAsset* Asset = Fixture.Asset(Document, TEXT("gameobj_admission"));
    if (Asset == nullptr) return false;
    FMHCompositeDocument Extracted;
    TArray<uint8> OriginalBytes;
    TArray<uint8> ExtractedBytes;
    FString Error;
    if (!TestTrue(TEXT("applied gameobj extracts"), MHExtractCompositeV5(*Asset, Extracted, Error)) ||
        !MHWriteCanonicalCompositeV5(Document, OriginalBytes, Error) || !MHWriteCanonicalCompositeV5(Extracted, ExtractedBytes, Error)) return false;
    TestTrue(TEXT("source-shaped apply/extract preserves gameobj identity/name/TRS exactly"), OriginalBytes == ExtractedBytes);

    // Unknown, invalid, and spawnable registry entries must be equally irrelevant.
    for (int32 RegistryCase = 0; RegistryCase < 3; ++RegistryCase)
    {
        Fixture.Settings->ActorClassRegistry.Remove(Token);
        if (RegistryCase == 1) Fixture.Settings->ActorClassRegistry.Add(Token, FSoftClassPath(AInfo::StaticClass()));
        if (RegistryCase == 2) Fixture.Settings->ActorClassRegistry.Add(Token, FSoftClassPath(AStaticMeshActor::StaticClass()));
        FGameObjNoSourceResolver Resolver;
        if (!TestTrue(TEXT("gameobj source admission is independent of ActorClassRegistry"),
            MHProbeCompositeBuildV5(Asset->LogicalName, Document, Resolver, *Fixture.Settings, Error)))
        {
            AddError(Error);
            return false;
        }
        TestEqual(TEXT("gameobj tokens never resolve source payloads"), Resolver.ResolveCalls, 0);
        FMHRandomSourceGraph Graph;
        TSet<FMHResourceKey> Dependencies;
        if (!TestTrue(TEXT("gameobj applied admission succeeds without any gameobj asset"),
            MHBuildAppliedCompositeGraph(*Asset, *Fixture.Settings, Graph, Dependencies, Error)))
        {
            AddError(Error);
            return false;
        }
        TestEqual(TEXT("only the root composite is a dependency"), Dependencies.Num(), 1);
        FMHResolvedCompositePlan Plan;
        if (!TestTrue(TEXT("gameobj plan resolves"), MHResolveCompositePlan(Graph, 100, 700, Plan, Error))) return false;
        TestTrue(TEXT("ordinary and selected gameobjs emit no leaves"), Plan.Leaves.IsEmpty());
        TestEqual(TEXT("gameobj tokens do not enter source closure"), Plan.Closure.Resources.Num(), 1);
        TestTrue(TEXT("gameobj-only graph has no SelectedDependencies"), Plan.SelectedDependencies.IsEmpty());
        if (!TestEqual(TEXT("group/ordinary gameobj/random/selected gameobj metadata"), Plan.Nodes.Num(), 4) ||
            !TestEqual(TEXT("random gameobj is selected once"), Plan.Decisions.Num(), 1)) return false;
        TestEqual(TEXT("zero-weight empty is not selected"), Plan.Decisions[0].OptionIndex, 1);
        TestTrue(TEXT("ordinary gameobj retains semantic kind"), Plan.Nodes[1].SemanticKind == EMHRandomSemanticKind::GameObj);
        TestEqual(TEXT("ordinary gameobj retains resource token"), Plan.Nodes[1].Resource, Token);
        TestEqual(TEXT("ordinary gameobj retains display name"), Plan.Nodes[1].DisplayName, FString(TEXT("Authored gameobj name")));
        TestEqual(TEXT("parent100 plus gameobj local25 gives world125"), Plan.Nodes[1].WorldMatrix.GetOrigin().X, 125.0);
        TestEqual(TEXT("selected gameobj has the option NodePath"), Plan.Nodes[3].NodePath,
            Plan.Decisions[0].NodePath + TEXT("/options[1]"));
        TestTrue(TEXT("selected gameobj retains semantic kind"), Plan.Nodes[3].SemanticKind == EMHRandomSemanticKind::GameObj);
        TestEqual(TEXT("selected gameobj retains identity independently of random name"), Plan.Nodes[3].Resource, Token);
        TestEqual(TEXT("selected gameobj retains random display name"), Plan.Nodes[3].DisplayName, FString(TEXT("Authored random name")));
        TestTrue(TEXT("option world equals resolved random-node world"), Plan.Nodes[3].WorldMatrix.Equals(Plan.Nodes[2].WorldMatrix, 0.0));
        TestTrue(TEXT("option carries no authored local transform"), Plan.Nodes[3].AuthoredLocalTrs.TranslationCm.IsZero());
        TestEqual(TEXT("gameobj does not consume an extra draw"), Plan.Draws.Num(), 1);
        FMHRandomStream1 Stream = MHMakeNodeRandomStream(100, Plan.Decisions[0].NodePath);
        TestEqual(TEXT("selection draw stays path-derived"), Plan.Decisions[0].RawU32, Stream.NextU32());
        const TArray<uint8> Preimage = Plan.SignaturePreimage;
        const FString Signature = Plan.ResolvedSignature;
        Plan.Nodes[1].Resource = TEXT("derived_metadata_only");
        Plan.Nodes[3].DisplayName = TEXT("Presentation only");
        MHRefreshResolvedCompositeSignature(Plan);
        TestTrue(TEXT("derived gameobj metadata never changes signature bytes"), Plan.SignaturePreimage == Preimage);
        TestEqual(TEXT("signature hash is unchanged by derived metadata"), Plan.ResolvedSignature, Signature);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeGameObjRuntimeTransportTest,
    "Mimir.V5.Composite.GameObj.RuntimeTransportAndProfile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeGameObjRuntimeTransportTest::RunTest(const FString& Parameters)
{
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Mesh) == 0);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Actor) == 1);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Composite) == 2);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Group) == 3);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Random) == 4);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::Empty) == 5);
    static_assert(static_cast<uint8>(EMHRandomSemanticKind::GameObj) == 6);
    FGameObjFixture Fixture(*this);
    UMHCompositeAsset* Asset = Fixture.Asset(GameObjTestDocument(TEXT("runtime_gameobj")), TEXT("gameobj_runtime"));
    if (Asset == nullptr) return false;
    FMHRandomSourceGraph Graph;
    TSet<FMHResourceKey> Dependencies;
    FString Error;
    if (!MHBuildAppliedCompositeGraph(*Asset, *Fixture.Settings, Graph, Dependencies, Error)) return false;
    FMHRandomNode& GameObj = Graph.Composites.FindChecked(Graph.RootComposite).Nodes[0].Children[0];
    GameObj.Profile = TEXT("gameobj_profile");
    FMHRandomPlacementProfile Profile;
    Profile.Name = GameObj.Profile;
    Profile.bHasOffsetCm = true;
    Profile.OffsetCm[0] = {2.0f, 1.0f};
    Graph.Profiles.Add(Profile.Name, Profile);
    Graph.RawHashes.Add(TEXT("placement_profile:") + Profile.Name, MHRawPayloadHash(GameObjTestUtf8(TEXT("profile fixture"))));

    FMHRuntimeCompositeInput Input;
    if (!TestTrue(TEXT("gameobj runtime input encodes"), MHEncodeRuntimeCompositeGraph(Graph, Input.GraphBytes, Error))) return false;
    FMHRandomSourceGraph Decoded;
    TArray<uint8> Reencoded;
    if (!TestTrue(TEXT("gameobj runtime input decodes"), MHDecodeRuntimeCompositeGraph(Input.GraphBytes, Decoded, Error)) ||
        !MHEncodeRuntimeCompositeGraph(Decoded, Reencoded, Error)) return false;
    TestTrue(TEXT("runtime bytes are deterministic and lossless"), Input.GraphBytes == Reencoded);
    TArray<FString> BindingKeys;
    TestTrue(TEXT("gameobj graph collects cook bindings"), MHCollectRuntimeCompositeBindingKeys(Decoded, BindingKeys, Error));
    TestTrue(TEXT("gameobj and source-only profile need no runtime endpoint bindings"), BindingKeys.IsEmpty());
    TestTrue(TEXT("empty binding set is complete for gameobj-only graph"), MHValidateRuntimeCompositeBindings(Decoded, Input.Bindings, Error));
    FMHRuntimeCompositeBinding Phantom;
    Phantom.ResourceKey = TEXT("actor:runtime_gameobj");
    Phantom.Object = AStaticMeshActor::StaticClass();
    const TArray<FMHRuntimeCompositeBinding> PhantomBindings = {Phantom};
    TestFalse(TEXT("phantom actor binding for gameobj is rejected"), MHValidateRuntimeCompositeBindings(Decoded, PhantomBindings, Error));

    // A node resource is serialized as Kind + little-endian byte-count + UTF-8.
    // Corrupt the first gameobj's kind in valid bytes to exercise the reader,
    // not only the semantic validation that precedes writing.
    const TArray<uint8> Needle = GameObjTestUtf8(TEXT("runtime_gameobj"));
    int32 TokenOffset = INDEX_NONE;
    for (int32 Offset = 5; Offset + Needle.Num() <= Input.GraphBytes.Num(); ++Offset)
    {
        bool bMatches = true;
        for (int32 Index = 0; Index < Needle.Num(); ++Index)
            bMatches &= Input.GraphBytes[Offset + Index] == Needle[Index];
        if (bMatches) { TokenOffset = Offset; break; }
    }
    if (!TestTrue(TEXT("gameobj transport token is located"), TokenOffset >= 5)) return false;
    TestEqual(TEXT("gameobj uses appended ordinal six"), Input.GraphBytes[TokenOffset - 5], static_cast<uint8>(6));
    for (const uint8 InvalidKind : {static_cast<uint8>(5), static_cast<uint8>(255)})
    {
        TArray<uint8> Corrupt = Input.GraphBytes;
        Corrupt[TokenOffset - 5] = InvalidKind;
        FMHRandomSourceGraph Refused;
        TestFalse(TEXT("reader rejects ordinary Empty and unknown kind bytes"), MHDecodeRuntimeCompositeGraph(Corrupt, Refused, Error));
    }

    FMHResolvedCompositePlan Before;
    FMHResolvedCompositePlan After;
    if (!MHResolveCompositePlan(Graph, 100, 700, Before, Error) || !MHResolveCompositePlan(Decoded, 100, 700, After, Error)) return false;
    TestEqual(TEXT("runtime transport preserves signature"), After.ResolvedSignature, Before.ResolvedSignature);
    TestTrue(TEXT("runtime transport preserves signature preimage"), After.SignaturePreimage == Before.SignaturePreimage);
    TestEqual(TEXT("gameobj profile samples XYZ; random selection consumes one"), After.Draws.Num(), 4);
    TestTrue(TEXT("profiled gameobjs still emit no leaves"), After.Leaves.IsEmpty());
    FMHRandomStream1 ProfileStream = MHMakeNodeRandomStream(100, Before.Nodes[1].NodePath);
    for (int32 Index = 0; Index < 3; ++Index)
        TestEqual(TEXT("profile draws use the ordinary gameobj's independent stream"), Before.Draws[Index].RawU32, ProfileStream.NextU32());

    FMHRandomSourceGraph Invalid = Decoded;
    Invalid.Composites.FindChecked(Invalid.RootComposite).Nodes[0].Kind = EMHRandomSemanticKind::Empty;
    TestFalse(TEXT("appending gameobj must not admit ordinary Empty"), MHEncodeRuntimeCompositeGraph(Invalid, Reencoded, Error));
    Invalid = Decoded;
    Invalid.Composites.FindChecked(Invalid.RootComposite).Nodes[0].Children[0].Children[0].Options[1].Kind = EMHRandomSemanticKind::Group;
    TestFalse(TEXT("appending gameobj must not admit Group as an option"), MHEncodeRuntimeCompositeGraph(Invalid, Reencoded, Error));
    Invalid = Decoded;
    Invalid.Composites.FindChecked(Invalid.RootComposite).Nodes[0].Children[0].Resource.Reset();
    TestFalse(TEXT("runtime gameobj still requires a canonical token"), MHEncodeRuntimeCompositeGraph(Invalid, Reencoded, Error));

    if (!TestNotNull(TEXT("runtime gameobj test world"), Fixture.World)) return false;
    AMHRuntimeCompositeActor* Runtime = Fixture.World->SpawnActor<AMHRuntimeCompositeActor>();
    if (!TestNotNull(TEXT("runtime gameobj placement exists"), Runtime)) return false;
    if (!TestTrue(TEXT("runtime gameobj-only placement configures"), Runtime->Configure(Input, 100, 700, Error)))
    {
        AddError(Error);
        return false;
    }
    TestEqual(TEXT("runtime gameobj produces no components"), Runtime->GetMaterializedComponents().Num(), 0);
    TestEqual(TEXT("runtime gameobj produces no child actor"), GameObjSpawnedChildCount(*Runtime), 0);
    if (!TestNotNull(TEXT("runtime exposes the same semantic plan"), Runtime->GetResolvedPlan())) return false;
    TestEqual(TEXT("runtime result signature agrees with reference entrypoint"), Runtime->GetResolvedPlan()->ResolvedSignature, Before.ResolvedSignature);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeGameObjPreviewNoSpawnTest,
    "Mimir.V5.Composite.GameObj.PreviewNoSpawnAndActorUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeGameObjPreviewNoSpawnTest::RunTest(const FString& Parameters)
{
    FGameObjFixture Fixture(*this);
    if (!TestNotNull(TEXT("preview gameobj test world"), Fixture.World)) return false;
    const FString Token = Fixture.Name(TEXT("gameobj_collision"));
    Fixture.Settings->ActorClassRegistry.Add(Token, FSoftClassPath(AStaticMeshActor::StaticClass()));
    UMHCompositeAsset* GameObjAsset = Fixture.Asset(GameObjTestDocument(Token), TEXT("gameobj_preview"));
    if (GameObjAsset == nullptr) return false;
    AMHCompositeActor* Preview = Fixture.World->SpawnActor<AMHCompositeActor>();
    if (!TestNotNull(TEXT("preview placement exists"), Preview)) return false;
    Preview->SetSeed(100);
    Preview->SetCompositeAsset(GameObjAsset);
    if (!TestNotNull(TEXT("preview admits gameobj-only composite"), Preview->GetResolvedPlan()))
    {
        AddError(Preview->GetLastPlacementError());
        return false;
    }
    TestEqual(TEXT("preview never spawns actor for a colliding gameobj token"), GameObjSpawnedChildCount(*Preview), 0);
    TestEqual(TEXT("gameobj has no leaf components; root authoring handle remains"), Preview->GetDerivedComponents().Num(), 1);

    FMHCompositeDocument ActorDocument;
    FMHCompositeNode& ActorNode = ActorDocument.Nodes.AddDefaulted_GetRef();
    ActorNode.Kind = EMHCompositeNodeKind::Actor;
    ActorNode.Resource = Token;
    UMHCompositeAsset* ActorAsset = Fixture.Asset(ActorDocument, TEXT("gameobj_actor_control"));
    if (ActorAsset == nullptr) return false;
    Preview->SetCompositeAsset(ActorAsset);
    if (!TestNotNull(TEXT("native actor kind remains admitted"), Preview->GetResolvedPlan())) return false;
    TestEqual(TEXT("native actor retains one leaf"), Preview->GetResolvedPlan()->Leaves.Num(), 1);
    TestEqual(TEXT("native actor still spawns the registered class"), GameObjSpawnedChildCount(*Preview), 1);

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
        !TestTrue(TEXT("native actor runtime remains admitted"), Runtime->Configure(Input, 100, 700, Error))) return false;
    TestEqual(TEXT("native actor still spawns at runtime"), GameObjSpawnedChildCount(*Runtime), 1);

    Fixture.Settings->ActorClassRegistry.Remove(Token);
    FGameObjNoSourceResolver Resolver;
    TestFalse(TEXT("unregistered native actor still fails source admission"),
        MHProbeCompositeBuildV5(ActorAsset->LogicalName, ActorDocument, Resolver, *Fixture.Settings, Error));
    TestTrue(TEXT("native actor rejection remains explicit"), Error.Contains(TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeGameObjIndexTest,
    "Mimir.V5.Composite.GameObj.IndexHasNoGameObjEdges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeGameObjIndexTest::RunTest(const FString& Parameters)
{
    struct FGameObjIndexFiles
    {
        FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests"),
            TEXT("GameObjIndex_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
        ~FGameObjIndexFiles() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
    } Files;
    if (!TestTrue(TEXT("isolated gameobj index directory exists"), IFileManager::Get().MakeDirectory(*Files.Root, true))) return false;
    FMHCompositeDocument Document = GameObjTestDocument(TEXT("not_a_resource_key"));
    FMHCompositeNode& GameObj = Document.Nodes[0].Children[0];
    GameObj.Profile = TEXT("gameobj_profile");
    FMHCompositeOption& Unselected = GameObj.Children[0].Options.AddDefaulted_GetRef();
    Unselected.Kind = EMHCompositeOptionKind::Composite;
    Unselected.Resource = TEXT("gameobj_child");
    Unselected.Weight = 0.0f;
    FString Error;
    TArray<uint8> Bytes;
    if (!MHWriteCanonicalCompositeV5(Document, Bytes, Error) ||
        !FFileHelper::SaveArrayToFile(Bytes, *FPaths::Combine(Files.Root, TEXT("gameobj_index_root.composite")))) return false;
    if (!FFileHelper::SaveArrayToFile(GameObjTestUtf8(TEXT(R"({"v":5,"nodes":[]})")),
        *FPaths::Combine(Files.Root, TEXT("gameobj_child.composite"))) ||
        !FFileHelper::SaveArrayToFile(GameObjTestUtf8(TEXT(R"({"v":1,"kind":"placement_profile"})")),
        *FPaths::Combine(Files.Root, TEXT("gameobj_profile.placement")))) return false;
    FMHProjectResourceIndex Index(Files.Root, FPaths::Combine(Files.Root, TEXT("cache/ProjectIndex.sqlite")));
    bool bRecreated = false;
    FMHProjectIndexUpdateResult Update;
    if (!TestTrue(TEXT("gameobj test index opens"), Index.Open(bRecreated, Error)) ||
        !TestTrue(TEXT("full scan accepts gameobj grammar"), Index.FullScan({}, Update, Error)))
    {
        AddError(Error);
        return false;
    }
    FMHResourceKey RootKey;
    RootKey.Kind = EMHResourceKind::Composite;
    RootKey.LogicalName = TEXT("gameobj_index_root");
    TestFalse(TEXT("gameobj token never creates an unresolved dependency"), Index.IsImportBlocked(RootKey, Error));
    FString Dump;
    if (!TestTrue(TEXT("index normalized projection is available"), Index.BuildNormalizedDump(Dump, Error))) return false;
    TestFalse(TEXT("gameobj identity is neither ResourceKey, edge nor diagnostic"), Dump.Contains(TEXT("not_a_resource_key")));
    TestTrue(TEXT("zero-weight real composite dependency below gameobj remains in closure"),
        Dump.Contains(TEXT("Dependencies\tcomposite\tgameobj_index_root\tcomposite\tgameobj_child\tplacement_composite")));
    TestTrue(TEXT("gameobj profile remains a real dependency"),
        Dump.Contains(TEXT("Dependencies\tcomposite\tgameobj_index_root\tplacement_profile\tgameobj_profile\tprofile")));
    TestTrue(TEXT("remove only the test's referenced child file"),
        IFileManager::Get().Delete(*FPaths::Combine(Files.Root, TEXT("gameobj_child.composite")), false, true));
    if (!Index.FullScan({}, Update, Error)) return false;
    TestTrue(TEXT("unselected missing real dependency below gameobj still blocks root"), Index.IsImportBlocked(RootKey, Error));
    TestTrue(TEXT("real dependency refusal names the absent composite"), Error.Contains(TEXT("gameobj_child")));
    return true;
}

} // namespace UE::MimirComposite::Tests
