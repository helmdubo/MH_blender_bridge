#include "Composite/MHCompositeCompiler.h"
#include "Composite/MHCompositeResolvedPlan.h"

#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{
namespace
{

constexpr const TCHAR* GeneratedMeshRoot = TEXT("/Game/MH/Generated/Meshes");

bool Unresolved(FString& OutError, const FString& Detail)
{
    OutError = FString::Printf(TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: %s"), *Detail);
    return false;
}

FTransform NodeTransform(const FMHCompositeNode& Node)
{
    return FTransform(Node.Transform.RotationQuat, Node.Transform.TranslationCm, Node.Transform.Scale);
}

bool LoadCompositeDocument(
    const FString& Resource,
    IMHSourceResolver& Resolver,
    FMHCompositeDocument& OutDocument,
    FString& OutError)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = Resource;
    const FMHResolveOutcome Outcome = Resolver.Resolve(Key);
    if (Outcome.Status != EMHResolveStatus::Resolved)
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("composite:%s does not resolve uniquely"), *Resource));
    }
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Outcome.PayloadPath))
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("cannot read source for composite:%s"), *Resource));
    }
    if (!Outcome.RawHash.IsEmpty() && MHRawPayloadHash(Bytes) != Outcome.RawHash)
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: composite:%s changed after source resolution"),
            *Resource);
        return false;
    }
    return MHParseCompositeV5(Bytes, OutDocument, OutError);
}

bool ResolveProfile(
    const FString& Resource,
    IMHSourceResolver& Resolver,
    FString& OutError)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::PlacementProfile;
    Key.LogicalName = Resource;
    const FMHResolveOutcome Outcome = Resolver.Resolve(Key);
    if (Outcome.Status != EMHResolveStatus::Resolved)
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("placement_profile:%s does not resolve uniquely"), *Resource));
    }
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Outcome.PayloadPath))
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("cannot read source for placement_profile:%s"), *Resource));
    }
    if (!Outcome.RawHash.IsEmpty() && MHRawPayloadHash(Bytes) != Outcome.RawHash)
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: placement_profile:%s changed after source resolution"),
            *Resource);
        return false;
    }
    FMHPlacementProfile Profile;
    Profile.LogicalName = Resource;
    return MHParsePlacementProfileV1(Bytes, Profile, OutError);
}

bool ResolveMesh(
    const FString& Resource,
    IMHSourceResolver& Resolver,
    UStaticMesh*& OutMesh,
    FString& OutError)
{
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::StaticMesh;
    Key.LogicalName = Resource;
    const FMHResolveOutcome Outcome = Resolver.Resolve(Key);
    if (Outcome.Status != EMHResolveStatus::Resolved)
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("static_mesh:%s does not resolve uniquely"), *Resource));
    }
    const FString ObjectPath = FString::Printf(
        TEXT("%s/%s.%s"), GeneratedMeshRoot, *Resource, *Resource);
    OutMesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
    if (OutMesh == nullptr)
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("static_mesh:%s has source but generated mesh %s is unavailable"),
            *Resource,
            *ObjectPath));
    }
    return true;
}

bool ResolveActorClass(
    const FString& Resource,
    const UMHCompositeSettings& Settings,
    UClass*& OutClass,
    FString& OutError)
{
    const FSoftClassPath* Path = Settings.ActorClassRegistry.Find(Resource);
    if (Path == nullptr || Path->IsNull())
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("actor:%s is absent from ActorClassRegistry"), *Resource));
    }
    OutClass = Path->TryLoadClass<AActor>();
    if (!MHIsSpawnableCompositeActorClass(OutClass))
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("actor:%s registry path is not a loadable AActor class: %s"),
            *Resource,
            *Path->ToString()));
    }
    return true;
}

struct FCompositeAdmissionContext
{
    IMHSourceResolver& Resolver;
    const UMHCompositeSettings& Settings;
    TArray<FString> Ancestors;
    FString Error;
};

bool WalkNodes(
    const TArray<FMHCompositeNode>& Nodes,
    FCompositeAdmissionContext& Context,
    const FMatrix& ParentWorld,
    const FString& PathPrefix,
    const TCHAR* SegmentLabel);

bool WalkCompositeReference(
    const FString& Resource,
    FCompositeAdmissionContext& Context,
    const FMatrix& ParentWorld,
    const FString& PathPrefix)
{
    if (Context.Ancestors.Contains(Resource))
    {
        Context.Error = FString::Printf(
            TEXT("MH_E_COMPOSITE_CYCLE: composite:%s includes itself or an ancestor"),
            *Resource);
        return false;
    }
    FMHCompositeDocument Nested;
    if (!LoadCompositeDocument(Resource, Context.Resolver, Nested, Context.Error)) return false;
    Context.Ancestors.Add(Resource);
    const bool bOk = WalkNodes(
        Nested.Nodes,
        Context,
        ParentWorld,
        PathPrefix + TEXT(">") + Resource + TEXT(":"),
        TEXT("nodes"));
    Context.Ancestors.Pop();
    return bOk;
}

