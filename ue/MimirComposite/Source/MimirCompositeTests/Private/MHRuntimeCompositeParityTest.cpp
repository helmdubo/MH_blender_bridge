#include "MHGoldenRoot.h"

#include "Canonical/MHCanonical.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlanReport.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHRuntimeCompositeActor.h"
#include "Composite/MHRuntimeCompositeInput.h"
#include "Components/ActorComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Containers/StringConv.h"
#include "Diagnostics/MHReaderOutputPath.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "IO/IoHash.h"
#include "MeshDescription.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite::Tests
{
namespace
{

const int32 ParitySeeds[] = {0, 1, 2, 42, 123, 1024, 2147483647};

/**
 * Every parity lane resolves the same appearance seed for a given layout seed,
 * so the S6.3 appearance arrays can be compared across lanes exactly like the
 * layout arrays already are. The derivation is the frozen migration primitive,
 * which keeps the value reproducible from the immutable seed set alone.
 */
int32 ParityAppearanceSeed(const int32 Seed)
{
    return MHDeriveAppearanceSeedFromLayoutSeed(Seed);
}


bool ParityHostGate(FAutomationTestBase& Test, bool& bRun)
{
    bRun = FParse::Param(FCommandLine::Get(), TEXT("MHS6ParityHost"));
    if (!bRun)
    {
        Test.AddInfo(TEXT("S6 external parity lane not run: requires explicit -MHS6ParityHost on the isolated host"));
        return true;
    }
    if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("MimirCompositeV5S6"))
    {
        Test.AddError(TEXT("-MHS6ParityHost is restricted to the isolated MimirCompositeV5S6 project"));
        return false;
    }
    return true;
}

TArray<uint8> ParityUtf8(const FString& Text)
{
    const FTCHARToUTF8 Utf8(*Text, Text.Len());
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return Bytes;
}

TSharedPtr<FJsonObject> ReadParityGolden(FAutomationTestBase& Test)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(Test, GoldenRoot)) return nullptr;
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(GoldenRoot, TEXT("v5/random_stream_1_vectors.json"))))
    {
        Test.AddError(TEXT("cannot read frozen S1.1 parity vectors"));
        return nullptr;
    }
    if (Bytes.Contains(static_cast<uint8>('\r')))
    {
        Test.AddError(TEXT("frozen parity vector is not LF-clean"));
        return nullptr;
    }
    TSharedPtr<FJsonValue> Value;
    const FMHCanonicalResult Parsed = MHParseJsonUtf8(Bytes, Value);
    if (!Parsed.bSuccess || !Value.IsValid() || Value->Type != EJson::Object)
    {
        Test.AddError(TEXT("invalid frozen parity vector: ") + Parsed.Error);
        return nullptr;
    }
    return Value->AsObject();
}

// Observed reports add matrices/class metadata. Every existing frozen field is
// compared exactly; additional observational fields never alter that oracle.
bool MatchesFrozenFields(const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
    const FString& Path, FString& Error)
{
    if (!Actual.IsValid() || !Expected.IsValid() || Actual->Type != Expected->Type)
    {
        Error = Path + TEXT(": missing value or different JSON type");
        return false;
    }
    bool bEqual = false;
    switch (Expected->Type)
    {
    case EJson::Object:
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Expected->AsObject()->Values)
        {
            const TSharedPtr<FJsonValue>* Found = Actual->AsObject()->Values.Find(Entry.Key);
            if (Found == nullptr || !MatchesFrozenFields(*Found, Entry.Value, Path + TEXT(".") + Entry.Key, Error))
            {
                if (Found == nullptr) Error = Path + TEXT(".") + Entry.Key + TEXT(": missing frozen field");
                return false;
            }
        }
        return true;
    case EJson::Array:
        if (Actual->AsArray().Num() != Expected->AsArray().Num()) break;
        for (int32 Index = 0; Index < Expected->AsArray().Num(); ++Index)
        {
            if (!MatchesFrozenFields(Actual->AsArray()[Index], Expected->AsArray()[Index],
                Path + FString::Printf(TEXT("[%d]"), Index), Error)) return false;
        }
        return true;
    case EJson::Number: bEqual = Actual->AsNumber() == Expected->AsNumber(); break;
    case EJson::String: bEqual = Actual->AsString() == Expected->AsString(); break;
    case EJson::Boolean: bEqual = Actual->AsBool() == Expected->AsBool(); break;
    case EJson::Null: bEqual = true; break;
    default: break;
    }
    if (!bEqual) Error = Path + TEXT(": differs from the frozen value");
    return bEqual;
}

