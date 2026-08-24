#include "Material/MHMaterialImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AssetCompilingManager.h"
#include "Engine/AssetUserData.h"
#include "Engine/Texture.h"
#include "EditorFramework/AssetImportData.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Material/MHMaterialSourceData.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceResolver.h"
#include "StaticParameterSet.h"
#include "Texture/MHTextureSourceData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetImportTask.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHMaterialPublish, Display, All);
DEFINE_LOG_CATEGORY_STATIC(LogMHMaterialImport, Display, All);

namespace UE::MimirComposite
{
namespace
{

constexpr const TCHAR* GeneratedMaterialRoot = TEXT("/Game/MH/Generated/Materials");
constexpr const TCHAR* GeneratedTextureRoot = TEXT("/Game/MH/Generated/Textures");

#if WITH_DEV_AUTOMATION_TESTS
TFunction<void()> GBeforeMaterialSourceCommitTestHook;
bool GFailNextMaterialPackageSaveForTests = false;
#endif

bool IsGlobal(const FMaterialParameterInfo& Info)
{
    return Info.Association == EMaterialParameterAssociation::GlobalParameter && Info.Index == INDEX_NONE;
}

bool IsTextureSlotName(const FString& Name, int32& OutSlot)
{
    if (!Name.StartsWith(TEXT("tex"), ESearchCase::CaseSensitive))
    {
        return false;
    }
    const FString Digits = Name.Mid(3);
    if (Digits.IsEmpty() || (Digits.Len() > 1 && Digits[0] == TEXT('0')))
    {
        return false;
    }
    return LexTryParseString(OutSlot, *Digits) && OutSlot >= 0 && OutSlot <= 15 &&
        FString::Printf(TEXT("tex%d"), OutSlot) == Name;
}

FString NormalizePackageRoot(FString Root)
{
    Root.TrimStartAndEndInline();
    while (Root.EndsWith(TEXT("/")))
    {
        Root.LeftChopInline(1);
    }
    return Root;
}

FString ParentPackageName(const FString& Root, const FString& Token)
{
    return NormalizePackageRoot(Root) + TEXT("/") + Token;
}

FString AppliedParentReceipt(const FMHMaterialDocument& Document)
{
    return FString(Document.Mode == EMHMaterialMode::Class ? TEXT("class:") : TEXT("library:")) + Document.Parent;
}

UMaterialInterface* LoadParent(const FString& Root, const FString& Token, FString& OutError)
{
    const FString PackageName = ParentPackageName(Root, Token);
    if (!FPackageName::IsValidLongPackageName(PackageName))
    {
        OutError = FString::Printf(
            TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: invalid parent registry package %s"),
            *PackageName);
        return nullptr;
    }
    const FString ObjectPath = PackageName + TEXT(".") + Token;
    UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ObjectPath);
    if (Parent == nullptr)
    {
        OutError = FString::Printf(
            TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: parent registry entry does not resolve: %s"),
            *ObjectPath);
    }
    return Parent;
}

bool MatchParent(
    const UMaterialInterface& Parent,
    const UMHCompositeSettings& Settings,
    EMHMaterialMode& OutMode,
    FString& OutToken)
{
    const FString PackageName = Parent.GetOutermost()->GetName();
    for (const TPair<EMHMaterialMode, FString> Candidate : {
        TPair<EMHMaterialMode, FString>(EMHMaterialMode::Class, Settings.MasterRoot),
        TPair<EMHMaterialMode, FString>(EMHMaterialMode::Library, Settings.LibraryRoot)})
    {
        const FString Root = NormalizePackageRoot(Candidate.Value);
        const FString Prefix = Root + TEXT("/");
        if (PackageName.StartsWith(Prefix, ESearchCase::CaseSensitive))
        {
            const FString Token = PackageName.Mid(Prefix.Len());
            if (!Token.Contains(TEXT("/")) && MHIsCanonicalMaterialToken(Token) && Parent.GetName() == Token)
            {
                OutMode = Candidate.Key;
                OutToken = Token;
                return true;
            }
        }
    }
    return false;
}

bool HasUnsupportedBaseOverride(const FMaterialInstanceBasePropertyOverrides& Overrides)
{
    return Overrides.bOverride_OpacityMaskClipValue ||
        Overrides.bOverride_BlendMode ||
        Overrides.bOverride_ShadingModel ||
        Overrides.bOverride_DitheredLODTransition ||
        Overrides.bOverride_CastDynamicShadowAsMasked ||
        Overrides.bOverride_bIsThinSurface ||
        Overrides.bOverride_OutputTranslucentVelocity ||
        Overrides.bOverride_bHasPixelAnimation ||
        Overrides.bOverride_bEnableTessellation ||
        Overrides.bOverride_DisplacementScaling ||
        Overrides.bOverride_bEnableDisplacementFade ||
        Overrides.bOverride_DisplacementFadeRange ||
        Overrides.bOverride_MaxWorldPositionOffsetDisplacement ||
        Overrides.bOverride_CompatibleWithLumenCardSharing;
}

bool HasAnyOverride(const UMaterialInstanceConstant& Material)
{
    return !Material.ScalarParameterValues.IsEmpty() ||
        !Material.VectorParameterValues.IsEmpty() ||
        !Material.DoubleVectorParameterValues.IsEmpty() ||
        !Material.TextureParameterValues.IsEmpty() ||
        !Material.TextureCollectionParameterValues.IsEmpty() ||
        !Material.ParameterCollectionParameterValues.IsEmpty() ||
        !Material.RuntimeVirtualTextureParameterValues.IsEmpty() ||
        !Material.SparseVolumeTextureParameterValues.IsEmpty() ||
        !Material.FontParameterValues.IsEmpty() ||
        Material.HasStaticParameters() ||
        Material.HasOverridenBaseProperties();
}

bool LoadBytes(const FString& Path, TArray<uint8>& OutBytes, FString& OutError)
{
    if (!FFileHelper::LoadFileToArray(OutBytes, *Path))
    {
        OutError = FString::Printf(TEXT("MH_E_MATERIAL_GRAMMAR: cannot read material payload %s"), *Path);
        return false;
    }
    return true;
}

bool RelativeToRoot(const FString& Root, const FString& Path, FString& OutRelative);

UTexture* ImportTexture(
    const FString& LogicalName,
    const FString& SourcePath,
    const FString& SourceRelativePath,
    const FString& ExpectedRawHash,
    FString& OutError)
{
    const FString PackageName = FString(GeneratedTextureRoot) + TEXT("/") + LogicalName;
    const FString ObjectPath = PackageName + TEXT(".") + LogicalName;

    TArray<uint8> PreImportSourceBytes;
    if (ExpectedRawHash.IsEmpty() ||
        !FFileHelper::LoadFileToArray(PreImportSourceBytes, *SourcePath) ||
        MHRawPayloadHash(PreImportSourceBytes) != ExpectedRawHash)
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: texture:%s changed before import task"),
            *LogicalName);
        return nullptr;
    }

    UAssetImportTask* Task = NewObject<UAssetImportTask>();
    Task->Filename = SourcePath;
    Task->DestinationPath = GeneratedTextureRoot;
    Task->DestinationName = LogicalName;
    Task->bAutomated = true;
    Task->bReplaceExisting = true;
    Task->bReplaceExistingSettings = false;
    // Persist only after the task result and its exact source receipt validate;
    // Interchange may otherwise return and save a stale pre-existing object.
    Task->bSave = false;
    Task->bAsync = false;

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    TArray<UAssetImportTask*> Tasks = {Task};
    AssetToolsModule.Get().ImportAssetTasks(Tasks);

    TArray<uint8> PostImportSourceBytes;
    if (!FFileHelper::LoadFileToArray(PostImportSourceBytes, *SourcePath) ||
        PostImportSourceBytes != PreImportSourceBytes ||
        MHRawPayloadHash(PostImportSourceBytes) != ExpectedRawHash)
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: texture:%s changed during import task"),
            *LogicalName);
        return nullptr;
    }

    UTexture* Texture = nullptr;
    for (UObject* Imported : Task->GetObjects())
    {
        if (UTexture* Candidate = Cast<UTexture>(Imported))
        {
            if (Candidate->GetPathName().Equals(ObjectPath, ESearchCase::CaseSensitive))
            {
                if (Texture != nullptr && Texture != Candidate)
                {
                    OutError = FString::Printf(
                        TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: current import returned duplicate exact objects for texture:%s"),
                        *LogicalName);
                    return nullptr;
                }
                Texture = Candidate;
            }
        }
    }
    if (Texture == nullptr)
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: texture:%s resolved to source but UE import failed: %s"),
            *LogicalName,
            *SourcePath);
        return nullptr;
    }

