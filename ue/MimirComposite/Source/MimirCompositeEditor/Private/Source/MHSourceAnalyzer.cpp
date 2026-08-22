#include "Source/MHSourceAnalyzer.h"

#include "Codec/MHCompositeCodec.h"
#include "Containers/Set.h"
#include "Misc/Paths.h"

namespace UE::MimirComposite
{
namespace
{

bool DiffStringEqual(const FString& Left, const FString& Right)
{
    return Left.Equals(Right, ESearchCase::CaseSensitive);
}

/** Payload path as stored in the Ledger: relative to source_root, forward slashes. */
FString AnalyzerRelativeSourcePath(const FString& SourceRootFull, const FString& PayloadPath)
{
    FString Full = FPaths::ConvertRelativePathToFull(PayloadPath);
    FPaths::NormalizeFilename(Full);

    FString Root = SourceRootFull;
    FPaths::NormalizeDirectoryName(Root);
    if (!Root.IsEmpty())
    {
        const FString Prefix = Root + TEXT("/");
        if (Full.StartsWith(Prefix, ESearchCase::IgnoreCase))
        {
            return Full.RightChop(Prefix.Len());
        }
    }
    return Full;
}

/**
 * The scan knows the kind of every candidate, the resolver seam does not expose
 * it, so the analyzer asks for each kind in turn. KindMismatch means "some other
 * kind carries this UID", which is exactly the probe answer we discard.
 */
bool AnalyzerProbeKind(
    IMHSourceResolver& Resolver,
    const FString& ResourceUid,
    EMHResourceKind& OutKind,
    FMHResolveOutcome& OutOutcome)
{
    static const EMHResourceKind ProbeOrder[] = {
        EMHResourceKind::StaticMesh,
        EMHResourceKind::Material,
        EMHResourceKind::Composite};

    for (const EMHResourceKind Probe : ProbeOrder)
    {
        FMHResolveOutcome Outcome = Resolver.Resolve(ResourceUid, Probe);
        if (Outcome.Status != EMHResolveStatus::KindMismatch &&
            Outcome.Status != EMHResolveStatus::Unresolved)
        {
            OutKind = Probe;
            OutOutcome = MoveTemp(Outcome);
            return true;
        }
    }
    return false;
}

void DiffOrderFlags(TArray<EMHDiffFlag>& Flags)
{
    Flags.Sort(
        [](const EMHDiffFlag Left, const EMHDiffFlag Right)
        {
            return static_cast<uint8>(Left) < static_cast<uint8>(Right);
        });
    for (int32 Index = Flags.Num() - 1; Index > 0; --Index)
    {
        if (Flags[Index] == Flags[Index - 1])
        {
            Flags.RemoveAt(Index);
        }
    }
}

bool DiffFlagAllowed(const EMHDiffFlag Flag, const bool bNodeSpace)
{
    if (bNodeSpace)
    {
        switch (Flag)
        {
        case EMHDiffFlag::Create:
        case EMHDiffFlag::Remove:
        case EMHDiffFlag::Rename:
        case EMHDiffFlag::UpdateTransform:
        case EMHDiffFlag::UpdateProperties:
        case EMHDiffFlag::Reparent:
        case EMHDiffFlag::UpdateResource:
        case EMHDiffFlag::UpdateKind:
            return true;
        default:
            return false;
        }
    }

    switch (Flag)
    {
    case EMHDiffFlag::Create:
    case EMHDiffFlag::Remove:
    case EMHDiffFlag::Rename:
    case EMHDiffFlag::UpdateGeometry:
    case EMHDiffFlag::UpdateProperties:
    case EMHDiffFlag::UpdateKind:
    case EMHDiffFlag::Move:
    case EMHDiffFlag::LocalEdit:
    case EMHDiffFlag::Conflict:
    case EMHDiffFlag::ExternalUnresolved:
        return true;
    default:
        return false;
    }
}

bool DiffNormalizeAndValidateFlags(
    TArray<EMHDiffFlag>& Flags,
    const bool bNodeSpace,
    const FString& Context,
    FString& OutError)
{
    DiffOrderFlags(Flags);
    if (Flags.IsEmpty())
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: empty diff flag set at %s"),
            *Context);
        return false;
    }
    for (const EMHDiffFlag Flag : Flags)
    {
        if (!DiffFlagAllowed(Flag, bNodeSpace))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: flag %s is invalid at %s"),
                MHDiffFlagLabel(Flag),
                *Context);
            return false;
        }
    }
    if ((Flags.Contains(EMHDiffFlag::Create) || Flags.Contains(EMHDiffFlag::Remove)) &&
        Flags.Num() != 1)
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: CREATE/REMOVE must be exclusive at %s"),
            *Context);
        return false;
    }
    return true;
}

