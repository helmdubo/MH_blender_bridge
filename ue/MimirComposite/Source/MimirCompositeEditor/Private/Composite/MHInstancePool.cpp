#include "Composite/MHInstancePool.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

using namespace UE::MimirComposite;

namespace UE::MimirComposite
{

FMHPoolBucketDescriptor FMHPoolBucketDescriptor::FromMesh(
    UStaticMesh& Mesh, const int32 InAppearanceLayout, const int32 InAppearanceCustomDataBaseIndex)
{
    // Mirrors the placement compiler's default bucket key: ISM class defaults
    // plus the mesh's section policies; synthetic meshes without a body setup
    // never collide (the stock ISM would assert creating an instance body).
    const UInstancedStaticMeshComponent* Defaults = GetDefault<UInstancedStaticMeshComponent>();
    FMHPoolBucketDescriptor Descriptor;
    Descriptor.StaticMesh = &Mesh;
    Descriptor.MaterialOverrides = Defaults->OverrideMaterials;
    Descriptor.CollisionProfileName = Defaults->GetCollisionProfileName();
    Descriptor.CollisionEnabled = Defaults->GetCollisionEnabled();
    if (Mesh.GetBodySetup() == nullptr)
    {
        Descriptor.CollisionEnabled = ECollisionEnabled::NoCollision;
        Descriptor.CollisionProfileName = UCollisionProfile::CustomCollisionProfileName;
    }
    Descriptor.CollisionObjectType = Defaults->GetCollisionObjectType();
    Descriptor.CollisionResponses = Defaults->GetCollisionResponseToChannels();
    Descriptor.bGenerateOverlapEvents = Defaults->GetGenerateOverlapEvents();
    Descriptor.bTraceComplexOnMove = Defaults->bTraceComplexOnMove;
    Descriptor.bReturnMaterialOnMove = Defaults->bReturnMaterialOnMove;
    Descriptor.bCastShadow = Defaults->CastShadow;
    Descriptor.bAffectDistanceFieldLighting = Defaults->bAffectDistanceFieldLighting;
    Descriptor.bVisibleInRayTracing = Defaults->bVisibleInRayTracing;
    Descriptor.Mobility = Defaults->Mobility;
    Descriptor.bVisible = Defaults->IsVisible();
    Descriptor.bHiddenInGame = Defaults->bHiddenInGame;
    Descriptor.AppearanceLayout = InAppearanceLayout;
    Descriptor.AppearanceCustomDataBaseIndex = InAppearanceCustomDataBaseIndex;
    for (int32 LodIndex = 0; LodIndex < Mesh.GetNumLODs(); ++LodIndex)
    {
        for (int32 SectionIndex = 0; SectionIndex < Mesh.GetNumSections(LodIndex); ++SectionIndex)
        {
            const FMeshSectionInfo Info = Mesh.GetSectionInfoMap().Get(LodIndex, SectionIndex);
            Descriptor.Sections.Add({Info.MaterialIndex, Info.bEnableCollision, Info.bCastShadow,
                Info.bVisibleInRayTracing, Info.bAffectDistanceFieldLighting, Info.bForceOpaque});
        }
    }
    return Descriptor;
}

} // namespace UE::MimirComposite

namespace
{
constexpr EObjectFlags PoolObjectFlags = RF_Transient | RF_DuplicateTransient | RF_TextExportTransient;

/** Applies a descriptor to a bucket component; the pool always swap-removes and owns per-instance hit proxies. */
void PoolConfigureBucket(UInstancedStaticMeshComponent& Component, const FMHPoolBucketDescriptor& Descriptor)
{
    Component.SetStaticMesh(Descriptor.StaticMesh);
    Component.OverrideMaterials = Descriptor.MaterialOverrides;
    Component.SetCollisionProfileName(Descriptor.CollisionProfileName);
    Component.SetCollisionEnabled(Descriptor.CollisionEnabled);
    Component.SetCollisionObjectType(Descriptor.CollisionObjectType);
    Component.SetCollisionResponseToChannels(Descriptor.CollisionResponses);
    Component.SetGenerateOverlapEvents(Descriptor.bGenerateOverlapEvents);
    Component.bTraceComplexOnMove = Descriptor.bTraceComplexOnMove;
    Component.bReturnMaterialOnMove = Descriptor.bReturnMaterialOnMove;
    Component.CastShadow = Descriptor.bCastShadow;
    Component.bAffectDistanceFieldLighting = Descriptor.bAffectDistanceFieldLighting;
    Component.bVisibleInRayTracing = Descriptor.bVisibleInRayTracing;
    Component.SetMobility(Descriptor.Mobility);
    Component.SetVisibility(Descriptor.bVisible);
    Component.SetHiddenInGame(Descriptor.bHiddenInGame);
    Component.bHasPerInstanceHitProxies = true;
    Component.bSupportRemoveAtSwap = true;
    Component.SetNumCustomDataFloats(Descriptor.AppearanceLayout);
}

bool PoolAppearanceFits(const FMHPoolBucketDescriptor& Descriptor)
{
    return Descriptor.AppearanceCustomDataBaseIndex >= 0 &&
        Descriptor.AppearanceCustomDataBaseIndex + MH_APPEARANCE_CHANNELS <= Descriptor.AppearanceLayout;
}
} // namespace