#if WITH_EDITORONLY_DATA
    const UAssetImportData* ImportData = Texture->AssetImportData;
    const FMD5Hash CurrentSourceHash = FMD5Hash::HashFile(*SourcePath);
    if (ImportData == nullptr || ImportData->GetSourceFileCount() != 1 ||
        !FPaths::IsSamePath(ImportData->GetFirstFilename(), SourcePath) ||
        !CurrentSourceHash.IsValid() ||
        ImportData->GetSourceData().SourceFiles[0].FileHash != CurrentSourceHash)
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: current import did not record exact source bytes for texture:%s"),
            *LogicalName);
        return nullptr;
    }
#else
    OutError = TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: texture import receipts require editor-only data");
    return nullptr;
#endif

    FAssetCompilingManager::Get().FinishAllCompilation();
    UPackage* Package = Texture->GetOutermost();
    Package->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const FString Filename = FPackageName::LongPackageNameToFilename(
        PackageName,
        FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Package, Texture, *Filename, Args))
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: imported texture:%s could not be persisted"),
            *LogicalName);
        return nullptr;
    }
    if (Package->HasAnyPackageFlags(PKG_InMemoryOnly))
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: persisted texture:%s remained in-memory-only"),
            *LogicalName);
        return nullptr;
    }

    UMHTextureSourceData* SourceData = Cast<UMHTextureSourceData>(
        Texture->GetAssetUserDataOfClass(UMHTextureSourceData::StaticClass()));
    if (SourceData == nullptr)
    {
        SourceData = NewObject<UMHTextureSourceData>(Texture, NAME_None, RF_Transactional);
        Texture->AddAssetUserData(SourceData);
    }
    SourceData->LogicalName = LogicalName;
    SourceData->SourceRelativePath = SourceRelativePath;
    SourceData->SourceHash = ExpectedRawHash;
    Texture->PostEditChange();
    Package->MarkPackageDirty();
    if (!UPackage::SavePackage(Package, Texture, *Filename, Args))
    {
        OutError = FString::Printf(
            TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: imported texture:%s receipt could not be persisted"),
            *LogicalName);
        return nullptr;
    }
    return Texture;
}

