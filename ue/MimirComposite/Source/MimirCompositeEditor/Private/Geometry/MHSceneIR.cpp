#include "Geometry/MHSceneIR.h"

namespace UE::MimirComposite
{
namespace
{
bool Fail(FString& OutError, const TCHAR* Code, const FString& Message)
{
    OutError = FString::Printf(TEXT("%s: %s"), Code, *Message);
    return false;
}

bool IsCanonicalResourceName(const FString& Value)
{
    if (Value.IsEmpty())
    {
        return false;
    }
    for (const TCHAR Character : Value)
    {
        if (!((Character >= TEXT('a') && Character <= TEXT('z')) ||
              (Character >= TEXT('0') && Character <= TEXT('9')) ||
              Character == TEXT('_')))
        {
            return false;
        }
    }
    return true;
}

bool ParseLodSuffix(const FString& Name, int32& OutLevel)
{
    if (Name.Len() < 6 || Name[Name.Len() - 6] != TEXT('_') || Name[Name.Len() - 5] != TEXT('l') ||
        Name[Name.Len() - 4] != TEXT('o') || Name[Name.Len() - 3] != TEXT('d'))
    {
        return false;
    }
    const TCHAR Tens = Name[Name.Len() - 2];
    const TCHAR Ones = Name[Name.Len() - 1];
    if (Tens < TEXT('0') || Tens > TEXT('9') || Ones < TEXT('0') || Ones > TEXT('9'))
    {
        return false;
    }
    OutLevel = (Tens - TEXT('0')) * 10 + (Ones - TEXT('0'));
    return true;
}

bool ParseCollisionSuffix(const FString& Name, EMHSceneCollisionMode& OutMode)
{
    if (Name.EndsWith(TEXT("_cls_phys"), ESearchCase::CaseSensitive))
    {
        OutMode = EMHSceneCollisionMode::PhysicsOnly;
        return true;
    }
    if (Name.EndsWith(TEXT("_cls_trace"), ESearchCase::CaseSensitive))
    {
        OutMode = EMHSceneCollisionMode::QueryOnly;
        return true;
    }
    if (Name.EndsWith(TEXT("_cls_both"), ESearchCase::CaseSensitive))
    {
        OutMode = EMHSceneCollisionMode::QueryAndPhysics;
        return true;
    }
    return false;
}

bool ValidateParentGraph(const TArray<FMHSceneIRNode>& Nodes, TArray<int32>& OutChildCounts, FString& OutError)
{
    OutChildCounts.Init(0, Nodes.Num());
    TSet<FString> Names;
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        const FMHSceneIRNode& Node = Nodes[NodeIndex];
        if (Node.Name.IsEmpty())
        {
            return Fail(OutError, TEXT("MH_E_INVALID_NODE_MARKERS"), TEXT("FBX node name must be non-empty"));
        }
        if (Names.Contains(Node.Name))
        {
            if (Node.Name.StartsWith(TEXT("SOCKET_"), ESearchCase::CaseSensitive))
            {
                return Fail(
                    OutError,
                    TEXT("MH_E_INVALID_RESOURCE_SOURCE"),
                    FString::Printf(TEXT("duplicate socket name '%s'"), *Node.Name.RightChop(7)));
            }
            return Fail(
                OutError,
                TEXT("MH_E_IMPORT_TARGET_OCCUPIED"),
                FString::Printf(TEXT("duplicate FBX Model node name '%s'"), *Node.Name));
        }
        Names.Add(Node.Name);

        if (Node.ParentIndex != INDEX_NONE)
        {
            if (!Nodes.IsValidIndex(Node.ParentIndex))
            {
                return Fail(
                    OutError,
                    TEXT("MH_E_PARENT_OUTSIDE_RESOURCE"),
                    FString::Printf(TEXT("node '%s' has parent index %d outside the resource"), *Node.Name, Node.ParentIndex));
            }
            ++OutChildCounts[Node.ParentIndex];
        }
    }