bool CheckParityPlan(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Golden,
    const TSharedPtr<FJsonObject>& Report)
{
    const int32 Seed = static_cast<int32>(Report->GetNumberField(TEXT("seed")));
    for (const TSharedPtr<FJsonValue>& Expected : Golden->GetArrayField(TEXT("plan_vectors")))
    {
        if (Expected->AsObject()->GetNumberField(TEXT("seed")) != Seed) continue;
        FString Error;
        const bool bPassed = MatchesFrozenFields(MakeShared<FJsonValueObject>(Report), Expected,
            FString::Printf(TEXT("seed[%d]"), Seed), Error);
        if (!bPassed) Test.AddError(Error);
        return bPassed;
    }
    Test.AddError(TEXT("seed not present in frozen parity vectors"));
    return false;
}

TSharedPtr<FJsonObject> ParityWireNode(const TSharedPtr<FJsonObject>& Input)
{
    TSharedPtr<FJsonObject> Wire = MakeShared<FJsonObject>();
    Wire->Values = Input->Values;
    Wire->RemoveField(TEXT("trs"));
    Wire->SetObjectField(TEXT("transform"), Input->GetObjectField(TEXT("trs")));
    const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
    if (Input->TryGetArrayField(TEXT("children"), Children))
    {
        TArray<TSharedPtr<FJsonValue>> Converted;
        for (const TSharedPtr<FJsonValue>& Child : *Children)
            Converted.Add(MakeShared<FJsonValueObject>(ParityWireNode(Child->AsObject())));
        Wire->SetArrayField(TEXT("children"), Converted);
    }
    return Wire;
}

struct FParityFixture
{
    FAutomationTestBase& Test;
    TSharedPtr<FJsonObject> Golden;
    TArray<UObject*> Assets;
    UMHCompositeAsset* Root = nullptr;
    UWorld* World = nullptr;
    bool bEditorWorld = false;
    ULevelEditorPlaySettings* PlaySettings = nullptr;

    explicit FParityFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    ~FParityFixture()
    {
        if (World != nullptr && bEditorWorld && GEditor != nullptr)
        {
            // Only an explicitly opted-in isolated host can own this world.
            // Failure paths must also remove its placements before their assets.
            if (GEditor->PlayWorld != nullptr) GEditor->EndPlayMap();
            else if (GEditor->IsPlaySessionInProgress()) GEditor->CancelRequestPlaySession();
            if (GEditor->GetEditorWorldContext().World() == World)
                UEditorLoadingAndSavingUtils::NewBlankMap(false);
            World = nullptr;
        }
        if (PlaySettings != nullptr) PlaySettings->RemoveFromRoot();
        if (World != nullptr && !bEditorWorld) World->DestroyWorld(false);
        for (UObject* Asset : Assets)
        {
            if (IsValid(Asset))
            {
                Asset->ClearFlags(RF_Public | RF_Standalone);
                Asset->MarkAsGarbage();
            }
        }
    }

