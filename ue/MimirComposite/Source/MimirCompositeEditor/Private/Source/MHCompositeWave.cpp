#include "Source/MHCompositeWave.h"

#include "Misc/FileHelper.h"

namespace UE::MimirComposite
{
namespace
{

// Walks Document by value semantics: children are inserted into the result map
// only after their own walk, so no reference into the map survives a rehash.
void WalkComposite(
    IMHSourceResolver& Resolver,
    const FMHCompositeDocument& Document,
    TArray<FString>& Stack,
    FMHCompositeWaveResult& Result)
{
    Stack.Add(Document.Uid);

    for (const FMHCompositeNode& Node : Document.Nodes)
    {
        if (Node.Kind != EMHCompositeNodeKind::CompositeRef)
        {
            continue;
        }
        const FString& ChildUid = Node.ResourceUid;

        if (Stack.Contains(ChildUid))
        {
            TArray<FString> Chain = Stack;
            Chain.Add(ChildUid);
            Result.Errors.Add(FString::Printf(
                TEXT("MH_E_COMPOSITE_CYCLE: %s"),
                *FString::Join(Chain, TEXT(" -> "))));
            continue;
        }
        if (Result.Composites.Contains(ChildUid) || Result.UnresolvedComposites.Contains(ChildUid))
        {
            continue;
        }

        FMHResolveOutcome Outcome = Resolver.Resolve(ChildUid, EMHResourceKind::Composite);
        if (Outcome.Status != EMHResolveStatus::Resolved)
        {
            Result.UnresolvedComposites.Add(ChildUid);
            if (!Outcome.Diagnostic.IsEmpty())
            {
                Result.Errors.Add(Outcome.Diagnostic);
            }
            continue;
        }
        if (!Outcome.Diagnostic.IsEmpty())
        {
            Result.Warnings.Add(Outcome.Diagnostic);
        }

        TArray<uint8> Bytes;
        FMHCompositeDocument Child;
        if (!FFileHelper::LoadFileToArray(Bytes, *Outcome.PayloadPath))
        {
            Result.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: cannot re-read %s"),
                *Outcome.PayloadPath));
            continue;
        }
        const FMHCanonicalResult Parsed = MHParseCompositeV2(Bytes, Child);
        if (!Parsed.bSuccess)
        {
            Result.Errors.Add(FString::Printf(TEXT("%s: %s"), *Outcome.PayloadPath, *Parsed.Error));
            continue;
        }
        if (Child.Uid != ChildUid)
        {
            Result.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: %s no longer declares UID %s"),
                *Outcome.PayloadPath,
                *ChildUid));
            continue;
        }

        Result.PayloadPaths.Add(ChildUid, Outcome.PayloadPath);
        WalkComposite(Resolver, Child, Stack, Result);
        Result.Composites.Add(ChildUid, MoveTemp(Child));
    }

    Stack.Pop();
}

} // namespace

void MHWalkCompositeWave(
    IMHSourceResolver& Resolver,
    const FMHCompositeDocument& Root,
    const FString& RootPayloadPath,
    FMHCompositeWaveResult& OutResult)
{
    OutResult = FMHCompositeWaveResult();
    OutResult.PayloadPaths.Add(Root.Uid, RootPayloadPath);

    TArray<FString> Stack;
    WalkComposite(Resolver, Root, Stack, OutResult);
    OutResult.Composites.Add(Root.Uid, Root);
}

} // namespace UE::MimirComposite
