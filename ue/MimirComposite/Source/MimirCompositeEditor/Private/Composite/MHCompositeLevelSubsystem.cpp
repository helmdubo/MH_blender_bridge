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
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Misc/FileHelper.h"
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
    /** R4-pre-3: the child composite's place inside the parent. */
    FMHCompositeCallContext CallContext;
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
            // The child keeps the streams it had here: its recipe root is walked
            // under the path that referenced it, and its leaves keep the parent's
            // appearance boundary unless the subtree declares its own (in which
            // case the first leaf under the path already carries it).
            const FString OptionPath = Node.SemanticKind == EMHRandomSemanticKind::Random
                ? FString::Printf(TEXT("%s/options[%d]"), *Node.NodePath, Node.SelectedOptionIndex)
                : Node.NodePath;
            Spec.CallContext.StreamNamespace = OptionPath + TEXT(">") + Resource;
            for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
            {
                if (Leaf.Origin.StartsWith(Spec.CallContext.StreamNamespace))
                {
                    Spec.CallContext.AppearanceBoundary = Leaf.AppearanceBoundaryPath;
                    break;
                }
            }
            if (Spec.CallContext.AppearanceBoundary.IsEmpty()) Spec.CallContext.AppearanceBoundary = Plan.Nodes.IsValidIndex(0) ? Plan.Nodes[0].NodePath.Left(Plan.Nodes[0].NodePath.Find(TEXT(":"))) : FString();
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
            // Context before the asset: setting the asset is the single build point.
            CompositeActor->SetCallContext(Spec.CallContext);
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

    UMHCompositeAsset* Asset = nullptr;
    if (!CreateManagedComposite(Document, AdoptTarget, Asset, OutWarnings, OutError)) return false;

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
    CompositeActor->SetActorLabel(AdoptTarget.LogicalName);
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