    bool Build(const bool bUseEditorWorld = false)
    {
        if (bUseEditorWorld && GEditor != nullptr && GEditor->GetEditorWorldContext().World() != nullptr &&
            GEditor->GetEditorWorldContext().World()->GetOutermost()->IsDirty())
        {
            Test.AddError(TEXT("PIE parity refuses to replace a dirty editor map"));
            return false;
        }
        CollectGarbage(RF_NoFlags);
        Golden = ReadParityGolden(Test);
        if (!Golden.IsValid()) return false;
        bEditorWorld = bUseEditorWorld;
        World = bEditorWorld ? UEditorLoadingAndSavingUtils::NewBlankMap(false)
            : UWorld::CreateWorld(EWorldType::EditorPreview, false);
        if (!Test.TestNotNull(TEXT("parity fixture world"), World)) return false;
        const TSharedPtr<FJsonObject> Input = Golden->GetObjectField(TEXT("fixture"));
        TMap<FString, FString> Hashes;
        for (const TSharedPtr<FJsonValue>& Value : Input->GetArrayField(TEXT("raw_hashes")))
        {
            const TSharedPtr<FJsonObject> Entry = Value->AsObject();
            Hashes.Add(Entry->GetStringField(TEXT("resource")), Entry->GetStringField(TEXT("hash")));
        }
        UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!Test.TestNotNull(TEXT("built stock mesh exists for cooked fixture geometry"), Cube)) return false;
        for (const TPair<FString, FString>& Hash : Hashes)
        {
            if (!Hash.Key.StartsWith(TEXT("static_mesh:"))) continue;
            const FString Name = Hash.Key.Mid(12);
            const FString PackageName = TEXT("/Game/MH/Generated/Meshes/") + Name;
            if (FindObject<UObject>(nullptr, *(PackageName + TEXT(".") + Name)) != nullptr)
            {
                Test.AddError(TEXT("parity fixture refuses an occupied generated mesh object: ") + Name);
                return false;
            }
            UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, CreatePackage(*PackageName), FName(*Name));
            if (!Test.TestNotNull(TEXT("cooked fixture mesh is duplicated"), Mesh)) return false;
            Mesh->SetFlags(RF_Public | RF_Standalone);
            Assets.Add(Mesh);
            // The S1 fixture has no material-resource edge. On cold PostLoad,
            // MeshUtilities::FixupMaterialSlotNames gives EVERY existing slot
            // a nonempty imported name (even a null/unnamed slot becomes
            // "MaterialSlot"). Remove slots, not just their imported names.
            // Keep the source descriptions consistent so rebuilding/cooking
            // cannot recover the stock cube's WorldGridMaterial association.
            Mesh->SetStaticMaterials(TArray<FStaticMaterial>());
            for (int32 Lod = 0; Lod < Mesh->GetNumSourceModels(); ++Lod)
            {
                FMeshDescription* Description = Mesh->GetMeshDescription(Lod);
                if (!Test.TestNotNull(TEXT("cooked fixture retains source mesh geometry"), Description)) return false;
                FStaticMeshAttributes Attributes(*Description);
                TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
                for (const FPolygonGroupID Group : Description->PolygonGroups().GetElementIDs()) SlotNames[Group] = NAME_None;
                Mesh->CommitMeshDescription(Lod);
            }
            TArray<FText> BuildErrors;
            Mesh->Build(true, &BuildErrors);
            FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
            for (const FText& BuildError : BuildErrors) Test.AddError(BuildError.ToString());
            if (!BuildErrors.IsEmpty() ||
                !Test.TestTrue(TEXT("rebuilt fixture has no authored material slots"), Mesh->GetStaticMaterials().IsEmpty())) return false;
            // Engine mesh sections without an assigned slot use the renderer's
            // default surface material; that display fallback is not a resource
            // in the frozen source graph and receives no invented MH receipt.
            UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
            Receipt->LogicalName = Name;
            Receipt->SourceRelativePath = Name + TEXT(".mesh.fbx");
            Receipt->SourceHash = Hash.Value;
            Receipt->ImporterVersion = MHStaticMeshImporterVersion;
            Mesh->SetAssetImportData(Receipt);
        }
        TMap<FString, FMHPlacementProfile> Profiles;
        for (const TSharedPtr<FJsonValue>& Value : Input->GetArrayField(TEXT("profiles")))
        {
            TSharedRef<FJsonObject> Wire = MakeShared<FJsonObject>();
            Wire->Values = Value->AsObject()->Values;
            const FString Name = Wire->GetStringField(TEXT("name"));
            Wire->RemoveField(TEXT("name"));
            FString Json;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
            if (!FJsonSerializer::Serialize(Wire, Writer)) return false;
            FMHPlacementProfile Profile;
            FString Error;
            if (!MHParsePlacementProfileV1(ParityUtf8(TEXT("{\"v\":1,\"kind\":\"placement_profile\",") + Json.Mid(1)), Profile, Error))
            {
                Test.AddError(Error);
                return false;
            }
            Profile.LogicalName = Name;
            Profile.SetAppliedSourceHash(Hashes.FindRef(TEXT("placement_profile:") + Name));
            Profiles.Add(Name, MoveTemp(Profile));
        }
        for (const TSharedPtr<FJsonValue>& Value : Input->GetArrayField(TEXT("composites")))
        {
            const TSharedPtr<FJsonObject> Entry = Value->AsObject();
            const FString Name = Entry->GetStringField(TEXT("name"));
            TArray<TSharedPtr<FJsonValue>> Nodes;
            for (const TSharedPtr<FJsonValue>& Node : Entry->GetArrayField(TEXT("nodes")))
                Nodes.Add(MakeShared<FJsonValueObject>(ParityWireNode(Node->AsObject())));
            FString Json;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
            if (!FJsonSerializer::Serialize(Nodes, Writer)) return false;
            FMHCompositeDocument Document;
            FString Error;
            if (!MHParseCompositeV5(ParityUtf8(TEXT("{\"v\":5,\"nodes\":") + Json + TEXT("}")), Document, Error))
            {
                Test.AddError(Error);
                return false;
            }
            TArray<FMHPlacementProfile> Inlined;
            TSet<FString> Seen;
            TFunction<void(const TArray<FMHCompositeNode>&)> Collect = [&](const TArray<FMHCompositeNode>& Items)
            {
                for (const FMHCompositeNode& Node : Items)
                {
                    if (!Node.Profile.IsEmpty() && !Seen.Contains(Node.Profile))
                    {
                        Seen.Add(Node.Profile);
                        if (const FMHPlacementProfile* Profile = Profiles.Find(Node.Profile)) Inlined.Add(*Profile);
                    }
                    Collect(Node.Children);
                }
            };
            Collect(Document.Nodes);
            const FString PackageName = TEXT("/Game/MH/Generated/Composites/") + Name;
            if (FindObject<UObject>(nullptr, *(PackageName + TEXT(".") + Name)) != nullptr)
            {
                Test.AddError(TEXT("parity fixture refuses an occupied generated composite object: ") + Name);
                return false;
            }
            UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(CreatePackage(*PackageName), FName(*Name), RF_Public | RF_Standalone);
            Assets.Add(Asset);
            Asset->LogicalName = Name;
            TArray<uint8> Bytes;
            if (!MHApplyCompositeV5(*Asset, Document, Inlined, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
            {
                Test.AddError(Error);
                return false;
            }
            // Exactly the frozen S1 synthetic-domain hashes, not hashes invented
            // for the cube's host serialization or for a new source generation.
            Asset->SourceRelativePath = Name + TEXT(".composite");
            Asset->SourceHash = Hashes.FindRef(TEXT("composite:") + Name);
            Asset->AppliedHash = MHRawPayloadHash(Bytes);
            if (Name == Input->GetStringField(TEXT("root"))) Root = Asset;
        }
        return Test.TestNotNull(TEXT("frozen root asset"), Root);
    }

    AMHCompositeActor* SpawnEditor(const int32 Seed)
    {
        AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
        if (!Test.TestNotNull(TEXT("editor parity placement"), Actor)) return nullptr;
        Actor->SetAutoSeed(false);
        Actor->SetAutoAppearanceSeed(false);
        Actor->SetSeed(Seed);
        Actor->SetAppearanceSeed(ParityAppearanceSeed(Seed));
        Actor->SetCompositeAsset(Root);
        if (!Test.TestNotNull(*Actor->GetLastPlacementError(), Actor->GetResolvedPlan())) return nullptr;
        return Actor;
    }

    bool SaveCookFixture()
    {
        // This switch authorizes only the explicit isolated parity host, never
        // user maps or generated assets in a project that happens to run tests.
        if (FPaths::GetBaseFilename(FPaths::GetProjectFilePath()) != TEXT("MimirCompositeV5S6"))
        {
            Test.AddError(TEXT("-MHS6PersistFixture requires isolated MimirCompositeV5S6 host"));
            return false;
        }
        TArray<UPackage*> Packages;
        for (UObject* Asset : Assets) Packages.AddUnique(Asset->GetOutermost());
        return Test.TestTrue(TEXT("fixture asset packages saved"), UEditorLoadingAndSavingUtils::SavePackages(Packages, false)) &&
            Test.TestTrue(TEXT("editor placements saved for real cook conversion"),
                UEditorLoadingAndSavingUtils::SaveMap(World, TEXT("/Game/MimirS6/RuntimeParity")));
    }
};

