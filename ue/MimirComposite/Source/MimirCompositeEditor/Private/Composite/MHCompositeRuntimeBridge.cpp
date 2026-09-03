#include "Composite/MHCompositeRuntimeBridge.h"

#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHCompositeTransformAdmission.h"
#include "Composite/MHEndpointPrototypeRegistry.h"
#include "Composite/MHProofCache.h"
#include "Composite/MHRuntimeCompositeActor.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/App.h"
#include "ModuleDescriptor.h"
#include "PluginDescriptor.h"
#include "ProjectDescriptor.h"
#include "Serialization/Archive.h"
#include "Serialization/ArchiveSavePackageData.h"
#include "Settings/MHCompositeSettings.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHRuntimeBridge, Log, All);

namespace UE::MimirComposite
{
namespace
{

struct FMHRuntimeBridgePreparedPlacement
{
    TWeakObjectPtr<AMHCompositeActor> Source;
    FMHRuntimeCompositeInput Input;
    int32 Seed = 0;
    int32 AppearanceSeed = 0;
    FTransform Transform;
};

struct FMHRuntimeBridgeLevelSnapshot
{
    TWeakObjectPtr<ULevel> Level;
    TArray<TObjectPtr<AActor>> Actors;
    bool bPackageDirty = false;
};

struct FMHRuntimeBridgeCookOverlay
{
    TArray<TWeakObjectPtr<AMHCompositeActor>> Sources;
    TArray<TWeakObjectPtr<AMHRuntimeCompositeActor>> RuntimeActors;
    TArray<FMHRuntimeBridgeLevelSnapshot> Levels;
    FString Error;
    bool bPrepared = false;
};

TMap<TWeakObjectPtr<UWorld>, FMHRuntimeBridgeCookOverlay> MHRuntimeBridgeCookOverlays;
FDelegateHandle MHRuntimeBridgePreSaveHandle;
FDelegateHandle MHRuntimeBridgePostSaveHandle;
FDelegateHandle MHRuntimeBridgeCollectHandle;
FDelegateHandle MHRuntimeBridgeObjectPreSaveHandle;
FDelegateHandle MHRuntimeBridgePIEHandle;
FDelegateHandle MHRuntimeBridgeWorldCleanupHandle;

FString MHRuntimeBridgeError(const FString& Detail)
{
    return TEXT("MH_E_INVALID_RESOURCE_SOURCE: ") + Detail;
}

bool MHRuntimeBridgeModuleShips(const FModuleDescriptor& Module)
{
    return Module.IsCompiledInConfiguration(FPlatformMisc::GetUBTPlatform(),
        EBuildConfiguration::Shipping, FApp::GetProjectName(), EBuildTargetType::Game,
        false, true);
}

bool MHRuntimeBridgeClassShips(const UClass& Class, FString& Error)
{
    // Native package flags cover built-in Engine editor/developer modules.
    // Descriptor checks also cover project/plugin platform and target exclusions.
    for (const UClass* Current = &Class; Current != nullptr; Current = Current->GetSuperClass())
    {
        const UPackage* Package = Current->GetOutermost();
        if (Package->HasAnyPackageFlags(PKG_EditorOnly | PKG_Developer))
        {
            Error = MHRuntimeBridgeError(Class.GetPathName() + TEXT(" depends on an editor/developer-only actor class"));
            return false;
        }
        const FString PackageName = Package->GetName();
        if (!PackageName.StartsWith(TEXT("/Script/"))) continue;
        const FName ModuleName(*PackageName.Mid(8));
        const auto Check = [&](const TArray<FModuleDescriptor>& Modules)
        {
            const FModuleDescriptor* Module = Modules.FindByPredicate(
                [&](const FModuleDescriptor& Value) { return Value.Name == ModuleName; });
            return Module == nullptr || MHRuntimeBridgeModuleShips(*Module);
        };
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().GetModuleOwnerPlugin(ModuleName);
        const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
        if ((Plugin.IsValid() && !Check(Plugin->GetDescriptor().Modules)) ||
            (Project != nullptr && !Check(Project->Modules)))
        {
            Error = MHRuntimeBridgeError(Class.GetPathName() + TEXT(" is excluded from the packaged Game target by its module descriptor"));
            return false;
        }
    }
    return true;
}

void MHRuntimeBridgeFindPlacements(UWorld& World, TArray<AMHCompositeActor*>& Out)
{
    Out.Reset();
    for (ULevel* Level : World.GetLevels())
        if (Level != nullptr)
            for (AActor* Actor : Level->Actors)
                if (AMHCompositeActor* Placement = Cast<AMHCompositeActor>(Actor);
                    IsValid(Placement) && !Placement->IsActorBeingDestroyed()) Out.AddUnique(Placement);
    Out.Sort([](const AMHCompositeActor& A, const AMHCompositeActor& B)
        { return A.GetPathName() < B.GetPathName(); });
}

bool MHRuntimeBridgeHasPartitionedPlacement(UWorld& World)
{
    UWorldPartition* Partition = World.GetWorldPartition();
    if (Partition == nullptr) return false;
    bool bFound = false;
    FWorldPartitionHelpers::ForEachActorDescInstance(Partition,
        [&](const FWorldPartitionActorDescInstance* Desc)
        {
            bFound = Desc->GetNativeClass() == AMHCompositeActor::StaticClass()->GetClassPathName();
            return !bFound;
        });
    return bFound;
}

bool MHRuntimeBridgeReferencesPlacement(const FProperty& Property, const void* Value,
    const TArray<AMHCompositeActor*>& Sources, FString& Target)
{
    if (Property.HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly | CPF_Deprecated)) return false;
    const auto MatchesObject = [&](const UObject* Object)
    {
        if (Object == nullptr) return false;
        for (const AMHCompositeActor* Source : Sources)
            if (Object == Source || Object->IsIn(Source))
            {
                Target = Object->GetPathName();
                return true;
            }
        return false;
    };
    const auto MatchesPath = [&](const FSoftObjectPath& Path)
    {
        const FString Text = Path.ToString();
        for (const AMHCompositeActor* Source : Sources)
        {
            const FString SourcePath = Source->GetPathName();
            const FString EditorPath = UWorld::RemovePIEPrefix(SourcePath);
            if (Text == SourcePath || Text.StartsWith(SourcePath + TEXT(".")) ||
                Text == EditorPath || Text.StartsWith(EditorPath + TEXT(".")))
            {
                Target = Text;
                return true;
            }
        }
        return false;
    };
    if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(&Property))
        return MatchesPath(Soft->GetPropertyValue(Value).ToSoftObjectPath());
    if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
        return MatchesObject(Object->GetObjectPropertyValue(Value));
    if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(&Property))
        return MatchesObject(Interface->GetPropertyValue(Value).GetObject());
    if (const FDelegateProperty* Delegate = CastField<FDelegateProperty>(&Property))
        return MatchesObject(Delegate->GetPropertyValue(Value).GetUObject());
    if (const FMulticastDelegateProperty* Delegate = CastField<FMulticastDelegateProperty>(&Property))
    {
        if (const FMulticastScriptDelegate* Multicast = Delegate->GetMulticastDelegate(Value))
            for (UObject* Object : Multicast->GetAllObjects()) if (MatchesObject(Object)) return true;
        return false;
    }
    if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
    {
        if (Struct->Struct->GetFName() == FName(TEXT("SoftObjectPath")) &&
            Struct->Struct->GetOutermost()->GetFName() == FName(TEXT("/Script/CoreUObject")))
            return MatchesPath(*static_cast<const FSoftObjectPath*>(Value));
        for (TFieldIterator<FProperty> Field(Struct->Struct); Field; ++Field)
            for (int32 Index = 0; Index < Field->ArrayDim; ++Index)
                if (MHRuntimeBridgeReferencesPlacement(**Field, Field->ContainerPtrToValuePtr<void>(Value, Index), Sources, Target)) return true;
    }
    else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
    {
        FScriptArrayHelper Helper(Array, Value);
        for (int32 Index = 0; Index < Helper.Num(); ++Index)
            if (MHRuntimeBridgeReferencesPlacement(*Array->Inner, Helper.GetRawPtr(Index), Sources, Target)) return true;
    }
    else if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
    {
        FScriptSetHelper Helper(Set, Value);
        for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            if (Helper.IsValidIndex(Index) && MHRuntimeBridgeReferencesPlacement(*Set->ElementProp, Helper.GetElementPtr(Index), Sources, Target)) return true;
    }
    else if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
    {
        FScriptMapHelper Helper(Map, Value);
        for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            if (Helper.IsValidIndex(Index) &&
                (MHRuntimeBridgeReferencesPlacement(*Map->KeyProp, Helper.GetKeyPtr(Index), Sources, Target) ||
                 MHRuntimeBridgeReferencesPlacement(*Map->ValueProp, Helper.GetValuePtr(Index), Sources, Target))) return true;
    }
    return false;
}

