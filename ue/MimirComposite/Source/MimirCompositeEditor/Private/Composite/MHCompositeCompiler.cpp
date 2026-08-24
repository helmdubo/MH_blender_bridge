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
        return Unresolved(OutError, FString::Printf(TEXT("composite:%s does not resolve uniquely"), *Resource));
    }
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Outcome.PayloadPath))
    {
        return Unresolved(OutError, FString::Printf(TEXT("cannot read source for composite:%s"), *Resource));
    }
    if (!Outcome.RawHash.IsEmpty() && MHRawPayloadHash(Bytes) != Outcome.RawHash)
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: composite:%s changed after source resolution"),
            *Resource);
        return false;
    }
    return MHParseCompositeV4(Bytes, OutDocument, OutError);
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
        return Unresolved(OutError, FString::Printf(TEXT("static_mesh:%s does not resolve uniquely"), *Resource));
    }
    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), GeneratedMeshRoot, *Resource, *Resource);
    OutMesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
    if (OutMesh == nullptr)
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("static_mesh:%s has source but generated mesh %s is unavailable"), *Resource, *ObjectPath));
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
        return Unresolved(OutError, FString::Printf(TEXT("actor:%s is absent from ActorClassRegistry"), *Resource));
    }
    OutClass = Path->TryLoadClass<AActor>();
    if (OutClass == nullptr || !OutClass->IsChildOf(AActor::StaticClass()))
    {
        return Unresolved(OutError, FString::Printf(
            TEXT("actor:%s registry path is not a loadable AActor class: %s"), *Resource, *Path->ToString()));
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
};

USceneComponent* NewSceneComponent(
    FCompileContext& Context,
    USceneComponent* Parent,
    UClass* Class,
    const FString& Label,
    const FTransform& SourceWorld)
{
    if (Context.Target == nullptr || Context.Components == nullptr)
    {
        return Parent;
    }
    const FName Name = MakeUniqueObjectName(Context.Target, Class, FName(*Label));
    constexpr EObjectFlags DerivedFlags =
        RF_Transactional | RF_Transient | RF_DuplicateTransient | RF_TextExportTransient;
    USceneComponent* Component = NewObject<USceneComponent>(Context.Target, Class, Name, DerivedFlags);
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
    // §6 stores UE world transforms. Attachment hierarchy is structural only.
    Component->SetWorldTransform(SourceWorld, false, nullptr, ETeleportType::TeleportPhysics);
    Context.Components->Add(Component);
    return Component;
}

bool WalkNodes(
    const TArray<FMHCompositeNode>& Nodes,
    FCompileContext& Context,
    USceneComponent* StructuralParent,
    const FTransform& DocumentBasis)
{
    for (const FMHCompositeNode& Node : Nodes)
    {
        // Authored nodes are world transforms inside their source document. A
        // nested resource uses its reference placement as that document's basis.
        const FTransform SourceWorld = NodeTransform(Node) * DocumentBasis;
        USceneComponent* Component = nullptr;
        switch (Node.Kind)
        {
        case EMHCompositeNodeKind::Group:
            Component = NewSceneComponent(Context, StructuralParent, USceneComponent::StaticClass(),
                TEXT("MH_Group"), SourceWorld);
            break;
        case EMHCompositeNodeKind::Mesh:
        {
            UStaticMesh* Mesh = nullptr;
            if (!ResolveMesh(Node.Resource, Context.Resolver, Mesh, Context.Error)) return false;
            Component = NewSceneComponent(Context, StructuralParent, UStaticMeshComponent::StaticClass(),
                FString(TEXT("MH_Mesh_")) + Node.Resource, SourceWorld);
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
            Component = NewSceneComponent(Context, StructuralParent, UChildActorComponent::StaticClass(),
                FString(TEXT("MH_Actor_")) + Node.Resource, SourceWorld);
            if (UChildActorComponent* ActorComponent = Cast<UChildActorComponent>(Component))
            {
                ActorComponent->SetChildActorClass(ActorClass);
            }
            break;
        }
        case EMHCompositeNodeKind::Composite:
        {
            if (Context.Ancestors.Contains(Node.Resource))
            {
                Context.Error = FString::Printf(
                    TEXT("MH_E_COMPOSITE_CYCLE: composite:%s includes itself or an ancestor"),
                    *Node.Resource);
                return false;
            }
            FMHCompositeDocument Nested;
            if (!LoadCompositeDocument(Node.Resource, Context.Resolver, Nested, Context.Error)) return false;
            Component = NewSceneComponent(Context, StructuralParent, USceneComponent::StaticClass(),
                FString(TEXT("MH_Composite_")) + Node.Resource, SourceWorld);
            Context.Ancestors.Add(Node.Resource);
            const bool bNestedOk = WalkNodes(Nested.Nodes, Context, Component, SourceWorld);
            Context.Ancestors.Pop();
            if (!bNestedOk) return false;
            break;
        }
        }
        // Children belong to the current source document and therefore retain
        // their own authored world placement; only structural attachment changes.
        if (!WalkNodes(Node.Children, Context, Component, DocumentBasis)) return false;
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
    FString& OutError)
{
    if (OutCreatedSyntheticRoot != nullptr) *OutCreatedSyntheticRoot = nullptr;
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
        // One synthetic root keeps every authored top-level component attached
        // to the target actor. It is structural and deliberately excluded from
        // the authored Components result.
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
    const FTransform DocumentBasis = Target != nullptr
        ? Target->GetActorTransform()
        : FTransform::Identity;
    if (!WalkNodes(Document.Nodes, Context, Root, DocumentBasis))
    {
        OutError = MoveTemp(Context.Error);
        return false;
    }
    return true;
}

} // namespace

bool MHValidateCompositeClosureV4(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError)
{
    OutError.Reset();
    return Run(nullptr, LogicalName, Document, Resolver, Settings, nullptr, nullptr, OutError);
}

FMHCompositeCompileResult MHCompileCompositeV4(
    AActor& Target,
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings)
{
    FMHCompositeCompileResult Result;
    if (!MHValidateCompositeClosureV4(LogicalName, Document, Resolver, Settings, Result.Error))
    {
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
            Result.Error))
    {
        for (UActorComponent* Component : Result.Components)
        {
            if (Component != nullptr) Component->DestroyComponent();
        }
        Result.Components.Reset();
        if (CreatedSyntheticRoot != nullptr)
        {
            if (Target.GetRootComponent() == CreatedSyntheticRoot)
            {
                Target.SetRootComponent(nullptr);
            }
            CreatedSyntheticRoot->DestroyComponent();
        }
    }
    return Result;
}

bool MHProbeCompositeBuildV4(
    const FString& LogicalName,
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    const UMHCompositeSettings& Settings,
    FString& OutError)
{
    OutError.Reset();
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
        const FMHCompositeCompileResult Compiled = MHCompileCompositeV4(
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
