#include "Settings/MHCompositeSettings.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHCompositeSettings)

UMHCompositeSettings::UMHCompositeSettings()
{
    ContentRoot = TEXT("/Game/MH");
    MasterRoot = TEXT("/Game/MH/Masters");
    StaticMeshPrefix = TEXT("SM_");
    MaterialInstancePrefix = TEXT("MI_");
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

FString UMHCompositeSettings::GetEffectiveTextureRootPath() const
{
    FString Path = TextureRoot.Path;
    Path.TrimStartAndEndInline();
    return Path.IsEmpty() ? GetSourceRootPath() : Path;
}
