#include "Material/MHMaterialFactory.h"

#include "Editor.h"
#include "Logging/MessageLog.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FeedbackContext.h"
#include "Misc/Paths.h"
#include "Source/MHSourceImporter.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHMaterialFactory)

UMHMaterialFactory::UMHMaterialFactory()
{
    SupportedClass = UMaterialInstanceConstant::StaticClass();
    bCreateNew = false;
    bEditorImport = true;
    Formats.Add(TEXT("material;MH Material Source"));
}

bool UMHMaterialFactory::FactoryCanImport(const FString& Filename)
{
    return FPaths::GetExtension(Filename, true).Equals(TEXT(".material"), ESearchCase::CaseSensitive);
}

UObject* UMHMaterialFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    // Like .composite, source identity determines the managed asset path,
    // independently of the Content Browser folder or UE's proposed asset name.
    (void)InClass;
    (void)InParent;
    (void)InName;
    (void)Flags;
    (void)Parms;
    bOutOperationCanceled = false;
    UMHSourceImporter* Importer = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHSourceImporter>() : nullptr;
    UMaterialInstanceConstant* Material = nullptr;
    TArray<FString> Warnings;
    FString Error;
    const bool bImported = Importer != nullptr &&
        Importer->ImportMaterialFile(Filename, Material, Warnings, Error);
    FMessageLog Log(TEXT("Mimir"));
    for (const FString& Warning : Warnings)
        Log.Warning(FText::FromString(FString::Printf(TEXT("%s: %s"), *Filename, *Warning)));
    if (!bImported)
    {
        if (Error.IsEmpty()) Error = TEXT("MH_E_IMPORT_THREAD_INVALID: MH Source Importer subsystem is unavailable");
        const FString Diagnostic = FString::Printf(TEXT("%s: %s"), *Filename, *Error);
        if (Warn != nullptr) Warn->Logf(ELogVerbosity::Error, TEXT("%s"), *Diagnostic);
        Log.Error(FText::FromString(Diagnostic));
        Log.Notify(INVTEXT("MH Material import failed"), EMessageSeverity::Error, true);
        return nullptr;
    }
    Log.Info(FText::FromString(FString::Printf(TEXT("Imported %s as %s"), *Filename, *Material->GetPathName())));
    Log.Notify(INVTEXT("MH Material imported"), EMessageSeverity::Info, true);
    return Material;
}
