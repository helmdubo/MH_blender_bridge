#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Source/MHPayloadHashes.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UI/MHCompositeOutlinerModel.h"
#include "UObject/Package.h"

namespace UE::MimirComposite::Tests
{
namespace
{
struct FMHISMMaterializationFixture
{
    FAutomationTestBase& Test;
    TArray<UObject*> Assets;
    UStaticMesh* Mesh = nullptr;
    UMHCompositeAsset* Composite = nullptr;

    explicit FMHISMMaterializationFixture(FAutomationTestBase& InTest) : Test(InTest) {}

    ~FMHISMMaterializationFixture()
    {
        for (UObject* Object : Assets)
        {
            if (!IsValid(Object)) continue;
            Object->ClearFlags(RF_Public | RF_Standalone);
            Object->MarkAsGarbage();
        }
    }

    bool Build(const int32 LeafCount, const bool bRandomLeaves = false)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
        const FString MeshName = TEXT("u5_ism_mesh_") + Suffix;
        const FString CompositeName = TEXT("u5_ism_root_") + Suffix;
        Mesh = NewObject<UStaticMesh>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Meshes/") + MeshName)),
            FName(*MeshName), RF_Public | RF_Standalone);
        Assets.Add(Mesh);
        UMHStaticMeshImportData* Receipt = NewObject<UMHStaticMeshImportData>(Mesh);
        Receipt->LogicalName = MeshName;
        Receipt->SourceRelativePath = MeshName + TEXT(".mesh.fbx");
        Receipt->SourceHash = TEXT("blake3-160:0123456789012345678901234567890123456789");
        Receipt->ImporterVersion = MHStaticMeshImporterVersion;
        Mesh->SetAssetImportData(Receipt);

