#include "Composite/MHCompositeThumbnailRenderer.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeProtocol.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "ShowFlags.h"
#include "ThumbnailHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeThumbnailRenderer)

namespace
{
constexpr int32 MHCompositeThumbnailComponentBudget = 256;
constexpr int32 MHCompositeThumbnailNodeBudget = 1024;
constexpr int32 MHCompositeThumbnailDepthBudget = 64;
constexpr TCHAR MHCompositeThumbnailMeshRoot[] = TEXT("/Game/MH/Generated/Meshes");
constexpr TCHAR MHCompositeThumbnailCompositeRoot[] = TEXT("/Game/MH/Generated/Composites");

FTransform MHCompositeThumbnailNodeTransform(const UE::MimirComposite::FMHCompositeNode& Node)
{
    return FTransform(Node.Transform.RotationQuat, Node.Transform.TranslationCm, Node.Transform.Scale);
}

UStaticMesh* MHLoadCompositeThumbnailMesh(const FString& Resource)
{
    return LoadObject<UStaticMesh>(nullptr, *FString::Printf(
        TEXT("%s/%s.%s"),
        MHCompositeThumbnailMeshRoot,
        *Resource,
        *Resource));
}

UMHCompositeAsset* MHLoadCompositeThumbnailComposite(const FString& Resource)
{
    return LoadObject<UMHCompositeAsset>(nullptr, *FString::Printf(
        TEXT("%s/%s.%s"),
        MHCompositeThumbnailCompositeRoot,
        *Resource,
        *Resource));
}
} // namespace

class FMHCompositeThumbnailScene final : public FThumbnailPreviewScene
{
public:
    FMHCompositeThumbnailScene()
        : FThumbnailPreviewScene()
    {
        bForceAllUsedMipsResident = false;
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        SpawnParameters.ObjectFlags = RF_Transient;
        PreviewActor = GetWorld()->SpawnActor<AActor>(
            AActor::StaticClass(),
            FTransform::Identity,
            SpawnParameters);
        check(PreviewActor != nullptr);
        PreviewActor->SetActorEnableCollision(false);
        Root = NewObject<USceneComponent>(PreviewActor, TEXT("MHCompositeThumbnailRoot"), RF_Transient);
        PreviewActor->AddInstanceComponent(Root);
        PreviewActor->SetRootComponent(Root);
        Root->RegisterComponent();
    }

    void SetCompositeAsset(UMHCompositeAsset* Asset)
    {
        ClearDerivedComponents();
        VisitedNodeCount = 0;
        if (Asset == nullptr)
        {
            return;
        }

        TArray<FString> Ancestors;
        Ancestors.Add(Asset->LogicalName);
        AddCompositeAsset(*Asset, FTransform::Identity, Ancestors, 0);
        if (DerivedComponents.IsEmpty())
        {
            AddPlaceholder(FTransform::Identity);
        }
    }

    virtual void GetViewMatrixParameters(
        const float InFOVDegrees,
        FVector& OutOrigin,
        float& OutOrbitPitch,
        float& OutOrbitYaw,
        float& OutOrbitZoom) const override
    {
        FBox Bounds = PreviewActor->GetComponentsBoundingBox(false);
        if (!Bounds.IsValid)
        {
            Bounds = FBox(FVector(-50.0), FVector(50.0));
        }
        const FBoxSphereBounds SphereBounds(Bounds);
        const float HalfFOVRadians = FMath::DegreesToRadians(InFOVDegrees) * 0.5f;
        const float TargetDistance = FMath::Max(1.0f, SphereBounds.SphereRadius) /
            FMath::Tan(HalfFOVRadians);
        OutOrigin = -SphereBounds.Origin;
        OutOrbitPitch = -11.25f;
        OutOrbitYaw = -157.5f;
        OutOrbitZoom = TargetDistance * 1.15f;
    }

private:
    void ClearDerivedComponents()
    {
        for (int32 Index = DerivedComponents.Num() - 1; Index >= 0; --Index)
        {
            if (UActorComponent* Component = DerivedComponents[Index])
            {
                Component->DestroyComponent();
            }
        }
        DerivedComponents.Reset();
    }

    void AddMesh(UStaticMesh& Mesh, const FTransform& WorldTransform)
    {
        if (DerivedComponents.Num() >= MHCompositeThumbnailComponentBudget)
        {
            return;
        }
        UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(
            PreviewActor,
            MakeUniqueObjectName(PreviewActor, UStaticMeshComponent::StaticClass(), TEXT("MHThumbnailMesh")),
            RF_Transient);
        PreviewActor->AddInstanceComponent(Component);
        Component->SetupAttachment(Root);
        Component->SetStaticMesh(&Mesh);
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->RegisterComponent();
        Component->SetWorldTransform(WorldTransform, false, nullptr, ETeleportType::TeleportPhysics);
        DerivedComponents.Add(Component);
    }

