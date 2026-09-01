#include "Diagnostics/MHSourceOperations.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Engine/StaticMesh.h"
#include "Index/MHProjectResourceIndex.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceComposition.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "UObject/SoftObjectPath.h"

namespace UE::MimirComposite
{
namespace
{

bool IsNameDiagnosticCode(const FString& Code)
{
    return Code.Contains(TEXT("NAME"), ESearchCase::CaseSensitive) ||
        Code == TEXT("MH_E_INVALID_NODE_MARKERS") ||
        Code == TEXT("MH_E_INVALID_LOD_HIERARCHY");
}

void AddDiagnosticToAnalysis(
    const FMHProjectIndexDiagnostic& Diagnostic,
    FMHSourceAnalysis& Analysis)
{
    FMHResourceKey Owner;
    Owner.LogicalName = Diagnostic.OwnerName;
    const bool bHasOwner =
        MHResourceKindFromLabel(Diagnostic.OwnerKind, Owner.Kind) &&
        Owner.IsCanonical();
    if (!bHasOwner)
    {
        (Diagnostic.Severity == EMHProjectDiagnosticSeverity::Warning
            ? Analysis.Warnings
            : Analysis.Errors).Add(Diagnostic.Message);
        return;
    }

    FMHSourceAnalysisEntry* Entry = Analysis.Entries.FindByPredicate(
        [&Owner](const FMHSourceAnalysisEntry& Candidate)
        {
            return Candidate.Key == Owner;
        });
    if (Entry == nullptr)
    {
        Entry = &Analysis.Entries.AddDefaulted_GetRef();
        Entry->Key = Owner;
        Entry->Change = EMHSourceChange::Blocked;
        Entry->SourcePath = Diagnostic.Path;
    }
    (Diagnostic.Severity == EMHProjectDiagnosticSeverity::Warning
        ? Entry->Warnings
        : Entry->Errors).Add(Diagnostic.Message);
}

bool CreateServices(
    const FString& SourceRoot,
    FMHSourceAnalysisServices& OutServices,
    FMHSourceAnalysis& OutAnalysis,
    FString& OutError)
{
    OutAnalysis = FMHSourceAnalysis();
    OutError.Reset();
    if (SourceRoot.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        OutAnalysis.Errors.Add(OutError);
        return false;
    }
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot, OutServices, OutError))
    {
        OutAnalysis.Errors.Add(OutError);
        return false;
    }
    return true;
}

FMHSourceAnalysisEntry& AddClaimEntry(
    const FMHProjectIndexGeneratedAssetState& State,
    const EMHResourceKind ExpectedKind,
    FMHSourceAnalysis& Analysis)
{
    FMHSourceAnalysisEntry& Entry = Analysis.Entries.AddDefaulted_GetRef();
    Entry.Key.Kind = ExpectedKind;
    Entry.Key.LogicalName = State.LogicalName;
    Entry.SourcePath = State.UEObjectPath;
    Entry.RawHash = State.SourceHash;
    Entry.Change = EMHSourceChange::NoChange;
    return Entry;
}

bool ClaimIsStructurallyVerifiable(
    const FMHProjectIndexGeneratedAssetState& State,
    FMHSourceAnalysisEntry& Entry)
{
    if (!State.bKeyValid || !State.bReceiptValid ||
        State.Status == EMHGeneratedAssetStatus::InvalidReceipt ||
        State.Status == EMHGeneratedAssetStatus::DuplicateClaim)
    {
        Entry.Change = EMHSourceChange::Blocked;
        Entry.Errors.Add(FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: managed asset claim is %s: %s"),
            MHGeneratedAssetStatusLabel(State.Status),
            *State.UEObjectPath));
        return false;
    }
    return true;
}