bool MHRuntimeBridgeValidateSceneReferences(UWorld& World,
    const TArray<AMHCompositeActor*>& Sources, FString& Error)
{
    if (Sources.IsEmpty()) return true;
    for (const AMHCompositeActor* Source : Sources)
    {
        if (const AActor* Parent = Source->GetAttachParentActor())
        {
            Error = MHRuntimeBridgeError(Source->GetPathName() + TEXT(" is attached to ") + Parent->GetPathName() +
                TEXT("; runtime handoff does not rewrite authoring attachments"));
            return false;
        }
        TArray<AActor*> Children;
        Source->GetAttachedActors(Children, true, false);
        for (const AActor* Child : Children)
            if (IsValid(Child) && !Child->HasAnyFlags(RF_Transient))
            {
                Error = MHRuntimeBridgeError(Child->GetPathName() + TEXT(" is attached to composite placement ") +
                    Source->GetPathName() + TEXT("; runtime handoff does not rewrite authoring attachments"));
                return false;
            }
    }
    // Inspect persisted actor properties and owned subobjects, not the Level's
    // required Actors list or transient editor selection/preview machinery.
    // Nested structs/containers and soft paths are checked without loading assets
    // or serializing arbitrary gameplay objects as a side effect of admission.
    for (ULevel* Level : World.GetLevels())
        if (Level != nullptr)
            for (AActor* Actor : Level->Actors)
            {
                if (!IsValid(Actor) || Sources.Contains(Actor) || Actor->IsEditorOnly() || Actor->HasAnyFlags(RF_Transient)) continue;
                TArray<UObject*> Objects;
                GetObjectsWithOuter(Actor, Objects, true, RF_Transient | RF_ClassDefaultObject | RF_ArchetypeObject);
                Objects.AddUnique(Actor);
                Objects.Sort([](const UObject& A, const UObject& B) { return A.GetPathName() < B.GetPathName(); });
                for (const UObject* Object : Objects)
                {
                    if (Object->IsEditorOnly()) continue;
                    for (TFieldIterator<FProperty> Property(Object->GetClass()); Property; ++Property)
                        for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
                        {
                            FString Target;
                            if (MHRuntimeBridgeReferencesPlacement(**Property,
                                Property->ContainerPtrToValuePtr<void>(Object, Index), Sources, Target))
                            {
                                Error = MHRuntimeBridgeError(Object->GetPathName() + TEXT(".") + Property->GetName() +
                                    TEXT(" references editor placement ") + Target + TEXT("; runtime handoff does not rewrite level references"));
                                return false;
                            }
                        }
                }
            }
    return true;
}

