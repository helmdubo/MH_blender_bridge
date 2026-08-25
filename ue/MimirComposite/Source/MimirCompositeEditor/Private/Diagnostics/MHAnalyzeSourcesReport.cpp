#include "Diagnostics/MHAnalyzeSourcesReport.h"

#include "Diagnostics/MHReaderOutputPath.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Source/MHSourceResolver.h"

namespace UE::MimirComposite
{
namespace
{

#if WITH_DEV_AUTOMATION_TESTS
TFunction<void(const FString&)> GBeforeAnalyzeSourcesReportReadBackTestHook;
#endif

void WriteSortedStringArray(
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>& Writer,
    const TCHAR* Name,
    const TArray<FString>& Values)
{
    TArray<FString> Sorted = Values;
    Sorted.Sort();
    Writer->WriteArrayStart(Name);
    for (const FString& Value : Sorted)
    {
        Writer->WriteValue(Value);
    }
    Writer->WriteArrayEnd();
}

bool AnalyzeEntryLess(const FMHSourceAnalysisEntry& A, const FMHSourceAnalysisEntry& B)
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
}

} // namespace

#if WITH_DEV_AUTOMATION_TESTS
void MHSetBeforeAnalyzeSourcesReportReadBackTestHook(
    TFunction<void(const FString&)> Hook)
{
    GBeforeAnalyzeSourcesReportReadBackTestHook = MoveTemp(Hook);
}
#endif

bool MHSerializeAnalyzeSourcesReportV4(
    const FString& SourceRoot,
    const FMHSourceAnalysis& Analysis,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    OutBytes.Reset();
    OutError.Reset();

    FString NormalizedRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(NormalizedRoot);
    if (NormalizedRoot.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot serialize a report with an empty source_root");
        return false;
    }

    FString Json;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
    Writer->WriteObjectStart();
    Writer->WriteValue(TEXT("tag"), MHAnalyzeSourcesReportTag);
    Writer->WriteValue(TEXT("source_root"), NormalizedRoot);
    Writer->WriteArrayStart(TEXT("entries"));

    TArray<FMHSourceAnalysisEntry> Entries = Analysis.Entries;
    Entries.Sort(AnalyzeEntryLess);
    for (const FMHSourceAnalysisEntry& Entry : Entries)
    {
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("kind"), MHResourceKindLabel(Entry.Key.Kind));
        Writer->WriteValue(TEXT("name"), Entry.Key.LogicalName);
        Writer->WriteValue(TEXT("classification"), MHSourceChangeLabel(Entry.Change));
        Writer->WriteValue(TEXT("source_path"), Entry.SourcePath);
        Writer->WriteValue(TEXT("source_hash"), Entry.RawHash);
        WriteSortedStringArray(Writer, TEXT("warnings"), Entry.Warnings);
        WriteSortedStringArray(Writer, TEXT("errors"), Entry.Errors);
        Writer->WriteObjectEnd();
    }
    Writer->WriteArrayEnd();
    WriteSortedStringArray(Writer, TEXT("warnings"), Analysis.Warnings);
    WriteSortedStringArray(Writer, TEXT("errors"), Analysis.Errors);
    Writer->WriteObjectEnd();
    Writer->Close();
    Json.AppendChar(TEXT('\n'));

    FTCHARToUTF8 Utf8(*Json);
    OutBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    return true;
}

bool MHValidateAnalyzeSourcesReportV4(
    const TArray<uint8>& Bytes,
    FString& OutError)
{
    OutError.Reset();
    if (Bytes.IsEmpty())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: analyze report is empty");
        return false;
    }
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(
            TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())),
            Root) ||
        !Root.IsValid())
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: analyze report read-back is not valid JSON");
        return false;
    }
    FString Tag;
    if (!Root->TryGetStringField(TEXT("tag"), Tag) || Tag != MHAnalyzeSourcesReportTag)
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: analyze report read-back has the wrong tag");
        return false;
    }
    return true;
}

bool MHWriteAnalyzeSourcesReportV4(
    const FString& SourceRoot,
    const FString& RequestedPath,
    const FMHSourceAnalysis& Analysis,
    FString& OutAbsolutePath,
    FString& OutError)
{
    OutAbsolutePath.Reset();
    OutError.Reset();
    if (RequestedPath.IsEmpty() ||
        !MHResolveReaderOutputPath(SourceRoot, RequestedPath, OutAbsolutePath, OutError))
    {
        if (OutError.IsEmpty())
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: analyze report path is empty");
        }
        return false;
    }

    TArray<uint8> Bytes;
    if (!MHSerializeAnalyzeSourcesReportV4(SourceRoot, Analysis, Bytes, OutError))
    {
        return false;
    }
    const FString Folder = FPaths::GetPath(OutAbsolutePath);
    if (!IFileManager::Get().MakeDirectory(*Folder, true))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: cannot create report directory: %s"),
            *Folder);
        return false;
    }

    const FString TempPath = OutAbsolutePath + FString::Printf(
        TEXT(".tmp.%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: cannot write sibling report temporary: %s"),
            *TempPath);
        return false;
    }

#if WITH_DEV_AUTOMATION_TESTS
    if (GBeforeAnalyzeSourcesReportReadBackTestHook)
    {
        TFunction<void(const FString&)> Hook =
            MoveTemp(GBeforeAnalyzeSourcesReportReadBackTestHook);
        Hook(TempPath);
    }
#endif

    TArray<uint8> ReadBack;
    FString ValidationError;
    if (!FFileHelper::LoadFileToArray(ReadBack, *TempPath) ||
        ReadBack != Bytes ||
        !MHValidateAnalyzeSourcesReportV4(ReadBack, ValidationError))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: analyze report temporary read-back failed: %s"),
            *ValidationError);
        return false;
    }
    if (!IFileManager::Get().Move(*OutAbsolutePath, *TempPath, true, true, false, true))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: analyze report atomic replace failed: %s"),
            *OutAbsolutePath);
        return false;
    }
    return true;
}

} // namespace UE::MimirComposite