bool UMHCompositeLevelSubsystem::CreateManagedComposite(
    const FMHCompositeDocument& Document,
    const FMHCompositeAdoptTarget& Target,
    UMHCompositeAsset*& OutAsset,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    // One managed composite from a document: validated against the fresh
    // source projection, published to Source Root under the adopt target and
    // imported back. Shared by Build and Save Unique (R6-U).
    OutAsset = nullptr;
#if WITH_DEV_AUTOMATION_TESTS
    if (DefinitionCreatorForTests)
    {
        OutAsset = DefinitionCreatorForTests(Document, Target, OutError);
        if (OutAsset == nullptr && OutError.IsEmpty()) OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the test definition creator returned no asset");
        return OutAsset != nullptr;
    }
#endif
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (Settings == nullptr || SourceRoot.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }
    FString SourcePath;
    FString SourceRelativePath;
    if (!MHValidateCompositeAdoptTarget(
            SourceRoot,
            Target,
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
    Key.LogicalName = Target.LogicalName;
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
        &Target);
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
    OutAsset = Asset;
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
    const UMHCompositeSettings* EditSettings = GetDefault<UMHCompositeSettings>();
    if (EditSettings != nullptr && EditSettings->bCompositeEditModeV2)
    {
        // CE-3c: the CE backend for the root definition — the same session,
        // draft and projection as a nested one (the whole placement is the
        // occurrence); the placement never enters the legacy edit mode.
        ++EditSessionEpoch;
        EditingActor = Actor;
        EditingAsset = Asset;
        EditingInvocationPath.Reset();
        EditingParentWorld = Actor->GetActorTransform().ToMatrixWithScale();
        EditingTopLevelComponents.Reset();
        OpenEditSession(Actor, Asset, FString());
        FString ProjectionError;
        if (!EditSession->OpenProjection(ProjectionError))
        {
            OutError = ProjectionError;
            ResetEditSession();
            return false;
        }
        UMHCompositeEditorMode::ActivateForSession();
        return true;
    }

    const FScopedTransaction Transaction(INVTEXT("Edit MH Composite"));
    Actor->Modify();
    Actor->SetPlacementEditMode(true);
    ++EditSessionEpoch;
    EditingActor = Actor;
    EditingAsset = Asset;
    EditingInvocationPath.Reset();
    EditingParentWorld = Actor->GetActorTransform().ToMatrixWithScale();
    EditingTopLevelComponents.Reset();
    for (USceneComponent* Component : TopLevel)
    {
        EditingTopLevelComponents.Add(Component);
    }
    OpenEditSession(Actor, Asset, FString());
    return true;
}

void UMHCompositeLevelSubsystem::OpenEditSession(AMHCompositeActor* Root, UMHCompositeAsset* Asset, const FString& InvocationNodePath)
{
    // CE-1: one owner of the session state; the subsystem keeps the strong reference.
    EditSession = NewObject<UMHCompositeEditSession>(this);
    EditSession->Open(Root, Asset, InvocationNodePath, EditingDocument, EditSessionEpoch);
}

bool UMHCompositeLevelSubsystem::CommitEditComposite(
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    AMHCompositeActor* Actor = EditingActor.Get();
    UMHCompositeAsset* Asset = Actor != nullptr ? Actor->GetCompositeAsset() : nullptr;
    if (Actor != nullptr && !EditingInvocationPath.IsEmpty()) return CommitNestedEditComposite(OutWarnings, OutError);
    // CE-3c: a root session under the CE backend commits the session draft;
    // the legacy path reads the placement's handles.
    const bool bSessionEdit = Actor != nullptr && !Actor->IsPlacementEditMode() && EditSession != nullptr && EditSession->IsOpen() && EditSession->GetDraft() != nullptr;
    if (Actor == nullptr || Asset == nullptr || (!bSessionEdit && EditingTopLevelComponents.Num() != EditingDocument.Nodes.Num()))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: no valid composite edit session is active");
        return false;
    }

    FMHCompositeDocument Edited;
    if (bSessionEdit)
    {
        if (!EditSession->GetDraft()->Extract(Edited, OutError)) return false;
    }
    else
    {
        // Flush a handle edit even if Commit precedes the next editor tick. Basis
        // moves are already serviced synchronously by the root transform hook.
        Actor->Tick(0.0f);
        if (!Actor->GetEditedCompositeDocument(Edited))
        {
            OutError = Actor->GetLastPlacementError().IsEmpty()
                ? TEXT("MH_E_INVALID_RESOURCE_SOURCE: edited placement has no admitted resolved plan")
                : Actor->GetLastPlacementError();
            return false;
        }
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

    // CE-5a: a CE-backend root session publishes without closing first.
    if (bSessionEdit) return PublishFromSession(*Asset, Edited, CanonicalPreflight, OutWarnings, OutError);

    const FString PreviousSourceRelativePath = Asset->SourceRelativePath;
    Actor->SetPlacementEditMode(false);
    ResetEditSession();
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

bool UMHCompositeLevelSubsystem::CommitNestedEditComposite(TArray<FString>& OutWarnings, FString& OutError)
{
    // R6-D2 (docs/16 §2.7): Apply Shared Definition. The nested draft becomes
    // the child definition's source; every placement invoking it follows via
    // the resource-changed notification. Decision (a), 2026-09-06: no revision
    // guard against Blender — the file is overwritten as-is, and a later
    // Blender export may overwrite it in turn.
    AMHCompositeActor* Root = EditingActor.Get();
    UMHCompositeAsset* Child = EditingAsset.Get();
    const bool bLegacyEdit = Root != nullptr && Root->IsPlacementEditMode();
    const bool bSessionEdit = EditSession != nullptr && EditSession->IsOpen() && EditSession->GetDraft() != nullptr;
    if (Root == nullptr || Child == nullptr || (!bLegacyEdit && !bSessionEdit) ||
        (bLegacyEdit && Root->GetEditScopeInvocationPath() != EditingInvocationPath))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: no valid nested composite edit session is active");
        return false;
    }
    // Flush a handle edit even if the publish precedes the next editor tick.
    Root->Tick(0.0f);
    FMHCompositeDocument Edited;
    if (bLegacyEdit ? !Root->GetEditedCompositeDocument(Edited) : !EditSession->GetDraft()->Extract(Edited, OutError))
    {
        OutError = Root->GetLastPlacementError().IsEmpty()
            ? TEXT("MH_E_INVALID_RESOURCE_SOURCE: edited nested definition has no admitted resolved plan")
            : Root->GetLastPlacementError();
        return false;
    }
    TArray<uint8> CanonicalPreflight;
    if (!MHWriteCanonicalCompositeV5(Edited, CanonicalPreflight, OutError)) return false;
    FMHCompositeDocument Previous;
    if (!MHExtractCompositeV5(*Child, Previous, OutError)) return false;

    // CE-5a: a CE-backend session publishes without closing first (a
    // failure keeps the draft); the legacy path — where the session is only
    // the facade over the placement's handles — crosses the boundary here.
    if (bSessionEdit && !bLegacyEdit) return PublishFromSession(*Child, Edited, CanonicalPreflight, OutWarnings, OutError);

    // Source boundary, as for a root Commit: once the file is written, UE
    // Undo must not resurrect a pre-publish snapshot.
    Root->SetPlacementEditMode(false);
    ResetEditSession();
    GEditor->ResetTransaction(INVTEXT("MH Composite shared definition publish cannot be undone"));

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    const bool bPublished = PublishDefinition(*Child, Edited, SourceRoot, OutWarnings, OutError);
    Root->RebuildComposite();
    return bPublished;
}

bool UMHCompositeLevelSubsystem::PublishFromSession(UMHCompositeAsset& Asset, const FMHCompositeDocument& Edited, const TArray<uint8>& CanonicalBytes, TArray<FString>& OutWarnings, FString& OutError)
{
    // CE-5a (spec §10.2, A25/A26): the session outlives a failed publish. The
    // source boundary is crossed only on success; the outcome of a failure
    // is read from the file itself, not from a UI flag.
    AMHCompositeActor* Root = EditingActor.Get();
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    FString TargetPath;
    if (!Asset.SourceRelativePath.IsEmpty())
    {
        TargetPath = FPaths::ConvertRelativePathToFull(SourceRoot, Asset.SourceRelativePath);
        FPaths::NormalizeFilename(TargetPath);
    }
    TArray<uint8> Before;
    if (!TargetPath.IsEmpty()) FFileHelper::LoadFileToArray(Before, *TargetPath);

    if (PublishDefinition(Asset, Edited, SourceRoot, OutWarnings, OutError))
    {
        LastPublishOutcome = EMHCompositePublishOutcome::Succeeded;
        // Source boundary: once the file is written, UE Undo must not
        // resurrect a pre-publish snapshot (the mode's Exit resets as well).
        ResetEditSession();
        if (GEditor != nullptr) GEditor->ResetTransaction(INVTEXT("MH Composite publish cannot be undone"));
        if (Root != nullptr) Root->RebuildComposite();
        return true;
    }

    TArray<uint8> After;
    if (!TargetPath.IsEmpty()) FFileHelper::LoadFileToArray(After, *TargetPath);
    const bool bSourceCommitted = !TargetPath.IsEmpty() && After == CanonicalBytes && After != Before;
    LastPublishOutcome = bSourceCommitted ? EMHCompositePublishOutcome::SourceCommitted : EMHCompositePublishOutcome::NoExternalChange;
    if (bSourceCommitted)
    {
        // The file holds the draft; Cancel can no longer promise the previous
        // source, so the draft is measured against what was committed. The
        // definition follows the file too: the restore may have fallen back
        // to the pre-publish document when the reconcile itself failed, and
        // the committed document is known exactly.
        TArray<uint8> DefinitionBytes;
        FMHCompositeDocument DefinitionDocument;
        FString RealignError;
        if (!MHExtractCompositeV5(Asset, DefinitionDocument, RealignError) || !MHWriteCanonicalCompositeV5(DefinitionDocument, DefinitionBytes, RealignError) || DefinitionBytes != CanonicalBytes)
        {
            if (MHApplyCompositeV5(Asset, Edited, RealignError)) MHNotifyCompositeAssetChanged(Asset);
            else OutWarnings.Add(TEXT("MH_W_SOURCE_COMMITTED: the definition could not be realigned to the committed file: ") + RealignError);
        }
        if (EditSession != nullptr) EditSession->RebaseOriginal(Edited);
        OutWarnings.Add(TEXT("MH_W_SOURCE_COMMITTED: the source file was written before the failure; the session stays open and its original is now the committed source — Cancel does not restore the previous file"));
    }
    else
    {
        OutWarnings.Add(TEXT("MH_W_NO_EXTERNAL_CHANGE: nothing was written; the session and its draft stay — fix the cause and Save again"));
    }
    // The restore notified consumers; the projection follows the rebuilt placement.
    if (EditSession != nullptr) EditSession->RefreshProjection();
    return false;
}

bool UMHCompositeLevelSubsystem::PublishDefinition(
    UMHCompositeAsset& Asset,
    const FMHCompositeDocument& Document,
    const FString& SourceRoot,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    // Apply + publish through the test seam; a failure restores the definition
    // (authoritative source or the pre-publish document) and notifies consumers.
    FMHCompositeDocument Previous;
    if (!MHExtractCompositeV5(Asset, Previous, OutError)) return false;
    const FString PreviousSourceRelativePath = Asset.SourceRelativePath;
    FMHCompositeOperationResult Published;
    if (MHApplyCompositeV5(Asset, Document, Published.Error))
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (CommitPublisherForTests)
        {
            if (CommitPublisherForTests(Asset, Published.Error)) Published.Asset = &Asset;
        }
        else
#endif
        {
            Published = MHPublishCompositeV5(Asset, SourceRoot, nullptr);
        }
    }
    OutWarnings.Append(Published.Warnings);
    if (Published.Succeeded()) return true;
    OutError = MoveTemp(Published.Error);
    RestoreDefinition(Asset, Previous, PreviousSourceRelativePath, SourceRoot, OutWarnings, OutError);
    return false;
}

