#include "Composite/MHProofCache.h"

#include "Composite/MHCompositeActor.h"
#include "Editor.h"
#include "Engine/World.h"

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
const TCHAR* const ProofCacheRedError = TEXT("MH_E_NOT_IMPLEMENTED: the proof cache is not implemented (R2c red)");
}

} // namespace UE::MimirComposite

using namespace UE::MimirComposite;

void UMHProofCacheSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
}

void UMHProofCacheSubsystem::Deinitialize()
{
    SourceHashProviderForTests = nullptr;
    Super::Deinitialize();
}

UMHProofCacheSubsystem* UMHProofCacheSubsystem::Get()
{
    return GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHProofCacheSubsystem>() : nullptr;
}

FMHProofResult UMHProofCacheSubsystem::GetProofState(const AMHCompositeActor& Placement) const
{
    return FMHProofResult();
}

EMHProofState UMHProofCacheSubsystem::RequestProof(const AMHCompositeActor& Placement)
{
    return EMHProofState::Unknown;
}

void UMHProofCacheSubsystem::FlushPendingProofs()
{
}

bool UMHProofCacheSubsystem::BuildProofNow(const AMHCompositeActor& Placement, FMHProofResult& OutResult, FString& OutError)
{
    OutResult = FMHProofResult();
    OutError = ProofCacheRedError;
    return false;
}

TArray<FMHProofAuditRow> UMHProofCacheSubsystem::AuditWorld(const UWorld& World) const
{
    return {};
}

void UMHProofCacheSubsystem::InvalidateAll()
{
}

void UMHProofCacheSubsystem::SetSourceHashProviderForTests(TFunction<bool(const FMHResourceKey&, FString&)> Provider)
{
    SourceHashProviderForTests = MoveTemp(Provider);
}