bool ValidateRandomOptions(
    const FMHCompositeNode& Node,
    FCompositeAdmissionContext& Context,
    const FMatrix& NodeWorld,
    const FString& NodePath)
{
    for (int32 OptionIndex = 0; OptionIndex < Node.Options.Num(); ++OptionIndex)
    {
        const FMHCompositeOption& Option = Node.Options[OptionIndex];
        const FString OptionPath = FString::Printf(
            TEXT("%s/options[%d]"), *NodePath, OptionIndex);
        if (Option.Kind == EMHCompositeOptionKind::Mesh)
        {
            UStaticMesh* Mesh = nullptr;
            if (!ResolveMesh(Option.Resource, Context.Resolver, Mesh, Context.Error)) return false;
        }
        else if (Option.Kind == EMHCompositeOptionKind::Actor)
        {
            UClass* ActorClass = nullptr;
            if (!ResolveActorClass(Option.Resource, Context.Settings, ActorClass, Context.Error)) return false;
        }
        else if (Option.Kind == EMHCompositeOptionKind::Composite)
        {
            if (!WalkCompositeReference(
                    Option.Resource,
                    Context,
                    NodeWorld,
                    OptionPath))
            {
                return false;
            }
        }
    }
    return true;
}

bool WalkNodes(
    const TArray<FMHCompositeNode>& Nodes,
    FCompositeAdmissionContext& Context,
    const FMatrix& ParentWorld,
    const FString& PathPrefix,
    const TCHAR* SegmentLabel)
{
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        const FMHCompositeNode& Node = Nodes[NodeIndex];
        const FString NodePath = FString::Printf(
            TEXT("%s%s[%d]"), *PathPrefix, SegmentLabel, NodeIndex);
        const FTransform Local = NodeTransform(Node);
        const FMatrix NodeWorld = Local.ToMatrixWithScale() * ParentWorld;
        if (!MHIsRepresentableTransformMatrix(NodeWorld))
        {
            Context.Error = FString::Printf(
                TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: %s cannot round-trip through FTransform within 8 ULP"),
                *NodePath);
            return false;
        }
        if (!Node.Profile.IsEmpty())
        {
            if (!ResolveProfile(Node.Profile, Context.Resolver, Context.Error)) return false;
        }

        switch (Node.Kind)
        {
        case EMHCompositeNodeKind::Group:
        case EMHCompositeNodeKind::GameObj:
            // A gameobj carries source identity/TRS, not a generated endpoint.
            break;
        case EMHCompositeNodeKind::Mesh:
        {
            UStaticMesh* Mesh = nullptr;
            if (!ResolveMesh(Node.Resource, Context.Resolver, Mesh, Context.Error)) return false;
            break;
        }
        case EMHCompositeNodeKind::Actor:
        {
            UClass* ActorClass = nullptr;
            if (!ResolveActorClass(Node.Resource, Context.Settings, ActorClass, Context.Error)) return false;
            break;
        }
        case EMHCompositeNodeKind::Composite:
            if (!WalkCompositeReference(
                    Node.Resource,
                    Context,
                    NodeWorld,
                    NodePath))
            {
                return false;
            }
            break;
        case EMHCompositeNodeKind::Random:
            if (!ValidateRandomOptions(Node, Context, NodeWorld, NodePath)) return false;
            break;
        }
        if (!WalkNodes(
                Node.Children,
                Context,
                NodeWorld,
                NodePath + TEXT("/"),
                TEXT("children")))
        {
            return false;
        }
    }
    return true;
}

bool RunCompositeAdmission(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError)
{
    if (!MHIsCanonicalCompositeToken(LogicalName))
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: root composite logical name is not canonical");
        return false;
    }
    FCompositeAdmissionContext Context{Resolver, Settings};
    Context.Ancestors.Add(LogicalName);
    if (!WalkNodes(
            Document.Nodes,
            Context,
            FMatrix::Identity,
            LogicalName + TEXT(":"),
            TEXT("nodes")))
    {
        OutError = MoveTemp(Context.Error);
        return false;
    }
    return true;
}

} // namespace

bool MHValidateCompositeClosureV5(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError)
{
    OutError.Reset();
    return RunCompositeAdmission(
        LogicalName,
        Document,
        Resolver,
        Settings,
        OutError);
}

bool MHProbeCompositeBuildV5(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError)
{
    // Import owns a definition, not a placement. Admission visits every option
    // and profile without choosing a seed or constructing preview components.
    return MHValidateCompositeClosureV5(LogicalName, Document, Resolver, Settings, OutError);
}

} // namespace UE::MimirComposite
