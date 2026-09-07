#include "Editing/MHCompositeEditSession.h"

#include "Composite/MHCompositeTransformAdmission.h"
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
    SelectedNodeIds.Reset();
    ActiveNodeId.Invalidate();
    PreviewError.Reset();
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
        if (!IsOpen()) return;
        PruneSelection();
        RefreshProjection();
        OnChanged.Broadcast();
    });
    Draft->Load(Original, InEditedAsset != nullptr ? TConstArrayView<FMHPlacementProfile>(InEditedAsset->InlinedPlacementProfiles) : TConstArrayView<FMHPlacementProfile>());
    State = EMHCompositeEditSessionState::EditingClean;
    DirtySerial = 0;
    bDirtyCached = false;
    bDirtyCacheValid = false;
}

void UMHCompositeEditSession::Close()
{
    // Selection notifications during projection teardown must not reselect
    // the actor that is being destroyed. Close command admission first.
    State = EMHCompositeEditSessionState::Closed;
    CloseProjection();
    if (!SelectedNodeIds.IsEmpty())
    {
        SelectedNodeIds.Reset();
        ActiveNodeId.Invalidate();
        OnSelectionChanged.Broadcast();
    }
}

bool UMHCompositeEditSession::OpenProjection(FString& OutError)
{
    if (!IsOpen())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the composite edit session is closed");
        return false;
    }
    if (Projection == nullptr) Projection = NewObject<UMHCompositeEditProjection>(this);
    const bool bOpened = Projection->Open(*this, OutError);
    PreviewError = bOpened ? FString() : OutError;
    return bOpened;
}

void UMHCompositeEditSession::CloseProjection()
{
    if (Projection != nullptr)
    {
        Projection->Close();
        Projection = nullptr;
    }
    PreviewError.Reset();
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
    if (!bDirtyCacheValid || DirtySerial != Draft->GetChangeSerial())
    {
        TArray<uint8> Bytes;
        FString Error;
        // Invalid draft state must never masquerade as clean and disappear on
        // Cancel/close paths that consult dirty state.
        bDirtyCached = !Draft->CanonicalBytes(Bytes, Error) || Bytes != OriginalBytes;
        DirtySerial = Draft->GetChangeSerial();
        bDirtyCacheValid = true;
    }
    return bDirtyCached;
}

void UMHCompositeEditSession::RebaseOriginal(const FMHCompositeDocument& Committed)
{
    Original = Committed;
    OriginalBytes.Reset();
    FString Error;
    MHWriteCanonicalCompositeV5(Original, OriginalBytes, Error);
    bDirtyCacheValid = false;
}

void UMHCompositeEditSession::SetSelectedNodeIds(const TArray<FGuid>& NodeIds, const FGuid& RequestedActiveNodeId)
{
    TArray<FGuid> Admitted;
    Admitted.Reserve(NodeIds.Num());
    TSet<FGuid> Seen;
    if (Draft != nullptr && IsOpen())
    {
        for (const FGuid& NodeId : NodeIds)
        {
            if (!NodeId.IsValid() || Seen.Contains(NodeId) || Draft->FindNodeIndex(NodeId) == INDEX_NONE) continue;
            Seen.Add(NodeId);
            Admitted.Add(NodeId);
        }
    }
    FGuid AdmittedActive;
    if (Admitted.Contains(RequestedActiveNodeId))
    {
        AdmittedActive = RequestedActiveNodeId;
    }
    else if (!Admitted.IsEmpty())
    {
        AdmittedActive = Admitted[0];
    }
    if (SelectedNodeIds == Admitted && ActiveNodeId == AdmittedActive) return;
    SelectedNodeIds = MoveTemp(Admitted);
    ActiveNodeId = AdmittedActive;
    OnSelectionChanged.Broadcast();
}

void UMHCompositeEditSession::PruneSelection()
{
    SetSelectedNodeIds(SelectedNodeIds, ActiveNodeId);
}

void UMHCompositeEditSession::FinishAuthoringCommand(const bool bStructureChanged)
{
    if (bStructureChanged) PruneSelection();
    RefreshProjection();
    OnChanged.Broadcast();
}

bool UMHCompositeEditSession::SetNodeTransforms(const TArray<FGuid>& NodeIds, const TArray<FTransform>& LocalTransforms, FString& OutError)
{
    if (!IsOpen() || Draft == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the composite edit session is closed");
        return false;
    }
    const uint32 RevisionBefore = Draft->GetRevision();
    if (!Draft->SetNodeTransforms(NodeIds, LocalTransforms, OutError)) return false;
    if (Draft->GetRevision() != RevisionBefore) FinishAuthoringCommand();
    return true;
}

bool UMHCompositeEditSession::SetNodeTransform(const FGuid& NodeId, const FTransform& LocalTransform, FString& OutError)
{
    return SetNodeTransforms({NodeId}, {LocalTransform}, OutError);
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
        if (Projection->Refresh(RefreshError)) PreviewError.Reset();
        else PreviewError = MoveTemp(RefreshError);
    }
}

FGuid UMHCompositeEditSession::AddNode(const FGuid& ParentId, const EMHCompositeNodeKind Kind, const FString& Resource, const FString& Name, const FTransform& LocalTransform, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return FGuid();
    const FGuid Id = Draft->AddNode(ParentId, Kind, Resource, Name, LocalTransform, OutError);
    if (Id.IsValid()) FinishAuthoringCommand(true);
    return Id;
}

bool UMHCompositeEditSession::DeleteNode(const FGuid& NodeId, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    if (!Draft->DeleteNode(NodeId, OutError)) return false;
    FinishAuthoringCommand(true);
    return true;
}