/**
 * Independent oracle: rebuild every appearance value straight from the stream
 * primitive instead of trusting the resolver that produced the report. Also
 * pins the channel count, the per-channel order, and the PlacementSignature
 * composition rule.
 */
bool CheckAppearanceContract(FAutomationTestBase& Test, const FMHResolvedCompositePlan& Plan, const int32 Seed)
{
    bool bPassed = Test.TestEqual(TEXT("lane resolves the agreed appearance seed"),
        Plan.Appearance.AppearanceSeed, ParityAppearanceSeed(Seed));
    if (!Test.TestEqual(TEXT("exactly MH_APPEARANCE_CHANNELS draws per leaf"),
            Plan.Appearance.Draws.Num(), Plan.Leaves.Num() * MH_APPEARANCE_CHANNELS)) return false;
    for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
        FMHRandomStream1 Stream = MHMakeNodeRandomStream(Plan.Appearance.AppearanceSeed, Leaf.AppearanceBoundaryPath);
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
        {
            const FMHResolvedCompositeAppearanceDraw& Draw = Plan.Appearance.Draws[Index * MH_APPEARANCE_CHANNELS + Channel];
            const uint32 Expected = Stream.NextU32();
            bPassed &= Test.TestEqual(TEXT("appearance draw belongs to its leaf"), Draw.NodePath, Leaf.Origin);
            bPassed &= Test.TestEqual(TEXT("appearance draw records its boundary"), Draw.BoundaryPath, Leaf.AppearanceBoundaryPath);
            bPassed &= Test.TestEqual(TEXT("channels are drawn in order without gaps"), Draw.Channel, Channel);
            bPassed &= Test.TestEqual(TEXT("RawU32 matches the independent stream"), Draw.RawU32, Expected);
            bPassed &= Test.TestEqual(TEXT("leaf channel is derived from RawU32"),
                Leaf.AppearanceChannels[Channel], static_cast<float>(static_cast<double>(Expected) / 4294967296.0));
        }
    }
    const FString Concatenated = Plan.ResolvedSignature + Plan.Appearance.AppearanceSignature;
    const FTCHARToUTF8 Utf8(*Concatenated, Concatenated.Len());
    const FIoHash Hash = FIoHash::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()));
    bPassed &= Test.TestEqual(TEXT("PlacementSignature hashes both signatures without a separator"),
        Plan.PlacementSignature, TEXT("blake3-160:") + LexToString(Hash).ToLower());
    return bPassed;
}