bool DiffValidateSnapshotIdentity(
    const FMHDiffSnapshotValue& Snapshot,
    const TCHAR* SnapshotLabel,
    FString& OutError)
{
    for (const TPair<FString, FMHDiffResourceValue>& Pair : Snapshot.Resources)
    {
        const FMHDiffResourceValue& Value = Pair.Value;
        if (!MHIsCanonicalUuid(Pair.Key) ||
            !DiffStringEqual(Pair.Key, Value.ResourceUid))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: %s diff snapshot has non-canonical or mismatched ResourceUID"),
                SnapshotLabel);
            return false;
        }
        if (Value.Name.IsEmpty() || Value.SourcePath.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: %s diff resource %s has incomplete identity values"),
                SnapshotLabel,
                *Pair.Key);
            return false;
        }
    }
    return true;
}

bool DiffRequireSemanticValues(
    const FMHDiffResourceValue& OldValue,
    const FMHDiffResourceValue& NewValue,
    FString& OutError)
{
    if (!OldValue.bHasValidatedSemanticValue || !NewValue.bHasValidatedSemanticValue ||
        OldValue.CanonicalSemanticValue.IsEmpty() || NewValue.CanonicalSemanticValue.IsEmpty())
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: diff resource %s has no validated old/new semantic values"),
            *NewValue.ResourceUid);
        return false;
    }
    return true;
}

bool DiffBuildNodeMap(
    const FMHDiffResourceValue& Resource,
    const TCHAR* SnapshotLabel,
    TMap<FString, const FMHDiffNodeValue*>& OutNodes,
    FString& OutError)
{
    OutNodes.Reset();
    if (!Resource.bHasValidatedNodeValues)
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: composite %s has no validated %s node values"),
            *Resource.ResourceUid,
            SnapshotLabel);
        return false;
    }
    for (const FMHDiffNodeValue& Node : Resource.Nodes)
    {
        if (!MHIsCanonicalUuid(Node.NodeUid) || Node.Kind.IsEmpty() ||
            Node.CanonicalTransformValue.IsEmpty() || Node.CanonicalPropertiesValue.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: composite %s has incomplete %s node values"),
                *Resource.ResourceUid,
                SnapshotLabel);
            return false;
        }
        if (OutNodes.Contains(Node.NodeUid))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: composite %s repeats %s NodeUID %s"),
                *Resource.ResourceUid,
                SnapshotLabel,
                *Node.NodeUid);
            return false;
        }
        OutNodes.Add(Node.NodeUid, &Node);
    }
    return true;
}

bool DiffBuildCompositeNodes(
    const FMHDiffResourceValue& OldValue,
    const FMHDiffResourceValue& NewValue,
    FMHDiffCompositeNodeOps& OutOps,
    FString& OutError)
{
    TMap<FString, const FMHDiffNodeValue*> OldNodes;
    TMap<FString, const FMHDiffNodeValue*> NewNodes;
    if (!DiffBuildNodeMap(OldValue, TEXT("old"), OldNodes, OutError) ||
        !DiffBuildNodeMap(NewValue, TEXT("new"), NewNodes, OutError))
    {
        return false;
    }

    TArray<FString> NodeUids;
    OldNodes.GenerateKeyArray(NodeUids);
    for (const TPair<FString, const FMHDiffNodeValue*>& Pair : NewNodes)
    {
        if (!OldNodes.Contains(Pair.Key))
        {
            NodeUids.Add(Pair.Key);
        }
    }
    NodeUids.Sort();

    OutOps = FMHDiffCompositeNodeOps();
    OutOps.ResourceUid = NewValue.ResourceUid;
    for (const FString& NodeUid : NodeUids)
    {
        const FMHDiffNodeValue* const* OldNodePtr = OldNodes.Find(NodeUid);
        const FMHDiffNodeValue* const* NewNodePtr = NewNodes.Find(NodeUid);

        FMHDiffNodeOp Op;
        Op.NodeUid = NodeUid;
        if (OldNodePtr == nullptr)
        {
            Op.Flags.Add(EMHDiffFlag::Create);
        }
        else if (NewNodePtr == nullptr)
        {
            Op.Flags.Add(EMHDiffFlag::Remove);
        }
        else
        {
            const FMHDiffNodeValue& OldNode = **OldNodePtr;
            const FMHDiffNodeValue& NewNode = **NewNodePtr;
            if (!DiffStringEqual(OldNode.DisplayName, NewNode.DisplayName))
            {
                Op.Flags.Add(EMHDiffFlag::Rename);
            }
            if (!DiffStringEqual(OldNode.CanonicalTransformValue, NewNode.CanonicalTransformValue))
            {
                Op.Flags.Add(EMHDiffFlag::UpdateTransform);
            }
            if (!DiffStringEqual(OldNode.CanonicalPropertiesValue, NewNode.CanonicalPropertiesValue))
            {
                Op.Flags.Add(EMHDiffFlag::UpdateProperties);
            }
            if (!DiffStringEqual(OldNode.ParentUid, NewNode.ParentUid))
            {
                Op.Flags.Add(EMHDiffFlag::Reparent);
            }
            if (!DiffStringEqual(OldNode.ResourceUid, NewNode.ResourceUid))
            {
                Op.Flags.Add(EMHDiffFlag::UpdateResource);
            }
            if (!DiffStringEqual(OldNode.Kind, NewNode.Kind))
            {
                Op.Flags.Add(EMHDiffFlag::UpdateKind);
            }
        }

        if (!Op.Flags.IsEmpty())
        {
            DiffOrderFlags(Op.Flags);
            OutOps.Nodes.Add(MoveTemp(Op));
        }
    }
    return true;
}