AMHInstancePoolActor::AMHInstancePoolActor()
{
    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("PoolRoot"));
    Root->SetMobility(EComponentMobility::Static);
    RootComponent = Root;
    bIsEditorOnlyActor = true;
#if WITH_EDITORONLY_DATA
    bListedInSceneOutliner = false;
#endif
    SetActorHiddenInGame(true);
}

bool UMHInstancePoolSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    const UWorld* World = Cast<UWorld>(Outer);
    return World != nullptr && (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::EditorPreview);
}

void UMHInstancePoolSubsystem::Deinitialize()
{
    Buckets.Reset();
    PoolActors.Reset();
    Super::Deinitialize();
}

UMHInstancePoolSubsystem* UMHInstancePoolSubsystem::Get(const UWorld* World)
{
    return World != nullptr ? World->GetSubsystem<UMHInstancePoolSubsystem>() : nullptr;
}

FMHInstanceHandle UMHInstancePoolSubsystem::Add(
    AActor& Owner, const FString& NodePath, ULevel& Level, const FMHPoolBucketDescriptor& Descriptor,
    const FMatrix& WorldMatrix, const float (&AppearanceChannels)[UE::MimirComposite::MH_APPEARANCE_CHANNELS])
{
    if (Descriptor.StaticMesh == nullptr) return FMHInstanceHandle();
    const int32 BucketId = FindOrCreateBucket(Level, Descriptor);
    if (BucketId == INDEX_NONE) return FMHInstanceHandle();
    FBucket& Bucket = Buckets[BucketId];

    int32 SlotId = INDEX_NONE;
    if (Bucket.FreeSlots.Num() > 0)
    {
        SlotId = Bucket.FreeSlots.Pop(EAllowShrinking::No);
    }
    else
    {
        SlotId = Bucket.Slots.AddDefaulted();
    }
    FSlot& Slot = Bucket.Slots[SlotId];
    // A reused slot advances its generation, so the handle of the removed
    // instance stays dead; a fresh slot starts at 1 (0 = never issued).
    Slot.Generation = FMath::Max<uint32>(1u, Slot.Generation + 1u);
    Slot.Owner = &Owner;
    Slot.NodePath = NodePath;
    Slot.bFree = false;
    Slot.bHidden = false;
    Slot.WorldMatrix = WorldMatrix;
    for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel) Slot.Appearance[Channel] = AppearanceChannels[Channel];
    AddInstanceToComponent(Bucket, SlotId);
    ++Metrics.InstancesAdded;
    MarkDirty(Bucket, true);

    FMHInstanceHandle Handle;
    Handle.BucketId = BucketId;
    Handle.SlotId = SlotId;
    Handle.Generation = Slot.Generation;
    return Handle;
}

bool UMHInstancePoolSubsystem::Update(const FMHInstanceHandle& Handle, const FMatrix& WorldMatrix)
{
    FSlot* Slot = nullptr;
    FBucket* Bucket = ResolveHandle(Handle, Slot);
    if (Bucket == nullptr) return false;
    Slot->WorldMatrix = WorldMatrix;
    if (!Slot->bHidden)
    {
        UInstancedStaticMeshComponent* Component = Bucket->Component.Get();
        if (Component == nullptr || !Component->UpdateInstanceTransform(Slot->InstanceIndex, FTransform(WorldMatrix), true, false, true)) return false;
        MarkDirty(*Bucket, false);
    }
    return true;
}

