#include "Composite/MHCompositeLevelSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAppearanceTransport.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeCompiler.h"
#include "Composite/MHCompiledRecipe.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Index/MHProjectResourceIndex.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadScanResolver.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeLevelSubsystem)

namespace UE::MimirComposite
{
namespace
{

constexpr const TCHAR* MHLevelGeneratedCompositeRoot = TEXT("/Game/MH/Generated/Composites");
constexpr const TCHAR* MHLevelGeneratedMeshRoot = TEXT("/Game/MH/Generated/Meshes");

struct FMHBreakSpawnSpec
{
    EMHRandomSemanticKind Kind = EMHRandomSemanticKind::Empty;
    FString Resource;
    FString DisplayLabel;
    FTransform WorldTransform = FTransform::Identity;
    TObjectPtr<UStaticMesh> Mesh;
    TObjectPtr<UClass> ActorClass;
    TObjectPtr<UMHCompositeAsset> CompositeAsset;
    int32 Seed = 0;
    int32 AppearanceSeed = 0;
    float AppearanceChannels[MH_APPEARANCE_CHANNELS] = {};
    int32 AppearanceBaseIndex = 0;
    FName FolderPath;
};

const TCHAR* MHBreakLeafKindLabel(const EMHRandomSemanticKind Kind)
{
    switch (Kind)
    {
    case EMHRandomSemanticKind::Mesh:
        return TEXT("mesh");
    case EMHRandomSemanticKind::Actor:
        return TEXT("actor");
    case EMHRandomSemanticKind::Composite:
        return TEXT("composite");
    default:
        return TEXT("unknown");
    }
}

bool MHReverseLookupActorToken(
    const AActor& Actor,
    const UMHCompositeSettings& Settings,
    FString& OutToken,
    FString& OutReason)
{
    OutToken.Reset();
    TArray<FString> Matches;
    for (const TPair<FString, FSoftClassPath>& Pair : Settings.ActorClassRegistry)
    {
        UClass* RegisteredClass = Pair.Value.TryLoadClass<AActor>();
        if (RegisteredClass == Actor.GetClass())
        {
            Matches.Add(Pair.Key);
        }
    }
    Matches.Sort();
    if (Matches.Num() != 1 || !MHIsCanonicalCompositeToken(Matches[0]))
    {
        OutReason = Matches.IsEmpty()
            ? FString::Printf(TEXT("actor class '%s' has no ActorClassRegistry reverse match"), *Actor.GetClass()->GetPathName())
            : FString::Printf(TEXT("actor class '%s' has %d ActorClassRegistry reverse matches"), *Actor.GetClass()->GetPathName(), Matches.Num());
        return false;
    }
    OutToken = MoveTemp(Matches[0]);
    return true;
}

bool MHBuildNodeForActor(
    AActor& Actor,
    const FTransform& Pivot,
    const UMHCompositeSettings& Settings,
    FMHCompositeNode& OutNode,
    FString& OutReason)
{
    OutNode = FMHCompositeNode();
    const FMatrix LocalMatrix = Actor.GetActorTransform().ToMatrixWithScale() * Pivot.ToInverseMatrixWithScale();
    if (!MHIsRepresentableTransformMatrix(LocalMatrix))
    {
        OutReason = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: actor transform is not representable as parent-local T/R/S within 8 float32 ULP");
        return false;
    }
    const FTransform LocalTransform(LocalMatrix);
    OutNode.Transform.TranslationCm = LocalTransform.GetTranslation();
    OutNode.Transform.RotationQuat = LocalTransform.GetRotation();
    OutNode.Transform.Scale = LocalTransform.GetScale3D();
    OutNode.Name = Actor.GetActorLabel();

    if (AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(&Actor))
    {
        UStaticMesh* Mesh = StaticMeshActor->GetStaticMeshComponent() != nullptr
            ? StaticMeshActor->GetStaticMeshComponent()->GetStaticMesh()
            : nullptr;
        const UMHStaticMeshImportData* Receipt = Mesh != nullptr
            ? Cast<UMHStaticMeshImportData>(Mesh->GetAssetImportData())
            : nullptr;
        FMHResourceKey MeshKey;
        MeshKey.Kind = EMHResourceKind::StaticMesh;
        MeshKey.LogicalName = Receipt != nullptr ? Receipt->LogicalName : FString();
        if (Receipt == nullptr || !MeshKey.IsCanonical())
        {
            OutReason = TEXT("StaticMeshActor does not reference a managed canonical static mesh");
            return false;
        }
        OutNode.Kind = EMHCompositeNodeKind::Mesh;
        OutNode.Resource = Receipt->LogicalName;
        return true;
    }

    if (AMHCompositeActor* CompositeActor = Cast<AMHCompositeActor>(&Actor))
    {
        UMHCompositeAsset* Asset = CompositeActor->GetCompositeAsset();
        if (Asset == nullptr || !MHIsCanonicalCompositeToken(Asset->LogicalName) || Asset->SourceRelativePath.IsEmpty())
        {
            OutReason = TEXT("AMHCompositeActor does not reference a live managed composite");
            return false;
        }
        OutNode.Kind = EMHCompositeNodeKind::Composite;
        OutNode.Resource = Asset->LogicalName;
        return true;
    }

    if (!MHReverseLookupActorToken(Actor, Settings, OutNode.Resource, OutReason))
    {
        return false;
    }
    OutNode.Kind = EMHCompositeNodeKind::Actor;
    return true;
}

FBox MHSelectionBounds(const TArray<AActor*>& Actors)
{
    FBox Bounds(ForceInit);
    for (const AActor* Actor : Actors)
    {
        if (Actor == nullptr)
        {
            continue;
        }
        const FBox ActorBounds = Actor->GetComponentsBoundingBox(true);
        if (ActorBounds.IsValid)
        {
            Bounds += ActorBounds;
        }
        else if (!Actor->GetActorLocation().ContainsNaN())
        {
            Bounds += Actor->GetActorLocation();
        }
    }
    return Bounds;
}

bool MHCollectBreakSpecs(
    const FMHResolvedCompositePlan& Plan,
    const FMHCompiledRecipe& Recipe,
    const FTransform& PlacementTransform,
    const UMHCompositeSettings& Settings,
    const int32 Seed,
    const int32 AppearanceSeed,
    const FName FolderPath,
    TArray<FMHBreakSpawnSpec>& OutSpecs,
    FString& OutError)
{
    if (!MHValidateResolvedPlacementTransforms(Plan, PlacementTransform, OutError))
    {
        return false;
    }
    const FMatrix PlacementWorld = PlacementTransform.ToMatrixWithScale();
    // Lookup only: top-layer selection still comes from Nodes, so nested
    // composite leaves never become separate actors during this Break.
    TMap<int32, const FMHResolvedCompositeLeaf*> MeshLeaves;
    for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
    {
        if (Leaf.Kind == EMHRandomSemanticKind::Mesh)
            MeshLeaves.Add(Leaf.OwningResolvedNodeIndex, &Leaf);
    }
    for (const FMHResolvedCompositeNode& Node : Plan.Nodes)
    {
        // A '>' enters a nested composite. Break preserves that composite as
        // one actor, so none of its internal resolved nodes belong to this layer.
        if (Node.NodePath.Contains(TEXT(">"))) continue;

        EMHRandomSemanticKind Kind = Node.SemanticKind;
        FString Resource = Node.Resource;
        if (Kind == EMHRandomSemanticKind::Random)
        {
            const FMHCompiledRecipeComponent* Component = Recipe.Components.FindByPredicate(
                [&Node](const FMHCompiledRecipeComponent& Value)
                {
                    return Value.NodePath == Node.NodePath;
                });
            if (Component == nullptr || !Component->Options.IsValidIndex(Node.SelectedOptionIndex))
            {
                OutError = FString::Printf(
                    TEXT("MH_E_INVALID_RESOURCE_SOURCE: resolved random node %s has no current selected option"),
                    *Node.NodePath);
                return false;
            }
            const FMHCompiledRecipeOption& Option = Component->Options[Node.SelectedOptionIndex];
            Kind = Option.Kind;
            Resource = Option.Resource;
        }

        // Groups only carry transforms for their promoted descendants. Empty
        // and gameobj currently have no level-entity representation.
        if (Kind == EMHRandomSemanticKind::Group || Kind == EMHRandomSemanticKind::Empty ||
            Kind == EMHRandomSemanticKind::GameObj || Kind == EMHRandomSemanticKind::Random) continue;
        if (Kind != EMHRandomSemanticKind::Mesh && Kind != EMHRandomSemanticKind::Actor &&
            Kind != EMHRandomSemanticKind::Composite)
        {
            OutError = FString::Printf(
                TEXT("MH_E_INVALID_RESOURCE_SOURCE: resolved Break node %s has an unsupported semantic kind"),
                *Node.NodePath);
            return false;
        }

        FMHBreakSpawnSpec& Spec = OutSpecs.AddDefaulted_GetRef();
        Spec.Kind = Kind;
        Spec.Resource = Resource;
        Spec.DisplayLabel = !Node.DisplayName.IsEmpty() ? Node.DisplayName : Resource;
        // The plan keeps the full root-relative product. Only after the shared
        // shear preflight may Break decompose the final actor-world matrix.
        Spec.WorldTransform = FTransform(Node.WorldMatrix * PlacementWorld);
        Spec.Seed = Seed;
        Spec.AppearanceSeed = AppearanceSeed;
        Spec.FolderPath = FolderPath;
        if (Kind == EMHRandomSemanticKind::Mesh)
        {
            const int32 NodeIndex = static_cast<int32>(&Node - Plan.Nodes.GetData());
            const FMHResolvedCompositeLeaf* const* FoundLeaf = MeshLeaves.Find(NodeIndex);
            if (FoundLeaf == nullptr || (*FoundLeaf)->Resource != Resource ||
                !MHIsAdmissibleAppearanceCustomDataBaseIndex(Settings.AppearanceCustomDataBaseIndex))
            {
                OutError = FString::Printf(
                    TEXT("MH_E_INVALID_RESOURCE_SOURCE: mesh:%s at %s has no matching appearance leaf or valid custom-data window for Break"),
                    *Resource, *Node.NodePath);
                return false;
            }
            const FMHResolvedCompositeLeaf& Leaf = **FoundLeaf;
            Spec.WorldTransform = FTransform(Leaf.WorldMatrix * PlacementWorld);
            Spec.AppearanceBaseIndex = Settings.AppearanceCustomDataBaseIndex;
            for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
                Spec.AppearanceChannels[Channel] = Leaf.AppearanceChannels[Channel];
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::StaticMesh;
            Key.LogicalName = Resource;
            Spec.Mesh = Cast<UStaticMesh>(UMHEndpointPrototypeRegistry::ResolveEndpoint(Key, OutError));
            if (!OutError.IsEmpty()) return false;
            if (Spec.Mesh == nullptr)
            {
                OutError = FString::Printf(
                    TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: static_mesh:%s at %s is unavailable for Break"),
                    *Resource, *Node.NodePath);
                return false;
            }
        }
        else if (Kind == EMHRandomSemanticKind::Actor)
        {
            const FSoftClassPath* Path = Settings.ActorClassRegistry.Find(Resource);
            Spec.ActorClass = Path != nullptr ? Path->TryLoadClass<AActor>() : nullptr;
            if (!MHIsSpawnableCompositeActorClass(Spec.ActorClass))
            {
                OutError = FString::Printf(
                    TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: actor:%s at %s is unavailable for Break"),
                    *Resource, *Node.NodePath);
                return false;
            }
        }
        else
        {
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::Composite;
            Key.LogicalName = Resource;
            Spec.CompositeAsset = Cast<UMHCompositeAsset>(UMHEndpointPrototypeRegistry::ResolveEndpoint(Key, OutError));
            if (!OutError.IsEmpty()) return false;
            if (Spec.CompositeAsset == nullptr)
            {
                OutError = FString::Printf(
                    TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: composite:%s at %s is unavailable for Break"),
                    *Resource, *Node.NodePath);
                return false;
            }
            if (Node.DisplayName.IsEmpty()) Spec.DisplayLabel = Spec.CompositeAsset->LogicalName;
        }
    }
    return true;
}

AActor* MHSpawnBreakSpec(
    const FMHBreakSpawnSpec& Spec,
    ULevel& Level,
    FString& OutError)
{
    if (GEditor == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: editor engine is unavailable");
        return nullptr;
    }

    AActor* Spawned = nullptr;
    if (Spec.Kind == EMHRandomSemanticKind::Mesh)
    {
        AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(GEditor->AddActor(
            &Level,
            AStaticMeshActor::StaticClass(),
            Spec.WorldTransform,
            true,
            RF_Transactional,
            false));
        if (MeshActor != nullptr && MeshActor->GetStaticMeshComponent() != nullptr)
        {
            UStaticMeshComponent* Component = MeshActor->GetStaticMeshComponent();
            Component->SetStaticMesh(Spec.Mesh);
            FMHResolvedCompositeLeaf Leaf;
            Leaf.Kind = EMHRandomSemanticKind::Mesh;
            for (int32 Channel = 0; Channel < MH_APPEARANCE_CHANNELS; ++Channel)
                Leaf.AppearanceChannels[Channel] = Spec.AppearanceChannels[Channel];
            // The shared transport writes transient render data. The new
            // standalone actor also needs defaults for save/load and re-register.
            Component->Modify();
            Component->SetDefaultCustomPrimitiveDataFloatArray(
                Spec.AppearanceBaseIndex, MakeArrayView(Spec.AppearanceChannels));
            MHApplyLeafAppearanceCustomData(Component, Leaf, Spec.AppearanceBaseIndex);
        }
        Spawned = MeshActor;
    }
    else if (Spec.Kind == EMHRandomSemanticKind::Actor)
    {
        Spawned = GEditor->AddActor(
            &Level,
            Spec.ActorClass,
            Spec.WorldTransform,
            true,
            RF_Transactional,
            false);
    }
    else if (Spec.Kind == EMHRandomSemanticKind::Composite)
    {
        AMHCompositeActor* CompositeActor = Cast<AMHCompositeActor>(GEditor->AddActor(
            &Level,
            AMHCompositeActor::StaticClass(),
            Spec.WorldTransform,
            true,
            RF_Transactional,
            false));
        if (CompositeActor != nullptr)
        {
            CompositeActor->SetAutoSeed(false);
            CompositeActor->SetSeed(Spec.Seed);
            CompositeActor->SetAutoAppearanceSeed(false);
            CompositeActor->SetAppearanceSeed(Spec.AppearanceSeed);
            // Setting the asset is the single build point, after both seeds.
            CompositeActor->SetCompositeAsset(Spec.CompositeAsset);
        }
        Spawned = CompositeActor;
    }
    if (Spawned == nullptr)
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: Break could not spawn %s:%s"),
            MHBreakLeafKindLabel(Spec.Kind),
            *Spec.Resource);
    }
    else
    {
        Spawned->SetActorLabel(Spec.DisplayLabel, false);
        Spawned->SetFolderPath(Spec.FolderPath);
    }
    return Spawned;
}

