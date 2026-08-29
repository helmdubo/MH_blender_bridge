#include "MHGoldenRoot.h"

#include "Canonical/MHCanonical.h"
#include "Dom/JsonObject.h"
#include "Diagnostics/MHDiagnosticRegistry.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"

namespace UE::MimirComposite::Tests
{
namespace
{

bool ReadUint64(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, uint64& Out)
{
    const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
    FString Raw;
    if (Value == nullptr || !Value->IsValid() || !(*Value)->TryGetString(Raw)) return false;
    TCHAR* End = nullptr;
    Out = FCString::Strtoui64(*Raw, &End, 10);
    return End != nullptr && *End == 0;
}

bool ReadInt32(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32& Out)
{
    const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
    FString Raw;
    if (Value == nullptr || !Value->IsValid() || !(*Value)->TryGetString(Raw)) return false;
    TCHAR* End = nullptr;
    const int64 Parsed = FCString::Strtoi64(*Raw, &End, 10);
    if (End == nullptr || *End != 0 || Parsed < MIN_int32 || Parsed > MAX_int32) return false;
    Out = static_cast<int32>(Parsed);
    return true;
}

bool ReadFloat(const TSharedPtr<FJsonValue>& Value, float& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Number) return false;
    Out = static_cast<float>(Value->AsNumber());
    return FMath::IsFinite(Out);
}

bool ReadVector3(const TSharedPtr<FJsonValue>& Value, FVector3f& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 3) return false;
    return ReadFloat(Value->AsArray()[0], Out.X) && ReadFloat(Value->AsArray()[1], Out.Y) &&
        ReadFloat(Value->AsArray()[2], Out.Z);
}

bool ReadQuat(const TSharedPtr<FJsonValue>& Value, FQuat4f& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != 4) return false;
    return ReadFloat(Value->AsArray()[0], Out.X) && ReadFloat(Value->AsArray()[1], Out.Y) &&
        ReadFloat(Value->AsArray()[2], Out.Z) && ReadFloat(Value->AsArray()[3], Out.W);
}

bool ReadTrs(const TSharedPtr<FJsonObject>& Object, FMHRandomTrs& Out)
{
    const TSharedPtr<FJsonValue>* Translation = Object->Values.Find(TEXT("translation_cm"));
    const TSharedPtr<FJsonValue>* Rotation = Object->Values.Find(TEXT("rotation_quat"));
    const TSharedPtr<FJsonValue>* Scale = Object->Values.Find(TEXT("scale"));
    return Translation != nullptr && Rotation != nullptr && Scale != nullptr &&
        ReadVector3(*Translation, Out.TranslationCm) && ReadQuat(*Rotation, Out.RotationQuat) &&
        ReadVector3(*Scale, Out.Scale);
}

bool ReadRange(const TSharedPtr<FJsonValue>& Value, FMHRandomRange& Out)
{
    return Value.IsValid() && Value->Type == EJson::Array && Value->AsArray().Num() == 2 &&
        ReadFloat(Value->AsArray()[0], Out.Base) && ReadFloat(Value->AsArray()[1], Out.Deviation);
}

bool ReadRangeTriple(const TSharedPtr<FJsonValue>& Value, FMHRandomRange Out[3])
{
    return Value.IsValid() && Value->Type == EJson::Array && Value->AsArray().Num() == 3 &&
        ReadRange(Value->AsArray()[0], Out[0]) && ReadRange(Value->AsArray()[1], Out[1]) &&
        ReadRange(Value->AsArray()[2], Out[2]);
}

bool ParseKind(const FString& Value, EMHRandomSemanticKind& Out)
{
    if (Value == TEXT("mesh")) Out = EMHRandomSemanticKind::Mesh;
    else if (Value == TEXT("actor")) Out = EMHRandomSemanticKind::Actor;
    else if (Value == TEXT("composite")) Out = EMHRandomSemanticKind::Composite;
    else if (Value == TEXT("group")) Out = EMHRandomSemanticKind::Group;
    else if (Value == TEXT("random")) Out = EMHRandomSemanticKind::Random;
    else if (Value == TEXT("empty")) Out = EMHRandomSemanticKind::Empty;
    else return false;
    return true;
}