bool RecordEditor(FAutomationTestBase& Test, const AMHCompositeActor& Actor,
    const TSharedPtr<FJsonObject>& Golden, TArray<TSharedPtr<FJsonValue>>& Reports)
{
    const FMHResolvedCompositePlan* Plan = Actor.GetResolvedPlan();
    if (!Test.TestNotNull(TEXT("editor placement has a plan"), Plan)) return false;
    const TArray<FMHCompositeLeafMaterialization>& Materializations =
        Actor.GetLeafMaterializations();
    if (!Test.TestEqual(TEXT("editor materialization stays plan-aligned"),
        Materializations.Num(), Plan->Leaves.Num())) return false;
    TArray<TObjectPtr<USceneComponent>> Components;
    TArray<TObjectPtr<UInstancedStaticMeshComponent>> InstanceTransformProxies;
    for (int32 LeafIndex = 0; LeafIndex < Plan->Leaves.Num(); ++LeafIndex)
    {
        const FMHCompositeLeafMaterialization& Materialization = Materializations[LeafIndex];
        if (!Materialization.IsInstanced())
        {
            Components.Add(Materialization.Component);
            continue;
        }

        const UInstancedStaticMeshComponent* Bucket =
            Cast<UInstancedStaticMeshComponent>(Materialization.Component.Get());
        FTransform InstanceWorld;
        if (!Test.TestNotNull(TEXT("editor parity ISM bucket"), Bucket) ||
            !Test.TestTrue(TEXT("editor parity instance transform exists"),
                Bucket->GetInstanceTransform(Materialization.InstanceIndex, InstanceWorld, true)))
        {
            return false;
        }
        // MHBuildCompositePlanReport intentionally remains runtime-only and
        // component-shaped. Feed it a test-only proxy carrying the exact ISM
        // instance world matrix; no production transport or signature changes.
        UInstancedStaticMeshComponent* Proxy =
            NewObject<UInstancedStaticMeshComponent>(GetTransientPackage());
        Proxy->SetWorldTransform(InstanceWorld);
        InstanceTransformProxies.Add(Proxy);
        Components.Add(Proxy);
    }
    TSharedPtr<FJsonObject> Report;
    FString Error;
    if (!MHBuildCompositePlanReport(*Plan, Components, Report, Error))
    {
        Test.AddError(Error);
        return false;
    }
    Reports.Add(MakeShared<FJsonValueObject>(Report));
    return CheckParityPlan(Test, Golden, Report) & CheckAppearanceContract(Test, *Plan, Plan->Seed);
}

