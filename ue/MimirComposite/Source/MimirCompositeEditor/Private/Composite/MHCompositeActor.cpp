#include "Composite/MHCompositeActor.h"

#include "Composite/MHCompositePlacementCompiler.h"
#include "Composite/MHCompositeProtocol.h"
#include "Composite/MHCompositeResolvedPlan.h"
#include "Composite/MHCompositeRuntimeBridge.h"
#include "Engine/World.h"
#include "LevelEditor.h"
#include "Logging/MessageLog.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Serialization/Archive.h"
#include "Serialization/ArchiveSavePackageData.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeActor)

DEFINE_LOG_CATEGORY_STATIC(LogMHCompositeActor, Display, All);

namespace
{
void BroadcastMHCompositeComponentsEdited()
{
    if (!IsRunningCommandlet() && FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
        FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor")).BroadcastComponentsEdited();
}

void DestroyMHRetiredComponents(const TArray<TObjectPtr<UActorComponent>>& Previous,
    const TArray<TObjectPtr<UActorComponent>>& Current)
{
    if (Previous.IsEmpty()) return;
    // One membership set instead of a linear Contains per retired candidate.
    // Retirement order and the set of destroyed components are unchanged.
    const TSet<TObjectPtr<UActorComponent>> Kept(Current);
    for (int32 Index = Previous.Num() - 1; Index >= 0; --Index)
        if (IsValid(Previous[Index]) && !Kept.Contains(Previous[Index])) Previous[Index]->DestroyComponent();
}
}

AMHCompositeActor::AMHCompositeActor()
{
    CompositeRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MHCompositeRoot"));
    SetRootComponent(CompositeRoot);
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
}

void AMHCompositeActor::Serialize(FArchive& Archive)
{
    Super::Serialize(Archive);
    const FArchiveSavePackageData* SaveData = Archive.GetSavePackageData();
    const FObjectSavePackageSerializeContext* SaveContext = SaveData != nullptr ? &SaveData->SavePackageContext : nullptr;
    if (Archive.IsCooking() && !IsTemplate() && SaveContext != nullptr &&
        SaveContext->GetPhase() != EObjectSaveContextPhase::CookDependencyHarvest &&
        !UE::MimirComposite::MHIsRuntimeCompositeCookPrepared(*this))
    {
        Archive.SetError();
        UE_LOG(LogMHCompositeActor, Error,
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s has no admitted runtime cook handoff"), *GetPathName());
    }
}

int32 AMHCompositeActor::GenerateAutoSeed(const int32 DifferentFrom)
{
    int32 Result = 0;
    do
    {
        const uint32 Bits = FGuid::NewGuid().A;
        FMemory::Memcpy(&Result, &Bits, sizeof(Result));
    } while (Result == 0 || Result == DifferentFrom);
    return Result;
}

void AMHCompositeActor::SetSeed(const int32 NewSeed)
{
    if (bPlacementEditMode)
    {
        LastPlacementError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: finish or cancel Edit before changing Seed");
        return;
    }
    if (Seed != NewSeed)
    {
        Modify();
        Seed = NewSeed;
    }
    RebuildPlacement(true);
}

void AMHCompositeActor::Reseed()
{
    SetSeed(GenerateAutoSeed(Seed));
}

void AMHCompositeActor::SetAutoSeed(const bool bEnabled)
{
    if (bAutoSeed != bEnabled)
    {
        Modify();
        bAutoSeed = bEnabled;
    }
}

const UE::MimirComposite::FMHResolvedCompositePlan* AMHCompositeActor::GetResolvedPlan() const
{
    return bPlanAvailable && ResolvedPlan.IsValid() && ResolvedPlan->Seed == Seed &&
        LastPlacementError.IsEmpty() ? ResolvedPlan.Get() : nullptr;
}

void AMHCompositeActor::SetCompositeAsset(UMHCompositeAsset* Asset)
{
    Modify();
    SetPlacementEditMode(false);
    if (CompositeAsset.Get() != Asset)
    {
        AppliedGraph.Reset();
        ResolvedPlan.Reset();
        bPlanAvailable = false;
    }
    CompositeAsset = Asset;
    RebuildComposite();
}

UMHCompositeAsset* AMHCompositeActor::GetCompositeAsset() const
{
    return CompositeAsset.LoadSynchronous();
}

void AMHCompositeActor::SetPlacementEditMode(const bool bEnabled)
{
    bPlacementEditMode = bEnabled;
    EditingGraph.Reset();
    EditingDocument.Reset();
    LastEditHandleTransforms.Reset();
    LastEditBasis = GetActorTransform();
    if (bEnabled && AppliedGraph.IsValid())
    {
        EditingGraph = *AppliedGraph;
        if (const UMHCompositeAsset* Asset = GetCompositeAsset())
        {
            UE::MimirComposite::FMHCompositeDocument Document;
            FString Error;
            if (UE::MimirComposite::MHExtractCompositeV5(*Asset, Document, Error)) EditingDocument = MoveTemp(Document);
        }
        for (const USceneComponent* Handle : TopLevelPlacementComponents)
            LastEditHandleTransforms.Add(Handle != nullptr ? Handle->GetComponentTransform() : FTransform::Identity);
    }
    SetActorTickEnabled(bEnabled);
}

bool AMHCompositeActor::DependsOnResource(const UE::MimirComposite::FMHResourceKey& Key) const
{
    return PlacementDependencies.Contains(Key);
}

bool AMHCompositeActor::GetEditedCompositeDocument(UE::MimirComposite::FMHCompositeDocument& OutDocument) const
{
    if (!bPlacementEditMode || !EditingDocument.IsSet() || GetResolvedPlan() == nullptr) return false;
    if (TopLevelPlacementComponents.Num() != EditingDocument->Nodes.Num() ||
        TopLevelPlacementComponents.ContainsByPredicate([](const USceneComponent* Handle) { return !IsValid(Handle); })) return false;
    OutDocument = EditingDocument.GetValue();
    return true;
}

void AMHCompositeActor::AttachRootTransformHook()
{
    if (CompositeRoot != nullptr && !IsTemplate())
    {
        CompositeRoot->TransformUpdated.RemoveAll(this);
        CompositeRoot->TransformUpdated.AddUObject(this, &AMHCompositeActor::UpdatePlacementBasis);
    }
}

void AMHCompositeActor::ClearDerivedComponents()
{
    const TArray<TObjectPtr<UActorComponent>> Previous = MoveTemp(DerivedComponents);
    DestroyMHRetiredComponents(Previous, DerivedComponents);
    TopLevelPlacementComponents.Reset();
    LeafPlacementComponents.Reset();
    PlacementDependencies.Reset();
    LastPlacementWarnings.Reset();
    LastPlacementError.Reset();
    ResolvedSignature.Reset();
    ResolvedPlan.Reset();
    AppliedGraph.Reset();
    bPlanAvailable = false;
    bBasisRejected = false;
    BroadcastMHCompositeComponentsEdited();
}

void AMHCompositeActor::ReportPlacementError()
{
    if (LastPlacementError.IsEmpty()) return;
    const FString Diagnostic = CompositeAsset.ToSoftObjectPath().ToString() + TEXT(": ") + LastPlacementError;
    // Missing applied dependencies are a visible, recoverable placement state,
    // not an engine error. The machine-readable code remains in the message.
    UE_LOG(LogMHCompositeActor, Warning, TEXT("%s"), *Diagnostic);
    if (!IsRunningCommandlet()) FMessageLog(TEXT("Mimir")).Warning(FText::FromString(Diagnostic));
}

void AMHCompositeActor::RebuildComposite()
{
    RebuildPlacement(false);
}

void AMHCompositeActor::RebuildPlacement(const bool bSeedOnly)
{
    using namespace UE::MimirComposite;
    // Play/cook consumes a fresh applied-input snapshot through the runtime
    // wrapper. Never manufacture an editor preview in either handoff world.
    if (IsRunningCookCommandlet() ||
        (GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::PIE)) return;
    if (bRebuildInProgress || bPlacementEditMode || IsTemplate() || IsActorBeingDestroyed()) return;
    TGuardValue<bool> Guard(bRebuildInProgress, true);
    ++PlacementRebuildCount;
    // Instrumentation for the S6.2 lifecycle guard: placement components may
    // only be created once this actor's own components are already registered.
    if (!HasActorRegisteredAllComponents()) ++PlacementUnregisteredBuildCount;
    bBasisRejected = false;
    AttachRootTransformHook();
    if (CompositeAsset.ToSoftObjectPath().IsNull())
    {
        ClearDerivedComponents();
        return;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr) return;
    UMHCompositeAsset* Asset = GetCompositeAsset();
    const FString Name = Asset != nullptr ? Asset->LogicalName : CompositeAsset.ToSoftObjectPath().GetAssetName();
    FMHResourceKey RootKey;
    RootKey.Kind = EMHResourceKind::Composite;
    RootKey.LogicalName = Name;
    PlacementDependencies.Add(RootKey);

    TSharedPtr<const FMHRandomSourceGraph> CandidateGraph = AppliedGraph;
    FString Error;
    if (!bSeedOnly || !bPlanAvailable || !CandidateGraph.IsValid())
    {
        TSharedRef<FMHRandomSourceGraph> Graph = MakeShared<FMHRandomSourceGraph>();
        TSet<FMHResourceKey> Dependencies;
        if (Asset == nullptr)
            Error = TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: composite:") + Name + TEXT(" has no generated asset");
        else if (MHBuildAppliedCompositeGraph(*Asset, *Settings, *Graph, Dependencies, Error))
        {
            CandidateGraph = Graph;
            PlacementDependencies = MoveTemp(Dependencies);
        }
        else
        {
            // Retain previously known keys so restoring an unavailable branch
            // still notifies this actor, without inventing its missing payload.
            PlacementDependencies.Append(Dependencies);
        }
    }

    TSharedRef<FMHResolvedCompositePlan> CandidatePlan = MakeShared<FMHResolvedCompositePlan>();
    if (Error.IsEmpty() && CandidateGraph.IsValid())
    {
        if (bSeedOnly && bPlanAvailable && ResolvedPlan.IsValid() &&
            ResolvedPlan->Draws.IsEmpty() && ResolvedPlan->Decisions.IsEmpty())
        {
            *CandidatePlan = *ResolvedPlan;
            CandidatePlan->Seed = Seed;
            MHRefreshResolvedCompositeSignature(*CandidatePlan);
        }
        else if (!MHResolveCompositePlan(*CandidateGraph, Seed, *CandidatePlan, Error))
        {
            if (!Error.StartsWith(TEXT("MH_E_"))) Error = TEXT("MH_E_COMPOSITE_GRAMMAR: ") + Error;
        }
        if (Error.IsEmpty()) MHValidateResolvedPlacementTransforms(*CandidatePlan, GetActorTransform(), Error);
    }
    LastPlacementWarnings.Reset();
    LastPlacementError = Error;
    if (!Error.IsEmpty())
    {
        ResolvedSignature.Reset();
        bPlanAvailable = false;
        if (Error.Contains(TEXT("MH_E_UNREPRESENTABLE_TRANSFORM")))
        {
            // No mutation of either old or new components at this boundary.
            ReportPlacementError();
            return;
        }
        FMHCompositePlacementCompileResult View;
        if (ResolvedPlan.IsValid() && AppliedGraph.IsValid() && AppliedGraph->RootComposite == Name)
        {
            const FMHRandomComposite* PreviousRoot = AppliedGraph->Composites.Find(Name);
            if (PreviousRoot != nullptr)
                View = MHCompileCompositePlacementV5(*this, *ResolvedPlan, *PreviousRoot, *Settings, DerivedComponents);
        }
        FMHCompositePlacementCompileResult Marker = MHBuildCompositeDiagnosticView(*this, TEXT("composite:") + Name, Error);
        View.Components.Append(Marker.Components);
        View.Warnings.Append(Marker.Warnings);
        const TArray<TObjectPtr<UActorComponent>> Previous = MoveTemp(DerivedComponents);
        DerivedComponents = MoveTemp(View.Components);
        TopLevelPlacementComponents = MoveTemp(View.TopLevelComponents);
        LeafPlacementComponents = MoveTemp(View.LeafComponents);
        LastPlacementWarnings = MoveTemp(View.Warnings);
        DestroyMHRetiredComponents(Previous, DerivedComponents);
        BroadcastMHCompositeComponentsEdited();
        ReportPlacementError();
        return;
    }
    if (!CandidateGraph.IsValid()) return;
    const FMHRandomComposite* Root = CandidateGraph->Composites.Find(Name);
    if (Root == nullptr) return;
    SeedAffectsResult = MHClassifyCompositeGraph(*CandidateGraph);
    // None means visual invariance, not absence of random draws. Resolve above
    // still refreshes decision traces for single-option and zero-deviation nodes.
    if (!(bSeedOnly && bPlanAvailable && SeedAffectsResult == EMHCompositeSeedEffect::None))
    {
        FMHCompositePlacementCompileResult View = MHCompileCompositePlacementV5(*this, *CandidatePlan, *Root, *Settings, DerivedComponents);
        if (!View.Succeeded())
        {
            LastPlacementError = View.Error;
            bPlanAvailable = false;
            ResolvedSignature.Reset();
            ReportPlacementError();
            return;
        }
        const TArray<TObjectPtr<UActorComponent>> Previous = MoveTemp(DerivedComponents);
        DerivedComponents = MoveTemp(View.Components);
        TopLevelPlacementComponents = MoveTemp(View.TopLevelComponents);
        LeafPlacementComponents = MoveTemp(View.LeafComponents);
        LastPlacementWarnings = MoveTemp(View.Warnings);
        DestroyMHRetiredComponents(Previous, DerivedComponents);
        if (Previous != DerivedComponents) BroadcastMHCompositeComponentsEdited();
    }
    AppliedGraph = CandidateGraph;
    ResolvedPlan = CandidatePlan;
    ResolvedSignature = CandidatePlan->ResolvedSignature;
    bPlanAvailable = true;
}

void AMHCompositeActor::UpdatePlacementBasis(USceneComponent*, EUpdateTransformFlags, ETeleportType)
{
    using namespace UE::MimirComposite;
    if (bRebuildInProgress) return;
    if (bPlacementEditMode)
    {
        Tick(0.0f);
        return;
    }
    if (!ResolvedPlan.IsValid() ||
        !AppliedGraph.IsValid() || ResolvedPlan->Seed != Seed) return;
    if (!bPlanAvailable && !bBasisRejected)
    {
        // The cached plan may predate a rejected dependency update. Never let
        // moving the actor clear that failure using the older graph.
        RebuildComposite();
        return;
    }
    const FMHRandomComposite* Root = AppliedGraph->Composites.Find(AppliedGraph->RootComposite);
    if (Root == nullptr) return;
    bool bDesynchronized = false;
    {
        TGuardValue<bool> Guard(bRebuildInProgress, true);
        FString Error;
        if (!MHUpdateCompositePlacementBasis(*this, *ResolvedPlan, *Root, TopLevelPlacementComponents, LeafPlacementComponents, Error))
        {
            LastPlacementError = Error;
            ResolvedSignature.Reset();
            bPlanAvailable = false;
            bBasisRejected = true;
            ReportPlacementError();
            bDesynchronized = Error.StartsWith(TEXT("MH_E_PLACEMENT_STATE_DESYNC"));
        }
        else if (bBasisRejected)
        {
            LastPlacementError.Reset();
            ResolvedSignature = ResolvedPlan->ResolvedSignature;
            bPlanAvailable = true;
            bBasisRejected = false;
        }
    }
    if (!bDesynchronized) return;
    // A component view that no longer matches the plan is never repaired by a
    // partial walk over the shorter array. Rebuild the whole placement instead,
    // outside the reentrancy guard the basis update runs under.
    ++PlacementDesyncCount;
    RebuildComposite();
}

void AMHCompositeActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    AttachRootTransformHook();
    if (!ResolvedPlan.IsValid() && LastPlacementError.IsEmpty()) RebuildComposite();
    else UpdatePlacementBasis(nullptr, EUpdateTransformFlags::None, ETeleportType::None);
}

void AMHCompositeActor::PostActorCreated()
{
    Super::PostActorCreated();
    if (!IsTemplate() && bAutoSeed) Seed = GenerateAutoSeed();
    AttachRootTransformHook();
}

void AMHCompositeActor::PostLoad()
{
    Super::PostLoad();
    AttachRootTransformHook();
    // A loaded actor is not required to be in a world yet, so PostLoad cannot
    // create or register placement components. Record the need; the single
    // admitted first-build point below runs once the actor is registered.
    bNeedsInitialPlacementBuild = true;
}

void AMHCompositeActor::PostRegisterAllComponents()
{
    Super::PostRegisterAllComponents();
    // The one lifecycle point where the actor is in a world and its own
    // components are already registered. OnConstruction stays a basis update:
    // it runs on every PostEditMove and must never become a full rebuild.
    if (!bNeedsInitialPlacementBuild) return;
    bNeedsInitialPlacementBuild = false;
    RebuildComposite();
}

void AMHCompositeActor::PostDuplicate(const EDuplicateMode::Type DuplicateMode)
{
    Super::PostDuplicate(DuplicateMode);
    if (DuplicateMode == EDuplicateMode::Normal && !IsTemplate() && bAutoSeed) Seed = GenerateAutoSeed(Seed);
    AttachRootTransformHook();
    RebuildComposite();
}

void AMHCompositeActor::Destroyed()
{
    if (CompositeRoot != nullptr) CompositeRoot->TransformUpdated.RemoveAll(this);
    ClearDerivedComponents();
    Super::Destroyed();
}

void AMHCompositeActor::Tick(const float DeltaSeconds)
{
    using namespace UE::MimirComposite;
    Super::Tick(DeltaSeconds);
    if (!bPlacementEditMode || bRebuildInProgress || !EditingGraph.IsSet() || !EditingDocument.IsSet()) return;
    FMHRandomComposite* Root = EditingGraph->Composites.Find(EditingGraph->RootComposite);
    if (Root == nullptr || Root->Nodes.Num() != TopLevelPlacementComponents.Num() ||
        TopLevelPlacementComponents.ContainsByPredicate([](const USceneComponent* Handle) { return !IsValid(Handle); }))
    {
        LastPlacementError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: an authored Edit handle disappeared");
        bPlanAvailable = false;
        ResolvedSignature.Reset();
        return;
    }
    const FTransform CurrentBasis = GetActorTransform();
    bool bChanged = !CurrentBasis.Equals(LastEditBasis, 0.0);
    bool bAuthoredChanged = false;
    for (int32 Index = 0; Index < TopLevelPlacementComponents.Num(); ++Index)
    {
        USceneComponent* Handle = TopLevelPlacementComponents[Index];
        const FTransform Current = Handle->GetComponentTransform();
        if (LastEditHandleTransforms.IsValidIndex(Index) && Current.Equals(LastEditHandleTransforms[Index], 0.0)) continue;
        // Absolute handles still live in the previously admitted basis when
        // the actor moves. A placement move must not become a source edit.
        const FMatrix LocalMatrix = Current.ToMatrixWithScale() * LastEditBasis.ToInverseMatrixWithScale();
        if (!MHIsRepresentableTransformMatrix(LocalMatrix))
        {
            LastPlacementError = TEXT("MH_E_UNREPRESENTABLE_TRANSFORM: edited top-level handle");
            bPlanAvailable = false;
            ResolvedSignature.Reset();
            return;
        }
        const FTransform Local(LocalMatrix);
        Root->Nodes[Index].Transform.TranslationCm = FVector3f(Local.GetTranslation());
        Root->Nodes[Index].Transform.RotationQuat = FQuat4f(Local.GetRotation());
        Root->Nodes[Index].Transform.Scale = FVector3f(Local.GetScale3D());
        EditingDocument->Nodes[Index].Transform.TranslationCm = Local.GetTranslation();
        EditingDocument->Nodes[Index].Transform.RotationQuat = Local.GetRotation();
        EditingDocument->Nodes[Index].Transform.Scale = Local.GetScale3D();
        bAuthoredChanged = true;
        bChanged = true;
    }
    if (!bChanged) return;
    TGuardValue<bool> Guard(bRebuildInProgress, true);
    TSharedRef<FMHResolvedCompositePlan> Plan = MakeShared<FMHResolvedCompositePlan>();
    FString Error;
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (bAuthoredChanged)
    {
        TArray<uint8> ProspectiveBytes;
        if (!MHWriteCanonicalCompositeV5(*EditingDocument, ProspectiveBytes, Error))
        {
            LastPlacementError = Error;
            ResolvedSignature.Reset();
            bPlanAvailable = false;
            return;
        }
        // Hash the exact prospective source bytes, without changing the live
        // asset/receipt. A pure basis move keeps the original applied raw hash.
        EditingGraph->RawHashes.Add(TEXT("composite:") + EditingGraph->RootComposite, MHRawPayloadHash(ProspectiveBytes));
    }
    if (!MHResolveCompositePlan(*EditingGraph, Seed, *Plan, Error) ||
        !MHValidateResolvedPlacementTransforms(*Plan, GetActorTransform(), Error))
    {
        LastPlacementError = Error;
        ResolvedSignature.Reset();
        bPlanAvailable = false;
        return;
    }
    FMHCompositePlacementCompileResult View = MHCompileCompositePlacementV5(*this, *Plan, *Root, *Settings, DerivedComponents);
    if (!View.Succeeded())
    {
        LastPlacementError = View.Error;
        bPlanAvailable = false;
        ResolvedSignature.Reset();
        return;
    }
    const TArray<TObjectPtr<UActorComponent>> Previous = MoveTemp(DerivedComponents);
    DerivedComponents = MoveTemp(View.Components);
    TopLevelPlacementComponents = MoveTemp(View.TopLevelComponents);
    LeafPlacementComponents = MoveTemp(View.LeafComponents);
    DestroyMHRetiredComponents(Previous, DerivedComponents);
    LastEditHandleTransforms.Reset();
    for (const USceneComponent* Handle : TopLevelPlacementComponents) LastEditHandleTransforms.Add(Handle->GetComponentTransform());
    LastEditBasis = CurrentBasis;
    ResolvedPlan = Plan;
    ResolvedSignature = Plan->ResolvedSignature;
    LastPlacementError.Reset();
    bPlanAvailable = true;
}

#if WITH_EDITOR
void AMHCompositeActor::PostEditUndo()
{
    Super::PostEditUndo();
    RebuildComposite();
}

void AMHCompositeActor::PostEditImport()
{
    Super::PostEditImport();
    if (!IsTemplate() && bAutoSeed) Seed = GenerateAutoSeed(Seed);
    AttachRootTransformHook();
    RebuildComposite();
}

bool AMHCompositeActor::CanEditChange(const FProperty* Property) const
{
    if (bPlacementEditMode && Property != nullptr &&
        (Property->GetFName() == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, Seed) ||
         Property->GetFName() == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, bAutoSeed))) return false;
    return Super::CanEditChange(Property);
}

void AMHCompositeActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);
    const FName Name = PropertyChangedEvent.GetPropertyName();
    if (Name == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, CompositeAsset)) RebuildComposite();
    else if (Name == GET_MEMBER_NAME_CHECKED(AMHCompositeActor, Seed)) RebuildPlacement(true);
}
#endif
