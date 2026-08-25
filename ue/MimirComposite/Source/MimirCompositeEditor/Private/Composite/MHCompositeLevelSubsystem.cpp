#include "Composite/MHCompositeLevelSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadScanResolver.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceImporter.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeLevelSubsystem)

namespace UE::MimirComposite
{
namespace
{

constexpr const TCHAR* MHLevelGeneratedCompositeRoot = TEXT("/Game/MH/Generated/Composites");
constexpr const TCHAR* MHLevelGeneratedMeshRoot = TEXT("/Game/MH/Generated/Meshes");

void MHDirtyCompositeThumbnail(UMHCompositeAsset& Asset)
{
    if (FObjectThumbnail* Thumbnail = ThumbnailTools::GetThumbnailForObject(&Asset))
    {
        Thumbnail->MarkAsDirty();
    }
    if (UThumbnailManager* ThumbnailManager = UThumbnailManager::TryGet())
    {
        ThumbnailManager->GetOnThumbnailDirtied().Broadcast(FSoftObjectPath(&Asset));
    }
}

struct FMHBreakSpawnSpec
{
    EMHCompositeNodeKind Kind = EMHCompositeNodeKind::Group;
    FString Resource;
    FString DisplayLabel;
    FTransform WorldTransform = FTransform::Identity;
    TObjectPtr<UStaticMesh> Mesh;
    TObjectPtr<UMHCompositeAsset> Composite;
    TObjectPtr<UClass> ActorClass;
};

const TCHAR* MHLevelNodeKindLabel(const EMHCompositeNodeKind Kind)
{
    switch (Kind)
    {
    case EMHCompositeNodeKind::Mesh:
        return TEXT("mesh");
    case EMHCompositeNodeKind::Actor:
        return TEXT("actor");
    case EMHCompositeNodeKind::Composite:
        return TEXT("composite");
    case EMHCompositeNodeKind::Group:
        return TEXT("group");
    default:
        return TEXT("unknown");
    }
}

bool MHSaveCompositeLevelAsset(UMHCompositeAsset& Asset, FString& OutError)
{
    UPackage* Package = Asset.GetOutermost();
    if (Package == nullptr || !FPackageName::IsValidLongPackageName(Package->GetName()))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: composite asset package is not persistent");
        return false;
    }
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const FString Filename = FPackageName::LongPackageNameToFilename(
        Package->GetName(),
        FPackageName::GetAssetPackageExtension());
    Package->MarkPackageDirty();
    if (!UPackage::SavePackage(Package, &Asset, *Filename, Args))
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: cannot save composite asset '%s'"),
            *Asset.GetPathName());
        return false;
    }
    return true;
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
    OutNode.Transform.TranslationCm = Actor.GetActorTransform().GetRelativeTransform(Pivot).GetTranslation();
    OutNode.Transform.RotationQuat = Actor.GetActorTransform().GetRelativeTransform(Pivot).GetRotation();
    OutNode.Transform.Scale = Actor.GetActorTransform().GetRelativeTransform(Pivot).GetScale3D();
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
    const TArray<FMHCompositeNode>& Nodes,
    const FTransform& DocumentBasis,
    const UMHCompositeSettings& Settings,
    TArray<FMHBreakSpawnSpec>& OutSpecs,
    FString& OutError)
{
    for (const FMHCompositeNode& Node : Nodes)
    {
        const FTransform WorldTransform(
            Node.Transform.RotationQuat,
            Node.Transform.TranslationCm,
            Node.Transform.Scale);
        const FTransform PlacedWorld = WorldTransform * DocumentBasis;
        if (Node.Kind != EMHCompositeNodeKind::Group)
        {
            FMHBreakSpawnSpec& Spec = OutSpecs.AddDefaulted_GetRef();
            Spec.Kind = Node.Kind;
            Spec.Resource = Node.Resource;
            // Authored names are presentation identity for every placement
            // kind. Older source documents may omit one, in which case the
            // stable resource token remains the deterministic fallback.
            Spec.DisplayLabel = !Node.Name.IsEmpty() ? Node.Name : Node.Resource;
            Spec.WorldTransform = PlacedWorld;
            if (Node.Kind == EMHCompositeNodeKind::Mesh)
            {
                const FString ObjectPath = FString::Printf(
                    TEXT("%s/%s.%s"), MHLevelGeneratedMeshRoot, *Node.Resource, *Node.Resource);
                Spec.Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
                if (Spec.Mesh == nullptr)
                {
                    OutError = FString::Printf(
                        TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: static_mesh:%s is unavailable for Break"),
                        *Node.Resource);
                    return false;
                }
            }
            else if (Node.Kind == EMHCompositeNodeKind::Composite)
            {
                const FString ObjectPath = FString::Printf(
                    TEXT("%s/%s.%s"), MHLevelGeneratedCompositeRoot, *Node.Resource, *Node.Resource);
                Spec.Composite = LoadObject<UMHCompositeAsset>(nullptr, *ObjectPath);
                if (Spec.Composite == nullptr)
                {
                    OutError = FString::Printf(
                        TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: composite:%s is unavailable for Break"),
                        *Node.Resource);
                    return false;
                }
            }
            else if (Node.Kind == EMHCompositeNodeKind::Actor)
            {
                const FSoftClassPath* Path = Settings.ActorClassRegistry.Find(Node.Resource);
                Spec.ActorClass = Path != nullptr ? Path->TryLoadClass<AActor>() : nullptr;
                if (Spec.ActorClass == nullptr || !Spec.ActorClass->IsChildOf(AActor::StaticClass()))
                {
                    OutError = FString::Printf(
                        TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: actor:%s is unavailable for Break"),
                        *Node.Resource);
                    return false;
                }
            }
        }
        // Transforms in the v4 document are authored world transforms inside
        // the document basis; group attachment is structural and dissolves.
        if (!MHCollectBreakSpecs(Node.Children, DocumentBasis, Settings, OutSpecs, OutError))
        {
            return false;
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
    if (Spec.Kind == EMHCompositeNodeKind::Mesh)
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
            MeshActor->GetStaticMeshComponent()->SetStaticMesh(Spec.Mesh);
        }
        Spawned = MeshActor;
    }
    else if (Spec.Kind == EMHCompositeNodeKind::Composite)
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
            CompositeActor->SetCompositeAsset(Spec.Composite);
        }
        Spawned = CompositeActor;
    }
    else if (Spec.Kind == EMHCompositeNodeKind::Actor)
    {
        Spawned = GEditor->AddActor(
            &Level,
            Spec.ActorClass,
            Spec.WorldTransform,
            true,
            RF_Transactional,
            false);
    }
    if (Spawned == nullptr)
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: Break could not spawn %s:%s"),
            MHLevelNodeKindLabel(Spec.Kind),
            *Spec.Resource);
    }
    else
    {
        Spawned->SetActorLabel(Spec.DisplayLabel, false);
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
    TArray<FString> Reasons;
    for (AActor* Actor : Actors)
    {
        if (Actor == nullptr)
        {
            Reasons.Add(TEXT("<null>: selection contains a null actor"));
        }
        else if (Actor->GetLevel() != TargetLevel)
        {
            Reasons.Add(FString::Printf(TEXT("%s: selection spans multiple levels"), *Actor->GetPathName()));
        }
    }

    const FBox Bounds = MHSelectionBounds(Actors);
    if (!Bounds.IsValid || Bounds.GetCenter().ContainsNaN())
    {
        Reasons.Add(TEXT("selection has no finite world AABB"));
    }
    const FTransform Pivot(FQuat::Identity, Bounds.IsValid ? Bounds.GetCenter() : FVector::ZeroVector);
    FMHCompositeDocument Document;
    for (AActor* Actor : Actors)
    {
        if (Actor == nullptr)
        {
            continue;
        }
        FMHCompositeNode Node;
        FString Reason;
        if (!MHBuildNodeForActor(*Actor, Pivot, *Settings, Node, Reason))
        {
            Reasons.Add(FString::Printf(TEXT("%s: %s"), *Actor->GetPathName(), *Reason));
        }
        else
        {
            Document.Nodes.Add(MoveTemp(Node));
        }
    }
    if (!Reasons.IsEmpty())
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNREPRESENTABLE_SCENE_OBJECT: %s"),
            *FString::Join(Reasons, TEXT("; ")));
        return false;
    }

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
    FMHPayloadScanResolver SourceResolver(SourceRoot);
    if (!SourceResolver.Initialize(OutError))
    {
        return false;
    }
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::Composite;
    Key.LogicalName = AdoptTarget.LogicalName;
    if (SourceResolver.Resolve(Key).Status != EMHResolveStatus::Unresolved)
    {
        OutError = FString::Printf(
            TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME: composite:%s already exists in source_root"),
            *Key.LogicalName);
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

    UPackage* Package = CreatePackage(*PackageName);
    UMHCompositeAsset* Asset = NewObject<UMHCompositeAsset>(
        Package,
        FName(*Key.LogicalName),
        RF_Public | RF_Standalone | RF_Transactional);
    if (Asset == nullptr || !MHApplyCompositeV4(*Asset, Document, OutError))
    {
        return false;
    }
    FAssetRegistryModule::AssetCreated(Asset);
    FMHCompositeOperationResult Published = MHPublishCompositeV4(
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
    for (AActor* Actor : Actors)
    {
        Actor->Modify();
        Actor->GetWorld()->EditorDestroyActor(Actor, true);
    }
    GEditor->SelectNone(false, true, false);
    GEditor->SelectActor(CompositeActor, true, true, true);
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
        UMHCompositeAsset* Asset = Actor != nullptr ? Actor->GetCompositeAsset() : nullptr;
        if (Actor == nullptr || Asset == nullptr)
        {
            OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: Break cannot resolve the root composite asset");
            return false;
        }
        FMHCompositeDocument Document;
        if (!MHExtractCompositeV4(*Asset, Document, OutError))
        {
            return false;
        }
        FActorBreakPlan& Plan = Plans.AddDefaulted_GetRef();
        Plan.Actor = Actor;
        if (!MHCollectBreakSpecs(
                Document.Nodes,
                Actor->GetActorTransform(),
                *Settings,
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
    for (const FActorBreakPlan& Plan : Plans)
    {
        Plan.Actor->Modify();
        Plan.Actor->GetWorld()->EditorDestroyActor(Plan.Actor, true);
    }
    GEditor->SelectNone(false, true, false);
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
    if (Actor == nullptr || Asset == nullptr || !MHExtractCompositeV4(*Asset, EditingDocument, OutError))
    {
        if (OutError.IsEmpty())
        {
            OutError = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: Edit requires a live managed composite");
        }
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

    FMHCompositeDocument Edited = EditingDocument;
    for (int32 Index = 0; Index < Edited.Nodes.Num(); ++Index)
    {
        USceneComponent* Component = EditingTopLevelComponents[Index].Get();
        if (Component == nullptr)
        {
            OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: an edit component disappeared before Commit");
            return false;
        }
        const FTransform Relative = Component->GetComponentTransform().GetRelativeTransform(Actor->GetActorTransform());
        Edited.Nodes[Index].Transform.TranslationCm = Relative.GetTranslation();
        Edited.Nodes[Index].Transform.RotationQuat = Relative.GetRotation();
        Edited.Nodes[Index].Transform.Scale = Relative.GetScale3D();
    }

    const FMHCompositeDocument Previous = EditingDocument;
    const FScopedTransaction Transaction(INVTEXT("Commit MH Composite Edit"));
    Asset->Modify();
    Actor->Modify();
    if (!MHApplyCompositeV4(*Asset, Edited, OutError))
    {
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    FMHCompositeOperationResult Published = MHPublishCompositeV4(*Asset, SourceRoot, nullptr);
    OutWarnings.Append(Published.Warnings);
    if (!Published.Succeeded())
    {
        FString RestoreError;
        MHApplyCompositeV4(*Asset, Previous, RestoreError);
        MHSaveCompositeLevelAsset(*Asset, RestoreError);
        OutError = MoveTemp(Published.Error);
        return false;
    }
    Actor->SetPlacementEditMode(false);
    EditingActor.Reset();
    EditingTopLevelComponents.Reset();
    EditingDocument = FMHCompositeDocument();
    Actor->RebuildComposite();
    return true;
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
    TSet<UMHCompositeAsset*> ChangedAssets;
    for (AMHCompositeActor* Actor : Actors)
    {
        if (Actor == nullptr)
        {
            continue;
        }
        Actor->Modify();
        Actor->RebuildComposite();
        OutWarnings.Append(Actor->GetLastPlacementWarnings());
        if (UMHCompositeAsset* Asset = Actor->GetCompositeAsset())
        {
            ChangedAssets.Add(Asset);
        }
    }
    // Refresh can reflect restored nested resources without mutating the root
    // asset. Dirty only its visual cache, once per unique asset.
    for (UMHCompositeAsset* Asset : ChangedAssets)
    {
        MHDirtyCompositeThumbnail(*Asset);
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