bool RecordRuntime(FAutomationTestBase& Test, const AMHRuntimeCompositeActor& Actor,
    const TSharedPtr<FJsonObject>& Golden, TArray<TSharedPtr<FJsonValue>>& Reports)
{
    const FMHResolvedCompositePlan* Plan = Actor.GetResolvedPlan();
    if (!Test.TestNotNull(*Actor.GetLastRuntimeError(), Plan)) return false;
    TSharedPtr<FJsonObject> Report;
    FString Error;
    if (!MHBuildCompositePlanReport(*Plan, Actor.GetMaterializedComponents(), Report, Error))
    {
        Test.AddError(Error);
        return false;
    }
    Reports.Add(MakeShared<FJsonValueObject>(Report));
    return CheckParityPlan(Test, Golden, Report) & CheckAppearanceContract(Test, *Plan, Plan->Seed);
}

bool WriteReports(FAutomationTestBase& Test, const FString& Lane, const FString& WorldType,
    const TArray<TSharedPtr<FJsonValue>>& Reports)
{
    FString Error;
    const FString SourceRoot = GetDefault<UMHCompositeSettings>()->GetSourceRootPath();
    FString CheckedPath;
    if (!SourceRoot.IsEmpty() && !MHResolveReaderOutputPath(SourceRoot, MHCompositeParityReportPath(Lane), CheckedPath, Error))
    {
        Test.AddError(Error);
        return false;
    }
    if (!MHWriteCompositeParityReport(Lane, WorldType, false, Reports, Error))
    {
        Test.AddError(Error);
        return false;
    }
    Test.AddInfo(TEXT("S6 distinct lane report: ") + MHCompositeParityReportPath(Lane));
    return true;
}

class FParityPIECommand final : public IAutomationLatentCommand
{
public:
    explicit FParityPIECommand(TSharedPtr<FParityFixture> InFixture)
        : Fixture(MoveTemp(InFixture)), StartedAt(FPlatformTime::Seconds()) {}