bool UMHInstancePoolSubsystem::UpdateAppearance(const FMHInstanceHandle& Handle, const float (&AppearanceChannels)[UE::MimirComposite::MH_APPEARANCE_CHANNELS])
{
    FSlot* Slot = nullptr;
    FBucket* Bucket = ResolveHandle(Handle, Slot);
    if (Bucket == nullptr) return false;
    for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel) Slot->Appearance[Channel] = AppearanceChannels[Channel];
    if (!Slot->bHidden)
    {
        UInstancedStaticMeshComponent* Component = Bucket->Component.Get();
        if (Component == nullptr || !PoolAppearanceFits(Bucket->Descriptor)) return false;
        const int32 Base = Bucket->Descriptor.AppearanceCustomDataBaseIndex;
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
        {
            Component->SetCustomDataValue(Slot->InstanceIndex, Base + Channel, AppearanceChannels[Channel], false);
        }
        MarkDirty(*Bucket, false);
    }
    return true;
}

bool UMHInstancePoolSubsystem::Remove(const FMHInstanceHandle& Handle)
{
    FSlot* Slot = nullptr;
    FBucket* Bucket = ResolveHandle(Handle, Slot);
    if (Bucket == nullptr) return false;
    if (!Slot->bHidden) RemoveInstanceFromComponent(*Bucket, Handle.SlotId);
    Slot->bFree = true;
    Slot->bHidden = false;
    Slot->Owner.Reset();
    Slot->NodePath.Reset();
    Slot->InstanceIndex = INDEX_NONE;
    Bucket->FreeSlots.Push(Handle.SlotId);
    ++Metrics.InstancesRemoved;
    MarkDirty(*Bucket, true);
    return true;
}

bool UMHInstancePoolSubsystem::IsValidHandle(const FMHInstanceHandle& Handle) const
{
    const FSlot* Slot = nullptr;
    return ResolveHandle(Handle, Slot) != nullptr;
}

bool UMHInstancePoolSubsystem::ReverseLookup(const UInstancedStaticMeshComponent* Component, const int32 InstanceIndex, AActor*& OutOwner, FString& OutNodePath) const
{
    OutOwner = nullptr;
    OutNodePath.Reset();
    if (Component == nullptr || InstanceIndex < 0) return false;
    for (const FBucket& Bucket : Buckets)
    {
        if (Bucket.Component.Get() != Component) continue;
        if (!Bucket.InstanceToSlot.IsValidIndex(InstanceIndex)) return false;
        const FSlot& Slot = Bucket.Slots[Bucket.InstanceToSlot[InstanceIndex]];
        if (Slot.bFree || Slot.bHidden) return false;
        OutOwner = Slot.Owner.Get();
        OutNodePath = Slot.NodePath;
        return OutOwner != nullptr;
    }
    return false;
}

bool UMHInstancePoolSubsystem::GetInstance(const FMHInstanceHandle& Handle, UInstancedStaticMeshComponent*& OutComponent, int32& OutInstanceIndex) const
{
    OutComponent = nullptr;
    OutInstanceIndex = INDEX_NONE;
    const FSlot* Slot = nullptr;
    const FBucket* Bucket = ResolveHandle(Handle, Slot);
    if (Bucket == nullptr) return false;
    OutComponent = Bucket->Component.Get();
    OutInstanceIndex = Slot->bHidden ? INDEX_NONE : Slot->InstanceIndex;
    return OutComponent != nullptr;
}

void UMHInstancePoolSubsystem::BeginBulk()
{
    ++BulkDepth;
}

void UMHInstancePoolSubsystem::EndBulk()
{
    if (BulkDepth <= 0) return;
    if (--BulkDepth > 0) return;
    for (FBucket& Bucket : Buckets)
    {
        if (Bucket.bDirty) Flush(Bucket);
    }
}

void UMHInstancePoolSubsystem::HideOwner(const AActor& Owner)
{
    BeginBulk();
    for (FBucket& Bucket : Buckets)
    {
        for (int32 SlotId = 0; SlotId < Bucket.Slots.Num(); ++SlotId)
        {
            FSlot& Slot = Bucket.Slots[SlotId];
            if (Slot.bFree || Slot.bHidden || Slot.Owner.Get() != &Owner) continue;
            RemoveInstanceFromComponent(Bucket, SlotId);
            Slot.bHidden = true;
            MarkDirty(Bucket, true);
        }
    }
    EndBulk();
}

void UMHInstancePoolSubsystem::ShowOwner(const AActor& Owner)
{
    BeginBulk();
    for (FBucket& Bucket : Buckets)
    {
        for (int32 SlotId = 0; SlotId < Bucket.Slots.Num(); ++SlotId)
        {
            FSlot& Slot = Bucket.Slots[SlotId];
            if (Slot.bFree || !Slot.bHidden || Slot.Owner.Get() != &Owner) continue;
            Slot.bHidden = false;
            AddInstanceToComponent(Bucket, SlotId);
            MarkDirty(Bucket, true);
        }
    }
    EndBulk();
}