    void AddPlaceholder(const FTransform& WorldTransform)
    {
        UStaticMesh* Cube = LoadObject<UStaticMesh>(
            nullptr,
            TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (Cube == nullptr)
        {
            return;
        }
        FTransform PlaceholderTransform = WorldTransform;
        PlaceholderTransform.SetScale3D(WorldTransform.GetScale3D() * 0.25);
        AddMesh(*Cube, PlaceholderTransform);
    }

    void AddCompositeAsset(
        UMHCompositeAsset& Asset,
        const FTransform& DocumentBasis,
        TArray<FString>& Ancestors,
        const int32 Depth)
    {
        if (DerivedComponents.Num() >= MHCompositeThumbnailComponentBudget ||
            VisitedNodeCount >= MHCompositeThumbnailNodeBudget ||
            Depth > MHCompositeThumbnailDepthBudget)
        {
            return;
        }
        UE::MimirComposite::FMHCompositeDocument Document;
        FString Error;
        if (!UE::MimirComposite::MHExtractCompositeV4(Asset, Document, Error))
        {
            AddPlaceholder(DocumentBasis);
            return;
        }
        AddNodes(Document.Nodes, DocumentBasis, Ancestors, Depth);
    }

    void AddNodes(
        const TArray<UE::MimirComposite::FMHCompositeNode>& Nodes,
        const FTransform& DocumentBasis,
        TArray<FString>& Ancestors,
        const int32 Depth)
    {
        if (Depth > MHCompositeThumbnailDepthBudget)
        {
            return;
        }
        for (const UE::MimirComposite::FMHCompositeNode& Node : Nodes)
        {
            if (DerivedComponents.Num() >= MHCompositeThumbnailComponentBudget ||
                VisitedNodeCount >= MHCompositeThumbnailNodeBudget)
            {
                return;
            }
            ++VisitedNodeCount;
            const FTransform SourceWorld = MHCompositeThumbnailNodeTransform(Node) * DocumentBasis;
            switch (Node.Kind)
            {
            case EMHCompositeNodeKind::Mesh:
                if (UStaticMesh* Mesh = MHLoadCompositeThumbnailMesh(Node.Resource))
                {
                    AddMesh(*Mesh, SourceWorld);
                }
                else
                {
                    AddPlaceholder(SourceWorld);
                }
                break;
            case EMHCompositeNodeKind::Composite:
                if (Ancestors.Contains(Node.Resource))
                {
                    AddPlaceholder(SourceWorld);
                }
                else if (UMHCompositeAsset* Nested = MHLoadCompositeThumbnailComposite(Node.Resource))
                {
                    Ancestors.Add(Node.Resource);
                    AddCompositeAsset(*Nested, SourceWorld, Ancestors, Depth + 1);
                    Ancestors.Pop();
                }
                else
                {
                    AddPlaceholder(SourceWorld);
                }
                break;
            case EMHCompositeNodeKind::Actor:
                // Never run arbitrary ActorClassRegistry constructors from a
                // Content Browser thumbnail. A neutral cube is the safe view.
                AddPlaceholder(SourceWorld);
                break;
            case EMHCompositeNodeKind::Group:
                break;
            }

            // Mirror the accepted placement compiler: hierarchy is structural
            // and child transforms remain document-world.
            AddNodes(Node.Children, DocumentBasis, Ancestors, Depth + 1);
        }
    }

    TObjectPtr<AActor> PreviewActor;
    TObjectPtr<USceneComponent> Root;
    TArray<TObjectPtr<UActorComponent>> DerivedComponents;
    int32 VisitedNodeCount = 0;
};

void UMHCompositeThumbnailRenderer::Draw(
    UObject* Object,
    const int32 X,
    const int32 Y,
    const uint32 Width,
    const uint32 Height,
    FRenderTarget* RenderTarget,
    FCanvas* Canvas,
    const bool bAdditionalViewFamily)
{
    UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Object);
    if (!IsValid(Asset))
    {
        return;
    }
    if (ThumbnailScene == nullptr || ThumbnailScene->GetWorld() == nullptr)
    {
        delete ThumbnailScene;
        ThumbnailScene = new FMHCompositeThumbnailScene();
    }
    ThumbnailScene->SetCompositeAsset(Asset);
    ThumbnailScene->GetScene()->UpdateSpeedTreeWind(0.0);

    FSceneViewFamilyContext ViewFamily(
        FSceneViewFamily::ConstructionValues(
            RenderTarget,
            ThumbnailScene->GetScene(),
            FEngineShowFlags(ESFIM_Game))
        .SetTime(UThumbnailRenderer::GetTime())
        .SetAdditionalViewFamily(bAdditionalViewFamily));
    ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
    ViewFamily.EngineShowFlags.MotionBlur = 0;
    ViewFamily.EngineShowFlags.LOD = 0;
    RenderViewFamily(
        Canvas,
        &ViewFamily,
        ThumbnailScene->CreateView(&ViewFamily, X, Y, Width, Height));
}

bool UMHCompositeThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
    return IsValid(Cast<UMHCompositeAsset>(Object));
}

EThumbnailRenderFrequency UMHCompositeThumbnailRenderer::GetThumbnailRenderFrequency(UObject* Object) const
{
    return EThumbnailRenderFrequency::OnPropertyChange;
}

void UMHCompositeThumbnailRenderer::BeginDestroy()
{
    delete ThumbnailScene;
    ThumbnailScene = nullptr;
    Super::BeginDestroy();
}
