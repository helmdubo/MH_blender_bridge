#include "Source/MHSourceImporter.h"

#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Composite/MHCompositeImporter.h"
#include "Engine/StaticMesh.h"
#include "Logging/MessageLog.h"
#include "MessageLogModule.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialAdoptDialog.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"
#include "StaticMesh/MHStaticMeshImportData.h"
#include "StaticMesh/MHStaticMeshImporter.h"
#include "UObject/UObjectGlobals.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHSourceImporter)

namespace UE::MimirComposite
{

void MHFilterAnalysisToScope(
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& InOutAnalysis)
{
    if (Scope.ResourceKeys.IsEmpty())
    {
        return;
    }

    TSet<FMHResourceKey> Analyzed;
    for (const FMHSourceAnalysisEntry& Entry : InOutAnalysis.Entries)
    {
        Analyzed.Add(Entry.Key);
    }

    TSet<FMHResourceKey> Included;
    TArray<FMHResourceKey> Invalid;
    for (const FMHResourceKey& Key : Scope.ResourceKeys)
    {
        if (Key.IsCanonical())
        {
            Included.Add(Key);
        }
        else
        {
            Invalid.Add(Key);
        }
    }
    InOutAnalysis.Entries.RemoveAll(
        [&Included](const FMHSourceAnalysisEntry& Entry)
        {
            return !Included.Contains(Entry.Key);
        });

    for (const FMHResourceKey& Key : Included)
    {
        if (Analyzed.Contains(Key))
        {
            continue;
        }

        FMHSourceAnalysisEntry& Missing = InOutAnalysis.Entries.AddDefaulted_GetRef();
        Missing.Key = Key;
        Missing.Change = EMHSourceChange::Blocked;
        Missing.Errors.Add(FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: requested scope key %s was not found in the source snapshot or applied state"),
            *Key.ToString()));
    }
    for (const FMHResourceKey& Key : Invalid)
    {
        FMHSourceAnalysisEntry& Rejected = InOutAnalysis.Entries.AddDefaulted_GetRef();
        Rejected.Key = Key;
        Rejected.Change = EMHSourceChange::Blocked;
        Rejected.Errors.Add(FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: requested scope key is not canonical: %s"),
            *Key.ToString()));
    }
    InOutAnalysis.Entries.Sort([](
        const FMHSourceAnalysisEntry& A,
        const FMHSourceAnalysisEntry& B)
    {
        if (A.Key.Kind != B.Key.Kind)
        {
            return static_cast<uint8>(A.Key.Kind) < static_cast<uint8>(B.Key.Kind);
        }
        return A.Key.LogicalName < B.Key.LogicalName;
    });
}

bool MHBuildSourceImportPlan(
    IMHChangeDetector& ChangeDetector,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& OutAnalysis,
    bool& bOutExecuted)
{
    OutAnalysis = FMHSourceAnalysis();
    bOutExecuted = false;
    MHAnalyzeSources(ChangeDetector, Resolver, SourceRoot, OutAnalysis);
    MHFilterAnalysisToScope(Scope, OutAnalysis);
    return !OutAnalysis.HasErrors();
}

} // namespace UE::MimirComposite

using namespace UE::MimirComposite;

void UMHSourceImporter::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    if (IsRunningCommandlet())
    {
        return;
    }

    IAssetRegistry& AssetRegistry =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    if (AssetRegistry.IsLoadingAssets())
    {
        FilesLoadedHandle = AssetRegistry.OnFilesLoaded().AddUObject(
            this,
            &UMHSourceImporter::OnAssetRegistryFilesLoaded);
    }
    else
    {
        OnAssetRegistryFilesLoaded();
    }
}