void UMHCompositeLevelSubsystem::RestoreDefinition(
    UMHCompositeAsset& Asset,
    const FMHCompositeDocument& Previous,
    const FString& SourceRelativePath,
    const FString& SourceRoot,
    TArray<FString>& OutWarnings,
    FString& InOutError)
{
    FString SourcePath = FPaths::ConvertRelativePathToFull(SourceRoot, SourceRelativePath);
    FPaths::NormalizeFilename(SourcePath);
    UMHSourceImporter* Importer = GEditor != nullptr ? GEditor->GetEditorSubsystem<UMHSourceImporter>() : nullptr;
    if (Importer != nullptr && !SourceRelativePath.IsEmpty() && FPaths::FileExists(SourcePath))
    {
        FString ReconcileError;
        TArray<FString> ReconcileWarnings;
        UMHCompositeAsset* Reconciled = nullptr;
        const bool bReconciled = Importer->ImportCompositeFile(
            SourcePath, Asset.GetOutermost()->GetName(), Reconciled, ReconcileWarnings, ReconcileError) && Reconciled == &Asset;
        OutWarnings.Append(ReconcileWarnings);
        if (bReconciled)
        {
            MHNotifyCompositeAssetChanged(Asset);
            return;
        }
        InOutError += TEXT("; managed asset reconciliation from authoritative source failed: ") +
            (ReconcileError.IsEmpty() ? FString(TEXT("source import was unavailable")) : ReconcileError);
    }
    FString RestoreError;
    if (!MHApplyCompositeV5(Asset, Previous, RestoreError))
    {
        InOutError += TEXT("; the pre-publish definition could not be restored: ") + RestoreError;
        return;
    }
    MHNotifyCompositeAssetChanged(Asset);
}