void MHDestroySpawnedActors(const TArray<AActor*>& Actors)
{
    for (AActor* Actor : Actors)
    {
        if (Actor != nullptr && Actor->GetWorld() != nullptr)
        {
            Actor->GetWorld()->EditorDestroyActor(Actor, true);
        }
    }
}

} // namespace
} // namespace UE::MimirComposite

using namespace UE::MimirComposite;

bool UE::MimirComposite::MHPreflightBuildComposite(
    const TArray<AActor*>& Actors,
    const UMHCompositeSettings& Settings,
    FMHCompositeDocument& OutDocument,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutDocument = FMHCompositeDocument();
    OutWarnings.Reset();
    OutError.Reset();
    TArray<FString> Reasons;
    const FBox Bounds = MHSelectionBounds(Actors);
    if (!Bounds.IsValid || Bounds.GetCenter().ContainsNaN()) Reasons.Add(TEXT("selection has no finite world AABB"));
    const FTransform Pivot(FQuat::Identity, Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector);
    const ULevel* TargetLevel = !Actors.IsEmpty() && Actors[0] != nullptr ? Actors[0]->GetLevel() : nullptr;
    const auto HasEditedProperties = [](const UObject& Object, const UObject* Defaults)
    {
        if (Defaults == nullptr || Object.GetClass() != Defaults->GetClass()) return true;
        for (TFieldIterator<FProperty> Property(Object.GetClass()); Property; ++Property)
        {
            // EditAnywhere, including inherited fields and fixed-size arrays.
            if (!Property->HasAnyPropertyFlags(CPF_Edit) ||
                Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance | CPF_DisableEditOnTemplate)) continue;
            for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
                if (!Property->Identical_InContainer(&Object, Defaults, Index)) return true;
        }
        return false;
    };
    for (AActor* Actor : Actors)
    {
        if (Actor == nullptr) { Reasons.Add(TEXT("<null>: selection contains a null actor")); continue; }
        if (Actor->GetLevel() != TargetLevel)
            Reasons.Add(FString::Printf(TEXT("%s: selection spans multiple levels"), *Actor->GetPathName()));
        FMHCompositeNode Node;
        FString Reason;
        if (!MHBuildNodeForActor(*Actor, Pivot, Settings, Node, Reason))
        {
            Reasons.Add(FString::Printf(TEXT("%s: %s"), *Actor->GetPathName(), *Reason));
        }
        else
        {
            if (const AMHCompositeActor* Composite = Cast<AMHCompositeActor>(Actor))
            {
                OutWarnings.Add(FString::Printf(
                    TEXT("%s: child composite seeds (Seed=%d, AppearanceSeed=%d) are not representable in the recipe; its random subtree re-rolls under the new parent"),
                    *Actor->GetPathName(), Composite->GetSeed(), Composite->GetAppearanceSeed()));
            }
            else if (const AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor))
            {
                const UStaticMeshComponent* Component = MeshActor->GetStaticMeshComponent();
                for (int32 Slot = 0; Slot < Component->OverrideMaterials.Num(); ++Slot)
                {
                    if (const UMaterialInterface* Material = Component->OverrideMaterials[Slot])
                        OutWarnings.Add(FString::Printf(
                            TEXT("%s: material override in slot %d (%s) is not representable in the recipe and is dropped"),
                            *Actor->GetPathName(), Slot, *Material->GetPathName()));
                }
                if (!Component->GetCustomPrimitiveData().Data.IsEmpty())
                    OutWarnings.Add(FString::Printf(
                        TEXT("%s: custom primitive data (%d floats) is not representable in the recipe and is dropped"),
                        *Actor->GetPathName(), Component->GetCustomPrimitiveData().Data.Num()));
            }
            else
            {
                const AActor* Defaults = Cast<AActor>(Actor->GetClass()->GetDefaultObject(false));
                const USceneComponent* Root = Actor->GetRootComponent();
                const UObject* RootDefaults = Defaults != nullptr ? Defaults->GetRootComponent() : nullptr;
                // Blueprint-created roots may live on an archetype rather than
                // on the actor CDO; compare that template without creating one.
                if (Root != nullptr && (RootDefaults == nullptr || RootDefaults->GetClass() != Root->GetClass()))
                    RootDefaults = Root->GetArchetype();
                if (HasEditedProperties(*Actor, Defaults) ||
                    (Root != nullptr && HasEditedProperties(*Root, RootDefaults)))
                    OutWarnings.Add(FString::Printf(
                        TEXT("%s: instance properties differing from class defaults are not representable in the recipe and are dropped"),
                        *Actor->GetPathName()));
            }
            OutDocument.Nodes.Add(MoveTemp(Node));
        }
    }
    if (!Reasons.IsEmpty())
    {
        OutError = FString::Printf(TEXT("MH_E_UNREPRESENTABLE_SCENE_OBJECT: %s"), *FString::Join(Reasons, TEXT("; ")));
        return false;
    }
    return true;
}

