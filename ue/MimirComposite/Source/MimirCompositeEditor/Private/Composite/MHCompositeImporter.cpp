#include "Composite/MHCompositeImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Composite/MHCompositePlacementEvents.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHPayloadHashes.h"
#include "Source/MHSourceAnalyzer.h"
#include "Source/MHSourceComposition.h"
#include "Source/MHSourceResolver.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHCompositePublish, Display, All);
DEFINE_LOG_CATEGORY_STATIC(LogMHCompositeImport, Display, All);

namespace UE::MimirComposite
{
namespace
{

constexpr const TCHAR* GeneratedCompositeRoot = TEXT("/Game/MH/Generated/Composites");

#if WITH_DEV_AUTOMATION_TESTS
TFunction<void()> GBeforeCompositeSourceCommitTestHook;
#endif

void CollectProfileNames(
    const TArray<FMHCompositeNode>& Nodes,
    TArray<FString>& OutNames,
    TSet<FString>& Seen)
{
    for (const FMHCompositeNode& Node : Nodes)
    {
        if (!Node.Profile.IsEmpty() && !Seen.Contains(Node.Profile))
        {
            Seen.Add(Node.Profile);
            OutNames.Add(Node.Profile);
        }
        CollectProfileNames(Node.Children, OutNames, Seen);
    }
}

bool LoadInlinedProfiles(
    const FMHCompositeDocument& Document,
    IMHSourceResolver& Resolver,
    TArray<FMHPlacementProfile>& OutProfiles,
    FString& OutError)
{
    TArray<FString> Names;
    TSet<FString> Seen;
    CollectProfileNames(Document.Nodes, Names, Seen);
    OutProfiles.Reset(Names.Num());
    for (const FString& Name : Names)
    {
        FMHResourceKey Key;
        Key.Kind = EMHResourceKind::PlacementProfile;
        Key.LogicalName = Name;
        const FMHResolveOutcome Outcome = Resolver.Resolve(Key);
        if (Outcome.Status != EMHResolveStatus::Resolved)
        {
            OutError = FString::Printf(
                TEXT("MH_E_UNRESOLVED_COMPOSITE_REFERENCE: placement_profile:%s does not resolve uniquely"),
                *Name);
            return false;
        }
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Outcome.PayloadPath))
        {
            OutError = FString::Printf(
                TEXT("MH_E_PLACEMENT_PROFILE_GRAMMAR: cannot read placement_profile:%s"), *Name);
            return false;
        }
        if (!Outcome.RawHash.IsEmpty() && MHRawPayloadHash(Bytes) != Outcome.RawHash)
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: placement_profile:%s changed after source resolution"),
                *Name);
            return false;
        }
        FMHPlacementProfile& Profile = OutProfiles.AddDefaulted_GetRef();
        Profile.LogicalName = Name;
        if (!MHParsePlacementProfileV1(Bytes, Profile, OutError)) return false;
    }
    return true;
}

bool CompositeRelativeToRoot(const FString& Root, const FString& Path, FString& OutRelative)
{
    FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeDirectoryName(FullRoot);
    FString FullPath = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(FullPath);
    if (!FPaths::IsUnderDirectory(FullPath, FullRoot)) return false;
    FullRoot += TEXT("/");
    OutRelative = FullPath;
    return FPaths::MakePathRelativeTo(OutRelative, *FullRoot) &&
        FPaths::IsRelative(OutRelative) && !OutRelative.StartsWith(TEXT("../"));
}

bool SaveAssetPackage(UMHCompositeAsset& Asset, FString& OutError)
{
    UPackage* Package = Asset.GetOutermost();
    const FString PackageName = Package->GetName();
    if (!FPackageName::IsValidLongPackageName(PackageName))
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: composite asset has no persistent package");
        return false;
    }
    Package->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const FString Filename = FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetAssetPackageExtension());
    if (!UPackage::SavePackage(Package, &Asset, *Filename, Args))
    {
        OutError = FString::Printf(TEXT("MH_E_COMPOSITE_GRAMMAR: failed to save package %s"), *PackageName);
        return false;
    }
    return true;
}

void RemoveFailedCreatedAsset(UMHCompositeAsset& Asset)
{
    const FString PackageName = Asset.GetOutermost()->GetName();
    FAssetRegistryModule::AssetDeleted(&Asset);
    Asset.ClearFlags(RF_Public | RF_Standalone);
    Asset.MarkAsGarbage();
    if (FPackageName::IsValidLongPackageName(PackageName))
    {
        const FString Filename = FPackageName::LongPackageNameToFilename(
            PackageName, FPackageName::GetAssetPackageExtension());
        IFileManager::Get().Delete(*Filename, false, true, true);
    }
}

