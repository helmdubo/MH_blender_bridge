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