bool UMHCompositeLevelSubsystem::BuildComposite(
    const TArray<AActor*>& Actors,
    const FMHCompositeAdoptTarget& AdoptTarget,
    AMHCompositeActor*& OutActor,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutActor = nullptr;
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || GEditor == nullptr || Actors.IsEmpty())
    {
        OutError = TEXT("MH_E_UNREPRESENTABLE_SCENE_OBJECT: Build Composite requires selected level actors on the game thread");
        return false;
    }
    if (EditingActor.IsValid())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: finish or cancel the active composite edit before Build");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (Settings == nullptr || SourceRoot.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }

    ULevel* TargetLevel = Actors[0] != nullptr ? Actors[0]->GetLevel() : nullptr;
    FMHCompositeDocument Document;
    if (!MHPreflightBuildComposite(Actors, *Settings, Document, OutWarnings, OutError))
    {
        return false;
    }
    const FTransform Pivot(FQuat::Identity, MHSelectionBounds(Actors).GetCenter());

    FString SourcePath;
    FString SourceRelativePath;
    if (!MHValidateCompositeAdoptTarget(
            SourceRoot,
            AdoptTarget,
            SourcePath,
            SourceRelativePath,
            OutError))
    {
        return false;
    }
    // Use the same fresh source/receipt projection as import, before Build can
    // create a package or publish a source document. A valid old placement is
    // not proof that its transitive source dependencies are still available.
    FMHSourceAnalysisServices Services;
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot, Services, OutError))
    {
        return false;
    }
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = AdoptTarget.LogicalName;
    if (Services.Resolver->Resolve(Key).Status != EMHResolveStatus::Unresolved)
    {
        OutError = FString::Printf(
            TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME: composite:%s already exists in source_root"),
            *Key.LogicalName);
        return false;
    }
    if (!MHCheckGeneratedAssetClaims(Key, OutError))
    {
        return false;
    }

    const FString PackageName = FString(MHLevelGeneratedCompositeRoot) + TEXT("/") + Key.LogicalName;
    const FString ObjectPath = PackageName + TEXT(".") + Key.LogicalName;
    if (StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath) != nullptr ||
        FPackageName::DoesPackageExist(PackageName))
    {
        OutError = FString::Printf(
            TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: generated target already exists: %s"),
            *ObjectPath);
        return false;
    }

    // Build produces one direct mesh/composite/actor node per selected actor.
    // The existing index walks each resource's full source closure, including
    // zero-weight random options, profiles and mesh/material dependencies.
    for (const FMHCompositeNode& Node : Document.Nodes)
    {
        FMHResourceKey Dependency;
        if (Node.Kind == EMHCompositeNodeKind::Mesh)
        {
            Dependency.Kind = EMHResourceKind::StaticMesh;
        }
        else if (Node.Kind == EMHCompositeNodeKind::Composite)
        {
            Dependency.Kind = EMHResourceKind::Composite;
        }
        else
        {
            continue;
        }
        Dependency.LogicalName = Node.Resource;
        if (!MHCheckGeneratedAssetClaims(Dependency, OutError))
        {
            return false;
        }
        if (Services.Index->IsImportBlocked(Dependency, OutError))
        {
            return false;
        }
    }
    // Reuse the importer's seed-free admission for generated endpoints and
    // transforms as well; a late import rejection must not be the first check.
    if (!MHProbeCompositeBuildV5(Key.LogicalName, Document, *Services.Resolver, *Settings, OutError))
    {
        return false;
    }

    UPackage* Package = CreatePackage(*PackageName);
    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
        Package,
        FName(*Key.LogicalName),
        RF_Public | RF_Standalone | RF_Transactional);
    if (Asset == nullptr || !MHApplyCompositeV5(*Asset, Document, OutError))
    {
        return false;
    }
    FAssetRegistryModule::AssetCreated(Asset);
    FMHCompositeOperationResult Published = MHPublishCompositeV5(
        *Asset,
        SourceRoot,
        &AdoptTarget);
    OutWarnings.Append(Published.Warnings);
    if (!Published.Succeeded())
    {
        OutError = MoveTemp(Published.Error);
        ObjectTools::DeleteSingleObject(Asset, false);
        return false;
    }

    UMHSourceImporter* Importer = GEditor->GetEditorSubsystem<UMHSourceImporter>();
    UMHCompositeAsset* ImportedAsset = nullptr;
    TArray<FString> ImportWarnings;
    if (Importer == nullptr || !Importer->ImportCompositeFile(
            SourcePath,
            PackageName,
            ImportedAsset,
            ImportWarnings,
            OutError))
    {
        return false;
    }
    OutWarnings.Append(ImportWarnings);
    if (ImportedAsset != Asset)
    {
        OutError = TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: Build import returned a different composite UObject");
        return false;
    }

    const FScopedTransaction Transaction(INVTEXT("Build MH Composite"));
    AMHCompositeActor* CompositeActor = Cast<AMHCompositeActor>(GEditor->AddActor(
        TargetLevel,
        AMHCompositeActor::StaticClass(),
        Pivot,
        true,
        RF_Transactional,
        false));
    if (CompositeActor == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Build could not create AMHCompositeActor");
        return false;
    }
    CompositeActor->SetCompositeAsset(Asset);
    CompositeActor->SetActorLabel(Key.LogicalName);
    // Selection must release the source actors while they are still valid.
    // Deselecting after EditorDestroyActor asks the Level Editor to operate on
    // pending-kill actors and leaves stale hit proxies for nested composites.
    GEditor->SelectNone(false, true, false);
    for (AActor* Actor : Actors)
    {
        Actor->Modify();
        Actor->GetWorld()->EditorDestroyActor(Actor, true);
    }
    GEditor->SelectActor(CompositeActor, true, true, true);
    GEditor->RedrawAllViewports();
    OutActor = CompositeActor;
    return true;
}

