#include "Settings/MHCompositeSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeSettings)

UMHCompositeSettings::UMHCompositeSettings()
{
    MasterRoot = TEXT("/Game/Mimir/MasterMaterials");
    LibraryRoot = TEXT("/Game/Mimir/MaterialLibrary");
    PhysicalMaterialRoot = TEXT("/Game/PhysicalMaterials");
    StaticMeshPrefix = TEXT("SM_");
    TexturePrefix = TEXT("T_");
    CompositeAssetPrefix = TEXT("CA_");
}

FName UMHCompositeSettings::GetCategoryName() const
{
    return FName(TEXT("Plugins"));
}

FName UMHCompositeSettings::GetSectionName() const
{
    return FName(TEXT("Mimir Composite"));
}

FString UMHCompositeSettings::GetSourceRootPath() const
{
    FString Path = SourceRoot.Path;
    Path.TrimStartAndEndInline();
    return Path;
}