void UMHInstancePoolSubsystem::RemoveOwner(const AActor& Owner)
{
    BeginBulk();
    for (int32 BucketId = 0; BucketId < Buckets.Num(); ++BucketId)
    {
        FBucket& Bucket = Buckets[BucketId];
        for (int32 SlotId = 0; SlotId < Bucket.Slots.Num(); ++SlotId)
        {
            const FSlot& Slot = Bucket.Slots[SlotId];
            if (Slot.bFree || Slot.Owner.Get() != &Owner) continue;
            FMHInstanceHandle Handle;
            Handle.BucketId = BucketId;
            Handle.SlotId = SlotId;
            Handle.Generation = Slot.Generation;
            Remove(Handle);
        }
    }
    EndBulk();
}

void UMHInstancePoolSubsystem::MoveOwner(const AActor& Owner, const FMatrix& Delta)
{
    BeginBulk();
    for (FBucket& Bucket : Buckets)
    {
        UInstancedStaticMeshComponent* Component = Bucket.Component.Get();
        for (FSlot& Slot : Bucket.Slots)
        {
            if (Slot.bFree || Slot.Owner.Get() != &Owner) continue;
            Slot.WorldMatrix = Slot.WorldMatrix * Delta;
            if (Slot.bHidden || Component == nullptr) continue;
            Component->UpdateInstanceTransform(Slot.InstanceIndex, FTransform(Slot.WorldMatrix), true, false, true);
            MarkDirty(Bucket, false);
        }
    }
    EndBulk();
}

int32 UMHInstancePoolSubsystem::ReconcileMesh(const UStaticMesh& Mesh, const FMHEndpointInterfaceDelta& Delta)
{
    // R5b-0 red stub: the pool does not reconcile yet.
    static_cast<void>(Mesh);
    static_cast<void>(Delta);
    return 0;
}

void UMHInstancePoolSubsystem::GetBucketComponents(const UStaticMesh& Mesh, TArray<UInstancedStaticMeshComponent*>& OutComponents) const
{
    static_cast<void>(Mesh);
    OutComponents.Reset();
}

bool UMHInstancePoolSubsystem::MigrateBucket(FBucket& Bucket)
{
    static_cast<void>(Bucket);
    return false;
}

int32 UMHInstancePoolSubsystem::NumLiveInstances(const AActor& Owner) const
{
    int32 Count = 0;
    for (const FBucket& Bucket : Buckets)
    {
        for (const FSlot& Slot : Bucket.Slots)
        {
            if (!Slot.bFree && !Slot.bHidden && Slot.Owner.Get() == &Owner) ++Count;
        }
    }
    return Count;
}

int32 UMHInstancePoolSubsystem::FindOrCreateBucket(ULevel& Level, const FMHPoolBucketDescriptor& Descriptor)
{
    for (int32 BucketId = 0; BucketId < Buckets.Num(); ++BucketId)
    {
        const FBucket& Bucket = Buckets[BucketId];
        if (Bucket.Level.Get() == &Level && Bucket.Component.IsValid() && Bucket.Descriptor == Descriptor) return BucketId;
    }
    AMHInstancePoolActor* PoolActor = FindOrCreatePoolActor(Level);
    if (PoolActor == nullptr || PoolActor->GetRootComponent() == nullptr) return INDEX_NONE;
    UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(PoolActor, NAME_None, PoolObjectFlags);
    PoolConfigureBucket(*Component, Descriptor);
    Component->SetupAttachment(PoolActor->GetRootComponent());
    PoolActor->AddInstanceComponent(Component);
    Component->RegisterComponent();

    const int32 BucketId = Buckets.AddDefaulted();
    FBucket& Bucket = Buckets[BucketId];
    Bucket.Level = &Level;
    Bucket.Descriptor = Descriptor;
    Bucket.Component = Component;
    ++Metrics.BucketsCreated;
    return BucketId;
}

AMHInstancePoolActor* UMHInstancePoolSubsystem::FindOrCreatePoolActor(ULevel& Level)
{
    if (const TWeakObjectPtr<AMHInstancePoolActor>* Found = PoolActors.Find(&Level))
    {
        if (AMHInstancePoolActor* Existing = Found->Get(); IsValid(Existing)) return Existing;
    }
    UWorld* World = Level.GetWorld();
    if (World == nullptr) return nullptr;
    FActorSpawnParameters Params;
    Params.OverrideLevel = &Level;
    Params.ObjectFlags = PoolObjectFlags;
    Params.bNoFail = true;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
#if WITH_EDITOR
    Params.bHideFromSceneOutliner = true;
#endif
    AMHInstancePoolActor* Actor = World->SpawnActor<AMHInstancePoolActor>(Params);
    if (Actor == nullptr) return nullptr;
    PoolActors.Add(&Level, Actor);
    return Actor;
}