void DiffAppendIndent(FString& OutJson, const int32 Spaces)
{
    OutJson.AppendChars(TEXT("                                "), Spaces);
}

void DiffAppendJsonString(FString& OutJson, const FString& Value)
{
    OutJson.AppendChar(TEXT('"'));
    for (const TCHAR Character : Value)
    {
        switch (Character)
        {
        case TEXT('"'): OutJson.Append(TEXT("\\\"")); break;
        case TEXT('\\'): OutJson.Append(TEXT("\\\\")); break;
        case TEXT('\b'): OutJson.Append(TEXT("\\b")); break;
        case TEXT('\f'): OutJson.Append(TEXT("\\f")); break;
        case TEXT('\n'): OutJson.Append(TEXT("\\n")); break;
        case TEXT('\r'): OutJson.Append(TEXT("\\r")); break;
        case TEXT('\t'): OutJson.Append(TEXT("\\t")); break;
        default:
            if (static_cast<uint32>(Character) < 0x20u)
            {
                OutJson.Appendf(TEXT("\\u%04x"), static_cast<uint32>(Character));
            }
            else
            {
                // Python json.dump(..., ensure_ascii=False) preserves Unicode.
                OutJson.AppendChar(Character);
            }
            break;
        }
    }
    OutJson.AppendChar(TEXT('"'));
}

void DiffAppendFlagArray(
    FString& OutJson,
    const TArray<EMHDiffFlag>& Flags,
    const int32 ClosingIndent)
{
    OutJson.Append(TEXT("[\n"));
    for (int32 Index = 0; Index < Flags.Num(); ++Index)
    {
        DiffAppendIndent(OutJson, ClosingIndent + 2);
        DiffAppendJsonString(OutJson, MHDiffFlagLabel(Flags[Index]));
        OutJson.Append(Index + 1 < Flags.Num() ? TEXT(",\n") : TEXT("\n"));
    }
    DiffAppendIndent(OutJson, ClosingIndent);
    OutJson.AppendChar(TEXT(']'));
}

} // namespace

const TCHAR* MHDiffFlagLabel(const EMHDiffFlag Flag)
{
    switch (Flag)
    {
    case EMHDiffFlag::Create: return TEXT("CREATE");
    case EMHDiffFlag::Remove: return TEXT("REMOVE");
    case EMHDiffFlag::Rename: return TEXT("RENAME");
    case EMHDiffFlag::UpdateGeometry: return TEXT("UPDATE_GEOMETRY");
    case EMHDiffFlag::UpdateTransform: return TEXT("UPDATE_TRANSFORM");
    case EMHDiffFlag::UpdateProperties: return TEXT("UPDATE_PROPERTIES");
    case EMHDiffFlag::Reparent: return TEXT("REPARENT");
    case EMHDiffFlag::UpdateResource: return TEXT("UPDATE_RESOURCE");
    case EMHDiffFlag::UpdateKind: return TEXT("UPDATE_KIND");
    case EMHDiffFlag::Move: return TEXT("MOVE");
    case EMHDiffFlag::LocalEdit: return TEXT("LOCAL_EDIT");
    case EMHDiffFlag::Conflict: return TEXT("CONFLICT");
    case EMHDiffFlag::ExternalUnresolved: return TEXT("EXTERNAL_UNRESOLVED");
    default: return TEXT("UNKNOWN");
    }
}