bool UMHCompositeLevelSubsystem::BreakComposites(
    const TArray<AMHCompositeActor*>& Actors,
    TArray<AActor*>& OutActors,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutActors.Reset();
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || GEditor == nullptr || Actors.IsEmpty())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Break Composite requires selected AMHCompositeActor instances");
        return false;
    }
    if (EditingActor.IsValid() && Actors.Contains(EditingActor.Get()))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: finish or cancel the active composite edit before Break");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr)
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: settings are unavailable");
        return false;
    }

    struct FActorBreakPlan
    {
        TObjectPtr<AMHCompositeActor> Actor;
        TArray<FMHBreakSpawnSpec> Specs;
    };
    TArray<FActorBreakPlan> Plans;
    for (AMHCompositeActor* Actor : Actors)
    {
        if (!IsValid(Actor) || Actor->IsTemplate() || Actor->IsActorBeingDestroyed() || Actor->GetWorld() == nullptr ||
            Actor->GetLevel() == nullptr || Actor->IsPlacementEditMode() ||
            Plans.ContainsByPredicate([Actor](const FActorBreakPlan& Existing) { return Existing.Actor == Actor; }))
        {
            OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Break requires distinct live, sealed MH Composite placements");
            return false;
        }
        // Break is a preview-plane operation: the resident plan is the sole
        // layout authority. Proof, closure and source freshness are exit-point
        // concerns for save, snapshot and cook, not for one-layer decomposition.
        const FMHResolvedCompositePlan* ResolvedPlan = Actor->GetResolvedPlan();
        const FString PlacementError = Actor->GetLastPlacementError();
        if (ResolvedPlan == nullptr || !PlacementError.IsEmpty())
        {
            OutError = PlacementError.IsEmpty()
                ? FString::Printf(TEXT("MH_E_INVALID_RESOURCE_SOURCE: Break requires a current resolved plan for %s"), *Actor->GetPathName())
                : PlacementError + TEXT(" (Break: ") + Actor->GetPathName() + TEXT(")");
            return false;
        }
        const UMHCompositeAsset* CompositeAsset = Actor->GetCompositeAsset();
        const UMHCompiledRecipeRegistry* Recipes = UMHCompiledRecipeRegistry::Get();
        const FMHCompiledRecipe* Recipe = CompositeAsset != nullptr && Recipes != nullptr
            ? Recipes->Find(*CompositeAsset)
            : nullptr;
        if (Recipe == nullptr)
        {
            OutError = FString::Printf(
                TEXT("MH_E_INVALID_RESOURCE_SOURCE: Break requires a current compiled recipe for %s"),
                *Actor->GetPathName());
            return false;
        }
        FActorBreakPlan& Plan = Plans.AddDefaulted_GetRef();
        Plan.Actor = Actor;
        if (!MHCollectBreakSpecs(
                *ResolvedPlan,
                *Recipe,
                Actor->GetActorTransform(),
                *Settings,
                Actor->GetSeed(),
                Actor->GetAppearanceSeed(),
                Actor->GetFolderPath(),
                Plan.Specs,
                OutError))
        {
            return false;
        }
    }
    const FScopedTransaction Transaction(INVTEXT("Break MH Composite"));
    for (const FActorBreakPlan& Plan : Plans)
    {
        TArray<AActor*> SpawnedForActor;
        for (const FMHBreakSpawnSpec& Spec : Plan.Specs)
        {
            AActor* Spawned = MHSpawnBreakSpec(Spec, *Plan.Actor->GetLevel(), OutError);
            if (Spawned == nullptr)
            {
                MHDestroySpawnedActors(OutActors);
                MHDestroySpawnedActors(SpawnedForActor);
                OutActors.Reset();
                return false;
            }
            SpawnedForActor.Add(Spawned);
        }
        OutActors.Append(SpawnedForActor);
    }
    // Release selection while the original actors are still live, just as in
    // Build; the editor must not inspect their pending-kill hit proxies.
    GEditor->SelectNone(false, true, false);
    for (const FActorBreakPlan& Plan : Plans)
    {
        Plan.Actor->Modify();
        Plan.Actor->GetWorld()->EditorDestroyActor(Plan.Actor, true);
    }
    for (AActor* Actor : OutActors)
    {
        GEditor->SelectActor(Actor, true, false, true);
    }
    GEditor->NoteSelectionChange();
    return true;
}

