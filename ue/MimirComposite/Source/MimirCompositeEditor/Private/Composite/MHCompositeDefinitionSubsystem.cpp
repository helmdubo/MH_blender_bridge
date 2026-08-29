#include "Composite/MHCompositeDefinitionSubsystem.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositePlacementMetrics.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Engine/StaticMesh.h"
#include "Settings/MHCompositeSettings.h"
#include "StaticMesh/MHStaticMeshImportData.h"

using namespace UE::MimirComposite;

namespace
{
bool MatchesDefinitionEndpointIdentity(const FMHResourceKey& Key, const UObject& Object)
{
    if (Key.Kind != EMHResourceKind::StaticMesh || !Object.IsA<UStaticMesh>()) return false;
    const FString ExpectedPath = FString::Printf(
        TEXT("/Game/MH/Generated/Meshes/%s.%s"), *Key.LogicalName, *Key.LogicalName);
    return Object.GetPathName() == ExpectedPath;
}
} // namespace

UObject* UE::MimirComposite::MHResolveCompositeDefinitionEndpoint(
    FMHCompositeDefinitionEntry& Definition, const FMHResourceKey& Key, FString& OutError)
{
    if (TWeakObjectPtr<UObject>* Cached = Definition.Endpoints.Find(Key))
    {
        if (UObject* Object = Cached->Get();
            Object != nullptr && MatchesDefinitionEndpointIdentity(Key, *Object))
        {
            MHRecordDefinitionEndpointHit();
            return Object;
        }
        MHRecordDefinitionDeadEndpointReload();
        Definition.Endpoints.Remove(Key);
    }

    MHRecordDefinitionEndpointResolve();
    UObject* Object = MHLoadAppliedResource(Key, OutError);
    if (Object != nullptr && MatchesDefinitionEndpointIdentity(Key, *Object))
    {
        Definition.Endpoints.Add(Key, Object);
        MHRecordDefinitionEndpointStore();
    }
    return Object;
}

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
        if (!It.Value().IsValid() || !It.Value()->RootObject.IsValid() ||
            !It.Value()->Graph.IsValid()) It.RemoveCurrent();
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

    for (auto It = Definitions.CreateIterator(); It; ++It)
    {
        const FMHCompositeDefinitionKey& Key = It.Key();
        const TSharedPtr<FMHCompositeDefinitionEntry>& Entry = It.Value();
        if (Key.RootResourceKey != RootKey) continue;
        if (Key.RootAppliedHash != RootAppliedHash ||
            Key.ActorClassRegistryRevision != RegistryRevision ||
            Key.ImporterVersion != MHStaticMeshImporterVersion ||
            Entry->RootSourceHash != RootSourceHash)
        {
            It.RemoveCurrent();
            continue;
        }
        if (!MHValidateAppliedCompositeRoot(Root, OutError))
        {
            It.RemoveCurrent();
            break;
        }
        OutDependencies = Entry->Dependencies;
        return Entry;
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
    TSharedRef<FMHCompositeDefinitionEntry> Entry = MakeShared<FMHCompositeDefinitionEntry>();
    Entry->RootObject = &Root;
    Entry->RootSourceHash = RootSourceHash;
    Entry->Graph = Graph;
    Entry->Dependencies = OutDependencies;
    Definitions.Add(MoveTemp(Key), Entry);
    return Entry;
}

void UMHCompositeDefinitionSubsystem::InvalidateDefinition(const FMHResourceKey& ChangedKey)
{
    if (!ChangedKey.IsCanonical()) return;
    ++InvalidationSerial;
    ResourceInvalidationRevisions.Add(ChangedKey, InvalidationSerial);
    for (auto It = Definitions.CreateIterator(); It; ++It)
        if (It.Value().IsValid() && It.Value()->Dependencies.Contains(ChangedKey)) It.RemoveCurrent();
}

void UMHCompositeDefinitionSubsystem::InvalidateAllDefinitions()
{
    ++InvalidationSerial;
    GlobalInvalidationRevision = InvalidationSerial;
    ResourceInvalidationRevisions.Reset();
    Definitions.Reset();
}
