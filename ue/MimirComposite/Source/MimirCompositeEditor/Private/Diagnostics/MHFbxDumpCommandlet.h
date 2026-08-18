#pragma once

#include "Commandlets/Commandlet.h"
#include "MHFbxDumpCommandlet.generated.h"

UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHFbxDumpCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
