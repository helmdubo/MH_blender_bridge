#include "Composite/MHProofCache.h"

#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Logging/MessageLog.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/ObjectSaveContext.h"

namespace UE::MimirComposite
{

const TCHAR* MHProofStateLabel(const EMHProofState State)
{
    switch (State)
    {
    case EMHProofState::Unknown: return TEXT("Unknown");
    case EMHProofState::ProofPending: return TEXT("ProofPending");
    case EMHProofState::Fresh: return TEXT("Fresh");
    case EMHProofState::Stale: return TEXT("Stale");
    case EMHProofState::Missing: return TEXT("Missing");
    }
    return TEXT("Unknown");
}

namespace
{

FMHResourceKey ProofRootKey(const AMHCompositeActor& Placement)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    if (const UMHCompositeAsset* Root = Placement.GetCompositeAsset())
    {
        Key.LogicalName = Root->LogicalName;
    }
    return Key;
}

const TCHAR* ProofWarningCode(const EMHProofState State)
{
    switch (State)
    {
    case EMHProofState::Unknown: return TEXT("MH_W_PROOF_UNKNOWN");
    case EMHProofState::ProofPending: return TEXT("MH_W_PROOF_PENDING");
    case EMHProofState::Stale: return TEXT("MH_W_PROOF_STALE");
    case EMHProofState::Missing: return TEXT("MH_W_PROOF_MISSING");
    case EMHProofState::Fresh: break;
    }
    return TEXT("MH_W_PROOF_UNKNOWN");
}

} // namespace

} // namespace UE::MimirComposite

using namespace UE::MimirComposite;

struct UMHProofCacheSubsystem::FImpl
{
    struct FEndpointRevision
    {
        FMHResourceKey Key;
        uint32 Revision = 0;

        friend bool operator==(const FEndpointRevision& A, const FEndpointRevision& B)
        {
            return A.Key == B.Key && A.Revision == B.Revision;
        }
    };

    struct FProofKey
    {
        FObjectKey RootAsset;
        uint32 RecipeRevision = 0;
        int32 Seed = 0;
        int32 AppearanceSeed = 0;
        int64 ProjectIndexGeneration = 0;
        int32 ImporterVersion = 0;
        TArray<FEndpointRevision> EndpointRevisions;

        friend bool operator==(const FProofKey& A, const FProofKey& B)
        {
            return A.RootAsset == B.RootAsset &&
                A.RecipeRevision == B.RecipeRevision &&
                A.Seed == B.Seed &&
                A.AppearanceSeed == B.AppearanceSeed &&
                A.ProjectIndexGeneration == B.ProjectIndexGeneration &&
                A.ImporterVersion == B.ImporterVersion &&
                A.EndpointRevisions == B.EndpointRevisions;
        }
    };

    struct FCachedProof
    {
        FProofKey Key;
        FMHProofResult Result;
        TArray<FMHResourceKey> Dependencies;
    };

    FProofKey MakeKey(
        const AMHCompositeActor& Placement,
        const TArray<FMHResourceKey>& Dependencies) const
    {
        FProofKey Key;
        const UMHCompositeAsset* Root = Placement.GetCompositeAsset();
        Key.RootAsset = FObjectKey(Root);
        Key.Seed = Placement.GetSeed();
        Key.AppearanceSeed = Placement.GetAppearanceSeed();
        Key.ImporterVersion = MHStaticMeshImporterVersion;
        if (Root != nullptr)
        {
            if (const UMHCompiledRecipeRegistry* Recipes = UMHCompiledRecipeRegistry::Get())
            {
                Key.RecipeRevision = Recipes->GetRecipeRevision(*Root);
            }
        }
        const TSharedPtr<FMHProjectResourceIndex> Index = MHPeekProjectIndex();
        Key.ProjectIndexGeneration = Index.IsValid() ? Index->GetGeneration() : 0;

        if (const UMHEndpointPrototypeRegistry* Endpoints = UMHEndpointPrototypeRegistry::Get())
        {
            Key.EndpointRevisions.Reserve(Dependencies.Num());
            for (const FMHResourceKey& Dependency : Dependencies)
            {
                FEndpointRevision& Row = Key.EndpointRevisions.AddDefaulted_GetRef();
                Row.Key = Dependency;
                Row.Revision = Endpoints->GetRevision(Dependency);
            }
        }
        return Key;
    }

