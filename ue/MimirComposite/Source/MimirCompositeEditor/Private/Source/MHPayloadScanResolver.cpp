#include "Source/MHPayloadScanResolver.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/MHPayloadHashes.h"

namespace UE::MimirComposite
{
namespace
{

struct FPayloadFileState
{
    int64 Size = INDEX_NONE;
    FDateTime Timestamp;
};

bool ReadFileState(const FString& Path, FPayloadFileState& OutState)
{
    IFileManager& FileManager = IFileManager::Get();
    OutState.Size = FileManager.FileSize(*Path);
    OutState.Timestamp = FileManager.GetTimeStamp(*Path);
    return OutState.Size >= 0;
}

bool SameFileState(const FPayloadFileState& A, const FPayloadFileState& B)
{
    return A.Size == B.Size && A.Timestamp == B.Timestamp;
}

bool ReadStablePayload(
    const FString& Path,
    TArray<uint8>& OutBytes,
    FString& OutError)
{
    FPayloadFileState Before;
    FPayloadFileState After;
    if (!ReadFileState(Path, Before) ||
        !FFileHelper::LoadFileToArray(OutBytes, *Path) ||
        !ReadFileState(Path, After))
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot read source payload");
        return false;
    }
    if (!SameFileState(Before, After) || static_cast<int64>(OutBytes.Num()) != Before.Size)
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: source payload changed during scan");
        return false;
    }
    return true;
}

bool ValidateNoNestedFilesystemAlias(
    const FString& SourceRoot,
    const FString& PayloadPath,
    FString& OutError)
{
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    FString Component = PayloadPath;
    while (!Component.Equals(SourceRoot, ESearchCase::IgnoreCase))
    {
        const ESymlinkResult Result = PlatformFile.IsSymlink(*Component);
        if (Result != ESymlinkResult::NonSymlink)
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: nested filesystem alias is forbidden or unavailable: %s"),
                *Component);
            return false;
        }
        FString Parent = FPaths::GetPath(Component);
        FPaths::NormalizeDirectoryName(Parent);
        if (Parent.IsEmpty() || Parent.Equals(Component, ESearchCase::IgnoreCase))
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: source path parent chain is invalid");
            return false;
        }
        Component = MoveTemp(Parent);
    }
    return true;
}

bool IsCanonicalLogicalName(const FString& Value)
{
    if (Value.IsEmpty())
    {
        return false;
    }
    for (const TCHAR Character : Value)
    {
        if (!((Character >= TEXT('a') && Character <= TEXT('z')) ||
              (Character >= TEXT('0') && Character <= TEXT('9')) ||
              Character == TEXT('_')))
        {
            return false;
        }
    }
    return true;
}

bool IsTextureExtension(const FString& Extension)
{
    static const TSet<FString> Extensions = {
        TEXT("png"), TEXT("tga"), TEXT("tif"), TEXT("tiff"), TEXT("exr"),
        TEXT("jpg"), TEXT("jpeg"), TEXT("dds"), TEXT("hdr")};
    return Extensions.Contains(Extension);
}

bool ResourceKeyLess(const FMHResourceKey& A, const FMHResourceKey& B)
{
    if (A.Kind != B.Kind)
    {
        return static_cast<uint8>(A.Kind) < static_cast<uint8>(B.Kind);
    }
    return A.LogicalName < B.LogicalName;
}

} // namespace

const TCHAR* MHResourceKindLabel(const EMHResourceKind Kind)
{
    switch (Kind)
    {
    case EMHResourceKind::StaticMesh: return TEXT("static_mesh");
    case EMHResourceKind::Material: return TEXT("material");
    case EMHResourceKind::Composite: return TEXT("composite");
    case EMHResourceKind::Texture: return TEXT("texture");
    }
    return TEXT("unknown");
}

bool MHResourceKindFromLabel(const FString& Label, EMHResourceKind& OutKind)
{
    for (const EMHResourceKind Kind : {
        EMHResourceKind::StaticMesh,
        EMHResourceKind::Material,
        EMHResourceKind::Composite,
        EMHResourceKind::Texture})
    {
        if (Label == MHResourceKindLabel(Kind))
        {
            OutKind = Kind;
            return true;
        }
    }
    return false;
}

bool FMHResourceKey::IsCanonical() const
{
    switch (Kind)
    {
    case EMHResourceKind::StaticMesh:
    case EMHResourceKind::Material:
    case EMHResourceKind::Composite:
    case EMHResourceKind::Texture:
        return IsCanonicalLogicalName(LogicalName);
    }
    return false;
}

FString FMHResourceKey::ToString() const
{
    return FString::Printf(TEXT("%s:%s"), MHResourceKindLabel(Kind), *LogicalName);
}