namespace
{
/** One definition of an invocation chain and the slot (node/option selector) inside it that invokes the next one. */
struct FMHUniqueChainLink
{
    FString Definition;
    FString Selector;
};

/** "root:nodes[1]>child:nodes[0]/children[2]" -> [{root, nodes[1]}, {child, nodes[0]/children[2]}]. */
bool ParseInvocationChain(const FString& InvocationPath, TArray<FMHUniqueChainLink>& OutChain, FString& OutError)
{
    OutChain.Reset();
    TArray<FString> Segments;
    InvocationPath.ParseIntoArray(Segments, TEXT(">"), true);
    for (const FString& Segment : Segments)
    {
        FMHUniqueChainLink& Link = OutChain.AddDefaulted_GetRef();
        if (!Segment.Split(TEXT(":"), &Link.Definition, &Link.Selector) || Link.Definition.IsEmpty() || Link.Selector.IsEmpty())
        {
            OutError = FString::Printf(TEXT("MH_E_COMPOSITE_GRAMMAR: %s is not a nested composite invocation path"), *InvocationPath);
            return false;
        }
    }
    if (OutChain.IsEmpty()) OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: empty composite invocation path");
    return !OutChain.IsEmpty();
}

/** The Resource slot the selector names in Document ("nodes[1]", "nodes[1]/children[0]", ".../options[2]"), or nullptr. */
FString* FindReferenceSlot(FMHCompositeDocument& Document, const FString& Selector)
{
    TArray<FString> Parts;
    Selector.ParseIntoArray(Parts, TEXT("/"), true);
    TArray<FMHCompositeNode>* Nodes = &Document.Nodes;
    FMHCompositeNode* Node = nullptr;
    for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
    {
        const FString& Part = Parts[PartIndex];
        int32 Open = INDEX_NONE;
        if (!Part.FindChar(TEXT('['), Open) || !Part.EndsWith(TEXT("]"))) return nullptr;
        const FString Label = Part.Left(Open);
        const int32 Index = FCString::Atoi(*Part.Mid(Open + 1, Part.Len() - Open - 2));
        if (Label == TEXT("options"))
        {
            return Node != nullptr && PartIndex == Parts.Num() - 1 && Node->Options.IsValidIndex(Index) ? &Node->Options[Index].Resource : nullptr;
        }
        if ((Label != TEXT("nodes") && Label != TEXT("children")) || !Nodes->IsValidIndex(Index)) return nullptr;
        Node = &(*Nodes)[Index];
        Nodes = &Node->Children;
    }
    return Node != nullptr ? &Node->Resource : nullptr;
}

/**
 * R6-U2: the resolved subtree of the definition invoked at InvocationPath, as
 * concrete mesh/actor nodes relative to the invocation. Random draws, groups,
 * nested composites and placement draws are all already applied by the plan.
 */
bool BakeScopeDocument(
    const FMHResolvedCompositePlan& Plan,
    const FString& InvocationPath,
    const FString& Definition,
    FMHCompositeDocument& OutDocument,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutDocument = FMHCompositeDocument();
    const FMHResolvedCompositeNode* Invocation = Plan.Nodes.FindByPredicate(
        [&InvocationPath](const FMHResolvedCompositeNode& Node) { return Node.NodePath == InvocationPath; });
    if (Invocation == nullptr)
    {
        OutError = FString::Printf(TEXT("MH_E_COMPOSITE_GRAMMAR: %s is not a node of the resolved plan"), *InvocationPath);
        return false;
    }
    const FMatrix InvocationInverse = Invocation->WorldMatrix.Inverse();
    const FString Prefix = InvocationPath + TEXT(">") + Definition + TEXT(":");
    bool bBoundaryInside = false;
    for (const FMHResolvedCompositeLeaf& Leaf : Plan.Leaves)
    {
        if (!Leaf.Origin.StartsWith(Prefix)) continue;
        if (Leaf.Kind != EMHRandomSemanticKind::Mesh && Leaf.Kind != EMHRandomSemanticKind::Actor) continue;
        const FMatrix Local = Leaf.WorldMatrix * InvocationInverse;
        if (!MHIsRepresentableTransformMatrix(Local))
        {
            OutError = FString::Printf(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: baked leaf %s cannot round-trip through FTransform"), *Leaf.Origin);
            return false;
        }
        const FTransform LocalTransform(Local);
        FMHCompositeNode& Node = OutDocument.Nodes.AddDefaulted_GetRef();
        Node.Kind = Leaf.Kind == EMHRandomSemanticKind::Mesh ? EMHCompositeNodeKind::Mesh : EMHCompositeNodeKind::Actor;
        Node.Resource = Leaf.Resource;
        Node.Name = Leaf.DisplayName;
        Node.Transform.TranslationCm = LocalTransform.GetTranslation();
        Node.Transform.RotationQuat = LocalTransform.GetRotation();
        Node.Transform.Scale = LocalTransform.GetScale3D();
        bBoundaryInside |= Leaf.AppearanceBoundaryPath.StartsWith(Prefix);
    }
    if (OutDocument.Nodes.IsEmpty())
    {
        OutError = FString::Printf(TEXT("MH_E_COMPOSITE_GRAMMAR: nothing to bake under %s: no mesh or actor leaves"), *InvocationPath);
        return false;
    }
    // Appearance streams are keyed by boundary path: a boundary declared
    // inside the baked definition follows the copy's name, the root's does not.
    if (bBoundaryInside)
    {
        OutWarnings.Add(FString::Printf(
            TEXT("appearance boundaries declared inside composite:%s re-key under the baked copy's name (appearance streams are keyed by boundary path)"),
            *Definition));
    }
    return true;
}

bool NodesHaveRandomization(const TArray<FMHCompositeNode>& Nodes)
{
    for (const FMHCompositeNode& Node : Nodes)
    {
        if (Node.Kind == EMHCompositeNodeKind::Random || !Node.Options.IsEmpty() || Node.bHasInlinePlacement ||
            !Node.Profile.IsEmpty() || NodesHaveRandomization(Node.Children))
        {
            return true;
        }
    }
    return false;
}
} // namespace

bool UE::MimirComposite::MHCompositeDocumentHasRandomization(const FMHCompositeDocument& Document)
{
    return NodesHaveRandomization(Document.Nodes);
}

bool UMHCompositeLevelSubsystem::DescribeSaveUnique(const EMHCompositeUniqueScope Scope, const EMHCompositeUniqueVariant Variant, FMHCompositeSaveUniquePlan& OutPlan, FString& OutError) const
{

    // R6-U (docs/16 §2.7): innermost first — the edited definition, then (for
    // this placement) every definition of the invocation chain up to the root.
    OutPlan = FMHCompositeSaveUniquePlan();
    OutError.Reset();
    const AMHCompositeActor* Root = EditingActor.Get();
    const UMHCompositeAsset* Edited = EditingAsset.Get();
    if (Root == nullptr || Edited == nullptr || EditingInvocationPath.IsEmpty())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Save Unique needs an active Edit Contents session");
        return false;
    }
    TArray<FMHUniqueChainLink> Chain;
    if (!ParseInvocationChain(EditingInvocationPath, Chain, OutError)) return false;
    OutPlan.Copies.Add(Edited->LogicalName);
    if (Scope == EMHCompositeUniqueScope::ForThisPlacement)
    {
        for (int32 Index = Chain.Num() - 1; Index >= 0; --Index) OutPlan.Copies.Add(Chain[Index].Definition);
    }
    else
    {
        OutPlan.OverwrittenDefinition = Chain.Last().Definition;
    }
    // Streams are keyed by node path (16 §2.10): a copy under a new name
    // re-rolls the draws inside it. The root copy keeps its streams through
    // the placement's call context; the copies below it do not.
    for (int32 Index = 0; Index < OutPlan.Copies.Num(); ++Index)
    {
        const bool bRootCopy = Scope == EMHCompositeUniqueScope::ForThisPlacement && Index == OutPlan.Copies.Num() - 1;
        if (bRootCopy) continue;
        bool bRandom = false;
        if (Index == 0)
        {
            // A baked copy carries the resolved result: nothing draws in it.
            bRandom = Variant == EMHCompositeUniqueVariant::Procedural && MHCompositeDocumentHasRandomization(EditingDocument);
        }
        else
        {
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::Composite;
            Key.LogicalName = OutPlan.Copies[Index];
            FString ResolveError;
            const UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(UMHEndpointPrototypeRegistry::ResolveEndpoint(Key, ResolveError));
            FMHCompositeDocument Document;
            FString ExtractError;
            bRandom = Asset != nullptr && MHExtractCompositeV5(*Asset, Document, ExtractError) && MHCompositeDocumentHasRandomization(Document);
        }
        if (bRandom)
        {
            OutPlan.Warnings.Add(FString::Printf(
                TEXT("random draws inside composite:%s re-roll under the unique copy's name (streams are keyed by node path, 16 §2.10); bake the current result to keep them"),
                *OutPlan.Copies[Index]));
        }
    }
    return true;
}

