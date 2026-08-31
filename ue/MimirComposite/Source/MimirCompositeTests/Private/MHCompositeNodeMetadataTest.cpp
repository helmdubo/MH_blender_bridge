#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHRuntimeCompositeInput.h"
#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Random/MHRandomStream.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "UObject/Package.h"

// V5-S6.1 owner amendment 2 (document 13): the composite node carries two
// optional provenance fields, `place_type` (raw int) and
// `appearance_seed_boundary` (bool). They are stored on the asset so that any
// re-serialization reproduces the source bytes, and they are deliberately kept
// out of the applied graph, the runtime transport and the frozen signature.

namespace UE::MimirComposite::Tests
{
namespace
{

TArray<uint8> MetadataUtf8(const TCHAR* Text)
{
    const FTCHARToUTF8 Utf8(Text);
    return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

FString MetadataText(const TArray<uint8>& Bytes)
{
    const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    return FString(Converted.Length(), Converted.Get());
}

/** One node carrying every optional field admitted alongside the two carriers. */
const TCHAR* MetadataOrderedJson =
    TEXT(R"({"v":5,"nodes":[{"kind":"random","name":"Authored name","transform":{"translation_cm":[1,2,3]},)")
    TEXT(R"("profile":"metadata_profile","place_type":3,"appearance_seed_boundary":true,)")
    TEXT(R"("options":[{"kind":"gameobj","resource":"loot_box","weight":1}],"children":[{"kind":"group"}]}]})");

/** Group parent, ordinary gameobj child, random gameobj option. No resolvable resource. */
FMHCompositeDocument MetadataDocument(const FString& Token, const bool bCarriers, const bool bBoundaries = true)
{
    FMHCompositeDocument Document;
    FMHCompositeNode& Parent = Document.Nodes.AddDefaulted_GetRef();
    Parent.Name = TEXT("Metadata parent");
    Parent.Transform.TranslationCm.X = 100.0;
    FMHCompositeNode& Child = Parent.Children.AddDefaulted_GetRef();
    Child.Kind = EMHCompositeNodeKind::GameObj;
    Child.Resource = Token;
    Child.Name = TEXT("Authored gameobj name");
    Child.Transform.TranslationCm.X = 25.0;
    FMHCompositeNode& Random = Child.Children.AddDefaulted_GetRef();
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Transform.TranslationCm.Y = 10.0;
    FMHCompositeOption& Option = Random.Options.AddDefaulted_GetRef();
    Option.Kind = EMHCompositeOptionKind::GameObj;
    Option.Resource = Token;
    Option.Weight = 1.0f;
    if (bCarriers)
    {
        Parent.PlaceType = 0;
        Child.PlaceType = 3;
        Child.bAppearanceSeedBoundary = bBoundaries;
        Random.PlaceType = 8;
        Random.bAppearanceSeedBoundary = bBoundaries;
    }
    return Document;
}

struct FMetadataFixture
{
    FAutomationTestBase& Test;
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    TArray<UMHCompositeAsset*> Assets;

    explicit FMetadataFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    ~FMetadataFixture()
    {
        for (UMHCompositeAsset* Asset : Assets)
        {
            Asset->ClearFlags(RF_Public | RF_Standalone);
            Asset->MarkAsGarbage();
        }
    }

    FString Name(const TCHAR* Stem) const { return FString(Stem) + TEXT("_") + Suffix; }

    /**
     * The receipt is pinned to the carrier-free bytes on purpose. Holding the
     * raw hash fixed isolates the carriers: any observable difference below is
     * caused by the fields themselves, not by the closure hash of new bytes.
     */
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

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeNodeMetadataCodecTest,
    "Mimir.V5.Composite.NodeMetadata.CodecCarriers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeNodeMetadataCodecTest::RunTest(const FString& Parameters)
{
    bool bPassed = true;
    FMHCompositeDocument Ordered;
    FString Error;
    if (!TestTrue(TEXT("node accepts place_type and appearance_seed_boundary"),
        MHParseCompositeV5(MetadataUtf8(MetadataOrderedJson), Ordered, Error)))
    {
        AddError(Error);
        return false;
    }
    bPassed &= TestEqual(TEXT("place_type 3 is preserved"), Ordered.Nodes[0].PlaceType, 3);
    bPassed &= TestTrue(TEXT("appearance_seed_boundary true is preserved"), Ordered.Nodes[0].bAppearanceSeedBoundary);
    TArray<uint8> Canonical;
    if (!TestTrue(TEXT("carriers write canonically"), MHWriteCanonicalCompositeV5(Ordered, Canonical, Error)))
    {
        AddError(Error);
        return false;
    }
    const FString Text = MetadataText(Canonical);
    const int32 ProfileAt = Text.Find(TEXT("\"profile\""));
    const int32 PlaceTypeAt = Text.Find(TEXT("\"place_type\""));
    const int32 BoundaryAt = Text.Find(TEXT("\"appearance_seed_boundary\""));
    const int32 OptionsAt = Text.Find(TEXT("\"options\""));
    const int32 ChildrenAt = Text.Find(TEXT("\"children\""));
    bPassed &= TestTrue(TEXT("every canonical field is emitted once"),
        ProfileAt != INDEX_NONE && PlaceTypeAt != INDEX_NONE && BoundaryAt != INDEX_NONE &&
        OptionsAt != INDEX_NONE && ChildrenAt != INDEX_NONE);
    bPassed &= TestTrue(TEXT("canonical order is profile, place_type, appearance_seed_boundary, options, children"),
        ProfileAt < PlaceTypeAt && PlaceTypeAt < BoundaryAt && BoundaryAt < OptionsAt && OptionsAt < ChildrenAt);
    bPassed &= TestTrue(TEXT("place_type is emitted as a raw int"), Text.Contains(TEXT("\"place_type\": 3")));
    bPassed &= TestTrue(TEXT("appearance_seed_boundary is emitted as a bool"),
        Text.Contains(TEXT("\"appearance_seed_boundary\": true")));

    FMHCompositeDocument Reparsed;
    TArray<uint8> Rewritten;
    bPassed &= TestTrue(TEXT("canonical carriers reparse"), MHParseCompositeV5(Canonical, Reparsed, Error));
    bPassed &= TestTrue(TEXT("canonical carriers rewrite"), MHWriteCanonicalCompositeV5(Reparsed, Rewritten, Error));
    bPassed &= TestTrue(TEXT("carrier canonical bytes round-trip exactly"), Canonical == Rewritten);

    // Absence is not zero, an explicit zero survives, and unknown-but-non-negative
    // values pass as provenance: UE never executes placement, so nothing narrows 0..6.
    struct FCase
    {
        const TCHAR* Json;
        int32 Expected;
        bool bEmitted;
    };
    const FCase Cases[] = {
        {TEXT(R"({"v":5,"nodes":[{"kind":"group"}]})"), INDEX_NONE, false},
        {TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":0}]})"), 0, true},
        {TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":3}]})"), 3, true},
        {TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":8}]})"), 8, true},
        {TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":2147483647}]})"), MAX_int32, true}};
    for (const FCase& Case : Cases)
    {
        FMHCompositeDocument Document;
        TArray<uint8> Bytes;
        if (!TestTrue(TEXT("non-negative place_type is admitted"),
            MHParseCompositeV5(MetadataUtf8(Case.Json), Document, Error) &&
            MHWriteCanonicalCompositeV5(Document, Bytes, Error)))
        {
            AddError(Error);
            bPassed = false;
            continue;
        }
        bPassed &= TestEqual(TEXT("place_type value survives parse"), Document.Nodes[0].PlaceType, Case.Expected);
        bPassed &= TestTrue(TEXT("absent place_type is never written, explicit zero always is"),
            MetadataText(Bytes).Contains(TEXT("\"place_type\"")) == Case.bEmitted);
    }

