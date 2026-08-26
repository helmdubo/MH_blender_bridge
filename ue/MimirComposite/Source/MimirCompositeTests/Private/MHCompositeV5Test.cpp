#include "MHGoldenRoot.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeCompiler.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Geometry/MHGeometryBackends.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceResolver.h"
#include "UObject/Package.h"

#include <limits>

#if WITH_EDITOR
#include "Editor.h"
#endif

namespace UE::MimirComposite::Tests
{
namespace
{

TArray<uint8> Utf8Composite(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value, Value.Len());
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    return Bytes;
}

bool StartsWithCode(const FString& Error, const FString& Code)
{
    return Error.StartsWith(Code + TEXT(":"), ESearchCase::CaseSensitive);
}

class FCompositeTestResolver final : public IMHSourceResolver
{
public:
    virtual FMHSourceSnapshot GetSnapshot() const override { return FMHSourceSnapshot(); }

    virtual FMHResolveOutcome Resolve(const FMHResourceKey& Key) override
    {
        if (const FMHResolveOutcome* Outcome = Outcomes.Find(Key)) return *Outcome;
        FMHResolveOutcome Missing;
        Missing.Status = EMHResolveStatus::Unresolved;
        return Missing;
    }

    void AddResolved(const FMHResourceKey& Key, const FString& Path = FString(), const FString& Hash = FString())
    {
        FMHResolveOutcome Outcome;
        Outcome.Status = EMHResolveStatus::Resolved;
        Outcome.PayloadPath = Path;
        Outcome.RawHash = Hash;
        Outcomes.Add(Key, MoveTemp(Outcome));
    }

private:
    TMap<FMHResourceKey, FMHResolveOutcome> Outcomes;
};

class FFailOnSecondResolve final : public IMHSourceResolver
{
public:
    explicit FFailOnSecondResolve(FMHResourceKey InKey) : ExpectedKey(MoveTemp(InKey)) {}

    virtual FMHSourceSnapshot GetSnapshot() const override { return FMHSourceSnapshot(); }