bool UMHCompositeLevelSubsystem::BeginEditComposite(
    AMHCompositeActor* Actor,
    FString& OutError)
{
    OutError.Reset();
    if (EditingActor.IsValid())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: another composite edit session is already active");
        return false;
    }
    UMHCompositeAsset* Asset = Actor != nullptr ? Actor->GetCompositeAsset() : nullptr;
    if (Actor == nullptr || Asset == nullptr || !MHExtractCompositeV5(*Asset, EditingDocument, OutError))
    {
        if (OutError.IsEmpty())
        {
            OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: Edit requires a live managed composite");
        }
        return false;
    }
    const FMHResolvedCompositePlan* ResolvedPlan = Actor->GetResolvedPlan();
    if (ResolvedPlan == nullptr || ResolvedPlan->Seed != Actor->GetSeed() || !Actor->GetLastPlacementError().IsEmpty())
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: Edit requires a current resolved placement for %s: %s"),
            *Actor->GetPathName(), *Actor->GetLastPlacementError());
        return false;
    }
    const TArray<TObjectPtr<USceneComponent>>& TopLevel = Actor->GetTopLevelComponents();
    if (TopLevel.Num() != EditingDocument.Nodes.Num())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: placement view does not match top-level composite nodes");
        return false;
    }

    const FScopedTransaction Transaction(INVTEXT("Edit MH Composite"));
    Actor->Modify();
    Actor->SetPlacementEditMode(true);
    EditingActor = Actor;
    EditingTopLevelComponents.Reset();
    for (USceneComponent* Component : TopLevel)
    {
        EditingTopLevelComponents.Add(Component);
    }
    return true;
}