bool ResolveTextures(
    const FMHMaterialDocument& Document,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    TMap<FString, UTexture*>& OutTextures,
    TArray<FString>& OutWarnings,
    FString& OutError)
{
    OutTextures.Reset();
    TSet<FString> Unique;
    for (const TPair<int32, FString>& Pair : Document.Textures)
    {
        Unique.Add(Pair.Value);
    }
    TArray<FString> Names = Unique.Array();
    Names.Sort();
    for (const FString& Name : Names)
    {
        FMHResourceKey Key;
        Key.Kind = EMHResourceKind::Texture;
        Key.LogicalName = Name;
        const FMHResolveOutcome Outcome = Resolver.Resolve(Key);
        if (Outcome.Status == EMHResolveStatus::Unresolved)
        {
            OutError = FString::Printf(
                TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: no source texture for texture:%s"),
                *Name);
            return false;
        }
        if (Outcome.Status != EMHResolveStatus::Resolved)
        {
            OutError = Outcome.Diagnostic;
            return false;
        }
        FString SourceRelativePath;
        if (!RelativeToRoot(SourceRoot, Outcome.PayloadPath, SourceRelativePath))
        {
            OutError = FString::Printf(
                TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: texture:%s resolved outside source_root"),
                *Name);
            return false;
        }
        UTexture* Texture = ImportTexture(
            Name,
            Outcome.PayloadPath,
            SourceRelativePath,
            Outcome.RawHash,
            OutError);
        if (Texture == nullptr)
        {
            return false;
        }
        OutTextures.Add(Name, Texture);
        FString RebindEvent;
        if (MHConsumeOrphanRebindEvent(SourceRoot, Key, RebindEvent))
        {
            OutWarnings.Add(RebindEvent);
            UE_LOG(LogMHMaterialImport, Warning, TEXT("%s"), *RebindEvent);
        }
    }
    return true;
}

bool SaveMaterialPackage(UMaterialInstanceConstant& Material, FString& OutError)
{
    UPackage* Package = Material.GetOutermost();
    const FString PackageName = Package->GetName();
    if (!FPackageName::IsValidLongPackageName(PackageName))
    {
        OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: material has no persistent package");
        return false;
    }
#if WITH_DEV_AUTOMATION_TESTS
    if (GFailNextMaterialPackageSaveForTests)
    {
        GFailNextMaterialPackageSaveForTests = false;
        OutError = FString::Printf(
            TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: test-injected package save failure for %s"),
            *PackageName);
        return false;
    }
#endif
    Package->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const FString Filename = FPackageName::LongPackageNameToFilename(
        PackageName,
        FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Package, &Material, *Filename, Args))
    {
        OutError = FString::Printf(TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: failed to save package %s"), *PackageName);
        return false;
    }
    return true;
}

