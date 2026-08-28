#include "Composite/MHCompositePreviewCache.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Containers/Ticker.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Material/MHMaterialSourceData.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "Settings/MHCompositeSettings.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "Texture/MHTextureSourceData.h"
#include "UObject/UObjectGlobals.h"

namespace UE::MimirComposite
{
namespace
{
struct FPreviewGraphEntry
{
    TWeakObjectPtr<const UMHCompositeSettings> Settings;
    FString MasterRoot;
    FString LibraryRoot;
    TMap<FString, FSoftClassPath> ActorRegistry;
    TSharedPtr<const FMHRandomSourceGraph> Graph;
    TSet<FMHResourceKey> Dependencies;
};
TMap<TWeakObjectPtr<const UMHCompositeAsset>, FPreviewGraphEntry> PreviewGraphs;
TMap<TWeakObjectPtr<AMHCompositeActor>, TWeakObjectPtr<UStaticMesh>> PreviewPending;
FDelegateHandle PreviewAdded, PreviewRemoved, PreviewUpdated, PreviewUpdatedDisk, PreviewRenamed;
FDelegateHandle PreviewModified, PreviewPropertyChanged, PreviewCompiled;
FDelegateHandle PreviewPreExit;
bool bPreviewStarted = false;
uint64 PreviewRevision = 1;
uint64 PreviewGlobalRevision = 1;
TMap<FMHResourceKey, uint64> PreviewResourceRevisions;
FTSTicker::FDelegateHandle PreviewRetry;

void PreviewInvalidateAsset(const FAssetData&)
{
    // Claims can be malformed, wrong-class, or outside generated folders.
    // Do not filter these events by class/path and accidentally cache a winner.
    MHInvalidateCompositePreviewCache();
}

void PreviewInvalidateObject(UObject* Object)
{
    for (UObject* Cursor = Object; Cursor != nullptr; Cursor = Cursor->GetOuter())
    {
        if (Cursor->IsA<UMHStaticMeshImportData>() || Cursor->IsA<UMHMaterialSourceData>() ||
            Cursor->IsA<UMHTextureSourceData>() || Cursor->IsA<UMHCompositeSettings>())
        {
            // A receipt can add/remove a claim for an identity other than its
            // carrier name. Never consult a potentially compiling mesh here.
            MHInvalidateCompositePreviewCache();
            return;
        }
        FMHResourceKey Key;
        Key.LogicalName = Cursor->GetName();
        const TCHAR* Folder = nullptr;
        FString ReceiptName;
        if (const UMHCompositeAsset* Composite = Cast<UMHCompositeAsset>(Cursor))
        {
            Key.Kind = EMHResourceKind::Composite;
            Folder = TEXT("Composites");
            // A new unstamped composite has no foreign receipt claim; clearing
            // a previously valid receipt still revokes its object identity.
            ReceiptName = Composite->LogicalName.IsEmpty() ? Key.LogicalName : Composite->LogicalName;
        }
        else if (UStaticMesh* Mesh = Cast<UStaticMesh>(Cursor))
        {
            Key.Kind = EMHResourceKind::StaticMesh;
            Folder = TEXT("Meshes");
            if (!Mesh->IsCompiling())
                if (const UMHStaticMeshImportData* Receipt = Cast<UMHStaticMeshImportData>(Mesh->GetAssetImportData()))
                    ReceiptName = Receipt->LogicalName;
        }
        else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Cursor))
        {
            Key.Kind = EMHResourceKind::Material;
            Folder = TEXT("Materials");
            if (const UMHMaterialSourceData* Receipt = Cast<UMHMaterialSourceData>(Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass())))
                ReceiptName = Receipt->LogicalName;
        }
        else if (UTexture* Texture = Cast<UTexture>(Cursor))
        {
            Key.Kind = EMHResourceKind::Texture;
            Folder = TEXT("Textures");
            if (const UMHTextureSourceData* Receipt = Cast<UMHTextureSourceData>(Texture->GetAssetUserDataOfClass(UMHTextureSourceData::StaticClass())))
                ReceiptName = Receipt->LogicalName;
        }
        else continue;
        const bool bCanonicalCarrier = Key.IsCanonical() && ReceiptName == Key.LogicalName &&
            Cursor->GetPathName() == FString::Printf(TEXT("/Game/MH/Generated/%s/%s.%s"),
                Folder, *Key.LogicalName, *Key.LogicalName);
        // Unknown, out-of-folder, or divergent claims stay global. A proven
        // canonical ordinary asset edit need not revoke unrelated placements.
        MHInvalidateCompositePreviewCache(bCanonicalCarrier ? &Key : nullptr);
        return;
    }
}

void PreviewQueueCompleted(const TArray<FAssetCompileData>& Assets)
{
    bool bRelevant = false;
    for (const FAssetCompileData& Asset : Assets)
        for (const auto& Pending : PreviewPending)
            bRelevant |= Pending.Value.Get() == Asset.Asset.Get();
    if (!bRelevant || PreviewRetry.IsValid()) return;
    // Completion can be broadcast from inside another asset's synchronous load.
    // Retry on the next editor tick, never reenter compilation/registration.
    PreviewRetry = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
    {
        PreviewRetry.Reset();
        TArray<TWeakObjectPtr<AMHCompositeActor>> Ready;
        for (auto It = PreviewPending.CreateIterator(); It; ++It)
        {
            if (!It.Key().IsValid()) { It.RemoveCurrent(); continue; }
            if (!It.Value().IsValid() || !It.Value()->IsCompiling())
            {
                Ready.Add(It.Key());
                It.RemoveCurrent();
            }
        }
        for (const TWeakObjectPtr<AMHCompositeActor>& Weak : Ready)
            if (AMHCompositeActor* Actor = Weak.Get(); IsValid(Actor) && !Actor->IsActorBeingDestroyed() &&
                Actor->GetWorld() != nullptr && !Actor->GetWorld()->IsBeingCleanedUp())
                Actor->RebuildComposite(false);
        return false;
    }));
}
}

