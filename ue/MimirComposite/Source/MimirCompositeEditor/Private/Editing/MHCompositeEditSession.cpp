#include "Editing/MHCompositeEditSession.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeEditSession)

// CE-1 red stub: the session never opens.

void UMHCompositeEditSession::Open(
    AMHCompositeActor* InRootPlacement,
    UMHCompositeAsset* InEditedAsset,
    const FString& InInvocationPath,
    const UE::MimirComposite::FMHCompositeDocument& InOriginal,
    const uint32 InEpoch)
{
    static_cast<void>(InRootPlacement);
    static_cast<void>(InEditedAsset);
    static_cast<void>(InInvocationPath);
    static_cast<void>(InOriginal);
    static_cast<void>(InEpoch);
}

void UMHCompositeEditSession::Close()
{
    State = EMHCompositeEditSessionState::Closed;
}

EMHCompositeEditSessionState UMHCompositeEditSession::GetState() const
{
    return State;
}

bool UMHCompositeEditSession::IsDirty() const
{
    return false;
}

bool UMHCompositeEditSession::SetNodeTransform(const FGuid& NodeId, const FTransform& LocalTransform, FString& OutError)
{
    static_cast<void>(NodeId);
    static_cast<void>(LocalTransform);
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: edit session arrives with CE-1");
    return false;
}

bool UMHCompositeEditSession::SyncDraftFromLegacyEdit(FString& OutError)
{
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: edit session arrives with CE-1");
    return false;
}