bool MHRuntimeBridgePreflight(UWorld& World, TArray<FMHRuntimeBridgePreparedPlacement>& Out, FString& Error)
{
    Out.Reset();
    TArray<AMHCompositeActor*> Sources;
    MHRuntimeBridgeFindPlacements(World, Sources);
    if ((World.GetWorldPartition() != nullptr && !Sources.IsEmpty()) || MHRuntimeBridgeHasPartitionedPlacement(World))
    {
        Error = MHRuntimeBridgeError(TEXT("runtime handoff of World Partition composite placements requires the V5-S7 World Partition admission; cook/PIE blocked"));
        return false;
    }
    if (!MHRuntimeBridgeValidateSceneReferences(World, Sources, Error)) return false;
    for (AMHCompositeActor* Source : Sources)
    {
        if (Source->IsPackageExternal())
        {
            Error = MHRuntimeBridgeError(Source->GetPathName() + TEXT(" is an external actor; runtime handoff requires V5-S7 OFPA admission"));
            return false;
        }
        FMHRuntimeBridgePreparedPlacement Prepared;
        Prepared.Source = Source;
        Prepared.Seed = Source->GetSeed();
        // Same append-only handoff path the layout Seed already travels.
        Prepared.AppearanceSeed = Source->GetAppearanceSeed();
        Prepared.Transform = Source->GetActorTransform();
        if (!MHBuildRuntimeCompositeInput(*Source, Prepared.Input, Error)) return false;
        FMHRandomSourceGraph Graph;
        FMHResolvedCompositePlan TransportPlan;
        if (!MHDecodeRuntimeCompositeGraph(Prepared.Input.GraphBytes, Graph, Error) ||
            !MHResolveCompositePlan(Graph, Prepared.Seed, Prepared.AppearanceSeed, TransportPlan, Error) ||
            !MHValidateResolvedPlacementTransforms(TransportPlan, Prepared.Transform, Error)) return false;
        UMHProofCacheSubsystem* Proofs = UMHProofCacheSubsystem::Get();
        const FMHProofResult Proof = Proofs != nullptr
            ? Proofs->GetProofState(*Source)
            : FMHProofResult();
        if (Proof.State != EMHProofState::Fresh || !Proof.Plan.IsValid() ||
            Proof.Plan->ResolvedSignature != TransportPlan.ResolvedSignature ||
            Proof.Plan->PlacementSignature != TransportPlan.PlacementSignature)
        {
            Error = MHRuntimeBridgeError(
                TEXT("transport graph diverges from proof for ") + Source->GetPathName());
            return false;
        }
        Out.Add(MoveTemp(Prepared));
    }
    return true;
}