UMHInstancePoolSubsystem::FBucket* UMHInstancePoolSubsystem::ResolveHandle(const FMHInstanceHandle& Handle, FSlot*& OutSlot)
{
    const FSlot* ConstSlot = nullptr;
    const FBucket* Bucket = static_cast<const UMHInstancePoolSubsystem*>(this)->ResolveHandle(Handle, ConstSlot);
    OutSlot = const_cast<FSlot*>(ConstSlot);
    return const_cast<FBucket*>(Bucket);
}

const UMHInstancePoolSubsystem::FBucket* UMHInstancePoolSubsystem::ResolveHandle(const FMHInstanceHandle& Handle, const FSlot*& OutSlot) const
{
    OutSlot = nullptr;
    if (!Handle.IsSet() || !Buckets.IsValidIndex(Handle.BucketId)) return nullptr;
    const FBucket& Bucket = Buckets[Handle.BucketId];
    if (!Bucket.Slots.IsValidIndex(Handle.SlotId)) return nullptr;
    const FSlot& Slot = Bucket.Slots[Handle.SlotId];
    if (Slot.bFree || Slot.Generation != Handle.Generation) return nullptr;
    OutSlot = &Slot;
    return &Bucket;
}

void UMHInstancePoolSubsystem::RemoveInstanceFromComponent(FBucket& Bucket, const int32 SlotId)
{
    FSlot& Slot = Bucket.Slots[SlotId];
    const int32 Index = Slot.InstanceIndex;
    Slot.InstanceIndex = INDEX_NONE;
    if (!Bucket.InstanceToSlot.IsValidIndex(Index)) return;
    UInstancedStaticMeshComponent* Component = Bucket.Component.Get();
    if (Component != nullptr && Component->IsValidInstance(Index)) Component->RemoveInstance(Index);
    // Swap-remove: the last ISM instance now lives at Index; both maps follow.
    const int32 Last = Bucket.InstanceToSlot.Num() - 1;
    if (Index != Last)
    {
        const int32 MovedSlotId = Bucket.InstanceToSlot[Last];
        Bucket.InstanceToSlot[Index] = MovedSlotId;
        Bucket.Slots[MovedSlotId].InstanceIndex = Index;
    }
    Bucket.InstanceToSlot.Pop(EAllowShrinking::No);
}

void UMHInstancePoolSubsystem::AddInstanceToComponent(FBucket& Bucket, const int32 SlotId)
{
    FSlot& Slot = Bucket.Slots[SlotId];
    UInstancedStaticMeshComponent* Component = Bucket.Component.Get();
    if (Component == nullptr)
    {
        Slot.InstanceIndex = INDEX_NONE;
        return;
    }
    const int32 Index = Component->AddInstance(FTransform(Slot.WorldMatrix), true);
    // The ISM appends; the reverse map must agree with it or every later
    // swap-remove would corrupt the lookup.
    check(Index == Bucket.InstanceToSlot.Num());
    Bucket.InstanceToSlot.Add(SlotId);
    Slot.InstanceIndex = Index;
    if (PoolAppearanceFits(Bucket.Descriptor))
    {
        const int32 Base = Bucket.Descriptor.AppearanceCustomDataBaseIndex;
        for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
        {
            Component->SetCustomDataValue(Index, Base + Channel, Slot.Appearance[Channel], false);
        }
    }
}

void UMHInstancePoolSubsystem::MarkDirty(FBucket& Bucket, const bool bPhysics)
{
    Bucket.bDirty = true;
    Bucket.bPhysicsDirty |= bPhysics;
    if (BulkDepth == 0) Flush(Bucket);
}

void UMHInstancePoolSubsystem::Flush(FBucket& Bucket)
{
    UInstancedStaticMeshComponent* Component = Bucket.Component.Get();
    if (Component != nullptr)
    {
        Component->MarkRenderStateDirty();
        ++Metrics.RenderStateRefreshes;
        if (Bucket.bPhysicsDirty)
        {
            // Instance bodies follow AddInstance/RemoveInstance; the bucket's
            // bounds and navigation data are refreshed once per scope.
            Component->UpdateBounds();
            ++Metrics.PhysicsRefreshes;
        }
    }
    Bucket.bDirty = false;
    Bucket.bPhysicsDirty = false;
}