const FMHDiffResourceOp* FMHDiffReport::FindResource(const FString& ResourceUid) const
{
    for (const FMHDiffResourceOp& Op : Resources)
    {
        if (DiffStringEqual(Op.ResourceUid, ResourceUid))
        {
            return &Op;
        }
    }
    return nullptr;
}

const FMHDiffCompositeNodeOps* FMHDiffReport::FindCompositeNodes(const FString& ResourceUid) const
{
    for (const FMHDiffCompositeNodeOps& Op : Nodes)
    {
        if (DiffStringEqual(Op.ResourceUid, ResourceUid))
        {
            return &Op;
        }
    }
    return nullptr;
}

const FMHDiffNodeOp* FMHDiffCompositeNodeOps::Find(const FString& NodeUid) const
{
    for (const FMHDiffNodeOp& Op : Nodes)
    {
        if (DiffStringEqual(Op.NodeUid, NodeUid))
        {
            return &Op;
        }
    }
    return nullptr;
}

bool MHBuildDiffReport(
    const FMHDiffSnapshotValue& OldSnapshot,
    const FMHDiffSnapshotValue& NewSnapshot,
    FMHDiffReport& OutReport,
    FString& OutError)
{
    OutReport = FMHDiffReport();
    OutError.Reset();
    if (!DiffValidateSnapshotIdentity(OldSnapshot, TEXT("old"), OutError) ||
        !DiffValidateSnapshotIdentity(NewSnapshot, TEXT("new"), OutError))
    {
        return false;
    }

    TArray<FString> ResourceUids;
    OldSnapshot.Resources.GenerateKeyArray(ResourceUids);
    for (const TPair<FString, FMHDiffResourceValue>& Pair : NewSnapshot.Resources)
    {
        if (!OldSnapshot.Resources.Contains(Pair.Key))
        {
            ResourceUids.Add(Pair.Key);
        }
    }
    ResourceUids.Sort();

    FMHDiffReport Built;
    for (const FString& ResourceUid : ResourceUids)
    {
        const FMHDiffResourceValue* OldValue = OldSnapshot.Resources.Find(ResourceUid);
        const FMHDiffResourceValue* NewValue = NewSnapshot.Resources.Find(ResourceUid);

        FMHDiffResourceOp ResourceOp;
        ResourceOp.ResourceUid = ResourceUid;
        if (OldValue == nullptr)
        {
            ResourceOp.Flags.Add(EMHDiffFlag::Create);
        }
        else if (NewValue == nullptr)
        {
            ResourceOp.Flags.Add(EMHDiffFlag::Remove);
        }
        else
        {
            if (!DiffStringEqual(OldValue->Name, NewValue->Name))
            {
                ResourceOp.Flags.Add(EMHDiffFlag::Rename);
            }
            if (!DiffStringEqual(OldValue->SourcePath, NewValue->SourcePath))
            {
                ResourceOp.Flags.Add(EMHDiffFlag::Move);
            }

            if (OldValue->Kind != NewValue->Kind)
            {
                ResourceOp.Flags.Add(EMHDiffFlag::UpdateKind);
            }
            else
            {
                if (!DiffRequireSemanticValues(*OldValue, *NewValue, OutError))
                {
                    return false;
                }

                switch (NewValue->Kind)
                {
                case EMHResourceKind::StaticMesh:
                    if (OldValue->GeometryHash.IsEmpty() || NewValue->GeometryHash.IsEmpty() ||
                        OldValue->DescriptorHash.IsEmpty() || NewValue->DescriptorHash.IsEmpty())
                    {
                        OutError = FString::Printf(
                            TEXT("MH_E_SOURCE_INDEX_INVALID: static mesh %s has incomplete validated hash values"),
                            *ResourceUid);
                        return false;
                    }
                    if (!DiffStringEqual(OldValue->GeometryHash, NewValue->GeometryHash))
                    {
                        ResourceOp.Flags.Add(EMHDiffFlag::UpdateGeometry);
                    }
                    if (!DiffStringEqual(OldValue->DescriptorHash, NewValue->DescriptorHash) &&
                        !DiffStringEqual(
                            OldValue->CanonicalSemanticValue,
                            NewValue->CanonicalSemanticValue))
                    {
                        ResourceOp.Flags.Add(EMHDiffFlag::UpdateProperties);
                    }
                    break;

                case EMHResourceKind::Material:
                    if (!DiffStringEqual(
                            OldValue->CanonicalSemanticValue,
                            NewValue->CanonicalSemanticValue))
                    {
                        ResourceOp.Flags.Add(EMHDiffFlag::UpdateProperties);
                    }
                    break;

                case EMHResourceKind::Composite:
                    if (!DiffStringEqual(
                            OldValue->CanonicalSemanticValue,
                            NewValue->CanonicalSemanticValue))
                    {
                        ResourceOp.Flags.Add(EMHDiffFlag::UpdateProperties);
                    }
                    {
                        FMHDiffCompositeNodeOps NodeOps;
                        if (!DiffBuildCompositeNodes(*OldValue, *NewValue, NodeOps, OutError))
                        {
                            return false;
                        }
                        if (!NodeOps.Nodes.IsEmpty())
                        {
                            Built.Nodes.Add(MoveTemp(NodeOps));
                        }
                    }
                    break;

                default:
                    OutError = FString::Printf(
                        TEXT("MH_E_SOURCE_INDEX_INVALID: diff resource %s has unsupported kind"),
                        *ResourceUid);
                    return false;
                }
            }
        }

        if (!ResourceOp.Flags.IsEmpty())
        {
            DiffOrderFlags(ResourceOp.Flags);
            Built.Resources.Add(MoveTemp(ResourceOp));
        }
    }

    OutReport = MoveTemp(Built);
    return true;
}

