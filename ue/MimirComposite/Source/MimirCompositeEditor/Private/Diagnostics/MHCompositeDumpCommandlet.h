#pragma once

#include "Commandlets/Commandlet.h"
#include "MHCompositeDumpCommandlet.generated.h"

/**
 * -run=MHCompositeDump <file.composite> [-root=<source_root>]
 *
 * Prints the node hierarchy of one mh.composite v2 payload. With -root the
 * commandlet scans the Clean Sources v2 payload set, recursively expands
 * composite_ref dependencies and reports the resolve status of every
 * referenced resource (unresolved / duplicate / divergent / cycle).
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHCompositeDumpCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