bool ReadNode(const TSharedPtr<FJsonObject>& Object, FMHRandomNode& Out)
{
    FString Kind;
    const TSharedPtr<FJsonObject>* Trs = nullptr;
    if (!Object.IsValid() || !Object->TryGetStringField(TEXT("kind"), Kind) || !ParseKind(Kind, Out.Kind) ||
        !Object->TryGetObjectField(TEXT("trs"), Trs) || Trs == nullptr || !ReadTrs(*Trs, Out.Transform)) return false;
    Object->TryGetStringField(TEXT("resource"), Out.Resource);
    Object->TryGetStringField(TEXT("profile"), Out.Profile);
    if (const TArray<TSharedPtr<FJsonValue>>* Options = nullptr; Object->TryGetArrayField(TEXT("options"), Options) && Options != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Options)
        {
            const TSharedPtr<FJsonObject> OptionObject = Value->AsObject();
            FMHRandomOption& Option = Out.Options.AddDefaulted_GetRef();
            FString OptionKind;
            if (!OptionObject.IsValid() || !OptionObject->TryGetStringField(TEXT("kind"), OptionKind) ||
                !ParseKind(OptionKind, Option.Kind)) return false;
            OptionObject->TryGetStringField(TEXT("resource"), Option.Resource);
            const TSharedPtr<FJsonValue>* Weight = OptionObject->Values.Find(TEXT("weight"));
            if (Weight == nullptr || !ReadFloat(*Weight, Option.Weight)) return false;
        }
    }
    if (const TArray<TSharedPtr<FJsonValue>>* Children = nullptr; Object->TryGetArrayField(TEXT("children"), Children) && Children != nullptr)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Children)
        {
            if (!ReadNode(Value->AsObject(), Out.Children.AddDefaulted_GetRef())) return false;
        }
    }
    return true;
}

bool ReadFixture(const TSharedPtr<FJsonObject>& Root, FMHRandomSourceGraph& Out)
{
    const TSharedPtr<FJsonObject>* Fixture = nullptr;
    if (!Root->TryGetObjectField(TEXT("fixture"), Fixture) || Fixture == nullptr ||
        !(*Fixture)->TryGetStringField(TEXT("root"), Out.RootComposite)) return false;
    const TArray<TSharedPtr<FJsonValue>>* Composites = nullptr;
    if (!(*Fixture)->TryGetArrayField(TEXT("composites"), Composites) || Composites == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Composites)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        FMHRandomComposite Composite;
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("name"), Composite.Name)) return false;
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Object->TryGetArrayField(TEXT("nodes"), Nodes) || Nodes == nullptr) return false;
        for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
        {
            if (!ReadNode(NodeValue->AsObject(), Composite.Nodes.AddDefaulted_GetRef())) return false;
        }
        Out.Composites.Add(Composite.Name, MoveTemp(Composite));
    }
    const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
    if (!(*Fixture)->TryGetArrayField(TEXT("profiles"), Profiles) || Profiles == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Profiles)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        FMHRandomPlacementProfile Profile;
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("name"), Profile.Name)) return false;
        if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("offset_cm")))
        {
            Profile.bHasOffsetCm = true;
            if (!ReadRangeTriple(*Range, Profile.OffsetCm)) return false;
        }
        if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("rotation_deg")))
        {
            Profile.bHasRotationDeg = true;
            if (!ReadRangeTriple(*Range, Profile.RotationDeg)) return false;
        }
        if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("uniform_scale")))
        {
            Profile.bHasUniformScale = true;
            if (!ReadRange(*Range, Profile.UniformScale)) return false;
        }
        if (const TSharedPtr<FJsonValue>* Range = Object->Values.Find(TEXT("vertical_scale")))
        {
            Profile.bHasVerticalScale = true;
            if (!ReadRange(*Range, Profile.VerticalScale)) return false;
        }
        Out.Profiles.Add(Profile.Name, MoveTemp(Profile));
    }
    const TArray<TSharedPtr<FJsonValue>>* Hashes = nullptr;
    if (!(*Fixture)->TryGetArrayField(TEXT("raw_hashes"), Hashes) || Hashes == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Hashes)
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        FString Resource;
        FString Hash;
        if (!Object.IsValid() || !Object->TryGetStringField(TEXT("resource"), Resource) ||
            !Object->TryGetStringField(TEXT("hash"), Hash)) return false;
        Out.RawHashes.Add(Resource, Hash);
    }
    return true;
}

