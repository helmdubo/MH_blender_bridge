#include "Diagnostics/MHSourceCommandlets.h"

#include "Diagnostics/MHAnalyzeSourcesReport.h"
#include "Diagnostics/MHSourceOperations.h"
#include "Misc/Parse.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceImporter.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHSourceCommandlets)

DEFINE_LOG_CATEGORY_STATIC(LogMHSourceCommandlets, Display, All);

using namespace UE::MimirComposite;

namespace
{

void ConfigureSourceCommandlet(UCommandlet& Commandlet)
{
    Commandlet.IsClient = false;
    Commandlet.IsEditor = true;
    Commandlet.IsServer = false;
    Commandlet.LogToConsole = true;
    Commandlet.ShowErrorCount = true;
    Commandlet.UseCommandletResultAsExitCode = true;
}

bool ResolveSourceRoot(const FString& Params, FString& OutSourceRoot)
{
    FParse::Value(*Params, TEXT("root="), OutSourceRoot);
    if (OutSourceRoot.IsEmpty())
    {
        OutSourceRoot = GetDefault<UMHCompositeSettings>()->GetSourceRootPath();
    }
    return !OutSourceRoot.IsEmpty();
}

void LogAnalysis(const FMHSourceAnalysis& Analysis)
{
    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
    {
        UE_LOG(
            LogMHSourceCommandlets,
            Display,
            TEXT("%-18s %-11s %-24s %s"),
            MHSourceChangeLabel(Entry.Change),
            MHResourceKindLabel(Entry.Key.Kind),
            *Entry.Key.LogicalName,
            Entry.SourcePath.IsEmpty() ? TEXT("-") : *Entry.SourcePath);
        for (const FString& Warning : Entry.Warnings)
        {
            UE_LOG(LogMHSourceCommandlets, Warning, TEXT("  %s"), *Warning);
        }
        for (const FString& Error : Entry.Errors)
        {
            UE_LOG(LogMHSourceCommandlets, Error, TEXT("  %s"), *Error);
        }
    }
    for (const FString& Warning : Analysis.Warnings)
    {
        UE_LOG(LogMHSourceCommandlets, Warning, TEXT("%s"), *Warning);
    }
    for (const FString& Error : Analysis.Errors)
    {
        UE_LOG(LogMHSourceCommandlets, Error, TEXT("%s"), *Error);
    }
}

bool WriteOptionalReport(
    const FString& Params,
    const FString& SourceRoot,
    const FMHSourceAnalysis& Analysis)
{
    FString ReportPath;
    FParse::Value(*Params, TEXT("report="), ReportPath);
    if (ReportPath.IsEmpty())
    {
        return true;
    }
    FString AbsolutePath;
    FString Error;
    if (!MHWriteAnalyzeSourcesReportV4(
            SourceRoot,
            ReportPath,
            Analysis,
            AbsolutePath,
            Error))
    {
        UE_LOG(LogMHSourceCommandlets, Error, TEXT("%s"), *Error);
        return false;
    }
    UE_LOG(
        LogMHSourceCommandlets,
        Display,
        TEXT("wrote mh.analyze_sources:4 report: %s"),
        *AbsolutePath);
    return true;
}

int32 CompleteOperation(
    const FString& Params,
    const FString& SourceRoot,
    const bool bOperationSucceeded,
    FMHSourceAnalysis& Analysis,
    const FString& OperationError)
{
    if (!OperationError.IsEmpty() && !Analysis.Errors.Contains(OperationError))
    {
        Analysis.Errors.Add(OperationError);
    }
    LogAnalysis(Analysis);
    const bool bReportSucceeded = WriteOptionalReport(Params, SourceRoot, Analysis);
    return MHSourceCommandletExitCode(
        true,
        bOperationSucceeded && bReportSucceeded,
        Analysis);
}

} // namespace

UMHScanSourcesCommandlet::UMHScanSourcesCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    ConfigureSourceCommandlet(*this);
}