bool UMHCompositeLevelSubsystem::CommitEditComposite(
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    AMHCompositeActor* Actor = EditingActor.Get();
    UMHCompositeAsset* Asset = Actor != nullptr ? Actor->GetCompositeAsset() : nullptr;
    if (Actor == nullptr || Asset == nullptr || EditingTopLevelComponents.Num() != EditingDocument.Nodes.Num())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: no valid composite edit session is active");
        return false;
    }

    // Flush a handle edit even if Commit precedes the next editor tick. Basis
    // moves are already serviced synchronously by the root transform hook.
    Actor->Tick(0.0f);
    FMHCompositeDocument Edited;
    if (!Actor->GetEditedCompositeDocument(Edited))
    {
        OutError = Actor->GetLastPlacementError().IsEmpty()
            ? TEXT("MH_E_INVALID_RESOURCE_SOURCE: edited placement has no admitted resolved plan")
            : Actor->GetLastPlacementError();
        return false;
    }
    // Commit the already-admitted prospective document. Re-decomposing the
    // displayed world transforms here would add another numeric round trip
    // and could publish different bytes than the preview plan was hashed from.

    // Validate the complete edited document while the edit session is still
    // recoverable. Once Commit crosses the source-file boundary, UE Undo must
    // no longer be able to resurrect a pre-Commit component snapshot.
    TArray<uint8> CanonicalPreflight;
    if (!MHWriteCanonicalCompositeV5(Edited, CanonicalPreflight, OutError))
    {
        return false;
    }

    const FString PreviousSourceRelativePath = Asset->SourceRelativePath;
    Actor->SetPlacementEditMode(false);
    EditingActor.Reset();
    EditingTopLevelComponents.Reset();
    EditingDocument = FMHCompositeDocument();
    GEditor->ResetTransaction(INVTEXT("MH Composite source Commit cannot be undone"));

    if (!MHApplyCompositeV5(*Asset, Edited, OutError))
    {
        Actor->RebuildComposite();
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    FMHCompositeOperationResult Published;
#if WITH_DEV_AUTOMATION_TESTS
    if (CommitPublisherForTests)
    {
        if (CommitPublisherForTests(*Asset, Published.Error))
        {
            Published.Asset = Asset;
        }
    }
    else
#endif
    {
        Published = MHPublishCompositeV5(*Asset, SourceRoot, nullptr);
    }
    OutWarnings.Append(Published.Warnings);
    if (!Published.Succeeded())
    {
        const FString PublishError = MoveTemp(Published.Error);
        FString ReconcileError;
        TArray<FString> ReconcileWarnings;
        UMHCompositeAsset* ReconciledAsset = nullptr;
        UMHSourceImporter* Importer = GEditor->GetEditorSubsystem<UMHSourceImporter>();
        FString SourcePath = FPaths::ConvertRelativePathToFull(
            SourceRoot,
            PreviousSourceRelativePath);
        FPaths::NormalizeFilename(SourcePath);
        const bool bReconciled =
            Importer != nullptr &&
            !PreviousSourceRelativePath.IsEmpty() &&
            Importer->ImportCompositeFile(
                SourcePath,
                Asset->GetOutermost()->GetName(),
                ReconciledAsset,
                ReconcileWarnings,
                ReconcileError) &&
            ReconciledAsset == Asset;
        OutWarnings.Append(ReconcileWarnings);
        OutError = bReconciled
            ? PublishError
            : FString::Printf(
                TEXT("%s; managed asset reconciliation from authoritative source failed: %s"),
                *PublishError,
                ReconcileError.IsEmpty() ? TEXT("source import was unavailable") : *ReconcileError);
        Actor->RebuildComposite();
        return false;
    }
    Actor->RebuildComposite();
    return true;
}