bool MHResourceKeyFromSourceFile(
    const FString& Path,
    FMHResourceKey& OutKey,
    FString& OutError)
{
    OutKey = FMHResourceKey();
    OutError.Reset();

    const FString Filename = FPaths::GetCleanFilename(Path);
    const FString LowerFilename = Filename.ToLower();
    FString LogicalName;
    FString CanonicalSuffix;

    if (LowerFilename.EndsWith(TEXT(".mesh.fbx")))
    {
        OutKey.Kind = EMHResourceKind::StaticMesh;
        CanonicalSuffix = TEXT(".mesh.fbx");
    }
    else if (LowerFilename.EndsWith(TEXT(".material")))
    {
        OutKey.Kind = EMHResourceKind::Material;
        CanonicalSuffix = TEXT(".material");
    }
    else if (LowerFilename.EndsWith(TEXT(".composite")))
    {
        OutKey.Kind = EMHResourceKind::Composite;
        CanonicalSuffix = TEXT(".composite");
    }
    else
    {
        const FString Extension = FPaths::GetExtension(LowerFilename, false);
        if (!IsTextureExtension(Extension))
        {
            return false;
        }
        OutKey.Kind = EMHResourceKind::Texture;
        CanonicalSuffix = FString::Printf(TEXT(".%s"), *Extension);
    }

    if (!Filename.EndsWith(CanonicalSuffix, ESearchCase::CaseSensitive))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: source filename extension is not canonical: %s"),
            *Filename);
        return false;
    }

    LogicalName = Filename.LeftChop(CanonicalSuffix.Len());
    if (!IsCanonicalLogicalName(LogicalName))
    {
        OutError = FString::Printf(
            TEXT("MH_E_NON_ASCII_RESOURCE_NAME: source logical name must match [a-z0-9_]+: %s"),
            *Filename);
        return false;
    }

    OutKey.LogicalName = MoveTemp(LogicalName);
    return true;
}

FMHPayloadScanResolver::FMHPayloadScanResolver(FString InSourceRoot)
    : SourceRoot(MoveTemp(InSourceRoot))
{
}

void FMHPayloadScanResolver::QuarantinePayload(
    const FString& Path,
    const FString& Diagnostic)
{
    FMHSourceQuarantine& Entry = QuarantineEntries.AddDefaulted_GetRef();
    Entry.PayloadPath = Path;
    FPaths::NormalizeFilename(Entry.PayloadPath);
    Entry.Diagnostic = FString::Printf(TEXT("%s: %s"), *Entry.PayloadPath, *Diagnostic);
}

void FMHPayloadScanResolver::AddPayloadFile(const FString& Path)
{
    FMHResourceKey Key;
    FString KeyError;
    if (!MHResourceKeyFromSourceFile(Path, Key, KeyError))
    {
        if (!KeyError.IsEmpty())
        {
            QuarantinePayload(Path, KeyError);
        }
        return;
    }

    TArray<uint8> Bytes;
    FString ReadError;
    if (!ReadStablePayload(Path, Bytes, ReadError))
    {
        QuarantinePayload(Path, ReadError);
        return;
    }

    FCandidate Candidate;
    Candidate.Key = Key;
    Candidate.Path = Path;
    Candidate.RawHash = MHRawPayloadHash(Bytes);
    CandidatesByKey.FindOrAdd(Key).Add(MoveTemp(Candidate));
    ++CandidateFileCount;
}

bool FMHPayloadScanResolver::DiscoverPayloadPaths(
    TArray<FString>& OutPaths,
    FString& OutError) const
{
    OutPaths.Reset();
    OutError.Reset();
    const bool bTraversed = IFileManager::Get().IterateDirectoryRecursively(
        *SourceRoot,
        [this, &OutPaths, &OutError](const TCHAR* Path, const bool bIsDirectory)
        {
            if (bIsDirectory)
            {
                return true;
            }

            FString File = FPaths::ConvertRelativePathToFull(Path);
            FPaths::NormalizeFilename(File);
            if (!FPaths::IsUnderDirectory(File, SourceRoot))
            {
                OutError = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: discovered file escapes source_root: %s"),
                    *File);
                return false;
            }

            FMHResourceKey IgnoredKey;
            FString ClassificationError;
            const bool bRecognized = MHResourceKeyFromSourceFile(File, IgnoredKey, ClassificationError);
            if (bRecognized || !ClassificationError.IsEmpty())
            {
                if (!ValidateNoNestedFilesystemAlias(SourceRoot, File, OutError))
                {
                    return false;
                }
                OutPaths.Add(MoveTemp(File));
            }
            return true;
        });
    OutPaths.Sort();
    if (!bTraversed)
    {
        if (OutError.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_INVALID: source traversal failed: %s"),
                *SourceRoot);
        }
        return false;
    }
    return true;
}

