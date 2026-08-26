#include "Composite/MHCompositePlacementCompiler.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/BoxComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Settings/MHCompositeSettings.h"

namespace UE::MimirComposite
{
namespace
{

constexpr TCHAR PlacementGeneratedMeshRoot[] = TEXT("/Game/MH/Generated/Meshes");
constexpr TCHAR PlacementGeneratedCompositeRoot[] = TEXT("/Game/MH/Generated/Composites");
constexpr EObjectFlags PlacementDerivedFlags =
    RF_Transactional | RF_Transient | RF_DuplicateTransient | RF_TextExportTransient;

FMHResourceKey PlacementResourceKey(const EMHResourceKind Kind, const FString& LogicalName)
{
    FMHResourceKey Key;
    Key.Kind = Kind;
    Key.LogicalName = LogicalName;
    return Key;
}

FTransform PlacementNodeTransform(const FMHCompositeNode& Node)
{
    return FTransform(Node.Transform.RotationQuat, Node.Transform.TranslationCm, Node.Transform.Scale);
}

struct FPlacementCompileContext
{
    AActor& Target;
    const UMHCompositeSettings& Settings;
    FMHCompositePlacementCompileResult& Result;
    TArray<FString> Ancestors;
};

USceneComponent* NewPlacementComponent(
    FPlacementCompileContext& Context,
    USceneComponent* Parent,
    UClass* Class,
    const FString& Label,
    const FTransform& LocalTransform)
{
    const FName Name = MakeUniqueObjectName(&Context.Target, Class, FName(*Label));
    USceneComponent* Component = NewObject<USceneComponent>(
        &Context.Target,
        Class,
        Name,
        PlacementDerivedFlags);
    Context.Target.AddInstanceComponent(Component);
    if (Parent != nullptr)
    {
        Component->SetupAttachment(Parent);
    }
    else if (Context.Target.GetRootComponent() != nullptr)
    {
        Component->SetupAttachment(Context.Target.GetRootComponent());
    }
    else
    {
        Context.Target.SetRootComponent(Component);
    }
    Component->RegisterComponent();
    Component->SetRelativeTransform(LocalTransform, false, nullptr, ETeleportType::TeleportPhysics);
    Context.Result.Components.Add(Component);
    return Component;
}

USceneComponent* NewUnresolvedPlacement(
    FPlacementCompileContext& Context,
    USceneComponent* Parent,
    const FString& Label,
    const FTransform& LocalTransform,
    const FString& Diagnostic)
{
    USceneComponent* Placeholder = NewPlacementComponent(
        Context,
        Parent,
        USceneComponent::StaticClass(),
        TEXT("MH_Unresolved_") + Label,
        LocalTransform);

    UBoxComponent* Box = Cast<UBoxComponent>(NewPlacementComponent(
        Context,
        Placeholder,
        UBoxComponent::StaticClass(),
        TEXT("MH_UnresolvedBox_") + Label,
        FTransform::Identity));
    if (Box != nullptr)
    {
        Box->SetBoxExtent(FVector(50.0));
        Box->ShapeColor = FColor::Red;
        Box->bDrawOnlyIfSelected = false;
        Box->SetLineThickness(3.0f);
        Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Box->SetHiddenInGame(false);
    }

    UTextRenderComponent* Text = Cast<UTextRenderComponent>(NewPlacementComponent(
        Context,
        Placeholder,
        UTextRenderComponent::StaticClass(),
        TEXT("MH_UnresolvedLabel_") + Label,
        FTransform::Identity));
    if (Text != nullptr)
    {
        Text->SetText(FText::FromString(Label));
        Text->SetTextRenderColor(FColor::Red);
        Text->SetHorizontalAlignment(EHTA_Center);
        Text->SetWorldSize(24.0f);
        Text->SetRelativeLocation(FVector(0.0, 0.0, 60.0));
        Text->SetHiddenInGame(false);
    }

    Context.Result.Warnings.Add(FString::Printf(
        TEXT("MH_W_UNRESOLVED_PLACEMENT: %s"),
        *Diagnostic));
    return Placeholder;
}

UStaticMesh* LoadPlacementMesh(const FString& Resource)
{
    const FString ObjectPath = FString::Printf(
        TEXT("%s/%s.%s"),
        PlacementGeneratedMeshRoot,
        *Resource,
        *Resource);
    return LoadObject<UStaticMesh>(nullptr, *ObjectPath);
}

UMHCompositeAsset* LoadPlacementComposite(const FString& Resource)
{
    const FString ObjectPath = FString::Printf(
        TEXT("%s/%s.%s"),
        PlacementGeneratedCompositeRoot,
        *Resource,
        *Resource);
    return LoadObject<UMHCompositeAsset>(nullptr, *ObjectPath);
}

UClass* LoadPlacementActorClass(
    const FString& Resource,
    const UMHCompositeSettings& Settings)
{
    const FSoftClassPath* ClassPath = Settings.ActorClassRegistry.Find(Resource);
    if (ClassPath == nullptr || ClassPath->IsNull())
    {
        return nullptr;
    }
    UClass* Class = ClassPath->TryLoadClass<AActor>();
    return Class != nullptr && Class->IsChildOf(AActor::StaticClass()) ? Class : nullptr;
}

bool WalkPlacementNodes(
    const TArray<FMHCompositeNode>& Nodes,
    FPlacementCompileContext& Context,
    USceneComponent* StructuralParent,
    const bool bRecordTopLevel)
{
    for (const FMHCompositeNode& Node : Nodes)
    {
        const FTransform LocalTransform = PlacementNodeTransform(Node);
        USceneComponent* Component = nullptr;
        switch (Node.Kind)
        {
        case EMHCompositeNodeKind::Group:
            Component = NewPlacementComponent(
                Context,
                StructuralParent,
                USceneComponent::StaticClass(),
                TEXT("MH_Group"),
                LocalTransform);
            break;
        case EMHCompositeNodeKind::Mesh:
        {
            const FMHResourceKey Key = PlacementResourceKey(EMHResourceKind::StaticMesh, Node.Resource);
            Context.Result.Dependencies.Add(Key);
            if (UStaticMesh* Mesh = LoadPlacementMesh(Node.Resource))
            {
                Component = NewPlacementComponent(
                    Context,
                    StructuralParent,
                    UStaticMeshComponent::StaticClass(),
                    TEXT("MH_Mesh_") + Node.Resource,
                    LocalTransform);
                CastChecked<UStaticMeshComponent>(Component)->SetStaticMesh(Mesh);
            }
            else
            {
                Component = NewUnresolvedPlacement(
                    Context,
                    StructuralParent,
                    Key.ToString(),
                    LocalTransform,
                    FString::Printf(TEXT("%s has no generated static mesh"), *Key.ToString()));
            }
            break;
        }
        case EMHCompositeNodeKind::Actor:
        {
            if (UClass* ActorClass = LoadPlacementActorClass(Node.Resource, Context.Settings))
            {
                Component = NewPlacementComponent(
                    Context,
                    StructuralParent,
                    UChildActorComponent::StaticClass(),
                    TEXT("MH_Actor_") + Node.Resource,
                    LocalTransform);
                UChildActorComponent* ChildActorComponent = CastChecked<UChildActorComponent>(Component);
#if WITH_EDITOR
                // The composite owns one persisted Outliner row. Its derived
                // child actor must not leak an unnamed transient row while the
                // component is being reconstructed.
                ChildActorComponent->SetEditorTreeViewVisualizationMode(
                    EChildActorComponentTreeViewVisualizationMode::Hidden);
#endif
                ChildActorComponent->SetChildActorClass(ActorClass);
                if (AActor* ChildActor = ChildActorComponent->GetChildActor())
                {
                    const FString DisplayLabel = Node.Name.IsEmpty() ? Node.Resource : Node.Name;
                    ChildActor->SetActorLabel(DisplayLabel, false);
                }
            }
            else
            {
                Component = NewUnresolvedPlacement(
                    Context,
                    StructuralParent,
                    TEXT("actor:") + Node.Resource,
                    LocalTransform,
                    FString::Printf(
                        TEXT("actor:%s is unavailable in ActorClassRegistry"),
                        *Node.Resource));
            }
            break;
        }
        case EMHCompositeNodeKind::Composite:
        {
            const FMHResourceKey Key = PlacementResourceKey(EMHResourceKind::Composite, Node.Resource);
            Context.Result.Dependencies.Add(Key);
            UMHCompositeAsset* NestedAsset = LoadPlacementComposite(Node.Resource);
            if (NestedAsset == nullptr)
            {
                Component = NewUnresolvedPlacement(
                    Context,
                    StructuralParent,
                    Key.ToString(),
                    LocalTransform,
                    FString::Printf(TEXT("%s has no generated composite asset"), *Key.ToString()));
                break;
            }
            if (Context.Ancestors.Contains(Node.Resource))
            {
                Context.Result.Error = FString::Printf(
                    TEXT("MH_E_COMPOSITE_CYCLE: composite:%s includes itself or an ancestor"),
                    *Node.Resource);
                return false;
            }

            FMHCompositeDocument NestedDocument;
            if (!MHExtractCompositeV5(*NestedAsset, NestedDocument, Context.Result.Error))
            {
                return false;
            }
            Component = NewPlacementComponent(
                Context,
                StructuralParent,
                USceneComponent::StaticClass(),
                TEXT("MH_Composite_") + Node.Resource,
                LocalTransform);
            Context.Ancestors.Add(Node.Resource);
            const bool bNestedOk = WalkPlacementNodes(
                NestedDocument.Nodes,
                Context,
                Component,
                false);
            Context.Ancestors.Pop();
            if (!bNestedOk)
            {
                return false;
            }
            break;
        }
        case EMHCompositeNodeKind::Random:
            Context.Result.Error = TEXT(
                "MH_E_COMPOSITE_GRAMMAR: random placement requires the V5-S5 resolved-plan consumer");
            return false;
        }

        if (bRecordTopLevel)
        {
            Context.Result.TopLevelComponents.Add(Component);
        }
        if (!WalkPlacementNodes(
                Node.Children,
                Context,
                Component,
                false))
        {
            return false;
        }
    }
    return true;
}

bool PreflightPlacementNodes(
    const TArray<FMHCompositeNode>& Nodes,
    FPlacementCompileContext& Context,
    const FMatrix& ParentWorld,
    const FString& PathPrefix)
{
    for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
    {
        const FMHCompositeNode& Node = Nodes[NodeIndex];
        const FString NodePath = FString::Printf(
            TEXT("%snodes[%d]"), *PathPrefix, NodeIndex);
        const FMatrix NodeWorld =
            PlacementNodeTransform(Node).ToMatrixWithScale() * ParentWorld;
        if (!MHIsRepresentableTransformMatrix(NodeWorld))
        {
            Context.Result.Error = FString::Printf(
                TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: %s cannot round-trip through FTransform within 8 ULP"),
                *NodePath);
            return false;
        }
        if (Node.Kind == EMHCompositeNodeKind::Random || !Node.Profile.IsEmpty())
        {
            Context.Result.Error = FString::Printf(
                TEXT("MH_E_COMPOSITE_GRAMMAR: %s requires the V5-S5 resolved-plan consumer"),
                *NodePath);
            return false;
        }
        if (Node.Kind == EMHCompositeNodeKind::Composite)
        {
            if (Context.Ancestors.Contains(Node.Resource))
            {
                Context.Result.Error = FString::Printf(
                    TEXT("MH_E_COMPOSITE_CYCLE: composite:%s includes itself or an ancestor"),
                    *Node.Resource);
                return false;
            }
            if (UMHCompositeAsset* NestedAsset = LoadPlacementComposite(Node.Resource))
            {
                FMHCompositeDocument NestedDocument;
                if (!MHExtractCompositeV5(*NestedAsset, NestedDocument, Context.Result.Error)) return false;
                Context.Ancestors.Add(Node.Resource);
                const bool bNestedOk = PreflightPlacementNodes(
                    NestedDocument.Nodes,
                    Context,
                    NodeWorld,
                    NodePath + TEXT(">") + Node.Resource + TEXT(":"));
                Context.Ancestors.Pop();
                if (!bNestedOk) return false;
            }
        }
        if (!PreflightPlacementNodes(
                Node.Children,
                Context,
                NodeWorld,
                NodePath + TEXT("/children/")))
        {
            return false;
        }
    }
    return true;
}

void DestroyPlacementComponents(TArray<TObjectPtr<UActorComponent>>& Components)
{
    for (int32 Index = Components.Num() - 1; Index >= 0; --Index)
    {
        if (UActorComponent* Component = Components[Index])
        {
            Component->DestroyComponent();
        }
    }
    Components.Reset();
}

} // namespace

FMHCompositePlacementCompileResult MHCompileCompositePlacementV5(
    AActor& Target,
    const UMHCompositeAsset* Asset,
    const FString& ExpectedLogicalName,
    const UMHCompositeSettings& Settings)
{
    FMHCompositePlacementCompileResult Result;
    const FMHResourceKey RootKey = PlacementResourceKey(
        EMHResourceKind::Composite,
        ExpectedLogicalName);
    if (!RootKey.IsCanonical())
    {
        Result.Error = TEXT("MH_E_COMPOSITE_GRAMMAR: placement root logical name is not canonical");
        return Result;
    }
    Result.Dependencies.Add(RootKey);

    FPlacementCompileContext Context{Target, Settings, Result};
    Context.Ancestors.Add(ExpectedLogicalName);
    USceneComponent* Root = Target.GetRootComponent();
    if (Asset == nullptr)
    {
        USceneComponent* Placeholder = NewUnresolvedPlacement(
            Context,
            Root,
            RootKey.ToString(),
            FTransform::Identity,
            FString::Printf(TEXT("%s asset reference is unavailable"), *RootKey.ToString()));
        Result.TopLevelComponents.Add(Placeholder);
        return Result;
    }

    FMHCompositeDocument Document;
    if (!MHExtractCompositeV5(*Asset, Document, Result.Error) ||
        !PreflightPlacementNodes(
            Document.Nodes,
            Context,
            Target.GetActorTransform().ToMatrixWithScale(),
            ExpectedLogicalName + TEXT(":")) ||
        !WalkPlacementNodes(
            Document.Nodes,
            Context,
            Root,
            true))
    {
        DestroyPlacementComponents(Result.Components);
        Result.TopLevelComponents.Reset();
    }
    return Result;
}

} // namespace UE::MimirComposite
