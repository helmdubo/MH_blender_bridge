#include "Material/MHMaterialClipboard.h"

#include "Engine/Texture.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialSourceData.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ScopedTransaction.h"
#include "Settings/MHCompositeSettings.h"
#include "StaticParameterSet.h"
#include "UObject/SoftObjectPath.h"

#define LOCTEXT_NAMESPACE "MimirCompositeMaterialClipboard"

namespace UE::MimirComposite
{
namespace
{

struct FClipboardSnapshot
{
    bool bValid = false;
    FString SourceLabel;
    FSoftObjectPath Parent;
    TArray<TPair<FMaterialParameterInfo, float>> Scalars;
    TArray<TPair<FMaterialParameterInfo, FLinearColor>> Vectors;
    TArray<TPair<FMaterialParameterInfo, FSoftObjectPath>> Textures;
    TArray<FStaticSwitchParameter> StaticSwitches;
    TArray<FStaticComponentMaskParameter> StaticComponentMasks;
    FMaterialInstanceBasePropertyOverrides BaseOverrides;
};

FClipboardSnapshot GClipboard;

/** Parameter categories the clipboard cannot carry; reported, never silently lost. */
void ReportUncopiedCategories(const UMaterialInstanceConstant& Material, TArray<FString>& OutWarnings)
{
    const auto Report = [&OutWarnings](const bool bPresent, const TCHAR* What)
    {
        if (bPresent)
        {
            OutWarnings.Add(FString::Printf(TEXT("not copied (unsupported by the MH material clipboard): %s"), What));
        }
    };
    Report(!Material.DoubleVectorParameterValues.IsEmpty(), TEXT("double vector parameters"));
    Report(!Material.FontParameterValues.IsEmpty(), TEXT("font parameters"));
    Report(!Material.TextureCollectionParameterValues.IsEmpty(), TEXT("texture collection parameters"));
    Report(!Material.RuntimeVirtualTextureParameterValues.IsEmpty(), TEXT("runtime virtual texture parameters"));
    Report(!Material.SparseVolumeTextureParameterValues.IsEmpty(), TEXT("sparse volume texture parameters"));
    Report(!Material.ParameterCollectionParameterValues.IsEmpty(), TEXT("parameter collection parameters"));
}

} // namespace

bool MHCopyMaterialDataToClipboard(
    const UMaterialInstanceConstant& Material,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (Material.Parent == nullptr)
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s has no parent material to copy"),
            *Material.GetPathName());
        return false;
    }

    FClipboardSnapshot Snapshot;
    Snapshot.SourceLabel = Material.GetPathName();
    Snapshot.Parent = FSoftObjectPath(Material.Parent);
    for (const FScalarParameterValue& Value : Material.ScalarParameterValues)
    {
#if WITH_EDITORONLY_DATA
        if (Value.AtlasData.bIsUsedAsAtlasPosition || !Value.AtlasData.Atlas.IsNull() || !Value.AtlasData.Curve.IsNull())
        {
            OutWarnings.Add(FString::Printf(
                TEXT("not copied (unsupported by the MH material clipboard): scalar atlas override '%s'"),
                *Value.ParameterInfo.Name.ToString()));
            continue;
        }
#endif
        Snapshot.Scalars.Emplace(Value.ParameterInfo, Value.ParameterValue);
    }
    for (const FVectorParameterValue& Value : Material.VectorParameterValues)
    {
        Snapshot.Vectors.Emplace(Value.ParameterInfo, Value.ParameterValue);
    }
    for (const FTextureParameterValue& Value : Material.TextureParameterValues)
    {
        Snapshot.Textures.Emplace(
            Value.ParameterInfo,
            Value.ParameterValue != nullptr ? FSoftObjectPath(Value.ParameterValue) : FSoftObjectPath());
    }

    // GetStaticParameterValues enumerates what the parent declares; the
    // instance's own overrides live in GetStaticParameters.
    const FStaticParameterSet StaticParameters = Material.GetStaticParameters();
    for (const FStaticSwitchParameter& Switch : StaticParameters.StaticSwitchParameters)
    {
        if (Switch.bOverride) Snapshot.StaticSwitches.Add(Switch);
    }
    for (const FStaticComponentMaskParameter& Mask : StaticParameters.EditorOnly.StaticComponentMaskParameters)
    {
        if (Mask.bOverride) Snapshot.StaticComponentMasks.Add(Mask);
    }
    if (StaticParameters.bHasMaterialLayers)
    {
        OutWarnings.Add(TEXT("not copied (unsupported by the MH material clipboard): material layer stack"));
    }
    Snapshot.BaseOverrides = Material.BasePropertyOverrides;
    ReportUncopiedCategories(Material, OutWarnings);

    Snapshot.bValid = true;
    GClipboard = MoveTemp(Snapshot);
    return true;
}