bool UMHCompositeLevelSubsystem::SaveEditAsUnique(
    const EMHCompositeUniqueScope Scope,
    const EMHCompositeUniqueVariant Variant,
    const TArray<FMHCompositeAdoptTarget>& Targets,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    FMHCompositeSaveUniquePlan Plan;
    if (!DescribeSaveUnique(Scope, Variant, Plan, OutError)) return false;
    OutWarnings.Append(Plan.Warnings);
    AMHCompositeActor* Root = EditingActor.Get();
    UMHCompositeAsset* Edited = EditingAsset.Get();
    const bool bLegacyEdit = Root != nullptr && Root->IsPlacementEditMode();
    const bool bSessionEdit = EditSession != nullptr && EditSession->IsOpen() && EditSession->GetDraft() != nullptr;
    if (Root == nullptr || Edited == nullptr || (!bLegacyEdit && !bSessionEdit) ||
        (bLegacyEdit && Root->GetEditScopeInvocationPath() != EditingInvocationPath))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: no valid nested composite edit session is active");
        return false;
    }
    // Targets: one canonical, fresh name per copy — checked before any write.
    if (Targets.Num() != Plan.Copies.Num())
    {
        OutError = FString::Printf(TEXT("MH_E_COMPOSITE_GRAMMAR: Save Unique expects %d target name(s), got %d"), Plan.Copies.Num(), Targets.Num());
        return false;
    }
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        const FString& Name = Targets[Index].LogicalName;
        if (!MHIsCanonicalCompositeToken(Name))
        {
            OutError = FString::Printf(TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: %s is not a canonical composite name"), *Name);
            return false;
        }
        bool bTaken = Plan.Copies.Contains(Name) || Plan.OverwrittenDefinition == Name;
        for (int32 Other = 0; Other < Index && !bTaken; ++Other) bTaken = Targets[Other].LogicalName == Name;
        if (bTaken)
        {
            OutError = FString::Printf(TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME: composite:%s is already a definition of this chain or another target"), *Name);
            return false;
        }
    }
    TArray<FMHUniqueChainLink> Chain;
    if (!ParseInvocationChain(EditingInvocationPath, Chain, OutError)) return false;
    // Chain definitions and their slots, innermost first, resolved before the boundary.
    struct FChainDocument
    {
        UMHCompositeAsset* Asset = nullptr;
        FMHCompositeDocument Document;
        FString Selector;
    };
    const int32 RewiredLinks = Scope == EMHCompositeUniqueScope::InParentDefinition ? 1 : Chain.Num();
    TArray<FChainDocument> ChainDocuments;
    ChainDocuments.Reserve(RewiredLinks);
    for (int32 Step = 0; Step < RewiredLinks; ++Step)
    {
        const FMHUniqueChainLink& Link = Chain[Chain.Num() - 1 - Step];
        FMHResourceKey Key;
        Key.Kind = EMHResourceKind::Composite;
        Key.LogicalName = Link.Definition;
        FString ResolveError;
        UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(UMHEndpointPrototypeRegistry::ResolveEndpoint(Key, ResolveError));
        if (Asset == nullptr)
        {
            OutError = ResolveError.IsEmpty()
                ? TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: composite:") + Link.Definition + TEXT(" has no managed asset")
                : ResolveError;
            return false;
        }
        FChainDocument& Entry = ChainDocuments.AddDefaulted_GetRef();
        Entry.Asset = Asset;
        Entry.Selector = Link.Selector;
        if (!MHExtractCompositeV5(*Asset, Entry.Document, OutError)) return false;
        const FString& Expected = Step == 0 ? Edited->LogicalName : Chain[Chain.Num() - Step].Definition;
        const FString* Slot = FindReferenceSlot(Entry.Document, Link.Selector);
        if (Slot == nullptr || *Slot != Expected)
        {
            OutError = FString::Printf(TEXT("MH_E_COMPOSITE_GRAMMAR: composite:%s does not invoke composite:%s at %s"), *Link.Definition, *Expected, *Link.Selector);
            return false;
        }
    }
    // The edited draft, flushed and admitted.
    Root->Tick(0.0f);
    FMHCompositeDocument EditedDocument;
    if (bLegacyEdit ? !Root->GetEditedCompositeDocument(EditedDocument) : !EditSession->GetDraft()->Extract(EditedDocument, OutError))
    {
        OutError = Root->GetLastPlacementError().IsEmpty()
            ? TEXT("MH_E_INVALID_RESOURCE_SOURCE: edited nested definition has no admitted resolved plan")
            : Root->GetLastPlacementError();
        return false;
    }
    if (Variant == EMHCompositeUniqueVariant::BakeCurrentResult)
    {
        // R6-U2: the copy is the resolved subtree of this placement's session plan.
        const FMHResolvedCompositePlan* ResolvedPlan = Root->GetResolvedPlan();
        if (ResolvedPlan == nullptr || !BakeScopeDocument(*ResolvedPlan, EditingInvocationPath, Edited->LogicalName, EditedDocument, OutWarnings, OutError))
        {
            if (OutError.IsEmpty()) OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the edited placement has no resolved plan to bake");
            return false;
        }
    }
    TArray<uint8> CanonicalPreflight;
    if (!MHWriteCanonicalCompositeV5(EditedDocument, CanonicalPreflight, OutError)) return false;
    const UMHCompositeAsset* OriginalRoot = Root->GetCompositeAsset();
    const FString OriginalRootName = OriginalRoot != nullptr ? OriginalRoot->LogicalName : FString();

    // Source boundary: new definitions are published and, for the parent
    // scope, the shared parent is overwritten; UE Undo cannot cross it.
    Root->SetPlacementEditMode(false);
    ResetEditSession();
    GEditor->ResetTransaction(INVTEXT("MH Composite Save Unique cannot be undone"));

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    TArray<UMHCompositeAsset*> Created;
    FMHCompositeDocument NextDocument = EditedDocument;
    for (int32 Index = 0; Index < Targets.Num(); ++Index)
    {
        UMHCompositeAsset* Copy = nullptr;
        if (CreateManagedComposite(NextDocument, Targets[Index], Copy, OutWarnings, OutError))
        {
            Created.Add(Copy);
            if (Index + 1 == Targets.Num()) break;
            // The next definition up the chain, invoking the copy just made.
            NextDocument = ChainDocuments[Index].Document;
            if (FString* Slot = FindReferenceSlot(NextDocument, ChainDocuments[Index].Selector))
            {
                *Slot = Copy->LogicalName;
                continue;
            }
            OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: the invocation slot vanished while copying composite:") + ChainDocuments[Index].Asset->LogicalName;
        }
        for (const UMHCompositeAsset* Orphan : Created)
        {
            OutWarnings.Add(FString::Printf(TEXT("composite:%s was created and is not invoked by anything yet"), *Orphan->LogicalName));
        }
        Root->RebuildComposite();
        return false;
    }
    if (Scope == EMHCompositeUniqueScope::InParentDefinition)
    {
        // The shared parent points at the copy; its publish refreshes every
        // placement invoking it (R6-D2 path).
        FMHCompositeDocument ParentDocument = ChainDocuments[0].Document;
        bool bPublished = false;
        if (FString* Slot = FindReferenceSlot(ParentDocument, ChainDocuments[0].Selector))
        {
            *Slot = Created[0]->LogicalName;
            bPublished = PublishDefinition(*ChainDocuments[0].Asset, ParentDocument, SourceRoot, OutWarnings, OutError);
        }
        else
        {
            OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: the invocation slot vanished while rewiring composite:") + ChainDocuments[0].Asset->LogicalName;
        }
        if (!bPublished) OutWarnings.Add(FString::Printf(TEXT("composite:%s was created and is not invoked by anything"), *Created[0]->LogicalName));
        Root->RebuildComposite();
        return bPublished;
    }
    // Only this placement moves to the unique root. Its root-level streams
    // stay keyed by the original root's path through the call context
    // (16 §2.10; the second explicit writer after Break).
    if (Root->GetCallContext().IsEmpty() && !OriginalRootName.IsEmpty())
    {
        FMHCompositeCallContext Context;
        Context.StreamNamespace = OriginalRootName;
        Context.AppearanceBoundary = OriginalRootName;
        Root->SetCallContext(Context);
    }
    Root->SetCompositeAsset(Created.Last());
    return true;
}

