#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Random/MHRandomStream.h"
#include "Source/MHSourceResolver.h"
#include "MHCompositeDefinitionSubsystem.generated.h"

class UMHCompositeAsset;
class UMHCompositeSettings;

namespace UE::MimirComposite
{
/** The exact five-part S6.5 identity of one immutable admitted definition. */
struct FMHCompositeDefinitionKey
{
    FMHResourceKey RootResourceKey;
    FString RootAppliedHash;
    FString ClosureHash;
    uint64 ActorClassRegistryRevision = 0;
    int32 ImporterVersion = 0;

    bool operator==(const FMHCompositeDefinitionKey& Other) const
    {
        return RootResourceKey == Other.RootResourceKey &&
            RootAppliedHash == Other.RootAppliedHash &&
            ClosureHash == Other.ClosureHash &&
            ActorClassRegistryRevision == Other.ActorClassRegistryRevision &&
            ImporterVersion == Other.ImporterVersion;
    }
};

inline uint32 GetTypeHash(const FMHCompositeDefinitionKey& Key)
{
    uint32 Hash = GetTypeHash(Key.RootResourceKey);
    Hash = HashCombine(Hash, GetTypeHash(Key.RootAppliedHash));
    Hash = HashCombine(Hash, GetTypeHash(Key.ClosureHash));
    Hash = HashCombine(Hash, ::GetTypeHash(Key.ActorClassRegistryRevision));
    return HashCombine(Hash, ::GetTypeHash(Key.ImporterVersion));
}

struct FMHCompositeDefinitionEntry
{
    TWeakObjectPtr<const UMHCompositeAsset> RootObject;
    FString RootSourceHash;
    TSharedPtr<const FMHRandomSourceGraph> Graph;
    TSet<FMHResourceKey> Dependencies;
};
} // namespace UE::MimirComposite

/** Session-only pool of successful immutable editor definition graphs. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHCompositeDefinitionSubsystem final : public UEditorSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;

    TSharedPtr<UE::MimirComposite::FMHCompositeDefinitionEntry> GetOrBuildDefinition(
        const UMHCompositeAsset& Root,
        const UMHCompositeSettings& Settings,
        TSet<UE::MimirComposite::FMHResourceKey>& OutDependencies,
        FString& OutError);

    /** Revoke every definition whose admitted closure observed ChangedKey. */
    void InvalidateDefinition(const UE::MimirComposite::FMHResourceKey& ChangedKey);

    /** Conservative startup/settings invalidation. */
    void InvalidateAllDefinitions();

private:
    uint64 RefreshActorClassRegistryRevision(const UMHCompositeSettings& Settings);
    void RemoveDeadDefinitions();
    void IndexDefinition(
        const UE::MimirComposite::FMHCompositeDefinitionKey& Key,
        const UE::MimirComposite::FMHCompositeDefinitionEntry& Entry);
    /** Index-only maintenance; the caller still owns the Definitions removal. */
    void UnindexDefinition(
        const UE::MimirComposite::FMHCompositeDefinitionKey& Key,
        const UE::MimirComposite::FMHCompositeDefinitionEntry* Entry);
    void RemoveDefinitionByKey(const UE::MimirComposite::FMHCompositeDefinitionKey& Key);
    bool WasInvalidatedDuring(
        uint64 AdmissionSerial,
        const TSet<UE::MimirComposite::FMHResourceKey>& Dependencies) const;

    TMap<UE::MimirComposite::FMHCompositeDefinitionKey,
        TSharedPtr<UE::MimirComposite::FMHCompositeDefinitionEntry>> Definitions;
    // Secondary views of Definitions, never an identity of their own. The
    // five-part key stays the integrity identity; every mutation of Definitions
    // must keep both maps exactly consistent and drop emptied buckets.
    TMap<UE::MimirComposite::FMHResourceKey,
        TArray<UE::MimirComposite::FMHCompositeDefinitionKey>> DefinitionKeysByRoot;
    TMap<UE::MimirComposite::FMHResourceKey,
        TSet<UE::MimirComposite::FMHCompositeDefinitionKey>> DefinitionKeysByDependency;
    TMap<FString, FSoftClassPath> ActorClassRegistrySnapshot;
    TMap<UE::MimirComposite::FMHResourceKey, uint64> ResourceInvalidationRevisions;
    uint64 ActorClassRegistryRevision = 1;
    uint64 InvalidationSerial = 1;
    uint64 GlobalInvalidationRevision = 1;
    bool bActorClassRegistryInitialized = false;
};