    TArray<uint8> VisitState;
    VisitState.Init(0, Nodes.Num());
    for (int32 StartIndex = 0; StartIndex < Nodes.Num(); ++StartIndex)
    {
        int32 NodeIndex = StartIndex;
        while (NodeIndex != INDEX_NONE && VisitState[NodeIndex] == 0)
        {
            VisitState[NodeIndex] = 1;
            NodeIndex = Nodes[NodeIndex].ParentIndex;
        }
        if (NodeIndex != INDEX_NONE && VisitState[NodeIndex] == 1)
        {
            return Fail(
                OutError,
                TEXT("MH_E_PARENT_CYCLE"),
                FString::Printf(TEXT("FBX Model parent graph is cyclic at '%s'"), *Nodes[NodeIndex].Name));
        }

        NodeIndex = StartIndex;
        while (NodeIndex != INDEX_NONE && VisitState[NodeIndex] == 1)
        {
            VisitState[NodeIndex] = 2;
            NodeIndex = Nodes[NodeIndex].ParentIndex;
        }
    }
    return true;
}

bool ClassifyNode(FMHSceneIRNode& Node, const bool bHasChildren, FString& OutError)
{
    const bool bHasUcx = Node.Name.StartsWith(TEXT("UCX_"), ESearchCase::CaseSensitive);
    const bool bHasSocket = Node.Name.StartsWith(TEXT("SOCKET_"), ESearchCase::CaseSensitive);
    EMHSceneCollisionMode SuffixMode = EMHSceneCollisionMode::None;
    const bool bHasClassSuffix = ParseCollisionSuffix(Node.Name, SuffixMode);
    int32 LodLevel = 0;
    const bool bHasLodSuffix = ParseLodSuffix(Node.Name, LodLevel);

    Node.Kind = EMHSceneNodeKind::Unclassified;
    Node.LODLevel = INDEX_NONE;
    Node.CollisionMode = EMHSceneCollisionMode::None;
    Node.SocketName.Reset();

    // V5-S6.1.2: an explicit mh_collision carrier is authoritative and precedes
    // every name marker. Real Dagor collision nodes carry arbitrary authored
    // names ("gaz53_a_body.lod01 cls phys.001"), so name markers alone would
    // classify them as render nodes and pull their slots into the material
    // union. Carrier nodes are collision and never enter the render inventory.
    if (Node.CollisionCarrier != EMHSceneCollisionCarrier::None)
    {
        if (Node.Attribute != EMHSceneNodeAttribute::Mesh)
        {
            return Fail(
                OutError,
                TEXT("MH_E_INVALID_NODE_MARKERS"),
                FString::Printf(TEXT("mh_collision is valid only on a mesh node: '%s'"), *Node.Name));
        }
        if (bHasSocket)
        {
            return Fail(
                OutError,
                TEXT("MH_E_INVALID_NODE_MARKERS"),
                FString::Printf(TEXT("SOCKET_ is valid only on a null node: '%s'"), *Node.Name));
        }
        Node.Kind = EMHSceneNodeKind::Collision;
        Node.CollisionMode = Node.CollisionCarrier == EMHSceneCollisionCarrier::Phys
            ? EMHSceneCollisionMode::PhysicsOnly
            : EMHSceneCollisionMode::QueryOnly;
        return true;
    }

    if (Node.Attribute == EMHSceneNodeAttribute::Mesh)
    {
        if (bHasSocket)
        {
            return Fail(
                OutError,
                TEXT("MH_E_INVALID_NODE_MARKERS"),
                FString::Printf(TEXT("SOCKET_ is valid only on a null node: '%s'"), *Node.Name));
        }
        const int32 MarkerCount = static_cast<int32>(bHasUcx) +
            static_cast<int32>(bHasClassSuffix) + static_cast<int32>(bHasLodSuffix);
        if (MarkerCount > 1)
        {
            return Fail(
                OutError,
                TEXT("MH_E_INVALID_NODE_MARKERS"),
                FString::Printf(TEXT("mesh node carries multiple classification markers: '%s'"), *Node.Name));
        }
        if (bHasUcx || bHasClassSuffix)
        {
            Node.Kind = EMHSceneNodeKind::Collision;
            Node.CollisionMode = bHasUcx ? EMHSceneCollisionMode::QueryAndPhysics : SuffixMode;
            return true;
        }
        Node.Kind = EMHSceneNodeKind::Render;
        Node.LODLevel = bHasLodSuffix ? LodLevel : 0;
        return true;
    }

    if (Node.Attribute != EMHSceneNodeAttribute::Null)
    {
        return Fail(
            OutError,
            TEXT("MH_E_UNSUPPORTED_NODE_KIND"),
            FString::Printf(TEXT("node '%s' is neither an FBX Mesh nor Null"), *Node.Name));
    }
    if (!Node.MaterialSlots.IsEmpty() || Node.Geometry.IsSet())
    {
        return Fail(
            OutError,
            TEXT("MH_E_UNSUPPORTED_NODE_KIND"),
            FString::Printf(TEXT("FBX Null node '%s' cannot own geometry or material slots"), *Node.Name));
    }
    if (bHasUcx || bHasClassSuffix || bHasLodSuffix)
    {
        return Fail(
            OutError,
            TEXT("MH_E_INVALID_NODE_MARKERS"),
            FString::Printf(TEXT("null node carries a mesh classification marker: '%s'"), *Node.Name));
    }
    if (bHasSocket)
    {
        if (bHasChildren)
        {
            return Fail(
                OutError,
                TEXT("MH_E_INVALID_NODE_MARKERS"),
                FString::Printf(TEXT("SOCKET_ null node has children: '%s'"), *Node.Name));
        }
        Node.SocketName = Node.Name.RightChop(7);
        if (Node.SocketName.IsEmpty())
        {
            return Fail(OutError, TEXT("MH_E_INVALID_NODE_MARKERS"), TEXT("SOCKET_ marker has an empty socket name"));
        }
        Node.Kind = EMHSceneNodeKind::Socket;
        return true;
    }
    Node.Kind = EMHSceneNodeKind::Group;
    return true;
}
} // namespace