bool MHDiffReportToJson(
    const FMHDiffReport& Report,
    FString& OutJson,
    FString& OutError)
{
    OutJson.Reset();
    OutError.Reset();

    FMHDiffReport Ordered = Report;
    Ordered.Resources.Sort(
        [](const FMHDiffResourceOp& Left, const FMHDiffResourceOp& Right)
        {
            return Left.ResourceUid.Compare(
                       Right.ResourceUid,
                       ESearchCase::CaseSensitive) < 0;
        });
    for (int32 Index = 0; Index < Ordered.Resources.Num(); ++Index)
    {
        FMHDiffResourceOp& Op = Ordered.Resources[Index];
        if (!MHIsCanonicalUuid(Op.ResourceUid) ||
            (Index > 0 && DiffStringEqual(
                Ordered.Resources[Index - 1].ResourceUid,
                Op.ResourceUid)))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: diff report has empty or duplicate ResourceUID operations");
            return false;
        }
        if (!DiffNormalizeAndValidateFlags(
                Op.Flags,
                false,
                FString::Printf(TEXT("resources/%s"), *Op.ResourceUid),
                OutError))
        {
            return false;
        }
    }

    Ordered.Nodes.Sort(
        [](const FMHDiffCompositeNodeOps& Left, const FMHDiffCompositeNodeOps& Right)
        {
            return Left.ResourceUid.Compare(
                       Right.ResourceUid,
                       ESearchCase::CaseSensitive) < 0;
        });
    for (int32 ResourceIndex = 0; ResourceIndex < Ordered.Nodes.Num(); ++ResourceIndex)
    {
        FMHDiffCompositeNodeOps& CompositeOps = Ordered.Nodes[ResourceIndex];
        if (!MHIsCanonicalUuid(CompositeOps.ResourceUid) || CompositeOps.Nodes.IsEmpty() ||
            (ResourceIndex > 0 &&
             DiffStringEqual(
                 Ordered.Nodes[ResourceIndex - 1].ResourceUid,
                 CompositeOps.ResourceUid)))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: diff report has empty or duplicate composite node operations");
            return false;
        }
        CompositeOps.Nodes.Sort(
            [](const FMHDiffNodeOp& Left, const FMHDiffNodeOp& Right)
            {
                return Left.NodeUid.Compare(
                           Right.NodeUid,
                           ESearchCase::CaseSensitive) < 0;
            });
        for (int32 NodeIndex = 0; NodeIndex < CompositeOps.Nodes.Num(); ++NodeIndex)
        {
            FMHDiffNodeOp& Op = CompositeOps.Nodes[NodeIndex];
            if (!MHIsCanonicalUuid(Op.NodeUid) ||
                (NodeIndex > 0 && DiffStringEqual(
                    CompositeOps.Nodes[NodeIndex - 1].NodeUid,
                    Op.NodeUid)))
            {
                OutError = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_INVALID: composite %s has empty or duplicate NodeUID operations"),
                    *CompositeOps.ResourceUid);
                return false;
            }
            if (!DiffNormalizeAndValidateFlags(
                    Op.Flags,
                    true,
                    FString::Printf(
                        TEXT("nodes/%s/%s"),
                        *CompositeOps.ResourceUid,
                        *Op.NodeUid),
                    OutError))
            {
                return false;
            }
        }
    }

    // Match Python json.dump(report, indent=2, ensure_ascii=False) plus LF.
    OutJson.Append(TEXT("{\n"));
    OutJson.Append(TEXT("  \"schema\": \"mh.diff_report\",\n"));
    OutJson.Append(TEXT("  \"schema_version\": 1,\n"));
    OutJson.Append(TEXT("  \"resources\": "));
    if (Ordered.Resources.IsEmpty())
    {
        OutJson.Append(TEXT("{}"));
    }
    else
    {
        OutJson.Append(TEXT("{\n"));
        for (int32 Index = 0; Index < Ordered.Resources.Num(); ++Index)
        {
            const FMHDiffResourceOp& Op = Ordered.Resources[Index];
            DiffAppendIndent(OutJson, 4);
            DiffAppendJsonString(OutJson, Op.ResourceUid);
            OutJson.Append(TEXT(": "));
            DiffAppendFlagArray(OutJson, Op.Flags, 4);
            OutJson.Append(Index + 1 < Ordered.Resources.Num() ? TEXT(",\n") : TEXT("\n"));
        }
        OutJson.Append(TEXT("  }"));
    }
    OutJson.Append(TEXT(",\n"));
    OutJson.Append(TEXT("  \"nodes\": "));
    if (Ordered.Nodes.IsEmpty())
    {
        OutJson.Append(TEXT("{}"));
    }
    else
    {
        OutJson.Append(TEXT("{\n"));
        for (int32 ResourceIndex = 0; ResourceIndex < Ordered.Nodes.Num(); ++ResourceIndex)
        {
            const FMHDiffCompositeNodeOps& CompositeOps = Ordered.Nodes[ResourceIndex];
            DiffAppendIndent(OutJson, 4);
            DiffAppendJsonString(OutJson, CompositeOps.ResourceUid);
            OutJson.Append(TEXT(": {\n"));
            for (int32 NodeIndex = 0; NodeIndex < CompositeOps.Nodes.Num(); ++NodeIndex)
            {
                const FMHDiffNodeOp& Op = CompositeOps.Nodes[NodeIndex];
                DiffAppendIndent(OutJson, 6);
                DiffAppendJsonString(OutJson, Op.NodeUid);
                OutJson.Append(TEXT(": "));
                DiffAppendFlagArray(OutJson, Op.Flags, 6);
                OutJson.Append(NodeIndex + 1 < CompositeOps.Nodes.Num() ? TEXT(",\n") : TEXT("\n"));
            }
            OutJson.Append(TEXT("    }"));
            OutJson.Append(ResourceIndex + 1 < Ordered.Nodes.Num() ? TEXT(",\n") : TEXT("\n"));
        }
        OutJson.Append(TEXT("  }"));
    }
    OutJson.Append(TEXT("\n}\n"));
    return true;
}

