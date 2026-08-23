#include "Diagnostics/MHAnalyzeSourcesCommandlet.h"

#include "Ledger/MHImportLedger.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceComposition.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHAnalyzeSourcesCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogMHAnalyzeSources, Display, All);

using namespace UE::MimirComposite;

UMHAnalyzeSourcesCommandlet::UMHAnalyzeSourcesCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
    ShowErrorCount = true;
    UseCommandletResultAsExitCode = true;
}

int32 UMHAnalyzeSourcesCommandlet::Main(const FString& Params)
{
    FString SourceRoot;
    FParse::Value(*Params, TEXT("root="), SourceRoot);
    if (SourceRoot.IsEmpty())
    {
        SourceRoot = GetDefault<UMHCompositeSettings>()->GetSourceRootPath();
    }
    if (SourceRoot.IsEmpty())
    {
        UE_LOG(
            LogMHAnalyzeSources,
            Error,
            TEXT("Usage: -run=MHAnalyzeSources -root=<source_root> [-ledger=<snapshot.json>] ")
            TEXT("[-report=<disabled-until-owner-schema-decision>]"));
        return 2;
    }

    FString LedgerPath;
    FParse::Value(*Params, TEXT("ledger="), LedgerPath);
    FString WriteLedgerPath;
    const bool bWriteLedgerRequested =
        FParse::Value(*Params, TEXT("writeledger="), WriteLedgerPath) ||
        FParse::Param(*Params, TEXT("writeledger"));
    FString ReportPath;
    FParse::Value(*Params, TEXT("report="), ReportPath);

    if (bWriteLedgerRequested)
    {
        UE_LOG(
            LogMHAnalyzeSources,
            Error,
            TEXT("MH_E_SOURCE_INDEX_INVALID: -writeledger is disabled in C1 Analyze/Plan-only; ")
            TEXT("Ledger advances only after a successful Execute operation"));
        return 2;
    }

    if (!ReportPath.IsEmpty())
    {
        UE_LOG(
            LogMHAnalyzeSources,
            Error,
            TEXT("MH_E_SOURCE_INDEX_INVALID: -report is disabled until the owner ratifies a v4 diagnostic schema; no legacy tag is reused"));
        return 2;
    }

    TMap<FString, FMHLedgerRow> Ledger;
    if (!LedgerPath.IsEmpty())
    {
        FString LedgerJson;
        if (!FFileHelper::LoadFileToString(LedgerJson, *LedgerPath))
        {
            UE_LOG(LogMHAnalyzeSources, Error, TEXT("cannot read ledger snapshot %s"), *LedgerPath);
            return 1;
        }
        FString LedgerError;
        if (!MHLedgerSnapshotFromJson(LedgerJson, Ledger, LedgerError))
        {
            UE_LOG(LogMHAnalyzeSources, Error, TEXT("%s"), *LedgerError);
            return 1;
        }
    }

    TUniquePtr<IMHSourceResolver> Resolver;
    FString ScanError;
    if (!MHCreateDefaultSourceResolver(SourceRoot, Resolver, ScanError))
    {
        UE_LOG(LogMHAnalyzeSources, Error, TEXT("%s"), *ScanError);
        return 1;
    }

    FMHSourceAnalysis Analysis;
    TUniquePtr<IMHChangeDetector> ChangeDetector =
        MHCreateSnapshotChangeDetector(Ledger);
    MHAnalyzeSources(*ChangeDetector, *Resolver, SourceRoot, Analysis);

    const FMHSourceSnapshot Snapshot = Resolver->GetSnapshot();

    UE_LOG(
        LogMHAnalyzeSources,
        Display,
        TEXT("scanned %s: %d resource keys, %d deprecated Ledger rows, %d classified resources"),
        *SourceRoot,
        Snapshot.ResourceKeys.Num(),
        Ledger.Num(),
        Analysis.Entries.Num());

    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
    {
        UE_LOG(
            LogMHAnalyzeSources,
            Display,
            TEXT("%-18s %-11s %-24s %s"),
            MHSourceChangeLabel(Entry.Change),
            MHResourceKindLabel(Entry.Key.Kind),
            *Entry.Key.LogicalName,
            Entry.SourcePath.IsEmpty() ? TEXT("-") : *Entry.SourcePath);
        for (const FString& Warning : Entry.Warnings)
        {
            UE_LOG(LogMHAnalyzeSources, Warning, TEXT("  %s"), *Warning);
        }
        for (const FString& Error : Entry.Errors)
        {
            UE_LOG(LogMHAnalyzeSources, Error, TEXT("  %s"), *Error);
        }
    }
    for (const FString& Warning : Analysis.Warnings)
    {
        UE_LOG(LogMHAnalyzeSources, Warning, TEXT("%s"), *Warning);
    }
    for (const FString& Error : Analysis.Errors)
    {
        UE_LOG(LogMHAnalyzeSources, Error, TEXT("%s"), *Error);
    }

    return Analysis.HasErrors() ? 1 : 0;
}