bool AtomicWriteComposite(const FString& TargetPath, const TArray<uint8>& Bytes, FString& OutError)
{
    const FString Folder = FPaths::GetPath(TargetPath);
    if (!IFileManager::Get().MakeDirectory(*Folder, true))
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: cannot create publish folder");
        return false;
    }
    const FString TempPath = TargetPath + FString::Printf(
        TEXT(".tmp.%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
    {
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: cannot write sibling temporary composite");
        return false;
    }
    TArray<uint8> ReadBack;
    FMHCompositeDocument Parsed;
    TArray<uint8> Rewritten;
    FString ValidationError;
    if (!FFileHelper::LoadFileToArray(ReadBack, *TempPath) || ReadBack != Bytes ||
        !MHParseCompositeV5(ReadBack, Parsed, ValidationError) ||
        !MHWriteCanonicalCompositeV5(Parsed, Rewritten, ValidationError) || Rewritten != Bytes)
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = FString::Printf(
            TEXT("MH_E_COMPOSITE_GRAMMAR: temporary read-back validation failed: %s"),
            *ValidationError);
        return false;
    }
    if (!IFileManager::Get().Move(*TargetPath, *TempPath, true, true, false, true))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        OutError = TEXT("MH_E_COMPOSITE_GRAMMAR: atomic replace failed");
        return false;
    }
    return true;
}

} // namespace

#if WITH_DEV_AUTOMATION_TESTS
void MHSetBeforeCompositeSourceCommitTestHook(TFunction<void()> Hook)
{
    GBeforeCompositeSourceCommitTestHook = MoveTemp(Hook);
}
#endif

bool MHValidateCompositeAdoptTarget(
    const FString& SourceRoot,
    const FMHCompositeAdoptTarget& Target,
    FString& OutPath,
    FString& OutRelativePath,
    FString& OutError)
{
    OutPath.Reset();
    OutRelativePath.Reset();
    OutError.Reset();
    if (!MHIsCanonicalCompositeToken(Target.LogicalName))
    {
        OutError = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: Adopt logical name must match [a-z0-9_]+");
        return false;
    }
    OutPath = FPaths::Combine(Target.Folder, Target.LogicalName + TEXT(".composite"));
    if (!FPaths::GetCleanFilename(OutPath).Equals(
            Target.LogicalName + TEXT(".composite"), ESearchCase::CaseSensitive) ||
        !CompositeRelativeToRoot(SourceRoot, OutPath, OutRelativePath))
    {
        OutError = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: Adopt target must be inside source_root with exact .composite suffix");
        return false;
    }
    return true;
}

bool MHDetectManagedCompositeLocalModification(
    const UMHCompositeAsset& Asset,
    FString& OutWarning)
{
    OutWarning.Reset();
    if (Asset.AppliedHash.IsEmpty()) return false;
    FMHCompositeDocument Extracted;
    TArray<uint8> Bytes;
    FString Error;
    if (!MHExtractCompositeV5(Asset, Extracted, Error) ||
        !MHWriteCanonicalCompositeV5(Extracted, Bytes, Error) ||
        MHRawPayloadHash(Bytes) != Asset.AppliedHash)
    {
        OutWarning = FString::Printf(
            TEXT("MH_W_MANAGED_ASSET_LOCALLY_MODIFIED: %s differs from applied composite state"),
            *Asset.GetPathName());
        return true;
    }
    return false;
}

