#include "Wind/MHPivotWindSetupCommandlet.h"

#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"
#include "FileHelpers.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialShared.h"
#include "Math/Float16Color.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "Texture/MHTextureImporter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Wind/MHPivotWindController.h"
#include "Wind/MHDagorWindNoise.h"
#include "Wind/MHPivotWindMaterial.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHPivotWindSetup, Log, All);

namespace
{
const TCHAR* AssetNames[] = {
    TEXT("MPC_MHWind"), TEXT("MF_MHPivotWind_v1"),
    TEXT("T_MHPivotDefaultPos_v1"), TEXT("T_MHPivotDefaultDir_v1"),
    TEXT("BP_MHWindController"), TEXT("T_MHWindNoise_v2")};
const FName DirectionName(TEXT("MH_WindDirectionSpeed"));
const FName NoiseName(TEXT("MH_WindNoise"));
const FName TreeName(TEXT("MH_WindTree"));
const FLinearColor DirectionDefault(1, 0, 0, 0);
const FLinearColor NoiseDefault(1, 1.0f / 70.0f, 0.5f, 2);
const FLinearColor TreeDefault(0.1f, 0.1f, 0, 0);

bool ValidRoot(const FString& Root)
{
    return Root.StartsWith(TEXT("/Game/")) && !Root.EndsWith(TEXT("/")) &&
        FPackageName::IsValidLongPackageName(Root);
}

FString AssetPath(const FString& Root, const TCHAR* Name)
{
    return Root / Name + TEXT(".") + Name;
}

TArray64<uint8> DefaultPixels(const bool bPosition)
{
    TArray64<uint8> Pixels;
    Pixels.SetNumZeroed(32 * 64 * (bPosition ? 8 : 4));
    if (bPosition)
    {
        // An unbound atlas must disable deformation even if is_pivoted is on.
        const FFloat16Color Value(FLinearColor(0, 0, 0, -1));
        for (int32 Index = 0; Index < 32 * 64; ++Index)
            FMemory::Memcpy(Pixels.GetData() + Index * sizeof(Value), &Value, sizeof(Value));
    }
    else
    {
        const FColor Value(128, 255, 128, 0);
        for (int32 Index = 0; Index < 32 * 64; ++Index)
        {
            FMemory::Memcpy(Pixels.GetData() + Index * sizeof(FColor), &Value, sizeof(FColor));
        }
    }
    return Pixels;
}

bool InitializeTexture(UTexture2D& Texture, const bool bPosition, FString& Error)
{
    const TArray64<uint8> Pixels = DefaultPixels(bPosition);
    Texture.PreEditChange(nullptr);
    Texture.Source.Init(32, 64, 1, 1, bPosition ? TSF_RGBA16F : TSF_BGRA8, Pixels.GetData());
    return UE::MimirComposite::MHTextureApplyManagedPivotSettings(
        Texture,
        bPosition
            ? UE::MimirComposite::EMHPivotTextureKind::Position
            : UE::MimirComposite::EMHPivotTextureKind::Direction,
        Error);
}

bool ValidateTexture(UTexture2D& Texture, const bool bPosition, FString& Error)
{
    const UE::MimirComposite::EMHPivotTextureKind Kind = bPosition
        ? UE::MimirComposite::EMHPivotTextureKind::Position
        : UE::MimirComposite::EMHPivotTextureKind::Direction;
    TArray64<uint8> Pixels;
    if (!UE::MimirComposite::MHValidatePivotTextureSource(Texture, Kind, Error))
    {
        Error += TEXT("; existing asset preserved");
        return false;
    }
    if (!UE::MimirComposite::MHTextureHasManagedPivotSettings(Texture, Kind))
    {
        Error = Texture.GetPathName() +
            TEXT(": lossless pivot-atlas build or sampling policy differs; existing asset preserved");
        return false;
    }
    if (!Texture.Source.GetMipData(Pixels, 0) || Pixels != DefaultPixels(bPosition))
    {
        Error = Texture.GetPathName() + TEXT(": default atlas contents differ; existing asset preserved");
        return false;
    }
    return true;
}

bool PreflightMaster(UMaterial& Material, UMaterialFunction* Function, FString& Error)
{
    const UMaterialEditorOnlyData* Data = Material.GetEditorOnlyData();
    if (Data == nullptr || Material.bUseMaterialAttributes || Material.MaterialDomain != MD_Surface)
    {
        Error = Material.GetPathName() + TEXT(": requires a surface master with individual property inputs");
        return false;
    }
    if (Data->WorldPositionOffset.Expression != nullptr)
    {
        if (Function == nullptr ||
            !UE::MimirComposite::MHValidatePivotWindAttachment(Material, *Function, Error))
        {
            if (Error.IsEmpty())
                Error = Material.GetPathName() + TEXT(": existing WPO must be composed explicitly; no masters were changed");
            return false;
        }
    }
    return true;
}

bool ValidateCompilation(UMaterial& Material, FString& Error)
{
    FMaterialResource* Resource = Material.GetMaterialResource(GMaxRHIShaderPlatform);
    if (Resource == nullptr)
    {
        Error = Material.GetPathName() + TEXT(": no compiled material resource; run with rendering enabled");
        return false;
    }
    // Finish the resource's asynchronous cache request as well as submitted
    // shader jobs; a global asset barrier alone may precede cache completion.
    Resource->FinishCompilation();
    if (!Resource->GetCompileErrors().IsEmpty())
    {
        Error = Material.GetPathName() + TEXT(": ") + FString::Join(Resource->GetCompileErrors(), TEXT("\n"));
        return false;
    }
    if (!Resource->HasValidGameThreadShaderMap() || !Resource->IsGameThreadShaderMapComplete())
    {
        Error = Material.GetPathName() + TEXT(": shader map is missing or incomplete; no packages saved");
        return false;
    }
    return true;
}
}

