#include "Composite/MHCompositeDefinitionSubsystem.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Settings/MHCompositeSettings.h"
#include "StaticMesh/MHStaticMeshImportData.h"

using namespace UE::MimirComposite;

void UMHCompositeDefinitionSubsystem::Deinitialize()
{
    Definitions.Reset();
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

void UMHCompositeDefinitionSubsystem::RemoveDeadDefinitions()
{
    for (auto It = Definitions.CreateIterator(); It; ++It)
        if (!It.Value().RootObject.IsValid() || !It.Value().Graph.IsValid()) It.RemoveCurrent();
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

TSharedPtr<const FMHRandomSourceGraph> UMHCompositeDefinitionSubsystem::GetOrBuildDefinition(
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

    for (auto It = Definitions.CreateIterator(); It; ++It)
    {
        const FMHCompositeDefinitionKey& Key = It.Key();
        FMHCompositeDefinitionEntry& Entry = It.Value();
        if (Key.RootResourceKey != RootKey) continue;
        if (Key.RootAppliedHash != RootAppliedHash ||
            Key.ActorClassRegistryRevision != RegistryRevision ||
            Key.ImporterVersion != MHStaticMeshImporterVersion ||
            Entry.RootSourceHash != RootSourceHash)
        {
            It.RemoveCurrent();
            continue;
        }
        if (!MHValidateAppliedCompositeRoot(Root, OutError))
        {
            It.RemoveCurrent();
            break;
        }
        FMHRandomSourceClosure StoredClosure;
        FString ValidationError;
        MHRecordDefinitionClosureHitBuild();
        if (!MHBuildRandomSourceClosure(*Entry.Graph, StoredClosure, ValidationError) ||
            StoredClosure.ClosureHash != Key.ClosureHash)
        {
            It.RemoveCurrent();
            continue;
        }
        OutDependencies = Entry.Dependencies;
        return Entry.Graph;
    }

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
    FMHCompositeDefinitionEntry Entry;
    Entry.RootObject = &Root;
    Entry.RootSourceHash = RootSourceHash;
    Entry.Graph = Graph;
    Entry.Dependencies = OutDependencies;
    Definitions.Add(MoveTemp(Key), MoveTemp(Entry));
    return Graph;
}

void UMHCompositeDefinitionSubsystem::InvalidateDefinition(const FMHResourceKey& ChangedKey)
{
    if (!ChangedKey.IsCanonical()) return;
    ++InvalidationSerial;
    ResourceInvalidationRevisions.Add(ChangedKey, InvalidationSerial);
    for (auto It = Definitions.CreateIterator(); It; ++It)
        if (It.Value().Dependencies.Contains(ChangedKey)) It.RemoveCurrent();
}

void UMHCompositeDefinitionSubsystem::InvalidateAllDefinitions()
{
    ++InvalidationSerial;
    GlobalInvalidationRevision = InvalidationSerial;
    ResourceInvalidationRevisions.Reset();
    Definitions.Reset();
}