FMHCompositeOperationResult MHImportCompositeV5(
    const FMHSourceAnalysisEntry& Entry,
    IMHSourceResolver& Resolver,
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings)
{
    FMHCompositeOperationResult Result;
    (void)SourceRoot;
    if (Entry.Key.Kind != EMHResourceKind::Composite || !Entry.Key.IsCanonical() ||
        Entry.PayloadPath.IsEmpty() || Entry.SourcePath.IsEmpty())
    {
        Result.Error = TEXT("MH_E_COMPOSITE_GRAMMAR: import entry is not a resolved composite");
        return Result;
    }
    TArray<uint8> SourceBytes;
    FMHCompositeDocument Document;
    if (!FFileHelper::LoadFileToArray(SourceBytes, *Entry.PayloadPath) ||
        !MHParseCompositeV5(SourceBytes, Document, Result.Error))
    {
        if (Result.Error.IsEmpty()) Result.Error = TEXT("MH_E_COMPOSITE_GRAMMAR: cannot read composite payload");
        return Result;
    }
    const FString InitialHash = MHRawPayloadHash(SourceBytes);
    if (!Entry.RawHash.IsEmpty() && InitialHash != Entry.RawHash)
    {
        Result.Error = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: composite bytes changed after source scan");
        return Result;
    }
    TArray<uint8> CanonicalSource;
    TArray<FMHPlacementProfile> InlinedProfiles;
    if (!MHWriteCanonicalCompositeV5(Document, CanonicalSource, Result.Error) ||
        !LoadInlinedProfiles(Document, Resolver, InlinedProfiles, Result.Error) ||
        !MHProbeCompositeBuildV5(Entry.Key.LogicalName, Document, Resolver, Settings, Result.Error))
    {
        return Result;
    }

#if WITH_DEV_AUTOMATION_TESTS
    if (GBeforeCompositeSourceCommitTestHook)
    {
        TFunction<void()> Hook = MoveTemp(GBeforeCompositeSourceCommitTestHook);
        Hook();
    }
#endif
    TArray<uint8> FinalBytes;
    if (!FFileHelper::LoadFileToArray(FinalBytes, *Entry.PayloadPath) ||
        FinalBytes != SourceBytes || MHRawPayloadHash(FinalBytes) != InitialHash)
    {
        Result.Error = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: composite bytes changed before generated-asset mutation");
        return Result;
    }

    const FString PackageName = FString(GeneratedCompositeRoot) + TEXT("/") + Entry.Key.LogicalName;
    const FString ObjectPath = PackageName + TEXT(".") + Entry.Key.LogicalName;
    UObject* ExistingObject = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
    UMHCompositeAsset* Asset = Cast<UMHCompositeAsset>(ExistingObject);
    if (ExistingObject != nullptr && Asset == nullptr)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_COMPOSITE_GRAMMAR: generated path is occupied by %s"),
            *ExistingObject->GetClass()->GetName());
        return Result;
    }
    if (Asset == nullptr)
    {
        UPackage* Package = CreatePackage(*PackageName);
        Asset = NewObject<UMHCompositeAsset>(
            Package, FName(*Entry.Key.LogicalName), RF_Public | RF_Standalone | RF_Transactional);
        FAssetRegistryModule::AssetCreated(Asset);
        Result.bCreated = true;
    }
    else
    {
        FString Warning;
        if (MHDetectManagedCompositeLocalModification(*Asset, Warning)) Result.Warnings.Add(MoveTemp(Warning));
    }

    const TArray<FMHCompositeAssetNode> PreviousNodes = Asset->Nodes;
    const TArray<FMHPlacementProfile> PreviousProfiles = Asset->InlinedPlacementProfiles;
    const FString PreviousLogicalName = Asset->LogicalName;
    const FString PreviousSourcePath = Asset->SourceRelativePath;
    const FString PreviousSourceHash = Asset->SourceHash;
    const FString PreviousAppliedHash = Asset->AppliedHash;
    if (!MHApplyCompositeV5(*Asset, Document, InlinedProfiles, Result.Error))
    {
        if (Result.bCreated) RemoveFailedCreatedAsset(*Asset);
        return Result;
    }
    FMHCompositeDocument Extracted;
    TArray<uint8> AppliedBytes;
    if (!MHExtractCompositeV5(*Asset, Extracted, Result.Error) ||
        !MHWriteCanonicalCompositeV5(Extracted, AppliedBytes, Result.Error) || AppliedBytes != CanonicalSource)
    {
        Result.Error = FString::Printf(
            TEXT("MH_E_COMPOSITE_GRAMMAR: applied asset does not round-trip canonical source: %s"),
            *Result.Error);
        Asset->Nodes = PreviousNodes;
        Asset->InlinedPlacementProfiles = PreviousProfiles;
        if (Result.bCreated) RemoveFailedCreatedAsset(*Asset);
        return Result;
    }
    // Persist the compiled payload before advancing the receipt (§7). A
    // failed payload save leaves both receipt and source identity unchanged.
    Asset->PostEditChange();
    if (!SaveAssetPackage(*Asset, Result.Error))
    {
        Asset->Nodes = PreviousNodes;
        Asset->InlinedPlacementProfiles = PreviousProfiles;
        Asset->LogicalName = PreviousLogicalName;
        Asset->SourceRelativePath = PreviousSourcePath;
        Asset->SourceHash = PreviousSourceHash;
        Asset->AppliedHash = PreviousAppliedHash;
        if (Result.bCreated) RemoveFailedCreatedAsset(*Asset);
        return Result;
    }
    Asset->LogicalName = Entry.Key.LogicalName;
    Asset->SourceRelativePath = Entry.SourcePath;
    Asset->SourceHash = InitialHash;
    Asset->AppliedHash = MHRawPayloadHash(AppliedBytes);
    Asset->PostEditChange();
    if (!SaveAssetPackage(*Asset, Result.Error))
    {
        Asset->LogicalName = PreviousLogicalName;
        Asset->SourceRelativePath = PreviousSourcePath;
        Asset->SourceHash = PreviousSourceHash;
        Asset->AppliedHash = PreviousAppliedHash;
        Asset->PostEditChange();
        if (Result.bCreated) RemoveFailedCreatedAsset(*Asset);
        return Result;
    }
    FString RebindEvent;
    if (MHConsumeOrphanRebindEvent(SourceRoot, Entry.Key, RebindEvent))
    {
        Result.Warnings.Add(RebindEvent);
        UE_LOG(LogMHCompositeImport, Warning, TEXT("%s"), *RebindEvent);
    }
    if (!MHRefreshGeneratedAssetProjection(SourceRoot, Result.Error))
    {
        return Result;
    }
    Result.Asset = Asset;
    MHNotifyGeneratedResourceChanged(Entry.Key);
    return Result;
}

