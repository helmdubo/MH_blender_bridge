#include "Editing/MHCompositeEditProjection.h"

#include "Components/SceneComponent.h"
#include "Editing/MHCompositeEditSession.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeEditProjection)

// CE-2b red stub: the projection never opens.

AMHCompositeEditProjectionActor::AMHCompositeEditProjectionActor()
{
    bIsEditorOnlyActor = true;
    bListedInSceneOutliner = false;
    SetActorHiddenInGame(true);
    PrimaryActorTick.bCanEverTick = false;
    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("MH_EditProjectionRoot"));
    Root->SetMobility(EComponentMobility::Movable);
    SetRootComponent(Root);
}

bool UMHCompositeEditProjection::Open(UMHCompositeEditSession& InSession, FString& OutError)
{
    Session = &InSession;
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the edit projection arrives with CE-2b");
    return false;
}

bool UMHCompositeEditProjection::Refresh(FString& OutError)
{
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the edit projection arrives with CE-2b");
    return false;
}

void UMHCompositeEditProjection::Close()
{
}

FString UMHCompositeEditProjection::GetOriginForComponent(const USceneComponent* Component) const
{
    static_cast<void>(Component);
    return FString();
}

FGuid UMHCompositeEditProjection::GetNodeIdForComponent(const USceneComponent* Component) const
{
    static_cast<void>(Component);
    return FGuid();
}

USceneComponent* UMHCompositeEditProjection::FindComponentForNodeId(const FGuid& NodeId) const
{
    static_cast<void>(NodeId);
    return nullptr;
}

bool UMHCompositeEditProjection::BuildDraftGraph(UE::MimirComposite::FMHRandomSourceGraph& OutGraph, FString& OutError)
{
    static_cast<void>(OutGraph);
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the edit projection arrives with CE-2b");
    return false;
}

USceneComponent* UMHCompositeEditProjection::PlaceComponent(const FString& Origin, UClass* Class, const FMatrix& WorldMatrix, const TFunction<void(USceneComponent&)>& Configure)
{
    static_cast<void>(Origin);
    static_cast<void>(Class);
    static_cast<void>(WorldMatrix);
    static_cast<void>(Configure);
    return nullptr;
}

void UMHCompositeEditProjection::AcquireLease()
{
}
