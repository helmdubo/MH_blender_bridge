#include "UI/MHEditorMenus.h"

#include "DesktopPlatformModule.h"
#include "Diagnostics/MHCompositeDumpUtil.h"
#include "Diagnostics/MHFbxDump.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Logging/MessageLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/MHFbxPassport.h"
#include "ToolMenus.h"

namespace MH::EditorMenus
{
namespace
{

const FName MimirLogName(TEXT("Mimir"));

bool PickFile(const FString& Title, const FString& FileTypes, FString& OutFile)
{
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (DesktopPlatform == nullptr || !FSlateApplication::IsInitialized())
    {
        return false;
    }
    const void* ParentWindowHandle =
        FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
    TArray<FString> Files;
    if (!DesktopPlatform->OpenFileDialog(
            ParentWindowHandle, Title, FPaths::ProjectDir(), FString(), FileTypes,
            EFileDialogFlags::None, Files) ||
        Files.Num() != 1)
    {
        return false;
    }
    OutFile = FPaths::ConvertRelativePathToFull(Files[0]);
    return true;
}

bool PickDirectory(const FString& Title, FString& OutDirectory)
{
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (DesktopPlatform == nullptr || !FSlateApplication::IsInitialized())
    {
        return false;
    }
    const void* ParentWindowHandle =
        FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
    FString Directory;
    if (!DesktopPlatform->OpenDirectoryDialog(ParentWindowHandle, Title, FPaths::ProjectDir(), Directory))
    {
        return false;
    }
    OutDirectory = FPaths::ConvertRelativePathToFull(Directory);
    return true;
}

FString DumpOutputDir()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirDumps"));
}

bool WriteDumpFile(const FString& Json, const FString& OutPath, FMessageLog& Log)
{
    if (!FFileHelper::SaveStringToFile(
            Json + TEXT("\n"),
            *OutPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Log.Error(FText::FromString(FString::Printf(TEXT("cannot write %s"), *OutPath)));
        return false;
    }
    Log.Info(FText::FromString(FString::Printf(TEXT("dump written: %s"), *OutPath)));
    return true;
}

void OnDumpFbx()
{
    FString FilePath;
    if (!PickFile(
            TEXT("Mimir FBX Dump"),
            TEXT("FBX payload (*.fbx)|*.fbx|All files (*.*)|*.*"),
            FilePath))
    {
        return;
    }

    FMessageLog Log(MimirLogName);
    Log.NewPage(FText::FromString(FString::Printf(TEXT("FBX Dump: %s"), *FPaths::GetCleanFilename(FilePath))));
    Log.Info(FText::FromString(FString::Printf(TEXT("mh.fbxdump for %s"), *FilePath)));

    const FString BaseName = FPaths::GetBaseFilename(FilePath);
    bool bAnyError = false;
    for (const bool bFull : {false, true})
    {
        TSharedPtr<FJsonObject> Root;
        FString Error;
        if (!MH::FbxDump::BuildDom(FilePath, bFull, Root, Error))
        {
            Log.Error(FText::FromString(Error));
            bAnyError = true;
            break;
        }
        FString Json;
        if (!MH::FbxDump::SerializeCanonical(Root, Json, Error))
        {
            Log.Error(FText::FromString(Error));
            bAnyError = true;
            break;
        }
        const FString OutPath = FPaths::Combine(
            DumpOutputDir(),
            FString::Printf(TEXT("%s.%s.json"), *BaseName, bFull ? TEXT("full") : TEXT("summary")));
        bAnyError |= !WriteDumpFile(Json, OutPath, Log);
    }

    // Field visibility of the mandatory v2 identity: a legacy FBX without the
    // Carrier B passport is expected to be quarantined by the production
    // reader, so the outcome is reported either way.
    UE::MimirComposite::FMHFbxPassport Passport;
    FString PassportError;
    if (UE::MimirComposite::MHReadFbxPassport(FilePath, Passport, PassportError))
    {
        Log.Info(FText::FromString(FString::Printf(
            TEXT("passport: uid=%s name=%s lod_levels=%d slots=%d copies=%d"),
            *Passport.ResourceUid,
            *Passport.Name,
            Passport.LodLevels.Num(),
            Passport.MaterialSlots.Num(),
            Passport.CopyCount)));
    }
    else
    {
        Log.Warning(FText::FromString(FString::Printf(
            TEXT("no valid Carrier B passport (production reader would quarantine): %s"),
            *PassportError)));
    }

    Log.Open(bAnyError ? EMessageSeverity::Error : EMessageSeverity::Info, true);
}

void OnDumpComposite()
{
    FString FilePath;
    if (!PickFile(
            TEXT("Mimir Composite Dump"),
            TEXT("Mimir composite (*.composite)|*.composite"),
            FilePath))
    {
        return;
    }
    FString SourceRoot;
    PickDirectory(
        TEXT("Select source_root for recursive resolve (Cancel = single file dump)"),
        SourceRoot);

    MH::CompositeDump::FDumpOutput Output;
    const bool bClean = MH::CompositeDump::BuildDump(FilePath, SourceRoot, Output);

    FMessageLog Log(MimirLogName);
    Log.NewPage(FText::FromString(FString::Printf(
        TEXT("Composite Dump: %s"),
        *FPaths::GetCleanFilename(FilePath))));
    for (const FString& Line : Output.Lines)
    {
        if (!Line.IsEmpty())
        {
            Log.Info(FText::FromString(Line));
        }
    }
    for (const FString& Warning : Output.Warnings)
    {
        Log.Warning(FText::FromString(Warning));
    }
    for (const FString& Error : Output.Errors)
    {
        Log.Error(FText::FromString(Error));
    }
    Log.Open(bClean ? EMessageSeverity::Info : EMessageSeverity::Error, true);
}

} // namespace

void Populate()
{
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
    if (Menu == nullptr)
    {
        return;
    }
    FToolMenuSection& Section = Menu->AddSection("MimirComposite", INVTEXT("Mimir"));
    Section.AddMenuEntry(
        "MHFbxDump",
        INVTEXT("Mimir FBX Dump..."),
        INVTEXT("Read one FBX with the raw mh.fbxdump reader, report the Carrier B passport and write summary/full JSON dumps to Saved/MimirDumps."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateStatic(&OnDumpFbx)));
    Section.AddMenuEntry(
        "MHCompositeDump",
        INVTEXT("Mimir Composite Dump..."),
        INVTEXT("Print the node hierarchy of one mh.composite v2 payload to the Mimir message log, optionally resolving dependencies under a source root."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateStatic(&OnDumpComposite)));
}

} // namespace MH::EditorMenus
