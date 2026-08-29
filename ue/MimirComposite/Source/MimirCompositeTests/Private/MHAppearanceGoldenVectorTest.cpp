#include "MHGoldenRoot.h"

#include "Canonical/MHCanonical.h"
#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"

namespace UE::MimirComposite::Tests
{
namespace
{

/**
 * Cross-host reader for the Python appearance goldens (`mh.appearance_vectors:1`).
 * It never writes a golden and never invents one: when the directory or the
 * vector file is absent the lane fails loudly with "vectors not found", so a
 * missing cross-host oracle can never be mistaken for a green comparison.
 */
constexpr const TCHAR* AppearanceGoldenDirectory = TEXT("v5/appearance");
constexpr const TCHAR* AppearanceGoldenSchema = TEXT("mh.appearance_vectors:1");

bool AppearanceGoldenReadFloat(const TSharedPtr<FJsonValue>& Value, float& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Number) return false;
    Out = static_cast<float>(Value->AsNumber());
    return true;
}

bool AppearanceGoldenReadUint32(const TSharedPtr<FJsonValue>& Value, uint32& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Number) return false;
    const double Number = Value->AsNumber();
    if (Number < 0.0 || Number > 4294967295.0 || Number != FMath::TruncToDouble(Number)) return false;
    Out = static_cast<uint32>(Number);
    return true;
}

bool AppearanceGoldenReadTrs(const TSharedPtr<FJsonObject>& Object, FMHRandomTrs& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Translation = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Rotation = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Scale = nullptr;
    if (!Object.IsValid() ||
        !Object->TryGetArrayField(TEXT("translation_cm"), Translation) || Translation == nullptr || Translation->Num() != 3 ||
        !Object->TryGetArrayField(TEXT("rotation_quat"), Rotation) || Rotation == nullptr || Rotation->Num() != 4 ||
        !Object->TryGetArrayField(TEXT("scale"), Scale) || Scale == nullptr || Scale->Num() != 3) return false;
    return AppearanceGoldenReadFloat((*Translation)[0], Out.TranslationCm.X) &&
        AppearanceGoldenReadFloat((*Translation)[1], Out.TranslationCm.Y) &&
        AppearanceGoldenReadFloat((*Translation)[2], Out.TranslationCm.Z) &&
        AppearanceGoldenReadFloat((*Rotation)[0], Out.RotationQuat.X) &&
        AppearanceGoldenReadFloat((*Rotation)[1], Out.RotationQuat.Y) &&
        AppearanceGoldenReadFloat((*Rotation)[2], Out.RotationQuat.Z) &&
        AppearanceGoldenReadFloat((*Rotation)[3], Out.RotationQuat.W) &&
        AppearanceGoldenReadFloat((*Scale)[0], Out.Scale.X) &&
        AppearanceGoldenReadFloat((*Scale)[1], Out.Scale.Y) &&
        AppearanceGoldenReadFloat((*Scale)[2], Out.Scale.Z);
}

bool AppearanceGoldenReadKind(const FString& Value, EMHRandomSemanticKind& Out)
{
    if (Value == TEXT("mesh")) Out = EMHRandomSemanticKind::Mesh;
    else if (Value == TEXT("actor")) Out = EMHRandomSemanticKind::Actor;
    else if (Value == TEXT("composite")) Out = EMHRandomSemanticKind::Composite;
    else if (Value == TEXT("group")) Out = EMHRandomSemanticKind::Group;
    else if (Value == TEXT("random")) Out = EMHRandomSemanticKind::Random;
    else if (Value == TEXT("empty")) Out = EMHRandomSemanticKind::Empty;
    else if (Value == TEXT("gameobj")) Out = EMHRandomSemanticKind::GameObj;
    else return false;
    return true;
}

