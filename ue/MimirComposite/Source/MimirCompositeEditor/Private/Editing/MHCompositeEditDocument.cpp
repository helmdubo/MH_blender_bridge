#include "Editing/MHCompositeEditDocument.h"

#include "Composite/MHCompositeTransformAdmission.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeEditDocument)

using namespace UE::MimirComposite;

namespace
{

/** "nodes[1]/children[0]" -> [{nodes,1},{children,0}]; false for anything else (options are not nodes). */
bool ParseSelector(const FString& Selector, TArray<TPair<FString, int32>>& OutParts)
{
    OutParts.Reset();
    TArray<FString> Parts;
    Selector.ParseIntoArray(Parts, TEXT("/"), true);
    for (int32 Index = 0; Index < Parts.Num(); ++Index)
    {
        const FString& Part = Parts[Index];
        int32 Open = INDEX_NONE;
        if (!Part.FindChar(TEXT('['), Open) || !Part.EndsWith(TEXT("]"))) return false;
        const FString Label = Part.Left(Open);
        if (!((Index == 0 && Label == TEXT("nodes")) || (Index > 0 && Label == TEXT("children")))) return false;
        OutParts.Emplace(Label, FCString::Atoi(*Part.Mid(Open + 1, Part.Len() - Open - 2)));
    }
    return !OutParts.IsEmpty();
}

} // namespace

void UMHCompositeEditDocument::Load(const FMHCompositeDocument& Document, const TConstArrayView<FMHPlacementProfile> InInlinedProfiles)
{
    Nodes.Reset();
    MHFlattenCompositeDocument(Document, Nodes);
    NodeIds.Reset(Nodes.Num());
    for (int32 Index = 0; Index < Nodes.Num(); ++Index) NodeIds.Add(FGuid::NewGuid());
    InlinedProfiles.Reset(InInlinedProfiles.Num());
    InlinedProfiles.Append(InInlinedProfiles.GetData(), InInlinedProfiles.Num());
    Revision = 0;
    MarkChanged();
}

bool UMHCompositeEditDocument::Extract(FMHCompositeDocument& OutDocument, FString& OutError) const
{
    if (CacheSerial != ChangeSerial)
    {
        CachedError.Reset();
        if (!MHUnflattenCompositeNodes(Nodes, CachedDocument, CachedError)) CachedDocument = FMHCompositeDocument();
        CacheSerial = ChangeSerial;
    }
    if (!CachedError.IsEmpty())
    {
        OutError = CachedError;
        return false;
    }
    OutDocument = CachedDocument;
    return true;
}

bool UMHCompositeEditDocument::CanonicalBytes(TArray<uint8>& OutBytes, FString& OutError) const
{
    FMHCompositeDocument Document;
    return Extract(Document, OutError) && MHWriteCanonicalCompositeV5(Document, OutBytes, OutError);
}

int32 UMHCompositeEditDocument::FindNodeIndexBySelector(const FString& Selector) const
{
    TArray<TPair<FString, int32>> Parts;
    if (!ParseSelector(Selector, Parts)) return INDEX_NONE;
    int32 Parent = INDEX_NONE;
    for (const TPair<FString, int32>& Part : Parts)
    {
        int32 Ordinal = 0;
        int32 Found = INDEX_NONE;
        for (int32 Index = 0; Index < Nodes.Num(); ++Index)
        {
            if (Nodes[Index].ParentIndex != Parent) continue;
            if (Ordinal++ == Part.Value)
            {
                Found = Index;
                break;
            }
        }
        if (Found == INDEX_NONE) return INDEX_NONE;
        Parent = Found;
    }
    return Parent;
}

