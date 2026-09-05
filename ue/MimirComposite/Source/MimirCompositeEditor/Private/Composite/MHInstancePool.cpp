#include "Composite/MHInstancePool.h"

#include "Components/InstancedStaticMeshComponent.h"
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

AMHInstancePoolActor::AMHInstancePoolActor()
{
    SetFlags(RF_Transient);
    bIsEditorOnlyActor = true;
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

// R5a red stub: no pool yet. Every operation fails closed.

FMHInstanceHandle UMHInstancePoolSubsystem::Add(
    AActor& Owner, const FString& NodePath, ULevel& Level, const FMHPoolBucketDescriptor& Descriptor,
    const FMatrix& WorldMatrix, const float (&AppearanceChannels)[UE::MimirComposite::MH_APPEARANCE_CHANNELS])
{
    static_cast<void>(Owner);
    static_cast<void>(NodePath);
    static_cast<void>(Level);
    static_cast<void>(Descriptor);
    static_cast<void>(WorldMatrix);
    static_cast<void>(AppearanceChannels);
    return FMHInstanceHandle();
}

bool UMHInstancePoolSubsystem::Update(const FMHInstanceHandle& Handle, const FMatrix& WorldMatrix)
{
    static_cast<void>(Handle);
    static_cast<void>(WorldMatrix);
    return false;
}

bool UMHInstancePoolSubsystem::UpdateAppearance(const FMHInstanceHandle& Handle, const float (&AppearanceChannels)[UE::MimirComposite::MH_APPEARANCE_CHANNELS])
{
    static_cast<void>(Handle);
    static_cast<void>(AppearanceChannels);
    return false;
}

bool UMHInstancePoolSubsystem::Remove(const FMHInstanceHandle& Handle)
{
    static_cast<void>(Handle);
    return false;
}

bool UMHInstancePoolSubsystem::IsValidHandle(const FMHInstanceHandle& Handle) const
{
    static_cast<void>(Handle);
    return false;
}

bool UMHInstancePoolSubsystem::ReverseLookup(const UInstancedStaticMeshComponent* Component, const int32 InstanceIndex, AActor*& OutOwner, FString& OutNodePath) const
{
    static_cast<void>(Component);
    static_cast<void>(InstanceIndex);
    OutOwner = nullptr;
    OutNodePath.Reset();
    return false;
}

bool UMHInstancePoolSubsystem::GetInstance(const FMHInstanceHandle& Handle, UInstancedStaticMeshComponent*& OutComponent, int32& OutInstanceIndex) const
{
    static_cast<void>(Handle);
    OutComponent = nullptr;
    OutInstanceIndex = INDEX_NONE;
    return false;
}

void UMHInstancePoolSubsystem::BeginBulk() { ++BulkDepth; }
void UMHInstancePoolSubsystem::EndBulk() { BulkDepth = FMath::Max(0, BulkDepth - 1); }

void UMHInstancePoolSubsystem::HideOwner(const AActor& Owner) { static_cast<void>(Owner); }
void UMHInstancePoolSubsystem::ShowOwner(const AActor& Owner) { static_cast<void>(Owner); }
void UMHInstancePoolSubsystem::RemoveOwner(const AActor& Owner) { static_cast<void>(Owner); }
void UMHInstancePoolSubsystem::MoveOwner(const AActor& Owner, const FMatrix& Delta)
{
    static_cast<void>(Owner);
    static_cast<void>(Delta);
}

int32 UMHInstancePoolSubsystem::NumLiveInstances(const AActor& Owner) const
{
    static_cast<void>(Owner);
    return 0;
}

int32 UMHInstancePoolSubsystem::FindOrCreateBucket(ULevel& Level, const FMHPoolBucketDescriptor& Descriptor)
{
    static_cast<void>(Level);
    static_cast<void>(Descriptor);
    return INDEX_NONE;
}

AMHInstancePoolActor* UMHInstancePoolSubsystem::FindOrCreatePoolActor(ULevel& Level)
{
    static_cast<void>(Level);
    return nullptr;
}

UMHInstancePoolSubsystem::FBucket* UMHInstancePoolSubsystem::ResolveHandle(const FMHInstanceHandle& Handle, FSlot*& OutSlot)
{
    static_cast<void>(Handle);
    OutSlot = nullptr;
    return nullptr;
}

const UMHInstancePoolSubsystem::FBucket* UMHInstancePoolSubsystem::ResolveHandle(const FMHInstanceHandle& Handle, const FSlot*& OutSlot) const
{
    static_cast<void>(Handle);
    OutSlot = nullptr;
    return nullptr;
}

void UMHInstancePoolSubsystem::RemoveInstanceFromComponent(FBucket& Bucket, const int32 SlotId)
{
    static_cast<void>(Bucket);
    static_cast<void>(SlotId);
}

void UMHInstancePoolSubsystem::AddInstanceToComponent(FBucket& Bucket, const int32 SlotId)
{
    static_cast<void>(Bucket);
    static_cast<void>(SlotId);
}

void UMHInstancePoolSubsystem::MarkDirty(FBucket& Bucket, const bool bPhysics)
{
    static_cast<void>(Bucket);
    static_cast<void>(bPhysics);
}

void UMHInstancePoolSubsystem::Flush(FBucket& Bucket)
{
    static_cast<void>(Bucket);
}
