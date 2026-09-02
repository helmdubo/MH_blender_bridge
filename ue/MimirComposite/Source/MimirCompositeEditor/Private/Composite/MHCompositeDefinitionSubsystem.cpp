#include "Composite/MHCompositeDefinitionSubsystem.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Engine/StaticMesh.h"
#include "Settings/MHCompositeSettings.h"
#include "StaticMesh/MHStaticMeshImportData.h"

using namespace UE::MimirComposite;


UObject* UE::MimirComposite::MHResolveCompositeDefinitionEndpoint(
    FMHCompositeDefinitionEntry& Definition, const FMHResourceKey& Key, FString& OutError)
{
    // R0a: the session-wide prototype registry is the endpoint cache. The
    // per-definition Endpoints map is dead and is removed with this symbol in R0b.
    (void)Definition;
    if (UMHEndpointPrototypeRegistry* Registry = UMHEndpointPrototypeRegistry::Get())
    {
        return Registry->ResolveObject(Key, OutError);
    }
    return MHLoadAppliedResource(Key, OutError);
}

void UMHCompositeDefinitionSubsystem::Deinitialize()
{
    Definitions.Reset();
    DefinitionKeysByRoot.Reset();
    DefinitionKeysByDependency.Reset();
    ActorClassRegistrySnapshot.Reset();
    ResourceInvalidationRevisions.Reset();
    Super::Deinitialize();
}

uint64 UMHCompositeDefinitionSubsystem::RefreshActorClassRegistryRevision(
    const UMHCompositeSettings& Settings)
{
    if (!bActorClassRegistryInitialized)
    {
        ActorClassRegistrySnapshot = Settings.ActorClassRegistry;
        bActorClassRegistryInitialized = true;
    }
    else if (!ActorClassRegistrySnapshot.OrderIndependentCompareEqual(Settings.ActorClassRegistry))
    {
        ActorClassRegistrySnapshot = Settings.ActorClassRegistry;
        ++ActorClassRegistryRevision;
        InvalidateAllDefinitions();
    }
    return ActorClassRegistryRevision;
}

void UMHCompositeDefinitionSubsystem::IndexDefinition(
    const FMHCompositeDefinitionKey& Key, const FMHCompositeDefinitionEntry& Entry)
{
    // AddUnique guards the readmission of an already-pooled five-part key.
    DefinitionKeysByRoot.FindOrAdd(Key.RootResourceKey).AddUnique(Key);
    for (const FMHResourceKey& Dependency : Entry.Dependencies)
        DefinitionKeysByDependency.FindOrAdd(Dependency).Add(Key);
}

void UMHCompositeDefinitionSubsystem::UnindexDefinition(
    const FMHCompositeDefinitionKey& Key, const FMHCompositeDefinitionEntry* Entry)
{
    if (TArray<FMHCompositeDefinitionKey>* RootKeys = DefinitionKeysByRoot.Find(Key.RootResourceKey))
    {
        RootKeys->RemoveSingle(Key);
        if (RootKeys->IsEmpty()) DefinitionKeysByRoot.Remove(Key.RootResourceKey);
    }
    if (Entry == nullptr) return;
    for (const FMHResourceKey& Dependency : Entry->Dependencies)
        if (TSet<FMHCompositeDefinitionKey>* Dependents = DefinitionKeysByDependency.Find(Dependency))
        {
            Dependents->Remove(Key);
            if (Dependents->IsEmpty()) DefinitionKeysByDependency.Remove(Dependency);
        }
}

void UMHCompositeDefinitionSubsystem::RemoveDefinitionByKey(const FMHCompositeDefinitionKey& Key)
{
    TSharedPtr<FMHCompositeDefinitionEntry> Entry;
    if (!Definitions.RemoveAndCopyValue(Key, Entry)) return;
    UnindexDefinition(Key, Entry.Get());
}

void UMHCompositeDefinitionSubsystem::RemoveDeadDefinitions()
{
    for (auto It = Definitions.CreateIterator(); It; ++It)
        if (!It.Value().IsValid() || !It.Value()->RootObject.IsValid() ||
            !It.Value()->Graph.IsValid())
        {
            UnindexDefinition(It.Key(), It.Value().Get());
            It.RemoveCurrent();
        }
}

bool UMHCompositeDefinitionSubsystem::WasInvalidatedDuring(
    const uint64 AdmissionSerial, const TSet<FMHResourceKey>& Dependencies) const
{
    if (GlobalInvalidationRevision > AdmissionSerial) return true;
    for (const FMHResourceKey& Dependency : Dependencies)
        if (const uint64* Revision = ResourceInvalidationRevisions.Find(Dependency);
            Revision != nullptr && *Revision > AdmissionSerial) return true;
    return false;
}