bool MHClassifySceneIR(FMHSceneIR& InOutScene, FString& OutError)
{
    OutError.Reset();
    FMHSceneIR Classified = InOutScene;
    Classified.MaterialNames.Reset();
    Classified.LODLevels.Reset();
    Classified.bUsesExplicitLODs = false;

    if (Classified.Nodes.IsEmpty())
    {
        return Fail(OutError, TEXT("MH_E_EMPTY_RESOURCE_COLLECTION"), TEXT("mesh FBX contains no Model nodes"));
    }
    if (!IsCanonicalResourceName(Classified.ResourceName))
    {
        return Fail(
            OutError,
            TEXT("MH_E_NONCANONICAL_RESOURCE_NAME"),
            FString::Printf(TEXT("mesh resource name '%s' must match [a-z0-9_]+"), *Classified.ResourceName));
    }

    TArray<int32> ChildCounts;
    if (!ValidateParentGraph(Classified.Nodes, ChildCounts, OutError))
    {
        return false;
    }

    TSet<FString> SocketNames;
    TSet<FString> MaterialNames;
    TSet<int32> LODLevels;
    // Owner contract 2026-08-30: the mesh material list is the deterministic
    // union of every LOD's slots, ordered LOD-major (all LOD0 slots in first-use
    // order, then the slots LOD1 introduces, then LOD2, ...). Slots are recorded
    // per level here and flattened once the dense LOD inventory is known.
    TMap<int32, TArray<FString>> SlotsByLODLevel;
    bool bHasRender = false;
    bool bHasExplicitRender = false;
    bool bHasPlainRender = false;

    for (int32 NodeIndex = 0; NodeIndex < Classified.Nodes.Num(); ++NodeIndex)
    {
        FMHSceneIRNode& Node = Classified.Nodes[NodeIndex];
        if (!ClassifyNode(Node, ChildCounts[NodeIndex] > 0, OutError))
        {
            return false;
        }

        if (Node.Kind == EMHSceneNodeKind::Socket)
        {
            if (SocketNames.Contains(Node.SocketName))
            {
                return Fail(
                    OutError,
                    TEXT("MH_E_INVALID_RESOURCE_SOURCE"),
                    FString::Printf(TEXT("duplicate socket name '%s'"), *Node.SocketName));
            }
            SocketNames.Add(Node.SocketName);
        }
        else if (Node.Kind == EMHSceneNodeKind::Render)
        {
            bHasRender = true;
            int32 ParsedLevel = 0;
            const bool bExplicit = ParseLodSuffix(Node.Name, ParsedLevel);
            bHasExplicitRender |= bExplicit;
            bHasPlainRender |= !bExplicit;
            LODLevels.Add(Node.LODLevel);
            for (const FString& Slot : Node.MaterialSlots)
            {
                if (!IsCanonicalResourceName(Slot))
                {
                    return Fail(
                        OutError,
                        TEXT("MH_E_NONCANONICAL_RESOURCE_NAME"),
                        FString::Printf(TEXT("material slot '%s' must match [a-z0-9_]+"), *Slot));
                }
                MaterialNames.Add(Slot);
                // Record the slot at every level that uses it; the flattening
                // pass below keeps only its lowest level, so a slot shared by
                // LOD0 and LOD2 stays in the LOD0 block regardless of the order
                // the FBX happens to list its nodes in.
                SlotsByLODLevel.FindOrAdd(Node.LODLevel).AddUnique(Slot);
            }
        }
    }

    if (!bHasRender)
    {
        return Fail(OutError, TEXT("MH_E_EMPTY_RESOURCE_COLLECTION"), TEXT("mesh FBX contains no render mesh nodes"));
    }
    if (bHasExplicitRender && bHasPlainRender)
    {
        return Fail(
            OutError,
            TEXT("MH_E_INVALID_LOD_HIERARCHY"),
            TEXT("LOD FBX mixes terminal _lodNN and unsuffixed render nodes"));
    }

    Classified.LODLevels = LODLevels.Array();
    Classified.LODLevels.Sort();
    Classified.bUsesExplicitLODs = bHasExplicitRender;
    if (bHasExplicitRender)
    {
        const int32 HighestLOD = Classified.LODLevels.Last();
        for (int32 Level = 0; Level <= HighestLOD; ++Level)
        {
            if (!LODLevels.Contains(Level))
            {
                return Fail(
                    OutError,
                    TEXT("MH_E_LOD_LEVELS_SPARSE"),
                    FString::Printf(TEXT("FBX LOD level lod%02d is missing"), Level));
            }
        }
    }

    // Flatten the per-level slot lists in ascending LOD order, taking each slot
    // at the lowest level that uses it. LODLevels is the sorted, dense inventory
    // of the render levels, so every recorded level is visited exactly once and
    // the resulting order depends only on the FBX content, never on the hash
    // iteration order of SlotsByLODLevel.
    Classified.MaterialNames.Reserve(MaterialNames.Num());
    TSet<FString> EmittedSlots;
    EmittedSlots.Reserve(MaterialNames.Num());
    for (const int32 Level : Classified.LODLevels)
    {
        const TArray<FString>* LevelSlots = SlotsByLODLevel.Find(Level);
        if (LevelSlots == nullptr)
        {
            continue;
        }
        for (const FString& Slot : *LevelSlots)
        {
            if (!EmittedSlots.Contains(Slot))
            {
                EmittedSlots.Add(Slot);
                Classified.MaterialNames.Add(Slot);
            }
        }
    }

    InOutScene = MoveTemp(Classified);
    return true;
}

} // namespace UE::MimirComposite
