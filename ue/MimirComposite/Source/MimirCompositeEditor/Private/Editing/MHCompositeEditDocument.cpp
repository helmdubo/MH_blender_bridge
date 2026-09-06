#include "Editing/MHCompositeEditDocument.h"

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

bool UMHCompositeEditDocument::SetNodeTransform(const FGuid& Id, const FTransform& LocalTransform, FString& OutError)
{
    const int32 Index = FindNodeIndex(Id);
    if (Index == INDEX_NONE)
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: unknown session node");
        return false;
    }
    // Modify() first: the reflected draft joins the open transaction as a whole.
    Modify();
    Nodes[Index].Transform = LocalTransform;
    ++Revision;
    MarkChanged();
    return true;
}

namespace
{

bool IsContainerKind(const EMHCompositeNodeKind Kind)
{
    return Kind == EMHCompositeNodeKind::Group;
}

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
    const int32 Parent = ParentId.IsValid() ? FindNodeIndex(ParentId) : INDEX_NONE;
    if (ParentId.IsValid() && Parent == INDEX_NONE) { GrammarError(OutError, TEXT("unknown parent node")); return FGuid(); }
    if (Parent != INDEX_NONE && !IsContainerKind(Nodes[Parent].Kind)) { GrammarError(OutError, TEXT("only a group can take children")); return FGuid(); }
    if (Kind == EMHCompositeNodeKind::Random) { GrammarError(OutError, TEXT("a random node needs options: use the random command")); return FGuid(); }
    if (KindTakesResource(Kind) && !IsCanonicalResource(Resource)) { GrammarError(OutError, TEXT("mesh/actor/composite/gameobj resource must be canonical [a-z0-9_]+")); return FGuid(); }
    if (!KindTakesResource(Kind) && !Resource.IsEmpty()) { GrammarError(OutError, TEXT("group node forbids resource")); return FGuid(); }
    Modify();
    FMHCompositeAssetNode Node;
    Node.ParentIndex = INDEX_NONE;
    Node.Kind = Kind;
    Node.Resource = Resource;
    Node.Name = Name;
    Node.Transform = LocalTransform;
    const FGuid Id = FGuid::NewGuid();
    InsertBlock(Parent == INDEX_NONE ? Nodes.Num() : SubtreeEnd(Parent), Parent, {Node}, {Id});
    ++Revision;
    MarkChanged();
    return Id;
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
    if (NewParent != INDEX_NONE && !IsContainerKind(Nodes[NewParent].Kind)) return GrammarError(OutError, TEXT("only a group can take children"));
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