bool ExactTrs(FAutomationTestBase& Test, const FString& Label, const FMHRandomTrs& Actual, const TSharedPtr<FJsonObject>& Expected)
{
    FMHRandomTrs Parsed;
    if (!ReadTrs(Expected, Parsed))
    {
        Test.AddError(Label + TEXT(": malformed expected TRS"));
        return false;
    }
    bool bPassed = true;
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s translation x"), *Label), Actual.TranslationCm.X, Parsed.TranslationCm.X);
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s translation y"), *Label), Actual.TranslationCm.Y, Parsed.TranslationCm.Y);
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s translation z"), *Label), Actual.TranslationCm.Z, Parsed.TranslationCm.Z);
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s rotation x"), *Label), Actual.RotationQuat.X, Parsed.RotationQuat.X);
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s rotation y"), *Label), Actual.RotationQuat.Y, Parsed.RotationQuat.Y);
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s rotation z"), *Label), Actual.RotationQuat.Z, Parsed.RotationQuat.Z);
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s rotation w"), *Label), Actual.RotationQuat.W, Parsed.RotationQuat.W);
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s scale x"), *Label), Actual.Scale.X, Parsed.Scale.X);
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s scale y"), *Label), Actual.Scale.Y, Parsed.Scale.Y);
    bPassed &= Test.TestEqual(*FString::Printf(TEXT("%s scale z"), *Label), Actual.Scale.Z, Parsed.Scale.Z);
    return bPassed;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHRandomStream1GoldenTest,
    "Mimir.V5.Random.StreamTraceAndSignatureParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRandomStream1GoldenTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    TArray<uint8> Bytes;
    const FString Path = FPaths::Combine(GoldenRoot, TEXT("v5/random_stream_1_vectors.json"));
    if (!FFileHelper::LoadFileToArray(Bytes, *Path))
    {
        AddError(TEXT("cannot read shared random_stream_1_vectors.json"));
        return false;
    }
    TSharedPtr<FJsonValue> RootValue;
    const FMHCanonicalResult ParseResult = MHParseJsonUtf8(Bytes, RootValue);
    if (!ParseResult.bSuccess || !RootValue.IsValid() || RootValue->Type != EJson::Object)
    {
        AddError(TEXT("shared random_stream_1_vectors.json is invalid JSON"));
        return false;
    }
    const TSharedPtr<FJsonObject> Root = RootValue->AsObject();
    FString StreamTag;
    FString ResolverTag;
    bool bPassed = TestTrue(TEXT("stream tag"), Root->TryGetStringField(TEXT("stream"), StreamTag) && StreamTag == MHRandomStream1Tag);
    bPassed &= TestTrue(TEXT("resolver tag"), Root->TryGetStringField(TEXT("resolver"), ResolverTag) && ResolverTag == MHRandomResolverTag);
	bPassed &= TestEqual(TEXT("exact registered MH_E count"), MHRegisteredErrorCodes().Num(), 53);
	bPassed &= TestTrue(
		TEXT("placement state desync code is registered"),
		MHRegisteredErrorCodes().Contains(TEXT("MH_E_PLACEMENT_STATE_DESYNC")));
	bPassed &= TestTrue(
		TEXT("duplicate random option index code is registered"),
		MHRegisteredErrorCodes().Contains(TEXT("MH_E_DUPLICATE_RANDOM_OPTION_INDEX")));
	bPassed &= TestTrue(
		TEXT("partial publish code is registered"),
		MHRegisteredErrorCodes().Contains(TEXT("MH_E_PARTIAL_PUBLISH")));
    bPassed &= TestEqual(TEXT("exact registered MH_W count"), MHRegisteredWarningCodes().Num(), 15);
    bPassed &= TestTrue(TEXT("explicit Dagor construct drop warning is registered"),
        MHRegisteredWarningCodes().Contains(TEXT("MH_W_DAGOR_CONSTRUCT_DROPPED")));
    bPassed &= TestTrue(TEXT("placement grammar code registered"),
        MHRegisteredErrorCodes().Contains(TEXT("MH_E_PLACEMENT_PROFILE_GRAMMAR")));
    bPassed &= TestTrue(TEXT("unrepresentable transform code registered"),
        MHRegisteredErrorCodes().Contains(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM")));

    const TArray<TSharedPtr<FJsonValue>>* StreamVectors = nullptr;
    if (!Root->TryGetArrayField(TEXT("stream_vectors"), StreamVectors) || StreamVectors == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& VectorValue : *StreamVectors)
    {
        const TSharedPtr<FJsonObject> Vector = VectorValue->AsObject();
        int32 Seed = 0;
        uint64 InitialState = 0;
        if (!ReadInt32(Vector, TEXT("seed"), Seed) || !ReadUint64(Vector, TEXT("initial_state"), InitialState)) return false;
        FMHRandomStream1 Stream(Seed);
        bPassed &= TestEqual(*FString::Printf(TEXT("seed %d initial state"), Seed), Stream.GetInitialState(), InitialState);
        const TArray<TSharedPtr<FJsonValue>>* Draws = nullptr;
        if (!Vector->TryGetArrayField(TEXT("draws"), Draws) || Draws == nullptr) return false;
        for (const TSharedPtr<FJsonValue>& DrawValue : *Draws)
        {
            const TSharedPtr<FJsonObject> Draw = DrawValue->AsObject();
            uint64 U64 = 0;
            uint64 U32 = 0;
            if (!ReadUint64(Draw, TEXT("u64"), U64) || !ReadUint64(Draw, TEXT("u32"), U32)) return false;
            const uint64 ActualU64 = Stream.NextU64();
            bPassed &= TestEqual(*FString::Printf(TEXT("seed %d u64"), Seed), ActualU64, U64);
            bPassed &= TestEqual(*FString::Printf(TEXT("seed %d u32"), Seed), static_cast<uint32>(ActualU64 >> 32), static_cast<uint32>(U32));
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* NodeStreamVectors = nullptr;
    if (!Root->TryGetArrayField(TEXT("node_stream_vectors"), NodeStreamVectors) || NodeStreamVectors == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& VectorValue : *NodeStreamVectors)
    {
        const TSharedPtr<FJsonObject> Vector = VectorValue->AsObject();
        int32 Seed = 0;
        FString NodePath;
        uint64 PlacementState = 0;
        uint64 PathHash64 = 0;
        uint64 MixedState = 0;
        uint64 InitialState = 0;
        if (!ReadInt32(Vector, TEXT("seed"), Seed) || !Vector->TryGetStringField(TEXT("path"), NodePath) ||
            !ReadUint64(Vector, TEXT("placement_state"), PlacementState) ||
            !ReadUint64(Vector, TEXT("path_hash64"), PathHash64) ||
            !ReadUint64(Vector, TEXT("mixed_state"), MixedState) ||
            !ReadUint64(Vector, TEXT("initial_state"), InitialState)) return false;
        const FMHRandomStream1 PlacementStream(Seed);
        bPassed &= TestEqual(*FString::Printf(TEXT("seed %d placement state"), Seed), PlacementStream.GetInitialState(), PlacementState);
        bPassed &= TestEqual(*FString::Printf(TEXT("seed %d path hash %s"), Seed, *NodePath), MHRandomPathHash64(NodePath), PathHash64);
        bPassed &= TestEqual(*FString::Printf(TEXT("seed %d mixed state %s"), Seed, *NodePath), PlacementState ^ PathHash64, MixedState);
        FMHRandomStream1 Stream = MHMakeNodeRandomStream(Seed, NodePath);
        bPassed &= TestEqual(*FString::Printf(TEXT("seed %d node initial state %s"), Seed, *NodePath), Stream.GetInitialState(), InitialState);
        const TArray<TSharedPtr<FJsonValue>>* Draws = nullptr;
        if (!Vector->TryGetArrayField(TEXT("draws"), Draws) || Draws == nullptr) return false;
        for (const TSharedPtr<FJsonValue>& DrawValue : *Draws)
        {
            const TSharedPtr<FJsonObject> Draw = DrawValue->AsObject();
            uint64 U64 = 0;
            uint64 U32 = 0;
            if (!ReadUint64(Draw, TEXT("u64"), U64) || !ReadUint64(Draw, TEXT("u32"), U32)) return false;
            const uint64 ActualU64 = Stream.NextU64();
            bPassed &= TestEqual(*FString::Printf(TEXT("seed %d node u64 %s"), Seed, *NodePath), ActualU64, U64);
            bPassed &= TestEqual(*FString::Printf(TEXT("seed %d node u32 %s"), Seed, *NodePath), static_cast<uint32>(ActualU64 >> 32), static_cast<uint32>(U32));
        }
    }

    FMHRandomSourceGraph Graph;
    if (!ReadFixture(Root, Graph))
    {
        AddError(TEXT("shared resolver fixture is malformed"));
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Plans = nullptr;
    if (!Root->TryGetArrayField(TEXT("plan_vectors"), Plans) || Plans == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& PlanValue : *Plans)
    {
        const TSharedPtr<FJsonObject> Expected = PlanValue->AsObject();
        int32 Seed = 0;
        if (!ReadInt32(Expected, TEXT("seed"), Seed)) return false;
        FMHResolvedCompositePlan Actual;
        FString Error;
        bPassed &= TestTrue(*FString::Printf(TEXT("seed %d resolves"), Seed), MHResolveCompositePlan(Graph, Seed, Seed, Actual, Error));
        if (!Error.IsEmpty()) AddError(Error);

        const TArray<TSharedPtr<FJsonValue>>* ExpectedDecisions = nullptr;
        if (!Expected->TryGetArrayField(TEXT("decisions"), ExpectedDecisions) || ExpectedDecisions == nullptr) return false;
        bPassed &= TestEqual(*FString::Printf(TEXT("seed %d decision count"), Seed), Actual.Decisions.Num(), ExpectedDecisions->Num());
        for (int32 Index = 0; Index < FMath::Min(Actual.Decisions.Num(), ExpectedDecisions->Num()); ++Index)
        {
            const TSharedPtr<FJsonObject> Item = (*ExpectedDecisions)[Index]->AsObject();
            FString PathValue;
            int32 Option = 0;
            uint64 Raw = 0;
            Item->TryGetStringField(TEXT("path"), PathValue);
            ReadInt32(Item, TEXT("option"), Option);
            ReadUint64(Item, TEXT("raw_u32"), Raw);
            bPassed &= TestEqual(TEXT("decision path"), Actual.Decisions[Index].NodePath, PathValue);
            bPassed &= TestEqual(TEXT("decision option"), Actual.Decisions[Index].OptionIndex, Option);
            bPassed &= TestEqual(TEXT("decision raw draw"), Actual.Decisions[Index].RawU32, static_cast<uint32>(Raw));
        }

        const TArray<TSharedPtr<FJsonValue>>* ExpectedDraws = nullptr;
        if (!Expected->TryGetArrayField(TEXT("draws"), ExpectedDraws) || ExpectedDraws == nullptr) return false;
        bPassed &= TestEqual(*FString::Printf(TEXT("seed %d trace count"), Seed), Actual.Draws.Num(), ExpectedDraws->Num());
        for (int32 Index = 0; Index < FMath::Min(Actual.Draws.Num(), ExpectedDraws->Num()); ++Index)
        {
            const TSharedPtr<FJsonObject> Item = (*ExpectedDraws)[Index]->AsObject();
            FString PathValue;
            FString Role;
            uint64 Raw = 0;
            Item->TryGetStringField(TEXT("path"), PathValue);
            Item->TryGetStringField(TEXT("role"), Role);
            ReadUint64(Item, TEXT("raw_u32"), Raw);
            bPassed &= TestEqual(TEXT("draw path"), Actual.Draws[Index].NodePath, PathValue);
            bPassed &= TestEqual(TEXT("draw role"), Actual.Draws[Index].Role, Role);
            bPassed &= TestEqual(TEXT("draw raw"), Actual.Draws[Index].RawU32, static_cast<uint32>(Raw));
            const double ExpectedSample = Item->GetNumberField(TEXT("sample"));
            bPassed &= TestEqual(TEXT("draw sample"), Actual.Draws[Index].Sample, ExpectedSample);
        }

        const TArray<TSharedPtr<FJsonValue>>* ExpectedLeaves = nullptr;
        if (!Expected->TryGetArrayField(TEXT("leaves"), ExpectedLeaves) || ExpectedLeaves == nullptr) return false;
        bPassed &= TestEqual(*FString::Printf(TEXT("seed %d leaf count"), Seed), Actual.Leaves.Num(), ExpectedLeaves->Num());
        for (int32 Index = 0; Index < FMath::Min(Actual.Leaves.Num(), ExpectedLeaves->Num()); ++Index)
        {
            const TSharedPtr<FJsonObject> Item = (*ExpectedLeaves)[Index]->AsObject();
            FString Resource;
            FString Origin;
            Item->TryGetStringField(TEXT("resource"), Resource);
            Item->TryGetStringField(TEXT("origin"), Origin);
            bPassed &= TestEqual(TEXT("leaf resource"), Actual.Leaves[Index].Resource, Resource);
            bPassed &= TestEqual(TEXT("leaf origin"), Actual.Leaves[Index].Origin, Origin);
            const TSharedPtr<FJsonObject>* WorldTrs = nullptr;
            if (!Item->TryGetObjectField(TEXT("world_trs"), WorldTrs) || WorldTrs == nullptr) return false;
            bPassed &= ExactTrs(*this, FString::Printf(TEXT("seed %d leaf %d"), Seed, Index), Actual.Leaves[Index].WorldTrs, *WorldTrs);
        }

        const TArray<TSharedPtr<FJsonValue>>* Selected = nullptr;
        if (!Expected->TryGetArrayField(TEXT("selected_dependencies"), Selected) || Selected == nullptr) return false;
        bPassed &= TestEqual(TEXT("selected dependency count"), Actual.SelectedDependencies.Num(), Selected->Num());
        for (int32 Index = 0; Index < FMath::Min(Actual.SelectedDependencies.Num(), Selected->Num()); ++Index)
        {
            bPassed &= TestEqual(TEXT("selected dependency"), Actual.SelectedDependencies[Index], (*Selected)[Index]->AsString());
        }
        FString ExpectedPreimage;
        FString ExpectedSignature;
        Expected->TryGetStringField(TEXT("signature_preimage_utf8"), ExpectedPreimage);
        Expected->TryGetStringField(TEXT("resolved_signature"), ExpectedSignature);
        const FUTF8ToTCHAR ActualPreimage(
            reinterpret_cast<const ANSICHAR*>(Actual.SignaturePreimage.GetData()),
            Actual.SignaturePreimage.Num());
        bPassed &= TestEqual(TEXT("signature preimage bytes"), FString(ActualPreimage.Length(), ActualPreimage.Get()), ExpectedPreimage);
        bPassed &= TestEqual(TEXT("resolved signature"), Actual.ResolvedSignature, ExpectedSignature);
    }
    return bPassed;
}

} // namespace UE::MimirComposite::Tests
