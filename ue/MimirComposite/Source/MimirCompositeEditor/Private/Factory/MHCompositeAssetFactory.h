#pragma once

#include "EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "MHCompositeAssetFactory.generated.h"

/** Imports mh.composite v2 payloads into read-only UMHCompositeAsset objects. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHCompositeAssetFactory final : public UFactory, public FReimportHandler
{
    GENERATED_UCLASS_BODY()

public:
    virtual UObject* FactoryCreateFile(
        UClass* InClass,
        UObject* InParent,
        FName InName,
        EObjectFlags Flags,
        const FString& Filename,
        const TCHAR* Parms,
        FFeedbackContext* Warn,
        bool& bOutOperationCanceled) override;

    virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
    virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
    virtual EReimportResult::Type Reimport(UObject* Obj) override;
};
