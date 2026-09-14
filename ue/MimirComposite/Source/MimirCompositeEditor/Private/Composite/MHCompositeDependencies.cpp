#include "Composite/MHCompositeDependencies.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Source/MHSourceImportBatch.h"
#include "StaticMeshCompiler.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::MimirComposite
{
namespace
{
struct FDependencyBindingPlan
{
    FMHResourceKey Key;
    UStaticMesh* Mesh = nullptr;
    TMap<int32, UMaterialInstanceConstant*> Materials;
};

struct FDependencyCollector
{
    FMHCompositeDependencyUpdateResult& Result;
    TMap<FMHResourceKey, UObject*> Objects;
    TMap<FMHResourceKey, FString> Failures;
    TArray<TStrongObjectPtr<UObject>> KeepAlive;
    TSet<UMHCompositeAsset*> Visiting;
    TSet<UMHCompositeAsset*> Visited;
    TSet<FMHResourceKey> MeshKeys;

    UObject* Resolve(const FMHResourceKey& Key, FString& Error)
    {
        Error.Reset();
        if (const FString* Failure = Failures.Find(Key)) { Error = *Failure; return nullptr; }
        if (UObject* const* Object = Objects.Find(Key)) return *Object;
        UObject* Object = nullptr;
        if (!Key.IsCanonical())
            Error = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: ") + Key.ToString();
        else if (MHCheckGeneratedAssetClaims(Key, Error))
        {
            const FString Path = MHEndpointObjectPath(Key);
            // Explicit repair reads the current generated asset, not a cached
            // failed endpoint from before an asset was restored.
            Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
            if (Object == nullptr) Error = TEXT("MH_E_RESOURCE_NOT_FOUND: no generated asset at ") + Path;
            else if (!MHAdmitEndpointIdentity(Key, *Object, Error)) Object = nullptr;
        }
        if (Object == nullptr) { Failures.Add(Key, Error); return nullptr; }
        Objects.Add(Key, Object);
        KeepAlive.Emplace(Object);
        return Object;
    }

    void Resource(const bool bComposite, const bool bMesh, const FString& Name)
    {
        if (!bComposite && !bMesh) return;
        const FMHResourceKey Key{bComposite ? EMHResourceKind::Composite : EMHResourceKind::StaticMesh, Name};
        if (bMesh) { MeshKeys.Add(Key); return; }
        FString Error;
        if (UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(Resolve(Key, Error))) Visit(*Asset);
        else Result.Errors.AddUnique(Error);
    }

    void Visit(UMHCompositeAsset& Asset)
    {
        if (Visiting.Contains(&Asset) || Visiting.Num() >= 256)
        {
            Result.Errors.AddUnique(TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: cyclic or excessively deep dependency at ") + Asset.GetPathName());
            return;
        }
        if (Visited.Contains(&Asset)) return;
        Visited.Add(&Asset);
        const FMHResourceKey Key{EMHResourceKind::Composite, Asset.LogicalName};
        FString Error;
        if (!MHCheckGeneratedAssetClaims(Key, Error) || !MHAdmitEndpointIdentity(Key, Asset, Error))
        {
            Result.Errors.AddUnique(Error);
            return;
        }
        FMHCompositeDocument Document;
        if (!MHExtractCompositeV5(Asset, Document, Error))
        {
            Result.Errors.AddUnique(Error);
            return;
        }
        ++Result.CompositesVisited;
        KeepAlive.Emplace(&Asset);
        Visiting.Add(&Asset);
        // Nodes are the validated flattened authored structure. Inspect every
        // option, including weight zero; the current placement seed is irrelevant.
        for (const FMHCompositeAssetNode& Node : Asset.Nodes)
        {
            Resource(Node.Kind == EMHCompositeNodeKind::Composite, Node.Kind == EMHCompositeNodeKind::Mesh, Node.Resource);
            for (const FMHCompositeOption& Option : Node.Options)
                Resource(Option.Kind == EMHCompositeOptionKind::Composite, Option.Kind == EMHCompositeOptionKind::Mesh, Option.Resource);
        }
        Visiting.Remove(&Asset);
    }
};
} // namespace

bool MHUpdateCompositeDependencies(const TArray<UMHCompositeAsset*>& Roots, FMHCompositeDependencyUpdateResult& OutResult)
{
    return MHUpdateAssetDependencies(Roots, {}, OutResult);
}

bool MHUpdateAssetDependencies(
    const TArray<UMHCompositeAsset*>& Roots,
    const TArray<UStaticMesh*>& Meshes,
    FMHCompositeDependencyUpdateResult& OutResult)
{
    static bool bUpdatingDependencies = false;
    OutResult = {};
    if (!IsInGameThread() || GEditor == nullptr || GEditor->PlayWorld != nullptr || MHIsSourceImportBatchActive() || bUpdatingDependencies)
    {
        OutResult.Errors.Add(TEXT("MH_E_IMPORT_THREAD_INVALID: update dependencies requires the editor outside PIE and active imports"));
        return false;
    }
    TGuardValue<bool> UpdatingGuard(bUpdatingDependencies, true);
    if (const UMHCompositeLevelSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>();
        Subsystem != nullptr && Subsystem->IsEditingComposite())
    {
        OutResult.Errors.Add(TEXT("MH_E_IMPORT_THREAD_INVALID: finish Composite Edit Mode before updating dependencies"));
        return false;
    }
    if (Roots.IsEmpty() && Meshes.IsEmpty())
    {
        OutResult.Errors.Add(TEXT("MH_E_INVALID_RESOURCE_SOURCE: select at least one MH composite or Static Mesh"));
        return false;
    }
    IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.WaitForCompletion();
    FDependencyCollector Collector{OutResult};
    for (UMHCompositeAsset* Root : Roots)
    {
        if (IsValid(Root)) Collector.Visit(*Root);
        else OutResult.Errors.AddUnique(TEXT("MH_E_INVALID_RESOURCE_SOURCE: selected composite is unavailable"));
    }
    TArray<FMHResourceKey> MeshKeys = Collector.MeshKeys.Array();
    MeshKeys.Sort([](const FMHResourceKey& A, const FMHResourceKey& B) { return A.LogicalName < B.LogicalName; });
    TArray<FDependencyBindingPlan> Candidates;
    TSet<UStaticMesh*> SeenMeshes;
    for (const FMHResourceKey& Key : MeshKeys)
    {
        FString Error;
        UStaticMesh* Mesh = Cast<UStaticMesh>(Collector.Resolve(Key, Error));
        if (Mesh == nullptr) { OutResult.Errors.AddUnique(Error); continue; }
        SeenMeshes.Add(Mesh);
        Candidates.Add({Key, Mesh});
    }
    for (UStaticMesh* Mesh : Meshes)
    {
        if (!IsValid(Mesh))
        {
            OutResult.Errors.AddUnique(TEXT("MH_E_INVALID_RESOURCE_SOURCE: selected Static Mesh is unavailable"));
            continue;
        }
        if (SeenMeshes.Contains(Mesh)) continue;
        SeenMeshes.Add(Mesh);
        Collector.KeepAlive.Emplace(Mesh);
        FMHResourceKey Key{EMHResourceKind::StaticMesh, Mesh->GetName()};
        FString IdentityError;
        // Explicit asset selection also supports ordinary UE meshes. Only an
        // admitted MH mesh emits a managed-resource notification afterwards.
        if (!Key.IsCanonical() || !MHAdmitEndpointIdentity(Key, *Mesh, IdentityError)) Key.LogicalName.Reset();
        Candidates.Add({Key, Mesh});
    }
    TArray<FDependencyBindingPlan> Plans;
    for (FDependencyBindingPlan& Plan : Candidates)
    {
        UStaticMesh* Mesh = Plan.Mesh;
        FString Error;
        ++OutResult.MeshesVisited;
        const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
        for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
        {
            const FStaticMaterial& Slot = Slots[SlotIndex];
            const FName Name = Slot.MaterialSlotName.IsNone() ? Slot.ImportedMaterialSlotName : Slot.MaterialSlotName;
            UMaterialInstanceConstant* Material = Cast<UMaterialInstanceConstant>(Collector.Resolve(
                {EMHResourceKind::Material, Name.ToString()}, Error));
            if (Material == nullptr)
            {
                OutResult.Errors.Add(FString::Printf(TEXT("%s: slot '%s': %s"), *Mesh->GetPathName(), *Name.ToString(), *Error));
                continue;
            }
            if (Slot.MaterialInterface != Material) Plan.Materials.Add(SlotIndex, Material);
        }
        if (!Plan.Materials.IsEmpty()) Plans.Add(MoveTemp(Plan));
    }
    if (Plans.IsEmpty()) return OutResult.Errors.IsEmpty();
    TArray<UStaticMesh*> ChangedMeshes;
    FProperty* MaterialsProperty = FindFProperty<FProperty>(UStaticMesh::StaticClass(), UStaticMesh::GetStaticMaterialsName());
    check(MaterialsProperty != nullptr);
    {
        const FScopedTransaction Transaction(NSLOCTEXT("MimirComposite", "UpdateCompositeDependencies", "Update composite dependencies"));
        for (FDependencyBindingPlan& Plan : Plans)
        {
            UStaticMesh& Mesh = *Plan.Mesh;
            Mesh.SetFlags(RF_Transactional);
            Mesh.Modify();
            // One native material-property edit per mesh protects render state
            // and preserves both slot-name fields and the section-to-slot maps.
            Mesh.PreEditChange(MaterialsProperty);
            for (const TPair<int32, UMaterialInstanceConstant*>& Binding : Plan.Materials)
                Mesh.GetStaticMaterials()[Binding.Key].MaterialInterface = Binding.Value;
            FPropertyChangedEvent Changed(MaterialsProperty, EPropertyChangeType::ValueSet);
            Mesh.PostEditChangeProperty(Changed);
            Mesh.MarkPackageDirty();
            ChangedMeshes.Add(&Mesh);
            ++OutResult.MeshesUpdated;
            OutResult.SlotsUpdated += Plan.Materials.Num();
        }
    }
    FStaticMeshCompilingManager::Get().FinishCompilation(ChangedMeshes);
    // Reconcile pooled components and endpoint interfaces through the existing
    // notification funnel, once per changed mesh after all bindings are ready.
    for (const FDependencyBindingPlan& Plan : Plans)
        if (Plan.Key.IsCanonical()) MHNotifyGeneratedResourceChanged(Plan.Key);
    return OutResult.Errors.IsEmpty();
}
} // namespace UE::MimirComposite