bool AppearanceGoldenReadNode(const TSharedPtr<FJsonObject>& Object, FMHRandomNode& Out)
{
    FString Kind;
    const TSharedPtr<FJsonObject>* Trs = nullptr;
    if (!Object.IsValid() || !Object->TryGetStringField(TEXT("kind"), Kind) || !AppearanceGoldenReadKind(Kind, Out.Kind) ||
        !Object->TryGetObjectField(TEXT("trs"), Trs) || Trs == nullptr || !AppearanceGoldenReadTrs(*Trs, Out.Transform)) return false;
    Object->TryGetStringField(TEXT("resource"), Out.Resource);
    Object->TryGetStringField(TEXT("profile"), Out.Profile);
    Object->TryGetBoolField(TEXT("appearance_seed_boundary"), Out.bAppearanceSeedBoundary);
    if (const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
        Object->TryGetArrayField(TEXT("options"), Options) && Options != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Options)
        {
            const TSharedPtr<FJsonObject> OptionObject = Value->AsObject();
            FMHRandomOption& Option = Out.Options.AddDefaulted_GetRef();
            FString OptionKind;
            if (!OptionObject.IsValid() || !OptionObject->TryGetStringField(TEXT("kind"), OptionKind) ||
                !AppearanceGoldenReadKind(OptionKind, Option.Kind)) return false;
            OptionObject->TryGetStringField(TEXT("resource"), Option.Resource);
            const TSharedPtr<FJsonValue>* Weight = OptionObject->Values.Find(TEXT("weight"));
            if (Weight == nullptr || !AppearanceGoldenReadFloat(*Weight, Option.Weight)) return false;
        }
    }
    if (const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
        Object->TryGetArrayField(TEXT("children"), Children) && Children != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Children)
            if (!AppearanceGoldenReadNode(Value->AsObject(), Out.Children.AddDefaulted_GetRef())) return false;
    }
    return true;
}

/** The appearance fixture writes a range as the pair [base, deviation]. */
bool AppearanceGoldenReadRange(const TSharedPtr<FJsonValue>& Value, FMHRandomRange& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 2) return false;
    return AppearanceGoldenReadFloat(Value->AsArray()[0], Out.Base) &&
        AppearanceGoldenReadFloat(Value->AsArray()[1], Out.Deviation);
}

bool AppearanceGoldenReadRangeTriple(const TSharedPtr<FJsonValue>& Value, FMHRandomRange (&Out)[3])
{
    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 3) return false;
    for (int32 Index = 0; Index < 3; ++Index)
        if (!AppearanceGoldenReadRange(Value->AsArray()[Index], Out[Index])) return false;
    return true;
}

bool AppearanceGoldenReadFixture(const TSharedPtr<FJsonObject>& Fixture, FMHRandomSourceGraph& Out, FString& OutError)
{
    if (!Fixture.IsValid() || !Fixture->TryGetStringField(TEXT("root"), Out.RootComposite))
    {
        OutError = TEXT("fixture has no root composite");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Composites = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("composites"), Composites) || Composites == nullptr)
    {
        OutError = TEXT("fixture has no composites array");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Composites)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        FMHRandomComposite Composite;
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("name"), Composite.Name) ||
            !Object->TryGetArrayField(TEXT("nodes"), Nodes) || Nodes == nullptr)
        {
            OutError = TEXT("fixture composite is malformed");
            return false;
        }
        for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
        {
            if (!AppearanceGoldenReadNode(NodeValue->AsObject(), Composite.Nodes.AddDefaulted_GetRef()))
            {
                OutError = TEXT("fixture node is malformed in composite ") + Composite.Name;
                return false;
            }
        }
        Out.Composites.Add(Composite.Name, MoveTemp(Composite));
    }
    if (const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
        Fixture->TryGetArrayField(TEXT("profiles"), Profiles) && Profiles != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Profiles)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            FMHRandomPlacementProfile Profile;
            if (!Object.IsValid() || !Object->TryGetStringField(TEXT("name"), Profile.Name))
            {
                OutError = TEXT("fixture profile is malformed");
                return false;
            }
            bool bOk = true;
            if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("offset_cm")))
            {
                Profile.bHasOffsetCm = true;
                bOk &= AppearanceGoldenReadRangeTriple(*Range, Profile.OffsetCm);
            }
            if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("rotation_deg")))
            {
                Profile.bHasRotationDeg = true;
                bOk &= AppearanceGoldenReadRangeTriple(*Range, Profile.RotationDeg);
            }
            if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("uniform_scale")))
            {
                Profile.bHasUniformScale = true;
                bOk &= AppearanceGoldenReadRange(*Range, Profile.UniformScale);
            }
            if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("vertical_scale")))
            {
                Profile.bHasVerticalScale = true;
                bOk &= AppearanceGoldenReadRange(*Range, Profile.VerticalScale);
            }
            if (!bOk)
            {
                OutError = TEXT("fixture profile range is malformed: ") + Profile.Name;
                return false;
            }
            Out.Profiles.Add(Profile.Name, MoveTemp(Profile));
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* Hashes = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("raw_hashes"), Hashes) || Hashes == nullptr)
    {
        OutError = TEXT("fixture has no raw_hashes array");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Hashes)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        FString Resource;
        FString Hash;
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("resource"), Resource) ||
            !Object->TryGetStringField(TEXT("hash"), Hash))
        {
            OutError = TEXT("fixture raw hash entry is malformed");
            return false;
        }
        Out.RawHashes.Add(Resource, Hash);
    }
    return true;
}

