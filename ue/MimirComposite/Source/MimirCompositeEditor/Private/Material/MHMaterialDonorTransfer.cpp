#include "Material/MHMaterialDonorTransfer.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Material/MHMaterialImporter.h"
#include "Material/MHMaterialProtocol.h"
#include "Material/MHUnrealMaterialDocument.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/MHPayloadHashes.h"

namespace UE::MimirComposite
{
namespace
{
FString FullPath(const FString& Path)
{
    FString Result = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Result);
    FPaths::CollapseRelativeDirectories(Result);
    FPaths::NormalizeDirectoryName(Result);
    return Result;
}

bool NoNestedAlias(const FString& Root, FString Path, FString& OutError)
{
    IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
    while (!FPaths::IsSamePath(Path, Root))
    {
        if (!FPaths::IsUnderDirectory(Path, Root) ||
            ((Files.FileExists(*Path) || Files.DirectoryExists(*Path)) &&
             Files.IsSymlink(*Path) != ESymlinkResult::NonSymlink))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: donor destination has a filesystem alias or leaves Source Root: %s"), *Path);
            return false;
        }
        Path = FPaths::GetPath(Path);
    }
    return true;
}

bool MaterialPaths(const FString& Root, TMap<FString, TArray<FString>>& OutPaths, FString& OutError)
{
    IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
    TArray<FString> Folders{Root};
    while (!Folders.IsEmpty())
    {
        const FString Folder = Folders.Pop();
        if (!Files.IterateDirectory(*Folder, [&](const TCHAR* Filename, bool bDirectory)
            {
                const FString Path = FullPath(Filename);
                if (Files.IsSymlink(*Path) != ESymlinkResult::NonSymlink)
                {
                    OutError = TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: nested filesystem alias in Source Root: ") + Path;
                    return false;
                }
                if (bDirectory) Folders.Add(Path);
                else if (Path.EndsWith(TEXT(".material"), ESearchCase::IgnoreCase))
                {
                    FMHResourceKey Key;
                    if (!MHResourceKeyFromSourceFile(Path, Key, OutError)) return false;
                    OutPaths.FindOrAdd(Key.LogicalName).Add(Path);
                }
                return true;
            }))
        {
            if (OutError.IsEmpty()) OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot enumerate material source folder: ") + Folder;
            return false;
        }
    }
    return true;
}
}