UMHMaterialSourceData* GetSourceData(UMaterialInstanceConstant& Material)
{
    return Cast<UMHMaterialSourceData>(Material.GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
}

const UMHMaterialSourceData* GetSourceData(const UMaterialInstanceConstant& Material)
{
    return Cast<UMHMaterialSourceData>(
        const_cast<UMaterialInstanceConstant&>(Material).GetAssetUserDataOfClass(UMHMaterialSourceData::StaticClass()));
}

bool RelativeToRoot(const FString& Root, const FString& Path, FString& OutRelative)
{
    FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeDirectoryName(FullRoot);
    FString FullPath = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(FullPath);
    if (!FPaths::IsUnderDirectory(FullPath, FullRoot))
    {
        return false;
    }
    FullRoot += TEXT("/");
    OutRelative = FullPath;
    return FPaths::MakePathRelativeTo(OutRelative, *FullRoot) &&
        FPaths::IsRelative(OutRelative) && !OutRelative.StartsWith(TEXT("../"));
}

bool AtomicWriteMaterial(const FString& TargetPath, const TArray<uint8>& Bytes, FString& OutError)
{
    const FString Folder = FPaths::GetPath(TargetPath);
    if (!IFileManager::Get().MakeDirectory(*Folder, true))
    {
        OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: cannot create publish folder");
        return false;
    }
    const FString TempPath = TargetPath + FString::Printf(TEXT(".tmp.%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
    {
        OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: cannot write sibling temporary material");
        return false;
    }

    TArray<uint8> ReadBack;
    FMHMaterialDocument Parsed;
    TArray<uint8> Rewritten;
    FString ValidationError;
    if (!FFileHelper::LoadFileToArray(ReadBack, *TempPath) || ReadBack != Bytes ||
        !MHParseMaterialV4(ReadBack, Parsed, ValidationError) ||
        !MHWriteCanonicalMaterialV4(Parsed, Rewritten, ValidationError) || Rewritten != Bytes)
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: temporary read-back validation failed: %s"),
            *ValidationError);
        return false;
    }

    if (!IFileManager::Get().Move(
            *TargetPath,
            *TempPath,
            true,
            true,
            false,
            true))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: atomic replace failed");
        return false;
    }
    return true;
}

} // namespace

#if WITH_DEV_AUTOMATION_TESTS
void MHSetBeforeMaterialSourceCommitTestHook(TFunction<void()> Hook)
{
    GBeforeMaterialSourceCommitTestHook = MoveTemp(Hook);
}

MIMIRCOMPOSITEEDITOR_API void MHSetFailNextMaterialPackageSaveForTests(const bool bFail)
{
    GFailNextMaterialPackageSaveForTests = bFail;
}
#endif

bool MHValidateMaterialAdoptTarget(
    const FString& SourceRoot,
    const FMHMaterialAdoptTarget& Target,
    FString& OutPath,
    FString& OutRelativePath,
    FString& OutError)
{
    OutPath.Reset();
    OutRelativePath.Reset();
    OutError.Reset();
    if (!MHIsCanonicalMaterialToken(Target.LogicalName))
    {
        OutError = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: Adopt logical name must match [a-z0-9_]+");
        return false;
    }
    OutPath = FPaths::Combine(Target.Folder, Target.LogicalName + TEXT(".material"));
    if (!FPaths::GetCleanFilename(OutPath).Equals(
            Target.LogicalName + TEXT(".material"),
            ESearchCase::CaseSensitive) ||
        !RelativeToRoot(SourceRoot, OutPath, OutRelativePath))
    {
        OutError = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: Adopt target must be inside source_root");
        return false;
    }
    return true;
}