bool FMHPayloadScanResolver::Initialize(FString& OutError)
{
    OutError.Reset();
    CandidatesByKey.Reset();
    QuarantineEntries.Reset();
    CandidateFileCount = 0;

    SourceRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    FPaths::NormalizeDirectoryName(SourceRoot);
    if (!IFileManager::Get().DirectoryExists(*SourceRoot))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: source_root does not exist: %s"),
            *SourceRoot);
        return false;
    }

    TArray<FString> PayloadPaths;
    if (!DiscoverPayloadPaths(PayloadPaths, OutError))
    {
        return false;
    }
    for (const FString& Path : PayloadPaths)
    {
        AddPayloadFile(Path);
    }

    TArray<FString> ConfirmedPaths;
    if (!DiscoverPayloadPaths(ConfirmedPaths, OutError) || ConfirmedPaths != PayloadPaths)
    {
        CandidatesByKey.Reset();
        QuarantineEntries.Reset();
        CandidateFileCount = 0;
        if (OutError.IsEmpty())
        {
            OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: source payload set changed during scan");
        }
        return false;
    }

    // Path equality alone does not confirm an immutable snapshot: a writer can
    // atomically replace one payload at the same path after the first read.
    // Re-read and re-hash every valid candidate so Initialize never publishes
    // a path paired with bytes from an earlier source generation.
    for (const FString& Path : ConfirmedPaths)
    {
        FMHResourceKey Key;
        FString ClassificationError;
        if (!MHResourceKeyFromSourceFile(Path, Key, ClassificationError))
        {
            // DiscoverPayloadPaths includes invalid known payload names so they
            // remain quarantined. Their diagnostic is path-derived, not byte-
            // derived, so no content confirmation is required.
            if (!ClassificationError.IsEmpty())
            {
                continue;
            }
            OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: source payload classification changed during scan");
            CandidatesByKey.Reset();
            QuarantineEntries.Reset();
            CandidateFileCount = 0;
            return false;
        }

        TArray<uint8> ConfirmedBytes;
        FString ReadError;
        const TArray<FCandidate>* Candidates = CandidatesByKey.Find(Key);
        const FCandidate* Candidate = Candidates != nullptr
            ? Candidates->FindByPredicate([&Path](const FCandidate& Value)
                {
                    return Value.Path == Path;
                })
            : nullptr;
        if (!ReadStablePayload(Path, ConfirmedBytes, ReadError) || Candidate == nullptr ||
            Candidate->RawHash != MHRawPayloadHash(ConfirmedBytes))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: source payload bytes changed during scan: %s"),
                *Path);
            CandidatesByKey.Reset();
            QuarantineEntries.Reset();
            CandidateFileCount = 0;
            return false;
        }
    }
    return true;
}

FMHSourceSnapshot FMHPayloadScanResolver::GetSnapshot() const
{
    FMHSourceSnapshot Snapshot;
    CandidatesByKey.GenerateKeyArray(Snapshot.ResourceKeys);
    Snapshot.ResourceKeys.Sort(ResourceKeyLess);
    Snapshot.Quarantined = QuarantineEntries;
    for (const FMHSourceQuarantine& Entry : QuarantineEntries)
    {
        Snapshot.Errors.Add(Entry.Diagnostic);
    }
    for (const TPair<FMHResourceKey, TArray<FCandidate>>& Pair : CandidatesByKey)
    {
        if (Pair.Value.Num() > 1)
        {
            Snapshot.Warnings.Add(FString::Printf(
                TEXT("MH_W_DUPLICATE_RESOURCE_NAME: %s has %d candidates"),
                *Pair.Key.ToString(),
                Pair.Value.Num()));
        }
    }
    Snapshot.Warnings.Sort();
    return Snapshot;
}

FMHResolveOutcome FMHPayloadScanResolver::Resolve(const FMHResourceKey& Key)
{
    FMHResolveOutcome Outcome;
    if (!Key.IsCanonical())
    {
        Outcome.Status = EMHResolveStatus::Invalid;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: noncanonical resource key %s"),
            *Key.ToString());
        return Outcome;
    }

    const TArray<FCandidate>* Candidates = CandidatesByKey.Find(Key);
    if (Candidates == nullptr || Candidates->IsEmpty())
    {
        Outcome.Status = EMHResolveStatus::Unresolved;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: no source payload for %s"),
            *Key.ToString());
        return Outcome;
    }
    for (const FCandidate& Candidate : *Candidates)
    {
        Outcome.CandidatePaths.Add(Candidate.Path);
    }
    if (Candidates->Num() > 1)
    {
        Outcome.Status = EMHResolveStatus::Ambiguous;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_AMBIGUOUS_RESOURCE_NAME: %s has %d candidates"),
            *Key.ToString(),
            Candidates->Num());
        return Outcome;
    }

    Outcome.Status = EMHResolveStatus::Resolved;
    Outcome.PayloadPath = (*Candidates)[0].Path;
    Outcome.RawHash = (*Candidates)[0].RawHash;
    return Outcome;
}

} // namespace UE::MimirComposite