    virtual FMHResolveOutcome Resolve(const FMHResourceKey& Key) override
    {
        FMHResolveOutcome Outcome;
        ++ResolveCount;
        if (Key == ExpectedKey && ResolveCount == 1)
        {
            Outcome.Status = EMHResolveStatus::Resolved;
        }
        else
        {
            Outcome.Status = EMHResolveStatus::Unresolved;
        }
        return Outcome;
    }

private:
    FMHResourceKey ExpectedKey;
    int32 ResolveCount = 0;
};

FMHResourceKey CompositeTestKey(const EMHResourceKind Kind, const FString& Name)
{
    FMHResourceKey Result;
    Result.Kind = Kind;
    Result.LogicalName = Name;
    return Result;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeGoldenVectorsTest,
    "Mimir.V5.Composite.CanonicalGoldenVectors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeGoldenVectorsTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    FString FixtureText;
    const FString FixturePath = FPaths::Combine(GoldenRoot, TEXT("v5/source_protocol_v5_codec_vectors.json"));
    if (!FFileHelper::LoadFileToString(FixtureText, *FixturePath))
    {
        AddError(FString::Printf(TEXT("cannot read %s"), *FixturePath));
        return false;
    }
    TSharedPtr<FJsonObject> Fixture;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FixtureText), Fixture) || !Fixture.IsValid())
    {
        AddError(TEXT("source_protocol_v5_codec_vectors.json is not valid JSON"));
        return false;
    }
    FString Schema;
    bool bPassed = TestTrue(TEXT("fixture schema"),
        Fixture->TryGetStringField(TEXT("schema"), Schema) && Schema == TEXT("mh.source_protocol_v5_codec_vectors:1"));
    const TArray<TSharedPtr<FJsonValue>>* Vectors = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("composite_vectors"), Vectors) || Vectors == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Vectors)
    {
        const TSharedPtr<FJsonObject> Vector = Value->AsObject();
        FString Name;
        FString Expected;
        if (!Vector.IsValid() || !Vector->TryGetStringField(TEXT("name"), Name) ||
            !Vector->TryGetStringField(TEXT("canonical_utf8"), Expected))
        {
            AddError(TEXT("malformed composite golden vector"));
            return false;
        }
        FMHCompositeDocument Parsed;
        TArray<uint8> Actual;
        FString Error;
        bPassed &= TestTrue(*FString::Printf(TEXT("%s parses"), *Name),
            MHParseCompositeV5(Utf8Composite(Expected), Parsed, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("%s writes"), *Name),
            MHWriteCanonicalCompositeV5(Parsed, Actual, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("%s exact bytes"), *Name), Actual == Utf8Composite(Expected));
    }

    const TArray<TSharedPtr<FJsonValue>>* Negative = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("composite_negative_vectors"), Negative) || Negative == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Negative)
    {
        const TSharedPtr<FJsonObject> Vector = Value->AsObject();
        FString Name;
        FString Json;
        FString Code;
        if (!Vector.IsValid() || !Vector->TryGetStringField(TEXT("name"), Name) ||
            !Vector->TryGetStringField(TEXT("json"), Json) || !Vector->TryGetStringField(TEXT("error"), Code))
        {
            AddError(TEXT("malformed negative composite vector"));
            return false;
        }
        FMHCompositeDocument Parsed;
        FString Error;
        bPassed &= TestFalse(*FString::Printf(TEXT("%s rejected"), *Name),
            MHParseCompositeV5(Utf8Composite(Json), Parsed, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("%s error code"), *Name), StartsWithCode(Error, Code));
        FString RequiredMessage;
        if (Vector->TryGetStringField(TEXT("message_contains"), RequiredMessage))
        {
            bPassed &= TestTrue(*FString::Printf(TEXT("%s mandatory message"), *Name), Error.Contains(RequiredMessage));
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* PlacementVectors = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("placement_vectors"), PlacementVectors) || PlacementVectors == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *PlacementVectors)
    {
        const TSharedPtr<FJsonObject> Vector = Value->AsObject();
        FString Name;
        FString Expected;
        if (!Vector.IsValid() || !Vector->TryGetStringField(TEXT("name"), Name) ||
            !Vector->TryGetStringField(TEXT("canonical_utf8"), Expected)) return false;
        FMHPlacementProfile Parsed;
        Parsed.LogicalName = TEXT("golden_profile");
        TArray<uint8> Actual;
        FString Error;
        bPassed &= TestTrue(*FString::Printf(TEXT("placement %s parses"), *Name),
            MHParsePlacementProfileV1(Utf8Composite(Expected), Parsed, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("placement %s writes"), *Name),
            MHWriteCanonicalPlacementProfileV1(Parsed, Actual, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("placement %s exact bytes"), *Name), Actual == Utf8Composite(Expected));
    }
    const TArray<TSharedPtr<FJsonValue>>* PlacementNegative = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("placement_negative_vectors"), PlacementNegative) || PlacementNegative == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *PlacementNegative)
    {
        const TSharedPtr<FJsonObject> Vector = Value->AsObject();
        FString Name;
        FString Json;
        FString Code;
        if (!Vector.IsValid() || !Vector->TryGetStringField(TEXT("name"), Name) ||
            !Vector->TryGetStringField(TEXT("json"), Json) || !Vector->TryGetStringField(TEXT("error"), Code)) return false;
        FMHPlacementProfile Parsed;
        Parsed.LogicalName = TEXT("golden_profile");
        FString Error;
        bPassed &= TestFalse(*FString::Printf(TEXT("placement %s rejected"), *Name),
            MHParsePlacementProfileV1(Utf8Composite(Json), Parsed, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("placement %s error code"), *Name), StartsWithCode(Error, Code));
    }

    const TArray<TSharedPtr<FJsonValue>>* MatrixVectors = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("transform_representability_vectors"), MatrixVectors) || MatrixVectors == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *MatrixVectors)
    {
        const TSharedPtr<FJsonObject> Vector = Value->AsObject();
        FString Name;
        bool bExpected = false;
        const TArray<TSharedPtr<FJsonValue>>* SourceRows = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* RestoredRows = nullptr;
        if (!Vector.IsValid() || !Vector->TryGetStringField(TEXT("name"), Name) ||
            !Vector->TryGetBoolField(TEXT("representable"), bExpected) ||
            !Vector->TryGetArrayField(TEXT("matrix"), SourceRows) || SourceRows == nullptr ||
            !Vector->TryGetArrayField(TEXT("reconstructed"), RestoredRows) || RestoredRows == nullptr ||
            SourceRows->Num() != 4 || RestoredRows->Num() != 4) return false;
        FMatrix Source = FMatrix::Identity;
        FMatrix Restored = FMatrix::Identity;
        for (int32 Row = 0; Row < 4; ++Row)
        {
            if ((*SourceRows)[Row]->AsArray().Num() != 4 || (*RestoredRows)[Row]->AsArray().Num() != 4) return false;
            for (int32 Column = 0; Column < 4; ++Column)
            {
                Source.M[Row][Column] = (*SourceRows)[Row]->AsArray()[Column]->AsNumber();
                Restored.M[Row][Column] = (*RestoredRows)[Row]->AsArray()[Column]->AsNumber();
            }
        }
        bPassed &= TestEqual(
            *FString::Printf(TEXT("%s 8-ULP predicate"), *Name),
            MHMatrixElementsWithinTrsTolerance(Source, Restored),
            bExpected);
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeApplyExtractTest,
    "Mimir.V5.Composite.ApplyExtractAndLocalEdit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeApplyExtractTest::RunTest(const FString& Parameters)
{
    const TArray<uint8> Source = Utf8Composite(
        TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"group\",\"name\":\"root\",")
        TEXT("\"transform\":{\"translation_cm\":[100,20,3]},\"children\":[")
        TEXT("{\"kind\":\"actor\",\"resource\":\"light\",\"transform\":{\"translation_cm\":[25,0,0]}}]}]}"));
    FMHCompositeDocument Parsed;
    FString Error;
    bool bPassed = TestTrue(TEXT("source parses"), MHParseCompositeV5(Source, Parsed, Error));
    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>();
    bPassed &= TestTrue(TEXT("apply succeeds"), MHApplyCompositeV5(*Asset, Parsed, Error));
    FMHCompositeDocument Extracted;
    TArray<uint8> Canonical;
    TArray<uint8> ExtractedBytes;
    bPassed &= TestTrue(TEXT("extract succeeds"), MHExtractCompositeV5(*Asset, Extracted, Error));
    bPassed &= TestTrue(TEXT("source canonical writes"), MHWriteCanonicalCompositeV5(Parsed, Canonical, Error));
    bPassed &= TestTrue(TEXT("extract canonical writes"), MHWriteCanonicalCompositeV5(Extracted, ExtractedBytes, Error));
    bPassed &= TestTrue(TEXT("apply/extract exact canonical bytes"), ExtractedBytes == Canonical);
    Asset->AppliedHash = MHRawPayloadHash(Canonical);
    FString Warning;
    bPassed &= TestFalse(TEXT("fresh receipt is not locally modified"),
        MHDetectManagedCompositeLocalModification(*Asset, Warning));
    Asset->Nodes[1].Transform.SetTranslation(FVector(26.0, 0.0, 0.0));
    bPassed &= TestTrue(TEXT("changed applied node is locally modified"),
        MHDetectManagedCompositeLocalModification(*Asset, Warning));
    bPassed &= TestTrue(TEXT("local modification warning is machine-coded"),
        Warning.StartsWith(TEXT("MH_W_MANAGED_ASSET_LOCALLY_MODIFIED:")));

    FMHCompositeDocument ProfiledDocument;
    FMHCompositeNode& ProfiledNode = ProfiledDocument.Nodes.AddDefaulted_GetRef();
    ProfiledNode.Kind = EMHCompositeNodeKind::Group;
    ProfiledNode.Profile = TEXT("scatter_profile");
    FMHPlacementProfile Profile;
    Profile.LogicalName = TEXT("scatter_profile");
    Profile.bHasUniformScale = true;
    Profile.UniformScale.Base = 1.0f;
    Profile.UniformScale.Deviation = 0.25f;
    const TArray<FMHPlacementProfile> Profiles = {Profile};
    UMHCompositeAsset* ProfiledAsset = NewObject<UMHCompositeAsset>();
    bPassed &= TestFalse(
        TEXT("profile reference cannot apply without its source-only carrier"),
        MHApplyCompositeV5(*ProfiledAsset, ProfiledDocument, Error));
    bPassed &= TestTrue(
        TEXT("profile reference applies with exactly matching inline carrier"),
        MHApplyCompositeV5(*ProfiledAsset, ProfiledDocument, Profiles, Error));
    bPassed &= TestEqual(
        TEXT("UMHCompositeAsset stores one inline typed profile"),
        ProfiledAsset->InlinedPlacementProfiles.Num(),
        1);
    if (ProfiledAsset->InlinedPlacementProfiles.Num() == 1)
    {
        bPassed &= TestEqual(
            TEXT("inline profile retains logical identity"),
            ProfiledAsset->InlinedPlacementProfiles[0].LogicalName,
            FString(TEXT("scatter_profile")));
    }

    FMHCompositeDocument NonFiniteWriter;
    FMHCompositeNode& InvalidNode = NonFiniteWriter.Nodes.AddDefaulted_GetRef();
    InvalidNode.Kind = EMHCompositeNodeKind::Group;
    InvalidNode.Transform.RotationQuat = FQuat(
        std::numeric_limits<double>::infinity(), 0.0, 0.0, 1.0);
    TArray<uint8> InvalidBytes;
    bPassed &= TestFalse(TEXT("writer rejects non-finite quaternion"),
        MHWriteCanonicalCompositeV5(NonFiniteWriter, InvalidBytes, Error));
    bPassed &= TestTrue(TEXT("writer non-finite quaternion code"),
        StartsWithCode(Error, TEXT("MH_E_NAN_INF_VALUE")));

    UMHCompositeAsset* InvalidAsset = NewObject<UMHCompositeAsset>();
    FMHCompositeAssetNode& StoredInvalid = InvalidAsset->Nodes.AddDefaulted_GetRef();
    StoredInvalid.Kind = EMHCompositeNodeKind::Group;
    StoredInvalid.Transform = FTransform(
        FQuat(TNumericLimits<double>::Max(), 0.0, 0.0, 1.0),
        FVector::ZeroVector,
        FVector::OneVector);
    FMHCompositeDocument InvalidExtract;
    bPassed &= TestFalse(TEXT("extract rejects quaternion outside finite float32"),
        MHExtractCompositeV5(*InvalidAsset, InvalidExtract, Error));
    bPassed &= TestTrue(TEXT("extract non-finite quaternion code"),
        StartsWithCode(Error, TEXT("MH_E_NAN_INF_VALUE")));
    const FMHCompositeOperationResult InvalidPublish = MHPublishCompositeV5(
        *InvalidAsset, FPaths::ProjectSavedDir());
    bPassed &= TestFalse(TEXT("publish rejects non-finite extracted quaternion"),
        InvalidPublish.Succeeded());
    bPassed &= TestTrue(TEXT("publish preserves non-finite quaternion code"),
        StartsWithCode(InvalidPublish.Error, TEXT("MH_E_NAN_INF_VALUE")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeClosureTest,
    "Mimir.V5.Composite.ClosureCycleAndUnresolved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeClosureTest::RunTest(const FString& Parameters)
{
    const FString TempRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests/closure"));
    IFileManager::Get().MakeDirectory(*TempRoot, true);
    const FString NestedPath = FPaths::Combine(TempRoot, TEXT("nested.composite"));
    const TArray<uint8> NestedBytes = Utf8Composite(
        TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"composite\",\"resource\":\"root\"}]}"));
    FFileHelper::SaveArrayToFile(NestedBytes, *NestedPath);

    FMHCompositeDocument Root;
    FString Error;
    MHParseCompositeV5(
        Utf8Composite(TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"random\",\"options\":[")
            TEXT("{\"kind\":\"empty\",\"weight\":1},{\"kind\":\"composite\",\"resource\":\"nested\",\"weight\":0}]}]}")),
        Root,
        Error);
    FCompositeTestResolver Resolver;
    Resolver.AddResolved(CompositeTestKey(EMHResourceKind::Composite, TEXT("nested")), NestedPath, MHRawPayloadHash(NestedBytes));
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    bool bPassed = TestFalse(TEXT("cycle in non-selected zero-weight option rejected"),
        MHValidateCompositeClosureV5(TEXT("root"), Root, Resolver, *Settings, Error));
    bPassed &= TestTrue(TEXT("cycle code"), StartsWithCode(Error, TEXT("MH_E_COMPOSITE_CYCLE")));

    FMHCompositeDocument MissingActor;
    MHParseCompositeV5(
        Utf8Composite(TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"actor\",\"resource\":\"missing\"}]}")),
        MissingActor,
        Error);
    bPassed &= TestFalse(TEXT("unresolved actor rejected"),
        MHValidateCompositeClosureV5(TEXT("root"), MissingActor, Resolver, *Settings, Error));
    bPassed &= TestTrue(TEXT("unresolved code"),
        StartsWithCode(Error, TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE")));
    IFileManager::Get().Delete(*NestedPath, false, true, true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeImportPublishReceiptTest,
    "Mimir.V5.Composite.ImportPublishReceipts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeImportPublishReceiptTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests/import_publish"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString SourcePath = FPaths::Combine(SourceRoot, TEXT("ue_s3_roundtrip.composite"));
    const TArray<uint8> RawBytes = Utf8Composite(TEXT("{ \"v\" : 5, \"nodes\" : [] }\r\n"));
    FFileHelper::SaveArrayToFile(RawBytes, *SourcePath);

    FMHSourceAnalysisEntry Entry;
    Entry.Key = CompositeTestKey(EMHResourceKind::Composite, TEXT("ue_s3_roundtrip"));
    Entry.PayloadPath = SourcePath;
    Entry.SourcePath = TEXT("ue_s3_roundtrip.composite");
    Entry.RawHash = MHRawPayloadHash(RawBytes);
    Entry.Change = EMHSourceChange::Create;
    FCompositeTestResolver Resolver;
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHCompositeOperationResult Imported = MHImportCompositeV5(
        Entry, Resolver, SourceRoot, *Settings);
    bool bPassed = TestTrue(TEXT("empty composite imports"), Imported.Succeeded());
    if (!Imported.Succeeded())
    {
        AddError(Imported.Error);
        return false;
    }
    bPassed &= TestEqual(TEXT("logical receipt"), Imported.Asset->LogicalName, TEXT("ue_s3_roundtrip"));
    bPassed &= TestEqual(TEXT("source path receipt"), Imported.Asset->SourceRelativePath, Entry.SourcePath);
    bPassed &= TestEqual(TEXT("raw source hash receipt"), Imported.Asset->SourceHash, MHRawPayloadHash(RawBytes));
    const TArray<uint8> CanonicalEmpty = Utf8Composite(TEXT("{\n  \"v\": 5,\n  \"nodes\": []\n}\n"));
    bPassed &= TestEqual(TEXT("applied canonical hash receipt"),
        Imported.Asset->AppliedHash, MHRawPayloadHash(CanonicalEmpty));
    bPassed &= TestNotEqual(TEXT("dual hashes retain raw/canonical distinction"),
        Imported.Asset->SourceHash, Imported.Asset->AppliedHash);

    const FAssetData AssetData = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
        .Get().GetAssetByObjectPath(FSoftObjectPath::ConstructFromObject(Imported.Asset));
    TSet<FName> MHTags;
    AssetData.TagsAndValues.ForEach([&MHTags](const TPair<FName, FAssetTagValueRef>& Pair)
    {
        if (Pair.Key.ToString().StartsWith(TEXT("MH."), ESearchCase::CaseSensitive)) MHTags.Add(Pair.Key);
    });
    bPassed &= TestEqual(TEXT("exactly six MH registry tags"), MHTags.Num(), 6);
    bPassed &= TestTrue(TEXT("kind tag"), MHTags.Contains(FName(TEXT("MH.Kind"))));
    bPassed &= TestTrue(TEXT("logical tag"), MHTags.Contains(FName(TEXT("MH.LogicalName"))));
    bPassed &= TestTrue(TEXT("source tag"), MHTags.Contains(FName(TEXT("MH.SourcePath"))));
    bPassed &= TestTrue(TEXT("source hash tag"), MHTags.Contains(FName(TEXT("MH.SourceHash"))));
    bPassed &= TestTrue(TEXT("applied tag"), MHTags.Contains(FName(TEXT("MH.AppliedHash"))));
    bPassed &= TestTrue(TEXT("managed tag"), MHTags.Contains(FName(TEXT("MH.Managed"))));
    FString SourceHashTag;
    bPassed &= TestTrue(
        TEXT("source hash tag projects the raw receipt"),
        AssetData.GetTagValue(FName(TEXT("MH.SourceHash")), SourceHashTag) &&
            SourceHashTag == Imported.Asset->SourceHash);

    FMHCompositeDocument Edited;
    FString Error;
    MHParseCompositeV5(Utf8Composite(
        TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"group\",\"name\":\"published\"}]}")), Edited, Error);
    bPassed &= TestTrue(TEXT("local source-shaped edit applies"),
        MHApplyCompositeV5(*Imported.Asset, Edited, Error));
    FMHCompositeOperationResult Published = MHPublishCompositeV5(
        *Imported.Asset, SourceRoot);
    bPassed &= TestTrue(TEXT("managed publish succeeds"), Published.Succeeded());
    if (!Published.Succeeded()) AddError(Published.Error);
    TArray<uint8> PublishedBytes;
    bPassed &= TestTrue(TEXT("published source readable"),
        FFileHelper::LoadFileToArray(PublishedBytes, *SourcePath));
    bPassed &= TestEqual(TEXT("publish source/applied hashes converge"),
        Imported.Asset->SourceHash, Imported.Asset->AppliedHash);
    bPassed &= TestEqual(TEXT("publish receipt hashes canonical file"),
        Imported.Asset->SourceHash, MHRawPayloadHash(PublishedBytes));

    Imported.Asset->LogicalName.Reset();
    Imported.Asset->SourceRelativePath.Reset();
    Imported.Asset->SourceHash.Reset();
    Imported.Asset->AppliedHash.Reset();
    const FString AdoptFolder = FPaths::Combine(SourceRoot, TEXT("adopted"));
    FMHCompositeAdoptTarget Adopt{AdoptFolder, TEXT("ue_s3_adopted")};
    FMHCompositeOperationResult Adopted = MHPublishCompositeV5(
        *Imported.Asset, SourceRoot, &Adopt);
    bPassed &= TestTrue(TEXT("unmanaged Adopt publish succeeds"), Adopted.Succeeded());
    if (!Adopted.Succeeded()) AddError(Adopted.Error);
    bPassed &= TestTrue(TEXT("Adopt writes exact target"),
        FPaths::FileExists(FPaths::Combine(AdoptFolder, TEXT("ue_s3_adopted.composite"))));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositePlacementProfileReceiptTest,
    "Mimir.V5.Composite.PlacementProfileAppliedSourceHash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositePlacementProfileReceiptTest::RunTest(const FString& Parameters)
{
    const FString Token = FString::Printf(TEXT("profile_receipt_%08x"), FPlatformTime::Cycles());
    const FString SourceRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests"), Token);
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString CompositePath = FPaths::Combine(SourceRoot, Token + TEXT(".composite"));
    const FString PlacementPath = FPaths::Combine(SourceRoot, TEXT("receipt_profile.placement"));
    const TArray<uint8> CompositeBytes = Utf8Composite(
        TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"group\",\"profile\":\"receipt_profile\"}]}"));
    const TArray<uint8> PlacementBytes = Utf8Composite(
        TEXT("{ \"v\" : 1, \"kind\" : \"placement_profile\", \"uniform_scale\" : [ 1, 0.25 ] }\r\n"));
    FFileHelper::SaveArrayToFile(CompositeBytes, *CompositePath);
    FFileHelper::SaveArrayToFile(PlacementBytes, *PlacementPath);

    FMHSourceAnalysisEntry Entry;
    Entry.Key = CompositeTestKey(EMHResourceKind::Composite, Token);
    Entry.PayloadPath = CompositePath;
    Entry.SourcePath = Token + TEXT(".composite");
    Entry.RawHash = MHRawPayloadHash(CompositeBytes);
    Entry.Change = EMHSourceChange::Create;
    FCompositeTestResolver Resolver;
    Resolver.AddResolved(
        CompositeTestKey(EMHResourceKind::PlacementProfile, TEXT("receipt_profile")),
        PlacementPath,
        MHRawPayloadHash(PlacementBytes));
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    const FMHCompositeOperationResult Imported = MHImportCompositeV5(
        Entry, Resolver, SourceRoot, *Settings);
    bool bPassed = TestTrue(TEXT("profiled composite imports"), Imported.Succeeded());
    if (!Imported.Succeeded())
    {
        AddError(Imported.Error);
        return false;
    }
    bPassed &= TestEqual(
        TEXT("one placement profile is inlined"),
        Imported.Asset->InlinedPlacementProfiles.Num(),
        1);
    if (Imported.Asset->InlinedPlacementProfiles.Num() != 1)
    {
        return false;
    }
    FMHPlacementProfile& Profile = Imported.Asset->InlinedPlacementProfiles[0];
    const FString ExactProfileHash = MHRawPayloadHash(PlacementBytes);
    bPassed &= TestEqual(
        TEXT("private receipt stores exact raw placement bytes hash"),
        Profile.GetAppliedSourceHash(),
        ExactProfileHash);
    TArray<uint8> SerializedProfile;
    FMemoryWriter ProfileWriter(SerializedProfile);
    FMHPlacementProfile::StaticStruct()->SerializeItem(
        ProfileWriter,
        &Profile,
        nullptr);
    ProfileWriter.Close();
    FMHPlacementProfile ReloadedProfile;
    FMemoryReader ProfileReader(SerializedProfile);
    FMHPlacementProfile::StaticStruct()->SerializeItem(
        ProfileReader,
        &ReloadedProfile,
        nullptr);
    ProfileReader.Close();
    bPassed &= TestEqual(
        TEXT("private profile receipt survives reflected serialization"),
        ReloadedProfile.GetAppliedSourceHash(),
        ExactProfileHash);
    TArray<uint8> CanonicalPlacement;
    FString Error;
    bPassed &= TestTrue(
        TEXT("receipt-bearing profile still writes canonical placement wire bytes"),
        MHWriteCanonicalPlacementProfileV1(Profile, CanonicalPlacement, Error));
    if (!Error.IsEmpty()) AddError(Error);
    bPassed &= TestNotEqual(
        TEXT("receipt hash remains raw rather than canonicalized"),
        Profile.GetAppliedSourceHash(),
        MHRawPayloadHash(CanonicalPlacement));
    FMHCompositeDocument ExtractedBefore;
    TArray<uint8> CompositeBefore;
    bPassed &= TestTrue(
        TEXT("extract receipt-bearing composite"),
        MHExtractCompositeV5(*Imported.Asset, ExtractedBefore, Error) &&
        MHWriteCanonicalCompositeV5(ExtractedBefore, CompositeBefore, Error));
    const FString AppliedHashBefore = Imported.Asset->AppliedHash;
    Profile.SetAppliedSourceHash(MHRawPayloadHash(Utf8Composite(TEXT("different receipt bytes"))));
    FMHCompositeDocument ExtractedAfter;
    TArray<uint8> CompositeAfter;
    bPassed &= TestTrue(
        TEXT("extract composite after private receipt-only edit"),
        MHExtractCompositeV5(*Imported.Asset, ExtractedAfter, Error) &&
        MHWriteCanonicalCompositeV5(ExtractedAfter, CompositeAfter, Error));
    bPassed &= TestTrue(
        TEXT("private profile receipt is excluded from canonical composite extract"),
        CompositeAfter == CompositeBefore);
    bPassed &= TestEqual(
        TEXT("private profile receipt does not change composite AppliedHash"),
        Imported.Asset->AppliedHash,
        AppliedHashBefore);
    Profile.SetAppliedSourceHash(ExactProfileHash);

    const FAssetData AssetData = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
        .Get().GetAssetByObjectPath(FSoftObjectPath::ConstructFromObject(Imported.Asset));
    TSet<FName> MHTags;
    AssetData.TagsAndValues.ForEach([&MHTags](const TPair<FName, FAssetTagValueRef>& Pair)
    {
        if (Pair.Key.ToString().StartsWith(TEXT("MH."), ESearchCase::CaseSensitive))
        {
            MHTags.Add(Pair.Key);
        }
    });
    bPassed &= TestEqual(
        TEXT("profile receipt adds no seventh Asset Registry tag"),
        MHTags.Num(),
        6);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSourceRaceNoGhostTest,
    "Mimir.V5.Composite.SourceRaceNoGhost",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSourceRaceNoGhostTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests/race"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString Token = FString::Printf(TEXT("ue_s3_race_%08x"), FPlatformTime::Cycles());
    const FString SourcePath = FPaths::Combine(SourceRoot, Token + TEXT(".composite"));
    const TArray<uint8> Initial = Utf8Composite(TEXT("{\"v\":5,\"nodes\":[]}"));
    FFileHelper::SaveArrayToFile(Initial, *SourcePath);
    FMHSourceAnalysisEntry Entry;
    Entry.Key = CompositeTestKey(EMHResourceKind::Composite, Token);
    Entry.PayloadPath = SourcePath;
    Entry.SourcePath = Token + TEXT(".composite");
    Entry.RawHash = MHRawPayloadHash(Initial);
    Entry.Change = EMHSourceChange::Create;
    FCompositeTestResolver Resolver;
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    MHSetBeforeCompositeSourceCommitTestHook([SourcePath]()
    {
        FFileHelper::SaveStringToFile(TEXT("{\"v\":5,\"nodes\":[]}\n"), *SourcePath);
    });
    const FMHCompositeOperationResult Result = MHImportCompositeV5(
        Entry, Resolver, SourceRoot, *Settings);
    bool bPassed = TestFalse(TEXT("source race blocks import"), Result.Succeeded());
    bPassed &= TestTrue(TEXT("source race code"),
        StartsWithCode(Result.Error, TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED")));
    const FString ObjectPath = FString::Printf(
        TEXT("/Game/MH/Generated/Composites/%s.%s"), *Token, *Token);
    bPassed &= TestNull(TEXT("source race creates no generated asset"),
        StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));

    const FString ProfileToken = FString::Printf(TEXT("profile_race_%08x"), FPlatformTime::Cycles());
    const FString ProfileSourcePath = FPaths::Combine(SourceRoot, ProfileToken + TEXT(".composite"));
    const FString PlacementPath = FPaths::Combine(SourceRoot, TEXT("race_profile.placement"));
    const TArray<uint8> ProfileCompositeBytes = Utf8Composite(
        TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"group\",\"profile\":\"race_profile\"}]}"));
    const TArray<uint8> InitialPlacementBytes = Utf8Composite(
        TEXT("{\"v\":1,\"kind\":\"placement_profile\",\"offset_cm\":[[0,1],[0,1],[0,1]]}"));
    FFileHelper::SaveArrayToFile(ProfileCompositeBytes, *ProfileSourcePath);
    FFileHelper::SaveArrayToFile(InitialPlacementBytes, *PlacementPath);
    FMHSourceAnalysisEntry ProfileEntry;
    ProfileEntry.Key = CompositeTestKey(EMHResourceKind::Composite, ProfileToken);
    ProfileEntry.PayloadPath = ProfileSourcePath;
    ProfileEntry.SourcePath = ProfileToken + TEXT(".composite");
    ProfileEntry.RawHash = MHRawPayloadHash(ProfileCompositeBytes);
    ProfileEntry.Change = EMHSourceChange::Create;
    FCompositeTestResolver ProfileResolver;
    ProfileResolver.AddResolved(
        CompositeTestKey(EMHResourceKind::PlacementProfile, TEXT("race_profile")),
        PlacementPath,
        MHRawPayloadHash(InitialPlacementBytes));
    MHSetBeforeCompositeSourceCommitTestHook([PlacementPath]()
    {
        FFileHelper::SaveStringToFile(
            TEXT("{\"v\":1,\"kind\":\"placement_profile\",\"offset_cm\":[[0,2],[0,1],[0,1]]}"),
            *PlacementPath);
    });
    const FMHCompositeOperationResult ProfileRace = MHImportCompositeV5(
        ProfileEntry, ProfileResolver, SourceRoot, *Settings);
    bPassed &= TestFalse(TEXT("placement profile race blocks import"), ProfileRace.Succeeded());
    bPassed &= TestTrue(TEXT("placement profile race code"),
        StartsWithCode(ProfileRace.Error, TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED")));
    const FString ProfileObjectPath = FString::Printf(
        TEXT("/Game/MH/Generated/Composites/%s.%s"), *ProfileToken, *ProfileToken);
    bPassed &= TestNull(TEXT("placement profile race creates no generated asset"),
        StaticFindObject(UObject::StaticClass(), nullptr, *ProfileObjectPath));
    return bPassed;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeCompilerParentLocalTransformTest,
    "Mimir.V5.Composite.CompilerComposesParentLocalTransforms",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeCompilerParentLocalTransformTest::RunTest(const FString& Parameters)
{
    if (GEditor == nullptr || GEditor->GetEditorWorldContext().World() == nullptr)
    {
        AddError(TEXT("editor world is unavailable"));
        return false;
    }
    UPackage* MeshPackage = CreatePackage(TEXT("/Game/MH/Generated/Meshes/world_probe"));
    UStaticMesh* Mesh = FindObject<UStaticMesh>(MeshPackage, TEXT("world_probe"));
    if (Mesh == nullptr)
    {
        Mesh = NewObject<UStaticMesh>(MeshPackage, TEXT("world_probe"), RF_Public | RF_Standalone);
    }
    FCompositeTestResolver Resolver;
    Resolver.AddResolved(CompositeTestKey(EMHResourceKind::StaticMesh, TEXT("world_probe")));
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHCompositeDocument Document;
    FString Error;
    if (!MHParseCompositeV5(Utf8Composite(
            TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"group\",\"name\":\"parent\",\"transform\":{")
            TEXT("\"translation_cm\":[100,0,0]},\"children\":[{\"kind\":\"mesh\",")
            TEXT("\"resource\":\"world_probe\",\"transform\":{\"translation_cm\":[25,0,0]}}]}]}")),
            Document, Error))
    {
        AddError(Error);
        return false;
    }
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transient;
    AActor* Target = GEditor->GetEditorWorldContext().World()->SpawnActor<AActor>(SpawnParameters);
    if (Target == nullptr)
    {
        AddError(TEXT("cannot spawn compiler target"));
        return false;
    }
    const FMHCompositeCompileResult Compiled = MHCompileCompositeV5(
        *Target, TEXT("root"), Document, Resolver, *Settings);
    bool bPassed = TestTrue(TEXT("compiler succeeds"), Compiled.Succeeded());
    bPassed &= TestEqual(TEXT("group and mesh components"), Compiled.Components.Num(), 2);
    if (Compiled.Components.Num() == 2)
    {
        const USceneComponent* Group = Cast<USceneComponent>(Compiled.Components[0]);
        const USceneComponent* Child = Cast<USceneComponent>(Compiled.Components[1]);
        bPassed &= TestTrue(TEXT("group component"), Group != nullptr);
        bPassed &= TestTrue(TEXT("child component"), Child != nullptr);
        if (Group != nullptr && Child != nullptr)
        {
            bPassed &= TestTrue(TEXT("parent local transform"),
                Group->GetComponentLocation().Equals(FVector(100, 0, 0), UE_KINDA_SMALL_NUMBER));
            bPassed &= TestTrue(TEXT("parent 100 plus child local 25 equals world 125"),
                Child->GetComponentLocation().Equals(FVector(125, 0, 0), UE_KINDA_SMALL_NUMBER));
            bPassed &= TestTrue(TEXT("authored transform parent retained"), Child->GetAttachParent() == Group);
        }
    }
    Target->Destroy();

    FMHCompositeDocument ShearDocument;
    FMHCompositeNode& ShearParent = ShearDocument.Nodes.AddDefaulted_GetRef();
    ShearParent.Kind = EMHCompositeNodeKind::Group;
    ShearParent.Transform.Scale = FVector(2.0, 1.0, 1.0);
    FMHCompositeNode& RotatedChild = ShearParent.Children.AddDefaulted_GetRef();
    RotatedChild.Kind = EMHCompositeNodeKind::Mesh;
    RotatedChild.Resource = TEXT("world_probe");
    RotatedChild.Transform.RotationQuat = FQuat(FVector::UpVector, FMath::DegreesToRadians(45.0));
    AActor* ShearTarget = GEditor->GetEditorWorldContext().World()->SpawnActor<AActor>(SpawnParameters);
    if (ShearTarget == nullptr)
    {
        AddError(TEXT("cannot spawn shear compiler target"));
        return false;
    }
    const FMHCompositeCompileResult ShearResult = MHCompileCompositeV5(
        *ShearTarget,
        TEXT("root"),
        ShearDocument,
        Resolver,
        *Settings);
    bPassed &= TestFalse(TEXT("compiler rejects accumulated shear without TRS approximation"), ShearResult.Succeeded());
    bPassed &= TestTrue(TEXT("compiler shear failure uses frozen code"),
        ShearResult.Error.StartsWith(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM:")));
    bPassed &= TestEqual(TEXT("shear failure mutates no authored components"), ShearResult.Components.Num(), 0);
    ShearTarget->Destroy();

    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeCompilerTopLevelAttachmentTest,
    "Mimir.V5.Composite.CompilerTopLevelAttachment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeCompilerTopLevelAttachmentTest::RunTest(const FString& Parameters)
{
    if (GEditor == nullptr || GEditor->GetEditorWorldContext().World() == nullptr)
    {
        AddError(TEXT("editor world is unavailable"));
        return false;
    }
    UPackage* MeshPackage = CreatePackage(TEXT("/Game/MH/Generated/Meshes/top_level_probe"));
    UStaticMesh* Mesh = FindObject<UStaticMesh>(MeshPackage, TEXT("top_level_probe"));
    if (Mesh == nullptr)
    {
        Mesh = NewObject<UStaticMesh>(MeshPackage, TEXT("top_level_probe"), RF_Public | RF_Standalone);
    }
    FCompositeTestResolver Resolver;
    Resolver.AddResolved(CompositeTestKey(EMHResourceKind::StaticMesh, TEXT("top_level_probe")));
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHCompositeDocument Document;
    FString Error;
    if (!MHParseCompositeV5(Utf8Composite(
            TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"mesh\",\"resource\":\"top_level_probe\",")
            TEXT("\"name\":\"lights/front: left \\u03bb\",")
            TEXT("\"transform\":{\"translation_cm\":[10,0,0]}},{\"kind\":\"mesh\",")
            TEXT("\"resource\":\"top_level_probe\",\"transform\":{\"translation_cm\":[20,0,0]}}]}")),
            Document, Error))
    {
        AddError(Error);
        return false;
    }
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transient;
    AActor* Target = GEditor->GetEditorWorldContext().World()->SpawnActor<AActor>(SpawnParameters);
    if (Target == nullptr)
    {
        AddError(TEXT("cannot spawn top-level compiler target"));
        return false;
    }
    const FMHCompositeCompileResult Compiled = MHCompileCompositeV5(
        *Target, TEXT("top_level_root"), Document, Resolver, *Settings);
    bool bPassed = TestTrue(TEXT("two-top-level compiler succeeds"), Compiled.Succeeded());
    bPassed &= TestEqual(TEXT("display-only name survives protocol parse"),
        Document.Nodes[0].Name, FString(TEXT("lights/front: left \u03bb")));
    bPassed &= TestEqual(TEXT("synthetic root excluded from authored result"), Compiled.Components.Num(), 2);
    USceneComponent* SyntheticRoot = Target->GetRootComponent();
    bPassed &= TestTrue(TEXT("synthetic target root exists"), SyntheticRoot != nullptr);
    if (Compiled.Components.Num() == 2 && SyntheticRoot != nullptr)
    {
        USceneComponent* First = Cast<USceneComponent>(Compiled.Components[0]);
        USceneComponent* Second = Cast<USceneComponent>(Compiled.Components[1]);
        bPassed &= TestTrue(TEXT("first top-level attached to synthetic root"),
            First != nullptr && First->GetAttachParent() == SyntheticRoot);
        bPassed &= TestTrue(TEXT("second top-level attached to synthetic root"),
            Second != nullptr && Second->GetAttachParent() == SyntheticRoot);
        Target->SetActorLocation(FVector(100.0, 0.0, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
        if (First != nullptr && Second != nullptr)
        {
            bPassed &= TestTrue(TEXT("first top-level follows actor movement"),
                First->GetComponentLocation().Equals(FVector(110.0, 0.0, 0.0), UE_KINDA_SMALL_NUMBER));
            bPassed &= TestTrue(TEXT("second top-level follows actor movement"),
                Second->GetComponentLocation().Equals(FVector(120.0, 0.0, 0.0), UE_KINDA_SMALL_NUMBER));
        }
    }
    Target->Destroy();

    AActor* FailureTarget = GEditor->GetEditorWorldContext().World()->SpawnActor<AActor>(SpawnParameters);
    if (FailureTarget == nullptr)
    {
        AddError(TEXT("cannot spawn failed-compile target"));
        return false;
    }
    FFailOnSecondResolve FlakyResolver(CompositeTestKey(EMHResourceKind::StaticMesh, TEXT("top_level_probe")));
    FMHCompositeDocument OneNode;
    OneNode.Nodes.Add(Document.Nodes[0]);
    const FMHCompositeCompileResult Failed = MHCompileCompositeV5(
        *FailureTarget, TEXT("top_level_root"), OneNode, FlakyResolver, *Settings);
    bPassed &= TestFalse(TEXT("endpoint race fails second compile pass"), Failed.Succeeded());
    bPassed &= TestEqual(TEXT("failed compile returns no authored components"), Failed.Components.Num(), 0);
    bPassed &= TestNull(TEXT("failed compile leaves no synthetic root"), FailureTarget->GetRootComponent());
    FailureTarget->Destroy();
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeFbxPlacementParityTest,
    "Mimir.V5.Composite.FbxPlacementParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeFbxPlacementParityTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    const FString AxisProbe = FPaths::Combine(GoldenRoot, TEXT("fixtures/axis/axis_probe.fbx"));
    FMHGeometryProbeResult FbxResult;
    FString Error;
    if (!FMHFbxBackend::ImportAxisProbe(AxisProbe, FbxResult, Error))
    {
        AddError(FString::Printf(TEXT("FBX parity fixture import failed: %s"), *Error));
        return false;
    }
    const FVector ExpectedLocal(37.0, -11.0, 193.0);
    FVector Local = FVector::ZeroVector;
    double LocalDistance = TNumericLimits<double>::Max();
    for (const FVector3f& Position : FbxResult.RenderPositions)
    {
        const FVector Candidate(Position);
        const double Distance = FVector::Distance(Candidate, ExpectedLocal);
        if (Distance < LocalDistance)
        {
            LocalDistance = Distance;
            Local = Candidate;
        }
    }
    bool bPassed = TestTrue(TEXT("FBX path exposes expected local control point"), LocalDistance <= 0.1);

    if (GEditor == nullptr || GEditor->GetEditorWorldContext().World() == nullptr)
    {
        AddError(TEXT("editor world is unavailable"));
        return false;
    }
    UPackage* MeshPackage = CreatePackage(TEXT("/Game/MH/Generated/Meshes/axis_probe_parity"));
    UStaticMesh* Mesh = FindObject<UStaticMesh>(MeshPackage, TEXT("axis_probe_parity"));
    if (Mesh == nullptr)
    {
        Mesh = NewObject<UStaticMesh>(MeshPackage, TEXT("axis_probe_parity"), RF_Public | RF_Standalone);
    }
    FCompositeTestResolver Resolver;
    Resolver.AddResolved(CompositeTestKey(EMHResourceKind::StaticMesh, TEXT("axis_probe_parity")));
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHCompositeDocument Document;
    if (!MHParseCompositeV5(Utf8Composite(
            TEXT("{\"v\":5,\"nodes\":[{\"kind\":\"mesh\",\"resource\":\"axis_probe_parity\",")
            TEXT("\"transform\":{\"translation_cm\":[125,250,75],")
            TEXT("\"rotation_quat\":[-0.038135,0.189308,-0.239298,0.951549]}}]}")),
            Document, Error))
    {
        AddError(Error);
        return false;
    }
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transient;
    AActor* Target = GEditor->GetEditorWorldContext().World()->SpawnActor<AActor>(SpawnParameters);
    if (Target == nullptr)
    {
        AddError(TEXT("cannot spawn Composite parity target"));
        return false;
    }
    const FMHCompositeCompileResult Compiled = MHCompileCompositeV5(
        *Target, TEXT("axis_parity_root"), Document, Resolver, *Settings);
    bPassed &= TestTrue(TEXT("Composite compiler succeeds"), Compiled.Succeeded());
    bPassed &= TestEqual(TEXT("one compiled mesh component"), Compiled.Components.Num(), 1);
    if (Compiled.Components.Num() == 1)
    {
        const USceneComponent* Component = Cast<USceneComponent>(Compiled.Components[0]);
        bPassed &= TestTrue(TEXT("compiled node is a scene component"), Component != nullptr);
        if (Component != nullptr)
        {
            const FVector CompositeWorld = Component->GetComponentTransform().TransformPosition(Local);
            const FVector ExpectedWorld(223.3146, 219.4280, 242.7456);
            const double Distance = FVector::Distance(CompositeWorld, ExpectedWorld);
            bPassed &= TestTrue(
                FString::Printf(
                    TEXT("FBX local through Composite placement equals FBX world path: actual %s delta %.6f cm"),
                    *CompositeWorld.ToString(), Distance),
                Distance <= 0.1);
        }
    }
    Target->Destroy();
    return bPassed;
}
#endif

} // namespace UE::MimirComposite::Tests
