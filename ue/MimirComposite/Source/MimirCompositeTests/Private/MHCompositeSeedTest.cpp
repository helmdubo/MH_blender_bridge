#include "MHGoldenRoot.h"

#include "Canonical/MHCanonical.h"
#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Components/SceneComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/StringConv.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Math/Transform.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Random/MHRandomStream.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite::Tests
{
namespace
{

FMHCompositeNode SeedTestMeshNode(const FString& Name, const double X = 0.0)
{
    FMHCompositeNode Node;
    Node.Kind = EMHCompositeNodeKind::Mesh;
    Node.Resource = Name;
    Node.Transform.TranslationCm.X = X;
    return Node;
}

FMHCompositeOption SeedTestOption(const EMHCompositeOptionKind Kind, const FString& Name, const float Weight = 1.0f)
{
    FMHCompositeOption Option;
    Option.Kind = Kind;
    Option.Resource = Name;
    Option.Weight = Weight;
    return Option;
}

FMHResourceKey SeedTestKey(const EMHResourceKind Kind, const FString& Name)
{
    FMHResourceKey Key;
    Key.Kind = Kind;
    Key.LogicalName = Name;
    return Key;
}

FString SeedTestSignature(const AMHCompositeActor& Actor)
{
    const FStrProperty* Property = FindFProperty<FStrProperty>(AMHCompositeActor::StaticClass(), TEXT("ResolvedSignature"));
    return Property != nullptr ? Property->GetPropertyValue_InContainer(&Actor) : FString();
}

USceneComponent* SeedTestLeafComponent(const AMHCompositeActor& Actor, const int32 Index)
{
    return Actor.GetLeafMaterializations().IsValidIndex(Index)
        ? Actor.GetLeafMaterializations()[Index].Component.Get() : nullptr;
}

bool SeedTestLeafTransform(
    const AMHCompositeActor& Actor, const int32 Index, FTransform& OutTransform)
{
    if (!Actor.GetLeafMaterializations().IsValidIndex(Index)) return false;
    const FMHCompositeLeafMaterialization& Row = Actor.GetLeafMaterializations()[Index];
    if (!IsValid(Row.Component)) return false;
    if (const UInstancedStaticMeshComponent* Bucket =
            Cast<UInstancedStaticMeshComponent>(Row.Component))
        return Bucket->GetInstanceTransform(Row.InstanceIndex, OutTransform, true);
    OutTransform = Row.Component->GetComponentTransform();
    return true;
}

bool SeedTestTraceEqual(const FMHResolvedCompositePlan& A, const FMHResolvedCompositePlan& B)
{
    if (A.Decisions.Num() != B.Decisions.Num() || A.Draws.Num() != B.Draws.Num()) return false;
    for (int32 Index = 0; Index < A.Decisions.Num(); ++Index)
    {
        const FMHResolvedCompositeDecision& Left = A.Decisions[Index];
        const FMHResolvedCompositeDecision& Right = B.Decisions[Index];
        if (Left.NodePath != Right.NodePath || Left.OptionIndex != Right.OptionIndex || Left.Weights != Right.Weights ||
            Left.Total != Right.Total || Left.RawU32 != Right.RawU32 || Left.Unit != Right.Unit || Left.Target != Right.Target) return false;
    }
    for (int32 Index = 0; Index < A.Draws.Num(); ++Index)
    {
        const FMHResolvedCompositeDraw& Left = A.Draws[Index];
        const FMHResolvedCompositeDraw& Right = B.Draws[Index];
        if (Left.NodePath != Right.NodePath || Left.Role != Right.Role || Left.RawU32 != Right.RawU32 ||
            Left.Unit != Right.Unit || Left.Sample != Right.Sample) return false;
    }
    return true;
}

// R2b-2: the preview plane publishes no signature, so "same result" is asserted
// as shadow parity of decisions/draws/nodes/leaves/appearance instead.
bool SeedTestShadowParity(
    FAutomationTestBase& Test, const TCHAR* What,
    const FMHResolvedCompositePlan& Reference, const FMHResolvedCompositePlan& Preview)
{
    TArray<FString> Mismatches;
    const bool bParity = MHCompareRecipeShadowParity(Reference, Preview, Mismatches);
    FString Label(What);
    if (!Mismatches.IsEmpty()) Label += TEXT(": ") + FString::Join(Mismatches, TEXT("; "));
    return Test.TestTrue(Label, bParity && Mismatches.IsEmpty());
}

struct FSeedTestComponentSnapshot
{
    TArray<TObjectPtr<UActorComponent>> Components;
    TArray<FTransform> Transforms;

    explicit FSeedTestComponentSnapshot(const AMHCompositeActor& Actor)
        : Components(Actor.GetDerivedComponents())
    {
        for (const UActorComponent* Component : Components)
        {
            const USceneComponent* Scene = Cast<USceneComponent>(Component);
            Transforms.Add(Scene != nullptr ? Scene->GetComponentTransform() : FTransform::Identity);
        }
    }

    bool Check(FAutomationTestBase& Test, const AMHCompositeActor& Actor, const bool bCheckTransforms = true) const
    {
        if (!Test.TestEqual(TEXT("component count preserved"), Actor.GetDerivedComponents().Num(), Components.Num())) return false;
        bool bPassed = true;
        for (int32 Index = 0; Index < Components.Num(); ++Index)
        {
            bPassed &= Test.TestEqual(*FString::Printf(TEXT("component %d pointer preserved"), Index),
                Actor.GetDerivedComponents()[Index].Get(), Components[Index].Get());
            const USceneComponent* Scene = Cast<USceneComponent>(Actor.GetDerivedComponents()[Index]);
            if (bCheckTransforms && Scene != nullptr)
                bPassed &= Test.TestTrue(*FString::Printf(TEXT("component %d transform preserved"), Index),
                    Scene->GetComponentTransform().Equals(Transforms[Index], 0.0));
        }
        return bPassed;
    }
};

struct FSeedTestFixture
{
    FAutomationTestBase& Test;
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UWorld* World = nullptr;
    TArray<UObject*> Assets;
    TArray<AMHCompositeActor*> DuplicatedActors;

    explicit FSeedTestFixture(FAutomationTestBase& InTest) : Test(InTest)
    {
        World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
        Test.TestNotNull(TEXT("seed test preview world exists"), World);
    }

    ~FSeedTestFixture()
    {
        for (AMHCompositeActor* Actor : DuplicatedActors) if (IsValid(Actor)) Actor->Destroy();
        if (World != nullptr) World->DestroyWorld(false);
        for (UObject* Asset : Assets)
        {
            if (IsValid(Asset))
            {
                Asset->ClearFlags(RF_Public | RF_Standalone);
                Asset->MarkAsGarbage();
            }
        }
    }

    FString Name(const TCHAR* Stem) const { return FString(Stem) + TEXT("_") + Suffix; }

    UStaticMesh* Mesh(const FString& Name, const FString& FixtureHash = FString())
    {
        const FString PackageName = TEXT("/Game/MH/Generated/Meshes/") + Name;
        if (FindObject<UObject>(nullptr, *(PackageName + TEXT(".") + Name)) != nullptr)
        {
            Test.AddError(TEXT("seed test refuses to overwrite occupied fixture ") + Name);
            return nullptr;
        }
        UStaticMesh* Result = NewObject<UStaticMesh>(CreatePackage(*PackageName), FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Result);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Result);
        Receipt->LogicalName = Name;
        Receipt->SourceRelativePath = Name + TEXT(".mesh.fbx");
        const FTCHARToUTF8 Utf8(*Name);
        TArray<uint8> SyntheticPayload;
        SyntheticPayload.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        Receipt->SourceHash = FixtureHash.IsEmpty() ? MHRawPayloadHash(SyntheticPayload) : FixtureHash;
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Result->SetAssetImportData(Receipt);
        return Result;
    }

    bool Apply(UMHCompositeAsset& Asset, const FMHCompositeDocument& Document,
        const TArray<FMHPlacementProfile>& Profiles = {}, const FString& FixtureHash = FString())
    {
        FString Error;
        TArray<uint8> Bytes;
        if (!MHApplyCompositeV5(Asset, Document, Profiles, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(TEXT("managed composite fixture apply failed: ") + Error);
            return false;
        }
        Asset.SourceRelativePath = Asset.LogicalName + TEXT(".composite");
        Asset.SourceHash = FixtureHash.IsEmpty() ? MHRawPayloadHash(Bytes) : FixtureHash;
        Asset.AppliedHash = MHRawPayloadHash(Bytes);
        return true;
    }

    UMHCompositeAsset* Composite(const FString& Name, const FMHCompositeDocument& Document,
        const TArray<FMHPlacementProfile>& Profiles = {}, const bool bGenerated = true,
        const FString& FixtureHash = FString())
    {
        UObject* Outer = GetTransientPackage();
        if (bGenerated)
        {
            const FString PackageName = TEXT("/Game/MH/Generated/Composites/") + Name;
            if (FindObject<UObject>(nullptr, *(PackageName + TEXT(".") + Name)) != nullptr)
            {
                Test.AddError(TEXT("seed test refuses to overwrite occupied fixture ") + Name);
                return nullptr;
            }
            Outer = CreatePackage(*PackageName);
        }
        UMHCompositeAsset* Result = NewObject<UMHCompositeAsset>(Outer,
            bGenerated ? FName(*Name) : NAME_None, bGenerated ? RF_Public | RF_Standalone : RF_Transient);
        Assets.Add(Result);
        Result->LogicalName = Name;
        return Apply(*Result, Document, Profiles, FixtureHash) ? Result : nullptr;
    }

    FMHPlacementProfile OffsetProfile(const FString& Name, const float Base, const float Deviation)
    {
        FMHPlacementProfile Profile;
        Profile.LogicalName = Name;
        Profile.bHasOffsetCm = true;
        Profile.OffsetCm.SetNum(3);
        Profile.OffsetCm[0].Base = Base;
        Profile.OffsetCm[0].Deviation = Deviation;
        StampProfile(Profile);
        return Profile;
    }

    bool StampProfile(FMHPlacementProfile& Profile)
    {
        FString Error;
        TArray<uint8> Bytes;
        if (!MHWriteCanonicalPlacementProfileV1(Profile, Bytes, Error))
        {
            Test.AddError(TEXT("profile fixture failed: ") + Error);
            return false;
        }
        Profile.SetAppliedSourceHash(MHRawPayloadHash(Bytes));
        return true;
    }

    AMHCompositeActor* Spawn(UMHCompositeAsset* Asset = nullptr, const int32 Seed = 100)
    {
        AMHCompositeActor* Actor = World != nullptr ? World->SpawnActor<AMHCompositeActor>() : nullptr;
        Test.TestNotNull(TEXT("seed test actor spawns"), Actor);
        if (Actor != nullptr && Asset != nullptr)
        {
            Actor->SetSeed(Seed);
            Actor->SetCompositeAsset(Asset);
            Test.TestNotNull(*FString::Printf(TEXT("seed %d actor has a plan: %s"), Seed, *Actor->GetLastPlacementError()), Actor->GetResolvedPlan());
        }
        return Actor;
    }

    AMHCompositeActor* Duplicate(AMHCompositeActor& Source, const EDuplicateMode::Type Mode)
    {
        FObjectDuplicationParameters Parameters(&Source, World->PersistentLevel);
        Parameters.DestName = MakeUniqueObjectName(World->PersistentLevel, AMHCompositeActor::StaticClass());
        Parameters.DuplicateMode = Mode;
        AMHCompositeActor* Result = Cast<AMHCompositeActor>(StaticDuplicateObjectEx(Parameters));
        Test.TestNotNull(TEXT("actor duplication returns a composite actor"), Result);
        if (Result != nullptr) DuplicatedActors.Add(Result);
        return Result;
    }
};

TArray<uint8> SeedTestUtf8Bytes(const FString& Text)
{
    const FTCHARToUTF8 Utf8(*Text, Text.Len());
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return Bytes;
}

TSharedPtr<FJsonObject> SeedGoldenWireNode(const TSharedPtr<FJsonObject>& FixtureNode)
{
    TSharedPtr<FJsonObject> Wire = MakeShared<FJsonObject>();
    Wire->Values = FixtureNode->Values;
    Wire->RemoveField(TEXT("trs"));
    Wire->SetObjectField(TEXT("transform"), FixtureNode->GetObjectField(TEXT("trs")));
    if (const TArray<TSharedPtr<FJsonValue>>* Children = nullptr; FixtureNode->TryGetArrayField(TEXT("children"), Children))
    {
        TArray<TSharedPtr<FJsonValue>> WireChildren;
        for (const TSharedPtr<FJsonValue>& Child : *Children)
            WireChildren.Add(MakeShared<FJsonValueObject>(SeedGoldenWireNode(Child->AsObject())));
        Wire->SetArrayField(TEXT("children"), WireChildren);
    }
    return Wire;
}

bool SeedApplyFrozenFixture(FSeedTestFixture& Fixture, const TSharedPtr<FJsonObject>& Golden, UMHCompositeAsset*& OutRoot)
{
    const TSharedPtr<FJsonObject> Input = Golden->GetObjectField(TEXT("fixture"));
    TMap<FString, FString> Hashes;
    for (const TSharedPtr<FJsonValue>& Value : Input->GetArrayField(TEXT("raw_hashes")))
    {
        const TSharedPtr<FJsonObject> Entry = Value->AsObject();
        Hashes.Add(Entry->GetStringField(TEXT("resource")), Entry->GetStringField(TEXT("hash")));
    }
    TMap<FString, FMHPlacementProfile> Profiles;
    for (const TSharedPtr<FJsonValue>& Value : Input->GetArrayField(TEXT("profiles")))
    {
        const TSharedPtr<FJsonObject> InputProfile = Value->AsObject();
        const FString Name = InputProfile->GetStringField(TEXT("name"));
        TSharedPtr<FJsonObject> Wire = MakeShared<FJsonObject>();
        Wire->Values = InputProfile->Values;
        Wire->RemoveField(TEXT("name"));
        FString Json;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        if (!FJsonSerializer::Serialize(Wire.ToSharedRef(), Writer)) return false;
        const FString ProfileJson = TEXT("{\"v\":1,\"kind\":\"placement_profile\",") + Json.Mid(1);
        FMHPlacementProfile Profile;
        FString Error;
        if (!MHParsePlacementProfileV1(SeedTestUtf8Bytes(ProfileJson), Profile, Error))
        {
            Fixture.Test.AddError(TEXT("frozen profile fixture cannot pass v5 codec: ") + Error);
            return false;
        }
        Profile.LogicalName = Name;
        Profile.SetAppliedSourceHash(Hashes.FindRef(TEXT("placement_profile:") + Name));
        Profiles.Add(Name, Profile);
    }
    for (const TSharedPtr<FJsonValue>& Value : Input->GetArrayField(TEXT("composites")))
    {
        const TSharedPtr<FJsonObject> InputComposite = Value->AsObject();
        const FString Name = InputComposite->GetStringField(TEXT("name"));
        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (const TSharedPtr<FJsonValue>& Node : InputComposite->GetArrayField(TEXT("nodes")))
            Nodes.Add(MakeShared<FJsonValueObject>(SeedGoldenWireNode(Node->AsObject())));
        FString NodesJson;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&NodesJson);
        if (!FJsonSerializer::Serialize(Nodes, Writer)) return false;
        FMHCompositeDocument Document;
        FString Error;
        if (!MHParseCompositeV5(SeedTestUtf8Bytes(TEXT("{\"v\":5,\"nodes\":") + NodesJson + TEXT("}")), Document, Error))
        {
            Fixture.Test.AddError(TEXT("frozen composite fixture cannot pass v5 codec: ") + Error);
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
        // The frozen S1 fixture explicitly hashes synthetic domain payloads, not
        // .composite bytes. Only this test adapter injects those accepted receipts.
        UMHCompositeAsset* Asset = Fixture.Composite(Name, Document, Inlined, true, Hashes.FindRef(TEXT("composite:") + Name));
        if (Asset == nullptr) return false;
        if (Name == Input->GetStringField(TEXT("root"))) OutRoot = Asset;
    }
    for (const TPair<FString, FString>& Hash : Hashes)
    {
        if (Hash.Key.StartsWith(TEXT("static_mesh:")) && Fixture.Mesh(Hash.Key.Mid(12), Hash.Value) == nullptr) return false;
    }
    return OutRoot != nullptr;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSeedLifecycleTest,
    "Mimir.V5.Composite.Seed.Lifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSeedLifecycleTest::RunTest(const FString& Parameters)
{
    FSeedTestFixture Fixture(*this);
    AMHCompositeActor* Actor = Fixture.Spawn();
    if (Actor == nullptr) return false;
    TestNotEqual(TEXT("auto-created placement gets a non-zero seed"), Actor->GetSeed(), 0);
    TestTrue(TEXT("new actor duplicates with a new seed by default"), Actor->GetAutoSeed());
    Actor->SetSeed(0);
    TestEqual(TEXT("manual zero is valid"), Actor->GetSeed(), 0);
    Actor->RerunConstructionScripts();
    TestEqual(TEXT("construction does not reinterpret manual zero as auto"), Actor->GetSeed(), 0);
    Actor->Reseed();
    TestNotEqual(TEXT("Reseed produces non-zero"), Actor->GetSeed(), 0);
    const int32 FirstSeed = Actor->GetSeed();
    Actor->Reseed();
    TestNotEqual(TEXT("Reseed differs from previous seed"), Actor->GetSeed(), FirstSeed);
    Actor->SetSeed(100);
    AMHCompositeActor* DefaultCopy = Fixture.Duplicate(*Actor, EDuplicateMode::Normal);
    if (DefaultCopy != nullptr)
    {
        TestNotEqual(TEXT("normal duplicate gets a new seed"), DefaultCopy->GetSeed(), 100);
        TestNotEqual(TEXT("normal duplicate auto seed is non-zero"), DefaultCopy->GetSeed(), 0);
    }
    TestEqual(TEXT("duplicate leaves source seed unchanged"), Actor->GetSeed(), 100);
    Actor->SetAutoSeed(false);
    AMHCompositeActor* KeptCopy = Fixture.Duplicate(*Actor, EDuplicateMode::Normal);
    if (KeptCopy != nullptr)
    {
        TestEqual(TEXT("explicit Keep Seed preserves the duplicate seed"), KeptCopy->GetSeed(), 100);
        TestFalse(TEXT("Keep Seed policy persists on duplicate"), KeptCopy->GetAutoSeed());
    }
    Actor->SetAutoSeed(true);
    AMHCompositeActor* PieCopy = Fixture.Duplicate(*Actor, EDuplicateMode::PIE);
    if (PieCopy != nullptr) TestEqual(TEXT("PIE duplication preserves placement seed even with auto enabled"), PieCopy->GetSeed(), 100);
    TestNull(TEXT("there is no InstanceSeed property"), FindFProperty<FProperty>(AMHCompositeActor::StaticClass(), TEXT("InstanceSeed")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSeedDeterminismTest,
    "Mimir.V5.Composite.Seed.DeterminismNestedProfilesAndMove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSeedDeterminismTest::RunTest(const FString& Parameters)
{
    FSeedTestFixture Fixture(*this);
    UStaticMesh* MeshA = Fixture.Mesh(Fixture.Name(TEXT("s5_seed_mesh_a")));
    UStaticMesh* MeshB = Fixture.Mesh(Fixture.Name(TEXT("s5_seed_mesh_b")));
    if (MeshA == nullptr || MeshB == nullptr) return false;
    FMHCompositeDocument NestedDocument;
    FMHCompositeNode Random;
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Options.Add(SeedTestOption(EMHCompositeOptionKind::Mesh, MeshA->GetName()));
    Random.Options.Add(SeedTestOption(EMHCompositeOptionKind::Mesh, MeshB->GetName()));
    NestedDocument.Nodes.Add(Random);
    FMHPlacementProfile NestedProfile = Fixture.OffsetProfile(Fixture.Name(TEXT("s5_nested_profile")), 5.0f, 2.0f);
    FMHCompositeNode ProfileMesh = SeedTestMeshNode(MeshA->GetName());
    ProfileMesh.Profile = NestedProfile.LogicalName;
    NestedDocument.Nodes.Add(ProfileMesh);
    UMHCompositeAsset* Nested = Fixture.Composite(Fixture.Name(TEXT("s5_seed_nested")), NestedDocument, {NestedProfile}, true);
    if (Nested == nullptr) return false;

    FMHCompositeDocument RootDocument;
    FMHCompositeNode Group;
    Group.Transform.TranslationCm.X = 100.0;
    FMHCompositeNode Child;
    Child.Kind = EMHCompositeNodeKind::Composite;
    Child.Resource = Nested->LogicalName;
    Child.Transform.TranslationCm.X = 25.0;
    Group.Children.Add(Child);
    RootDocument.Nodes.Add(Group);
    FMHPlacementProfile RootProfile = Fixture.OffsetProfile(Fixture.Name(TEXT("s5_root_profile")), 10.0f, 3.0f);
    ProfileMesh.Profile = RootProfile.LogicalName;
    RootDocument.Nodes.Add(ProfileMesh);
    UMHCompositeAsset* Root = Fixture.Composite(TEXT("s5_seed_determinism_root"), RootDocument, {RootProfile});
    if (Root == nullptr) return false;
    AMHCompositeActor* First = Fixture.Spawn(Root, 100);
    AMHCompositeActor* Equal = Fixture.Spawn(Root, 100);
    AMHCompositeActor* Other = Fixture.Spawn(Root, 200);
    if (First == nullptr || Equal == nullptr || Other == nullptr || First->GetResolvedPlan() == nullptr ||
        Equal->GetResolvedPlan() == nullptr || Other->GetResolvedPlan() == nullptr) return false;
    // R2b-2: preview signatures are empty, so equal seeds are compared by plan
    // content; the content includes appearance channels, so the appearance
    // seeds (auto-rolled on spawn) are aligned first.
    Equal->SetAppearanceSeed(First->GetAppearanceSeed());
    if (Equal->GetResolvedPlan() == nullptr) return false;
    const FMHResolvedCompositePlan Before = *First->GetResolvedPlan();
    SeedTestShadowParity(*this, TEXT("equal placement seeds give equal plan content"), Before, *Equal->GetResolvedPlan());
    TestTrue(TEXT("equal placement seeds give byte-identical traces"), SeedTestTraceEqual(Before, *Equal->GetResolvedPlan()));
    // R2b-2: seed separation is observed on the trace; the preview publishes no signature.
    TestFalse(TEXT("seed 200 gives different nested/profile trace"), SeedTestTraceEqual(Before, *Other->GetResolvedPlan()));
    TestTrue(TEXT("nested random and profile consume draws"), Before.Decisions.Num() == 1 && Before.Draws.Num() == 7);
    TestEqual(TEXT("root local profile dominates child-only seed effect"), First->GetSeedAffectsResult(), EMHCompositeSeedEffect::Transform);

    FMHRandomSourceGraph AppliedGraph;
    TSet<FMHResourceKey> Dependencies;
    FString Error;
    TestTrue(TEXT("actor input can be rebuilt strictly from applied receipts"),
        MHBuildAppliedCompositeGraph(*Root, *GetDefault<UMHCompositeSettings>(), AppliedGraph, Dependencies, Error));
    FMHResolvedCompositePlan Direct;
    if (!TestTrue(TEXT("one shared resolver reproduces actor plan"), MHResolveCompositePlan(AppliedGraph, 100, First->GetAppearanceSeed(), Direct, Error))) return false;
    // R2b-2: the actor preview matches the shared resolver in content, not in signature.
    SeedTestShadowParity(*this, TEXT("actor plan matches shared resolver plan"), Direct, Before);
    TestTrue(TEXT("actor trace equals shared resolver trace"), SeedTestTraceEqual(Before, Direct));
    // R2b-2: the preview plane never publishes a derived signature.
    TestTrue(TEXT("read-only derived signature stays empty on the preview plane"), SeedTestSignature(*First).IsEmpty());

    const FSeedTestComponentSnapshot Components(*First);
    First->SetActorTransform(FTransform(FRotator(0.0, 90.0, 0.0), FVector(1000.0, 200.0, 50.0), FVector(2.0)));
    First->RerunConstructionScripts();
    if (!TestNotNull(TEXT("moved actor retains a valid plan"), First->GetResolvedPlan())) return false;
    Components.Check(*this, *First, false);
    TestEqual(TEXT("moving actor does not change seed"), First->GetSeed(), 100);
    // R2b-2: the move keeps the same resident preview plan content, not a signature.
    SeedTestShadowParity(*this, TEXT("moving actor does not change plan content"), Before, *First->GetResolvedPlan());
    TestTrue(TEXT("moving actor does not consume new draws"), SeedTestTraceEqual(Before, *First->GetResolvedPlan()));
    for (int32 Index = 0; Index < Before.Leaves.Num(); ++Index)
    {
        const USceneComponent* Component = SeedTestLeafComponent(*First, Index);
        if (!TestNotNull(TEXT("moved resolved leaf component exists"), Component)) return false;
        FTransform LeafTransform;
        if (!TestTrue(TEXT("moved resolved leaf transform exists"),
                SeedTestLeafTransform(*First, Index, LeafTransform))) return false;
        TestTrue(TEXT("leaf uses plan matrix and actor basis exactly once"), MHMatrixElementsWithinTrsTolerance(
            LeafTransform.ToMatrixWithScale(),
            Before.Leaves[Index].WorldMatrix * First->GetActorTransform().ToMatrixWithScale()));
    }

    const FString RootRawHash = Root->SourceHash;
    const uint32 RevisionBeforeProfile = First->GetPreviewRevision();
    RootProfile.OffsetCm[0].Base += 20.0f;
    if (!Fixture.StampProfile(RootProfile) || !Fixture.Apply(*Root, RootDocument, {RootProfile})) return false;
    MHNotifyGeneratedResourceChanged(SeedTestKey(EMHResourceKind::PlacementProfile, RootProfile.LogicalName));
    if (!TestNotNull(TEXT("profile dependency notification rebuilds placement"), First->GetResolvedPlan())) return false;
    TestEqual(TEXT("profile apply does not rewrite composite raw receipt"), Root->SourceHash, RootRawHash);
    TestEqual(TEXT("dependency notification preserves seed"), First->GetSeed(), 100);
    // R2b-2: signatures live on the proof plane; resolve the applied graph again to observe them.
    FMHRandomSourceGraph ChangedGraph;
    TSet<FMHResourceKey> ChangedDependencies;
    FMHResolvedCompositePlan ChangedDirect;
    if (!TestTrue(TEXT("changed profile still admits an applied graph"),
            MHBuildAppliedCompositeGraph(*Root, *GetDefault<UMHCompositeSettings>(), ChangedGraph, ChangedDependencies, Error)) ||
        !TestTrue(TEXT("changed profile resolves on the proof plane"),
            MHResolveCompositePlan(ChangedGraph, 100, First->GetAppearanceSeed(), ChangedDirect, Error))) return false;
    TestNotEqual(TEXT("profile raw receipt and values change the proof signature"), ChangedDirect.ResolvedSignature, Direct.ResolvedSignature);
    TArray<FString> ProfileMismatches;
    TestFalse(TEXT("profile change refreshes the actor preview plan content"),
        MHCompareRecipeShadowParity(Before, *First->GetResolvedPlan(), ProfileMismatches));
    TestTrue(TEXT("profile change advances the preview revision"), First->GetPreviewRevision() > RevisionBeforeProfile);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSeedMissingAppliedDependencyTest,
    "Mimir.V5.Composite.Seed.MissingAppliedDependencyAndHealing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSeedMissingAppliedDependencyTest::RunTest(const FString& Parameters)
{
    FSeedTestFixture Fixture(*this);
    UStaticMesh* MeshA = Fixture.Mesh(Fixture.Name(TEXT("s5_available_mesh")));
    UStaticMesh* MeshB = Fixture.Mesh(Fixture.Name(TEXT("s5_unselected_mesh")));
    if (MeshA == nullptr || MeshB == nullptr) return false;
    FMHCompositeDocument Document;
    FMHCompositeNode Random;
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Options.Add(SeedTestOption(EMHCompositeOptionKind::Mesh, MeshA->GetName()));
    Random.Options.Add(SeedTestOption(EMHCompositeOptionKind::Mesh, MeshB->GetName(), 0.0f));
    Document.Nodes.Add(Random);
    UMHCompositeAsset* Root = Fixture.Composite(TEXT("s5_seed_missing_root"), Document);
    if (Root == nullptr) return false;
    AMHCompositeActor* Actor = Fixture.Spawn(Root, 100);
    if (Actor == nullptr || Actor->GetResolvedPlan() == nullptr) return false;
    // R2b-2: closures and signatures only exist on the proof plane, so the test
    // rebuilds the applied graph itself instead of reading them off the preview.
    const auto ProofResolve = [&](FString& OutSignature, FString& OutError) -> bool
    {
        OutSignature.Reset();
        OutError.Reset();
        FMHRandomSourceGraph Graph;
        TSet<FMHResourceKey> Dependencies;
        FMHResolvedCompositePlan Proof;
        if (!MHBuildAppliedCompositeGraph(*Root, *GetDefault<UMHCompositeSettings>(), Graph, Dependencies, OutError)) return false;
        if (!MHResolveCompositePlan(Graph, 100, Actor->GetAppearanceSeed(), Proof, OutError)) return false;
        OutSignature = Proof.ResolvedSignature;
        return true;
    };
    FString InitialSignature;
    FString ProofError;
    if (!TestTrue(TEXT("initial applied closure resolves on the proof plane"), ProofResolve(InitialSignature, ProofError))) return false;
    UMHStaticMeshImportData* Receipt = Cast<UMHStaticMeshImportData>(MeshB->GetAssetImportData());
    const FMHResourceKey MissingKey = SeedTestKey(EMHResourceKind::StaticMesh, MeshB->GetName());
    TestTrue(TEXT("unselected option is still a placement dependency"), Actor->DependsOnResource(MissingKey));
    MeshB->SetAssetImportData(nullptr);
    MHNotifyGeneratedResourceChanged(MissingKey);
    // R2b-2: the preview materializes Layout + Appearance and never validates unselected endpoints.
    TestNotNull(TEXT("missing unselected dependency still exposes a preview plan"), Actor->GetResolvedPlan());
    TestTrue(TEXT("missing unselected dependency raises no placement error"), Actor->GetLastPlacementError().IsEmpty());
    TestTrue(TEXT("preview retains the dependency key for healing"), Actor->DependsOnResource(MissingKey));
    FString BrokenSignature;
    // R2b-2: the applied-graph refusal moved to the proof plane with the same diagnostic.
    TestFalse(TEXT("missing applied dependency blocks the proof closure"), ProofResolve(BrokenSignature, ProofError));
    TestTrue(TEXT("unresolved diagnostic names unavailable unselected resource"),
        ProofError.Contains(MeshB->GetName()) && ProofError.Contains(TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE")));
    MeshB->SetAssetImportData(Receipt);
    MHNotifyGeneratedResourceChanged(MissingKey);
    if (!TestNotNull(TEXT("restoring receipt heals same placement"), Actor->GetResolvedPlan())) return false;
    FString HealedSignature;
    TestTrue(TEXT("restored receipt resolves the proof closure again"), ProofResolve(HealedSignature, ProofError));
    TestEqual(TEXT("identical closure heals to original signature"), HealedSignature, InitialSignature);
    TestTrue(TEXT("healing clears error and warnings"), Actor->GetLastPlacementError().IsEmpty() && Actor->GetLastPlacementWarnings().IsEmpty());

    const TArray<uint8> ChangedUnselectedPayload = {0x75, 0x6e, 0x73, 0x65, 0x6c};
    Receipt->SourceHash = MHRawPayloadHash(ChangedUnselectedPayload);
    MHNotifyGeneratedResourceChanged(MissingKey);
    if (!TestNotNull(TEXT("unselected dependency change keeps a valid plan"), Actor->GetResolvedPlan())) return false;
    FString ChangedSignature;
    // R2b-2: an unselected-only receipt change is observable on the proof signature only.
    TestTrue(TEXT("changed unselected receipt still resolves the proof closure"), ProofResolve(ChangedSignature, ProofError));
    TestNotEqual(TEXT("unselected source closure hash changes resolved signature"), ChangedSignature, InitialSignature);
    TestEqual(TEXT("unselected change does not change placement seed"), Actor->GetSeed(), 100);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSeedShearAdmissionTest,
    "Mimir.V5.Composite.Seed.ShearBeforeComponentMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSeedShearAdmissionTest::RunTest(const FString& Parameters)
{
    FSeedTestFixture Fixture(*this);
    UStaticMesh* Mesh = Fixture.Mesh(Fixture.Name(TEXT("s5_shear_mesh")));
    if (Mesh == nullptr) return false;
    FMHCompositeDocument Document;
    FMHCompositeNode Parent;
    FMHCompositeNode Child = SeedTestMeshNode(Mesh->GetName(), 25.0);
    Child.Transform.RotationQuat = FQuat(FVector::UpVector, FMath::DegreesToRadians(45.0));
    Parent.Children.Add(Child);
    Document.Nodes.Add(Parent);
    UMHCompositeAsset* Root = Fixture.Composite(TEXT("s5_seed_shear_root"), Document);
    if (Root == nullptr) return false;
    AMHCompositeActor* Actor = Fixture.Spawn(Root, 100);
    if (Actor == nullptr || Actor->GetResolvedPlan() == nullptr) return false;
    const FSeedTestComponentSnapshot Before(*Actor);
    Document.Nodes[0].Transform.Scale = FVector(2.0, 1.0, 1.0);
    if (!Fixture.Apply(*Root, Document)) return false;
    MHNotifyGeneratedResourceChanged(SeedTestKey(EMHResourceKind::Composite, Root->LogicalName));
    // R2b-2: the preview publishes no derived signature; the null plan above carries the refusal.
    TestNull(TEXT("sheared source cannot expose a current plan"), Actor->GetResolvedPlan());
    TestTrue(TEXT("shear uses registered fail-closed diagnostic"), Actor->GetLastPlacementError().Contains(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM")));
    Before.Check(*this, *Actor);
    Actor->SetActorLocation(FVector(100.0, 0.0, 0.0));
    TestNull(TEXT("ordinary movement cannot heal a rejected applied definition with the cached old plan"), Actor->GetResolvedPlan());
    TestTrue(TEXT("definition rejection remains visible after moving placement"), Actor->GetLastPlacementError().Contains(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM")));
    // R2b-2: republication of a stale result is observed as the still-null plan above, not as a signature.
    Before.Check(*this, *Actor);

    Document.Nodes[0].Transform.Scale = FVector::OneVector;
    if (!Fixture.Apply(*Root, Document)) return false;
    MHNotifyGeneratedResourceChanged(SeedTestKey(EMHResourceKind::Composite, Root->LogicalName));
    if (!TestNotNull(TEXT("representable source restores valid plan"), Actor->GetResolvedPlan())) return false;
    const FSeedTestComponentSnapshot BeforeActorShear(*Actor);
    Actor->SetActorScale3D(FVector(2.0, 1.0, 1.0));
    TestNull(TEXT("actor basis shear invalidates current plan admission"), Actor->GetResolvedPlan());
    TestTrue(TEXT("actor basis shear reports diagnostic"), Actor->GetLastPlacementError().Contains(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM")));
    BeforeActorShear.Check(*this, *Actor);
    Actor->SetActorScale3D(FVector::OneVector);
    TestNotNull(TEXT("moving back to representable basis restores cached resolution"), Actor->GetResolvedPlan());
    TestEqual(TEXT("shear refusal never changes seed"), Actor->GetSeed(), 100);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSeedMinimalUpdatesTest,
    "Mimir.V5.Composite.Seed.MinimalNoneTransformTopologyUpdates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSeedMinimalUpdatesTest::RunTest(const FString& Parameters)
{
    FSeedTestFixture Fixture(*this);
    UStaticMesh* MeshA = Fixture.Mesh(Fixture.Name(TEXT("s5_update_mesh_a")));
    UStaticMesh* MeshB = Fixture.Mesh(Fixture.Name(TEXT("s5_update_mesh_b")));
    if (MeshA == nullptr || MeshB == nullptr) return false;
    FMHCompositeDocument NoneDocument;
    NoneDocument.Nodes.Add(SeedTestMeshNode(MeshA->GetName(), 25.0));
    NoneDocument.Nodes.Add(SeedTestMeshNode(MeshB->GetName(), 100.0));
    UMHCompositeAsset* NoneAsset = Fixture.Composite(TEXT("s5_seed_none_root"), NoneDocument);
    if (NoneAsset == nullptr) return false;
    AMHCompositeActor* NoneActor = Fixture.Spawn(NoneAsset, 100);
    if (NoneActor == nullptr || NoneActor->GetResolvedPlan() == nullptr) return false;
    const FSeedTestComponentSnapshot NoneSnapshot(*NoneActor);
    NoneActor->SetSeed(200);
    TestEqual(TEXT("fixed definition classifies None"), NoneActor->GetSeedAffectsResult(), EMHCompositeSeedEffect::None);
    NoneSnapshot.Check(*this, *NoneActor);
    if (!TestNotNull(TEXT("None seed change keeps current plan"), NoneActor->GetResolvedPlan())) return false;
    TestTrue(TEXT("None without streams remains draw-free"), NoneActor->GetResolvedPlan()->Draws.IsEmpty());
    // R2b-2: the preview carries no signature; the resident plan records the new seed instead.
    TestEqual(TEXT("None seed change updates the resident plan seed"), NoneActor->GetResolvedPlan()->Seed, 200);

    FMHPlacementProfile VaryingProfile = Fixture.OffsetProfile(Fixture.Name(TEXT("s5_update_profile")), 10.0f, 5.0f);
    FMHCompositeDocument TransformDocument = NoneDocument;
    TransformDocument.Nodes[0].Profile = VaryingProfile.LogicalName;
    UMHCompositeAsset* TransformAsset = Fixture.Composite(TEXT("s5_seed_transform_root"), TransformDocument, {VaryingProfile});
    if (TransformAsset == nullptr) return false;
    AMHCompositeActor* TransformActor = Fixture.Spawn(TransformAsset, 100);
    if (TransformActor == nullptr || TransformActor->GetResolvedPlan() == nullptr) return false;
    const FSeedTestComponentSnapshot TransformSnapshot(*TransformActor);
    USceneComponent* MovingLeaf = SeedTestLeafComponent(*TransformActor, 0);
    USceneComponent* FixedLeaf = SeedTestLeafComponent(*TransformActor, 1);
    if (!TestNotNull(TEXT("profile leaf exists"), MovingLeaf) || !TestNotNull(TEXT("fixed leaf exists"), FixedLeaf)) return false;
    FTransform MovingBefore;
    FTransform FixedBefore;
    if (!SeedTestLeafTransform(*TransformActor, 0, MovingBefore) ||
        !SeedTestLeafTransform(*TransformActor, 1, FixedBefore)) return false;
    TransformActor->SetSeed(200);
    TestEqual(TEXT("varying profile classifies Transform"), TransformActor->GetSeedAffectsResult(), EMHCompositeSeedEffect::Transform);
    TransformSnapshot.Check(*this, *TransformActor, false);
    FTransform MovingAfter;
    FTransform FixedAfter;
    if (!SeedTestLeafTransform(*TransformActor, 0, MovingAfter) ||
        !SeedTestLeafTransform(*TransformActor, 1, FixedAfter)) return false;
    TestFalse(TEXT("profile-dependent leaf gets its new transform"), MovingAfter.Equals(MovingBefore, 0.0));
    TestTrue(TEXT("unaffected leaf transform is untouched"), FixedAfter.Equals(FixedBefore, 0.0));

    FMHCompositeDocument TopologyDocument;
    FMHCompositeNode Random;
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Options.Add(SeedTestOption(EMHCompositeOptionKind::Mesh, MeshA->GetName()));
    Random.Options.Add(SeedTestOption(EMHCompositeOptionKind::Mesh, MeshB->GetName()));
    TopologyDocument.Nodes.Add(Random);
    TopologyDocument.Nodes.Add(SeedTestMeshNode(MeshA->GetName(), 100.0));
    // This stable NodePath selects option 0 for seed 100 and option 1 for 200.
    // Mesh receipt names are unique per test but do not key the node stream.
    UMHCompositeAsset* TopologyAsset = Fixture.Composite(TEXT("s5_seed_topology"), TopologyDocument);
    if (TopologyAsset == nullptr) return false;
    AMHCompositeActor* TopologyActor = Fixture.Spawn(TopologyAsset, 100);
    if (TopologyActor == nullptr || TopologyActor->GetResolvedPlan() == nullptr) return false;
    const TArray<TObjectPtr<USceneComponent>> Handles = TopologyActor->GetTopLevelPlacementComponents();
    USceneComponent* PriorChoice = SeedTestLeafComponent(*TopologyActor, 0);
    USceneComponent* UnaffectedLeaf = SeedTestLeafComponent(*TopologyActor, 1);
    if (!TestNotNull(TEXT("initial choice leaf exists"), PriorChoice) || !TestNotNull(TEXT("topology fixed leaf exists"), UnaffectedLeaf)) return false;
    FTransform UnaffectedTransform;
    if (!SeedTestLeafTransform(*TopologyActor, 1, UnaffectedTransform)) return false;
    TestEqual(TEXT("seed 100 fixture selects mesh A"), TopologyActor->GetResolvedPlan()->Decisions[0].OptionIndex, 0);
    TopologyActor->SetSeed(200);
    if (!TestNotNull(TEXT("topology update produces new plan"), TopologyActor->GetResolvedPlan())) return false;
    TestEqual(TEXT("different resources classify Topology"), TopologyActor->GetSeedAffectsResult(), EMHCompositeSeedEffect::Topology);
    TestEqual(TEXT("seed 200 fixture selects mesh B"), TopologyActor->GetResolvedPlan()->Decisions[0].OptionIndex, 1);
    TestTrue(TEXT("topology update preserves authored handles"), TopologyActor->GetTopLevelPlacementComponents() == Handles);
    TestNotEqual(TEXT("changed resource replaces only its selected leaf"), SeedTestLeafComponent(*TopologyActor, 0), PriorChoice);
    TestEqual(TEXT("unaffected leaf object survives topology change"), SeedTestLeafComponent(*TopologyActor, 1), UnaffectedLeaf);
    FTransform UnaffectedAfter;
    if (!SeedTestLeafTransform(*TopologyActor, 1, UnaffectedAfter)) return false;
    TestTrue(TEXT("unaffected leaf transform survives topology change"),
        UnaffectedAfter.Equals(UnaffectedTransform, 0.0));

    UMHCompositeAsset* Nested = Fixture.Composite(Fixture.Name(TEXT("s5_child_variation")), TransformDocument, {VaryingProfile}, true);
    if (Nested == nullptr) return false;
    FMHCompositeDocument ChildDocument;
    FMHCompositeNode NestedNode;
    NestedNode.Kind = EMHCompositeNodeKind::Composite;
    NestedNode.Resource = Nested->LogicalName;
    ChildDocument.Nodes.Add(NestedNode);
    ChildDocument.Nodes.Add(SeedTestMeshNode(MeshB->GetName(), 250.0));
    UMHCompositeAsset* ChildAsset = Fixture.Composite(TEXT("s5_seed_child_only_root"), ChildDocument);
    if (ChildAsset == nullptr) return false;
    AMHCompositeActor* ChildActor = Fixture.Spawn(ChildAsset, 100);
    if (ChildActor == nullptr || ChildActor->GetResolvedPlan() == nullptr) return false;
    const FSeedTestComponentSnapshot ChildSnapshot(*ChildActor);
    USceneComponent* ChildFixed = SeedTestLeafComponent(*ChildActor, 2);
    if (!TestNotNull(TEXT("root sibling of nested variation exists"), ChildFixed)) return false;
    FTransform ChildFixedTransform;
    if (!SeedTestLeafTransform(*ChildActor, 2, ChildFixedTransform)) return false;
    ChildActor->SetSeed(200);
    TestEqual(TEXT("nested-only variation classifies ChildSeedsOnly"), ChildActor->GetSeedAffectsResult(), EMHCompositeSeedEffect::ChildSeedsOnly);
    ChildSnapshot.Check(*this, *ChildActor, false);
    FTransform ChildFixedAfter;
    if (!SeedTestLeafTransform(*ChildActor, 2, ChildFixedAfter)) return false;
    TestTrue(TEXT("root sibling remains untouched by child-only variation"),
        ChildFixedAfter.Equals(ChildFixedTransform, 0.0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSeedConstantTraceTest,
    "Mimir.V5.Composite.Seed.ConstantVisualsRefreshTrace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSeedConstantTraceTest::RunTest(const FString& Parameters)
{
    FSeedTestFixture Fixture(*this);
    UStaticMesh* Mesh = Fixture.Mesh(Fixture.Name(TEXT("s5_constant_mesh")));
    if (Mesh == nullptr) return false;
    FMHCompositeDocument Document;
    FMHCompositeNode Random;
    Random.Kind = EMHCompositeNodeKind::Random;
    Random.Options.Add(SeedTestOption(EMHCompositeOptionKind::Mesh, Mesh->GetName()));
    Document.Nodes.Add(Random);
    FMHPlacementProfile Profile = Fixture.OffsetProfile(Fixture.Name(TEXT("s5_constant_profile")), 10.0f, 0.0f);
    FMHCompositeNode ProfileNode = SeedTestMeshNode(Mesh->GetName(), 100.0);
    ProfileNode.Profile = Profile.LogicalName;
    Document.Nodes.Add(ProfileNode);
    UMHCompositeAsset* Root = Fixture.Composite(TEXT("s5_seed_constant_root"), Document, {Profile});
    if (Root == nullptr) return false;
    AMHCompositeActor* Actor = Fixture.Spawn(Root, 100);
    if (Actor == nullptr || Actor->GetResolvedPlan() == nullptr) return false;
    const FMHResolvedCompositePlan Before = *Actor->GetResolvedPlan();
    const FSeedTestComponentSnapshot Components(*Actor);
    const uint32 RevisionBeforeReseed = Actor->GetPreviewRevision();
    Actor->SetSeed(200);
    if (!TestNotNull(TEXT("constant-visual seed change retains valid plan"), Actor->GetResolvedPlan())) return false;
    TestEqual(TEXT("constant random/profile definition classifies None"), Actor->GetSeedAffectsResult(), EMHCompositeSeedEffect::None);
    Components.Check(*this, *Actor);
    TestEqual(TEXT("single selection plus three offset draws remain present"), Actor->GetResolvedPlan()->Draws.Num(), 4);
    TestFalse(TEXT("None shortcut does not preserve stale trace"), SeedTestTraceEqual(Before, *Actor->GetResolvedPlan()));
    // R2b-2: the None shortcut is observed as a rebuilt preview at the new seed, not as a signature.
    TestEqual(TEXT("None shortcut still resolves the current seed"), Actor->GetResolvedPlan()->Seed, 200);
    TestTrue(TEXT("None shortcut advances the preview revision"), Actor->GetPreviewRevision() > RevisionBeforeReseed);
    FMHRandomSourceGraph Graph;
    TSet<FMHResourceKey> Dependencies;
    FString Error;
    FMHResolvedCompositePlan Expected;
    if (!MHBuildAppliedCompositeGraph(*Root, *GetDefault<UMHCompositeSettings>(), Graph, Dependencies, Error) ||
        !MHResolveCompositePlan(Graph, 200, Actor->GetAppearanceSeed(), Expected, Error))
    {
        AddError(Error);
        return false;
    }
    TestTrue(TEXT("constant-visual actor trace matches fresh shared resolution"), SeedTestTraceEqual(Expected, *Actor->GetResolvedPlan()));
    // R2b-2: the preview has no signature; parity against the proof plan is the equivalence.
    SeedTestShadowParity(*this, TEXT("constant-visual actor plan matches fresh shared resolution"), Expected, *Actor->GetResolvedPlan());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeSeedFrozenVectorTest,
    "Mimir.V5.Composite.Seed.FrozenSeed42AppliedAssetParity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeSeedFrozenVectorTest::RunTest(const FString& Parameters)
{
    FString GoldenRoot;
    if (!ResolveGoldenRoot(*this, GoldenRoot)) return false;
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(GoldenRoot, TEXT("v5/random_stream_1_vectors.json"))))
    {
        AddError(TEXT("cannot read frozen random_stream_1_vectors.json"));
        return false;
    }
    TSharedPtr<FJsonValue> Parsed;
    const FMHCanonicalResult Parse = MHParseJsonUtf8(Bytes, Parsed);
    if (!Parse.bSuccess || !Parsed.IsValid() || Parsed->Type != EJson::Object)
    {
        AddError(TEXT("invalid frozen vector JSON: ") + Parse.Error);
        return false;
    }
    const TSharedPtr<FJsonObject> Golden = Parsed->AsObject();
    TSharedPtr<FJsonObject> Expected;
    for (const TSharedPtr<FJsonValue>& Value : Golden->GetArrayField(TEXT("plan_vectors")))
    {
        if (Value->AsObject()->GetNumberField(TEXT("seed")) == 42.0) Expected = Value->AsObject();
    }
    if (!TestTrue(TEXT("ratified seed 42 vector exists"), Expected.IsValid())) return false;
    FSeedTestFixture Fixture(*this);
    UMHCompositeAsset* Root = nullptr;
    if (!SeedApplyFrozenFixture(Fixture, Golden, Root)) return false;
    AMHCompositeActor* Actor = Fixture.Spawn(Root, 42);
    if (Actor == nullptr || Actor->GetResolvedPlan() == nullptr) return false;
    const FMHResolvedCompositePlan& Plan = *Actor->GetResolvedPlan();
    // R2b-2: the ratified signature and preimage belong to the proof plane; resolve the applied graph for them.
    FMHRandomSourceGraph FrozenGraph;
    TSet<FMHResourceKey> FrozenDependencies;
    FMHResolvedCompositePlan Proof;
    FString FrozenError;
    if (!TestTrue(TEXT("frozen fixture admits an applied graph"),
            MHBuildAppliedCompositeGraph(*Root, *GetDefault<UMHCompositeSettings>(), FrozenGraph, FrozenDependencies, FrozenError)) ||
        !TestTrue(TEXT("frozen fixture resolves on the proof plane"),
            MHResolveCompositePlan(FrozenGraph, 42, Actor->GetAppearanceSeed(), Proof, FrozenError))) return false;
    TestEqual(TEXT("proof plan matches ratified resolver signature"), Proof.ResolvedSignature, Expected->GetStringField(TEXT("resolved_signature")));
    TestTrue(TEXT("proof signature preimage matches ratified bytes"),
        Proof.SignaturePreimage == SeedTestUtf8Bytes(Expected->GetStringField(TEXT("signature_preimage_utf8"))));
    SeedTestShadowParity(*this, TEXT("actor preview matches the ratified proof plan"), Proof, Plan);
    const TArray<TSharedPtr<FJsonValue>>& Decisions = Expected->GetArrayField(TEXT("decisions"));
    if (!TestEqual(TEXT("ratified decision count"), Plan.Decisions.Num(), Decisions.Num())) return false;
    for (int32 Index = 0; Index < Decisions.Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Decision = Decisions[Index]->AsObject();
        TestEqual(TEXT("ratified decision NodePath"), Plan.Decisions[Index].NodePath, Decision->GetStringField(TEXT("path")));
        TestEqual(TEXT("ratified selected option"), Plan.Decisions[Index].OptionIndex, static_cast<int32>(Decision->GetNumberField(TEXT("option"))));
        TestEqual(TEXT("ratified selection raw draw"), Plan.Decisions[Index].RawU32, static_cast<uint32>(Decision->GetNumberField(TEXT("raw_u32"))));
        TestEqual(TEXT("ratified ordered total"), Plan.Decisions[Index].Total, Decision->GetNumberField(TEXT("total")));
    }
    const TArray<TSharedPtr<FJsonValue>>& Draws = Expected->GetArrayField(TEXT("draws"));
    if (!TestEqual(TEXT("ratified draw count"), Plan.Draws.Num(), Draws.Num())) return false;
    for (int32 Index = 0; Index < Draws.Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Draw = Draws[Index]->AsObject();
        TestEqual(TEXT("ratified draw NodePath"), Plan.Draws[Index].NodePath, Draw->GetStringField(TEXT("path")));
        TestEqual(TEXT("ratified draw role"), Plan.Draws[Index].Role, Draw->GetStringField(TEXT("role")));
        TestEqual(TEXT("ratified raw draw"), Plan.Draws[Index].RawU32, static_cast<uint32>(Draw->GetNumberField(TEXT("raw_u32"))));
        TestEqual(TEXT("ratified unit draw"), Plan.Draws[Index].Unit, Draw->GetNumberField(TEXT("unit")));
        TestEqual(TEXT("ratified sampled value"), Plan.Draws[Index].Sample, Draw->GetNumberField(TEXT("sample")));
    }
    const TArray<TSharedPtr<FJsonValue>>& Leaves = Expected->GetArrayField(TEXT("leaves"));
    if (!TestEqual(TEXT("ratified leaf count"), Plan.Leaves.Num(), Leaves.Num())) return false;
    for (int32 Index = 0; Index < Leaves.Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Leaf = Leaves[Index]->AsObject();
        TestEqual(TEXT("ratified leaf resource"), Plan.Leaves[Index].Resource, Leaf->GetStringField(TEXT("resource")));
        TestEqual(TEXT("ratified leaf origin"), Plan.Leaves[Index].Origin, Leaf->GetStringField(TEXT("origin")));
        const USceneComponent* Component = SeedTestLeafComponent(*Actor, Index);
        if (!TestNotNull(TEXT("ratified leaf produces a preview component"), Component)) return false;
        FTransform LeafTransform;
        if (!TestTrue(TEXT("ratified leaf preview transform can be read"),
                SeedTestLeafTransform(*Actor, Index, LeafTransform))) return false;
        TestTrue(TEXT("preview consumes the full plan matrix"), MHMatrixElementsWithinTrsTolerance(
            LeafTransform.ToMatrixWithScale(), Plan.Leaves[Index].WorldMatrix));
    }
    TestEqual(TEXT("root 100 plus child 25 reaches preview unchanged"), Plan.Leaves[0].WorldMatrix.M[3][0], 125.0);
    // 100/200 are placement acceptance seeds, not extra frozen vectors. Use
    // the very same ratified fixture without regenerating its expected bytes.
    AMHCompositeActor* SameA = Fixture.Spawn(Root, 100);
    AMHCompositeActor* SameB = Fixture.Spawn(Root, 100);
    AMHCompositeActor* Different = Fixture.Spawn(Root, 200);
    if (SameA == nullptr || SameB == nullptr || Different == nullptr ||
        !TestNotNull(TEXT("frozen fixture seed 100 A resolves"), SameA->GetResolvedPlan()) ||
        !TestNotNull(TEXT("frozen fixture seed 100 B resolves"), SameB->GetResolvedPlan()) ||
        !TestNotNull(TEXT("frozen fixture seed 200 resolves"), Different->GetResolvedPlan())) return false;
    // R2b-2: seed equality and separation are observed on the preview trace, not on signatures.
    TestTrue(TEXT("ratified fixture 100 placements have identical traces"), SeedTestTraceEqual(*SameA->GetResolvedPlan(), *SameB->GetResolvedPlan()));
    TestFalse(TEXT("ratified fixture 200 trace differs"), SeedTestTraceEqual(*SameA->GetResolvedPlan(), *Different->GetResolvedPlan()));
    return true;
}

} // namespace UE::MimirComposite::Tests