FString UMHCompositeLevelSubsystem::GetEditingCompositeLogicalName() const
{
    const AMHCompositeActor* Actor = EditingActor.Get();
    const UMHCompositeAsset* Asset = Actor != nullptr ? Actor->GetCompositeAsset() : nullptr;
    return Asset != nullptr ? Asset->LogicalName : FString();
}

FString UMHCompositeLevelSubsystem::GetEditingCompositeSourceRelativePath() const
{
    const AMHCompositeActor* Actor = EditingActor.Get();
    const UMHCompositeAsset* Asset = Actor != nullptr ? Actor->GetCompositeAsset() : nullptr;
    return Asset != nullptr ? Asset->SourceRelativePath : FString();
}

bool UMHCompositeLevelSubsystem::CancelEditComposite(FString& OutError)
{
    OutError.Reset();
    AMHCompositeActor* Actor = EditingActor.Get();
    if (Actor == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: no composite edit session is active");
        return false;
    }
    const FScopedTransaction Transaction(INVTEXT("Cancel MH Composite Edit"));
    Actor->Modify();
    Actor->SetPlacementEditMode(false);
    EditingActor.Reset();
    EditingTopLevelComponents.Reset();
    EditingDocument = FMHCompositeDocument();
    Actor->RebuildComposite();
    return true;
}