FMHCompositeOperationResult MHPublishCompositeV5(
    UMHCompositeAsset& Asset,
    const FString& SourceRoot,
    const FMHCompositeAdoptTarget* AdoptTarget)
{
    FMHCompositeOperationResult Result;
    FString AbsoluteSourceRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(AbsoluteSourceRoot);
    FMHCompositeDocument Document;
    TArray<uint8> Bytes;
    if (!MHExtractCompositeV5(Asset, Document, Result.Error) ||
        !MHWriteCanonicalCompositeV5(Document, Bytes, Result.Error))
    {
        return Result;
    }
    FString LogicalName;
    FString TargetPath;
    FString RelativePath;
    if (!Asset.SourceRelativePath.IsEmpty())
    {
        LogicalName = Asset.LogicalName;
        TargetPath = FPaths::ConvertRelativePathToFull(AbsoluteSourceRoot, Asset.SourceRelativePath);
        if (!CompositeRelativeToRoot(AbsoluteSourceRoot, TargetPath, RelativePath))
        {
            Result.Error = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: managed composite source path escapes source_root");
            return Result;
        }
    }
    else
    {
        if (AdoptTarget == nullptr)
        {
            Result.Error = TEXT("MH_E_COMPOSITE_GRAMMAR: unmanaged composite publish requires Adopt folder and canonical name");
            return Result;
        }
        LogicalName = AdoptTarget->LogicalName;
        if (!MHValidateCompositeAdoptTarget(
                AbsoluteSourceRoot, *AdoptTarget, TargetPath, RelativePath, Result.Error)) return Result;
    }
    if (!MHIsCanonicalCompositeToken(LogicalName) ||
        !FPaths::GetCleanFilename(TargetPath).Equals(
            LogicalName + TEXT(".composite"), ESearchCase::CaseSensitive))
    {
        Result.Error = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: publish target must be <canonical>.composite");
        return Result;
    }
    // First persist the current applied asset. Source publication and receipt
    // advancement cannot make an unsaved local state authoritative.
    if (!SaveAssetPackage(Asset, Result.Error)) return Result;
    if (!AtomicWriteComposite(TargetPath, Bytes, Result.Error)) return Result;

    const FString PublishedHash = MHRawPayloadHash(Bytes);
    TArray<FString> SessionEvents;
    if (!MHUpsertPublishedSource(
            AbsoluteSourceRoot,
            TargetPath,
            PublishedHash,
            SessionEvents,
            Result.Error))
    {
        return Result;
    }
    for (const FString& Event : SessionEvents)
    {
        UE_LOG(LogMHCompositePublish, Display, TEXT("%s"), *Event);
    }

    const FString PreviousLogicalName = Asset.LogicalName;
    const FString PreviousSourcePath = Asset.SourceRelativePath;
    const FString PreviousSourceHash = Asset.SourceHash;
    const FString PreviousAppliedHash = Asset.AppliedHash;
    Asset.LogicalName = LogicalName;
    Asset.SourceRelativePath = RelativePath;
    Asset.SourceHash = PublishedHash;
    Asset.AppliedHash = PublishedHash;
    Asset.PostEditChange();
    if (!SaveAssetPackage(Asset, Result.Error))
    {
        Asset.LogicalName = PreviousLogicalName;
        Asset.SourceRelativePath = PreviousSourcePath;
        Asset.SourceHash = PreviousSourceHash;
        Asset.AppliedHash = PreviousAppliedHash;
        Asset.PostEditChange();
        return Result;
    }
    if (!MHRefreshGeneratedAssetProjection(AbsoluteSourceRoot, Result.Error))
    {
        return Result;
    }
    Result.Asset = &Asset;
    FMHResourceKey ChangedKey;
    ChangedKey.Kind = EMHResourceKind::Composite;
    ChangedKey.LogicalName = Asset.LogicalName;
    MHNotifyGeneratedResourceChanged(ChangedKey);
    return Result;
}

} // namespace UE::MimirComposite
