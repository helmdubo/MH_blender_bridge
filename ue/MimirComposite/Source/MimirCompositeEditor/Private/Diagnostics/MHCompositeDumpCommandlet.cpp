#include "Diagnostics/MHCompositeDumpCommandlet.h"

#include "Codec/MHCompositeCodec.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Source/MHCompositeWave.h"
#include "Source/MHPayloadScanResolver.h"
#include "Source/MHSourceResolver.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeDumpCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogMHCompositeDump, Display, All);

using namespace UE::MimirComposite;

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
    bool bAnyError = false;

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
        if (Outcome.Status != EMHResolveStatus::Resolved)
        {
            bAnyError = true;
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
        UE_LOG(LogMHCompositeDump, Display, TEXT("%s"), *Line);
        PrintNodeTree(Document, Node->NodeUid, Depth + 1, State);
    }
}

void PrintComposite(
    const FMHCompositeDocument& Document,
    const FString& PayloadPath,
    FDumpState& State)
{
    UE_LOG(LogMHCompositeDump, Display, TEXT(""));
    UE_LOG(
        LogMHCompositeDump,
        Display,
        TEXT("composite %s (%s) nodes=%d file=%s"),
        *Document.Name,
        *ShortUid(Document.Uid),
        Document.Nodes.Num(),
        *PayloadPath);
    if (Document.ResourcePropertiesJson != TEXT("{}"))
    {
        UE_LOG(LogMHCompositeDump, Display, TEXT("properties=%s"), *Document.ResourcePropertiesJson);
    }
    PrintNodeTree(Document, FString(), 1, State);
}

} // namespace

UMHCompositeDumpCommandlet::UMHCompositeDumpCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
    ShowErrorCount = true;
    UseCommandletResultAsExitCode = true;
}

int32 UMHCompositeDumpCommandlet::Main(const FString& Params)
{
    FString FilePath;
    const TCHAR* Cursor = *Params;
    FString Token;
    while (FParse::Token(Cursor, Token, false))
    {
        if (!Token.StartsWith(TEXT("-")) && FilePath.IsEmpty())
        {
            FilePath = Token;
        }
    }
    FString SourceRoot;
    FParse::Value(*Params, TEXT("root="), SourceRoot);

    if (FilePath.IsEmpty())
    {
        UE_LOG(LogMHCompositeDump, Error, TEXT("Usage: -run=MHCompositeDump <file.composite> [-root=<source_root>]"));
        return 2;
    }

    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
    {
        UE_LOG(LogMHCompositeDump, Error, TEXT("cannot read %s"), *FilePath);
        return 1;
    }
    FMHCompositeDocument Root;
    const FMHCanonicalResult Parsed = MHParseCompositeV2(Bytes, Root);
    if (!Parsed.bSuccess)
    {
        UE_LOG(LogMHCompositeDump, Error, TEXT("%s: %s"), *FilePath, *Parsed.Error);
        return 1;
    }

    FDumpState State;
    TUniquePtr<FMHPayloadScanResolver> Resolver;
    FMHCompositeWaveResult Wave;

    if (!SourceRoot.IsEmpty())
    {
        Resolver = MakeUnique<FMHPayloadScanResolver>(SourceRoot);
        FString Error;
        if (!Resolver->Initialize(Error))
        {
            UE_LOG(LogMHCompositeDump, Error, TEXT("%s"), *Error);
            return 1;
        }
        State.Resolver = Resolver.Get();
        UE_LOG(
            LogMHCompositeDump,
            Display,
            TEXT("scanned %s: %d payload candidates, %d quarantined, %d legacy v1 skipped"),
            *SourceRoot,
            Resolver->GetCandidateFileCount(),
            Resolver->GetQuarantined().Num(),
            Resolver->GetLegacySkipped().Num());
        for (const FString& Entry : Resolver->GetQuarantined())
        {
            UE_LOG(LogMHCompositeDump, Warning, TEXT("quarantined: %s"), *Entry);
        }
        for (const FString& Entry : Resolver->GetLegacySkipped())
        {
            UE_LOG(LogMHCompositeDump, Warning, TEXT("legacy v1 (migration only): %s"), *Entry);
        }

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
            UE_LOG(LogMHCompositeDump, Warning, TEXT("%s"), *Warning);
        }
        for (const FString& Error : Wave.Errors)
        {
            UE_LOG(LogMHCompositeDump, Error, TEXT("%s"), *Error);
            State.bAnyError = true;
        }
    }
    else
    {
        PrintComposite(Root, FilePath, State);
    }

    return State.bAnyError ? 1 : 0;
}
