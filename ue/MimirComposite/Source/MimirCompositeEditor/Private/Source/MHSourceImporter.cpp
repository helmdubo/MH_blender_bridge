#include "Source/MHSourceImporter.h"

#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Codec/MHCompositeCodec.h"
#include "Logging/MessageLog.h"
#include "MessageLogModule.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceComposition.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHSourceImporter)

namespace UE::MimirComposite
{

void MHFilterAnalysisToScope(
    const FMHImportSourcesScope& Scope,
    FMHSourceAnalysis& InOutAnalysis)
{
    if (Scope.ResourceUids.IsEmpty())
    {
        return;
    }

    TSet<FString> Analyzed;
    for (const FMHSourceAnalysisEntry& Entry : InOutAnalysis.Entries)
    {
        Analyzed.Add(Entry.ResourceUid);
    }

    TSet<FString> Included;
    TSet<FString> Invalid;
    for (const FString& Uid : Scope.ResourceUids)
    {
        if (MHIsCanonicalUuid(Uid))
        {
            Included.Add(Uid);
        }
        else
        {
            Invalid.Add(Uid);
        }
    }
    InOutAnalysis.Entries.RemoveAll(
        [&Included](const FMHSourceAnalysisEntry& Entry)
        {
            return !Included.Contains(Entry.ResourceUid);
        });

    for (const FString& Uid : Included)
    {
        if (Analyzed.Contains(Uid))
        {
            continue;
        }

        FMHSourceAnalysisEntry& Missing = InOutAnalysis.Entries.AddDefaulted_GetRef();
        Missing.ResourceUid = Uid;
        Missing.Change = EMHSourceChange::Blocked;
        Missing.bLedgerAdvanceAllowed = false;
        Missing.Errors.Add(FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: requested scope UID %s was not found in the source snapshot or applied state"),
            *Uid));
    }
    for (const FString& Uid : Invalid)
    {
        FMHSourceAnalysisEntry& Rejected = InOutAnalysis.Entries.AddDefaulted_GetRef();
        Rejected.ResourceUid = Uid;
        Rejected.Change = EMHSourceChange::Blocked;
        Rejected.bLedgerAdvanceAllowed = false;
        Rejected.Errors.Add(FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: requested scope UID is not canonical: %s"),
            *Uid));
    }
    InOutAnalysis.Entries.Sort([](
        const FMHSourceAnalysisEntry& A,
        const FMHSourceAnalysisEntry& B)
    {
        return A.ResourceUid < B.ResourceUid;
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
            Settings != nullptr ? Settings->ContentRoot : FString(),
            Services,
            CompositionError))
    {
        OutAnalysis.Errors.Add(CompositionError);
        PresentPlan(OutAnalysis);
        return false;
    }

    const bool bPlanSucceeded = MHBuildSourceImportPlan(
        *Services.ChangeDetector,
        *Services.Resolver,
        SourceRoot,
        Scope,
        OutAnalysis,
        bOutExecuted);
    PresentPlan(OutAnalysis);

    // TODO(QUESTION-17): C1 deliberately stops after Plan presentation. C2
    // supplies builders and the successful-operation-only Ledger commit.
    return bPlanSucceeded;
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
            *Entry.ResourceUid,
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
        TEXT("Mimir startup plan: %d resources, %d blocked. C1 is Analyze/Plan-only; no assets or Ledger rows were changed."),
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