FString AppearanceGoldenUtf8(TConstArrayView<uint8> Bytes)
{
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    return FString(Text.Length(), Text.Get());
}

/** One golden vector against one resolved plan; RawU32 and both signatures. */
bool AppearanceGoldenCheckVector(FAutomationTestBase& Test, const FString& Label,
    const FMHRandomSourceGraph& Graph, const TSharedPtr<FJsonObject>& Vector, int32& OutCompared)
{
    int32 Seed = 0;
    int32 AppearanceSeed = 0;
    if (!Vector.IsValid() || !Vector->TryGetNumberField(TEXT("seed"), Seed) ||
        !Vector->TryGetNumberField(TEXT("appearance_seed"), AppearanceSeed))
    {
        Test.AddError(Label + TEXT(": vector lacks seed/appearance_seed"));
        return false;
    }
    const FString Where = FString::Printf(TEXT("%s seed %d/%d"), *Label, Seed, AppearanceSeed);
    FMHResolvedCompositePlan Plan;
    FString Error;
    if (!MHResolveCompositePlan(Graph, Seed, AppearanceSeed, Plan, Error))
    {
        Test.AddError(Where + TEXT(": golden fixture does not resolve: ") + Error);
        return false;
    }
    ++OutCompared;
    bool bPassed = true;
    FString Expected;
    if (Vector->TryGetStringField(TEXT("resolved_signature"), Expected))
        bPassed &= Test.TestEqual(*(Where + TEXT(" ResolvedSignature")), Plan.ResolvedSignature, Expected);
    if (Vector->TryGetStringField(TEXT("appearance_signature"), Expected))
        bPassed &= Test.TestEqual(*(Where + TEXT(" AppearanceSignature")), Plan.Appearance.AppearanceSignature, Expected);
    if (Vector->TryGetStringField(TEXT("placement_signature"), Expected))
        bPassed &= Test.TestEqual(*(Where + TEXT(" PlacementSignature")), Plan.PlacementSignature, Expected);
    if (Vector->TryGetStringField(TEXT("appearance_signature_preimage_utf8"), Expected))
        bPassed &= Test.TestEqual(*(Where + TEXT(" AppearanceSignature preimage bytes")),
            AppearanceGoldenUtf8(Plan.Appearance.SignaturePreimage), Expected);
    if (Vector->TryGetStringField(TEXT("root_boundary"), Expected))
        bPassed &= Test.TestEqual(*(Where + TEXT(" placement root boundary")), Graph.RootComposite, Expected);

    if (const TArray<TSharedPtr<FJsonValue>>* Boundaries = nullptr;
        Vector->TryGetArrayField(TEXT("boundaries"), Boundaries) && Boundaries != nullptr)
    {
        TArray<FString> Distinct;
        for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves) Distinct.AddUnique(Leaf.AppearanceBoundaryPath);
        Distinct.Sort([](const FString& Left, const FString& Right)
        {
            return Left.Compare(Right, ESearchCase::CaseSensitive) < 0;
        });
        if (Test.TestEqual(*(Where + TEXT(" distinct boundary count")), Distinct.Num(), Boundaries->Num()))
        {
            for (int32 Index = 0; Index < Distinct.Num(); ++Index)
                bPassed &= Test.TestEqual(*(Where + TEXT(" boundary")), Distinct[Index], (*Boundaries)[Index]->AsString());
        }
        else bPassed = false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Leaves = nullptr;
    if (!Vector->TryGetArrayField(TEXT("leaves"), Leaves) || Leaves == nullptr)
    {
        Test.AddError(Where + TEXT(": vector has no leaves array"));
        return false;
    }
    if (!Test.TestEqual(*(Where + TEXT(" leaf count")), Plan.Leaves.Num(), Leaves->Num())) return false;
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> ExpectedLeaf = (*Leaves)[Index]->AsObject();
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        if (!ExpectedLeaf.IsValid())
        {
            Test.AddError(Where + TEXT(": leaf entry is not an object"));
            bPassed = false;
            continue;
        }
        FString Path;
        if (ExpectedLeaf->TryGetStringField(TEXT("path"), Path))
            bPassed &= Test.TestEqual(*(Where + TEXT(" leaf NodePath")), Leaf.Origin, Path);
        FString Boundary;
        if (ExpectedLeaf->TryGetStringField(TEXT("boundary"), Boundary))
            bPassed &= Test.TestEqual(*(Where + TEXT(" leaf boundary")), Leaf.AppearanceBoundaryPath, Boundary);
        const TArray<TSharedPtr<FJsonValue>>* Raw = nullptr;
        if (!ExpectedLeaf->TryGetArrayField(TEXT("raw_u32"), Raw) || Raw == nullptr ||
            Raw->Num() != MH_APPEARANCE_CHANNELS)
        {
            Test.AddError(Where + TEXT(": leaf has no raw_u32 of MH_APPEARANCE_CHANNELS length"));
            bPassed = false;
            continue;
        }
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
        {
            uint32 ExpectedRaw = 0;
            if (!AppearanceGoldenReadUint32((*Raw)[Channel], ExpectedRaw))
            {
                Test.AddError(Where + TEXT(": raw_u32 entry is not a uint32"));
                bPassed = false;
                continue;
            }
            bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s leaf %d channel %d RawU32"), *Where, Index, Channel),
                Plan.Appearance.Draws[Index * MH_APPEARANCE_CHANNELS + Channel].RawU32, ExpectedRaw);
        }
    }

    // The flat draw list is the same values in the same leaf-major order.
    if (const TArray<TSharedPtr<FJsonValue>>* Draws = nullptr;
        Vector->TryGetArrayField(TEXT("draws"), Draws) && Draws != nullptr)
    {
        if (Test.TestEqual(*(Where + TEXT(" draw count")), Plan.Appearance.Draws.Num(), Draws->Num()))
        {
            for (int32 Index = 0; Index < Plan.Appearance.Draws.Num(); ++Index)
            {
                const TSharedPtr<FJsonObject> ExpectedDraw = (*Draws)[Index]->AsObject();
                if (!ExpectedDraw.IsValid()) { bPassed = false; continue; }
                const FMHResolvedCompositeAppearanceDraw& Draw = Plan.Appearance.Draws[Index];
                int32 Channel = 0;
                uint32 Raw = 0;
                FString Path;
                if (ExpectedDraw->TryGetNumberField(TEXT("channel"), Channel))
                    bPassed &= Test.TestEqual(*(Where + TEXT(" draw channel")), Draw.Channel, Channel);
                if (ExpectedDraw->TryGetStringField(TEXT("path"), Path))
                    bPassed &= Test.TestEqual(*(Where + TEXT(" draw path")), Draw.NodePath, Path);
                const TSharedPtr<FJsonValue>* RawValue = ExpectedDraw->Values.Find(TEXT("raw_u32"));
                if (RawValue != nullptr && AppearanceGoldenReadUint32(*RawValue, Raw))
                    bPassed &= Test.TestEqual(*(Where + TEXT(" draw RawU32")), Draw.RawU32, Raw);
            }
        }
        else bPassed = false;
    }
    return bPassed;
}