bool MHExtractMaterialV4(
    const UMaterialInstanceConstant& Material,
    const UMHCompositeSettings& Settings,
    FMHMaterialDocument& OutDocument,
    FString& OutError)
{
    OutDocument = FMHMaterialDocument();
    OutError.Reset();
    const UMaterialInterface* Parent = Material.Parent;
    if (Parent == nullptr || !MatchParent(*Parent, Settings, OutDocument.Mode, OutDocument.Parent))
    {
        OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: parent is not a direct registered class or library asset");
        return false;
    }
    if (OutDocument.Mode == EMHMaterialMode::Library)
    {
        if (HasAnyOverride(Material))
        {
            OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: library-parent material contains local overrides");
            return false;
        }
        return true;
    }

    if (Material.HasStaticParameters() || HasUnsupportedBaseOverride(Material.BasePropertyOverrides) ||
        !Material.DoubleVectorParameterValues.IsEmpty() ||
        !Material.TextureCollectionParameterValues.IsEmpty() ||
        !Material.ParameterCollectionParameterValues.IsEmpty() ||
        !Material.RuntimeVirtualTextureParameterValues.IsEmpty() ||
        !Material.SparseVolumeTextureParameterValues.IsEmpty() ||
        !Material.FontParameterValues.IsEmpty())
    {
        OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: material contains unsupported local override types");
        return false;
    }
    if (Material.BasePropertyOverrides.bOverride_TwoSided)
    {
        OutDocument.bHasTwoSided = true;
        OutDocument.bTwoSided = Material.BasePropertyOverrides.TwoSided;
    }
    for (const FScalarParameterValue& Value : Material.ScalarParameterValues)
    {
        const FString Name = Value.ParameterInfo.Name.ToString();
        if (!IsGlobal(Value.ParameterInfo) || !MHIsCanonicalMaterialToken(Name))
        {
            OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: scalar override name is not serializable");
            return false;
        }
#if WITH_EDITORONLY_DATA
        if (Value.AtlasData.bIsUsedAsAtlasPosition || !Value.AtlasData.Atlas.IsNull() || !Value.AtlasData.Curve.IsNull())
        {
            OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: scalar atlas override is not serializable");
            return false;
        }
#endif
        if (OutDocument.Params.Contains(Name))
        {
            OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: duplicate or cross-typed parameter override");
            return false;
        }
        FMHMaterialParameter Parameter;
        Parameter.Scalar = Value.ParameterValue;
        OutDocument.Params.Add(Name, Parameter);
    }
    for (const FVectorParameterValue& Value : Material.VectorParameterValues)
    {
        const FString Name = Value.ParameterInfo.Name.ToString();
        if (!IsGlobal(Value.ParameterInfo) || !MHIsCanonicalMaterialToken(Name) || OutDocument.Params.Contains(Name))
        {
            OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: vector override name is not serializable or collides");
            return false;
        }
        FMHMaterialParameter Parameter;
        Parameter.bVector = true;
        Parameter.Vector = FVector4f(Value.ParameterValue.R, Value.ParameterValue.G, Value.ParameterValue.B, Value.ParameterValue.A);
        OutDocument.Params.Add(Name, Parameter);
    }
    for (const FTextureParameterValue& Value : Material.TextureParameterValues)
    {
        const FString Name = Value.ParameterInfo.Name.ToString();
        int32 Slot = INDEX_NONE;
        if (!IsGlobal(Value.ParameterInfo) || !IsTextureSlotName(Name, Slot) || Value.ParameterValue == nullptr)
        {
            OutError = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: texture override is not a tex0-tex15 object reference");
            return false;
        }
        const FString Token = Value.ParameterValue->GetName();
        if (!MHIsCanonicalMaterialToken(Token))
        {
            OutError = TEXT("MH_E_NONCANONICAL_TEXTURE_REFERENCE: texture asset name is not canonical");
            return false;
        }
        OutDocument.Textures.Add(Slot, Token);
    }
    TArray<uint8> Ignored;
    return MHWriteCanonicalMaterialV4(OutDocument, Ignored, OutError);
}

