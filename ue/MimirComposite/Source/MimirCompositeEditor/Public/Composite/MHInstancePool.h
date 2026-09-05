#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/Actor.h"
#include "Random/MHRandomStream.h"
#include "Subsystems/WorldSubsystem.h"
#include "MHInstancePool.generated.h"

class ULevel;
class UStaticMesh;
class UMaterialInterface;

namespace UE::MimirComposite
{

/** One ISM section policy of a bucket descriptor (matches the placement compiler's view). */
struct MIMIRCOMPOSITEEDITOR_API FMHPoolSectionPolicy
{
    int32 MaterialIndex = INDEX_NONE;
    bool bEnableCollision = false;
    bool bCastShadow = false;
    bool bVisibleInRayTracing = false;
    bool bAffectDistanceFieldLighting = false;
    bool bForceOpaque = false;
    bool operator==(const FMHPoolSectionPolicy& Other) const = default;
};

/**
 * Complete bucket identity (16 §2.8): every field that makes two ISM
 * instances compatible on one component. Never remove a field without a new
 * contract; the placement compiler's private key is the same set of fields.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHPoolBucketDescriptor
{
    TObjectPtr<UStaticMesh> StaticMesh = nullptr;
    TArray<TObjectPtr<UMaterialInterface>> MaterialOverrides;
    FName CollisionProfileName;
    ECollisionEnabled::Type CollisionEnabled = ECollisionEnabled::NoCollision;
    ECollisionChannel CollisionObjectType = ECC_WorldStatic;
    FCollisionResponseContainer CollisionResponses;
    bool bGenerateOverlapEvents = false;
    bool bTraceComplexOnMove = false;
    bool bReturnMaterialOnMove = false;
    bool bCastShadow = false;
    bool bAffectDistanceFieldLighting = false;
    bool bVisibleInRayTracing = false;
    TArray<FMHPoolSectionPolicy> Sections;
    EComponentMobility::Type Mobility = EComponentMobility::Static;
    bool bVisible = true;
    bool bHiddenInGame = false;
    /** NumCustomDataFloats of the bucket; the appearance channels live at AppearanceCustomDataBaseIndex. */
    int32 AppearanceLayout = 0;
    int32 AppearanceCustomDataBaseIndex = 0;

    bool operator==(const FMHPoolBucketDescriptor& Other) const = default;

    /** ISM component defaults for a mesh, as the placement compiler derives them. */
    static FMHPoolBucketDescriptor FromMesh(UStaticMesh& Mesh, int32 AppearanceLayout, int32 AppearanceCustomDataBaseIndex);
};

/**
 * Stable user handle of one pooled instance (16 §2.8). Never (Component*,
 * InstanceIndex): the ISM index is the address of the current representation
 * and moves on swap-remove; the handle does not. A stale handle (removed slot,
 * or a slot reused by a later Add) is rejected by Generation.
 */
struct MIMIRCOMPOSITEEDITOR_API FMHInstanceHandle
{
    int32 BucketId = INDEX_NONE;
    int32 SlotId = INDEX_NONE;
    uint32 Generation = 0;

    bool IsSet() const { return BucketId != INDEX_NONE && SlotId != INDEX_NONE; }
    bool operator==(const FMHInstanceHandle& Other) const = default;
};

/** Observable counters for tests and MH_PERF reports. */
struct MIMIRCOMPOSITEEDITOR_API FMHInstancePoolMetrics
{
    uint64 BucketsCreated = 0;
    uint64 BucketsMigrated = 0;
    uint64 RenderStateRefreshes = 0;
    uint64 PhysicsRefreshes = 0;
    uint64 InstancesAdded = 0;
    uint64 InstancesRemoved = 0;
};

} // namespace UE::MimirComposite

/** Transient service actor holding the ISM buckets of one ULevel (16 §2.8, first implementation). */
UCLASS(NotBlueprintable, NotPlaceable, Transient)
class MIMIRCOMPOSITEEDITOR_API AMHInstancePoolActor final : public AActor
{
    GENERATED_BODY()

public:
    AMHInstancePoolActor();
};

