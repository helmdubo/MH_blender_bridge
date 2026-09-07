#include "Composite/MHCompositeThumbnailRenderer.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeAppearanceTransport.h"
#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Composite/MHMaterializeLayout.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/StreamableManager.h"
#include "HAL/PlatformTime.h"
#include "ObjectTools.h"
#include "SceneView.h"
#include "Settings/MHCompositeSettings.h"
#include "ShowFlags.h"
#include "ThumbnailHelpers.h"
#include "ThumbnailRendering/SceneThumbnailInfo.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/ObjectKey.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeThumbnailRenderer)

using namespace UE::MimirComposite;

namespace
{
constexpr int32 ThumbnailSeed = 0;
constexpr int32 MaxCachedThumbnails = 32;

struct FThumbnailMeshRequest
{
    TSharedPtr<FStreamableHandle> Handle;
    TStrongObjectPtr<UStaticMesh> Mesh;
    bool bAdmitted = false;
    bool bFailed = false;
};

struct FThumbnailEntry
{
    TWeakObjectPtr<UMHCompositeAsset> Asset;
    TSharedPtr<const FMHResolvedCompositePlan> Plan;
    TSet<FMHResourceKey> Dependencies;
    TArray<TStrongObjectPtr<UStaticMesh>> Meshes;
    TMap<FMHResourceKey, FThumbnailMeshRequest> MeshRequests;
    uint32 RecipeRevision = 0;
    uint32 Version = 0;
    double LastUse = 0;
    bool bDirty = true;
    bool bPending = false;
};

void DirtyThumbnail(UMHCompositeAsset& Asset)
{
    if (FObjectThumbnail* Thumbnail = ThumbnailTools::GetThumbnailForObject(&Asset)) Thumbnail->MarkAsDirty();
    if (UThumbnailManager* Manager = UThumbnailManager::TryGet())
        Manager->GetOnThumbnailDirtied().Broadcast(FSoftObjectPath(&Asset));
}

class FMHCompositeThumbnailScene final : public FThumbnailPreviewScene
{
public:
    void SetEntry(const FObjectKey& Key, const FThumbnailEntry& Entry)
    {
        if (CurrentKey == Key && CurrentVersion == Entry.Version) return;
        for (UInstancedStaticMeshComponent* Component : Components)
        {
            RemoveComponent(Component);
            Component->DestroyComponent();
        }
        Components.Reset();
        CurrentKey = Key;
        CurrentVersion = Entry.Version;
        Bounds = FBox(ForceInit);
        const FMHResolvedCompositePlan& Plan = *Entry.Plan;
        for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
        {
            if (UStaticMesh* Mesh = Entry.Meshes[Index].Get())
                Bounds += Mesh->GetBoundingBox().TransformBy(Plan.Leaves[Index].WorldMatrix);
        }
        if (!Bounds.IsValid) return;
        const FVector Offset = -Bounds.GetCenter() + FVector(0, 0, Bounds.GetExtent().Z + 1.0);
        const int32 BaseIndex = GetDefault<UMHCompositeSettings>()->AppearanceCustomDataBaseIndex;
        TMap<UStaticMesh*, UInstancedStaticMeshComponent*> Buckets;
        for (int32 Index = 0; Index < Plan.Leaves.Num(); ++Index)
        {
            UStaticMesh* Mesh = Entry.Meshes[Index].Get();
            if (Mesh == nullptr) continue;
            UInstancedStaticMeshComponent*& Component = Buckets.FindOrAdd(Mesh);
            if (Component == nullptr)
            {
                Component = NewObject<UInstancedStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
                Component->SetCanEverAffectNavigation(false);
                Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                Component->SetMobility(EComponentMobility::Movable);
                Component->SetStaticMesh(Mesh);
                Component->ForcedLodModel = 1;
                Component->InstancingRandomSeed = 1;
                if (MHIsAdmissibleAppearanceCustomDataBaseIndex(BaseIndex))
                    Component->SetNumCustomDataFloats(BaseIndex + MH_APPEARANCE_CHANNELS);
                Components.Add(Component);
            }
            const FMHResolvedCompositeLeaf& Leaf = Plan.Leaves[Index];
            const int32 Instance = Component->AddInstance(FTransform(Leaf.WorldMatrix));
            if (MHIsAdmissibleAppearanceCustomDataBaseIndex(BaseIndex))
                for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
                    Component->SetCustomDataValue(Instance, BaseIndex + Channel, Leaf.AppearanceChannels[Channel], false);
        }
        for (UInstancedStaticMeshComponent* Component : Components)
            AddComponent(Component, FTransform(Offset));
    }

protected:
    virtual void GetViewMatrixParameters(const float FOV, FVector& Origin, float& Pitch, float& Yaw, float& Zoom) const override
    {
        const USceneThumbnailInfo* Info = GetDefault<USceneThumbnailInfo>();
        Origin = FVector(0, 0, -(Bounds.GetExtent().Z + 1.0));
        Pitch = Info->OrbitPitch;
        Yaw = Info->OrbitYaw;
        Zoom = FMath::Max(1.0, Bounds.GetExtent().Size() * 1.15 / FMath::Tan(FMath::DegreesToRadians(FOV) * 0.5));
    }

private:
    FObjectKey CurrentKey;
    uint32 CurrentVersion = 0;
    FBox Bounds = FBox(ForceInit);
    TArray<UInstancedStaticMeshComponent*> Components; // FPreviewScene owns GC references.
};
}