UMHPivotWindSetupCommandlet::UMHPivotWindSetupCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
}

int32 UMHPivotWindSetupCommandlet::Main(const FString& Params)
{
    FString DestinationRoot;
    FString MasterRoot;
    FParse::Value(*Params, TEXT("DestinationRoot="), DestinationRoot);
    const bool bAttachMasters = FParse::Value(*Params, TEXT("MasterRoot="), MasterRoot);
    const bool bAllowUpgrade = FParse::Param(*Params, TEXT("Upgrade"));
    const auto Fail = [](const FString& Error)
    {
        UE_LOG(LogMHPivotWindSetup, Error, TEXT("%s"), *Error);
        return 1;
    };
    if (!ValidRoot(DestinationRoot) || (bAttachMasters && !ValidRoot(MasterRoot)))
        return Fail(TEXT("Specify -DestinationRoot=/Game/... and optionally -MasterRoot=/Game/... without trailing slashes"));
    if (IsRunningCommandlet() && (!IsAllowCommandletRendering() || GUsingNullRHI))
        return Fail(TEXT("Shader validation requires -AllowCommandletRendering and a real RHI; no assets changed"));

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MimirComposite"));
    FString Code;
    if (!Plugin.IsValid() || !FFileHelper::LoadFileToString(Code,
        *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/MaterialFunctions/MHPivotWind.hlsl"))))
        return Fail(TEXT("Plugin pivot wind HLSL is missing; no assets changed"));

    TArray<TStrongObjectPtr<UObject>> KeepAlive;
    UObject* Assets[UE_ARRAY_COUNT(AssetNames)] = {};
    int32 ExistingCount = 0;
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(AssetNames); ++Index)
    {
        const FString PackageName = DestinationRoot / AssetNames[Index];
        const FString Path = AssetPath(DestinationRoot, AssetNames[Index]);
        Assets[Index] = FindObject<UObject>(nullptr, *Path);
        if (Assets[Index] || FPackageName::DoesPackageExist(PackageName))
        {
            if (!Assets[Index]) Assets[Index] = LoadObject<UObject>(nullptr, *Path);
            if (Assets[Index] == nullptr) return Fail(PackageName + TEXT(": existing package has no expected asset"));
            KeepAlive.Emplace(Assets[Index]);
            ++ExistingCount;
        }
    }
    const bool bLegacy = ExistingCount == 5 && Assets[5] == nullptr &&
        Assets[0] && Assets[1] && Assets[2] && Assets[3] && Assets[4];
    if (ExistingCount != 0 && ExistingCount != UE_ARRAY_COUNT(AssetNames) && !bLegacy)
        return Fail(TEXT("Destination contains a partial wind resource set. Choose an unused destination or restore the complete set; no assets changed"));
    if (bLegacy && (!bAllowUpgrade || !bAttachMasters))
        return Fail(TEXT("The original five-asset v1 wind set requires -Upgrade and -MasterRoot=/Game/... so both masters are protected and recompiled; no assets changed"));

    auto* Collection = Cast<UMaterialParameterCollection>(Assets[0]);
    auto* Function = Cast<UMaterialFunction>(Assets[1]);
    auto* Position = Cast<UTexture2D>(Assets[2]);
    auto* Direction = Cast<UTexture2D>(Assets[3]);
    auto* Blueprint = Cast<UBlueprint>(Assets[4]);
    auto* NoiseVolume = Cast<UVolumeTexture>(Assets[5]);
    FString Error;
    if (ExistingCount != 0)
    {
        if (!Collection || !Function || !Position || !Direction || !Blueprint || (!bLegacy && !NoiseVolume))
            return Fail(TEXT("An existing wind resource has the wrong asset class; no assets changed"));
        const FCollectionVectorParameter* Flow = Collection->GetVectorParameterByName(DirectionName);
        const FCollectionVectorParameter* Noise = Collection->GetVectorParameterByName(NoiseName);
        const FCollectionVectorParameter* Tree = Collection->GetVectorParameterByName(TreeName);
        if (Collection->VectorParameters.Num() != (bLegacy ? 2 : 3) || !Collection->ScalarParameters.IsEmpty() ||
            !Flow || !Noise || Flow->DefaultValue != DirectionDefault || Noise->DefaultValue != NoiseDefault ||
            !Flow->Id.IsValid() || !Noise->Id.IsValid() ||
            (!bLegacy && (!Tree || Tree->DefaultValue != TreeDefault || !Tree->Id.IsValid())))
            return Fail(Collection->GetPathName() + TEXT(": parameter names/defaults differ; existing collection preserved"));
        if (!ValidateTexture(*Position, true, Error) || !ValidateTexture(*Direction, false, Error)) return Fail(Error);
        if (bLegacy)
        {
            if (!UE::MimirComposite::MHValidateLegacyPivotWindFunction(
                *Function, *Collection, *Position, *Direction, Error)) return Fail(Error);
        }
        else if (!UE::MimirComposite::MHValidatePivotWindFunction(
            *Function, *Collection, *Position, *Direction, *NoiseVolume, Error)) return Fail(Error);
        const auto* Defaults = Blueprint->GeneratedClass
            ? Cast<AMHPivotWindController>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
        if (Blueprint->ParentClass != AMHPivotWindController::StaticClass() || !Defaults ||
            Defaults->WindParameters != Collection || Blueprint->Status == BS_Error)
            return Fail(Blueprint->GetPathName() + TEXT(": controller parent or collection binding differs; Blueprint preserved"));
    }

    TArray<UMaterial*> Masters;
    if (bAttachMasters)
    {
        for (const TCHAR* Name : {TEXT("rendinst_tree_colored"), TEXT("rendinst_tree_colored_alpha_split")})
        {
            const FString Path = AssetPath(MasterRoot, Name);
            if (!FPackageName::DoesPackageExist(MasterRoot / Name)) return Fail(Path + TEXT(": required master is missing"));
            UMaterial* Material = LoadObject<UMaterial>(nullptr, *Path);
            if (!Material) return Fail(Path + TEXT(": expected a UMaterial master"));
            KeepAlive.Emplace(Material);
            if (!PreflightMaster(*Material, Function, Error)) return Fail(Error);
            Masters.Add(Material);
        }
    }

    TArray<UPackage*> ChangedPackages;
    const auto NewAsset = [&](UClass* Class, const int32 Index)
    {
        UPackage* Package = CreatePackage(*(DestinationRoot / AssetNames[Index]));
        UObject* Asset = NewObject<UObject>(Package, Class, FName(AssetNames[Index]), RF_Public | RF_Standalone | RF_Transactional);
        KeepAlive.Emplace(Asset);
        ChangedPackages.AddUnique(Package);
        return Asset;
    };
    if (ExistingCount == 0)
    {
        Collection = CastChecked<UMaterialParameterCollection>(NewAsset(UMaterialParameterCollection::StaticClass(), 0));
        Collection->PreEditChange(nullptr);
        for (const auto& Entry : {TPair<FName,FLinearColor>(DirectionName, DirectionDefault),
            TPair<FName,FLinearColor>(NoiseName, NoiseDefault), TPair<FName,FLinearColor>(TreeName, TreeDefault)})
        {
            auto& Parameter = Collection->VectorParameters.AddDefaulted_GetRef();
            Parameter.ParameterName = Entry.Key;
            Parameter.DefaultValue = Entry.Value;
            Parameter.Id = FGuid::NewGuid();
        }
        Collection->PostEditChange();
        Position = CastChecked<UTexture2D>(NewAsset(UTexture2D::StaticClass(), 2));
        Direction = CastChecked<UTexture2D>(NewAsset(UTexture2D::StaticClass(), 3));
        if (!InitializeTexture(*Position, true, Error) || !InitializeTexture(*Direction, false, Error))
            return Fail(Error);
        Function = CastChecked<UMaterialFunction>(NewAsset(UMaterialFunction::StaticClass(), 1));
        UPackage* Package = CreatePackage(*(DestinationRoot / AssetNames[4]));
        Blueprint = FKismetEditorUtilities::CreateBlueprint(AMHPivotWindController::StaticClass(), Package,
            FName(AssetNames[4]), BPTYPE_Normal, FName(TEXT("MHPivotWindSetup")));
        if (!Blueprint) return Fail(TEXT("Could not create controller Blueprint"));
        KeepAlive.Emplace(Blueprint);
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass) return Fail(TEXT("Controller Blueprint compilation failed"));
        auto* Defaults = Cast<AMHPivotWindController>(Blueprint->GeneratedClass->GetDefaultObject());
        if (!Defaults) return Fail(TEXT("Controller Blueprint has no expected default object"));
        Defaults->WindParameters = Collection;
        Blueprint->MarkPackageDirty();
        ChangedPackages.AddUnique(Package);
        UObject* CreatedAssets[] = {Collection, Function, Position, Direction, Blueprint};
        for (UObject* Asset : CreatedAssets)
        {
            FAssetRegistryModule::AssetCreated(Asset);
            Asset->MarkPackageDirty();
        }
    }
    if (ExistingCount == 0 || bLegacy)
    {
        NoiseVolume = CastChecked<UVolumeTexture>(NewAsset(UVolumeTexture::StaticClass(), 5));
        if (!UE::MimirComposite::MHBuildDagorWindNoise(*NoiseVolume, Error)) return Fail(Error);
        FAssetRegistryModule::AssetCreated(NoiseVolume);
        NoiseVolume->MarkPackageDirty();
    }

    // Texture completion can invalidate dependent materials. Finish it before
    // compiling the newly attached wind graph, including on an existing set.
    FAssetCompilingManager::Get().FinishAllCompilation();

    // A transient consumer validates generated HLSL even without MasterRoot.
    // Pin all assets across material compilation, which may collect garbage.
    UMaterial* Probe = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
    KeepAlive.Emplace(Probe);
    {
        FMaterialUpdateContext Update;
        Update.AddMaterial(Probe);
        // The outer context must cover every existing consumer BEFORE changing
        // either the collection layout or the material function resource.
        for (UMaterial* Material : Masters) Update.AddMaterial(Material);
        if (bLegacy)
        {
            Collection->Modify();
            Collection->PreEditChange(nullptr);
            FCollectionVectorParameter& Tree = Collection->VectorParameters.AddDefaulted_GetRef();
            Tree.ParameterName = TreeName;
            Tree.DefaultValue = TreeDefault;
            Tree.Id = FGuid::NewGuid();
            Collection->PostEditChange();
            Collection->MarkPackageDirty();
            ChangedPackages.AddUnique(Collection->GetOutermost());
            if (!UE::MimirComposite::MHUpgradePivotWindFunction(
                *Function, *Collection, *Position, *Direction, *NoiseVolume, Error)) return Fail(Error);
            ChangedPackages.AddUnique(Function->GetOutermost());
        }
        else if (ExistingCount == 0)
        {
            if (!UE::MimirComposite::MHBuildPivotWindFunction(
                *Function, *Collection, *Position, *Direction, *NoiseVolume, Error)) return Fail(Error);
        }
        if (ExistingCount == 0 || bLegacy)
        {
            // PostEditChange alone does not advance a material function's
            // dependency StateId in UE 5.7. The native path also invalidates
            // loaded consumers under this same protected update context.
            // Probe is pinned but still empty; attach it after the revision
            // changes so its first shader map sees the complete new function.
            Function->ForceRecompileForRendering(Update, Probe);
        }
        if (!UE::MimirComposite::MHAttachPivotWind(*Probe, *Function, Error)) return Fail(Error);
        for (UMaterial* Material : Masters)
        {
            const bool bAlreadyAttached = Material->GetEditorOnlyData()->WorldPositionOffset.Expression != nullptr;
            if (bAlreadyAttached && !bLegacy) continue;
            if (bAlreadyAttached)
            {
                // Attachment and parameter identity stay intact; only the
                // function implementation changed. Explicitly invalidate the
                // old shader maps and cache the newly referenced noise volume.
                Material->Modify();
                Material->UpdateCachedExpressionData();
                Material->PostEditChange();
                Material->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
                Material->MarkPackageDirty();
            }
            else if (!UE::MimirComposite::MHAttachPivotWind(*Material, *Function, Error)) return Fail(Error);
            ChangedPackages.AddUnique(Material->GetOutermost());
        }
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    if (!UE::MimirComposite::MHValidatePivotWindFunction(
        *Function, *Collection, *Position, *Direction, *NoiseVolume, Error)) return Fail(Error);
    if (!ValidateCompilation(*Probe, Error)) return Fail(Error);
    for (UMaterial* Material : Masters)
        if (!UE::MimirComposite::MHValidatePivotWindAttachment(*Material, *Function, Error) ||
            !ValidateCompilation(*Material, Error)) return Fail(Error);
    if (!ChangedPackages.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages(ChangedPackages, true))
        return Fail(TEXT("Saving wind assets failed; inspect package save errors for any partially saved files"));
    UE_LOG(LogMHPivotWindSetup, Display, TEXT("Wind setup validated: %s; %d changed packages saved, %d masters checked"),
        *DestinationRoot, ChangedPackages.Num(), Masters.Num());
    return 0;
}
