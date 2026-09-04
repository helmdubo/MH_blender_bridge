#include "Material/MHMaterialClipboard.h"

#include "Engine/Texture.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialSourceData.h"
#include "MaterialShared.h"
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
    // A parent swap plus a static-permutation change is only safe inside a
    // FMaterialUpdateContext: its constructor syncs with the rendering thread and
    // drops the render state of every component, its destructor updates the
    // material resources and re-registers them. Without it the render thread kept
    // reading resources of the previous parent (owner crash 2026-09-04,
    // EXCEPTION_ACCESS_VIOLATION in D3D12RHI). This mirrors
    // UMaterialEditorInstanceConstant::UpdateSourceInstanceParent.
    {
        FMaterialUpdateContext UpdateContext;
        // Inside it, the editor's own order: swap the parent with a shader recache,
        // then one parameter context that clears every override, receives the values
        // and rebuilds the static permutation once when it closes.
        Material.SetParentEditorOnly(Parent);
        {
            FMaterialInstanceParameterUpdateContext ParameterContext(&Material, EMaterialInstanceClearParameterFlag::All);
            FStaticParameterSet& StaticParameters = ParameterContext.GetStaticParameters();
            StaticParameters.StaticSwitchParameters = GClipboard.StaticSwitches;
            StaticParameters.EditorOnly.StaticComponentMaskParameters = GClipboard.StaticComponentMasks;
            ParameterContext.SetBasePropertyOverrides(GClipboard.BaseOverrides);
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
                if (Object == nullptr)
                {
                    // A null texture override is never written: the renderer would
                    // have to substitute for it. The slot falls back to the parent.
                    OutWarnings.Add(FString::Printf(
                        TEXT("texture override '%s' left at the parent value: %s"),
                        *Texture.Key.Name.ToString(),
                        Texture.Value.IsNull() ? TEXT("the donor had no texture there") : *Texture.Value.ToString()));
                    continue;
                }
                Material.SetTextureParameterValueEditorOnly(Texture.Key, Object);
            }
        }
        Material.PostEditChange();
        UpdateContext.AddMaterialInstance(&Material);
    }
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