bool UMHCompositeLevelSubsystem::BeginEditNestedComposite(AMHCompositeActor* Root, const FString& InvocationNodePath, FString& OutError)
{
    OutError.Reset();
    if (EditingActor.IsValid())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: another composite edit session is already active");
        return false;
    }
    const FMHResolvedCompositePlan* Plan = Root != nullptr ? Root->GetResolvedPlan() : nullptr;
    if (Root == nullptr || Plan == nullptr || !Root->GetLastPlacementError().IsEmpty())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Edit Contents requires a current resolved placement");
        return false;
    }
    // The invocation is a node of the resident preview plan (16 §2.10); its
    // world matrix under the placement basis is the effective parent of
    // everything the child definition materializes here.
    const FMHResolvedCompositeNode* Invocation = Plan->Nodes.FindByPredicate(
        [&InvocationNodePath](const FMHResolvedCompositeNode& Node) { return Node.NodePath == InvocationNodePath; });
    if (Invocation == nullptr || Invocation->SemanticKind != EMHRandomSemanticKind::Composite)
    {
        OutError = FString::Printf(TEXT("MH_E_COMPOSITE_GRAMMAR: %s is not a nested composite invocation of this placement"), *InvocationNodePath);
        return false;
    }
    FMHResourceKey ChildKey;
    ChildKey.Kind = EMHResourceKind::Composite;
    ChildKey.LogicalName = Invocation->Resource;
    FString AdmissionError;
    UMHCompositeAsset* Child = Cast<UMHCompositeAsset>(UMHEndpointPrototypeRegistry::ResolveEndpoint(ChildKey, AdmissionError));
    if (Child == nullptr)
    {
        OutError = AdmissionError.IsEmpty()
            ? TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: ") + ChildKey.ToString() + TEXT(" has no managed asset")
            : AdmissionError;
        return false;
    }
    if (!MHExtractCompositeV5(*Child, EditingDocument, OutError))
    {
        EditingDocument = FMHCompositeDocument();
        return false;
    }
    // The root enters a Placement Edit session scoped to the invocation: the
    // child definition's nodes get handles under the invocation's effective
    // world (R6-D1); the source is untouched until R6-D2 publishes.
    ++EditSessionEpoch;
    EditingActor = Root;
    EditingAsset = Child;
    EditingInvocationPath = InvocationNodePath;
    EditingParentWorld = Invocation->WorldMatrix * Root->GetActorTransform().ToMatrixWithScale();
    EditingTopLevelComponents.Reset();
    const UMHCompositeSettings* EditSettings = GetDefault<UMHCompositeSettings>();
    if (EditSettings != nullptr && EditSettings->bCompositeEditModeV2)
    {
        // CE-2b: the CE backend — session draft + edit projection; the root
        // placement stays sealed and never enters the legacy edit mode.
        OpenEditSession(Root, Child, EditingInvocationPath);
        FString ProjectionError;
        if (!EditSession->OpenProjection(ProjectionError))
        {
            OutError = ProjectionError;
            ResetEditSession();
            return false;
        }
        // CE-3a: the editor mode follows the session (overlay, locked context, Escape).
        UMHCompositeEditorMode::ActivateForSession();
        return true;
    }
    {
        const FScopedTransaction Transaction(INVTEXT("Edit MH Composite Contents"));
        Root->Modify();
        Root->SetEditScope(InvocationNodePath);
        Root->SetPlacementEditMode(true);
    }
    // InvocationNodePath may alias a node of the plan SetPlacementEditMode just
    // replaced; the stored copy is the safe one from here on.
    OpenEditSession(Root, Child, EditingInvocationPath);
    return true;
}