const TCHAR* MHSourceChangeLabel(const EMHSourceChange Change)
{
    switch (Change)
    {
    case EMHSourceChange::Create: return TEXT("CREATE");
    case EMHSourceChange::UpdateGeometry: return TEXT("UPDATE_GEOMETRY");
    case EMHSourceChange::UpdateDescriptor: return TEXT("UPDATE_DESCRIPTOR");
    case EMHSourceChange::UpdateProperties: return TEXT("UPDATE_PROPERTIES");
    case EMHSourceChange::Move: return TEXT("MOVE");
    case EMHSourceChange::NoChange: return TEXT("NO_CHANGE");
    case EMHSourceChange::NoChangeExternal: return TEXT("NO_CHANGE_EXTERNAL");
    case EMHSourceChange::Remove: return TEXT("REMOVE");
    default: return TEXT("BLOCKED");
    }
}

bool MHSourceChangeAdvancesLedger(const EMHSourceChange Change)
{
    switch (Change)
    {
    case EMHSourceChange::Create:
    case EMHSourceChange::UpdateGeometry:
    case EMHSourceChange::UpdateDescriptor:
    case EMHSourceChange::UpdateProperties:
    case EMHSourceChange::Move:
    case EMHSourceChange::NoChange:
        return true;
    default:
        return false;
    }
}

const FMHSourceAnalysisEntry* FMHSourceAnalysis::Find(const FString& ResourceUid) const
{
    for (const FMHSourceAnalysisEntry& Entry : Entries)
    {
        if (Entry.ResourceUid == ResourceUid)
        {
            return &Entry;
        }
    }
    return nullptr;
}