void MHRuntimeBridgeCaptureLevel(ULevel& Level, FMHRuntimeBridgeCookOverlay& Overlay)
{
    if (Overlay.Levels.ContainsByPredicate([&](const FMHRuntimeBridgeLevelSnapshot& Value) { return Value.Level.Get() == &Level; })) return;
    FMHRuntimeBridgeLevelSnapshot Snapshot;
    Snapshot.Level = &Level;
    Snapshot.Actors = Level.Actors;
    Snapshot.bPackageDirty = Level.GetOutermost()->IsDirty();
    Overlay.Levels.Add(MoveTemp(Snapshot));
}

void MHRuntimeBridgeRemoveOverlay(UWorld& World, FMHRuntimeBridgeCookOverlay& Overlay)
{
    for (const TWeakObjectPtr<AMHRuntimeCompositeActor>& Weak : Overlay.RuntimeActors)
        if (AMHRuntimeCompositeActor* Actor = Weak.Get()) World.DestroyActor(Actor, false, false);
    for (const FMHRuntimeBridgeLevelSnapshot& Snapshot : Overlay.Levels)
        if (ULevel* Level = Snapshot.Level.Get())
        {
            // DestroyActor leaves null slots. Restore the exact pre-overlay list,
            // including any pre-existing holes, without touching authoring identity.
            Level->Actors = Snapshot.Actors;
            Level->GetOutermost()->SetDirtyFlag(Snapshot.bPackageDirty);
        }
    Overlay.RuntimeActors.Reset();
    Overlay.Levels.Reset();
    Overlay.bPrepared = false;
}