bool MHPasteMaterialDataFromClipboard(
    UMaterialInstanceConstant& Material,
    const UMHCompositeSettings& Settings,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutWarnings.Reset();
    OutError.Reset();
    if (!GClipboard.bValid)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the MH material clipboard is empty; copy a Material Instance first");
        return false;
    }
    UMaterialInterface* Parent = Cast<UMaterialInterface>(GClipboard.Parent.TryLoad());
    if (Parent == nullptr)
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: copied parent material %s is unavailable"),
            *GClipboard.Parent.ToString());
        return false;
    }
    if (Parent == &Material)
    {
        OutError = FString::Printf(
            TEXT("MH_E_INVALID_RESOURCE_SOURCE: %s cannot become its own parent"),
            *Material.GetPathName());
        return false;
    }

    const FScopedTransaction Transaction(LOCTEXT("PasteMaterialData", "Paste MH Material Data"));
    Material.Modify();
    Material.SetParentEditorOnly(Parent, false);
    Material.ClearParameterValuesEditorOnly();

    // Static state first: it recompiles the permutation the value overrides
    // below are applied to. A minimal set replaces the target's static state
    // wholesale, so nothing of the previous material survives the paste.
    FStaticParameterSet StaticParameters;
    StaticParameters.StaticSwitchParameters = GClipboard.StaticSwitches;
    StaticParameters.EditorOnly.StaticComponentMaskParameters = GClipboard.StaticComponentMasks;
    FMaterialInstanceBasePropertyOverrides BaseOverrides = GClipboard.BaseOverrides;
    Material.UpdateStaticPermutation(StaticParameters, BaseOverrides);

    for (const TPair<FMaterialParameterInfo, float>& Scalar : GClipboard.Scalars)
    {
        Material.SetScalarParameterValueEditorOnly(Scalar.Key, Scalar.Value);
    }
    for (const TPair<FMaterialParameterInfo, FLinearColor>& Vector : GClipboard.Vectors)
    {
        Material.SetVectorParameterValueEditorOnly(Vector.Key, Vector.Value);
    }
    for (const TPair<FMaterialParameterInfo, FSoftObjectPath>& Texture : GClipboard.Textures)
    {
        UTexture* Object = Texture.Value.IsNull() ? nullptr : Cast<UTexture>(Texture.Value.TryLoad());
        if (Object == nullptr && !Texture.Value.IsNull())
        {
            OutWarnings.Add(FString::Printf(
                TEXT("texture override '%s' is unset: %s is unavailable"),
                *Texture.Key.Name.ToString(),
                *Texture.Value.ToString()));
        }
        Material.SetTextureParameterValueEditorOnly(Texture.Key, Object);
    }
    Material.PostEditChange();
    Material.MarkPackageDirty();

    // A managed target now differs from its .material source. Say so, and say
    // whether it can still be published back: a parent outside MasterRoot or
    // LibraryRoot leaves the asset outside the source protocol.
    if (Cast<UMHMaterialSourceData>(Material.GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass())) != nullptr)
    {
        FString LocalWarning;
        if (MHDetectManagedMaterialLocalModification(Material, Settings, LocalWarning))
        {
            OutWarnings.Add(MoveTemp(LocalWarning));
        }
        FMHMaterialDocument Document;
        FString ExtractError;
        TArray<FString> ExtractWarnings;
        if (!MHExtractMaterialV4(Material, Settings, Document, ExtractError, &ExtractWarnings))
        {
            OutWarnings.Add(FString::Printf(
                TEXT("%s can no longer be published to MH Source: %s"),
                *Material.GetPathName(),
                *ExtractError));
        }
        else
        {
            OutWarnings.Append(ExtractWarnings);
        }
    }
    return true;
}

bool MHHasMaterialClipboardData()
{
    return GClipboard.bValid;
}

FString MHGetMaterialClipboardSourceLabel()
{
    return GClipboard.bValid ? GClipboard.SourceLabel : FString();
}

void MHClearMaterialClipboard()
{
    GClipboard = FClipboardSnapshot();
}

} // namespace UE::MimirComposite

#undef LOCTEXT_NAMESPACE