struct FMHCompositeThumbnailCache
{
    TMap<FObjectKey, FThumbnailEntry> Entries;
    TUniquePtr<FMHCompositeThumbnailScene> Scene;
    uint32 NextVersion = 0;
};

UMHCompositeThumbnailRenderer::UMHCompositeThumbnailRenderer() = default;
UMHCompositeThumbnailRenderer::~UMHCompositeThumbnailRenderer() = default;

bool UMHCompositeThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
    UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Object);
    UMHCompiledRecipeRegistry* Recipes = UMHCompiledRecipeRegistry::Get();
    if (!IsValid(Asset) || Recipes == nullptr || IsGarbageCollecting()) return false;
    if (!Cache)
    {
        Cache = MakeUnique<FMHCompositeThumbnailCache>();
        PendingTicker = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateUObject(this, &UMHCompositeThumbnailRenderer::TickPending), 0.2f);
        PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(
            this, &UMHCompositeThumbnailRenderer::OnObjectPropertyChanged);
    }
    const FObjectKey Key(Asset);
    if (!Cache->Entries.Contains(Key) && Cache->Entries.Num() >= MaxCachedThumbnails)
    {
        // Pending requests must keep their completion notification. Ready previews are LRU bounded.
        FObjectKey Oldest;
        double OldestUse = TNumericLimits<double>::Max();
        for (const auto& Pair : Cache->Entries)
            if (!Pair.Value.bPending && Pair.Value.LastUse < OldestUse)
            {
                Oldest = Pair.Key;
                OldestUse = Pair.Value.LastUse;
            }
        if (OldestUse != TNumericLimits<double>::Max()) Cache->Entries.Remove(Oldest);
    }
    FThumbnailEntry& Entry = Cache->Entries.FindOrAdd(Key);
    Entry.Asset = Asset;
    Entry.LastUse = FPlatformTime::Seconds();
    const uint32 Revision = Recipes->GetRecipeRevision(*Asset);
    if (Entry.bDirty || Entry.RecipeRevision != Revision)
    {
        Entry.bDirty = false;
        Entry.bPending = false;
        Entry.RecipeRevision = Revision;
        Entry.Version = ++Cache->NextVersion;
        Entry.Plan.Reset();
        Entry.Meshes.Reset();
        Entry.MeshRequests.Reset();
        Entry.Dependencies.Reset();
        FMHResourceKey RootKey;
        RootKey.Kind = EMHResourceKind::Composite;
        RootKey.LogicalName = Asset->LogicalName;
        Entry.Dependencies.Add(RootKey);
        FString Error;
        const FMHCompiledRecipe* Recipe = MHAdmitEndpointIdentity(RootKey, *Asset, Error)
            ? Recipes->Compile(*Asset, Error) : nullptr;
        if (Recipe != nullptr)
        {
            const FMHMaterializeResult Result = MHMaterializeLayout(*Recipe, ThumbnailSeed, ThumbnailSeed, FTransform::Identity);
            if (Result.Graph) MHCollectRecipeGraphDependencies(*Result.Graph, Entry.Dependencies);
            if (Result.Succeeded()) Entry.Plan = Result.Plan;
        }
        if (Entry.Plan) Entry.Meshes.SetNum(Entry.Plan->Leaves.Num());
    }
    if (!Entry.Plan) return false;
    const bool bWasPending = Entry.bPending;
    Entry.bPending = false;
    bool bHasMesh = false;
    for (int32 Index = 0; Index < Entry.Plan->Leaves.Num(); ++Index)
    {
        const FMHResolvedCompositeLeaf& Leaf = Entry.Plan->Leaves[Index];
        if (Leaf.Kind != EMHRandomSemanticKind::Mesh) continue;
        FMHResourceKey MeshKey;
        MeshKey.Kind = EMHResourceKind::StaticMesh;
        MeshKey.LogicalName = Leaf.Resource;
        Entry.Meshes[Index].Reset();
        FThumbnailMeshRequest& Request = Entry.MeshRequests.FindOrAdd(MeshKey);
        if (Request.bFailed) continue;
        if (Request.Handle && !Request.Handle->HasLoadCompleted())
        {
            Entry.bPending = true;
            continue;
        }
        const FString Path = MHEndpointObjectPath(MeshKey);
        UStaticMesh* Mesh = Request.Mesh.Get();
        if (Mesh == nullptr) Mesh = FindObject<UStaticMesh>(nullptr, *Path);
        if (Mesh == nullptr)
        {
            if (Request.Handle || Path.IsEmpty() || !FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)))
            {
                Request.bFailed = true;
                continue;
            }
            Request.Handle = UAssetManager::GetStreamableManager().RequestAsyncLoad(FSoftObjectPath(Path));
            Request.bFailed = !Request.Handle.IsValid();
            Entry.bPending |= Request.Handle.IsValid();
            continue;
        }
        Request.Mesh.Reset(Mesh);
        // The pool registry also reads render data/bounds and sets material usage.
        // A thumbnail needs only identity; avoid that admission path (including
        // its async completion callback) until mesh compilation has settled.
        if (Mesh->IsCompiling())
        {
            Entry.bPending = true;
            continue;
        }
        if (!Request.bAdmitted)
        {
            FString Error;
            Request.bAdmitted = MHAdmitEndpointIdentity(MeshKey, *Mesh, Error);
            Request.bFailed = !Request.bAdmitted;
        }
        if (Request.bAdmitted)
        {
            Entry.Meshes[Index].Reset(Mesh);
            bHasMesh = true;
        }
    }
    if (bWasPending && !Entry.bPending) Entry.Version = ++Cache->NextVersion;
    return bHasMesh && !Entry.bPending;
}

void UMHCompositeThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height,
    FRenderTarget* RenderTarget, FCanvas* Canvas, bool bAdditionalViewFamily)
{
    if (!CanVisualizeAsset(Object)) return;
    if (!Cache->Scene) Cache->Scene = MakeUnique<FMHCompositeThumbnailScene>();
    const FObjectKey Key(Object);
    Cache->Scene->SetEntry(Key, Cache->Entries.FindChecked(Key));
    FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
        RenderTarget, Cache->Scene->GetScene(), FEngineShowFlags(ESFIM_Game))
        .SetTime(UThumbnailRenderer::GetTime()).SetAdditionalViewFamily(bAdditionalViewFamily));
    ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
    ViewFamily.EngineShowFlags.MotionBlur = 0;
    RenderViewFamily(Canvas, &ViewFamily, Cache->Scene->CreateView(&ViewFamily, X, Y, Width, Height));
}

bool UMHCompositeThumbnailRenderer::TickPending(float DeltaTime)
{
    if (!Cache || IsGarbageCollecting()) return true;
    TArray<TWeakObjectPtr<UMHCompositeAsset>> Pending;
    for (auto It = Cache->Entries.CreateIterator(); It; ++It)
    {
        if (!It.Value().Asset.IsValid()) It.RemoveCurrent();
        else if (It.Value().bPending || It.Value().bDirty) Pending.Add(It.Value().Asset);
    }
    for (const TWeakObjectPtr<UMHCompositeAsset>& Weak : Pending)
    {
        UMHCompositeAsset* Asset = Weak.Get();
        if (Asset == nullptr) continue;
        CanVisualizeAsset(Asset);
        if (const FThumbnailEntry* Entry = Cache->Entries.Find(FObjectKey(Asset)); Entry != nullptr && !Entry->bPending)
            DirtyThumbnail(*Asset);
    }
    // A burst of cold requests may temporarily exceed the ready-cache budget.
    // Trim after completion as well as on insertion, without losing pending retries.
    while (Cache->Entries.Num() > MaxCachedThumbnails)
    {
        FObjectKey Oldest;
        double OldestUse = TNumericLimits<double>::Max();
        for (const auto& Pair : Cache->Entries)
            if (!Pair.Value.bPending && Pair.Value.LastUse < OldestUse)
            {
                Oldest = Pair.Key;
                OldestUse = Pair.Value.LastUse;
            }
        if (OldestUse == TNumericLimits<double>::Max()) break;
        Cache->Entries.Remove(Oldest);
    }
    return true;
}