    TMap<FObjectKey, FCachedProof> Entries;
    TArray<TWeakObjectPtr<const AMHCompositeActor>> Pending;
    FTSTicker::FDelegateHandle TickerHandle;
    FDelegateHandle PreSaveWorldHandle;
};

void UMHProofCacheSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    Impl = MakeShared<FImpl>();
    Impl->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UMHProofCacheSubsystem::TickPendingProofs));
    Impl->PreSaveWorldHandle = FEditorDelegates::PreSaveWorldWithContext.AddUObject(
        this, &UMHProofCacheSubsystem::HandlePreSaveWorld);
}

void UMHProofCacheSubsystem::Deinitialize()
{
    if (Impl.IsValid())
    {
        FTSTicker::RemoveTicker(Impl->TickerHandle);
        FEditorDelegates::PreSaveWorldWithContext.Remove(Impl->PreSaveWorldHandle);
        Impl->Pending.Reset();
        Impl->Entries.Reset();
    }
    Impl.Reset();
    SourceHashProviderForTests = nullptr;
    LastSaveAuditWarningCount = 0;
    Super::Deinitialize();
}

UMHProofCacheSubsystem* UMHProofCacheSubsystem::Get()
{
    return GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHProofCacheSubsystem>() : nullptr;
}

FMHProofResult UMHProofCacheSubsystem::GetProofState(const AMHCompositeActor& Placement) const
{
    if (!Impl.IsValid())
    {
        return FMHProofResult();
    }
    const FImpl::FCachedProof* Cached = Impl->Entries.Find(FObjectKey(&Placement));
    if (Cached == nullptr || !(Cached->Key == Impl->MakeKey(Placement, Cached->Dependencies)))
    {
        return FMHProofResult();
    }
    return Cached->Result;
}

EMHProofState UMHProofCacheSubsystem::RequestProof(const AMHCompositeActor& Placement)
{
    if (!Impl.IsValid())
    {
        return EMHProofState::Unknown;
    }
    const FMHProofResult Existing = GetProofState(Placement);
    if (Existing.State != EMHProofState::Unknown)
    {
        return Existing.State;
    }

    TArray<FMHResourceKey> Dependencies;
    const FMHResourceKey RootKey = ProofRootKey(Placement);
    if (RootKey.IsCanonical())
    {
        Dependencies.Add(RootKey);
    }
    FImpl::FCachedProof PendingEntry;
    PendingEntry.Dependencies = Dependencies;
    PendingEntry.Key = Impl->MakeKey(Placement, Dependencies);
    PendingEntry.Result.State = EMHProofState::ProofPending;
    Impl->Entries.Add(FObjectKey(&Placement), MoveTemp(PendingEntry));
    Impl->Pending.AddUnique(&Placement);
    return EMHProofState::ProofPending;
}

bool UMHProofCacheSubsystem::TickPendingProofs(float)
{
    if (!Impl.IsValid())
    {
        return true;
    }
    while (!Impl->Pending.IsEmpty())
    {
        const TWeakObjectPtr<const AMHCompositeActor> Pending = Impl->Pending[0];
        Impl->Pending.RemoveAt(0);
        if (const AMHCompositeActor* Placement = Pending.Get())
        {
            FMHProofResult Result;
            FString Error;
            BuildProofNow(*Placement, Result, Error);
            break;
        }
    }
    return true;
}

void UMHProofCacheSubsystem::FlushPendingProofs()
{
    if (!Impl.IsValid())
    {
        return;
    }
    while (!Impl->Pending.IsEmpty())
    {
        const TWeakObjectPtr<const AMHCompositeActor> Pending = Impl->Pending[0];
        Impl->Pending.RemoveAt(0);
        if (const AMHCompositeActor* Placement = Pending.Get())
        {
            FMHProofResult Result;
            FString Error;
            BuildProofNow(*Placement, Result, Error);
        }
    }
}

