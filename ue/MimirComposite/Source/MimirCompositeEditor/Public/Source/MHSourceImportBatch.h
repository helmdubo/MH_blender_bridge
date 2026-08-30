#pragma once

#include "CoreMinimal.h"
#include "Source/MHSourceResolver.h"

class UObject;
class UPackage;

namespace UE::MimirComposite
{
/** Editor-only state shared by the three passes of one Import Changed batch. */
class MIMIRCOMPOSITEEDITOR_API FMHSourceImportBatchContext final
{
public:
    FMHSourceImportBatchContext();
    ~FMHSourceImportBatchContext();

    FMHSourceImportBatchContext(const FMHSourceImportBatchContext&) = delete;
    FMHSourceImportBatchContext& operator=(const FMHSourceImportBatchContext&) = delete;

    bool FinishCompilation(TMap<FMHResourceKey, FString>& OutErrors);
    bool SavePackages(FString& OutError);
    bool CommitProjectionAndNotifications(const FString& SourceRoot, FString& OutError);
    bool HasPreparedResources() const { return !PackagesByResource.IsEmpty(); }
    void QueuePackage(UObject& Asset, const FMHResourceKey& Key);
    void QueuePostCompilation(
        const FMHResourceKey& Key,
        TFunction<bool(FString&)> Finalizer);
    void QueueCompilation() { bNeedsCompilation = true; }
    void QueueCompletion(const FMHResourceKey& Key, bool bNotify);
    void QueuePostSave(const FMHResourceKey& Key, TFunction<void()> Action);
    void QueueSourceGuard(
        const FMHResourceKey& Key,
        const FString& PayloadPath,
        const FString& ExpectedRawHash);

private:
    struct FDeferredFinalizer
    {
        FMHResourceKey Key;
        TFunction<bool(FString&)> Function;
    };

    struct FSourceGuard
    {
        FMHResourceKey Key;
        FString PayloadPath;
        FString ExpectedRawHash;
    };

    TMap<FMHResourceKey, TArray<UPackage*>> PackagesByResource;
    TArray<FDeferredFinalizer> Finalizers;
    TArray<TPair<FMHResourceKey, TFunction<void()>>> PostSaveActions;
    TArray<FSourceGuard> SourceGuards;
    TSet<FMHResourceKey> ProjectionKeys;
    TSet<FMHResourceKey> NotificationKeys;
    TSet<FMHResourceKey> FailedKeys;
    bool bNeedsCompilation = false;
};

MIMIRCOMPOSITEEDITOR_API bool MHIsSourceImportBatchActive();

/** Mark dirty and suppress the importer's immediate SavePackage call. */
MIMIRCOMPOSITEEDITOR_API bool MHDeferSourceImportPersistence(UObject& Asset);

/** Admit a successfully prepared package to pass 3. */
MIMIRCOMPOSITEEDITOR_API void MHQueueSourceImportPackage(
    UObject& Asset,
    const FMHResourceKey& Key);

/** Run after the batch-wide compilation wait and before any package save. */
MIMIRCOMPOSITEEDITOR_API void MHQueueSourceImportPostCompilation(
    const FMHResourceKey& Key,
    TFunction<bool(FString&)> Finalizer);

MIMIRCOMPOSITEEDITOR_API void MHQueueSourceImportCompilation();

/** Defer project-index projection and optional placement invalidation. */
MIMIRCOMPOSITEEDITOR_API void MHQueueSourceImportCompletion(
    const FMHResourceKey& Key,
    bool bNotify);

MIMIRCOMPOSITEEDITOR_API void MHQueueSourceImportPostSave(
    const FMHResourceKey& Key,
    TFunction<void()> Action);

MIMIRCOMPOSITEEDITOR_API void MHQueueSourceImportSourceGuard(
    const FMHResourceKey& Key,
    const FString& PayloadPath,
    const FString& ExpectedRawHash);
} // namespace UE::MimirComposite