void UMHCompositeThumbnailRenderer::InvalidateResource(const FMHResourceKey& Key)
{
    if (!Cache) return;
    for (auto& Pair : Cache->Entries)
        if (Pair.Value.Dependencies.Contains(Key) || Key.Kind == EMHResourceKind::Material || Key.Kind == EMHResourceKind::Texture)
            Pair.Value.bDirty = true;
}

void UMHCompositeThumbnailRenderer::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
    if (const UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Object))
    {
        FMHResourceKey Key;
        Key.Kind = EMHResourceKind::Composite;
        Key.LogicalName = Asset->LogicalName;
        InvalidateResource(Key);
    }
}

void UMHCompositeThumbnailRenderer::ReleaseResources()
{
    FTSTicker::GetCoreTicker().RemoveTicker(PendingTicker);
    PendingTicker.Reset();
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
    PropertyChangedHandle.Reset();
    Cache.Reset();
}

void UMHCompositeThumbnailRenderer::BeginDestroy()
{
    ReleaseResources();
    Super::BeginDestroy();
}

#if WITH_DEV_AUTOMATION_TESTS
const FMHResolvedCompositePlan* UMHCompositeThumbnailRenderer::GetPreparedPlanForTests(UMHCompositeAsset* Asset) const
{
    const FThumbnailEntry* Entry = Cache ? Cache->Entries.Find(FObjectKey(Asset)) : nullptr;
    return Entry != nullptr ? Entry->Plan.Get() : nullptr;
}
const FThumbnailPreviewScene* UMHCompositeThumbnailRenderer::GetPreviewSceneForTests() const { return Cache ? Cache->Scene.Get() : nullptr; }
bool UMHCompositeThumbnailRenderer::HasRequestedMeshForTests(const FString& LogicalName) const
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::StaticMesh;
    Key.LogicalName = LogicalName;
    if (Cache)
        for (const auto& Pair : Cache->Entries)
            if (Pair.Value.MeshRequests.Contains(Key)) return true;
    return false;
}
bool UMHCompositeThumbnailRenderer::FlushAsyncLoadsForTests()
{
    if (!Cache) return true;
    for (auto& Pair : Cache->Entries)
        for (auto& Request : Pair.Value.MeshRequests)
            if (Request.Value.Handle)
            {
                Request.Value.Handle->WaitUntilComplete();
                if (!Request.Value.Handle->HasLoadCompleted()) return false;
            }
    return true;
}
#endif

void UE::MimirComposite::MHInvalidateCompositeThumbnails(const FMHResourceKey& Key)
{
    for (TObjectIterator<UMHCompositeThumbnailRenderer> It; It; ++It)
        if (!It->IsTemplate()) It->InvalidateResource(Key);
}

void UE::MimirComposite::MHReleaseCompositeThumbnails()
{
    for (TObjectIterator<UMHCompositeThumbnailRenderer> It; It; ++It) It->ReleaseResources();
}