void UMHSourceImporter::Deinitialize()
{
    if (FilesLoadedHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
    {
        FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get()
            .OnFilesLoaded()
            .Remove(FilesLoadedHandle);
        FilesLoadedHandle.Reset();
    }
    Super::Deinitialize();
}

void UMHSourceImporter::OnAssetRegistryFilesLoaded()
{
    if (FilesLoadedHandle.IsValid())
    {
        FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
            .Get()
            .OnFilesLoaded()
            .Remove(FilesLoadedHandle);
        FilesLoadedHandle.Reset();
    }
    TWeakObjectPtr<UMHSourceImporter> WeakThis(this);
    AsyncTask(ENamedThreads::GameThread, [WeakThis]()
    {
        if (WeakThis.IsValid())
        {
            WeakThis->RunStartupPlan();
        }
    });
}

void UMHSourceImporter::RunStartupPlan()
{
    if (bStartupPlanRan)
    {
        return;
    }
    bStartupPlanRan = true;

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr || Settings->GetSourceRootPath().IsEmpty())
    {
        return;
    }

    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    ImportSources(FMHImportSourcesScope::All(), Analysis, bExecuted);
}

bool UMHSourceImporter::ImportSources(
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& OutAnalysis,
    bool& bOutExecuted)
{
    OutAnalysis = FMHSourceAnalysis();
    bOutExecuted = false;

    if (!IsInGameThread())
    {
        OutAnalysis.Errors.Add(TEXT("MH_E_IMPORT_THREAD_INVALID: ImportSources must run on the game thread"));
        return false;
    }

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (SourceRoot.IsEmpty())
    {
        OutAnalysis.Errors.Add(TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured"));
        PresentPlan(OutAnalysis);
        return false;
    }

    FMHSourceAnalysisServices Services;
    FString CompositionError;
    if (!MHCreateDefaultSourceAnalysisServices(
            SourceRoot,
            Services,
            CompositionError))
    {
        OutAnalysis.Errors.Add(CompositionError);
        PresentPlan(OutAnalysis);
        return false;
    }

    MHBuildSourceImportPlan(
        *Services.ChangeDetector,
        *Services.Resolver,
        SourceRoot,
        Scope,
        OutAnalysis,
        bOutExecuted);
    for (FMHSourceAnalysisEntry& Entry : OutAnalysis.Entries)
    {
        if (Entry.Key.Kind != EMHResourceKind::Material || !Entry.Errors.IsEmpty() ||
            !(Entry.Change == EMHSourceChange::Create ||
              Entry.Change == EMHSourceChange::Reimport ||
              Entry.Change == EMHSourceChange::Move))
        {
            continue;
        }
        FMHMaterialOperationResult MaterialResult = MHImportMaterialV4(
            Entry,
            *Services.Resolver,
            SourceRoot,
            *Settings);
        Entry.Warnings.Append(MaterialResult.Warnings);
        if (!MaterialResult.Succeeded())
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(MaterialResult.Error);
        }
        else
        {
            bOutExecuted = true;
        }
    }
    // ImporterVersion is persisted in the receipt rather than the exact-six
    // registry projection, so the coordinator promotes equal-hash meshes here,
    // outside the no-UObject-load project-index scan.
    for (FMHSourceAnalysisEntry& Entry : OutAnalysis.Entries)
    {
        if (Entry.Key.Kind != EMHResourceKind::StaticMesh || !Entry.Errors.IsEmpty())
        {
            continue;
        }
        if (Entry.Change == EMHSourceChange::NoChange)
        {
            const FString PackageName = FString(TEXT("/Game/MH/Generated/Meshes/")) + Entry.Key.LogicalName;
            const FString ObjectPath = PackageName + TEXT(".") + Entry.Key.LogicalName;
            if (const UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath))
            {
                const UMHStaticMeshImportData* Data = Cast<UMHStaticMeshImportData>(Mesh->GetAssetImportData());
                if (Data != nullptr && Data->ImporterVersion != MHStaticMeshImporterVersion)
                {
                    Entry.Change = EMHSourceChange::Reimport;
                }
            }
        }
        if (!(Entry.Change == EMHSourceChange::Create ||
              Entry.Change == EMHSourceChange::Reimport ||
              Entry.Change == EMHSourceChange::Move))
        {
            continue;
        }
        FMHStaticMeshOperationResult MeshResult = MHImportStaticMeshV4(
            Entry,
            *Services.Resolver,
            SourceRoot);
        Entry.Warnings.Append(MeshResult.Warnings);
        if (!MeshResult.Succeeded())
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(MeshResult.Error);
        }
        else
        {
            bOutExecuted |= MeshResult.bRebuilt || MeshResult.bReceiptUpdated;
        }
    }
    // Composite closure is resolved only after material and mesh execution.
    for (FMHSourceAnalysisEntry& Entry : OutAnalysis.Entries)
    {
        if (Entry.Key.Kind != EMHResourceKind::Composite || !Entry.Errors.IsEmpty() ||
            !(Entry.Change == EMHSourceChange::Create ||
              Entry.Change == EMHSourceChange::Reimport ||
              Entry.Change == EMHSourceChange::Move))
        {
            continue;
        }
        FMHCompositeOperationResult CompositeResult = MHImportCompositeV4(
            Entry,
            *Services.Resolver,
            SourceRoot,
            *Settings);
        Entry.Warnings.Append(CompositeResult.Warnings);
        if (!CompositeResult.Succeeded())
        {
            Entry.Change = EMHSourceChange::Blocked;
            Entry.Errors.Add(CompositeResult.Error);
        }
        else
        {
            bOutExecuted = true;
        }
    }
    PresentPlan(OutAnalysis);
    return !OutAnalysis.HasErrors();
}

