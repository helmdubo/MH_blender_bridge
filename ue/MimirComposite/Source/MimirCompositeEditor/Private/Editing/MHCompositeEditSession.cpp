#include "Editing/MHCompositeEditSession.h"

#include "Editing/MHCompositeEditProjection.h"
#include "Engine/World.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeEditSession)

using namespace UE::MimirComposite;

void UMHCompositeEditSession::Open(
    AMHCompositeActor* InRootPlacement,
    UMHCompositeAsset* InEditedAsset,
    const FString& InInvocationPath,
    const FMHCompositeDocument& InOriginal,
    const uint32 InEpoch)
{
    RootPlacement = InRootPlacement;
    EditedAsset = InEditedAsset;
    EditorWorld = InRootPlacement != nullptr ? InRootPlacement->GetWorld() : nullptr;
    InvocationPath = InInvocationPath;
    Original = InOriginal;
    OriginalBytes.Reset();
    FString Error;
    MHWriteCanonicalCompositeV5(Original, OriginalBytes, Error);
    Epoch = InEpoch;
    SessionId = FGuid::NewGuid();
    if (InRootPlacement != nullptr)
    {
        // Frozen at open: the draft is evaluated under the placement's own
        // seeds and call context for the whole session (spec §5.1).
        FrozenSeed = InRootPlacement->GetSeed();
        FrozenAppearanceSeed = InRootPlacement->GetAppearanceSeed();
        CallContext = InRootPlacement->GetCallContext();
    }
    // RF_Transactional: every Modify() of the draft joins the open transaction.
    Draft = NewObject<UMHCompositeEditDocument>(this, NAME_None, RF_Transactional);
    // CE-4a: Undo/Redo restore the draft; the projection is derived and follows.
    Draft->OnRestored.BindWeakLambda(this, [this]()
    {
        if (Projection != nullptr && Projection->IsOpen())
        {
            FString RefreshError;
            Projection->Refresh(RefreshError);
        }
    });
    Draft->Load(Original, InEditedAsset != nullptr ? TConstArrayView<FMHPlacementProfile>(InEditedAsset->InlinedPlacementProfiles) : TConstArrayView<FMHPlacementProfile>());
    State = EMHCompositeEditSessionState::EditingClean;
    DirtySerial = 0;
    bDirtyCached = false;
}

void UMHCompositeEditSession::Close()
{
    CloseProjection();
    State = EMHCompositeEditSessionState::Closed;
}

bool UMHCompositeEditSession::OpenProjection(FString& OutError)
{
    if (!IsOpen())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the composite edit session is closed");
        return false;
    }
    if (Projection == nullptr) Projection = NewObject<UMHCompositeEditProjection>(this);
    return Projection->Open(*this, OutError);
}

void UMHCompositeEditSession::CloseProjection()
{
    if (Projection != nullptr)
    {
        Projection->Close();
        Projection = nullptr;
    }
}

EMHCompositeEditSessionState UMHCompositeEditSession::GetState() const
{
    if (State == EMHCompositeEditSessionState::EditingClean || State == EMHCompositeEditSessionState::EditingDirty)
    {
        return IsDirty() ? EMHCompositeEditSessionState::EditingDirty : EMHCompositeEditSessionState::EditingClean;
    }
    return State;
}

bool UMHCompositeEditSession::IsDirty() const
{
    if (Draft == nullptr) return false;
    // Authoring changes only: compare canonical bytes, cached per draft change.
    if (DirtySerial != Draft->GetChangeSerial())
    {
        TArray<uint8> Bytes;
        FString Error;
        bDirtyCached = Draft->CanonicalBytes(Bytes, Error) && Bytes != OriginalBytes;
        DirtySerial = Draft->GetChangeSerial();
    }
    return bDirtyCached;
}

void UMHCompositeEditSession::RebaseOriginal(const FMHCompositeDocument& Committed)
{
    static_cast<void>(Committed);
}

bool UMHCompositeEditSession::SetNodeTransform(const FGuid& NodeId, const FTransform& LocalTransform, FString& OutError)
{
    if (!IsOpen() || Draft == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the composite edit session is closed");
        return false;
    }
    if (!Draft->SetNodeTransform(NodeId, LocalTransform, OutError)) return false;
    // The projection follows the draft; a refresh failure is a preview
    // problem, not an authoring one, so the command still counts.
    if (Projection != nullptr && Projection->IsOpen())
    {
        FString RefreshError;
        Projection->Refresh(RefreshError);
    }
    return true;
}

namespace
{

/** The draft is closed or gone: every structural command is refused the same way. */
bool SessionClosed(const UMHCompositeEditSession& Session, const UMHCompositeEditDocument* Draft, FString& OutError)
{
    if (Session.IsOpen() && Draft != nullptr) return false;
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the composite edit session is closed");
    return true;
}

} // namespace