bool AppendManagedMeshAudit(
    const TArray<FMHProjectIndexGeneratedAssetState>& States,
    const bool bStrict,
    FMHSourceAnalysis& Analysis)
{
    bool bInfrastructureOk = true;
    for (const FMHProjectIndexGeneratedAssetState& State : States)
    {
        if (State.KindLabel != TEXT("static_mesh"))
        {
            continue;
        }
        FMHSourceAnalysisEntry& Entry = AddClaimEntry(
            State,
            EMHResourceKind::StaticMesh,
            Analysis);
        if (!ClaimIsStructurallyVerifiable(State, Entry))
        {
            continue;
        }
        UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(State.UEObjectPath).TryLoad());
        if (Mesh == nullptr)
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: cannot load managed static mesh: %s"),
                *State.UEObjectPath));
            bInfrastructureOk = false;
            continue;
        }
        MHVerifyManagedStaticMeshLocalEdit(*Mesh, bStrict, Entry);
    }
    return bInfrastructureOk;
}

void SortAnalysis(FMHSourceAnalysis& Analysis)
{
    Analysis.Entries.Sort([](
        const FMHSourceAnalysisEntry& A,
        const FMHSourceAnalysisEntry& B)
    {
        if (A.Key.Kind != B.Key.Kind)
        {
            return static_cast<uint8>(A.Key.Kind) < static_cast<uint8>(B.Key.Kind);
        }
        if (A.Key.LogicalName != B.Key.LogicalName)
        {
            return A.Key.LogicalName < B.Key.LogicalName;
        }
        return A.SourcePath < B.SourcePath;
    });
}

} // namespace

bool MHScanSourcesOperation(
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis,
    FString& OutError,
    const EMHPerfScanTrigger Trigger)
{
    FMHSourceScanPerfScope PerfScope(Trigger);
    FMHSourceAnalysisServices Services;
    if (!CreateServices(SourceRoot, Services, OutAnalysis, OutError))
    {
        return false;
    }
    MHAnalyzeSources(
        *Services.ChangeDetector,
        *Services.Resolver,
        SourceRoot,
        OutAnalysis);
    return true;
}

bool MHValidateNamesOperation(
    const FString& SourceRoot,
    FMHSourceAnalysis& OutAnalysis,
    FString& OutError)
{
    FMHSourceAnalysisServices Services;
    if (!CreateServices(SourceRoot, Services, OutAnalysis, OutError))
    {
        return false;
    }
    TArray<FMHProjectIndexDiagnostic> Diagnostics;
    if (!Services.Index->GetDiagnostics(Diagnostics, OutError))
    {
        OutAnalysis.Errors.Add(OutError);
        return false;
    }
    for (const FMHProjectIndexDiagnostic& Diagnostic : Diagnostics)
    {
        if (IsNameDiagnosticCode(Diagnostic.Code))
        {
            AddDiagnosticToAnalysis(Diagnostic, OutAnalysis);
        }
    }
    SortAnalysis(OutAnalysis);
    return true;
}

void MHVerifyManagedStaticMeshLocalEdit(
    const UStaticMesh& StaticMesh,
    const bool bStrict,
    FMHSourceAnalysisEntry& InOutEntry)
{
    const UMHStaticMeshImportData* Data = Cast<UMHStaticMeshImportData>(
        StaticMesh.GetAssetImportData());
    if (Data == nullptr)
    {
        InOutEntry.Change = EMHSourceChange::Blocked;
        InOutEntry.Errors.Add(FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: managed static mesh has no v4 receipt: %s"),
            *StaticMesh.GetPathName()));
        return;
    }
    if (!Data->bLocallyModified)
    {
        return;
    }
    const FString Diagnostic = FString::Printf(
        TEXT("MH_W_MANAGED_STATIC_MESH_LOCALLY_MODIFIED: %s"),
        *StaticMesh.GetPathName());
    if (bStrict)
    {
        InOutEntry.Change = EMHSourceChange::Blocked;
        InOutEntry.Errors.Add(Diagnostic);
    }
    else
    {
        InOutEntry.Warnings.Add(Diagnostic);
    }
}