bool MHRuntimeBridgeMaterialize(UWorld& World, const TArray<FMHRuntimeBridgePreparedPlacement>& Prepared,
    const bool bCooking, FMHRuntimeBridgeCookOverlay& Overlay, FString& Error)
{
    for (const FMHRuntimeBridgePreparedPlacement& Placement : Prepared)
    {
        AMHCompositeActor* Source = Placement.Source.Get();
        if (Source == nullptr)
        {
            Error = MHRuntimeBridgeError(TEXT("placement disappeared after runtime handoff preflight"));
            MHRuntimeBridgeRemoveOverlay(World, Overlay);
            return false;
        }
        // Each streaming map is cooked as its own world/package. All loaded
        // placements were admitted, but this overlay writes only the saved map.
        if (bCooking && Source->GetLevel()->GetOutermost() != World.GetOutermost()) continue;
        MHRuntimeBridgeCaptureLevel(*Source->GetLevel(), Overlay);
        FActorSpawnParameters Parameters;
        Parameters.OverrideLevel = Source->GetLevel();
        Parameters.Name = FName(*(Source->GetName() + TEXT("_MHRuntime")));
        Parameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
        Parameters.ObjectFlags = bCooking ? RF_NoFlags : RF_Transient;
        Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AMHRuntimeCompositeActor* Runtime = World.SpawnActor<AMHRuntimeCompositeActor>(
            AMHRuntimeCompositeActor::StaticClass(), Placement.Transform, Parameters);
        if (Runtime == nullptr)
        {
            Error = MHRuntimeBridgeError(Source->GetPathName() + TEXT(" runtime wrapper could not be created"));
            MHRuntimeBridgeRemoveOverlay(World, Overlay);
            return false;
        }
        Overlay.RuntimeActors.Add(Runtime);
        // The base index is a project setting; the editor module resolves it
        // once at handoff so cooked runtime never reads editor settings.
        Runtime->SetAppearanceCustomDataBaseIndex(
            GetDefault<UMHCompositeSettings>()->AppearanceCustomDataBaseIndex);
        if (!Runtime->Configure(Placement.Input, Placement.Seed, Placement.AppearanceSeed, Error))
        {
            MHRuntimeBridgeRemoveOverlay(World, Overlay);
            return false;
        }
        Overlay.Sources.Add(Source);
    }
    Overlay.bPrepared = true;
    return true;
}

void MHRuntimeBridgePreSave(UWorld* World, FObjectPreSaveContext Context)
{
    if (World == nullptr || !Context.IsCooking()) return;
    FMHRuntimeBridgeCookOverlay& Overlay = MHRuntimeBridgeCookOverlays.FindOrAdd(World);
    if (Overlay.bPrepared) return;
    TArray<FMHRuntimeBridgePreparedPlacement> Prepared;
    if (!MHRuntimeBridgePreflight(*World, Prepared, Overlay.Error) ||
        !MHRuntimeBridgeMaterialize(*World, Prepared, true, Overlay, Overlay.Error))
        UE_LOG(LogMHRuntimeBridge, Error, TEXT("%s"), *Overlay.Error);
}

void MHRuntimeBridgePostSave(UWorld* World, FObjectPostSaveContext Context)
{
    if (World == nullptr || !Context.IsCooking()) return;
    if (FMHRuntimeBridgeCookOverlay* Overlay = MHRuntimeBridgeCookOverlays.Find(World))
    {
        MHRuntimeBridgeRemoveOverlay(*World, *Overlay);
        MHRuntimeBridgeCookOverlays.Remove(World);
    }
}

void MHRuntimeBridgeCollectSaveReferences(UWorld* World, FArchive& Archive)
{
    if (World == nullptr || !Archive.IsCooking()) return;
    const FArchiveSavePackageData* SaveData = Archive.GetSavePackageData();
    const FObjectSavePackageSerializeContext* Context = SaveData != nullptr ? &SaveData->SavePackageContext : nullptr;
    // Dependency-only harvest explicitly runs without PreSave/PostSave and
    // does not export this world. Its editor input is not a cooked placement.
    if (Context != nullptr && Context->GetPhase() == EObjectSaveContextPhase::CookDependencyHarvest) return;
    TArray<AMHCompositeActor*> Sources;
    MHRuntimeBridgeFindPlacements(*World, Sources);
    FMHRuntimeBridgeCookOverlay* Overlay = MHRuntimeBridgeCookOverlays.Find(World);
    if (Sources.IsEmpty() && !MHRuntimeBridgeHasPartitionedPlacement(*World) && Overlay == nullptr) return;
    if (Overlay == nullptr || !Overlay->bPrepared || !Overlay->Error.IsEmpty())
    {
        const FString Error = Overlay != nullptr && !Overlay->Error.IsEmpty() ? Overlay->Error :
            MHRuntimeBridgeError(World->GetPathName() + TEXT(" has no admitted runtime cook overlay (including unsupported concurrent save)"));
        Archive.SetError();
        // UE 5.7's package harvester does not promote its archive error to a
        // SavePackage failure. The Error log is mandatory: it fails the cook
        // commandlet/UAT. A failed cook's files are not accepted package output.
        UE_LOG(LogMHRuntimeBridge, Error, TEXT("%s"), *Error);
    }
}

