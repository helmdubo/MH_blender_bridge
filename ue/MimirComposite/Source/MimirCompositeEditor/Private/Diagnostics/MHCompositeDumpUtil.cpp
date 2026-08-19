#include "Diagnostics/MHCompositeDumpUtil.h"

#include "Codec/MHCompositeCodec.h"
#include "Misc/FileHelper.h"
#include "Source/MHCompositeWave.h"
#include "Source/MHPayloadScanResolver.h"
#include "Source/MHSourceResolver.h"

using namespace UE::MimirComposite;

namespace MH::CompositeDump
{
namespace
{

const TCHAR* NodeKindLabel(const EMHCompositeNodeKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeNodeKind::Group: return TEXT("group");
    case EMHCompositeNodeKind::Mesh: return TEXT("mesh");
    default: return TEXT("composite_ref");
    }
}

FString ShortUid(const FString& Uid)
{
    return Uid.Left(8);
}

FString DescribeOutcome(const FMHResolveOutcome& Outcome)
{
    switch (Outcome.Status)
    {
    case EMHResolveStatus::Resolved:
        return Outcome.Diagnostic.IsEmpty()
            ? FString::Printf(TEXT("resolved -> %s"), *Outcome.PayloadPath)
            : FString::Printf(TEXT("resolved (duplicates) -> %s"), *Outcome.PayloadPath);
    case EMHResolveStatus::DivergentRevisions:
        return TEXT("DIVERGENT REVISIONS");
    case EMHResolveStatus::KindMismatch:
        return TEXT("KIND MISMATCH");
    default:
        return TEXT("UNRESOLVED");
    }
}

struct FDumpState
{
    FMHPayloadScanResolver* Resolver = nullptr;
    TMap<FString, FMHResolveOutcome> OutcomeCache;
    FDumpOutput* Output = nullptr;

    const FMHResolveOutcome* ResolveCached(const FString& Uid, const EMHResourceKind Kind)
    {
        if (Resolver == nullptr)
        {
            return nullptr;
        }
        if (const FMHResolveOutcome* Existing = OutcomeCache.Find(Uid))
        {
            return Existing;
        }
        FMHResolveOutcome Outcome = Resolver->Resolve(Uid, Kind);
        if (Outcome.Status != EMHResolveStatus::Resolved && !Outcome.Diagnostic.IsEmpty())
        {
            Output->Errors.AddUnique(Outcome.Diagnostic);
        }
        return &OutcomeCache.Add(Uid, MoveTemp(Outcome));
    }
};

void PrintNodeTree(
    const FMHCompositeDocument& Document,
    const FString& ParentUid,
    const int32 Depth,
    FDumpState& State)
{
    TArray<const FMHCompositeNode*> Children;
    for (const FMHCompositeNode& Node : Document.Nodes)
    {
        if (Node.ParentUid == ParentUid)
        {
            Children.Add(&Node);
        }
    }
    Children.Sort([](const FMHCompositeNode& A, const FMHCompositeNode& B)
    {
        return A.NodeUid < B.NodeUid;
    });

    const FString Indent = FString::ChrN(Depth * 2, TEXT(' '));
    for (const FMHCompositeNode* Node : Children)
    {
        FString Line = FString::Printf(
            TEXT("%s- [%s] %s (node %s)"),
            *Indent,
            NodeKindLabel(Node->Kind),
            *Node->DisplayName,
            *ShortUid(Node->NodeUid));
        if (!Node->ResourceUid.IsEmpty())
        {
            Line += FString::Printf(TEXT(" resource %s"), *ShortUid(Node->ResourceUid));
            const EMHResourceKind Expected = Node->Kind == EMHCompositeNodeKind::Mesh
                ? EMHResourceKind::StaticMesh
                : EMHResourceKind::Composite;
            if (const FMHResolveOutcome* Outcome = State.ResolveCached(Node->ResourceUid, Expected))
            {
                Line += FString::Printf(TEXT(" [%s]"), *DescribeOutcome(*Outcome));
            }
        }
        if (Node->PropertiesJson != TEXT("{}"))
        {
            Line += FString::Printf(TEXT(" props=%s"), *Node->PropertiesJson);
        }
        State.Output->Lines.Add(MoveTemp(Line));
        PrintNodeTree(Document, Node->NodeUid, Depth + 1, State);
    }
}

void PrintComposite(
    const FMHCompositeDocument& Document,
    const FString& PayloadPath,
    FDumpState& State)
{
    State.Output->Lines.Add(FString());
    State.Output->Lines.Add(FString::Printf(
        TEXT("composite %s (%s) nodes=%d file=%s"),
        *Document.Name,
        *ShortUid(Document.Uid),
        Document.Nodes.Num(),
        *PayloadPath));
    if (Document.ResourcePropertiesJson != TEXT("{}"))
    {
        State.Output->Lines.Add(FString::Printf(TEXT("properties=%s"), *Document.ResourcePropertiesJson));
    }
    PrintNodeTree(Document, FString(), 1, State);
}

} // namespace

bool BuildDump(const FString& FilePath, const FString& SourceRoot, FDumpOutput& Out)
{
    Out = FDumpOutput();

    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
    {
        Out.Errors.Add(FString::Printf(TEXT("MH_E_SOURCE_INDEX_INVALID: cannot read %s"), *FilePath));
        return false;
    }
    FMHCompositeDocument Root;
    const FMHCanonicalResult Parsed = MHParseCompositeV2(Bytes, Root);
    if (!Parsed.bSuccess)
    {
        Out.Errors.Add(FString::Printf(TEXT("%s: %s"), *FilePath, *Parsed.Error));
        return false;
    }

    FDumpState State;
    State.Output = &Out;
    TUniquePtr<FMHPayloadScanResolver> Resolver;

    if (!SourceRoot.IsEmpty())
    {
        Resolver = MakeUnique<FMHPayloadScanResolver>(SourceRoot);
        FString Error;
        if (!Resolver->Initialize(Error))
        {
            Out.Errors.Add(Error);
            return false;
        }
        State.Resolver = Resolver.Get();
        Out.Lines.Add(FString::Printf(
            TEXT("scanned %s: %d payload candidates, %d quarantined, %d legacy v1 skipped"),
            *SourceRoot,
            Resolver->GetCandidateFileCount(),
            Resolver->GetQuarantined().Num(),
            Resolver->GetLegacySkipped().Num()));
        for (const FString& Entry : Resolver->GetQuarantined())
        {
            Out.Warnings.Add(FString::Printf(TEXT("quarantined: %s"), *Entry));
        }
        for (const FString& Entry : Resolver->GetLegacySkipped())
        {
            Out.Warnings.Add(FString::Printf(TEXT("legacy v1 (migration only): %s"), *Entry));
        }

        FMHCompositeWaveResult Wave;
        MHWalkCompositeWave(*Resolver, Root, FilePath, Wave);
        PrintComposite(Root, FilePath, State);
        for (const TPair<FString, FMHCompositeDocument>& Pair : Wave.Composites)
        {
            if (Pair.Key != Root.Uid)
            {
                PrintComposite(Pair.Value, Wave.PayloadPaths.FindRef(Pair.Key), State);
            }
        }
        for (const FString& Warning : Wave.Warnings)
        {
            Out.Warnings.AddUnique(Warning);
        }
        for (const FString& WaveError : Wave.Errors)
        {
            Out.Errors.AddUnique(WaveError);
        }
    }
    else
    {
        PrintComposite(Root, FilePath, State);
    }

    return Out.Errors.Num() == 0;
}

} // namespace MH::CompositeDump