    // false is the default and is elided; the elision is idempotent, not lossy.
    FMHCompositeDocument ExplicitFalse;
    TArray<uint8> FalseBytes;
    if (TestTrue(TEXT("explicit false boundary parses"),
        MHParseCompositeV5(MetadataUtf8(TEXT(R"({"v":5,"nodes":[{"kind":"group","appearance_seed_boundary":false}]})")),
            ExplicitFalse, Error) && MHWriteCanonicalCompositeV5(ExplicitFalse, FalseBytes, Error)))
    {
        bPassed &= TestFalse(TEXT("explicit false boundary is elided"), ExplicitFalse.Nodes[0].bAppearanceSeedBoundary);
        bPassed &= TestFalse(TEXT("default boundary is not emitted"),
            MetadataText(FalseBytes).Contains(TEXT("appearance_seed_boundary")));
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeNodeMetadataGrammarTest,
    "Mimir.V5.Composite.NodeMetadata.ClosedGrammar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeNodeMetadataGrammarTest::RunTest(const FString& Parameters)
{
    bool bPassed = true;
    const TCHAR* Invalid[] = {
        TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":-1}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":-3}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":1.5}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":"3"}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":true}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":null}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":[3]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","place_type":0,"place_type":1}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","appearance_seed_boundary":1}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","appearance_seed_boundary":"true"}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","appearance_seed_boundary":null}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"group","appearance_seed_boundary":true,"appearance_seed_boundary":false}]})"),
        // The node grammar stays closed: near-miss names are still unknown fields.
        TEXT(R"({"v":5,"nodes":[{"kind":"group","place_types":0}]})"),
        // Carriers are node-only; random options keep their three-field grammar.
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"gameobj","resource":"a","weight":1,"place_type":0}]}]})"),
        TEXT(R"({"v":5,"nodes":[{"kind":"random","options":[{"kind":"gameobj","resource":"a","weight":1,"appearance_seed_boundary":true}]}]})")};
    for (const TCHAR* Json : Invalid)
    {
        FMHCompositeDocument Document;
        FString Error;
        bPassed &= TestFalse(TEXT("malformed carrier is rejected"),
            MHParseCompositeV5(MetadataUtf8(Json), Document, Error));
        bPassed &= TestTrue(TEXT("rejection uses the existing composite grammar code"),
            Error.Contains(TEXT("MH_E_COMPOSITE_GRAMMAR")));
    }
    // Since the 2026-08-31 owner revision of OPEN-V5-15, `placement` is a
    // legal node field whose body keeps the full closed placement-v1 grammar
    // with its own codes; a malformed body is still rejected before any use.
    {
        FMHCompositeDocument PlacementDocument;
        FString PlacementError;
        bPassed &= TestFalse(TEXT("malformed inline placement body is rejected"),
            MHParseCompositeV5(
                MetadataUtf8(TEXT(R"({"v":5,"nodes":[{"kind":"group","placement":{"mode":"pivot"}}]})")),
                PlacementDocument, PlacementError));
        bPassed &= TestTrue(TEXT("inline placement rejection uses the placement grammar code"),
            PlacementError.Contains(TEXT("MH_E_PLACEMENT_PROFILE_GRAMMAR")));
    }
    // The writer refuses anything below the absent sentinel; no new code is introduced.
    FMHCompositeDocument Document;
    FMHCompositeNode& Node = Document.Nodes.AddDefaulted_GetRef();
    Node.PlaceType = -2;
    TArray<uint8> Bytes;
    FString Error;
    bPassed &= TestFalse(TEXT("writer rejects a place_type below the absent sentinel"),
        MHWriteCanonicalCompositeV5(Document, Bytes, Error));
    bPassed &= TestTrue(TEXT("writer rejection uses the existing composite grammar code"),
        Error.Contains(TEXT("MH_E_COMPOSITE_GRAMMAR")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeNodeMetadataProvenanceTest,
    "Mimir.V5.Composite.NodeMetadata.AssetProvenanceAndInertBehavior",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeNodeMetadataProvenanceTest::RunTest(const FString& Parameters)
{
    FMetadataFixture Fixture(*this);
    const FString Token = Fixture.Name(TEXT("metadata_gameobj"));
    const FMHCompositeDocument Plain = MetadataDocument(Token, false);
    const FMHCompositeDocument Carried = MetadataDocument(Token, true);
    // place_type has no executor and must stay out of every derived artefact;
    // appearance_seed_boundary acquired one in V5-S6.3 and is therefore allowed
    // into the runtime transport, and only there.
    const FMHCompositeDocument PlaceTypeOnly = MetadataDocument(Token, true, false);
    FString Error;
    TArray<uint8> PlainBytes;
    TArray<uint8> CarriedBytes;
    if (!TestTrue(TEXT("both fixtures write canonically"),
        MHWriteCanonicalCompositeV5(Plain, PlainBytes, Error) &&
        MHWriteCanonicalCompositeV5(Carried, CarriedBytes, Error)))
    {
        AddError(Error);
        return false;
    }
    TestTrue(TEXT("carriers change the source bytes they are supposed to preserve"), PlainBytes != CarriedBytes);

    UMHCompositeAsset* Asset = Fixture.Asset(Plain, TEXT("metadata_root"));
    if (Asset == nullptr) return false;
    FMHRandomSourceGraph PlainGraph;
    TSet<FMHResourceKey> PlainDependencies;
    if (!TestTrue(TEXT("carrier-free applied graph builds"),
        MHBuildAppliedCompositeGraph(*Asset, *Fixture.Settings, PlainGraph, PlainDependencies, Error)))
    {
        AddError(Error);
        return false;
    }
    TArray<uint8> PlainGraphBytes;
    FMHResolvedCompositePlan PlainPlan;
    if (!TestTrue(TEXT("carrier-free graph encodes and resolves"),
        MHEncodeRuntimeCompositeGraph(PlainGraph, PlainGraphBytes, Error) &&
        MHResolveCompositePlan(PlainGraph, 100, 700, PlainPlan, Error)))
    {
        AddError(Error);
        return false;
    }

    // Re-apply the same structure with carriers. The receipt is untouched, so
    // the closure hash is held fixed and only the carriers can move anything.
    if (!TestTrue(TEXT("carrier document applies to the same asset"),
        MHApplyCompositeV5(*Asset, Carried, Error)))
    {
        AddError(Error);
        return false;
    }
    if (!TestEqual(TEXT("apply preserves the node count"), Asset->Nodes.Num(), 3)) return false;
    TestEqual(TEXT("explicit zero place_type is stored as zero"), Asset->Nodes[0].PlaceType, 0);
    TestFalse(TEXT("absent boundary stays false on the parent"), Asset->Nodes[0].bAppearanceSeedBoundary);
    TestEqual(TEXT("gameobj node stores place_type three"), Asset->Nodes[1].PlaceType, 3);
    TestTrue(TEXT("gameobj node stores the boundary flag"), Asset->Nodes[1].bAppearanceSeedBoundary);
    TestEqual(TEXT("out-of-range place_type is stored as provenance"), Asset->Nodes[2].PlaceType, 8);

    FMHCompositeDocument Extracted;
    TArray<uint8> ExtractedBytes;
    if (!TestTrue(TEXT("carrier asset extracts"), MHExtractCompositeV5(*Asset, Extracted, Error)) ||
        !MHWriteCanonicalCompositeV5(Extracted, ExtractedBytes, Error))
    {
        AddError(Error);
        return false;
    }
    TestTrue(TEXT("asset round-trip reproduces the carrier bytes exactly"), ExtractedBytes == CarriedBytes);

    FMHRandomSourceGraph CarriedGraph;
    TSet<FMHResourceKey> CarriedDependencies;
    TArray<uint8> CarriedGraphBytes;
    FMHResolvedCompositePlan CarriedPlan;
    if (!TestTrue(TEXT("carrier applied graph builds, encodes and resolves"),
        MHBuildAppliedCompositeGraph(*Asset, *Fixture.Settings, CarriedGraph, CarriedDependencies, Error) &&
        MHEncodeRuntimeCompositeGraph(CarriedGraph, CarriedGraphBytes, Error) &&
        MHResolveCompositePlan(CarriedGraph, 100, 700, CarriedPlan, Error)))
    {
        AddError(Error);
        return false;
    }
    TestEqual(TEXT("carriers add no dependency"), CarriedDependencies.Num(), PlainDependencies.Num());

    // place_type alone still changes nothing at all, transport bytes included.
    // The same asset is reused so its logical name, and therefore every other
    // byte of the transport, is held fixed; only the carriers can move.
    FMHRandomSourceGraph PlaceTypeGraph;
    TSet<FMHResourceKey> PlaceTypeDependencies;
    TArray<uint8> PlaceTypeGraphBytes;
    if (!TestTrue(TEXT("place_type-only document applies, builds and encodes"),
            MHApplyCompositeV5(*Asset, PlaceTypeOnly, Error) &&
            MHBuildAppliedCompositeGraph(*Asset, *Fixture.Settings, PlaceTypeGraph, PlaceTypeDependencies, Error) &&
            MHEncodeRuntimeCompositeGraph(PlaceTypeGraph, PlaceTypeGraphBytes, Error)))
    {
        AddError(Error);
        return false;
    }
    TestTrue(TEXT("place_type never reaches the runtime transport bytes"), PlaceTypeGraphBytes == PlainGraphBytes);

    // The boundary carrier does reach the transport, and only the transport:
    // without it a cooked or PIE placement would resolve a different appearance
    // boundary than the editor and the five-way parity would be a lie.
    TestTrue(TEXT("the boundary carrier reaches the runtime transport bytes"), CarriedGraphBytes != PlainGraphBytes);
    FMHRandomSourceGraph DecodedGraph;
    if (TestTrue(TEXT("carrier transport bytes decode"),
        MHDecodeRuntimeCompositeGraph(CarriedGraphBytes, DecodedGraph, Error)))
    {
        const FMHRandomComposite& Decoded = DecodedGraph.Composites.FindChecked(DecodedGraph.RootComposite);
        TestFalse(TEXT("the parent declares no boundary after transport"), Decoded.Nodes[0].bAppearanceSeedBoundary);
        TestTrue(TEXT("the declared boundary survives transport"), Decoded.Nodes[0].Children[0].bAppearanceSeedBoundary);
    }
    else AddError(Error);
    TestTrue(TEXT("carriers never reach the frozen signature preimage"),
        CarriedPlan.SignaturePreimage == PlainPlan.SignaturePreimage);
    TestEqual(TEXT("carriers never change the resolved signature"),
        CarriedPlan.ResolvedSignature, PlainPlan.ResolvedSignature);
    TestEqual(TEXT("carriers never change the closure hash"),
        CarriedPlan.Closure.ClosureHash, PlainPlan.Closure.ClosureHash);
    TestEqual(TEXT("carriers never change the draw count"), CarriedPlan.Draws.Num(), PlainPlan.Draws.Num());
    TestEqual(TEXT("carriers never change the decision count"), CarriedPlan.Decisions.Num(), PlainPlan.Decisions.Num());
    TestTrue(TEXT("gameobj carriers still emit no leaf"), CarriedPlan.Leaves.IsEmpty() && PlainPlan.Leaves.IsEmpty());
    if (!TestEqual(TEXT("carriers never change the node count"), CarriedPlan.Nodes.Num(), PlainPlan.Nodes.Num())) return false;
    for (int32 Index = 0; Index < CarriedPlan.Nodes.Num(); ++Index)
    {
        TestEqual(TEXT("carriers never move a NodePath"), CarriedPlan.Nodes[Index].NodePath, PlainPlan.Nodes[Index].NodePath);
        TestTrue(TEXT("carriers never move a world matrix"),
            CarriedPlan.Nodes[Index].WorldMatrix.Equals(PlainPlan.Nodes[Index].WorldMatrix, 0.0));
    }
    for (int32 Index = 0; Index < CarriedPlan.Decisions.Num(); ++Index)
    {
        TestEqual(TEXT("carriers never change a selected option"),
            CarriedPlan.Decisions[Index].OptionIndex, PlainPlan.Decisions[Index].OptionIndex);
        TestEqual(TEXT("carriers never change a selection draw"),
            CarriedPlan.Decisions[Index].RawU32, PlainPlan.Decisions[Index].RawU32);
    }
    return true;
}

} // namespace UE::MimirComposite::Tests