FString UMHCompositeEditDocument::GetSelector(const int32 Index) const
{
    if (!Nodes.IsValidIndex(Index)) return FString();
    TArray<FString> Segments;
    for (int32 Current = Index; Current != INDEX_NONE; Current = Nodes[Current].ParentIndex)
    {
        const int32 Parent = Nodes[Current].ParentIndex;
        int32 Ordinal = 0;
        for (int32 Sibling = 0; Sibling < Current; ++Sibling)
        {
            if (Nodes[Sibling].ParentIndex == Parent) ++Ordinal;
        }
        Segments.Insert(FString::Printf(TEXT("%s[%d]"), Parent == INDEX_NONE ? TEXT("nodes") : TEXT("children"), Ordinal), 0);
    }
    return FString::Join(Segments, TEXT("/"));
}

bool UMHCompositeEditDocument::ValidateAuthoredTransform(const FTransform& LocalTransform, FString& OutError)
{
    OutError.Reset();
    const FVector Translation = LocalTransform.GetTranslation();
    const FVector Scale = LocalTransform.GetScale3D();
    const FQuat Rotation = LocalTransform.GetRotation();
    const FVector3f Translation32(Translation);
    const FVector3f Scale32(Scale);
    const FQuat4f Rotation32(Rotation);
    const bool bFinite =
        FMath::IsFinite(Translation.X) && FMath::IsFinite(Translation.Y) && FMath::IsFinite(Translation.Z) &&
        FMath::IsFinite(Scale.X) && FMath::IsFinite(Scale.Y) && FMath::IsFinite(Scale.Z) &&
        FMath::IsFinite(Rotation.X) && FMath::IsFinite(Rotation.Y) &&
        FMath::IsFinite(Rotation.Z) && FMath::IsFinite(Rotation.W) &&
        FMath::IsFinite(Translation32.X) && FMath::IsFinite(Translation32.Y) && FMath::IsFinite(Translation32.Z) &&
        FMath::IsFinite(Scale32.X) && FMath::IsFinite(Scale32.Y) && FMath::IsFinite(Scale32.Z) &&
        FMath::IsFinite(Rotation32.X) && FMath::IsFinite(Rotation32.Y) &&
        FMath::IsFinite(Rotation32.Z) && FMath::IsFinite(Rotation32.W);
    if (!bFinite)
    {
        OutError = TEXT("MH_E_NAN_INF_VALUE: writer received non-finite transform");
        return false;
    }
    if (Scale32.X == 0.0f || Scale32.Y == 0.0f || Scale32.Z == 0.0f)
    {
        OutError = TEXT("MH_E_INVALID_SCALE: composite scale components must be non-zero");
        return false;
    }
    if (!Rotation.IsNormalized() || !Rotation32.IsNormalized())
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: transform rotation_quat must be normalized");
        return false;
    }
    if (!MHIsRepresentableTransformMatrix(LocalTransform.ToMatrixWithScale()))
    {
        OutError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: authored local transform cannot round-trip through FTransform within 8 float32 ULP");
        return false;
    }
    return true;
}

bool UMHCompositeEditDocument::SetNodeTransforms(const TArray<FGuid>& Ids, const TArray<FTransform>& LocalTransforms, FString& OutError)
{
    OutError.Reset();
    if (Ids.Num() != LocalTransforms.Num())
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: transform target and value counts differ");
        return false;
    }
    TArray<int32> Indices;
    Indices.Reserve(Ids.Num());
    TSet<FGuid> Seen;
    bool bChanged = false;
    for (int32 Target = 0; Target < Ids.Num(); ++Target)
    {
        const int32 Index = FindNodeIndex(Ids[Target]);
        if (Index == INDEX_NONE)
        {
            OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: unknown session node");
            return false;
        }
        if (Seen.Contains(Ids[Target]))
        {
            OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: duplicate transform target");
            return false;
        }
        Seen.Add(Ids[Target]);
        if (!Nodes[Index].Profile.IsEmpty() || Nodes[Index].bHasInlinePlacement)
        {
            OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: procedural profile/p2 transform cannot be changed by an ordinary transform command");
            return false;
        }
        if (!ValidateAuthoredTransform(LocalTransforms[Target], OutError)) return false;
        Indices.Add(Index);
        bChanged |= !Nodes[Index].Transform.Equals(LocalTransforms[Target], 0.0);
    }
    if (!bChanged) return true;

    // Every target has passed admission. The reflected draft joins the open
    // transaction once and readers can never observe a partially applied batch.
    Modify();
    for (int32 Target = 0; Target < Indices.Num(); ++Target)
    {
        if (!Nodes[Indices[Target]].Transform.Equals(LocalTransforms[Target], 0.0))
        {
            Nodes[Indices[Target]].Transform = LocalTransforms[Target];
        }
    }
    ++Revision;
    MarkChanged();
    return true;
}