bool MHApplyMaterialV4(
    UMaterialInstanceConstant& Material,
    UMaterialInterface& Parent,
    const FMHMaterialDocument& Document,
    const TMap<FString, UTexture*>& Textures,
    FString& OutError)
{
    OutError.Reset();
    if (Document.Mode == EMHMaterialMode::Library &&
        (Document.bHasTwoSided || !Document.Textures.IsEmpty() || !Document.Params.IsEmpty()))
    {
        OutError = TEXT("MH_E_MATERIAL_GRAMMAR: library source cannot contain overrides");
        return false;
    }
    for (const TPair<int32, FString>& Pair : Document.Textures)
    {
        if (Textures.FindRef(Pair.Value) == nullptr)
        {
            OutError = FString::Printf(
                TEXT("MH_E_UNRESOLVED_TEXTURE_REFERENCE: no imported texture object for texture:%s"),
                *Pair.Value);
            return false;
        }
    }

    Material.Modify();
    Material.ClearParameterValuesEditorOnly();
    Material.SetParentEditorOnly(&Parent);
    FMaterialInstanceBasePropertyOverrides Overrides;
    if (Document.Mode == EMHMaterialMode::Class && Document.bHasTwoSided)
    {
        Overrides.bOverride_TwoSided = true;
        Overrides.TwoSided = Document.bTwoSided;
    }
    FStaticParameterSet EmptyStaticParameters;
    Material.UpdateStaticPermutation(EmptyStaticParameters, Overrides);
    if (Document.Mode == EMHMaterialMode::Class)
    {
        TArray<FString> ParamNames;
        Document.Params.GenerateKeyArray(ParamNames);
        ParamNames.Sort();
        for (const FString& Name : ParamNames)
        {
            const FMHMaterialParameter& Parameter = Document.Params.FindChecked(Name);
            const FMaterialParameterInfo Info{FName(*Name)};
            if (Parameter.bVector)
            {
                Material.SetVectorParameterValueEditorOnly(
                    Info,
                    FLinearColor(Parameter.Vector.X, Parameter.Vector.Y, Parameter.Vector.Z, Parameter.Vector.W));
            }
            else
            {
                Material.SetScalarParameterValueEditorOnly(Info, Parameter.Scalar);
            }
        }
        TArray<int32> Slots;
        Document.Textures.GenerateKeyArray(Slots);
        Slots.Sort();
        for (const int32 Slot : Slots)
        {
            const FString& Token = Document.Textures.FindChecked(Slot);
            Material.SetTextureParameterValueEditorOnly(
                FMaterialParameterInfo(FName(*FString::Printf(TEXT("tex%d"), Slot))),
                Textures.FindChecked(Token));
        }
    }
    return true;
}

bool MHDetectManagedMaterialLocalModification(
    const UMaterialInstanceConstant& Material,
    const UMHCompositeSettings& Settings,
    FString& OutWarning)
{
    OutWarning.Reset();
    const UMHMaterialSourceData* Data = GetSourceData(Material);
    if (Data == nullptr || Data->AppliedHash.IsEmpty())
    {
        return false;
    }
    FMHMaterialDocument Extracted;
    TArray<uint8> Bytes;
    FString Error;
    if (!MHExtractMaterialV4(Material, Settings, Extracted, Error) ||
        !MHWriteCanonicalMaterialV4(Extracted, Bytes, Error) ||
        MHRawPayloadHash(Bytes) != Data->AppliedHash)
    {
        OutWarning = FString::Printf(
            TEXT("MH_W_MANAGED_ASSET_LOCALLY_MODIFIED: %s differs from applied material state"),
            *Material.GetPathName());
        return true;
    }
    return false;
}

FMHMaterialOperationResult MHImportMaterialV4(
    const FMHSourceAnalysisEntry& Entry,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings)
{
    FMHMaterialOperationResult Result;
    if (Entry.Key.Kind != EMHResourceKind::Material || !Entry.Key.IsCanonical() ||
        Entry.PayloadPath.IsEmpty() || Entry.SourcePath.IsEmpty())
    {
        Result.Error = TEXT("MH_E_MATERIAL_GRAMMAR: import entry is not a resolved material");
        return Result;
    }

    TArray<uint8> SourceBytes;
    FMHMaterialDocument Document;
    if (!LoadBytes(Entry.PayloadPath, SourceBytes, Result.Error) ||
        !MHParseMaterialV4(SourceBytes, Document, Result.Error))
    {
        return Result;
    }
    if (!Entry.RawHash.IsEmpty() && MHRawPayloadHash(SourceBytes) != Entry.RawHash)
    {
        Result.Error = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: material bytes changed after source scan");
        return Result;
    }
    const FString InitialSourceHash = MHRawPayloadHash(SourceBytes);
    TArray<uint8> CanonicalSource;
    if (!MHWriteCanonicalMaterialV4(Document, CanonicalSource, Result.Error))
    {
        return Result;
    }
    UMaterialInterface* Parent = LoadParent(
        Document.Mode == EMHMaterialMode::Class ? Settings.MasterRoot : Settings.LibraryRoot,
        Document.Parent,
        Result.Error);
    if (Parent == nullptr)
    {
        return Result;
    }
    TMap<FString, UTexture*> Textures;
    if (!ResolveTextures(
            Document,
            Resolver,
            SourceRoot,
            Textures,
            Result.Warnings,
            Result.Error))
    {
        return Result;
    }

    UMaterialInstanceConstant* Probe = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
    if (!MHApplyMaterialV4(*Probe, *Parent, Document, Textures, Result.Error))
    {
        return Result;
    }
    FMHMaterialDocument ProbeExtract;
    TArray<uint8> ProbeBytes;
    if (!MHExtractMaterialV4(*Probe, Settings, ProbeExtract, Result.Error) ||
        !MHWriteCanonicalMaterialV4(ProbeExtract, ProbeBytes, Result.Error) || ProbeBytes != CanonicalSource)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: source cannot survive MI apply/extract: %s"),
            *Result.Error);
        return Result;
    }