bool MHPrepareMaterialDonorTransfer(
    TConstArrayView<UMaterialInstanceConstant*> Materials,
    const FString& SourceRoot,
    const FString& NewSourceFolder,
    FMHMaterialDocumentExportPlan& OutPlan,
    FString& OutError)
{
    OutPlan = FMHMaterialDocumentExportPlan();
    OutError.Reset();
    const FString Root = FullPath(SourceRoot);
    const FString Folder = FullPath(NewSourceFolder);
    if (Materials.IsEmpty() || SourceRoot.IsEmpty() || NewSourceFolder.IsEmpty() ||
        !IFileManager::Get().DirectoryExists(*Root) ||
        (!FPaths::IsSamePath(Folder, Root) && !FPaths::IsUnderDirectory(Folder, Root)))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: select donors and a destination folder inside an existing MH Source Root");
        return false;
    }
    if (!NoNestedAlias(Root, Folder, OutError)) return false;

    // Enumerate names only; do not read/hash unrelated mesh and texture payloads.
    TMap<FString, TArray<FString>> Sources;
    if (!MaterialPaths(Root, Sources, OutError)) return false;
    OutPlan.DonorSourceRoot = Root;
    TSet<FString> Names;
    TSet<FSoftObjectPath> Donors;
    for (const UMaterialInstanceConstant* Material : Materials)
        if (Material != nullptr) Donors.Add(FSoftObjectPath(Material));
    TMap<FSoftObjectPath, FSoftObjectPath> Parents;
    for (UMaterialInstanceConstant* Material : Materials)
    {
        FString Error;
        FString Destination;
        FString Name;
        if (Material == nullptr ||
            !Material->GetName().StartsWith(TEXT("m_"), ESearchCase::CaseSensitive))
        {
            Error = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: every donor asset name must start with exact m_");
        }
        else
        {
            Name = Material->GetName().Mid(2);
            if (!MHIsCanonicalMaterialToken(Name))
                Error = TEXT("MH_E_NONCANONICAL_RESOURCE_NAME: the name after m_ must match [a-z0-9_]+");
            else if (Names.Contains(Name))
                Error = TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME: multiple selected donors map to the same material");
            Names.Add(Name);
        }
        FMHPreparedMaterialDocumentExport Prepared;
        if (Error.IsEmpty())
        {
            const TArray<FString>* Existing = Sources.Find(Name);
            if (Existing != nullptr && Existing->Num() == 1)
            {
                Destination = (*Existing)[0];
                Prepared.bOverwritesExistingFile = true;
                TArray<uint8> Bytes;
                if (!FFileHelper::LoadFileToArray(Bytes, *Destination))
                    Error = TEXT("MH_E_INVALID_RESOURCE_SOURCE: cannot read the existing material source");
                else Prepared.ExpectedDestinationHash = MHRawPayloadHash(Bytes);
            }
            else if (Existing == nullptr)
            {
                Destination = FPaths::Combine(Folder, Name + TEXT(".material"));
                if (IFileManager::Get().FileExists(*Destination))
                    Error = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: destination exists but did not resolve in the source inventory");
            }
            else
            {
                Error = FString::Printf(TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME: material:%s has multiple source files"), *Name);
            }
        }
        if (Error.IsEmpty()) NoNestedAlias(Root, Destination, Error);
        FMHMaterialDocument Document;
        if (Error.IsEmpty() && MHExtractUnrealMaterialV1(*Material, Document, Error))
            MHWriteCanonicalMaterialV4(Document, Prepared.CanonicalBytes, Error);
        const FSoftObjectPath Target(FString::Printf(TEXT("/Game/MH/Generated/Materials/%s.%s"), *Name, *Name));
        if (Error.IsEmpty() && Donors.Contains(Target))
            Error = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: a selected donor is also a target of this batch");
        if (!Error.IsEmpty())
        {
            OutPlan.Skipped.Add({Material ? Material->GetPathName() : TEXT("<null>"), Destination, Error});
            continue;
        }
        Prepared.Material = Material;
        Parents.Add(Target, Document.UnrealInstance->Parent);
        Prepared.LogicalName = Name;
        Prepared.DestinationPath = Destination;
        Prepared.CanonicalHash = MHRawPayloadHash(Prepared.CanonicalBytes);
        if (Prepared.bOverwritesExistingFile) OutPlan.OverwritePaths.Add(Destination);
        OutPlan.Ready.Add(MoveTemp(Prepared));
    }
    // Walk the prospective graph, replacing all selected target edges at once.
    // This catches cycles that neither donor's current ancestry contains.
    TMap<FString, int32> ParentDepths;
    for (const auto& Edge : Parents)
    {
        TSet<FSoftObjectPath> Seen;
        FSoftObjectPath Current = Edge.Key;
        while (!Current.IsNull())
        {
            if (Seen.Contains(Current))
            {
                OutPlan.Skipped.Add({Edge.Key.ToString(), FString(),
                    TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: planned parent chain contains a cycle")});
                break;
            }
            Seen.Add(Current);
            if (const FSoftObjectPath* Planned = Parents.Find(Current)) Current = *Planned;
            else
            {
                const UMaterialInstance* Instance = Cast<UMaterialInstance>(Current.TryLoad());
                Current = Instance != nullptr ? FSoftObjectPath(Instance->Parent.Get()) : FSoftObjectPath();
            }
        }
        ParentDepths.Add(Edge.Key.GetAssetName(), Seen.Num());
    }
    if (!OutPlan.Skipped.IsEmpty())
    {
        OutPlan.Ready.Reset();
        OutPlan.OverwritePaths.Reset();
        OutError = FString::Printf(TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: %d donor(s) rejected; no documents were written"), OutPlan.Skipped.Num());
        return false;
    }
    OutPlan.Ready.Sort([&ParentDepths](const FMHPreparedMaterialDocumentExport& A, const FMHPreparedMaterialDocumentExport& B)
    {
        const int32 ADepth = ParentDepths.FindChecked(A.LogicalName);
        const int32 BDepth = ParentDepths.FindChecked(B.LogicalName);
        if (ADepth != BDepth) return ADepth < BDepth;
        return A.LogicalName < B.LogicalName;
    });
    OutPlan.OverwritePaths.Sort();
    return true;
}

bool MHValidateMaterialDonorDestinations(const FMHMaterialDocumentExportPlan& Plan, FString& OutError)
{
    TMap<FString, TArray<FString>> Sources;
    if (!MaterialPaths(Plan.DonorSourceRoot, Sources, OutError)) return false;
    for (const FMHPreparedMaterialDocumentExport& Item : Plan.Ready)
    {
        if (!NoNestedAlias(Plan.DonorSourceRoot, Item.DestinationPath, OutError)) return false;
        const TArray<FString>* Existing = Sources.Find(Item.LogicalName);
        const bool bMatches = Item.bOverwritesExistingFile
            ? Existing != nullptr && Existing->Num() == 1 && FPaths::IsSamePath((*Existing)[0], Item.DestinationPath)
            : Existing == nullptr;
        if (!bMatches)
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: donor destination mapping changed; prepare the transfer again: ") + Item.LogicalName;
            return false;
        }
    }
    return true;
}

FMHImportSourcesScope MHMaterialDonorImportScope(
    const FMHMaterialDocumentExportPlan& Plan,
    const FMHMaterialDocumentExportResult& Result)
{
    FMHImportSourcesScope Scope;
    Scope.bForceMaterialReimport = true;
    if (Result.bCancelled) return Scope;
    for (const FMHPreparedMaterialDocumentExport& Item : Plan.Ready)
    {
        if (Result.ExportedPaths.Contains(Item.DestinationPath))
        {
            FMHResourceKey Key;
            Key.Kind = EMHResourceKind::Material;
            Key.LogicalName = Item.LogicalName;
            if (Key.IsCanonical()) Scope.ResourceKeys.AddUnique(Key);
        }
    }
    return Scope;
}

} // namespace UE::MimirComposite
