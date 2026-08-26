#include "Composite/MHCompositeCompiler.h"

#include "Components/ChildActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
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
    if (OutClass == nullptr || !OutClass->IsChildOf(AActor::StaticClass()))
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("actor:%s registry path is not a loadable AActor class: %s"),
            *Resource,
            *Path->ToString()));
    }
    return true;
}

struct FCompileContext
{
    IMHSourceResolver& Resolver;
    const UMHCompositeSettings& Settings;
    AActor* Target = nullptr;
    TArray<TObjectPtr<UActorComponent>>* Components = nullptr;
    TArray<FString> Ancestors;
    FString Error;
    bool bRequiresResolvedPlan = false;
};

USceneComponent* NewSceneComponent(
    FCompileContext& Context,
    USceneComponent* Parent,
    UClass* Class,
    const FString& Label,
    const FTransform& LocalTransform)
{
    if (Context.Target == nullptr || Context.Components == nullptr) return Parent;
    const FName Name = MakeUniqueObjectName(Context.Target, Class, FName(*Label));
    constexpr EObjectFlags DerivedFlags =
        RF_Transactional | RF_Transient | RF_DuplicateTransient | RF_TextExportTransient;
    USceneComponent* Component = NewObject<USceneComponent>(
        Context.Target, Class, Name, DerivedFlags);
    Context.Target->AddInstanceComponent(Component);
    if (Parent != nullptr)
    {
        Component->SetupAttachment(Parent);
    }
    else if (Context.Target->GetRootComponent() == nullptr)
    {
        Context.Target->SetRootComponent(Component);
    }
    Component->RegisterComponent();
    Component->SetRelativeTransform(LocalTransform, false, nullptr, ETeleportType::TeleportPhysics);
    Context.Components->Add(Component);
    return Component;
}

bool WalkNodes(
    const TArray<FMHCompositeNode>& Nodes,
    FCompileContext& Context,
    USceneComponent* ParentComponent,
    const FMatrix& ParentWorld,
    const FString& PathPrefix,
    const TCHAR* SegmentLabel);

bool WalkCompositeReference(
    const FString& Resource,
    FCompileContext& Context,
    USceneComponent* ParentComponent,
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
        ParentComponent,
        ParentWorld,
        PathPrefix + TEXT(">") + Resource + TEXT(":"),
        TEXT("nodes"));
    Context.Ancestors.Pop();
    return bOk;
}

bool ValidateRandomOptions(
    const FMHCompositeNode& Node,
    FCompileContext& Context,
    const FMatrix& NodeWorld,
    const FString& NodePath)
{
    Context.bRequiresResolvedPlan = true;
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
                    nullptr,
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
    FCompileContext& Context,
    USceneComponent* ParentComponent,
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
            Context.bRequiresResolvedPlan = true;
            if (!ResolveProfile(Node.Profile, Context.Resolver, Context.Error)) return false;
        }

        USceneComponent* Component = nullptr;
        switch (Node.Kind)
        {
        case EMHCompositeNodeKind::Group:
            Component = NewSceneComponent(
                Context, ParentComponent, USceneComponent::StaticClass(), TEXT("MH_Group"), Local);
            break;
        case EMHCompositeNodeKind::Mesh:
        {
            UStaticMesh* Mesh = nullptr;
            if (!ResolveMesh(Node.Resource, Context.Resolver, Mesh, Context.Error)) return false;
            Component = NewSceneComponent(
                Context,
                ParentComponent,
                UStaticMeshComponent::StaticClass(),
                TEXT("MH_Mesh_") + Node.Resource,
                Local);
            if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Component))
            {
                MeshComponent->SetStaticMesh(Mesh);
            }
            break;
        }
        case EMHCompositeNodeKind::Actor:
        {
            UClass* ActorClass = nullptr;
            if (!ResolveActorClass(Node.Resource, Context.Settings, ActorClass, Context.Error)) return false;
            Component = NewSceneComponent(
                Context,
                ParentComponent,
                UChildActorComponent::StaticClass(),
                TEXT("MH_Actor_") + Node.Resource,
                Local);
            if (UChildActorComponent* ActorComponent = Cast<UChildActorComponent>(Component))
            {
                ActorComponent->SetChildActorClass(ActorClass);
            }
            break;
        }
        case EMHCompositeNodeKind::Composite:
            Component = NewSceneComponent(
                Context,
                ParentComponent,
                USceneComponent::StaticClass(),
                TEXT("MH_Composite_") + Node.Resource,
                Local);
            if (!WalkCompositeReference(
                    Node.Resource,
                    Context,
                    Component,
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
                Component,
                NodeWorld,
                NodePath + TEXT("/"),
                TEXT("children")))
        {
            return false;
        }
    }
    return true;
}