#if WITH_DEV_AUTOMATION_TESTS
    if (GBeforeMaterialSourceCommitTestHook)
    {
        TFunction<void()> Hook = MoveTemp(GBeforeMaterialSourceCommitTestHook);
        Hook();
    }
#endif
    TArray<uint8> FinalSourceBytes;
    if (!FFileHelper::LoadFileToArray(FinalSourceBytes, *Entry.PayloadPath) ||
        FinalSourceBytes != SourceBytes || MHRawPayloadHash(FinalSourceBytes) != InitialSourceHash)
    {
        Result.Error = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: material bytes changed before generated-asset mutation");
        return Result;
    }

    const FString PackageName = FString(GeneratedMaterialRoot) + TEXT("/") + Entry.Key.LogicalName;
    const FString ObjectPath = PackageName + TEXT(".") + Entry.Key.LogicalName;
    UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
    UMaterialInstanceConstant* Material = Cast<UMaterialInstanceConstant>(ExistingObject);
    if (ExistingObject != nullptr && Material == nullptr)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: generated path is occupied by %s"),
            *ExistingObject->GetClass()->GetName());
        return Result;
    }
    if (Material == nullptr)
    {
        UPackage* Package = CreatePackage(*PackageName);
        Material = NewObject<UMaterialInstanceConstant>(
            Package,
            FName(*Entry.Key.LogicalName),
            RF_Public | RF_Standalone | RF_Transactional);
        FAssetRegistryModule::AssetCreated(Material);
        Result.bCreated = true;
    }
    else
    {
        FString Warning;
        if (MHDetectManagedMaterialLocalModification(*Material, Settings, Warning))
        {
            Result.Warnings.Add(MoveTemp(Warning));
        }
    }

    if (!MHApplyMaterialV4(*Material, *Parent, Document, Textures, Result.Error))
    {
        return Result;
    }
    Material->PostEditChange();
    FAssetCompilingManager::Get().FinishAllCompilation();

    FMHMaterialDocument AppliedExtract;
    TArray<uint8> AppliedBytes;
    if (!MHExtractMaterialV4(*Material, Settings, AppliedExtract, Result.Error) ||
        !MHWriteCanonicalMaterialV4(AppliedExtract, AppliedBytes, Result.Error) ||
        AppliedBytes != CanonicalSource)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: actual MI does not match canonical source after compilation: %s"),
            *Result.Error);
        return Result;
    }

    if (!SaveMaterialPackage(*Material, Result.Error))
    {
        return Result;
    }

    UMHMaterialSourceData* Data = GetSourceData(*Material);
    if (Data == nullptr)
    {
        Data = NewObject<UMHMaterialSourceData>(Material, NAME_None, RF_Transactional);
        Material->AddAssetUserData(Data);
    }
    Data->LogicalName = Entry.Key.LogicalName;
    Data->SourceRelativePath = Entry.SourcePath;
    Data->SourceHash = InitialSourceHash;
    Data->AppliedHash = MHRawPayloadHash(AppliedBytes);
    Data->AppliedParent = AppliedParentReceipt(Document);
    Material->PostEditChange();
    if (!SaveMaterialPackage(*Material, Result.Error))
    {
        return Result;
    }
    FString RebindEvent;
    if (MHConsumeOrphanRebindEvent(SourceRoot, Entry.Key, RebindEvent))
    {
        Result.Warnings.Add(RebindEvent);
        UE_LOG(LogMHMaterialImport, Warning, TEXT("%s"), *RebindEvent);
    }
    if (!MHRefreshGeneratedAssetProjection(SourceRoot, Result.Error))
    {
        return Result;
    }
    Result.Material = Material;
    return Result;
}