bool UMHSourceImporter::ReimportStaticMesh(
    UStaticMesh* StaticMesh,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || StaticMesh == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: ReimportStaticMesh requires a mesh on the game thread");
        return false;
    }
    const UMHStaticMeshImportData* Data = Cast<UMHStaticMeshImportData>(StaticMesh->GetAssetImportData());
    if (Data == nullptr || Data->LogicalName.IsEmpty() || Data->SourceRelativePath.IsEmpty())
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: static mesh has no v4 source receipt");
        return false;
    }
    FMHResourceKey MeshKey;
    MeshKey.Kind = EMHResourceKind::StaticMesh;
    MeshKey.LogicalName = Data->LogicalName;
    if (!MeshKey.IsCanonical())
    {
        OutError = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: managed mesh receipt has a noncanonical logical name");
        return false;
    }
    const FString ExpectedPackageName = FString(TEXT("/Game/MH/Generated/Meshes/")) + MeshKey.LogicalName;
    const FString ExpectedObjectPath = ExpectedPackageName + TEXT(".") + MeshKey.LogicalName;
    if (StaticMesh->GetPathName() != ExpectedObjectPath ||
        StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *ExpectedObjectPath) != StaticMesh)
    {
        OutError = TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: explicit reimport target is not the canonical managed mesh UObject");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (SourceRoot.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }

    FMHSourceAnalysisServices Services;
    if (!MHCreateDefaultSourceAnalysisServices(SourceRoot, Services, OutError))
    {
        return false;
    }
    FMHSourceAnalysisEntry Entry;
    Entry.Key = MoveTemp(MeshKey);
    const FMHResolveOutcome Outcome = Services.Resolver->Resolve(Entry.Key);
    if (Outcome.Status != EMHResolveStatus::Resolved)
    {
        OutError = Outcome.Diagnostic.IsEmpty()
            ? TEXT("MH_E_INVALID_RESOURCE_SOURCE: managed mesh source does not resolve")
            : Outcome.Diagnostic;
        return false;
    }
    Entry.PayloadPath = Outcome.PayloadPath;
    FString RelativeBase = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(RelativeBase);
    RelativeBase += TEXT("/");
    Entry.SourcePath = Outcome.PayloadPath;
    if (!FPaths::MakePathRelativeTo(Entry.SourcePath, *RelativeBase))
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: cannot derive the current mesh source path");
        return false;
    }
    FPaths::NormalizeFilename(Entry.SourcePath);
    Entry.RawHash = Outcome.RawHash;
    Entry.Change = EMHSourceChange::Reimport;
    FMHStaticMeshOperationResult Result = MHImportStaticMeshV4(
        Entry,
        *Services.Resolver,
        SourceRoot,
        true);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    if (!Result.Succeeded())
    {
        return false;
    }
    if (Result.StaticMesh != StaticMesh)
    {
        OutError = TEXT("MH_E_AMBIGUOUS_GENERATED_ASSET: explicit reimport resolved a different managed UObject");
        return false;
    }
    return Result.bRebuilt;
}