    virtual bool Update() override
    {
        if (!bEnding)
        {
            UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld : nullptr;
            if (PlayWorld != nullptr && PlayWorld->WorldType == EWorldType::PIE && PlayWorld->HasBegunPlay())
            {
                TMap<int32, AMHRuntimeCompositeActor*> Actors;
                for (TActorIterator<AMHRuntimeCompositeActor> It(PlayWorld); It; ++It)
                {
                    if (!Fixture->Test.TestTrue(TEXT("runtime PIE actor began play"), It->HasActorBegunPlay())) break;
                    if (Actors.Contains(It->GetSeed())) Fixture->Test.AddError(TEXT("duplicate seed in runtime PIE lane"));
                    Actors.Add(It->GetSeed(), *It);
                }
                bool bPassed = Fixture->Test.TestEqual(TEXT("PIE bridge produces all seven runtime placements"), Actors.Num(), 7);
                TArray<TSharedPtr<FJsonValue>> Reports;
                for (const int32 Seed : ParitySeeds)
                {
                    AMHRuntimeCompositeActor* const* Actor = Actors.Find(Seed);
                    if (Actor == nullptr)
                    {
                        Fixture->Test.AddError(FString::Printf(TEXT("runtime PIE missing seed %d"), Seed));
                        bPassed = false;
                    }
                    else bPassed &= RecordRuntime(Fixture->Test, **Actor, Fixture->Golden, Reports);
                }
                if (bPassed) WriteReports(Fixture->Test, TEXT("pie"), TEXT("PIE"), Reports);
                GEditor->RequestEndPlayMap();
                bEnding = true;
                StartedAt = FPlatformTime::Seconds();
                return false;
            }
            if (FPlatformTime::Seconds() - StartedAt > 60.0)
            {
                Fixture->Test.AddError(TEXT("real PIE failed to reach BeginPlay within 60 seconds"));
                if (GEditor != nullptr)
                {
                    if (GEditor->PlayWorld != nullptr) GEditor->RequestEndPlayMap();
                    else GEditor->CancelRequestPlaySession();
                }
                bEnding = true;
                StartedAt = FPlatformTime::Seconds();
            }
            return false;
        }
        if (GEditor != nullptr && (GEditor->PlayWorld != nullptr || GEditor->IsPlaySessionInProgress()))
        {
            if (FPlatformTime::Seconds() - StartedAt <= 60.0) return false;
            Fixture->Test.AddError(TEXT("PIE shutdown did not complete within 60 seconds"));
            return true;
        }
        UEditorLoadingAndSavingUtils::NewBlankMap(false);
        Fixture->World = nullptr;
        Fixture.Reset();
        return true;
    }

private:
    TSharedPtr<FParityFixture> Fixture;
    double StartedAt;
    bool bEnding = false;
};

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHRuntimeSerializedParityTest,
    "Mimir.V5.Runtime.Parity.SerializedAutomation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRuntimeSerializedParityTest::RunTest(const FString& Parameters)
{
    bool bRun = false;
    if (!ParityHostGate(*this, bRun)) return false;
    if (!bRun) return true;
    FParityFixture Fixture(*this);
    if (!Fixture.Build()) return false;
    FMHRandomSourceGraph Graph;
    TSet<FMHResourceKey> Dependencies;
    FString Error;
    FMHRuntimeCompositeInput Input;
    if (!MHBuildAppliedCompositeGraph(*Fixture.Root, *GetDefault<UMHCompositeSettings>(), Graph, Dependencies, Error) ||
        !MHEncodeRuntimeCompositeGraph(Graph, Input.GraphBytes, Error))
    {
        AddError(Error);
        return false;
    }
    TArray<FString> BindingKeys;
    if (!MHCollectRuntimeCompositeBindingKeys(Graph, BindingKeys, Error)) { AddError(Error); return false; }
    for (const FString& Key : BindingKeys)
    {
        FMHRuntimeCompositeBinding& Binding = Input.Bindings.AddDefaulted_GetRef();
        Binding.ResourceKey = Key;
        if (!Key.StartsWith(TEXT("static_mesh:"))) { AddError(TEXT("unexpected frozen fixture binding")); return false; }
        const FString Name = Key.Mid(12);
        Binding.Object = FindObject<UStaticMesh>(nullptr, *(TEXT("/Game/MH/Generated/Meshes/") + Name + TEXT(".") + Name));
    }
    FMHRandomSourceGraph Decoded;
    TArray<uint8> RoundTrip;
    if (!MHDecodeRuntimeCompositeGraph(Input.GraphBytes, Decoded, Error) ||
        !MHEncodeRuntimeCompositeGraph(Decoded, RoundTrip, Error)) { AddError(Error); return false; }
    if (!TestTrue(TEXT("runtime input transport round-trips byte-identically"), Input.GraphBytes == RoundTrip)) return false;
    TArray<TSharedPtr<FJsonValue>> Reports;
    for (const int32 Seed : ParitySeeds)
    {
        AMHRuntimeCompositeActor* Actor = Fixture.World->SpawnActor<AMHRuntimeCompositeActor>();
        if (!TestNotNull(TEXT("runtime materializer placement"), Actor) || !Actor->Configure(Input, Seed, ParityAppearanceSeed(Seed), Error))
        {
            AddError(Error);
            return false;
        }
        if (!RecordRuntime(*this, *Actor, Fixture.Golden, Reports)) return false;
    }
    return WriteReports(*this, TEXT("automation"), TEXT("EditorPreview"), Reports);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHRuntimeEditorPreviewParityTest,
    "Mimir.V5.Runtime.Parity.EditorPreview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRuntimeEditorPreviewParityTest::RunTest(const FString& Parameters)
{
    bool bRun = false;
    if (!ParityHostGate(*this, bRun)) return false;
    if (!bRun) return true;
    FParityFixture Fixture(*this);
    if (!Fixture.Build()) return false;
    TArray<TSharedPtr<FJsonValue>> Reports;
    for (const int32 Seed : ParitySeeds)
    {
        AMHCompositeActor* Actor = Fixture.SpawnEditor(Seed);
        if (Actor == nullptr || !RecordEditor(*this, *Actor, Fixture.Golden, Reports)) return false;
    }
    return WriteReports(*this, TEXT("editor_preview"), TEXT("EditorPreview"), Reports);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHRuntimePIEParityTest,
    "Mimir.V5.Runtime.Parity.PIE",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHRuntimePIEParityTest::RunTest(const FString& Parameters)
{
    bool bRun = false;
    if (!ParityHostGate(*this, bRun)) return false;
    if (!bRun) return true;
    if (!TestNotNull(TEXT("real editor engine exists for PIE"), GEditor) || GEditor->IsPlaySessionInProgress())
    {
        AddError(TEXT("PIE parity requires an idle real editor session"));
        return false;
    }
    TSharedPtr<FParityFixture> Fixture = MakeShared<FParityFixture>(*this);
    if (!Fixture->Build(true)) return false;
    for (const int32 Seed : ParitySeeds) if (Fixture->SpawnEditor(Seed) == nullptr) return false;
    if (FParse::Param(FCommandLine::Get(), TEXT("MHS6PersistFixture")) && !Fixture->SaveCookFixture()) return false;
    Fixture->PlaySettings = DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
    Fixture->PlaySettings->AddToRoot();
    Fixture->PlaySettings->SetPlayNetMode(PIE_Standalone);
    Fixture->PlaySettings->SetPlayNumberOfClients(1);
    Fixture->PlaySettings->SetRunUnderOneProcess(true);
    Fixture->PlaySettings->bLaunchSeparateServer = false;
    Fixture->PlaySettings->EnableGameSound = false;
    Fixture->PlaySettings->GameGetsMouseControl = false;
    Fixture->PlaySettings->NewWindowWidth = 320;
    Fixture->PlaySettings->NewWindowHeight = 240;
    FRequestPlaySessionParams Request;
    Request.EditorPlaySettings = Fixture->PlaySettings;
    Request.SessionDestination = EPlaySessionDestinationType::InProcess;
    Request.WorldType = EPlaySessionWorldType::PlayInEditor;
    Request.bAllowOnlineSubsystem = false;
    Request.StartLocation = FVector::ZeroVector;
    GEditor->RequestPlaySession(Request);
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FParityPIECommand>(Fixture));
    return true;
}

} // namespace UE::MimirComposite::Tests