bool Run(
    AActor* Target,
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    TArray<TObjectPtr<UActorComponent>>* Components,
    USceneComponent** OutCreatedSyntheticRoot,
    bool* OutRequiresResolvedPlan,
    FString& OutError)
{
    if (OutCreatedSyntheticRoot != nullptr) *OutCreatedSyntheticRoot = nullptr;
    if (OutRequiresResolvedPlan != nullptr) *OutRequiresResolvedPlan = false;
    if (!MHIsCanonicalCompositeToken(LogicalName))
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: root composite logical name is not canonical");
        return false;
    }
    FCompileContext Context{Resolver, Settings, Target, Components};
    Context.Ancestors.Add(LogicalName);
    USceneComponent* Root = Target != nullptr ? Target->GetRootComponent() : nullptr;
    if (Target != nullptr && Root == nullptr)
    {
        const FTransform TargetWorld = Target->GetActorTransform();
        Root = NewObject<USceneComponent>(
            Target,
            USceneComponent::StaticClass(),
            MakeUniqueObjectName(Target, USceneComponent::StaticClass(), TEXT("MHCompositeRoot")),
            RF_Transactional | RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);
        Target->AddInstanceComponent(Root);
        Target->SetRootComponent(Root);
        Root->RegisterComponent();
        Root->SetWorldTransform(TargetWorld, false, nullptr, ETeleportType::TeleportPhysics);
        if (OutCreatedSyntheticRoot != nullptr) *OutCreatedSyntheticRoot = Root;
    }
    const FMatrix RootWorld = Target != nullptr
        ? Target->GetActorTransform().ToMatrixWithScale()
        : FMatrix::Identity;
    if (!WalkNodes(
            Document.Nodes,
            Context,
            Root,
            RootWorld,
            LogicalName + TEXT(":"),
            TEXT("nodes")))
    {
        OutError = MoveTemp(Context.Error);
        return false;
    }
    if (OutRequiresResolvedPlan != nullptr)
    {
        *OutRequiresResolvedPlan = Context.bRequiresResolvedPlan;
    }
    return true;
}

void DestroyCompileResult(
    AActor& Target,
    FMHCompositeCompileResult& Result,
    USceneComponent* CreatedSyntheticRoot)
{
    for (UActorComponent* Component : Result.Components)
    {
        if (Component != nullptr) Component->DestroyComponent();
    }
    Result.Components.Reset();
    if (CreatedSyntheticRoot != nullptr)
    {
        if (Target.GetRootComponent() == CreatedSyntheticRoot) Target.SetRootComponent(nullptr);
        CreatedSyntheticRoot->DestroyComponent();
    }
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
    return Run(
        nullptr,
        LogicalName,
        Document,
        Resolver,
        Settings,
        nullptr,
        nullptr,
        nullptr,
        OutError);
}

FMHCompositeCompileResult MHCompileCompositeV5(
    AActor& Target,
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings)
{
    FMHCompositeCompileResult Result;
    bool bRequiresResolvedPlan = false;
    if (!Run(
            nullptr,
            LogicalName,
            Document,
            Resolver,
            Settings,
            nullptr,
            nullptr,
            &bRequiresResolvedPlan,
            Result.Error))
    {
        return Result;
    }
    if (bRequiresResolvedPlan)
    {
        Result.Error = TEXT("MH_E_COMPOSITE_GRAMMAR: random/profile placement requires the V5-S5 resolved-plan consumer");
        return Result;
    }
    USceneComponent* CreatedSyntheticRoot = nullptr;
    if (!Run(
            &Target,
            LogicalName,
            Document,
            Resolver,
            Settings,
            &Result.Components,
            &CreatedSyntheticRoot,
            nullptr,
            Result.Error))
    {
        DestroyCompileResult(Target, Result, CreatedSyntheticRoot);
    }
    return Result;
}

bool MHProbeCompositeBuildV5(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError)
{
    OutError.Reset();
    bool bRequiresResolvedPlan = false;
    if (!Run(
            nullptr,
            LogicalName,
            Document,
            Resolver,
            Settings,
            nullptr,
            nullptr,
            &bRequiresResolvedPlan,
            OutError))
    {
        return false;
    }
    if (bRequiresResolvedPlan) return true;

    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview, false);
    if (World == nullptr)
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: cannot create transient compiler world");
        return false;
    }
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transient;
    AActor* Target = World->SpawnActor<AActor>(SpawnParameters);
    bool bSucceeded = Target != nullptr;
    if (!bSucceeded)
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: cannot create transient compiler actor");
    }
    else
    {
        const FMHCompositeCompileResult Compiled = MHCompileCompositeV5(
            *Target, LogicalName, Document, Resolver, Settings);
        bSucceeded = Compiled.Succeeded();
        OutError = Compiled.Error;
        Target->Destroy();
    }
    World->DestroyWorld(false);
    World->RemoveFromRoot();
    return bSucceeded;
}

} // namespace UE::MimirComposite