bool MHVerifyMaterialsOperation(
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings,
    const bool bStrict,
    FMHSourceAnalysis& OutAnalysis,
    FString& OutError)
{
    FMHSourceAnalysisServices Services;
    if (!CreateServices(SourceRoot, Services, OutAnalysis, OutError))
    {
        return false;
    }
    TArray<FMHProjectIndexGeneratedAssetState> States;
    if (!Services.Index->GetAllGeneratedAssets(States, OutError))
    {
        OutAnalysis.Errors.Add(OutError);
        return false;
    }
    for (const FMHProjectIndexGeneratedAssetState& State : States)
    {
        if (State.KindLabel != TEXT("material"))
        {
            continue;
        }
        FMHSourceAnalysisEntry& Entry = AddClaimEntry(
            State,
            EMHResourceKind::Material,
            OutAnalysis);
        if (!ClaimIsStructurallyVerifiable(State, Entry))
        {
            continue;
        }
        UMaterialInstanceConstant* Material = Cast<UMaterialInstanceConstant>(
            FSoftObjectPath(State.UEObjectPath).TryLoad());
        if (Material == nullptr)
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: cannot load managed material: %s"),
                *State.UEObjectPath));
            continue;
        }
        const UMHMaterialSourceData* Data = Cast<UMHMaterialSourceData>(
            Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
        if (Data == nullptr || Data->LogicalName != State.LogicalName ||
            Data->SourceRelativePath != State.SourcePath ||
            Data->SourceHash != State.SourceHash ||
            Data->AppliedHash != State.AppliedHash ||
            !MHIsCanonicalRawPayloadHash(Data->SourceHash) ||
            !MHIsCanonicalRawPayloadHash(Data->AppliedHash))
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: managed material receipt disagrees with its six-tag projection: %s"),
                *State.UEObjectPath));
            continue;
        }
        FString Warning;
        if (MHDetectManagedMaterialLocalModification(*Material, Settings, Warning))
        {
            Entry.Warnings.Add(MoveTemp(Warning));
        }
    }
    AppendManagedMeshAudit(States, bStrict, OutAnalysis);
    SortAnalysis(OutAnalysis);
    return true;
}

bool MHVerifyCompositesOperation(
    const FString& SourceRoot,
    const bool bStrict,
    FMHSourceAnalysis& OutAnalysis,
    FString& OutError)
{
    FMHSourceAnalysisServices Services;
    if (!CreateServices(SourceRoot, Services, OutAnalysis, OutError))
    {
        return false;
    }
    TArray<FMHProjectIndexGeneratedAssetState> States;
    if (!Services.Index->GetAllGeneratedAssets(States, OutError))
    {
        OutAnalysis.Errors.Add(OutError);
        return false;
    }
    for (const FMHProjectIndexGeneratedAssetState& State : States)
    {
        if (State.KindLabel != TEXT("composite"))
        {
            continue;
        }
        FMHSourceAnalysisEntry& Entry = AddClaimEntry(
            State,
            EMHResourceKind::Composite,
            OutAnalysis);
        if (!ClaimIsStructurallyVerifiable(State, Entry))
        {
            continue;
        }
        UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(
            FSoftObjectPath(State.UEObjectPath).TryLoad());
        if (Asset == nullptr)
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: cannot load managed composite: %s"),
                *State.UEObjectPath));
            continue;
        }
        if (Asset->LogicalName != State.LogicalName ||
            Asset->SourceRelativePath != State.SourcePath ||
            Asset->SourceHash != State.SourceHash ||
            Asset->AppliedHash != State.AppliedHash ||
            !MHIsCanonicalRawPayloadHash(Asset->SourceHash) ||
            !MHIsCanonicalRawPayloadHash(Asset->AppliedHash))
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: managed composite receipt disagrees with its six-tag projection: %s"),
                *State.UEObjectPath));
            continue;
        }
        FString Warning;
        if (MHDetectManagedCompositeLocalModification(*Asset, Warning))
        {
            Entry.Warnings.Add(MoveTemp(Warning));
        }
    }
    AppendManagedMeshAudit(States, bStrict, OutAnalysis);
    SortAnalysis(OutAnalysis);
    return true;
}

int32 MHSourceCommandletExitCode(
    const bool bUsageValid,
    const bool bOperationSucceeded,
    const FMHSourceAnalysis& Analysis)
{
    if (!bUsageValid)
    {
        return 2;
    }
    return bOperationSucceeded && !Analysis.HasErrors() ? 0 : 1;
}

} // namespace UE::MimirComposite