int32 FMHSourceAnalysis::CountOf(const EMHSourceChange Change) const
{
    int32 Count = 0;
    for (const FMHSourceAnalysisEntry& Entry : Entries)
    {
        if (Entry.Change == Change)
        {
            ++Count;
        }
    }
    return Count;
}

bool FMHSourceAnalysis::HasErrors() const
{
    if (Errors.Num() > 0)
    {
        return true;
    }
    for (const FMHSourceAnalysisEntry& Entry : Entries)
    {
        if (Entry.Errors.Num() > 0)
        {
            return true;
        }
    }
    return false;
}

void FMHLedgerChangeDetector::DetectChanges(
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis)
{
    OutAnalysis = FMHSourceAnalysis();

    const FString SourceRootFull = FPaths::ConvertRelativePathToFull(SourceRoot);

    const FMHSourceSnapshot Snapshot = Resolver.GetSnapshot();
    OutAnalysis.Warnings.Append(Snapshot.Warnings);
    OutAnalysis.Errors.Append(Snapshot.Errors);

    TArray<FString> Uids = Snapshot.ResourceUids;
    const TSet<FString> Scanned(Uids);
    for (const TPair<FString, FMHLedgerRow>& Row : Ledger)
    {
        if (!Scanned.Contains(Row.Key))
        {
            Uids.Add(Row.Key);
        }
    }
    Uids.Sort();

    for (const FString& Uid : Uids)
    {
        const FMHLedgerRow* Row = Ledger.Find(Uid);

        FMHSourceAnalysisEntry Analyzed;
        Analyzed.ResourceUid = Uid;

        // The last-applied path is authoritative for corruption detection. If
        // that exact payload is now quarantined, a second valid candidate with
        // the same UID must not turn corruption into MOVE/update. Require an
        // explicit resolution of the quarantined revision first.
        const FMHSourceQuarantine* MatchingQuarantine = nullptr;
        if (Row != nullptr && !Row->SourcePath.IsEmpty())
        {
            FString PreviousPayloadPath = FPaths::ConvertRelativePathToFull(
                SourceRootFull,
                Row->SourcePath);
            FPaths::NormalizeFilename(PreviousPayloadPath);
            MatchingQuarantine = Snapshot.Quarantined.FindByPredicate(
                [&PreviousPayloadPath](const FMHSourceQuarantine& Quarantine)
                {
                    return Quarantine.PayloadPath.Equals(
                        PreviousPayloadPath,
                        ESearchCase::IgnoreCase);
                });
        }

        if (MatchingQuarantine != nullptr)
        {
            Analyzed.Kind = Row->Kind;
            Analyzed.SourcePath = Row->SourcePath;
            Analyzed.PayloadPath = MatchingQuarantine->PayloadPath;
            Analyzed.Change = EMHSourceChange::Blocked;
            Analyzed.Errors.Add(MatchingQuarantine->Diagnostic);
            OutAnalysis.Entries.Add(MoveTemp(Analyzed));
            continue;
        }

        // A Ledger UID without any valid candidate is an orphan: classify it and
        // leave the actual asset removal to a later gate. A payload that still
        // exists at the recorded path but was quarantined is corruption, never
        // a REMOVE inference.
        if (!Scanned.Contains(Uid))
        {
            Analyzed.Kind = Row != nullptr ? Row->Kind : EMHResourceKind::Composite;
            Analyzed.SourcePath = Row != nullptr ? Row->SourcePath : FString();
            Analyzed.Change = EMHSourceChange::Remove;
            OutAnalysis.Entries.Add(MoveTemp(Analyzed));
            continue;
        }

        EMHResourceKind Kind = EMHResourceKind::Composite;
        FMHResolveOutcome Outcome;
        if (!AnalyzerProbeKind(Resolver, Uid, Kind, Outcome))
        {
            Analyzed.Kind = Row != nullptr ? Row->Kind : EMHResourceKind::Composite;
            Analyzed.Change = EMHSourceChange::Blocked;
            Analyzed.Errors.Add(FString::Printf(
                TEXT("MH_E_RESOURCE_NOT_FOUND: scanned UID %s no longer resolves"),
                *Uid));
            OutAnalysis.Entries.Add(MoveTemp(Analyzed));
            continue;
        }
        Analyzed.Kind = Kind;

        if (Outcome.Status != EMHResolveStatus::Resolved)
        {
            Analyzed.Change = EMHSourceChange::Blocked;
            Analyzed.SourcePath = Row != nullptr ? Row->SourcePath : FString();
            Analyzed.Errors.Add(Outcome.Diagnostic);
            OutAnalysis.Entries.Add(MoveTemp(Analyzed));
            continue;
        }

        Analyzed.Name = Outcome.Name;
        Analyzed.PayloadPath = Outcome.PayloadPath;
        Analyzed.SourcePath = AnalyzerRelativeSourcePath(SourceRootFull, Outcome.PayloadPath);
        Analyzed.Fingerprint = Outcome.Fingerprint;
        Analyzed.GeometryHash = Outcome.GeometryHash;
        Analyzed.DescriptorHash = Outcome.DescriptorHash;
        if (!Outcome.Diagnostic.IsEmpty())
        {
            Analyzed.Warnings.Add(Outcome.Diagnostic);
        }

        if (Analyzed.DescriptorHash.IsEmpty() ||
            (Kind == EMHResourceKind::StaticMesh && Analyzed.GeometryHash.IsEmpty()))
        {
            Analyzed.Change = EMHSourceChange::Blocked;
            Analyzed.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: resolver returned incomplete semantic state for %s"),
                *Uid));
            OutAnalysis.Entries.Add(MoveTemp(Analyzed));
            continue;
        }

        if (Row == nullptr)
        {
            Analyzed.Change = EMHSourceChange::Create;
        }
        else if (Kind == EMHResourceKind::StaticMesh && Analyzed.GeometryHash != Row->AppliedGeometryHash)
        {
            Analyzed.Change = EMHSourceChange::UpdateGeometry;
        }
        else if (Analyzed.DescriptorHash != Row->AppliedDescriptorHash)
        {
            Analyzed.Change = Kind == EMHResourceKind::StaticMesh
                ? EMHSourceChange::UpdateDescriptor
                : EMHSourceChange::UpdateProperties;
        }
        else if (Analyzed.SourcePath != Row->SourcePath)
        {
            Analyzed.Change = EMHSourceChange::Move;
        }
        else if (!Row->PayloadFingerprint.IsEmpty() && Analyzed.Fingerprint != Row->PayloadFingerprint)
        {
            // Both semantic hashes are equal, so nothing is reimported; the row
            // still may not advance without an explicit confirmation.
            Analyzed.Change = EMHSourceChange::NoChangeExternal;
            Analyzed.Warnings.Add(FString::Printf(
                TEXT("MH_W_PAYLOAD_EXTERNAL_MODIFIED: %s changed on disk while both semantic hashes stayed equal"),
                *Analyzed.SourcePath));
            Analyzed.Errors.Add(FString::Printf(
                TEXT("MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED: %s needs explicit confirmation before the Ledger advances"),
                *Analyzed.SourcePath));
        }
        else
        {
            Analyzed.Change = EMHSourceChange::NoChange;
        }

        Analyzed.bLedgerAdvanceAllowed = MHSourceChangeAdvancesLedger(Analyzed.Change);
        OutAnalysis.Entries.Add(MoveTemp(Analyzed));
    }
}