const FMHCompositeDocument& UMHCompositeLevelSubsystem::GetEditingDraft() const
{
    // CE-1: the session's draft is the one current document; the field is
    // only its typed view (the legacy actor edits are mirrored in first).
    if (EditSession != nullptr && EditSession->IsOpen() && EditSession->GetDraft() != nullptr)
    {
        FString Error;
        EditSession->SyncDraftFromLegacyEdit(Error);
        EditSession->GetDraft()->Extract(EditingDocument, Error);
    }
    return EditingDocument;
}

FMHCompositeEditContext UMHCompositeLevelSubsystem::GetEditContext() const
{
    FMHCompositeEditContext Context;
    AMHCompositeActor* Root = EditingActor.Get();
    const UMHCompositeAsset* Asset = EditingAsset.Get();
    if (Root == nullptr || Asset == nullptr) return Context;
    Context.EditedLogicalName = Asset->LogicalName;
    Context.EditedSourceRelativePath = Asset->SourceRelativePath;
    Context.InvocationPath = EditingInvocationPath;
    Context.EffectiveParentWorld = EditingParentWorld;
    Context.RootPlacement = Root;
    Context.SaveScope = EMHCompositeEditSaveScope::SharedDefinition;
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = Asset->LogicalName;
    for (TObjectIterator<AMHCompositeActor> It; It; ++It)
    {
        const AMHCompositeActor* Placement = *It;
        const UWorld* World = IsValid(Placement) ? Placement->GetWorld() : nullptr;
        if (!IsValid(Placement) || Placement->IsTemplate() || Placement->IsActorBeingDestroyed() ||
            World == nullptr || World->IsBeingCleanedUp() || World->IsCleanedUp() || !Placement->DependsOnResource(Key)) continue;
        ++Context.ConsumerPlacements;
    }
    return Context;
}

