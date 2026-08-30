#include "Source/MHSourceImportBatch.h"

#include "AssetCompilingManager.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "FileHelpers.h"
#include "Misc/FileHelper.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImportMetrics.h"
#include "UObject/Package.h"

namespace UE::MimirComposite
{
namespace
{
thread_local FMHSourceImportBatchContext* GMHActiveSourceImportBatch = nullptr;
}

FMHSourceImportBatchContext::FMHSourceImportBatchContext()
{
    check(GMHActiveSourceImportBatch == nullptr);
    GMHActiveSourceImportBatch = this;
}

FMHSourceImportBatchContext::~FMHSourceImportBatchContext()
{
    check(GMHActiveSourceImportBatch == this);
    GMHActiveSourceImportBatch = nullptr;
}

bool FMHSourceImportBatchContext::FinishCompilation(
    TMap<FMHResourceKey, FString>& OutErrors)
{
    OutErrors.Reset();
    if (!bNeedsCompilation)
    {
        return true;
    }
    {
        FMHSourceImportMetricScope WaitScope(
            EMHSourceImportMetricResource::Batch,
            EMHSourceImportMetricStage::BuildWait);
        FAssetCompilingManager::Get().FinishAllCompilation();
    }
    for (FDeferredFinalizer& Deferred : Finalizers)
    {
        FString Error;
        if (!Deferred.Function(Error))
        {
            FailedKeys.Add(Deferred.Key);
            OutErrors.FindOrAdd(Deferred.Key) = Error.IsEmpty()
                ? TEXT("MH_E_IMPORT_FAILED: deferred compilation finalization failed")
                : MoveTemp(Error);
        }
    }
    for (const FMHResourceKey& Key : FailedKeys)
    {
        PackagesByResource.Remove(Key);
        ProjectionKeys.Remove(Key);
        NotificationKeys.Remove(Key);
    }
    return OutErrors.IsEmpty();
}

bool FMHSourceImportBatchContext::SavePackages(FString& OutError)
{
    OutError.Reset();
    for (const FSourceGuard& Guard : SourceGuards)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Guard.PayloadPath) ||
            MHRawPayloadHash(Bytes) != Guard.ExpectedRawHash)
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: %s changed before bulk package save"),
                *Guard.Key.ToString());
            return false;
        }
    }
    TSet<UPackage*> UniquePackages;
    for (const TPair<FMHResourceKey, TArray<UPackage*>>& Pair : PackagesByResource)
    {
        if (!FailedKeys.Contains(Pair.Key))
        {
            UniquePackages.Append(Pair.Value);
        }
    }
    TArray<UPackage*> Packages = UniquePackages.Array();
    Packages.Sort([](const UPackage& Left, const UPackage& Right)
    {
        return Left.GetName() < Right.GetName();
    });
    if (Packages.IsEmpty())
    {
        return true;
    }

    FMHSourceImportMetricScope SaveScope(
        EMHSourceImportMetricResource::Batch,
        EMHSourceImportMetricStage::SavePackage);
    if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, true))
    {
        OutError = TEXT("MH_E_IMPORT_FAILED: batch package save failed");
        return false;
    }
    for (const UPackage* Package : Packages)
    {
        if (Package == nullptr || Package->HasAnyPackageFlags(PKG_InMemoryOnly))
        {
            OutError = TEXT("MH_E_IMPORT_FAILED: batch save left an in-memory-only package");
            return false;
        }
    }
    for (TPair<FMHResourceKey, TFunction<void()>>& Pair : PostSaveActions)
    {
        if (!FailedKeys.Contains(Pair.Key))
        {
            Pair.Value();
        }
    }
    return true;
}

