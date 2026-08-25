#include "Composite/MHCompositeFactory.h"

#include "Composite/MHCompositeAsset.h"
#include "Composite/MHCompositeImporter.h"
#include "Editor.h"
#include "Logging/MessageLog.h"
#include "Misc/FeedbackContext.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceImporter.h"
#include "UI/MHSourceToolMenus.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeFactory)

namespace
{

bool MHCompositeFactoryFileIsInsideRoot(const FString& Filename, const FString& SourceRoot)
{
    if (SourceRoot.IsEmpty())
    {
        return false;
    }
    FString AbsoluteRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FString AbsoluteFile = FPaths::ConvertRelativePathToFull(Filename);
    FPaths::NormalizeDirectoryName(AbsoluteRoot);
    FPaths::NormalizeFilename(AbsoluteFile);
    FPaths::CollapseRelativeDirectories(AbsoluteRoot);
    FPaths::CollapseRelativeDirectories(AbsoluteFile);
    return FPaths::IsUnderDirectory(AbsoluteFile, AbsoluteRoot);
}

} // namespace

UMHCompositeFactory::UMHCompositeFactory()
{
    SupportedClass = UMHCompositeAsset::StaticClass();
    bCreateNew = false;
    bEditorImport = true;
    Formats.Add(TEXT("composite;MH Composite Source"));
}

bool UMHCompositeFactory::FactoryCanImport(const FString& Filename)
{
    return FPaths::GetExtension(Filename, true).Equals(
        TEXT(".composite"),
        ESearchCase::CaseSensitive);
}

UObject* UMHCompositeFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    (void)InClass;
    (void)InName;
    (void)Flags;
    (void)Parms;
    bOutOperationCanceled = false;

    const FString TargetPackageName = InParent != nullptr
        ? InParent->GetOutermost()->GetName()
        : FString();
    UMHSourceImporter* Importer = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHSourceImporter>()
        : nullptr;
    UMHCompositeAsset* Asset = nullptr;
    TArray<FString> Warnings;
    FString Error;
    bool bImported = false;
    if (Importer != nullptr)
    {
        const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
        const FString SourceRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
        if (MHCompositeFactoryFileIsInsideRoot(Filename, SourceRoot))
        {
            bImported = Importer->ImportCompositeFile(
                Filename,
                TargetPackageName,
                Asset,
                Warnings,
                Error);
        }
        else if (!SourceRoot.IsEmpty())
        {
            UE::MimirComposite::FMHCompositeAdoptTarget AdoptTarget;
            if (!MHPromptCompositeAdoptTarget(
                    AdoptTarget,
                    FPaths::GetBaseFilename(Filename),
                    INVTEXT("Adopt External MH Composite"),
                    INVTEXT("Adopt and Import")))
            {
                bOutOperationCanceled = true;
                return nullptr;
            }
            bImported = Importer->AdoptCompositeFile(
                Filename,
                AdoptTarget.Folder,
                AdoptTarget.LogicalName,
                Asset,
                Warnings,
                Error);
        }
    }
    if (!bImported)
    {
        if (Error.IsEmpty())
        {
            Error = TEXT("MH_E_IMPORT_THREAD_INVALID: MH Source Importer subsystem is unavailable");
        }
        if (Warn != nullptr)
        {
            Warn->Logf(ELogVerbosity::Error, TEXT("%s"), *Error);
        }
        FMessageLog Log(TEXT("Mimir"));
        Log.Error(FText::FromString(Error));
        Log.Notify(INVTEXT("MH Composite import failed"), EMessageSeverity::Error, true);
        return nullptr;
    }

    FMessageLog Log(TEXT("Mimir"));
    for (const FString& Warning : Warnings)
    {
        Log.Warning(FText::FromString(FString::Printf(TEXT("%s: %s"), *Filename, *Warning)));
    }
    Log.Info(FText::FromString(FString::Printf(
        TEXT("Imported %s as %s"),
        *Filename,
        *Asset->GetPathName())));
    Log.Notify(INVTEXT("MH Composite imported"), EMessageSeverity::Info, true);
    return Asset;
}
