#pragma once

#include "Commandlets/Commandlet.h"
#include "MHFbxDumpCommandlet.generated.h"

/**
 * -run=MHFbxDump <file.mesh.fbx> [-full] [-report=<Saved/Mimir relative path>]
 *
 * Read-only mapper-facing Source Protocol v4 transport dump. The commandlet
 * parses through the same FBX SDK -> FMHSceneIR path as the static-mesh
 * importer and never reads legacy passports or custom MH properties.
 */
UCLASS()
class MIMIRCOMPOSITEEDITOR_API UMHFbxDumpCommandlet final : public UCommandlet
{
    GENERATED_UCLASS_BODY()

public:
    virtual int32 Main(const FString& Params) override;
};
