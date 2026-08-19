#include "Diagnostics/MHAnalyzeSourcesCommandlet.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Ledger/MHImportLedger.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadScanResolver.h"
#include "Source/MHSourceAnalyzer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHAnalyzeSourcesCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogMHAnalyzeSources, Display, All);

using namespace UE::MimirComposite;

namespace
{

TSharedPtr<FJsonValue> AnalyzeSourcesStringArray(const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FString& Value : Values)
    {
        Items.Add(MakeShared<FJsonValueString>(Value));
    }
    return MakeShared<FJsonValueArray>(MoveTemp(Items));
}

FString AnalyzeSourcesReportJson(const FString& SourceRoot, const FMHSourceAnalysis& Analysis)
{
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
    {
        const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("resource_uid"), Entry.ResourceUid);
        Object->SetStringField(TEXT("kind"), MHResourceKindLabel(Entry.Kind));
        Object->SetStringField(TEXT("name"), Entry.Name);
        Object->SetStringField(TEXT("classification"), MHSourceChangeLabel(Entry.Change));
        Object->SetStringField(TEXT("source_path"), Entry.SourcePath);
        Object->SetStringField(TEXT("geometry_hash"), Entry.GeometryHash);
        Object->SetStringField(TEXT("descriptor_hash"), Entry.DescriptorHash);
        Object->SetStringField(TEXT("payload_fingerprint"), Entry.Fingerprint);
        Object->SetField(TEXT("warnings"), AnalyzeSourcesStringArray(Entry.Warnings));
        Object->SetField(TEXT("errors"), AnalyzeSourcesStringArray(Entry.Errors));
        Entries.Add(MakeShared<FJsonValueObject>(Object));
    }

    const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("tag"), TEXT("mh.analyze_sources:1"));
    Root->SetStringField(TEXT("source_root"), SourceRoot);
    Root->SetArrayField(TEXT("entries"), Entries);
    Root->SetField(TEXT("warnings"), AnalyzeSourcesStringArray(Analysis.Warnings));
    Root->SetField(TEXT("errors"), AnalyzeSourcesStringArray(Analysis.Errors));

    FString Json;
    const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
    return Json;
}

} // namespace

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
            TEXT("[-writeledger=<out.json>] [-report=<out.json>]"));
        return 2;
    }

    FString LedgerPath;
    FParse::Value(*Params, TEXT("ledger="), LedgerPath);
    FString WriteLedgerPath;
    FParse::Value(*Params, TEXT("writeledger="), WriteLedgerPath);
    FString ReportPath;
    FParse::Value(*Params, TEXT("report="), ReportPath);

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

    FMHPayloadScanResolver Resolver(SourceRoot);
    FString ScanError;
    if (!Resolver.Initialize(ScanError))
    {
        UE_LOG(LogMHAnalyzeSources, Error, TEXT("%s"), *ScanError);
        return 1;
    }

    FMHSourceAnalysis Analysis;
    MHAnalyzeSources(Resolver, SourceRoot, Ledger, Analysis);

    UE_LOG(
        LogMHAnalyzeSources,
        Display,
        TEXT("scanned %s: %d payload candidates, %d ledger rows, %d classified resources"),
        *SourceRoot,
        Resolver.GetCandidateFileCount(),
        Ledger.Num(),
        Analysis.Entries.Num());

    for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
    {
        UE_LOG(
            LogMHAnalyzeSources,
            Display,
            TEXT("%-18s %-11s %-24s %s"),
            MHSourceChangeLabel(Entry.Change),
            MHResourceKindLabel(Entry.Kind),
            Entry.Name.IsEmpty() ? TEXT("-") : *Entry.Name,
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

    if (!ReportPath.IsEmpty())
    {
        const FString ReportJson = AnalyzeSourcesReportJson(SourceRoot, Analysis);
        if (!FFileHelper::SaveStringToFile(
                ReportJson,
                *ReportPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogMHAnalyzeSources, Error, TEXT("cannot write report %s"), *ReportPath);
            return 1;
        }
        UE_LOG(LogMHAnalyzeSources, Display, TEXT("report written: %s"), *ReportPath);
    }

    if (!WriteLedgerPath.IsEmpty())
    {
        // Rows of the confirmation gate, orphans and blocked resources keep their
        // previous state: only classifications that a real import would commit
        // advance the snapshot.
        TMap<FString, FMHLedgerRow> Advanced = Ledger;
        int32 AdvancedCount = 0;
        for (const FMHSourceAnalysisEntry& Entry : Analysis.Entries)
        {
            FMHLedgerRow Row;
            if (MHLedgerRowFromAnalysis(Entry, Row))
            {
                Advanced.Add(Entry.ResourceUid, MoveTemp(Row));
                ++AdvancedCount;
            }
        }
        FString LedgerJson;
        if (!MHLedgerSnapshotToJson(Advanced, LedgerJson) ||
            !FFileHelper::SaveStringToFile(
                LedgerJson,
                *WriteLedgerPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogMHAnalyzeSources, Error, TEXT("cannot write ledger snapshot %s"), *WriteLedgerPath);
            return 1;
        }
        UE_LOG(
            LogMHAnalyzeSources,
            Display,
            TEXT("ledger snapshot written: %s (%d rows, %d advanced)"),
            *WriteLedgerPath,
            Advanced.Num(),
            AdvancedCount);
    }

    return Analysis.HasErrors() ? 1 : 0;
}