void MHRuntimeBridgeObjectPreSave(UObject* Object, FObjectPreSaveContext Context)
{
    if (!Context.IsCooking()) return;
    if (const AMHCompositeActor* Placement = Cast<AMHCompositeActor>(Object);
        Placement != nullptr && !Placement->IsTemplate() && !MHIsRuntimeCompositeCookPrepared(*Placement))
        UE_LOG(LogMHRuntimeBridge, Error, TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s has no admitted runtime cook overlay"), *Placement->GetPathName());
}

void MHRuntimeBridgeInitializedActors(const FActorsInitializedParams& Params)
{
    UWorld* World = Params.World;
    if (World == nullptr || World->WorldType != EWorldType::PIE) return;
    TArray<FMHRuntimeBridgePreparedPlacement> Prepared;
    FMHRuntimeBridgeCookOverlay Overlay;
    FString Error;
    if (!MHRuntimeBridgePreflight(*World, Prepared, Error) ||
        !MHRuntimeBridgeMaterialize(*World, Prepared, false, Overlay, Error))
    {
        UE_LOG(LogMHRuntimeBridge, Error, TEXT("%s"), *Error);
        if (GEditor != nullptr) GEditor->RequestEndPlayMap();
        return;
    }
    // Only the duplicated play world is changed. The source level keeps its
    // editor actor, asset reference, seed, authored transform and dirty state.
    for (const TWeakObjectPtr<AMHCompositeActor>& Source : Overlay.Sources)
        if (AMHCompositeActor* Actor = Source.Get()) World->DestroyActor(Actor, false, false);
}

void MHRuntimeBridgeWorldCleanup(UWorld* World, bool, bool)
{
    MHRuntimeBridgeCookOverlays.Remove(World);
}

} // namespace

bool MHBuildRuntimeCompositeInput(const AMHCompositeActor& Placement,
    FMHRuntimeCompositeInput& OutInput, FString& OutError)
{
    OutInput = FMHRuntimeCompositeInput();
    OutError.Reset();
    if (Placement.IsPlacementEditMode())
    {
        OutError = MHRuntimeBridgeError(Placement.GetPathName() + TEXT(" has an active uncommitted Edit session"));
        return false;
    }
    const UMHCompositeAsset* Asset = Placement.GetCompositeAsset();
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Asset == nullptr || Settings == nullptr)
    {
        OutError = MHRuntimeBridgeError(Placement.GetPathName() + TEXT(" has no applied composite definition"));
        return false;
    }

    UMHProofCacheSubsystem* Proofs = UMHProofCacheSubsystem::Get();
    FMHProofResult Proof;
    if (Proofs == nullptr)
    {
        OutError = MHRuntimeBridgeError(TEXT("proof cache subsystem is unavailable"));
        return false;
    }
    if (!Proofs->BuildProofNow(Placement, Proof, OutError))
    {
        return false;
    }

    // Proof is the admission authority. This second assembly produces the
    // transport graph bytes and bindings; preflight independently decodes and
    // resolves those exact bytes, then compares their proof signatures.
    FMHRandomSourceGraph Graph;
    TSet<FMHResourceKey> Dependencies;
    if (!MHBuildAppliedCompositeGraph(*Asset, *Settings, Graph, Dependencies, OutError)) return false;
    TArray<FString> Keys;
    if (!MHCollectRuntimeCompositeBindingKeys(Graph, Keys, OutError)) return false;
    FMHRuntimeCompositeInput Input;
    for (const FString& KeyString : Keys)
    {
        FString Kind;
        FString Name;
        if (!KeyString.Split(TEXT(":"), &Kind, &Name))
        {
            OutError = MHRuntimeBridgeError(TEXT("invalid runtime binding key ") + KeyString);
            return false;
        }
        UObject* Object = nullptr;
        if (Kind == TEXT("actor"))
        {
            const FSoftClassPath* ClassPath = Settings->ActorClassRegistry.Find(Name);
            UClass* Class = ClassPath != nullptr ? ClassPath->TryLoadClass<AActor>() : nullptr;
            if (Class == nullptr || !MHRuntimeBridgeClassShips(*Class, OutError))
            {
                if (OutError.IsEmpty()) OutError = MHRuntimeBridgeError(KeyString + TEXT(" has no packaged actor class"));
                return false;
            }
            Object = Class;
        }
        else
        {
            FMHResourceKey Key;
            Key.LogicalName = Name;
            if (!MHResourceKindFromLabel(Kind, Key.Kind))
            {
                OutError = MHRuntimeBridgeError(TEXT("unsupported runtime binding ") + KeyString);
                return false;
            }
            Object = UMHEndpointPrototypeRegistry::ResolveEndpoint(Key, OutError);
            if (Object == nullptr) return false;
        }
        FMHRuntimeCompositeBinding Binding;
        Binding.ResourceKey = KeyString;
        Binding.Object = Object;
        Input.Bindings.Add(MoveTemp(Binding));
    }
    if (!MHValidateRuntimeCompositeBindings(Graph, Input.Bindings, OutError) ||
        !MHEncodeRuntimeCompositeGraph(Graph, Input.GraphBytes, OutError)) return false;
    OutInput = MoveTemp(Input);
    return true;
}