TSharedPtr<FMHCompositeDefinitionEntry> UMHCompositeDefinitionSubsystem::GetOrBuildDefinition(
    const UMHCompositeAsset& Root, const UMHCompositeSettings& Settings,
    TSet<FMHResourceKey>& OutDependencies, FString& OutError)
{
    OutDependencies.Reset();
    OutError.Reset();
    RemoveDeadDefinitions();

    FMHResourceKey RootKey;
    RootKey.Kind = EMHResourceKind::Composite;
    RootKey.LogicalName = Root.LogicalName;
    const uint64 RegistryRevision = RefreshActorClassRegistryRevision(Settings);
    const FString RootAppliedHash = Root.AppliedHash;
    const FString RootSourceHash = Root.SourceHash;

    // Snapshot the bucket: a stale removal rewrites both secondary indices.
    TArray<FMHCompositeDefinitionKey> RootCandidates;
    if (const TArray<FMHCompositeDefinitionKey>* Bucket = DefinitionKeysByRoot.Find(RootKey))
        RootCandidates = *Bucket;
    for (const FMHCompositeDefinitionKey& Key : RootCandidates)
    {
        MHRecordDefinitionLookupProbe();
        const TSharedPtr<FMHCompositeDefinitionEntry>& Entry = Definitions.FindChecked(Key);
        if (Key.RootAppliedHash != RootAppliedHash ||
            Key.ActorClassRegistryRevision != RegistryRevision ||
            Key.ImporterVersion != MHStaticMeshImporterVersion ||
            Entry->RootSourceHash != RootSourceHash)
        {
            RemoveDefinitionByKey(Key);
            continue;
        }
        if (!MHValidateAppliedCompositeRoot(Root, OutError))
        {
            RemoveDefinitionByKey(Key);
            break;
        }
        MHRecordDefinitionCacheHit();
        OutDependencies = Entry->Dependencies;
        return Entry;
    }

    MHRecordDefinitionCacheMiss();
    const uint64 AdmissionSerial = InvalidationSerial;
    TSharedRef<FMHRandomSourceGraph> Graph = MakeShared<FMHRandomSourceGraph>();
    if (!MHBuildAppliedCompositeGraph(Root, Settings, *Graph, OutDependencies, OutError))
        return nullptr;
    FMHRandomSourceClosure Closure;
    if (!MHBuildRandomSourceClosure(*Graph, Closure, OutError)) return nullptr;

    const uint64 CurrentRegistryRevision = RefreshActorClassRegistryRevision(Settings);
    if (Root.AppliedHash != RootAppliedHash || Root.SourceHash != RootSourceHash ||
        CurrentRegistryRevision != RegistryRevision ||
        WasInvalidatedDuring(AdmissionSerial, OutDependencies))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: definition inputs changed during admission");
        return nullptr;
    }

    FMHCompositeDefinitionKey Key;
    Key.RootResourceKey = RootKey;
    Key.RootAppliedHash = RootAppliedHash;
    Key.ClosureHash = Closure.ClosureHash;
    Key.ActorClassRegistryRevision = RegistryRevision;
    Key.ImporterVersion = MHStaticMeshImporterVersion;
    TSharedRef<FMHCompositeDefinitionEntry> Entry = MakeShared<FMHCompositeDefinitionEntry>();
    Entry->RootObject = &Root;
    Entry->RootSourceHash = RootSourceHash;
    Entry->Graph = Graph;
    Entry->Dependencies = OutDependencies;
    IndexDefinition(Key, *Entry);
    Definitions.Add(MoveTemp(Key), Entry);
    return Entry;
}

void UMHCompositeDefinitionSubsystem::InvalidateDefinition(const FMHResourceKey& ChangedKey)
{
    if (!ChangedKey.IsCanonical()) return;
    ++InvalidationSerial;
    ResourceInvalidationRevisions.Add(ChangedKey, InvalidationSerial);
    // Detaching the bucket first also retires it: every definition that observed
    // ChangedKey is revoked below, so nothing may depend on it afterwards.
    TSet<FMHCompositeDefinitionKey> Dependents;
    if (!DefinitionKeysByDependency.RemoveAndCopyValue(ChangedKey, Dependents)) return;
    for (const FMHCompositeDefinitionKey& Key : Dependents)
    {
        MHRecordDefinitionInvalidationProbe();
        RemoveDefinitionByKey(Key);
    }
}

void UMHCompositeDefinitionSubsystem::InvalidateAllDefinitions()
{
    ++InvalidationSerial;
    GlobalInvalidationRevision = InvalidationSerial;
    ResourceInvalidationRevisions.Reset();
    Definitions.Reset();
    DefinitionKeysByRoot.Reset();
    DefinitionKeysByDependency.Reset();
}