bool UMHProofCacheSubsystem::BuildProofNow(
    const AMHCompositeActor& Placement,
    FMHProofResult& OutResult,
    FString& OutError)
{
    OutResult = FMHProofResult();
    OutError.Reset();
    if (!IsInGameThread())
    {
        OutResult.State = EMHProofState::Missing;
        OutResult.Diagnostic = TEXT("MH_E_INVALID_RESOURCE_SOURCE: proof builds require the game thread");
        OutError = OutResult.Diagnostic;
        return false;
    }
    if (!Impl.IsValid())
    {
        OutResult.State = EMHProofState::Missing;
        OutResult.Diagnostic = TEXT("MH_E_INVALID_RESOURCE_SOURCE: proof cache subsystem is unavailable");
        OutError = OutResult.Diagnostic;
        return false;
    }

    const TSharedPtr<FMHProjectResourceIndex> ProjectIndex = MHPeekProjectIndex();
    TSet<FMHResourceKey> DependencySet;
    if (const FMHResourceKey RootKey = ProofRootKey(Placement); RootKey.IsCanonical())
    {
        DependencySet.Add(RootKey);
    }

    const auto CacheResult = [&](const FMHProofResult& Result)
    {
        TArray<FMHResourceKey> Dependencies = DependencySet.Array();
        Dependencies.Sort([](const FMHResourceKey& A, const FMHResourceKey& B)
        {
            return A.ToString() < B.ToString();
        });
        FImpl::FCachedProof Cached;
        Cached.Dependencies = MoveTemp(Dependencies);
        Cached.Key = Impl->MakeKey(Placement, Cached.Dependencies);
        Cached.Result = Result;
        Impl->Entries.Add(FObjectKey(&Placement), MoveTemp(Cached));
        Impl->Pending.RemoveAll([&Placement](const TWeakObjectPtr<const AMHCompositeActor>& Pending)
        {
            return Pending.Get() == &Placement;
        });
    };
    const auto Missing = [&](const FString& Diagnostic)
    {
        OutResult = FMHProofResult();
        OutResult.State = EMHProofState::Missing;
        OutResult.Diagnostic = Diagnostic;
        OutError = Diagnostic;
        CacheResult(OutResult);
        return false;
    };

    const UMHCompositeAsset* Root = Placement.GetCompositeAsset();
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Root == nullptr || Settings == nullptr)
    {
        return Missing(TEXT("MH_E_INVALID_RESOURCE_SOURCE: placement has no applied composite definition"));
    }

    FMHRandomSourceGraph Graph;
    FString Diagnostic;
    if (!MHBuildAppliedCompositeGraph(*Root, *Settings, Graph, DependencySet, Diagnostic))
    {
        return Missing(Diagnostic);
    }

    TArray<FMHResourceKey> ClaimKeys = DependencySet.Array();
    ClaimKeys.Sort([](const FMHResourceKey& A, const FMHResourceKey& B)
    {
        return A.ToString() < B.ToString();
    });
    for (const FMHResourceKey& Key : ClaimKeys)
    {
        if (!MHCheckGeneratedAssetClaims(Key, Diagnostic))
        {
            return Missing(Diagnostic);
        }
    }

    const TSharedRef<FMHResolvedCompositePlan> Plan = MakeShared<FMHResolvedCompositePlan>();
    if (!MHResolveCompositePlan(
            Graph,
            Placement.GetSeed(),
            Placement.GetAppearanceSeed(),
            *Plan,
            Diagnostic) ||
        !MHValidateResolvedPlacementTransforms(*Plan, Placement.GetActorTransform(), Diagnostic))
    {
        return Missing(Diagnostic);
    }

    OutResult.Plan = Plan;
    for (const FMHResourceKey& Key : ClaimKeys)
    {
        const FString* ReceiptHash = Graph.RawHashes.Find(Key.ToString());
        if (ReceiptHash == nullptr)
        {
            continue;
        }

        FString SourceHash;
        bool bSourceKnown = false;
        if (SourceHashProviderForTests)
        {
            bSourceKnown = SourceHashProviderForTests(Key, SourceHash);
        }
        else if (ProjectIndex.IsValid())
        {
            const FMHResolveOutcome Outcome = ProjectIndex->Resolve(Key);
            if (Outcome.Status == EMHResolveStatus::Resolved)
            {
                SourceHash = Outcome.RawHash;
                bSourceKnown = true;
            }
        }
        if (bSourceKnown && SourceHash != *ReceiptHash)
        {
            OutResult.State = EMHProofState::Stale;
            OutResult.Diagnostic = FString::Printf(
                TEXT("MH_E_STALE_SOURCE: %s receipt %s differs from source %s"),
                *Key.ToString(),
                **ReceiptHash,
                *SourceHash);
            OutError = OutResult.Diagnostic;
            CacheResult(OutResult);
            return false;
        }
    }

    OutResult.State = EMHProofState::Fresh;
    OutResult.Diagnostic.Reset();
    CacheResult(OutResult);
    return true;
}

