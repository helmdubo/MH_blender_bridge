#pragma once

#include "Commandlets/Commandlet.h"
#include "MHSourceCommandlets.generated.h"

/** Read-only full Source Protocol v4 scan. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHScanSourcesCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};

/** Headless source-wins import of the complete stable snapshot. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHImportSourcesCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};

/** Read-only canonical-name diagnostics. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHValidateNamesCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};

/** Read-only managed material verification with strict managed-mesh audit. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHVerifyMaterialsCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};

/** Read-only managed composite verification with strict managed-mesh audit. */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHVerifyCompositesCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