FGuid UMHCompositeEditSession::DuplicateNode(const FGuid& NodeId, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return FGuid();
    const FGuid Id = Draft->DuplicateNode(NodeId, OutError);
    if (Id.IsValid()) FinishAuthoringCommand(true);
    return Id;
}

bool UMHCompositeEditSession::ReparentNode(const FGuid& NodeId, const FGuid& NewParentId, const int32 SiblingIndex, const bool bKeepWorld, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    // Keep-world: the node's rendered world (its projection component) is
    // re-authored under the new parent's world — the new parent's component,
    // or the occurrence (the projection actor's pivot) for a new root.
    TOptional<FTransform> KeptLocal;
    if (bKeepWorld)
    {
        if (Projection == nullptr || !Projection->IsOpen())
        {
            OutError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: keep-world reparent requires an open edit projection frame");
            return false;
        }
        FMHCompositeEditNodeFrame NodeFrame;
        if (!Projection->GetNodeFrame(NodeId, NodeFrame))
        {
            OutError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: keep-world reparent has no frame for the moved node");
            return false;
        }
        FMatrix NewParentWorld;
        if (NewParentId.IsValid())
        {
            FMHCompositeEditNodeFrame ParentFrame;
            if (!Projection->GetNodeFrame(NewParentId, ParentFrame))
            {
                OutError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: keep-world reparent has no frame for the new parent");
                return false;
            }
            NewParentWorld = ParentFrame.WorldMatrix;
        }
        else
        {
            FMHCompositeEditNodeFrame RootFrame = NodeFrame;
            while (RootFrame.ParentNodeId.IsValid())
            {
                FMHCompositeEditNodeFrame ParentFrame;
                if (!Projection->GetNodeFrame(RootFrame.ParentNodeId, ParentFrame))
                {
                    OutError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: keep-world reparent cannot resolve the occurrence frame");
                    return false;
                }
                RootFrame = MoveTemp(ParentFrame);
            }
            NewParentWorld = RootFrame.ParentWorldMatrix;
        }
        const double ParentDeterminant = NewParentWorld.Determinant();
        if (!FMath::IsFinite(ParentDeterminant) || ParentDeterminant == 0.0)
        {
            OutError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: keep-world parent frame is singular or non-finite");
            return false;
        }
        const FMatrix LocalMatrix = NodeFrame.WorldMatrix * NewParentWorld.Inverse();
        if (!MHIsRepresentableTransformMatrix(LocalMatrix))
        {
            OutError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: keep-world local matrix cannot round-trip through FTransform within 8 float32 ULP");
            return false;
        }
        KeptLocal = FTransform(LocalMatrix);
        const int32 NodeIndex = Draft->FindNodeIndex(NodeId);
        if (NodeIndex == INDEX_NONE)
        {
            OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: unknown session node");
            return false;
        }
        const FMHCompositeAssetNode& AuthoredNode = Draft->GetNodes()[NodeIndex];
        if (!AuthoredNode.Profile.IsEmpty() || AuthoredNode.bHasInlinePlacement)
        {
            OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: procedural profile/p2 transform cannot be changed by keep-world reparent");
            return false;
        }
        if (!UMHCompositeEditDocument::ValidateAuthoredTransform(KeptLocal.GetValue(), OutError)) return false;
    }
    if (!Draft->ReparentNode(NodeId, NewParentId, SiblingIndex, OutError)) return false;
    if (KeptLocal.IsSet() && !Draft->SetNodeTransform(NodeId, KeptLocal.GetValue(), OutError)) return false;
    FinishAuthoringCommand(true);
    return true;
}

bool UMHCompositeEditSession::SetNodeName(const FGuid& NodeId, const FString& Name, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    if (!Draft->SetNodeName(NodeId, Name, OutError)) return false;
    FinishAuthoringCommand();
    return true;
}

bool UMHCompositeEditSession::SetNodeResource(const FGuid& NodeId, const FString& Resource, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    if (!Draft->SetNodeResource(NodeId, Resource, OutError)) return false;
    FinishAuthoringCommand();
    return true;
}

FGuid UMHCompositeEditSession::AddRandomNode(const FGuid& ParentId, const FString& Name, const FTransform& LocalTransform, const TArray<FMHCompositeOption>& Options, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return FGuid();
    const FGuid Id = Draft->AddRandomNode(ParentId, Name, LocalTransform, Options, OutError);
    if (Id.IsValid()) FinishAuthoringCommand(true);
    return Id;
}

bool UMHCompositeEditSession::SetNodeOptions(const FGuid& NodeId, const TArray<FMHCompositeOption>& Options, FString& OutError)
{
    if (SessionClosed(*this, Draft, OutError)) return false;
    if (!Draft->SetNodeOptions(NodeId, Options, OutError)) return false;
    FinishAuthoringCommand();
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
    TArray<FGuid> NodeIds;
    TArray<FTransform> LocalTransforms;
    for (int32 Index = 0; Index < Legacy.Nodes.Num(); ++Index)
    {
        const int32 DraftIndex = Draft->FindNodeIndexBySelector(FString::Printf(TEXT("nodes[%d]"), Index));
        if (DraftIndex == INDEX_NONE) continue;
        const FMHCompositeTransform& Edited = Legacy.Nodes[Index].Transform;
        const FTransform LegacyTransform(Edited.RotationQuat, Edited.TranslationCm, Edited.Scale);
        if (Draft->GetNodes()[DraftIndex].Transform.Equals(LegacyTransform, 1e-6)) continue;
        NodeIds.Add(Draft->GetNodeId(DraftIndex));
        LocalTransforms.Add(LegacyTransform);
    }
    return SetNodeTransforms(NodeIds, LocalTransforms, OutError);
}