TArray<FMHProofAuditRow> UMHProofCacheSubsystem::AuditWorld(const UWorld& World) const
{
    TArray<FMHProofAuditRow> Rows;
    for (const ULevel* Level : World.GetLevels())
    {
        if (Level == nullptr)
        {
            continue;
        }
        for (const AActor* Actor : Level->Actors)
        {
            const AMHCompositeActor* Placement = Cast<AMHCompositeActor>(Actor);
            if (!IsValid(Placement) || Placement->IsTemplate() || Placement->IsActorBeingDestroyed())
            {
                continue;
            }
            const FMHProofResult Result = GetProofState(*Placement);
            FMHProofAuditRow& Row = Rows.AddDefaulted_GetRef();
            Row.Placement = Placement;
            Row.State = Result.State;
            Row.Diagnostic = Result.Diagnostic;
        }
    }
    Rows.Sort([](const FMHProofAuditRow& A, const FMHProofAuditRow& B)
    {
        const AMHCompositeActor* PlacementA = A.Placement.Get();
        const AMHCompositeActor* PlacementB = B.Placement.Get();
        return PlacementA != nullptr && PlacementB != nullptr
            ? PlacementA->GetPathName() < PlacementB->GetPathName()
            : PlacementA != nullptr;
    });
    return Rows;
}

void UMHProofCacheSubsystem::InvalidateAll()
{
    if (Impl.IsValid())
    {
        Impl->Entries.Reset();
        Impl->Pending.Reset();
    }
    LastSaveAuditWarningCount = 0;
}

void UMHProofCacheSubsystem::HandlePreSaveWorld(UWorld* World, FObjectPreSaveContext Context)
{
    LastSaveAuditWarningCount = 0;
    if (World == nullptr || Context.IsCooking())
    {
        return;
    }

    const TArray<FMHProofAuditRow> Rows = AuditWorld(*World);
    for (const FMHProofAuditRow& Row : Rows)
    {
        if (Row.State == EMHProofState::Fresh)
        {
            continue;
        }
        ++LastSaveAuditWarningCount;
        const AMHCompositeActor* Placement = Row.Placement.Get();
        FString Warning = FString::Printf(
            TEXT("%s: %s proof is %s"),
            ProofWarningCode(Row.State),
            Placement != nullptr ? *Placement->GetPathName() : TEXT("<expired placement>"),
            MHProofStateLabel(Row.State));
        if (!Row.Diagnostic.IsEmpty())
        {
            Warning += TEXT(": ") + Row.Diagnostic;
        }
        FMessageLog(TEXT("Mimir")).Warning(FText::FromString(Warning));
        if (Row.State == EMHProofState::Unknown && Placement != nullptr)
        {
            RequestProof(*Placement);
        }
    }
}

void UMHProofCacheSubsystem::SetSourceHashProviderForTests(
    TFunction<bool(const FMHResourceKey&, FString&)> Provider)
{
    SourceHashProviderForTests = MoveTemp(Provider);
}