void MHAnalyzeSources(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis)
{
    ChangeDetector.DetectChanges(Resolver, SourceRoot, OutAnalysis);
}

void MHAnalyzeSources(
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const TMap<FString, FMHLedgerRow>& Ledger,
    FMHSourceAnalysis& OutAnalysis)
{
    FMHLedgerChangeDetector ChangeDetector(Ledger);
    MHAnalyzeSources(ChangeDetector, Resolver, SourceRoot, OutAnalysis);
}

bool MHLedgerRowFromAnalysis(const FMHSourceAnalysisEntry& Entry, FMHLedgerRow& OutRow)
{
    if (!Entry.bLedgerAdvanceAllowed)
    {
        return false;
    }
    // Asset stays empty: C1 imports nothing, and only the importer of C2 knows
    // which package a row points at.
    OutRow = FMHLedgerRow();
    OutRow.Kind = Entry.Kind;
    OutRow.SourcePath = Entry.SourcePath;
    OutRow.AppliedGeometryHash = Entry.GeometryHash;
    OutRow.AppliedDescriptorHash = Entry.DescriptorHash;
    OutRow.PayloadFingerprint = Entry.Fingerprint;
    OutRow.ImportedAt = FDateTime::UtcNow();
    // C1 classifies only; C2 replaces this with the real import result.
    OutRow.ImportStatus = MHSourceChangeLabel(Entry.Change);
    return true;
}

} // namespace UE::MimirComposite