        Composite = NewObject<UMHCompositeAsset>(
            CreatePackage(*(TEXT("/Game/MH/Generated/Composites/") + CompositeName)),
            FName(*CompositeName), RF_Public | RF_Standalone);
        Assets.Add(Composite);
        FMHCompositeDocument Document;
        for (int32 Index = 0; Index < LeafCount; ++Index)
        {
            FMHCompositeNode& Leaf = Document.Nodes.AddDefaulted_GetRef();
            Leaf.Kind = bRandomLeaves
                ? EMHCompositeNodeKind::Random : EMHCompositeNodeKind::Mesh;
            if (bRandomLeaves)
            {
                FMHCompositeOption& Option = Leaf.Options.AddDefaulted_GetRef();
                Option.Kind = EMHCompositeOptionKind::Mesh;
                Option.Resource = MeshName;
                Option.Weight = 1.0f;
            }
            else
            {
                Leaf.Resource = MeshName;
            }
            Leaf.Transform.TranslationCm = FVector(25.0 * Index, 0.0, 0.0);
        }
        FString Error;
        TArray<uint8> Bytes;
        if (!MHApplyCompositeV5(*Composite, Document, Error) ||
            !MHWriteCanonicalCompositeV5(Document, Bytes, Error))
        {
            Test.AddError(Error);
            return false;
        }
        Composite->LogicalName = CompositeName;
        Composite->SourceRelativePath = CompositeName + TEXT(".composite");
        Composite->SourceHash = MHRawPayloadHash(Bytes);
        Composite->AppliedHash = Composite->SourceHash;
        return true;
    }
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeISMBucketMaterializationTest,
    "Mimir.V5.Composite.ISM.BucketsIdenticalStaticLeaves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeISMBucketMaterializationTest::RunTest(const FString& Parameters)
{
    constexpr int32 LeafCount = 12;
    FMHISMMaterializationFixture Fixture(*this);
    if (!Fixture.Build(LeafCount)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetAutoSeed(false);
    Actor->SetAutoAppearanceSeed(false);
    Actor->SetCompositeAsset(Fixture.Composite);

    // 16 §2.8 (R5b-1): buckets live on the level pool, not on the actor; the
    // rows name the bucket each leaf renders in.
    TArray<UInstancedStaticMeshComponent*> Buckets;
    for (const FMHCompositeLeafMaterialization& Row : Actor->GetLeafMaterializations())
    {
        if (UInstancedStaticMeshComponent* Bucket = Cast<UInstancedStaticMeshComponent>(Row.Component.Get()))
            Buckets.AddUnique(Bucket);
    }
    bool bPassed = TestEqual(TEXT("identical static leaves share one ISM bucket"), Buckets.Num(), 1);
    if (Buckets.Num() == 1)
    {
        bPassed &= TestEqual(TEXT("bucket contains every resolved leaf"),
            Buckets[0]->GetInstanceCount(), LeafCount);
    }
    bPassed &= TestEqual(TEXT("leaf compatibility view remains plan-aligned"),
        Actor->GetLeafPlacementComponents().Num(), LeafCount);
    const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
    bPassed &= TestNotNull(TEXT("resolved plan"), Plan);
    const TArray<FMHCompositeLeafMaterialization>& Rows = Actor->GetLeafMaterializations();
    bPassed &= TestEqual(TEXT("instance mapping remains plan-aligned"), Rows.Num(), LeafCount);
    TSet<int32> InstanceIndices;
    if (Buckets.Num() == 1 && Plan != nullptr && Rows.Num() == LeafCount)
    {
        for (int32 Index = 0; Index < LeafCount; ++Index)
        {
            const FMHCompositeLeafMaterialization& Row = Rows[Index];
            bPassed &= TestEqual(TEXT("mapping keeps the shared bucket"),
                Row.Component.Get(), static_cast<USceneComponent*>(Buckets[0]));
            bPassed &= TestTrue(TEXT("mapping has a unique instance index"),
                Row.InstanceIndex != INDEX_NONE && !InstanceIndices.Contains(Row.InstanceIndex));
            InstanceIndices.Add(Row.InstanceIndex);
            bPassed &= TestEqual(TEXT("mapping keeps ResolvedNodeIndex"),
                Row.ResolvedNodeIndex, Plan->Leaves[Index].OwningResolvedNodeIndex);
            bPassed &= TestEqual(TEXT("mapping keeps NodePath"),
                Row.NodePath, Plan->Leaves[Index].Origin);
            FTransform InstanceTransform;
            bPassed &= TestTrue(TEXT("instance transform can be read"),
                Buckets[0]->GetInstanceTransform(Row.InstanceIndex, InstanceTransform, false));
            bPassed &= TestTrue(TEXT("instance uses the admitted full world matrix"),
                InstanceTransform.ToMatrixWithScale().Equals(
                    Plan->Leaves[Index].WorldMatrix * Actor->GetActorTransform().ToMatrixWithScale(),
                    0.0));
        }

        FMHCompositeOutlinerModel Model;
        bPassed &= TestTrue(TEXT("Outliner model builds from an instanced placement"),
            Model.BuildFromActor(*Actor));
        const int32 Probe = LeafCount / 2;
        TSharedPtr<FMHCompositeOutlinerItem> Item =
            Model.FindForInstance(Buckets[0], Rows[Probe].InstanceIndex);
        bPassed &= TestTrue(TEXT("ISM instance selects the exact Outliner row"),
            Item.IsValid() && Item->NodePath == Plan->Leaves[Probe].Origin);
        bPassed &= TestTrue(TEXT("Outliner row selects the exact actor mapping"),
            Actor->SelectPlacementLeafByNodePath(Plan->Leaves[Probe].Origin) &&
            Actor->GetSelectedPlacementLeafPath() == Plan->Leaves[Probe].Origin);

        const FString EditedPath = Plan->Leaves[Probe].Origin;
        Actor->SetPlacementEditMode(true);
        const TArray<FMHCompositeLeafMaterialization>& EditRows = Actor->GetLeafMaterializations();
        const FMHCompositeLeafMaterialization* Extracted = EditRows.FindByPredicate(
            [&EditedPath](const FMHCompositeLeafMaterialization& Row)
            {
                return Row.NodePath == EditedPath;
            });
        bPassed &= TestTrue(TEXT("selected edit leaf is extracted to an ordinary SMC"),
            Extracted != nullptr && Extracted->InstanceIndex == INDEX_NONE &&
            Cast<UInstancedStaticMeshComponent>(Extracted->Component) == nullptr &&
            Cast<UStaticMeshComponent>(Extracted->Component) != nullptr);
        int32 InstancedDuringEdit = 0;
        for (const FMHCompositeLeafMaterialization& Row : EditRows)
            if (Row.IsInstanced()) ++InstancedDuringEdit;
        bPassed &= TestEqual(TEXT("all unselected leaves remain instanced in Edit Mode"),
            InstancedDuringEdit, LeafCount - 1);
        Actor->SetPlacementEditMode(false);
        int32 InstancedAfterEdit = 0;
        for (const FMHCompositeLeafMaterialization& Row : Actor->GetLeafMaterializations())
            if (Row.IsInstanced()) ++InstancedAfterEdit;
        bPassed &= TestEqual(TEXT("Edit Mode exit returns the leaf to its bucket"),
            InstancedAfterEdit, LeafCount);
    }

    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeResidentPlanTest,
    "Mimir.V5.Composite.ResidentPlan.SingleAuthority",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeResidentPlanTest::RunTest(const FString& Parameters)
{
    FMHISMMaterializationFixture Fixture(*this);
    if (!Fixture.Build(12, true)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetAutoSeed(false);
    Actor->SetAutoAppearanceSeed(false);
    Actor->SetSeed(1729);
    Actor->SetAppearanceSeed(2718);
    Actor->SetCompositeAsset(Fixture.Composite);

    // R2b-3: the preview plan is resident (§2.10 "LastPlacements") and is the
    // only plan the actor owns: no compact signed state, no lazy debug copy.
    const FMHResolvedCompositePlan* Plan = Actor->GetResolvedPlan();
    bool bPassed = TestNotNull(TEXT("placement exposes its resident preview plan"), Plan);
    if (Plan != nullptr)
    {
        bPassed &= TestEqual(TEXT("resident plan keeps every materialized leaf"), Plan->Leaves.Num(), 12);
        bPassed &= TestEqual(TEXT("resident plan keeps every decision"), Plan->Decisions.Num(), 12);
        bPassed &= TestTrue(TEXT("resident plan contains the layout diagnostic trace"),
            !Plan->Decisions.IsEmpty() && !Plan->Draws.IsEmpty() && !Plan->SignaturePreimage.IsEmpty());
        bPassed &= TestTrue(TEXT("resident plan contains the appearance trace"),
            !Plan->Appearance.Draws.IsEmpty() && !Plan->Appearance.SignaturePreimage.IsEmpty());
        bPassed &= TestTrue(TEXT("preview plan carries no closure or signature"),
            Plan->ResolvedSignature.IsEmpty() && Plan->Appearance.AppearanceSignature.IsEmpty() &&
            Plan->PlacementSignature.IsEmpty() && Plan->Closure.Resources.IsEmpty());
        bPassed &= TestTrue(TEXT("repeated reads return the same resident object"), Actor->GetResolvedPlan() == Plan);
        bPassed &= TestTrue(TEXT("preview revision advanced"), Actor->GetPreviewRevision() >= 1u);
    }

    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHCompositeISMBucketPolicyAdmissionTest,
    "Mimir.V5.Composite.ISM.BucketPolicyRejectsMutatedReuse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHCompositeISMBucketPolicyAdmissionTest::RunTest(const FString& Parameters)
{
    FMHISMMaterializationFixture Fixture(*this);
    if (!Fixture.Build(3)) return false;
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    AMHCompositeActor* Actor = World->SpawnActor<AMHCompositeActor>();
    Actor->SetAutoSeed(false);
    Actor->SetAutoAppearanceSeed(false);
    Actor->SetCompositeAsset(Fixture.Composite);

    const auto FindBucket = [Actor]() -> UInstancedStaticMeshComponent*
    {
        for (const FMHCompositeLeafMaterialization& Row : Actor->GetLeafMaterializations())
            if (UInstancedStaticMeshComponent* Bucket =
                    Cast<UInstancedStaticMeshComponent>(Row.Component.Get())) return Bucket;
        return nullptr;
    };
    struct FPolicyMutation
    {
        const TCHAR* Label;
        TFunction<void(UInstancedStaticMeshComponent&)> Apply;
    };
    const TArray<FPolicyMutation> Mutations{
        {TEXT("ordered material overrides"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.OverrideMaterials.Add(nullptr); }},
        {TEXT("collision profile policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName); }},
        {TEXT("collision enabled policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.SetCollisionEnabled(Bucket.GetCollisionEnabled() == ECollisionEnabled::NoCollision
                ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision); }},
        {TEXT("collision object policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.SetCollisionObjectType(Bucket.GetCollisionObjectType() == ECC_WorldStatic
                ? ECC_WorldDynamic : ECC_WorldStatic); }},
        {TEXT("collision response policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore); }},
        {TEXT("overlap generation policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.SetGenerateOverlapEvents(!Bucket.GetGenerateOverlapEvents()); }},
        {TEXT("complex trace policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.bTraceComplexOnMove = !Bucket.bTraceComplexOnMove; }},
        {TEXT("physical material return policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.bReturnMaterialOnMove = !Bucket.bReturnMaterialOnMove; }},
        {TEXT("cast-shadow policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.CastShadow = !Bucket.CastShadow; }},
        {TEXT("distance-field policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.bAffectDistanceFieldLighting = !Bucket.bAffectDistanceFieldLighting; }},
        {TEXT("ray-tracing visibility policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.bVisibleInRayTracing = !Bucket.bVisibleInRayTracing; }},
        {TEXT("mobility policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.SetMobility(Bucket.Mobility == EComponentMobility::Movable
                ? EComponentMobility::Static : EComponentMobility::Movable); }},
        {TEXT("editor visibility policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.SetVisibility(false); }},
        {TEXT("game visibility policy"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.SetHiddenInGame(true); }},
        {TEXT("appearance custom-data layout"), [](UInstancedStaticMeshComponent& Bucket)
            { Bucket.SetNumCustomDataFloats(Bucket.NumCustomDataFloats + 1); }},
    };

    bool bPassed = true;
    for (const FPolicyMutation& Mutation : Mutations)
    {
        UInstancedStaticMeshComponent* Previous = FindBucket();
        if (!TestNotNull(*FString::Printf(TEXT("%s has a source bucket"), Mutation.Label),
                Previous))
        {
            bPassed = false;
            break;
        }
        Mutation.Apply(*Previous);
        Actor->RebuildComposite();
        UInstancedStaticMeshComponent* Current = FindBucket();
        bPassed &= TestNotNull(
            *FString::Printf(TEXT("%s rebuild has a bucket"), Mutation.Label), Current);
        if (Current == nullptr) break;
        // R5b-1a: the pool retires a component that drifted from its descriptor.
        bPassed &= TestNotEqual(
            *FString::Printf(TEXT("%s cannot reuse a mismatched bucket"), Mutation.Label),
            Current, Previous);
        bPassed &= TestEqual(
            *FString::Printf(TEXT("%s rebuild preserves all instances"), Mutation.Label),
            Current->GetInstanceCount(), 3);
    }

    Actor->Destroy();
    World->DestroyWorld(false);
    return bPassed;
}
} // namespace UE::MimirComposite::Tests