bool MHIsRuntimeCompositeCookPrepared(const AMHCompositeActor& Placement)
{
    const FMHRuntimeBridgeCookOverlay* Overlay = MHRuntimeBridgeCookOverlays.Find(Placement.GetWorld());
    return Overlay != nullptr && Overlay->bPrepared && Overlay->Error.IsEmpty() &&
        Overlay->Sources.ContainsByPredicate([&](const TWeakObjectPtr<AMHCompositeActor>& Source) { return Source.Get() == &Placement; });
}

bool MHValidateRuntimeCompositeWorld(UWorld& World, FString& OutError)
{
    OutError.Reset();
    TArray<FMHRuntimeBridgePreparedPlacement> Prepared;
    return MHRuntimeBridgePreflight(World, Prepared, OutError);
}

void MHStartupRuntimeCompositeBridge()
{
    if (MHRuntimeBridgePreSaveHandle.IsValid()) return;
    MHRuntimeBridgePreSaveHandle = FEditorDelegates::PreSaveWorldWithContext.AddStatic(&MHRuntimeBridgePreSave);
    MHRuntimeBridgePostSaveHandle = FEditorDelegates::PostSaveWorldWithContext.AddStatic(&MHRuntimeBridgePostSave);
    MHRuntimeBridgeCollectHandle = FWorldDelegates::OnCollectSaveReferences.AddStatic(&MHRuntimeBridgeCollectSaveReferences);
    MHRuntimeBridgeObjectPreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddStatic(&MHRuntimeBridgeObjectPreSave);
    MHRuntimeBridgePIEHandle = FWorldDelegates::OnWorldInitializedActors.AddStatic(&MHRuntimeBridgeInitializedActors);
    MHRuntimeBridgeWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&MHRuntimeBridgeWorldCleanup);
}

void MHShutdownRuntimeCompositeBridge()
{
    FEditorDelegates::PreSaveWorldWithContext.Remove(MHRuntimeBridgePreSaveHandle);
    FEditorDelegates::PostSaveWorldWithContext.Remove(MHRuntimeBridgePostSaveHandle);
    FWorldDelegates::OnCollectSaveReferences.Remove(MHRuntimeBridgeCollectHandle);
    FCoreUObjectDelegates::OnObjectPreSave.Remove(MHRuntimeBridgeObjectPreSaveHandle);
    FWorldDelegates::OnWorldInitializedActors.Remove(MHRuntimeBridgePIEHandle);
    FWorldDelegates::OnWorldCleanup.Remove(MHRuntimeBridgeWorldCleanupHandle);
    MHRuntimeBridgePreSaveHandle.Reset();
    MHRuntimeBridgePostSaveHandle.Reset();
    MHRuntimeBridgeCollectHandle.Reset();
    MHRuntimeBridgeObjectPreSaveHandle.Reset();
    MHRuntimeBridgePIEHandle.Reset();
    MHRuntimeBridgeWorldCleanupHandle.Reset();
    // UObject services can already be gone at module shutdown. Weak overlay
    // state has no teardown work; normal cooked-save/World cleanup owns it.
    MHRuntimeBridgeCookOverlays.Reset();
}

} // namespace UE::MimirComposite
