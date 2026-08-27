#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Info.h"
#include "Math/Transform.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Random/MHRandomStream.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite::Tests
{
namespace
{

FString AdmissionStoredSignature(const AMHCompositeActor& Actor)
{
    const FStrProperty* Property = FindFProperty<FStrProperty>(AMHCompositeActor::StaticClass(), TEXT("ResolvedSignature"));
    return Property != nullptr ? Property->GetPropertyValue_InContainer(&Actor) : FString();
}

struct FAppliedAdmissionFixture
{
    FAutomationTestBase& Test;
    FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
    UWorld* World = nullptr;
    UMHCompositeSettings* Settings = GetMutableDefault<UMHCompositeSettings>();
    TMap<FString, FSoftClassPath> PreviousActorRegistry = Settings->ActorClassRegistry;
    TArray<UObject*> Assets;
    TSet<UObject*> RegisteredAssets;

    explicit FAppliedAdmissionFixture(FAutomationTestBase& InTest) : Test(InTest)
    {
        World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
        Test.TestNotNull(TEXT("admission test world exists"), World);
    }

    ~FAppliedAdmissionFixture()
    {
        if (World != nullptr) World->DestroyWorld(false);
        Settings->ActorClassRegistry = PreviousActorRegistry;
        for (UObject* Asset : RegisteredAssets) FAssetRegistryModule::AssetDeleted(Asset);
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

    UMHCompositeAsset* Composite(const FString& Name, const FMHCompositeDocument& Document,
        const TArray<FMHPlacementProfile>& Profiles = {}, const bool bDuplicatePath = false)
    {
        const FString PackageName = (bDuplicatePath ? TEXT("/Game/MimirCompositeTests/AdmissionAlias/") : TEXT("/Game/MH/Generated/Composites/")) + Name;
        UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(CreatePackage(*PackageName), FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Asset);
        Asset->LogicalName = Name;
        FString Error;
        TArray<uint8> Bytes;
        if (!MHApplyCompositeV5(*Asset, Document, Profiles, Error) || !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(TEXT("admission fixture apply failed: ") + Error);
            return nullptr;
        }
        Asset->SourceRelativePath = Name + TEXT(".composite");
        Asset->SourceHash = MHRawPayloadHash(Bytes);
        Asset->AppliedHash = Asset->SourceHash;
        return Asset;
    }

    UStaticMesh* Mesh(const FString& Name)
    {
        UStaticMesh* Mesh = NewObject<UStaticMesh>(CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + Name)), FName(*Name), RF_Public | RF_Standalone);
        Assets.Add(Mesh);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = Name;
        Receipt->SourceRelativePath = Name + TEXT(".mesh.fbx");
        const TArray<uint8> SyntheticPayload = {0x61, 0x64, 0x6d, 0x69, 0x74};
        Receipt->SourceHash = MHRawPayloadHash(SyntheticPayload);
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);
        return Mesh;
    }

    AMHCompositeActor* Spawn(UMHCompositeAsset& Asset)
    {
        AMHCompositeActor* Actor = World != nullptr ? World->SpawnActor<AMHCompositeActor>() : nullptr;
        Test.TestNotNull(TEXT("admission test placement exists"), Actor);
        if (Actor != nullptr)
        {
            Actor->SetSeed(100);
            Actor->SetCompositeAsset(&Asset);
        }
        return Actor;
    }

    void Register(UObject& Asset)
    {
        FAssetRegistryModule::AssetCreated(&Asset);
        RegisteredAssets.Add(&Asset);
    }

    void RemoveClaim(UObject& Asset)
    {
        if (RegisteredAssets.Remove(&Asset) > 0) FAssetRegistryModule::AssetDeleted(&Asset);
        Asset.ClearFlags(RF_Public | RF_Standalone);
        Asset.MarkAsGarbage();
    }

    bool ExpectRejected(UMHCompositeAsset& Root, AMHCompositeActor& Actor, const TCHAR* Diagnostic)
    {
        FMHRandomSourceGraph Graph;
        TSet<FMHResourceKey> Dependencies;
        FString Error;
        bool bPassed = Test.TestFalse(TEXT("invalid applied state blocks graph admission"),
            MHBuildAppliedCompositeGraph(Root, *Settings, Graph, Dependencies, Error));
        bPassed &= Test.TestTrue(TEXT("graph refusal preserves diagnostic"), Error.Contains(Diagnostic));
        Actor.RebuildComposite();
        bPassed &= Test.TestNull(TEXT("invalid applied state has no current placement plan"), Actor.GetResolvedPlan());
        bPassed &= Test.TestTrue(TEXT("invalid applied state has no current derived signature"), AdmissionStoredSignature(Actor).IsEmpty());
        UMHCompositeLevelSubsystem* Operations = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
        if (!Test.TestNotNull(TEXT("Break subsystem exists"), Operations)) return false;
        const TArray<TObjectPtr<UActorComponent>> BeforeBreak = Actor.GetDerivedComponents();
        TArray<AActor*> BrokenActors;
        TArray<FString> Warnings;
        Error.Reset();
        bPassed &= Test.TestFalse(TEXT("Break refuses the invalid placement before spawning"),
            Operations->BreakComposites({&Actor}, BrokenActors, Warnings, Error));
        bPassed &= Test.TestTrue(TEXT("Break refusal preserves original diagnostic"), Error.Contains(Diagnostic));
        bPassed &= Test.TestTrue(TEXT("failed Break publishes no actors"), BrokenActors.IsEmpty());
        bPassed &= Test.TestTrue(TEXT("failed Break preserves original placement"), IsValid(&Actor) && !Actor.IsActorBeingDestroyed());
        bPassed &= Test.TestTrue(TEXT("failed Break preserves component objects"), Actor.GetDerivedComponents() == BeforeBreak);
        return bPassed;
    }
};

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeInvalidRootReceiptAdmissionTest,
    "Mimir.V5.Composite.AppliedAdmission.InvalidRootReceiptBlocksPlanAndBreak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeInvalidRootReceiptAdmissionTest::RunTest(const FString& Parameters)
{
    FAppliedAdmissionFixture Fixture(*this);
    FMHCompositeDocument Document;
    Document.Nodes.AddDefaulted();
    UMHCompositeAsset* Root = Fixture.Composite(Fixture.Name(TEXT("s5_invalid_receipt")), Document);
    if (Root == nullptr) return false;
    AMHCompositeActor* Actor = Fixture.Spawn(*Root);
    if (Actor == nullptr || !TestNotNull(TEXT("valid receipt starts admitted"), Actor->GetResolvedPlan())) return false;
    const FString SourcePath = Root->SourceRelativePath;
    const FString AppliedHash = Root->AppliedHash;
    Root->SourceRelativePath.Reset();
    bool bPassed = Fixture.ExpectRejected(*Root, *Actor, TEXT("MH_E_SOURCE_INDEX_INVALID"));
    Root->SourceRelativePath = SourcePath;
    Actor->RebuildComposite();
    bPassed &= TestNotNull(TEXT("restoring SourcePath heals receipt admission"), Actor->GetResolvedPlan());
    Root->AppliedHash.Reset();
    bPassed &= Fixture.ExpectRejected(*Root, *Actor, TEXT("MH_E_SOURCE_INDEX_INVALID"));
    Root->AppliedHash = AppliedHash;
    Actor->RebuildComposite();
    bPassed &= TestNotNull(TEXT("restoring AppliedHash heals receipt admission"), Actor->GetResolvedPlan());
    Root->SourceRelativePath = TEXT("../") + SourcePath;
    bPassed &= Fixture.ExpectRejected(*Root, *Actor, TEXT("MH_E_SOURCE_INDEX_INVALID"));
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeDuplicateRootClaimAdmissionTest,
    "Mimir.V5.Composite.AppliedAdmission.DuplicateRootClaimBlocksPlanAndBreak",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeDuplicateRootClaimAdmissionTest::RunTest(const FString& Parameters)
{
    FAppliedAdmissionFixture Fixture(*this);
    FMHCompositeDocument Document;
    Document.Nodes.AddDefaulted();
    const FString Name = Fixture.Name(TEXT("s5_duplicate_root"));
    UMHCompositeAsset* Root = Fixture.Composite(Name, Document);
    if (Root == nullptr) return false;
    Fixture.Register(*Root);
    AMHCompositeActor* Actor = Fixture.Spawn(*Root);
    if (Actor == nullptr || !TestNotNull(TEXT("unique registered root starts admitted"), Actor->GetResolvedPlan())) return false;
    UMHCompositeAsset* Duplicate = Fixture.Composite(Name, Document, {}, true);
    if (Duplicate == nullptr) return false;
    Fixture.Register(*Duplicate);
    FARFilter Filter;
    Filter.TagsAndValues.Add(TEXT("MH.LogicalName"), Name);
    TArray<FAssetData> Claims;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Claims);
    // TagsAndValues entries are alternatives, not an AND predicate. Keep the
    // exact ResourceKey comparison explicit instead of counting unrelated tags.
    Claims.RemoveAll([&Name](const FAssetData& Claim)
    {
        FString Kind;
        FString LogicalName;
        return !Claim.GetTagValue(TEXT("MH.Kind"), Kind) || Kind != TEXT("composite") ||
            !Claim.GetTagValue(TEXT("MH.LogicalName"), LogicalName) || LogicalName != Name;
    });
    bool bPassed = TestEqual(TEXT("two explicit registry claims share one ResourceKey"), Claims.Num(), 2);
    bPassed &= TestNotEqual(TEXT("duplicate claims live at distinct object paths"), Root->GetPathName(), Duplicate->GetPathName());
    bPassed &= Fixture.ExpectRejected(*Root, *Actor, TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET"));
    Fixture.RemoveClaim(*Duplicate);
    Actor->RebuildComposite();
    bPassed &= TestNotNull(TEXT("deleting conflicting claim heals the unique root"), Actor->GetResolvedPlan());
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeAbstractActorAdmissionTest,
    "Mimir.V5.Composite.AppliedAdmission.UnselectedAbstractActorBlocksClosure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeAbstractActorAdmissionTest::RunTest(const FString& Parameters)
{
    FAppliedAdmissionFixture Fixture(*this);
    TestTrue(TEXT("AInfo fixture is abstract in the installed Engine"), AInfo::StaticClass()->HasAnyClassFlags(CLASS_Abstract));
    const FString ActorName = Fixture.Name(TEXT("s5_abstract_actor"));
    Fixture.Settings->ActorClassRegistry.Add(ActorName, FSoftClassPath(AInfo::StaticClass()));
    FMHCompositeDocument Document;
    FMHCompositeNode Random;
    Random.Kind = EMHCompositeNodeKind::Random;
    FMHCompositeOption Empty;
    Empty.Kind = EMHCompositeOptionKind::Empty;
    Empty.Weight = 1.0f;
    Random.Options.Add(Empty);
    FMHCompositeOption Abstract;
    Abstract.Kind = EMHCompositeOptionKind::Actor;
    Abstract.Resource = ActorName;
    Abstract.Weight = 0.0f;
    Random.Options.Add(Abstract);
    Document.Nodes.Add(Random);
    UMHCompositeAsset* Root = Fixture.Composite(Fixture.Name(TEXT("s5_abstract_root")), Document);
    if (Root == nullptr) return false;
    AMHCompositeActor* Actor = Fixture.Spawn(*Root);
    if (Actor == nullptr) return false;
    bool bPassed = Fixture.ExpectRejected(*Root, *Actor, TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE"));
    bPassed &= TestTrue(TEXT("refusal names the unselected abstract registry token"), Actor->GetLastPlacementError().Contains(ActorName));
    Fixture.Settings->ActorClassRegistry.Add(ActorName, FSoftClassPath(AStaticMeshActor::StaticClass()));
    Actor->RebuildComposite();
    if (!TestNotNull(TEXT("spawnable replacement admits the full closure"), Actor->GetResolvedPlan())) return false;
    bPassed &= TestTrue(TEXT("selected empty option emits no leaves"), Actor->GetResolvedPlan()->Leaves.IsEmpty());
    bPassed &= TestEqual(TEXT("zero-weight actor option was never selected"), Actor->GetResolvedPlan()->Decisions[0].OptionIndex, 0);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMHCompositeProspectiveEditPlanTest,
    "Mimir.V5.Composite.AppliedAdmission.EditPlacementBasisAndProspectivePlan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeProspectiveEditPlanTest::RunTest(const FString& Parameters)
{
    FAppliedAdmissionFixture Fixture(*this);
    UStaticMesh* Mesh = Fixture.Mesh(Fixture.Name(TEXT("s5_edit_mesh")));
    if (Mesh == nullptr) return false;
    FMHPlacementProfile Profile;
    Profile.LogicalName = Fixture.Name(TEXT("s5_edit_profile"));
    Profile.bHasOffsetCm = true;
    Profile.OffsetCm.SetNum(3);
    Profile.OffsetCm[0].Base = 10.0f;
    Profile.OffsetCm[0].Deviation = 5.0f;
    FString Error;
    TArray<uint8> ProfileBytes;
    if (!MHWriteCanonicalPlacementProfileV1(Profile, ProfileBytes, Error))
    {
        AddError(Error);
        return false;
    }
    Profile.SetAppliedSourceHash(MHRawPayloadHash(ProfileBytes));
    FMHCompositeDocument Document;
    FMHCompositeNode Group;
    Group.Transform.TranslationCm.X = 100.0;
    Group.Profile = Profile.LogicalName;
    FMHCompositeNode Child;
    Child.Kind = EMHCompositeNodeKind::Mesh;
    Child.Resource = Mesh->GetName();
    Child.Transform.TranslationCm.X = 25.0;
    Group.Children.Add(Child);
    Document.Nodes.Add(Group);
    UMHCompositeAsset* Root = Fixture.Composite(Fixture.Name(TEXT("s5_edit_root")), Document, {Profile});
    if (Root == nullptr) return false;
    AMHCompositeActor* Actor = Fixture.Spawn(*Root);
    if (Actor == nullptr || !TestNotNull(TEXT("Edit starts from admitted placement"), Actor->GetResolvedPlan())) return false;
    if (!TestEqual(TEXT("one authored root edit handle"), Actor->GetTopLevelPlacementComponents().Num(), 1) ||
        !TestEqual(TEXT("one handle plus one flattened leaf"), Actor->GetDerivedComponents().Num(), 2)) return false;
    const FMHResolvedCompositePlan AppliedPlan = *Actor->GetResolvedPlan();
    const FString SourceHash = Root->SourceHash;
    const FString AppliedHash = Root->AppliedHash;
    const FString ProfileHash = Root->InlinedPlacementProfiles[0].GetAppliedSourceHash();
    TArray<uint8> AppliedBytes;
    if (!MHWriteCanonicalCompositeV5(Document, AppliedBytes, Error)) return false;
    USceneComponent* Handle = Actor->GetTopLevelPlacementComponents()[0];
    USceneComponent* Leaf = Cast<USceneComponent>(Actor->GetDerivedComponents()[1]);
    if (!TestNotNull(TEXT("edit leaf is a scene component"), Leaf)) return false;

    Actor->SetPlacementEditMode(true);
    const FTransform MovedBasis(FRotator(0.0, 90.0, 0.0), FVector(1000.0, 200.0, 50.0));
    Actor->SetActorTransform(MovedBasis);
    Actor->Tick(0.0f);
    if (!TestNotNull(TEXT("placement move during Edit retains admitted prospective plan"), Actor->GetResolvedPlan())) return false;
    TestEqual(TEXT("moving placement during Edit does not change Seed"), Actor->GetSeed(), 100);
    TestEqual(TEXT("placement basis alone does not change signature"), Actor->GetResolvedPlan()->ResolvedSignature, AppliedPlan.ResolvedSignature);
    TestEqual(TEXT("placement basis alone does not change closure hash"), Actor->GetResolvedPlan()->Closure.ClosureHash, AppliedPlan.Closure.ClosureHash);
    TestTrue(TEXT("placement basis alone does not change raw closure receipts"),
        Actor->GetResolvedPlan()->Closure.OrderedRawHashes == AppliedPlan.Closure.OrderedRawHashes);
    TestEqual(TEXT("moving in Edit keeps authored handle object"), Actor->GetTopLevelPlacementComponents()[0].Get(), Handle);
    TestEqual(TEXT("moving in Edit keeps leaf object"), Actor->GetDerivedComponents()[1].Get(), static_cast<UActorComponent*>(Leaf));
    TestTrue(TEXT("authored handle follows placement basis without baking profile"), MHMatrixElementsWithinTrsTolerance(
        Handle->GetComponentTransform().ToMatrixWithScale(), FTransform(FVector(100.0, 0.0, 0.0)).ToMatrixWithScale() * MovedBasis.ToMatrixWithScale()));
    TestTrue(TEXT("resolved leaf follows placement basis"), MHMatrixElementsWithinTrsTolerance(
        Leaf->GetComponentTransform().ToMatrixWithScale(), AppliedPlan.Leaves[0].WorldMatrix * MovedBasis.ToMatrixWithScale()));
    TestEqual(TEXT("placement move preserves applied SourceHash"), Root->SourceHash, SourceHash);

    const FTransform EditedLocal(FVector(150.0, 0.0, 0.0));
    const FTransform SubmittedWorld(EditedLocal.ToMatrixWithScale() * MovedBasis.ToMatrixWithScale());
    Handle->SetWorldTransform(SubmittedWorld);
    // The rotated basis can leave tiny finite components after world->local.
    // Canonical expectation preserves the complete submitted host decomposition;
    // the test must not silently snap it back to an ideal translation-only TRS.
    const FTransform SubmittedLocal(Handle->GetComponentTransform().ToMatrixWithScale() *
        Actor->GetActorTransform().ToInverseMatrixWithScale());
    Actor->Tick(0.0f);
    if (!TestNotNull(TEXT("authored handle update produces prospective plan"), Actor->GetResolvedPlan())) return false;
    const FMHResolvedCompositePlan ProspectivePlan = *Actor->GetResolvedPlan();
    TestEqual(TEXT("prospective authored transform is not sampled profile transform"), ProspectivePlan.Nodes[0].AuthoredLocalTrs.TranslationCm.X, 150.0f);
    TestNotEqual(TEXT("authored Edit changes prospective signature"), ProspectivePlan.ResolvedSignature, AppliedPlan.ResolvedSignature);
    TestNotEqual(TEXT("authored Edit changes prospective closure hash"), ProspectivePlan.Closure.ClosureHash, AppliedPlan.Closure.ClosureHash);
    TestEqual(TEXT("authored Edit does not change applied SourceHash"), Root->SourceHash, SourceHash);
    TestEqual(TEXT("authored Edit does not change applied canonical hash"), Root->AppliedHash, AppliedHash);
    TestEqual(TEXT("authored Edit does not change inlined profile receipt"), Root->InlinedPlacementProfiles[0].GetAppliedSourceHash(), ProfileHash);
    FMHCompositeDocument Extracted;
    TArray<uint8> StillAppliedBytes;
    if (!MHExtractCompositeV5(*Root, Extracted, Error) || !MHWriteCanonicalCompositeV5(Extracted, StillAppliedBytes, Error)) return false;
    TestTrue(TEXT("Edit never mutates source-shaped applied asset"), StillAppliedBytes == AppliedBytes);
    FMHCompositeDocument ProspectiveDocument = Document;
    ProspectiveDocument.Nodes[0].Transform.TranslationCm = SubmittedLocal.GetTranslation();
    ProspectiveDocument.Nodes[0].Transform.RotationQuat = SubmittedLocal.GetRotation();
    ProspectiveDocument.Nodes[0].Transform.Scale = SubmittedLocal.GetScale3D();
    TArray<uint8> ProspectiveBytes;
    if (!MHWriteCanonicalCompositeV5(ProspectiveDocument, ProspectiveBytes, Error)) return false;
    const int32 RootHashIndex = ProspectivePlan.Closure.Resources.IndexOfByKey(TEXT("composite:") + Root->LogicalName);
    if (!TestTrue(TEXT("prospective closure still includes its root"), ProspectivePlan.Closure.OrderedRawHashes.IsValidIndex(RootHashIndex))) return false;
    TestEqual(TEXT("prospective root receipt hashes canonical edited source"),
        ProspectivePlan.Closure.OrderedRawHashes[RootHashIndex], MHRawPayloadHash(ProspectiveBytes));
    if (!TestEqual(TEXT("editing authored transform preserves draw count"), ProspectivePlan.Draws.Num(), AppliedPlan.Draws.Num())) return false;
    for (int32 Index = 0; Index < AppliedPlan.Draws.Num(); ++Index)
    {
        TestEqual(TEXT("editing transform does not reroll path-derived raw draw"), ProspectivePlan.Draws[Index].RawU32, AppliedPlan.Draws[Index].RawU32);
        TestEqual(TEXT("editing transform preserves profile sample"), ProspectivePlan.Draws[Index].Sample, AppliedPlan.Draws[Index].Sample);
    }

    Actor->SetPlacementEditMode(false);
    Actor->RebuildComposite();
    if (!TestNotNull(TEXT("Cancel plus Rebuild restores applied plan"), Actor->GetResolvedPlan())) return false;
    TestFalse(TEXT("Cancel exits transient Edit mode"), Actor->IsPlacementEditMode());
    TestEqual(TEXT("Cancel restores original signature despite moved placement"), Actor->GetResolvedPlan()->ResolvedSignature, AppliedPlan.ResolvedSignature);
    TestEqual(TEXT("Cancel restores original closure hash"), Actor->GetResolvedPlan()->Closure.ClosureHash, AppliedPlan.Closure.ClosureHash);
    TestEqual(TEXT("Cancel never rewrote source receipt"), Root->SourceHash, SourceHash);
    TestTrue(TEXT("Cancel restores authored handle in current placement basis"), MHMatrixElementsWithinTrsTolerance(
        Actor->GetTopLevelPlacementComponents()[0]->GetComponentTransform().ToMatrixWithScale(),
        FTransform(FVector(100.0, 0.0, 0.0)).ToMatrixWithScale() * MovedBasis.ToMatrixWithScale()));
    return true;
}

} // namespace UE::MimirComposite::Tests
