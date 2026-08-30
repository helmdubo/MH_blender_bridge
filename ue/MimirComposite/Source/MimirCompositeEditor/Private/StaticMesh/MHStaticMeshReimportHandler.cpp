#include "StaticMesh/MHStaticMeshReimportHandler.h"

#include "EditorReimportHandler.h"
#include "Engine/StaticMesh.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceResolver.h"
#include "StaticMesh/MHStaticMeshImportData.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHStaticMeshReimport, Log, All);

namespace UE::MimirComposite
{
namespace
{

bool ResolveManagedSourcePath(UObject* Object, FString& OutSourcePath)
{
    OutSourcePath.Reset();
    const UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object);
    if (StaticMesh == nullptr)
    {
        return false;
    }
    const UMHStaticMeshImportData* Data =
        Cast<UMHStaticMeshImportData>(StaticMesh->GetAssetImportData());
    if (Data == nullptr || Data->LogicalName.IsEmpty() || Data->SourceRelativePath.IsEmpty())
    {
        return false;
    }
    FMHResourceKey Key;
    Key.Kind = EMHResourceKind::StaticMesh;
    Key.LogicalName = Data->LogicalName;
    if (!Key.IsCanonical())
    {
        return false;
    }
    const FString ExpectedObjectPath = FString(TEXT("/Game/MH/Generated/Meshes/")) +
        Key.LogicalName + TEXT(".") + Key.LogicalName;
    if (StaticMesh->GetPathName() != ExpectedObjectPath)
    {
        return false;
    }
    const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
    const FString ConfiguredRoot = Settings != nullptr ? Settings->GetSourceRootPath() : FString();
    if (ConfiguredRoot.IsEmpty())
    {
        return false;
    }
    FString SourceRoot = FPaths::ConvertRelativePathToFull(ConfiguredRoot);
    FPaths::NormalizeDirectoryName(SourceRoot);
    OutSourcePath = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(SourceRoot, Data->SourceRelativePath));
    FPaths::NormalizeFilename(OutSourcePath);
    if (!FPaths::IsUnderDirectory(OutSourcePath, SourceRoot))
    {
        OutSourcePath.Reset();
        return false;
    }
    return true;
}

class FMHManagedStaticMeshReimportHandler final : public FReimportHandler
{
public:
    virtual bool CanReimport(UObject* Object, TArray<FString>& OutFilenames) override
    {
        return MHCanReimportManagedStaticMesh(Object, OutFilenames);
    }

    virtual void SetReimportPaths(UObject*, const TArray<FString>&) override
    {
        // Receipt provenance is authoritative. Targeted force-reimport never
        // adopts a replacement path through the generic UE file picker.
    }

    virtual EReimportResult::Type Reimport(UObject* Object) override
    {
        UE_LOG(
            LogMHStaticMeshReimport,
            Warning,
            TEXT("Managed static-mesh force-reimport is not implemented"));
        return EReimportResult::Failed;
    }
};

TUniquePtr<FMHManagedStaticMeshReimportHandler> GManagedStaticMeshReimportHandler;

} // namespace

bool MHCanReimportManagedStaticMesh(UObject* Object, TArray<FString>& OutFilenames)
{
    OutFilenames.Reset();
    FString SourcePath;
    if (!ResolveManagedSourcePath(Object, SourcePath))
    {
        return false;
    }
    OutFilenames.Add(MoveTemp(SourcePath));
    return true;
}

void MHStartupManagedStaticMeshReimportHandler()
{
    if (!GManagedStaticMeshReimportHandler.IsValid())
    {
        GManagedStaticMeshReimportHandler = MakeUnique<FMHManagedStaticMeshReimportHandler>();
    }
}

void MHShutdownManagedStaticMeshReimportHandler()
{
    GManagedStaticMeshReimportHandler.Reset();
}

#if WITH_DEV_AUTOMATION_TESTS
FReimportHandler* MHGetManagedStaticMeshReimportHandlerForTests()
{
    return GManagedStaticMeshReimportHandler.Get();
}
#endif

} // namespace UE::MimirComposite