bool UMHCompositeLevelSubsystem::RebuildComposites(
    const TArray<AMHCompositeActor*>& Actors,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (Actors.IsEmpty())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Rebuild requires at least one AMHCompositeActor");
        return false;
    }
    if (EditingActor.IsValid() && Actors.Contains(EditingActor.Get()))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: finish or cancel the active composite edit before Refresh");
        return false;
    }
    const FScopedTransaction Transaction(INVTEXT("Rebuild MH Composites"));
    for (AMHCompositeActor* Actor : Actors)
    {
        if (Actor == nullptr)
        {
            continue;
        }
        Actor->Modify();
        Actor->RebuildComposite();
        OutWarnings.Append(Actor->GetLastPlacementWarnings());
    }
    return true;
}

bool UMHCompositeLevelSubsystem::RebuildAllInstances(
    UMHCompositeAsset* Asset,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (Asset == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Rebuild All requires a composite asset");
        return false;
    }
    TArray<AMHCompositeActor*> Actors;
    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        AMHCompositeActor* Actor = *It;
        if (!Actor->IsTemplate() && Actor->GetWorld() != nullptr && Actor->GetCompositeAsset() == Asset)
        {
            Actors.Add(Actor);
        }
    }
    if (Actors.IsEmpty())
    {
        return true;
    }
    return RebuildComposites(Actors, OutWarnings, OutError);
}

bool UMHCompositeLevelSubsystem::DeleteCompositeResource(
    UMHCompositeAsset* Asset,
    const bool bBreakLoadedInstances,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (Asset == nullptr || !MHIsCanonicalCompositeToken(Asset->LogicalName) || Asset->SourceRelativePath.IsEmpty())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Delete resource requires a managed composite receipt");
        return false;
    }
    if (EditingActor.IsValid() && EditingActor->GetCompositeAsset() == Asset)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: finish or cancel the active composite edit before Delete resource");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (SourceRoot.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }
    FString AbsoluteRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FString SourcePath = FPaths::ConvertRelativePathToFull(SourceRoot, Asset->SourceRelativePath);
    FPaths::NormalizeDirectoryName(AbsoluteRoot);
    FPaths::NormalizeFilename(SourcePath);
    if (!FPaths::IsUnderDirectory(SourcePath, AbsoluteRoot))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: receipt path escapes source_root: %s"),
            *SourcePath);
        return false;
    }
    FMHPayloadScanResolver Resolver(SourceRoot);
    if (!Resolver.Initialize(OutError))
    {
        return false;
    }
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = Asset->LogicalName;
    const FMHResolveOutcome Outcome = Resolver.Resolve(Key);
    if (Outcome.Status != EMHResolveStatus::Resolved || !FPaths::IsSamePath(Outcome.PayloadPath, SourcePath))
    {
        OutError = Outcome.Diagnostic.IsEmpty()
            ? FString::Printf(TEXT("MH_E_INVALID_RESOURCE_SOURCE: composite:%s is not the unique receipt source"), *Key.LogicalName)
            : Outcome.Diagnostic;
        return false;
    }

    TArray<AMHCompositeActor*> LoadedInstances;
    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        AMHCompositeActor* Actor = *It;
        if (!Actor->IsTemplate() && Actor->GetWorld() != nullptr && Actor->GetCompositeAsset() == Asset)
        {
            LoadedInstances.Add(Actor);
        }
    }
    if (bBreakLoadedInstances && !LoadedInstances.IsEmpty())
    {
        TArray<AActor*> BrokenActors;
        if (!BreakComposites(LoadedInstances, BrokenActors, OutWarnings, OutError))
        {
            return false;
        }
    }
    if (!IFileManager::Get().Delete(*SourcePath, false, true, true))
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: cannot delete composite source: %s"),
            *SourcePath);
        return false;
    }

    const FString PackageName = Asset->GetOutermost()->GetName();
    if (!ObjectTools::DeleteSingleObject(Asset, false))
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: source was deleted but generated asset could not be deleted: %s"),
            *PackageName);
        return false;
    }
    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        PackageName,
        FPackageName::GetAssetPackageExtension());
    IFileManager::Get().Delete(*PackageFilename, false, true, true);

    FMHSourceAnalysisServices Services;
    FString ScanError;
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot, Services, ScanError))
    {
        OutWarnings.Add(ScanError);
    }
    MHNotifyGeneratedResourceChanged(Key);
    return true;
}