void UMHCompositeLevelSubsystem::ResetEditSession()
{
    // The session is over: whatever was captured for it is stale from here on.
    ++EditSessionEpoch;
    if (EditSession != nullptr)
    {
        EditSession->Close();
        EditSession = nullptr;
    }
    UMHCompositeEditorMode::DeactivateForSession();
    EditingActor.Reset();
    EditingAsset.Reset();
    EditingInvocationPath.Reset();
    EditingParentWorld = FMatrix::Identity;
    EditingTopLevelComponents.Reset();
    EditingDocument = FMHCompositeDocument();
}

FString UMHCompositeLevelSubsystem::GetEditingCompositeLogicalName() const
{
    const UMHCompositeAsset* Asset = EditingActor.IsValid() ? EditingAsset.Get() : nullptr;
    return Asset != nullptr ? Asset->LogicalName : FString();
}

FString UMHCompositeLevelSubsystem::GetEditingCompositeSourceRelativePath() const
{
    const UMHCompositeAsset* Asset = EditingActor.IsValid() ? EditingAsset.Get() : nullptr;
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
    // CE-4a: under the CE backend nothing of the placement is in the record
    // and the mode's Exit resets the undo history (BPP policy) — no transaction.
    if (!Actor->IsPlacementEditMode())
    {
        ResetEditSession();
        return true;
    }
    const FScopedTransaction Transaction(INVTEXT("Cancel MH Composite Edit"));
    Actor->Modify();
    // A root session extracted handles into the placement: rebuild restores
    // the preview. A nested draft (R6-D0) never touched the root's view.
    const bool bRootSession = Actor->IsPlacementEditMode();
    Actor->SetPlacementEditMode(false);
    ResetEditSession();
    if (bRootSession) Actor->RebuildComposite();
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