int32 UMHScanSourcesCommandlet::Main(const FString& Params)
{
    FString SourceRoot;
    if (!ResolveSourceRoot(Params, SourceRoot))
    {
        UE_LOG(LogMHSourceCommandlets, Error, TEXT("Usage: -run=MHScanSources -root=<source_root> [-report=<Saved/Mimir path>]"));
        return 2;
    }
    FMHSourceAnalysis Analysis;
    FString Error;
    const bool bSucceeded = MHScanSourcesOperation(SourceRoot, Analysis, Error);
    return CompleteOperation(Params, SourceRoot, bSucceeded, Analysis, Error);
}

UMHImportSourcesCommandlet::UMHImportSourcesCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    ConfigureSourceCommandlet(*this);
}

int32 UMHImportSourcesCommandlet::Main(const FString& Params)
{
    FString SourceRoot;
    if (!ResolveSourceRoot(Params, SourceRoot))
    {
        UE_LOG(LogMHSourceCommandlets, Error, TEXT("Usage: -run=MHImportSources -root=<source_root> [-report=<Saved/Mimir path>]"));
        return 2;
    }
    FMHSourceAnalysis Analysis;
    bool bExecuted = false;
    const bool bSucceeded = MHImportSourcesHeadless(
        SourceRoot,
        FMHImportSourcesScope::All(),
        *GetDefault<UMHCompositeSettings>(),
        Analysis,
        bExecuted);
    UE_LOG(
        LogMHSourceCommandlets,
        Display,
        TEXT("headless import completed; asset mutation executed: %s"),
        bExecuted ? TEXT("true") : TEXT("false"));
    return CompleteOperation(Params, SourceRoot, bSucceeded, Analysis, FString());
}

UMHValidateNamesCommandlet::UMHValidateNamesCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    ConfigureSourceCommandlet(*this);
}

int32 UMHValidateNamesCommandlet::Main(const FString& Params)
{
    FString SourceRoot;
    if (!ResolveSourceRoot(Params, SourceRoot))
    {
        UE_LOG(LogMHSourceCommandlets, Error, TEXT("Usage: -run=MHValidateNames -root=<source_root> [-report=<Saved/Mimir path>]"));
        return 2;
    }
    FMHSourceAnalysis Analysis;
    FString Error;
    const bool bSucceeded = MHValidateNamesOperation(SourceRoot, Analysis, Error);
    return CompleteOperation(Params, SourceRoot, bSucceeded, Analysis, Error);
}

UMHVerifyMaterialsCommandlet::UMHVerifyMaterialsCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    ConfigureSourceCommandlet(*this);
}

int32 UMHVerifyMaterialsCommandlet::Main(const FString& Params)
{
    FString SourceRoot;
    if (!ResolveSourceRoot(Params, SourceRoot))
    {
        UE_LOG(LogMHSourceCommandlets, Error, TEXT("Usage: -run=MHVerifyMaterials -root=<source_root> [-report=<Saved/Mimir path>]"));
        return 2;
    }
    FMHSourceAnalysis Analysis;
    FString Error;
    const bool bSucceeded = MHVerifyMaterialsOperation(
        SourceRoot,
        *GetDefault<UMHCompositeSettings>(),
        true,
        Analysis,
        Error);
    return CompleteOperation(Params, SourceRoot, bSucceeded, Analysis, Error);
}

UMHVerifyCompositesCommandlet::UMHVerifyCompositesCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    ConfigureSourceCommandlet(*this);
}

int32 UMHVerifyCompositesCommandlet::Main(const FString& Params)
{
    FString SourceRoot;
    if (!ResolveSourceRoot(Params, SourceRoot))
    {
        UE_LOG(LogMHSourceCommandlets, Error, TEXT("Usage: -run=MHVerifyComposites -root=<source_root> [-report=<Saved/Mimir path>]"));
        return 2;
    }
    FMHSourceAnalysis Analysis;
    FString Error;
    const bool bSucceeded = MHVerifyCompositesOperation(
        SourceRoot,
        true,
        Analysis,
        Error);
    return CompleteOperation(Params, SourceRoot, bSucceeded, Analysis, Error);
}