bool UMHSourceImporter::PublishMaterial(
    UMaterialInstanceConstant* Material,
    const FString& AdoptFolder,
    const FString& AdoptLogicalName,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || Material == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: PublishMaterial requires a material on the game thread");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr || Settings->GetSourceRootPath().IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }
    FMHMaterialAdoptTarget Adopt;
    const FMHMaterialAdoptTarget* AdoptPtr = nullptr;
    if (!AdoptFolder.IsEmpty() || !AdoptLogicalName.IsEmpty())
    {
        Adopt.Folder = AdoptFolder;
        Adopt.LogicalName = AdoptLogicalName;
        AdoptPtr = &Adopt;
    }
    FMHMaterialOperationResult Result = MHPublishMaterialV4(
        *Material,
        Settings->GetSourceRootPath(),
        *Settings,
        AdoptPtr);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    return Result.Succeeded();
}

bool UMHSourceImporter::PublishMaterialInteractive(
    UMaterialInstanceConstant* Material,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || Material == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: PublishMaterialInteractive requires a material on the game thread");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr || Settings->GetSourceRootPath().IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }
    const UMHMaterialSourceData* Data = Cast<UMHMaterialSourceData>(
        Material->GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
    FMHMaterialOperationResult Result = Data != nullptr && !Data->SourceRelativePath.IsEmpty()
        ? MHPublishMaterialV4(*Material, Settings->GetSourceRootPath(), *Settings)
        : MHShowMaterialAdoptDialog(*Material, Settings->GetSourceRootPath(), *Settings);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    return Result.Succeeded();
}

bool UMHSourceImporter::PublishComposite(
    UMHCompositeAsset* Asset,
    const FString& AdoptFolder,
    const FString& AdoptLogicalName,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!IsInGameThread() || Asset == nullptr)
    {
        OutError = TEXT("MH_E_IMPORT_THREAD_INVALID: PublishComposite requires an asset on the game thread");
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings == nullptr || Settings->GetSourceRootPath().IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: source_root is not configured");
        return false;
    }
    FMHCompositeAdoptTarget Adopt;
    const FMHCompositeAdoptTarget* AdoptPtr = nullptr;
    if (!AdoptFolder.IsEmpty() || !AdoptLogicalName.IsEmpty())
    {
        Adopt.Folder = AdoptFolder;
        Adopt.LogicalName = AdoptLogicalName;
        AdoptPtr = &Adopt;
    }
    FMHCompositeOperationResult Result = MHPublishCompositeV4(
        *Asset, Settings->GetSourceRootPath(), AdoptPtr);
    OutWarnings = MoveTemp(Result.Warnings);
    OutError = MoveTemp(Result.Error);
    return Result.Succeeded();
}

void UMHSourceImporter::PresentPlan(const FMHSourceAnalysis& Analysis) const
{
    FMessageLog Log(TEXT("Mimir"));
    Log.NewPage(INVTEXT("Source startup plan"));

    for (const FString& Warning : Analysis.Warnings)
    {
        Log.Warning(FText::FromString(Warning));
    }
    for (const FString& Error : Analysis.Errors)
    {
        Log.Error(FText::FromString(Error));
    }
    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
    {
        const FString Line = FString::Printf(
            TEXT("%s  %s  %s"),
            MHSourceChangeLabel(Entry.Change),
            *Entry.Key.ToString(),
            Entry.SourcePath.IsEmpty() ? TEXT("-") : *Entry.SourcePath);
        if (Entry.Errors.IsEmpty())
        {
            Log.Info(FText::FromString(Line));
        }
        else
        {
            Log.Error(FText::FromString(Line));
        }
        for (const FString& Warning : Entry.Warnings)
        {
            Log.Warning(FText::FromString(Warning));
        }
        for (const FString& Error : Entry.Errors)
        {
            Log.Error(FText::FromString(Error));
        }
    }

    const FString Summary = FString::Printf(
        TEXT("Mimir source pass: %d resources, %d blocked. S3 executes material and composite entries; mesh remains plan-only."),
        Analysis.Entries.Num(),
        Analysis.CountOf(EMHSourceChange::Blocked));
    Log.Info(FText::FromString(Summary));

    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    if (Settings != nullptr && Settings->StartupScanMode == EMHStartupScanMode::Prompt)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Summary));
        Log.Notify(INVTEXT("Mimir source plan is ready"));
    }
}
