#include "Diagnostics/MHAnalyzeSourcesCommandlet.h"

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
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    FString SourceRoot;
    FParse::Value(*Params, TEXT("root="), SourceRoot);
    if (SourceRoot.IsEmpty())
    {
        SourceRoot = Settings->GetSourceRootPath();
    }
    if (SourceRoot.IsEmpty())
    {
        UE_LOG(
            LogMHAnalyzeSources,
            Error,
            TEXT("Usage: -run=MHAnalyzeSources -root=<source_root> ")
            TEXT("[-report=<disabled-until-S6>]"));
        return 2;
    }

    FString ReportPath;
    FParse::Value(*Params, TEXT("report="), ReportPath);

    if (!ReportPath.IsEmpty())
    {
        UE_LOG(
            LogMHAnalyzeSources,
            Error,
            TEXT("MH_E_SOURCE_INDEX_INVALID: -report is disabled until the S6 mh.analyze_sources:4 implementation"));
        return 2;
    }

    FMHSourceAnalysisServices Services;
    FString ScanError;
    if (!MHCreateDefaultSourceAnalysisServices(
            SourceRoot,
            Services,
            ScanError))
    {
        UE_LOG(LogMHAnalyzeSources, Error, TEXT("%s"), *ScanError);
        return 1;
    }

    FMHSourceAnalysis Analysis;
    MHAnalyzeSources(
        *Services.ChangeDetector,
        *Services.Resolver,
        SourceRoot,
        Analysis);

    const FMHSourceSnapshot Snapshot = Services.Resolver->GetSnapshot();

    UE_LOG(
        LogMHAnalyzeSources,
        Display,
        TEXT("scanned %s: %d resource keys, %d classified resources"),
        *SourceRoot,
        Snapshot.ResourceKeys.Num(),
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