TSharedPtr<FJsonObject> AppearanceGoldenLoad(FAutomationTestBase& Test, const FString& Path)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Path))
    {
        Test.AddError(TEXT("cannot read appearance golden: ") + Path);
        return nullptr;
    }
    if (Bytes.Contains(static_cast<uint8>('\r')))
    {
        Test.AddError(TEXT("appearance golden is not LF-clean: ") + Path);
        return nullptr;
    }
    TSharedPtr<FJsonValue> Value;
    const FMHCanonicalResult Parsed = MHParseJsonUtf8(Bytes, Value);
    if (!Parsed.bSuccess || !Value.IsValid() || Value->Type != EJson::Object)
    {
        Test.AddError(TEXT("invalid appearance golden ") + Path + TEXT(": ") + Parsed.Error);
        return nullptr;
    }
    return Value->AsObject();
}

} // namespace

/**
 * Acceptance 3, cross-host half: Python reference == UE resolver on the frozen
 * appearance vectors, compared by RawU32 per leaf per channel plus the
 * AppearanceSignature (and its preimage bytes, when the golden carries them).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHAppearanceCrossHostGoldenTest,
    "Mimir.V5.Random.Appearance.CrossHostGoldenVectors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHAppearanceCrossHostGoldenTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    const FString Directory = FPaths::Combine(GoldenRoot, AppearanceGoldenDirectory);
    if (!IFileManager::Get().DirectoryExists(*Directory))
    {
        // Never a silent pass: the cross-host oracle is a gate, not an option.
        AddError(TEXT("appearance golden vectors not found: ") + Directory +
            TEXT(" does not exist; the Python appearance goldens have not landed yet"));
        return false;
    }
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.json"), true, false);
    Files.Sort();
    if (Files.IsEmpty())
    {
        AddError(TEXT("appearance golden vectors not found: no *.json under ") + Directory);
        return false;
    }

    bool bPassed = true;
    int32 ComparedVectors = 0;
    for (const FString& File : Files)
    {
        const TSharedPtr<FJsonObject> Root = AppearanceGoldenLoad(*this, File);
        if (!Root.IsValid())
        {
            bPassed = false;
            continue;
        }
        const FString Name = FPaths::GetCleanFilename(File);
        FString Text;
        if (Root->TryGetStringField(TEXT("schema"), Text))
            bPassed &= TestEqual(TEXT("golden declares the appearance vector schema"), Text, FString(AppearanceGoldenSchema));
        if (Root->TryGetStringField(TEXT("stage"), Text))
            bPassed &= TestEqual(TEXT("golden names the frozen appearance stage tag"), Text, FString(MHAppearanceTag));
        if (Root->TryGetStringField(TEXT("stream"), Text))
            bPassed &= TestEqual(TEXT("golden names the unchanged stream tag"), Text, FString(MHRandomStream1Tag));
        if (Root->TryGetStringField(TEXT("resolver"), Text))
            bPassed &= TestEqual(TEXT("golden names the unchanged layout resolver tag"), Text, FString(MHRandomResolverTag));
        int32 Channels = MH_APPEARANCE_CHANNELS;
        if (Root->TryGetNumberField(TEXT("channels"), Channels))
            bPassed &= TestEqual(TEXT("golden agrees on MH_APPEARANCE_CHANNELS"), Channels, MH_APPEARANCE_CHANNELS);

        // One graph per fixture-bearing group: the synthetic S1 fixture plus
        // every named boundary scenario the golden carries.
        TArray<TPair<FString, TSharedPtr<FJsonObject>>> Groups;
        if (const TSharedPtr<FJsonObject>* Synthetic = nullptr;
            Root->TryGetObjectField(TEXT("synthetic"), Synthetic) && Synthetic != nullptr)
            Groups.Emplace(Name + TEXT("/synthetic"), *Synthetic);
        if (const TArray<TSharedPtr<FJsonValue>>* Scenarios = nullptr;
            Root->TryGetArrayField(TEXT("scenarios"), Scenarios) && Scenarios != nullptr)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Scenarios)
            {
                const TSharedPtr<FJsonObject> Scenario = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!Scenario.IsValid())
                {
                    AddError(Name + TEXT(": scenario entry is not an object"));
                    bPassed = false;
                    continue;
                }
                FString ScenarioName;
                Scenario->TryGetStringField(TEXT("name"), ScenarioName);
                Groups.Emplace(Name + TEXT("/") + ScenarioName, Scenario);
            }
        }
        if (Groups.IsEmpty())
        {
            AddError(Name + TEXT(": golden carries neither a synthetic group nor scenarios"));
            bPassed = false;
            continue;
        }
        for (const TPair<FString, TSharedPtr<FJsonObject>>& Group : Groups)
        {
            const TSharedPtr<FJsonObject>* Fixture = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* Vectors = nullptr;
            if (!Group.Value->TryGetObjectField(TEXT("fixture"), Fixture) || Fixture == nullptr ||
                !Group.Value->TryGetArrayField(TEXT("vectors"), Vectors) || Vectors == nullptr)
            {
                AddError(Group.Key + TEXT(": group has no fixture/vectors pair"));
                bPassed = false;
                continue;
            }
            FMHRandomSourceGraph Graph;
            FString FixtureError;
            if (!AppearanceGoldenReadFixture(*Fixture, Graph, FixtureError))
            {
                AddError(Group.Key + TEXT(": fixture rejected: ") + FixtureError);
                bPassed = false;
                continue;
            }
            for (const TSharedPtr<FJsonValue>& VectorValue : *Vectors)
            {
                bPassed &= AppearanceGoldenCheckVector(*this, Group.Key, Graph,
                    VectorValue.IsValid() ? VectorValue->AsObject() : nullptr, ComparedVectors);
            }
        }
    }
    bPassed &= TestTrue(TEXT("at least one cross-host appearance vector was compared"), ComparedVectors > 0);
    AddInfo(FString::Printf(TEXT("cross-host appearance vectors compared: %d"), ComparedVectors));
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
