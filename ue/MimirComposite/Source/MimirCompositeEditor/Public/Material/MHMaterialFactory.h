#pragma once

#include "Factories/Factory.h"
#include "MHMaterialFactory.generated.h"

/** Native Content Browser import adapter for MH .material source documents. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHMaterialFactory final : public UFactory
{
    GENERATED_BODY()

public:
    UMHMaterialFactory();

    virtual bool FactoryCanImport(const FString& Filename) override;
    virtual UObject* FactoryCreateFile(
        UClass* InClass,
        UObject* InParent,
        FName InName,
        EObjectFlags Flags,
        const FString& Filename,
        const TCHAR* Parms,
        FFeedbackContext* Warn,
        bool& bOutOperationCanceled) override;
};
