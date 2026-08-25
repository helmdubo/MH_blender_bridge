#pragma once

#include "Factories/Factory.h"
#include "MHCompositeFactory.generated.h"

/** File-drop adapter: direct import inside source_root, explicit Adopt outside it. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHCompositeFactory final : public UFactory
{
    GENERATED_BODY()

public:
    UMHCompositeFactory();

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