void MHStartupCompositePreviewCache()
{
    if (bPreviewStarted) return;
    bPreviewStarted = true;
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    PreviewAdded = Registry.OnAssetAdded().AddStatic(&PreviewInvalidateAsset);
    PreviewRemoved = Registry.OnAssetRemoved().AddStatic(&PreviewInvalidateAsset);
    PreviewUpdated = Registry.OnAssetUpdated().AddStatic(&PreviewInvalidateAsset);
    PreviewUpdatedDisk = Registry.OnAssetUpdatedOnDisk().AddStatic(&PreviewInvalidateAsset);
    PreviewRenamed = Registry.OnAssetRenamed().AddLambda([](const FAssetData&, const FString&) { MHInvalidateCompositePreviewCache(); });
    PreviewModified = FCoreUObjectDelegates::OnObjectModified.AddStatic(&PreviewInvalidateObject);
    PreviewPropertyChanged = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
        [](UObject* Object, FPropertyChangedEvent&) { PreviewInvalidateObject(Object); });
    PreviewCompiled = FAssetCompilingManager::Get().OnAssetPostCompileEvent().AddStatic(&PreviewQueueCompleted);
    PreviewPreExit = FCoreDelegates::OnEnginePreExit.AddStatic(&MHShutdownCompositePreviewCache);
}

void MHShutdownCompositePreviewCache()
{
    if (!bPreviewStarted) return;
    bPreviewStarted = false;
    FCoreDelegates::OnEnginePreExit.Remove(PreviewPreExit);
    if (FAssetRegistryModule* Module = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
    {
        IAssetRegistry& Registry = Module->Get();
        Registry.OnAssetAdded().Remove(PreviewAdded);
        Registry.OnAssetRemoved().Remove(PreviewRemoved);
        Registry.OnAssetUpdated().Remove(PreviewUpdated);
        Registry.OnAssetUpdatedOnDisk().Remove(PreviewUpdatedDisk);
        Registry.OnAssetRenamed().Remove(PreviewRenamed);
    }
    FCoreUObjectDelegates::OnObjectModified.Remove(PreviewModified);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PreviewPropertyChanged);
    FAssetCompilingManager::Get().OnAssetPostCompileEvent().Remove(PreviewCompiled);
    FTSTicker::GetCoreTicker().RemoveTicker(PreviewRetry);
    PreviewRetry.Reset();
    PreviewPending.Reset();
    PreviewGraphs.Reset();
    PreviewResourceRevisions.Reset();
}

void MHInvalidateCompositePreviewCache(const FMHResourceKey* ChangedKey)
{
    ++PreviewRevision;
    if (ChangedKey == nullptr)
    {
        PreviewGlobalRevision = PreviewRevision;
        PreviewResourceRevisions.Reset();
        PreviewGraphs.Reset();
        return;
    }
    PreviewResourceRevisions.Add(*ChangedKey, PreviewRevision);
    for (auto It = PreviewGraphs.CreateIterator(); It; ++It)
        if (!It.Key().IsValid() || It.Value().Dependencies.Contains(*ChangedKey)) It.RemoveCurrent();
}

uint64 MHCompositePreviewRevision(const TSet<FMHResourceKey>& Dependencies)
{
    uint64 Revision = PreviewGlobalRevision;
    for (const FMHResourceKey& Key : Dependencies)
        if (const uint64* Changed = PreviewResourceRevisions.Find(Key)) Revision = FMath::Max(Revision, *Changed);
    return Revision;
}

TSharedPtr<const FMHRandomSourceGraph> MHGetCompositePreviewGraph(
    const UMHCompositeAsset& Root, const UMHCompositeSettings& Settings,
    TSet<FMHResourceKey>& Dependencies, FString& Error, UStaticMesh*& PendingMesh)
{
    Error.Reset();
    PendingMesh = nullptr;
    if (const FPreviewGraphEntry* Entry = PreviewGraphs.Find(&Root); Entry != nullptr && Entry->Settings.Get() == &Settings &&
        Entry->MasterRoot == Settings.MasterRoot && Entry->LibraryRoot == Settings.LibraryRoot &&
        Entry->ActorRegistry.OrderIndependentCompareEqual(Settings.ActorClassRegistry))
    {
        Dependencies = Entry->Dependencies;
        return Entry->Graph;
    }
    TSharedRef<FMHRandomSourceGraph> Graph = MakeShared<FMHRandomSourceGraph>();
    if (!MHBuildAppliedCompositeGraph(Root, Settings, *Graph, Dependencies, Error, false, &PendingMesh)) return nullptr;
    FPreviewGraphEntry Entry;
    Entry.Settings = &Settings;
    Entry.MasterRoot = Settings.MasterRoot;
    Entry.LibraryRoot = Settings.LibraryRoot;
    Entry.ActorRegistry = Settings.ActorClassRegistry;
    Entry.Graph = Graph;
    Entry.Dependencies = Dependencies;
    PreviewGraphs.Add(&Root, MoveTemp(Entry));
    return Graph;
}

void MHDeferCompositePreview(AMHCompositeActor& Actor, UStaticMesh& Mesh)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::StaticMesh;
    Key.LogicalName = Mesh.GetName();
    MHInvalidateCompositePreviewCache(&Key);
    PreviewPending.Add(&Actor, &Mesh);
}
}