bool UMHCompositeEditDocument::SetNodeTransform(const FGuid& Id, const FTransform& LocalTransform, FString& OutError)
{
    return SetNodeTransforms({Id}, {LocalTransform}, OutError);
}

namespace
{

bool KindTakesResource(const EMHCompositeNodeKind Kind)
{
    return Kind == EMHCompositeNodeKind::Mesh || Kind == EMHCompositeNodeKind::Actor ||
        Kind == EMHCompositeNodeKind::Composite || Kind == EMHCompositeNodeKind::GameObj;
}

bool IsCanonicalResource(const FString& Resource)
{
    if (Resource.IsEmpty()) return false;
    for (const TCHAR Char : Resource)
    {
        if (!((Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_'))) return false;
    }
    return true;
}

bool NodeKindToOptionKind(const EMHCompositeNodeKind Kind, EMHCompositeOptionKind& OutKind)
{
    switch (Kind)
    {
    case EMHCompositeNodeKind::Mesh: OutKind = EMHCompositeOptionKind::Mesh; return true;
    case EMHCompositeNodeKind::Actor: OutKind = EMHCompositeOptionKind::Actor; return true;
    case EMHCompositeNodeKind::Composite: OutKind = EMHCompositeOptionKind::Composite; return true;
    case EMHCompositeNodeKind::GameObj: OutKind = EMHCompositeOptionKind::GameObj; return true;
    default: return false;
    }
}

bool GrammarError(FString& OutError, const TCHAR* Message)
{
    OutError = FString(TEXT("MH_E_COMPOSITE_GRAMMAR: ")) + Message;
    return false;
}

} // namespace

int32 UMHCompositeEditDocument::SubtreeEnd(const int32 Index) const
{
    // Pre-order: a subtree is the contiguous range whose parents lie inside it.
    int32 End = Index + 1;
    while (End < Nodes.Num() && Nodes[End].ParentIndex >= Index && Nodes[End].ParentIndex < End) ++End;
    return End;
}

void UMHCompositeEditDocument::InsertBlock(const int32 At, const int32 ParentIndex, TArray<FMHCompositeAssetNode> Block, TArray<FGuid> Ids)
{
    // Block parents are relative (INDEX_NONE for the block's root); nodes at
    // or after the insertion point move by the block's size.
    const int32 Count = Block.Num();
    for (FMHCompositeAssetNode& Node : Nodes)
    {
        if (Node.ParentIndex >= At) Node.ParentIndex += Count;
    }
    for (FMHCompositeAssetNode& Node : Block)
    {
        Node.ParentIndex = Node.ParentIndex == INDEX_NONE ? ParentIndex : Node.ParentIndex + At;
    }
    Nodes.Insert(Block, At);
    NodeIds.Insert(Ids, At);
}

void UMHCompositeEditDocument::ExtractBlock(const int32 Index, TArray<FMHCompositeAssetNode>& OutBlock, TArray<FGuid>& OutIds)
{
    const int32 End = SubtreeEnd(Index);
    const int32 Count = End - Index;
    OutBlock.Reset(Count);
    OutIds.Reset(Count);
    for (int32 Current = Index; Current < End; ++Current)
    {
        FMHCompositeAssetNode Node = Nodes[Current];
        Node.ParentIndex = Current == Index ? INDEX_NONE : Node.ParentIndex - Index;
        OutBlock.Add(MoveTemp(Node));
        OutIds.Add(NodeIds[Current]);
    }
    Nodes.RemoveAt(Index, Count);
    NodeIds.RemoveAt(Index, Count);
    for (FMHCompositeAssetNode& Node : Nodes)
    {
        if (Node.ParentIndex >= End) Node.ParentIndex -= Count;
    }
}

FGuid UMHCompositeEditDocument::AddNode(const FGuid& ParentId, const EMHCompositeNodeKind Kind, const FString& Resource, const FString& Name, const FTransform& LocalTransform, FString& OutError)
{
    FMHCompositeNodeAdd Request;
    Request.Kind = Kind;
    Request.Resource = Resource;
    Request.Name = Name;
    Request.LocalTransform = LocalTransform;
    TArray<FGuid> Ids;
    return AddNodes(ParentId, MakeArrayView(&Request, 1), Ids, OutError) && Ids.Num() == 1 ? Ids[0] : FGuid();
}

bool UMHCompositeEditDocument::AddNodes(
    const FGuid& ParentId,
    const TConstArrayView<FMHCompositeNodeAdd> Requests,
    TArray<FGuid>& OutIds,
    FString& OutError,
    const int32 SiblingIndex)
{
    OutIds.Reset();
    OutError.Reset();
    const int32 Parent = ParentId.IsValid() ? FindNodeIndex(ParentId) : INDEX_NONE;
    if (ParentId.IsValid() && Parent == INDEX_NONE) return GrammarError(OutError, TEXT("unknown parent node"));

    const TArray<FGuid> Siblings = GetChildIds(ParentId);
    if (SiblingIndex < INDEX_NONE || SiblingIndex > Siblings.Num())
    {
        return GrammarError(OutError, TEXT("sibling insertion index is out of range"));
    }
    const int32 InsertAt = SiblingIndex != INDEX_NONE && SiblingIndex < Siblings.Num()
        ? FindNodeIndex(Siblings[SiblingIndex])
        : (Parent == INDEX_NONE ? Nodes.Num() : SubtreeEnd(Parent));

    for (const FMHCompositeNodeAdd& Request : Requests)
    {
        if (Request.Kind == EMHCompositeNodeKind::Random)
        {
            return GrammarError(OutError, TEXT("a random node needs options: use the random command"));
        }
        if (KindTakesResource(Request.Kind))
        {
            if (!IsCanonicalResource(Request.Resource))
            {
                return GrammarError(OutError, TEXT("mesh/actor/composite/gameobj resource must be canonical [a-z0-9_]+"));
            }
        }
        else if (Request.Kind != EMHCompositeNodeKind::Group)
        {
            return GrammarError(OutError, TEXT("unsupported authored node kind"));
        }
        else if (!Request.Resource.IsEmpty())
        {
            return GrammarError(OutError, TEXT("group node forbids resource"));
        }
        if (!ValidateAuthoredTransform(Request.LocalTransform, OutError)) return false;
    }
    if (Requests.IsEmpty()) return true;

    TArray<FMHCompositeAssetNode> Block;
    TArray<FGuid> Ids;
    Block.Reserve(Requests.Num());
    Ids.Reserve(Requests.Num());
    for (const FMHCompositeNodeAdd& Request : Requests)
    {
        FMHCompositeAssetNode& Node = Block.AddDefaulted_GetRef();
        Node.ParentIndex = INDEX_NONE;
        Node.Kind = Request.Kind;
        Node.Resource = Request.Resource;
        Node.Name = Request.Name;
        Node.Transform = Request.LocalTransform;
        Ids.Add(FGuid::NewGuid());
    }

    Modify();
    InsertBlock(InsertAt, Parent, MoveTemp(Block), Ids);
    ++Revision;
    MarkChanged();
    OutIds = MoveTemp(Ids);
    return true;
}

bool UMHCompositeEditDocument::DeleteNode(const FGuid& Id, FString& OutError)
{
    const int32 Index = FindNodeIndex(Id);
    if (Index == INDEX_NONE) return GrammarError(OutError, TEXT("unknown session node"));
    Modify();
    TArray<FMHCompositeAssetNode> Block;
    TArray<FGuid> Ids;
    ExtractBlock(Index, Block, Ids);
    ++Revision;
    MarkChanged();
    return true;
}

FGuid UMHCompositeEditDocument::DuplicateNode(const FGuid& Id, FString& OutError)
{
    const int32 Index = FindNodeIndex(Id);
    if (Index == INDEX_NONE) { GrammarError(OutError, TEXT("unknown session node")); return FGuid(); }
    const int32 End = SubtreeEnd(Index);
    TArray<FMHCompositeAssetNode> Block;
    TArray<FGuid> Ids;
    for (int32 Current = Index; Current < End; ++Current)
    {
        FMHCompositeAssetNode Node = Nodes[Current];
        Node.ParentIndex = Current == Index ? INDEX_NONE : Node.ParentIndex - Index;
        Block.Add(MoveTemp(Node));
        Ids.Add(FGuid::NewGuid());
    }
    Modify();
    InsertBlock(End, Nodes[Index].ParentIndex, MoveTemp(Block), Ids);
    ++Revision;
    MarkChanged();
    return Ids[0];
}

bool UMHCompositeEditDocument::ReparentNode(const FGuid& Id, const FGuid& NewParentId, const int32 SiblingIndex, FString& OutError)
{
    const int32 Index = FindNodeIndex(Id);
    if (Index == INDEX_NONE) return GrammarError(OutError, TEXT("unknown session node"));
    int32 NewParent = NewParentId.IsValid() ? FindNodeIndex(NewParentId) : INDEX_NONE;
    if (NewParentId.IsValid() && NewParent == INDEX_NONE) return GrammarError(OutError, TEXT("unknown parent node"));
    const int32 End = SubtreeEnd(Index);
    if (NewParent >= Index && NewParent < End) return GrammarError(OutError, TEXT("a node cannot be moved under itself or its descendants"));
    Modify();
    TArray<FMHCompositeAssetNode> Block;
    TArray<FGuid> Ids;
    ExtractBlock(Index, Block, Ids);
    if (NewParent >= End) NewParent -= Block.Num();
    // The insertion point: the sibling that will follow the block, or the end of the parent's subtree.
    int32 At = NewParent == INDEX_NONE ? Nodes.Num() : SubtreeEnd(NewParent);
    if (SiblingIndex != INDEX_NONE)
    {
        int32 Ordinal = 0;
        for (int32 Current = 0; Current < Nodes.Num(); ++Current)
        {
            if (Nodes[Current].ParentIndex != NewParent) continue;
            if (Ordinal++ == SiblingIndex) { At = Current; break; }
        }
    }
    InsertBlock(At, NewParent, MoveTemp(Block), MoveTemp(Ids));
    ++Revision;
    MarkChanged();
    return true;
}

bool UMHCompositeEditDocument::SetNodeName(const FGuid& Id, const FString& Name, FString& OutError)
{
    const int32 Index = FindNodeIndex(Id);
    if (Index == INDEX_NONE) return GrammarError(OutError, TEXT("unknown session node"));
    Modify();
    Nodes[Index].Name = Name;
    ++Revision;
    MarkChanged();
    return true;
}

bool UMHCompositeEditDocument::SetNodeResource(const FGuid& Id, const FString& Resource, FString& OutError)
{
    const int32 Index = FindNodeIndex(Id);
    if (Index == INDEX_NONE) return GrammarError(OutError, TEXT("unknown session node"));
    if (!KindTakesResource(Nodes[Index].Kind)) return GrammarError(OutError, TEXT("group/random node forbids resource"));
    if (!IsCanonicalResource(Resource)) return GrammarError(OutError, TEXT("mesh/actor/composite/gameobj resource must be canonical [a-z0-9_]+"));
    Modify();
    Nodes[Index].Resource = Resource;
    ++Revision;
    MarkChanged();
    return true;
}

bool UMHCompositeEditDocument::ValidateOptions(const TArray<FMHCompositeOption>& Options, FString& OutError)
{
    if (Options.IsEmpty()) return GrammarError(OutError, TEXT("random requires a non-empty options array"));
    bool bPositive = false;
    for (const FMHCompositeOption& Option : Options)
    {
        if (!FMath::IsFinite(Option.Weight) || Option.Weight < 0.0f) return GrammarError(OutError, TEXT("random option weight must be finite and non-negative"));
        bPositive |= Option.Weight > 0.0f;
        switch (Option.Kind)
        {
        case EMHCompositeOptionKind::Empty:
            if (!Option.Resource.IsEmpty()) return GrammarError(OutError, TEXT("empty option forbids resource"));
            break;
        case EMHCompositeOptionKind::Mesh:
        case EMHCompositeOptionKind::Actor:
        case EMHCompositeOptionKind::Composite:
        case EMHCompositeOptionKind::GameObj:
            if (!IsCanonicalResource(Option.Resource)) return GrammarError(OutError, TEXT("non-empty option requires canonical resource"));
            break;
        default:
            return GrammarError(OutError, TEXT("unsupported random option kind"));
        }
    }
    if (!bPositive) return GrammarError(OutError, TEXT("random requires at least one positive option weight"));
    return true;
}

FGuid UMHCompositeEditDocument::AddRandomNode(const FGuid& ParentId, const FString& Name, const FTransform& LocalTransform, const TArray<FMHCompositeOption>& Options, FString& OutError)
{
    const int32 Parent = ParentId.IsValid() ? FindNodeIndex(ParentId) : INDEX_NONE;
    if (ParentId.IsValid() && Parent == INDEX_NONE) { GrammarError(OutError, TEXT("unknown parent node")); return FGuid(); }
    if (!ValidateOptions(Options, OutError)) return FGuid();
    if (!ValidateAuthoredTransform(LocalTransform, OutError)) return FGuid();
    Modify();
    FMHCompositeAssetNode Node;
    Node.ParentIndex = INDEX_NONE;
    Node.Kind = EMHCompositeNodeKind::Random;
    Node.Name = Name;
    Node.Transform = LocalTransform;
    Node.Options = Options;
    const FGuid Id = FGuid::NewGuid();
    InsertBlock(Parent == INDEX_NONE ? Nodes.Num() : SubtreeEnd(Parent), Parent, {Node}, {Id});
    ++Revision;
    MarkChanged();
    return Id;
}

bool UMHCompositeEditDocument::SetNodeOptions(const FGuid& Id, const TArray<FMHCompositeOption>& Options, FString& OutError)
{
    const int32 Index = FindNodeIndex(Id);
    if (Index == INDEX_NONE) return GrammarError(OutError, TEXT("unknown session node"));
    if (Nodes[Index].Kind != EMHCompositeNodeKind::Random) return GrammarError(OutError, TEXT("options are allowed only on random nodes"));
    if (!ValidateOptions(Options, OutError)) return false;
    Modify();
    Nodes[Index].Options = Options;
    ++Revision;
    MarkChanged();
    return true;
}

bool UMHCompositeEditDocument::AddNodeOptions(
    const FGuid& NodeId,
    const TConstArrayView<FMHCompositeOption> Options,
    FString& OutError)
{
    OutError.Reset();
    const int32 Index = FindNodeIndex(NodeId);
    if (Index == INDEX_NONE) return GrammarError(OutError, TEXT("unknown session node"));
    if (Options.IsEmpty()) return true;

    TArray<FMHCompositeOption> NextOptions;
    const FMHCompositeAssetNode& Node = Nodes[Index];
    if (Node.Kind == EMHCompositeNodeKind::Random)
    {
        NextOptions = Node.Options;
    }
    else if (Node.Kind != EMHCompositeNodeKind::Group)
    {
        EMHCompositeOptionKind PreviousKind;
        if (!NodeKindToOptionKind(Node.Kind, PreviousKind)) return GrammarError(OutError, TEXT("node content cannot be converted to random"));
        FMHCompositeOption& Previous = NextOptions.AddDefaulted_GetRef();
        Previous.Kind = PreviousKind;
        Previous.Resource = Node.Resource;
        Previous.Weight = 1.0f;
    }
    NextOptions.Append(Options.GetData(), Options.Num());
    if (!ValidateOptions(NextOptions, OutError)) return false;

    Modify();
    Nodes[Index].Kind = EMHCompositeNodeKind::Random;
    Nodes[Index].Resource.Reset();
    Nodes[Index].Options = MoveTemp(NextOptions);
    ++Revision;
    MarkChanged();
    return true;
}

bool UMHCompositeEditDocument::SetNodeOptionWeight(
    const FGuid& NodeId,
    const int32 OptionIndex,
    const float Weight,
    FString& OutError)
{
    OutError.Reset();
    const int32 Index = FindNodeIndex(NodeId);
    if (Index == INDEX_NONE) return GrammarError(OutError, TEXT("unknown session node"));
    if (Nodes[Index].Kind != EMHCompositeNodeKind::Random) return GrammarError(OutError, TEXT("options are allowed only on random nodes"));
    if (!Nodes[Index].Options.IsValidIndex(OptionIndex)) return GrammarError(OutError, TEXT("random option index is out of range"));

    TArray<FMHCompositeOption> NextOptions = Nodes[Index].Options;
    NextOptions[OptionIndex].Weight = Weight;
    if (!ValidateOptions(NextOptions, OutError)) return false;
    if (Nodes[Index].Options[OptionIndex].Weight == Weight) return true;

    Modify();
    Nodes[Index].Options = MoveTemp(NextOptions);
    ++Revision;
    MarkChanged();
    return true;
}

bool UMHCompositeEditDocument::RemoveNodeOption(const FGuid& NodeId, const int32 OptionIndex, FString& OutError)
{
    OutError.Reset();
    const int32 Index = FindNodeIndex(NodeId);
    if (Index == INDEX_NONE) return GrammarError(OutError, TEXT("unknown session node"));
    if (Nodes[Index].Kind != EMHCompositeNodeKind::Random) return GrammarError(OutError, TEXT("options are allowed only on random nodes"));
    if (!Nodes[Index].Options.IsValidIndex(OptionIndex)) return GrammarError(OutError, TEXT("random option index is out of range"));

    TArray<FMHCompositeOption> NextOptions = Nodes[Index].Options;
    NextOptions.RemoveAt(OptionIndex);
    if (!NextOptions.IsEmpty() && !ValidateOptions(NextOptions, OutError)) return false;

    Modify();
    if (NextOptions.IsEmpty())
    {
        Nodes[Index].Kind = EMHCompositeNodeKind::Group;
        Nodes[Index].Resource.Reset();
        Nodes[Index].Options.Reset();
    }
    else
    {
        Nodes[Index].Options = MoveTemp(NextOptions);
    }
    ++Revision;
    MarkChanged();
    return true;
}

FGuid UMHCompositeEditDocument::GetParentId(const FGuid& Id) const
{
    const int32 Index = FindNodeIndex(Id);
    return Index != INDEX_NONE ? GetNodeId(Nodes[Index].ParentIndex) : FGuid();
}

TArray<FGuid> UMHCompositeEditDocument::GetChildIds(const FGuid& ParentId) const
{
    TArray<FGuid> Result;
    const int32 Parent = ParentId.IsValid() ? FindNodeIndex(ParentId) : INDEX_NONE;
    if (ParentId.IsValid() && Parent == INDEX_NONE) return Result;
    for (int32 Index = 0; Index < Nodes.Num(); ++Index)
    {
        if (Nodes[Index].ParentIndex == Parent) Result.Add(NodeIds[Index]);
    }
    return Result;
}

void UMHCompositeEditDocument::PostEditUndo()
{
    Super::PostEditUndo();
    // The reflected state was replaced behind the cache's back.
    MarkChanged();
    OnRestored.ExecuteIfBound();
}

void UMHCompositeEditDocument::MarkChanged()
{
    ++ChangeSerial;
}