bool FMHSourceImportBatchContext::CommitProjectionAndNotifications(
    const FString& SourceRoot,
    FString& OutError)
{
    if (!MHRefreshGeneratedAssetProjection(SourceRoot, OutError))
    {
        return false;
    }
    TArray<FMHResourceKey> Keys = NotificationKeys.Array();
    Keys.Sort([](const FMHResourceKey& Left, const FMHResourceKey& Right)
    {
        if (Left.Kind != Right.Kind)
        {
            return static_cast<uint8>(Left.Kind) < static_cast<uint8>(Right.Kind);
        }
        return Left.LogicalName < Right.LogicalName;
    });
    for (const FMHResourceKey& Key : Keys)
    {
        MHNotifyGeneratedResourceChanged(Key);
    }
    return true;
}

bool MHIsSourceImportBatchActive()
{
    return GMHActiveSourceImportBatch != nullptr;
}

void FMHSourceImportBatchContext::QueuePackage(
    UObject& Asset,
    const FMHResourceKey& Key)
{
    UPackage* Package = Asset.GetOutermost();
    check(Package != nullptr);
    Package->MarkPackageDirty();
    PackagesByResource.FindOrAdd(Key).AddUnique(Package);
}

void FMHSourceImportBatchContext::QueuePostCompilation(
    const FMHResourceKey& Key,
    TFunction<bool(FString&)> Finalizer)
{
    FDeferredFinalizer& Deferred = Finalizers.AddDefaulted_GetRef();
    Deferred.Key = Key;
    Deferred.Function = MoveTemp(Finalizer);
    bNeedsCompilation = true;
}

void FMHSourceImportBatchContext::QueueCompletion(
    const FMHResourceKey& Key,
    const bool bNotify)
{
    ProjectionKeys.Add(Key);
    if (bNotify)
    {
        NotificationKeys.Add(Key);
    }
}

void FMHSourceImportBatchContext::QueuePostSave(
    const FMHResourceKey& Key,
    TFunction<void()> Action)
{
    PostSaveActions.Emplace(Key, MoveTemp(Action));
}

void FMHSourceImportBatchContext::QueueSourceGuard(
    const FMHResourceKey& Key,
    const FString& PayloadPath,
    const FString& ExpectedRawHash)
{
    FSourceGuard& Guard = SourceGuards.AddDefaulted_GetRef();
    Guard.Key = Key;
    Guard.PayloadPath = PayloadPath;
    Guard.ExpectedRawHash = ExpectedRawHash;
}

bool MHDeferSourceImportPersistence(UObject& Asset)
{
    if (GMHActiveSourceImportBatch == nullptr)
    {
        return false;
    }
    Asset.GetOutermost()->MarkPackageDirty();
    return true;
}

void MHQueueSourceImportPackage(UObject& Asset, const FMHResourceKey& Key)
{
    check(GMHActiveSourceImportBatch != nullptr);
    GMHActiveSourceImportBatch->QueuePackage(Asset, Key);
}

void MHQueueSourceImportPostCompilation(
    const FMHResourceKey& Key,
    TFunction<bool(FString&)> Finalizer)
{
    check(GMHActiveSourceImportBatch != nullptr);
    GMHActiveSourceImportBatch->QueuePostCompilation(Key, MoveTemp(Finalizer));
}

void MHQueueSourceImportCompilation()
{
    check(GMHActiveSourceImportBatch != nullptr);
    GMHActiveSourceImportBatch->QueueCompilation();
}

void MHQueueSourceImportCompletion(const FMHResourceKey& Key, const bool bNotify)
{
    check(GMHActiveSourceImportBatch != nullptr);
    GMHActiveSourceImportBatch->QueueCompletion(Key, bNotify);
}

void MHQueueSourceImportPostSave(
    const FMHResourceKey& Key,
    TFunction<void()> Action)
{
    check(GMHActiveSourceImportBatch != nullptr);
    GMHActiveSourceImportBatch->QueuePostSave(Key, MoveTemp(Action));
}

void MHQueueSourceImportSourceGuard(
    const FMHResourceKey& Key,
    const FString& PayloadPath,
    const FString& ExpectedRawHash)
{
    check(GMHActiveSourceImportBatch != nullptr);
    GMHActiveSourceImportBatch->QueueSourceGuard(Key, PayloadPath, ExpectedRawHash);
}
} // namespace UE::MimirComposite