FMHMaterialOperationResult MHPublishMaterialV4(
    UMaterialInstanceConstant& Material,
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings,
    const FMHMaterialAdoptTarget* AdoptTarget)
{
    FMHMaterialOperationResult Result;
    FMHMaterialDocument Document;
    TArray<uint8> Bytes;
    if (!MHExtractMaterialV4(Material, Settings, Document, Result.Error) ||
        !MHWriteCanonicalMaterialV4(Document, Bytes, Result.Error))
    {
        return Result;
    }
    FString LogicalName;
    FString TargetPath;
    const UMHMaterialSourceData* ExistingData = GetSourceData(Material);
    if (ExistingData != nullptr && !ExistingData->SourceRelativePath.IsEmpty())
    {
        LogicalName = ExistingData->LogicalName;
        TargetPath = FPaths::ConvertRelativePathToFull(SourceRoot, ExistingData->SourceRelativePath);
    }
    else
    {
        if (AdoptTarget == nullptr)
        {
            Result.Error = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: unmanaged MI publish requires Adopt folder and canonical name");
            return Result;
        }
        LogicalName = AdoptTarget->LogicalName;
        FString AdoptRelativePath;
        if (!MHValidateMaterialAdoptTarget(
                SourceRoot,
                *AdoptTarget,
                TargetPath,
                AdoptRelativePath,
                Result.Error))
        {
            return Result;
        }
    }
    FString RelativePath;
    if (!MHIsCanonicalMaterialToken(LogicalName) ||
        !FPaths::GetCleanFilename(TargetPath).Equals(LogicalName + TEXT(".material"), ESearchCase::CaseSensitive) ||
        !RelativeToRoot(SourceRoot, TargetPath, RelativePath))
    {
        Result.Error = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: publish target must be <source_root>/.../<canonical>.material");
        return Result;
    }
    if (!AtomicWriteMaterial(TargetPath, Bytes, Result.Error))
    {
        return Result;
    }

    const FString PublishedHash = MHRawPayloadHash(Bytes);
    TArray<FString> SessionEvents;
    if (!MHUpsertPublishedSource(
            SourceRoot,
            TargetPath,
            PublishedHash,
            SessionEvents,
            Result.Error))
    {
        return Result;
    }
    for (const FString& Event : SessionEvents)
    {
        UE_LOG(LogMHMaterialPublish, Display, TEXT("%s"), *Event);
    }

    UMHMaterialSourceData* Data = GetSourceData(Material);
    const bool bCreatedSourceData = Data == nullptr;
    FString PreviousLogicalName;
    FString PreviousSourcePath;
    FString PreviousSourceHash;
    FString PreviousAppliedHash;
    FString PreviousAppliedParent;
    if (Data != nullptr)
    {
        PreviousLogicalName = Data->LogicalName;
        PreviousSourcePath = Data->SourceRelativePath;
        PreviousSourceHash = Data->SourceHash;
        PreviousAppliedHash = Data->AppliedHash;
        PreviousAppliedParent = Data->AppliedParent;
    }
    if (Data == nullptr)
    {
        Data = NewObject<UMHMaterialSourceData>(&Material, NAME_None, RF_Transactional);
        Material.AddAssetUserData(Data);
    }
    Data->LogicalName = LogicalName;
    Data->SourceRelativePath = RelativePath;
    Data->SourceHash = PublishedHash;
    Data->AppliedHash = PublishedHash;
    Data->AppliedParent = AppliedParentReceipt(Document);
    Material.PostEditChange();
    if (!SaveMaterialPackage(Material, Result.Error))
    {
        if (bCreatedSourceData)
        {
            Material.RemoveUserDataOfClass(UMHMaterialSourceData::StaticClass());
        }
        else
        {
            Data->LogicalName = PreviousLogicalName;
            Data->SourceRelativePath = PreviousSourcePath;
            Data->SourceHash = PreviousSourceHash;
            Data->AppliedHash = PreviousAppliedHash;
            Data->AppliedParent = PreviousAppliedParent;
        }
        Material.PostEditChange();
        return Result;
    }
    if (!MHRefreshGeneratedAssetProjection(SourceRoot, Result.Error))
    {
        return Result;
    }
    Result.Material = &Material;
    return Result;
}

} // namespace UE::MimirComposite