/**
 * World-level instance pool (Recipe Model v2 §2.8, R5a). Owns ISM buckets per
 * {ULevel, descriptor} and hands out stable handles; owners (composite
 * placements) never touch (Component*, InstanceIndex). Editor worlds only.
 *
 * R5a delivers the service and its invariants; R5b moves the placement
 * compiler, Outliner and Undo onto it.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHInstancePoolSubsystem final : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void Deinitialize() override;

    static UMHInstancePoolSubsystem* Get(const UWorld* World);

    /** Adds one instance for Owner/NodePath into the bucket of Level x Descriptor. Unset handle on failure. */
    UE::MimirComposite::FMHInstanceHandle Add(
        AActor& Owner,
        const FString& NodePath,
        ULevel& Level,
        const UE::MimirComposite::FMHPoolBucketDescriptor& Descriptor,
        const FMatrix& WorldMatrix,
        const float (&AppearanceChannels)[UE::MimirComposite::MH_APPEARANCE_CHANNELS]);
    bool Update(const UE::MimirComposite::FMHInstanceHandle& Handle, const FMatrix& WorldMatrix);
    bool UpdateAppearance(const UE::MimirComposite::FMHInstanceHandle& Handle, const float (&AppearanceChannels)[UE::MimirComposite::MH_APPEARANCE_CHANNELS]);
    /** Swap-remove inside the ISM; every other handle of the bucket stays valid. */
    bool Remove(const UE::MimirComposite::FMHInstanceHandle& Handle);
    bool IsValidHandle(const UE::MimirComposite::FMHInstanceHandle& Handle) const;

    /** Hit proxy / Outliner seam: which owner and node path a live ISM instance belongs to. */
    bool ReverseLookup(const UInstancedStaticMeshComponent* Component, int32 InstanceIndex, AActor*& OutOwner, FString& OutNodePath) const;
    /** The ISM instance currently rendering a handle (INDEX_NONE while hidden). */
    bool GetInstance(const UE::MimirComposite::FMHInstanceHandle& Handle, UInstancedStaticMeshComponent*& OutComponent, int32& OutInstanceIndex) const;

    /** One render-state and physics refresh per touched bucket for the whole scope. */
    void BeginBulk();
    void EndBulk();

    /** Owner operations: an ISM component's SetVisibility() is unusable here, it hides every owner. */
    void HideOwner(const AActor& Owner);
    void ShowOwner(const AActor& Owner);
    void RemoveOwner(const AActor& Owner);
    /** Applies Delta (world space) to every instance of Owner. */
    void MoveOwner(const AActor& Owner, const FMatrix& Delta);
    void SetOwnerEditorVisibility(const AActor& Owner, bool bVisible) { bVisible ? ShowOwner(Owner) : HideOwner(Owner); }

    /**
     * Reimport reconcile for every bucket rendering Mesh (16 §4, R5b-0):
     * payload/bounds -> render + bounds refresh; bucket descriptor -> the
     * bucket migrates to a new ISM configured from the mesh's current
     * interface, every handle survives (hidden ones stay hidden); collision
     * -> physics state recreated; material binding -> override materials
     * reset. Returns the number of buckets touched; an empty delta touches none.
     */
    int32 ReconcileMesh(const UStaticMesh& Mesh, const UE::MimirComposite::FMHEndpointInterfaceDelta& Delta);

    /** Live bucket components rendering Mesh (any level), in bucket order. */
    void GetBucketComponents(const UStaticMesh& Mesh, TArray<UInstancedStaticMeshComponent*>& OutComponents) const;

    /**
     * Editor selection highlight of one owner (R5b-2a): only that owner's
     * live instances are marked selected on the shared ISM; the state
     * survives Hide/Show and bucket migration. The pool mirrors the editor's
     * actor selection onto its owners automatically.
     */
    void SetOwnerSelected(const AActor& Owner, bool bSelected);
    bool IsOwnerSelected(const AActor& Owner) const;
    /** World-space bounds of the owner's live instances (mesh bounds under each instance matrix); invalid box when none. */
    FBox GetOwnerBounds(const AActor& Owner) const;

    int32 NumBuckets() const { return Buckets.Num(); }
    int32 NumLiveInstances(const AActor& Owner) const;
    const UE::MimirComposite::FMHInstancePoolMetrics& GetMetrics() const { return Metrics; }
    void ResetMetricsForTests() { Metrics = UE::MimirComposite::FMHInstancePoolMetrics(); }

private:
    struct FSlot
    {
        TWeakObjectPtr<AActor> Owner;
        FString NodePath;
        uint32 Generation = 0;
        /** Instance index inside the ISM, INDEX_NONE while hidden or free. */
        int32 InstanceIndex = INDEX_NONE;
        bool bFree = true;
        bool bHidden = false;
        FMatrix WorldMatrix = FMatrix::Identity;
        float Appearance[UE::MimirComposite::MH_APPEARANCE_CHANNELS] = {};
    };
    struct FBucket
    {
        TWeakObjectPtr<ULevel> Level;
        UE::MimirComposite::FMHPoolBucketDescriptor Descriptor;
        TWeakObjectPtr<UInstancedStaticMeshComponent> Component;
        TArray<FSlot> Slots;
        TArray<int32> FreeSlots;
        /** ISM instance index -> SlotId (the reverse map that swap-remove keeps in step). */
        TArray<int32> InstanceToSlot;
        bool bDirty = false;
        bool bPhysicsDirty = false;
    };

    int32 FindOrCreateBucket(ULevel& Level, const UE::MimirComposite::FMHPoolBucketDescriptor& Descriptor);
    AMHInstancePoolActor* FindOrCreatePoolActor(ULevel& Level);
    FBucket* ResolveHandle(const UE::MimirComposite::FMHInstanceHandle& Handle, FSlot*& OutSlot);
    const FBucket* ResolveHandle(const UE::MimirComposite::FMHInstanceHandle& Handle, const FSlot*& OutSlot) const;
    void RemoveInstanceFromComponent(FBucket& Bucket, int32 SlotId);
    void AddInstanceToComponent(FBucket& Bucket, int32 SlotId);
    void MarkDirty(FBucket& Bucket, bool bPhysics);
    void Flush(FBucket& Bucket);
    bool MigrateBucket(FBucket& Bucket);

    TArray<FBucket> Buckets;
    TMap<TWeakObjectPtr<ULevel>, TWeakObjectPtr<AMHInstancePoolActor>> PoolActors;
    int32 BulkDepth = 0;
    UE::MimirComposite::FMHInstancePoolMetrics Metrics;
};
