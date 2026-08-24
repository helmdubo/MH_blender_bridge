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

TArray<uint8> CompositeJsonObjectBytes(const TSharedRef<FJsonObject>& Object)
{
    FString Json;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    FJsonSerializer::Serialize(Object, Writer);
    return Utf8Composite(Json);
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

FMHResourceKey Key(const EMHResourceKind Kind, const FString& Name)
{
    FMHResourceKey Result;
    Result.Kind = Kind;
    Result.LogicalName = Name;
    return Result;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeGoldenVectorsTest,
    "Mimir.V4.Composite.CanonicalGoldenVectors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeGoldenVectorsTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    FString FixtureText;
    const FString FixturePath = FPaths::Combine(GoldenRoot, TEXT("composite_v4_vectors.json"));
    if (!FFileHelper::LoadFileToString(FixtureText, *FixturePath))
    {
        AddError(FString::Printf(TEXT("cannot read %s"), *FixturePath));
        return false;
    }
    TSharedPtr<FJsonObject> Fixture;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FixtureText), Fixture) || !Fixture.IsValid())
    {
        AddError(TEXT("composite_v4_vectors.json is not valid JSON"));
        return false;
    }
    FString Schema;
    bool bPassed = TestTrue(TEXT("fixture schema"),
        Fixture->TryGetStringField(TEXT("schema"), Schema) && Schema == TEXT("mh.composite_v4_vectors"));
    const TArray<TSharedPtr<FJsonValue>>* Vectors = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("vectors"), Vectors) || Vectors == nullptr) return false;
    for (const TSharedPtr<FJsonValue>& Value : *Vectors)
    {
        const TSharedPtr<FJsonObject> Vector = Value->AsObject();
        FString Name;
        FString Expected;
        const TSharedPtr<FJsonObject>* Document = nullptr;
        if (!Vector.IsValid() || !Vector->TryGetStringField(TEXT("name"), Name) ||
            !Vector->TryGetStringField(TEXT("canonical_utf8"), Expected) ||
            !Vector->TryGetObjectField(TEXT("value"), Document) || Document == nullptr)
        {
            AddError(TEXT("malformed composite golden vector"));
            return false;
        }
        FMHCompositeDocument Parsed;
        TArray<uint8> Actual;
        FString Error;
        bPassed &= TestTrue(*FString::Printf(TEXT("%s parses"), *Name),
            MHParseCompositeV4(CompositeJsonObjectBytes((*Document).ToSharedRef()), Parsed, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("%s writes"), *Name),
            MHWriteCanonicalCompositeV4(Parsed, Actual, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("%s exact bytes"), *Name), Actual == Utf8Composite(Expected));
    }

    const TArray<TSharedPtr<FJsonValue>>* Negative = nullptr;
    if (!Fixture->TryGetArrayField(TEXT("negative_vectors"), Negative) || Negative == nullptr) return false;
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
            MHParseCompositeV4(Utf8Composite(Json), Parsed, Error));
        bPassed &= TestTrue(*FString::Printf(TEXT("%s error code"), *Name), StartsWithCode(Error, Code));
    }
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeApplyExtractTest,
    "Mimir.V4.Composite.ApplyExtractAndLocalEdit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeApplyExtractTest::RunTest(const FString& Parameters)
{
    const TArray<uint8> Source = Utf8Composite(
        TEXT("{\"nodes\":[{\"kind\":\"group\",\"name\":\"root\",")
        TEXT("\"transform\":{\"translation_cm\":[100,20,3]},\"children\":[")
        TEXT("{\"kind\":\"actor\",\"resource\":\"light\",\"transform\":{\"translation_cm\":[125,20,3]}}]}]}"));
    FMHCompositeDocument Parsed;
    FString Error;
    bool bPassed = TestTrue(TEXT("source parses"), MHParseCompositeV4(Source, Parsed, Error));
    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>();
    bPassed &= TestTrue(TEXT("apply succeeds"), MHApplyCompositeV4(*Asset, Parsed, Error));
    FMHCompositeDocument Extracted;
    TArray<uint8> Canonical;
    TArray<uint8> ExtractedBytes;
    bPassed &= TestTrue(TEXT("extract succeeds"), MHExtractCompositeV4(*Asset, Extracted, Error));
    bPassed &= TestTrue(TEXT("source canonical writes"), MHWriteCanonicalCompositeV4(Parsed, Canonical, Error));
    bPassed &= TestTrue(TEXT("extract canonical writes"), MHWriteCanonicalCompositeV4(Extracted, ExtractedBytes, Error));
    bPassed &= TestTrue(TEXT("apply/extract exact canonical bytes"), ExtractedBytes == Canonical);
    Asset->AppliedHash = MHRawPayloadHash(Canonical);
    FString Warning;
    bPassed &= TestFalse(TEXT("fresh receipt is not locally modified"),
        MHDetectManagedCompositeLocalModification(*Asset, Warning));
    Asset->Nodes[1].Transform.SetTranslation(FVector(126.0, 20.0, 3.0));
    bPassed &= TestTrue(TEXT("changed applied node is locally modified"),
        MHDetectManagedCompositeLocalModification(*Asset, Warning));
    bPassed &= TestTrue(TEXT("local modification warning is machine-coded"),
        Warning.StartsWith(TEXT("MH_W_MANAGED_ASSET_LOCALLY_MODIFIED:")));

    FMHCompositeDocument NonFiniteWriter;
    FMHCompositeNode& InvalidNode = NonFiniteWriter.Nodes.AddDefaulted_GetRef();
    InvalidNode.Kind = EMHCompositeNodeKind::Group;
    InvalidNode.Transform.RotationQuat = FQuat(
        std::numeric_limits<double>::infinity(), 0.0, 0.0, 1.0);
    TArray<uint8> InvalidBytes;
    bPassed &= TestFalse(TEXT("writer rejects non-finite quaternion"),
        MHWriteCanonicalCompositeV4(NonFiniteWriter, InvalidBytes, Error));
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
        MHExtractCompositeV4(*InvalidAsset, InvalidExtract, Error));
    bPassed &= TestTrue(TEXT("extract non-finite quaternion code"),
        StartsWithCode(Error, TEXT("MH_E_NAN_INF_VALUE")));
    const FMHCompositeOperationResult InvalidPublish = MHPublishCompositeV4(
        *InvalidAsset, FPaths::ProjectSavedDir());
    bPassed &= TestFalse(TEXT("publish rejects non-finite extracted quaternion"),
        InvalidPublish.Succeeded());
    bPassed &= TestTrue(TEXT("publish preserves non-finite quaternion code"),
        StartsWithCode(InvalidPublish.Error, TEXT("MH_E_NAN_INF_VALUE")));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeClosureTest,
    "Mimir.V4.Composite.ClosureCycleAndUnresolved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeClosureTest::RunTest(const FString& Parameters)
{
    const FString TempRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests/closure"));
    IFileManager::Get().MakeDirectory(*TempRoot, true);
    const FString NestedPath = FPaths::Combine(TempRoot, TEXT("nested.composite"));
    const TArray<uint8> NestedBytes = Utf8Composite(
        TEXT("{\"nodes\":[{\"kind\":\"composite\",\"resource\":\"root\"}]}"));
    FFileHelper::SaveArrayToFile(NestedBytes, *NestedPath);

    FMHCompositeDocument Root;
    FString Error;
    MHParseCompositeV4(
        Utf8Composite(TEXT("{\"nodes\":[{\"kind\":\"composite\",\"resource\":\"nested\"}]}")),
        Root,
        Error);
    FCompositeTestResolver Resolver;
    Resolver.AddResolved(Key(EMHResourceKind::Composite, TEXT("nested")), NestedPath, MHRawPayloadHash(NestedBytes));
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    bool bPassed = TestFalse(TEXT("ancestor cycle rejected"),
        MHValidateCompositeClosureV4(TEXT("root"), Root, Resolver, *Settings, Error));
    bPassed &= TestTrue(TEXT("cycle code"), StartsWithCode(Error, TEXT("MH_E_COMPOSITE_CYCLE")));

    FMHCompositeDocument MissingActor;
    MHParseCompositeV4(
        Utf8Composite(TEXT("{\"nodes\":[{\"kind\":\"actor\",\"resource\":\"missing\"}]}")),
        MissingActor,
        Error);
    bPassed &= TestFalse(TEXT("unresolved actor rejected"),
        MHValidateCompositeClosureV4(TEXT("root"), MissingActor, Resolver, *Settings, Error));
    bPassed &= TestTrue(TEXT("unresolved code"),
        StartsWithCode(Error, TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE")));
    IFileManager::Get().Delete(*NestedPath, false, true, true);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeImportPublishReceiptTest,
    "Mimir.V4.Composite.ImportPublishReceipts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeImportPublishReceiptTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests/import_publish"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString SourcePath = FPaths::Combine(SourceRoot, TEXT("ue_s3_roundtrip.composite"));
    const TArray<uint8> RawBytes = Utf8Composite(TEXT("{ \"nodes\" : [] }\r\n"));
    FFileHelper::SaveArrayToFile(RawBytes, *SourcePath);

    FMHSourceAnalysisEntry Entry;
    Entry.Key = Key(EMHResourceKind::Composite, TEXT("ue_s3_roundtrip"));
    Entry.PayloadPath = SourcePath;
    Entry.SourcePath = TEXT("ue_s3_roundtrip.composite");
    Entry.RawHash = MHRawPayloadHash(RawBytes);
    Entry.Change = EMHSourceChange::Create;
    FCompositeTestResolver Resolver;
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHCompositeOperationResult Imported = MHImportCompositeV4(
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
    const TArray<uint8> CanonicalEmpty = Utf8Composite(TEXT("{\n  \"nodes\": []\n}\n"));
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
    MHParseCompositeV4(Utf8Composite(
        TEXT("{\"nodes\":[{\"kind\":\"group\",\"name\":\"published\"}]}")), Edited, Error);
    bPassed &= TestTrue(TEXT("local source-shaped edit applies"),
        MHApplyCompositeV4(*Imported.Asset, Edited, Error));
    FMHCompositeOperationResult Published = MHPublishCompositeV4(
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
    FMHCompositeOperationResult Adopted = MHPublishCompositeV4(
        *Imported.Asset, SourceRoot, &Adopt);
    bPassed &= TestTrue(TEXT("unmanaged Adopt publish succeeds"), Adopted.Succeeded());
    if (!Adopted.Succeeded()) AddError(Adopted.Error);
    bPassed &= TestTrue(TEXT("Adopt writes exact target"),
        FPaths::FileExists(FPaths::Combine(AdoptFolder, TEXT("ue_s3_adopted.composite"))));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSourceRaceNoGhostTest,
    "Mimir.V4.Composite.SourceRaceNoGhost",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSourceRaceNoGhostTest::RunTest(const FString& Parameters)
{
    const FString SourceRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirCompositeTests/race"));
    IFileManager::Get().MakeDirectory(*SourceRoot, true);
    const FString Token = FString::Printf(TEXT("ue_s3_race_%08x"), FPlatformTime::Cycles());
    const FString SourcePath = FPaths::Combine(SourceRoot, Token + TEXT(".composite"));
    const TArray<uint8> Initial = Utf8Composite(TEXT("{\"nodes\":[]}"));
    FFileHelper::SaveArrayToFile(Initial, *SourcePath);
    FMHSourceAnalysisEntry Entry;
    Entry.Key = Key(EMHResourceKind::Composite, Token);
    Entry.PayloadPath = SourcePath;
    Entry.SourcePath = Token + TEXT(".composite");
    Entry.RawHash = MHRawPayloadHash(Initial);
    Entry.Change = EMHSourceChange::Create;
    FCompositeTestResolver Resolver;
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    MHSetBeforeCompositeSourceCommitTestHook([SourcePath]()
    {
        FFileHelper::SaveStringToFile(TEXT("{\"nodes\":[]}\n"), *SourcePath);
    });
    const FMHCompositeOperationResult Result = MHImportCompositeV4(
        Entry, Resolver, SourceRoot, *Settings);
    bool bPassed = TestFalse(TEXT("source race blocks import"), Result.Succeeded());
    bPassed &= TestTrue(TEXT("source race code"),
        StartsWithCode(Result.Error, TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED")));
    const FString ObjectPath = FString::Printf(
        TEXT("/Game/MH/Generated/Composites/%s.%s"), *Token, *Token);
    bPassed &= TestNull(TEXT("source race creates no generated asset"),
        StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath));
    return bPassed;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeCompilerWorldTransformTest,
    "Mimir.V4.Composite.CompilerPreservesSourceWorldTransforms",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeCompilerWorldTransformTest::RunTest(const FString& Parameters)
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
    Resolver.AddResolved(Key(EMHResourceKind::StaticMesh, TEXT("world_probe")));
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHCompositeDocument Document;
    FString Error;
    if (!MHParseCompositeV4(Utf8Composite(
            TEXT("{\"nodes\":[{\"kind\":\"group\",\"name\":\"parent\",\"transform\":{")
            TEXT("\"translation_cm\":[100,0,0]},\"children\":[{\"kind\":\"mesh\",")
            TEXT("\"resource\":\"world_probe\",\"transform\":{\"translation_cm\":[125,0,0]}}]}]}")),
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
    const FMHCompositeCompileResult Compiled = MHCompileCompositeV4(
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
            bPassed &= TestTrue(TEXT("group source world"),
                Group->GetComponentLocation().Equals(FVector(100, 0, 0), UE_KINDA_SMALL_NUMBER));
            bPassed &= TestTrue(TEXT("child source world not double-applied"),
                Child->GetComponentLocation().Equals(FVector(125, 0, 0), UE_KINDA_SMALL_NUMBER));
            bPassed &= TestTrue(TEXT("authored structural parent retained"), Child->GetAttachParent() == Group);
        }
    }
    Target->Destroy();

    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeCompilerTopLevelAttachmentTest,
    "Mimir.V4.Composite.CompilerTopLevelAttachment",
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
    Resolver.AddResolved(Key(EMHResourceKind::StaticMesh, TEXT("top_level_probe")));
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHCompositeDocument Document;
    FString Error;
    if (!MHParseCompositeV4(Utf8Composite(
            TEXT("{\"nodes\":[{\"kind\":\"mesh\",\"resource\":\"top_level_probe\",")
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
    const FMHCompositeCompileResult Compiled = MHCompileCompositeV4(
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
    FFailOnSecondResolve FlakyResolver(Key(EMHResourceKind::StaticMesh, TEXT("top_level_probe")));
    FMHCompositeDocument OneNode;
    OneNode.Nodes.Add(Document.Nodes[0]);
    const FMHCompositeCompileResult Failed = MHCompileCompositeV4(
        *FailureTarget, TEXT("top_level_root"), OneNode, FlakyResolver, *Settings);
    bPassed &= TestFalse(TEXT("endpoint race fails second compile pass"), Failed.Succeeded());
    bPassed &= TestEqual(TEXT("failed compile returns no authored components"), Failed.Components.Num(), 0);
    bPassed &= TestNull(TEXT("failed compile leaves no synthetic root"), FailureTarget->GetRootComponent());
    FailureTarget->Destroy();
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeFbxPlacementParityTest,
    "Mimir.V4.Composite.FbxPlacementParity",
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
    Resolver.AddResolved(Key(EMHResourceKind::StaticMesh, TEXT("axis_probe_parity")));
    UMHCompositeSettings* Settings = NewObject<UMHCompositeSettings>();
    FMHCompositeDocument Document;
    if (!MHParseCompositeV4(Utf8Composite(
            TEXT("{\"nodes\":[{\"kind\":\"mesh\",\"resource\":\"axis_probe_parity\",")
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
    const FMHCompositeCompileResult Compiled = MHCompileCompositeV4(
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