void UMHCompositeEditSession::RefreshProjection()
{
    // The projection follows the draft; a refresh failure is a preview
    // problem, not an authoring one.
    if (Projection != nullptr && Projection->IsOpen())
    {
        FString RefreshError;
        Projection->Refresh(RefreshError);
    }
}

FGuid UMHCompositeEditSession::AddNode(const FGuid& ParentId, const EMHCompositeNodeKind Kind, const FString& Resource, const FString& Name, const FTransform& LocalTransform, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return FGuid();
    const FGuid Id = Draft->AddNode(ParentId, Kind, Resource, Name, LocalTransform, OutError);
    if (Id.IsValid()) RefreshProjection();
    return Id;
}

bool UMHCompositeEditSession::DeleteNode(const FGuid& NodeId, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    if (!Draft->DeleteNode(NodeId, OutError)) return false;
    RefreshProjection();
    return true;
}

FGuid UMHCompositeEditSession::DuplicateNode(const FGuid& NodeId, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return FGuid();
    const FGuid Id = Draft->DuplicateNode(NodeId, OutError);
    if (Id.IsValid()) RefreshProjection();
    return Id;
}

bool UMHCompositeEditSession::ReparentNode(const FGuid& NodeId, const FGuid& NewParentId, const int32 SiblingIndex, const bool bKeepWorld, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    // Keep-world: the node's rendered world (its projection component) is
    // re-authored under the new parent's world — the new parent's component,
    // or the occurrence (the projection actor's pivot) for a new root.
    TOptional<FTransform> KeptLocal;
    if (bKeepWorld && Projection != nullptr && Projection->IsOpen())
    {
        const USceneComponent* Own = Projection->FindComponentForNodeId(NodeId);
        const USceneComponent* Parent = NewParentId.IsValid() ? Projection->FindComponentForNodeId(NewParentId) : nullptr;
        const AActor* Occurrence = Projection->GetProjectionActor();
        if (Own != nullptr && (Parent != nullptr || (!NewParentId.IsValid() && Occurrence != nullptr)))
        {
            const FTransform ParentWorld = Parent != nullptr ? Parent->GetComponentTransform() : Occurrence->GetActorTransform();
            KeptLocal = Own->GetComponentTransform().GetRelativeTransform(ParentWorld);
        }
    }
    if (!Draft->ReparentNode(NodeId, NewParentId, SiblingIndex, OutError)) return false;
    if (KeptLocal.IsSet()) Draft->SetNodeTransform(NodeId, KeptLocal.GetValue(), OutError);
    RefreshProjection();
    return true;
}

bool UMHCompositeEditSession::SetNodeName(const FGuid& NodeId, const FString& Name, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    if (!Draft->SetNodeName(NodeId, Name, OutError)) return false;
    RefreshProjection();
    return true;
}

bool UMHCompositeEditSession::SetNodeResource(const FGuid& NodeId, const FString& Resource, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    if (!Draft->SetNodeResource(NodeId, Resource, OutError)) return false;
    RefreshProjection();
    return true;
}

FGuid UMHCompositeEditSession::AddRandomNode(const FGuid& ParentId, const FString& Name, const FTransform& LocalTransform, const TArray<FMHCompositeOption>& Options, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return FGuid();
    const FGuid Id = Draft->AddRandomNode(ParentId, Name, LocalTransform, Options, OutError);
    if (Id.IsValid()) RefreshProjection();
    return Id;
}

bool UMHCompositeEditSession::SetNodeOptions(const FGuid& NodeId, const TArray<FMHCompositeOption>& Options, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    if (!Draft->SetNodeOptions(NodeId, Options, OutError)) return false;
    RefreshProjection();
    return true;
}

bool UMHCompositeEditSession::SyncDraftFromLegacyEdit(FString& OutError)
{
    if (!IsOpen() || Draft == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the composite edit session is closed");
        return false;
    }
    const AMHCompositeActor* Root = RootPlacement.Get();
    FMHCompositeDocument Legacy;
    // Nothing admitted yet (or no legacy session): the draft already is the truth.
    if (Root == nullptr || !Root->IsPlacementEditMode() || !Root->GetEditedCompositeDocument(Legacy)) return true;
    for (int32 Index = 0; Index < Legacy.Nodes.Num(); ++Index)
    {
        const int32 DraftIndex = Draft->FindNodeIndexBySelector(FString::Printf(TEXT("nodes[%d]"), Index));
        if (DraftIndex == INDEX_NONE) continue;
        const FMHCompositeTransform& Edited = Legacy.Nodes[Index].Transform;
        const FTransform LegacyTransform(Edited.RotationQuat, Edited.TranslationCm, Edited.Scale);
        if (Draft->GetNodes()[DraftIndex].Transform.Equals(LegacyTransform, 1e-6)) continue;
        if (!Draft->SetNodeTransform(Draft->GetNodeId(DraftIndex), LegacyTransform, OutError)) return false;
    }
    return true;
}
