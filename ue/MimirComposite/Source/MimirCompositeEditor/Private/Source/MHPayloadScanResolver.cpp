#include "Source/MHPayloadScanResolver.h"

#include "Canonical/MHCanonical.h"
#include "Codec/MHCompositeCodec.h"
#include "Codec/MHMaterialCodec.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/MHFbxPassport.h"
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

bool ReadInitialPayloadBytes(
    const FString& Path,
    TArray<uint8>& OutBytes,
    FPayloadFileState& OutState,
    FString& OutError)
{
    FPayloadFileState AfterRead;
    if (!ReadFileState(Path, OutState) ||
        !FFileHelper::LoadFileToArray(OutBytes, *Path) ||
        !ReadFileState(Path, AfterRead))
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_INVALID: cannot read payload bytes");
        return false;
    }
    if (!SameFileState(OutState, AfterRead) ||
        static_cast<int64>(OutBytes.Num()) != OutState.Size)
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: payload changed during byte read");
        return false;
    }
    return true;
}

bool ConfirmPayloadBytes(
    const FString& Path,
    const FPayloadFileState& InitialState,
    TConstArrayView<uint8> InitialBytes,
    FString& OutError)
{
    FPayloadFileState BeforeRead;
    FPayloadFileState AfterRead;
    TArray<uint8> ConfirmedBytes;
    if (!ReadFileState(Path, BeforeRead) ||
        !FFileHelper::LoadFileToArray(ConfirmedBytes, *Path) ||
        !ReadFileState(Path, AfterRead))
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: payload became unreadable during scan");
        return false;
    }
    if (!SameFileState(InitialState, BeforeRead) ||
        !SameFileState(InitialState, AfterRead) ||
        static_cast<int64>(ConfirmedBytes.Num()) != InitialState.Size ||
        ConfirmedBytes.Num() != InitialBytes.Num() ||
        (ConfirmedBytes.Num() > 0 &&
         FMemory::Memcmp(
             ConfirmedBytes.GetData(),
             InitialBytes.GetData(),
             ConfirmedBytes.Num()) != 0))
    {
        OutError = TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: payload bytes changed during scan");
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
            if (Result == ESymlinkResult::Symlink)
            {
                OutError = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: nested filesystem alias is forbidden: %s"),
                    *Component);
            }
            else
            {
                OutError = FString::Printf(
                    TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: filesystem alias status is unavailable: %s"),
                    *Component);
            }
            return false;
        }

        FString Parent = FPaths::GetPath(Component);
        FPaths::NormalizeDirectoryName(Parent);
        if (Parent.IsEmpty() || Parent.Equals(Component, ESearchCase::IgnoreCase) ||
            (!Parent.Equals(SourceRoot, ESearchCase::IgnoreCase) &&
             !FPaths::IsUnderDirectory(Parent, SourceRoot)))
        {
            OutError = FString::Printf(
                TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: cannot prove payload ancestry: %s"),
                *PayloadPath);
            return false;
        }
        Component = MoveTemp(Parent);
    }
    return true;
}

} // namespace

FMHPayloadScanResolver::FMHPayloadScanResolver(FString InSourceRoot)
    : SourceRoot(MoveTemp(InSourceRoot))
{
}

void FMHPayloadScanResolver::QuarantinePayload(
    const FString& Path,
    const FString& Diagnostic)
{
    FString NormalizedPath = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(NormalizedPath);
    const FString QualifiedDiagnostic =
        FString::Printf(TEXT("%s: %s"), *NormalizedPath, *Diagnostic);
    Quarantined.Add(QualifiedDiagnostic);

    FMHSourceQuarantine& Entry = QuarantineEntries.AddDefaulted_GetRef();
    Entry.PayloadPath = MoveTemp(NormalizedPath);
    Entry.Diagnostic = QualifiedDiagnostic;
}

void FMHPayloadScanResolver::AddPayloadFile(const FString& Path)
{
    TArray<uint8> Bytes;
    FPayloadFileState InitialState;
    FString SnapshotError;
    if (!ReadInitialPayloadBytes(Path, Bytes, InitialState, SnapshotError))
    {
        QuarantinePayload(Path, SnapshotError);
        return;
    }

    FCandidate Candidate;
    Candidate.Path = Path;
    Candidate.Fingerprint = MHPayloadFingerprint(Bytes);

    if (Path.EndsWith(TEXT(".mesh.fbx")))
    {
        FMHFbxPassport Passport;
        FString Error;
        const bool bPassportValid = MHReadFbxPassport(Path, Passport, Error);
        if (!ConfirmPayloadBytes(Path, InitialState, Bytes, SnapshotError))
        {
            QuarantinePayload(Path, SnapshotError);
            return;
        }
        if (!bPassportValid)
        {
            QuarantinePayload(Path, Error);
            return;
        }
        Candidate.Kind = EMHResourceKind::StaticMesh;
        Candidate.Uid = Passport.ResourceUid;
        Candidate.Name = Passport.Name;
        Candidate.GeometryHash = Passport.GeometryHash;
        Candidate.DescriptorHash = MHPassportDescriptorHash(Passport);
        if (Candidate.DescriptorHash.IsEmpty())
        {
            QuarantinePayload(
                Path,
                TEXT("MH_E_PASSPORT_INVALID: passport descriptor cannot be canonicalized"));
            return;
        }
    }
    else if (Path.EndsWith(TEXT(".composite")))
    {
        FMHCompositeDocument Document;
        const FMHCanonicalResult Result = MHParseCompositeV2(Bytes, Document);
        if (!Result.bSuccess)
        {
            if (!ConfirmPayloadBytes(Path, InitialState, Bytes, SnapshotError))
            {
                QuarantinePayload(Path, SnapshotError);
                return;
            }
            if (Result.Error.StartsWith(TEXT("MH_W_LEGACY_COMPOSITE_V1")))
            {
                LegacySkipped.Add(Path);
            }
            else
            {
                QuarantinePayload(Path, Result.Error);
            }
            return;
        }
        Candidate.Kind = EMHResourceKind::Composite;
        Candidate.Uid = Document.Uid;
        Candidate.Name = Document.Name;
        FString HashError;
        if (!MHCompositeSemanticHash(Bytes, Candidate.DescriptorHash, HashError))
        {
            if (!ConfirmPayloadBytes(Path, InitialState, Bytes, SnapshotError))
            {
                QuarantinePayload(Path, SnapshotError);
                return;
            }
            QuarantinePayload(Path, HashError);
            return;
        }
    }
    else
    {
        FMHMaterialDocument Document;
        const FMHCanonicalResult Result = MHParseMaterialV1(Bytes, Document);
        if (!Result.bSuccess)
        {
            if (!ConfirmPayloadBytes(Path, InitialState, Bytes, SnapshotError))
            {
                QuarantinePayload(Path, SnapshotError);
                return;
            }
            QuarantinePayload(Path, Result.Error);
            return;
        }
        Candidate.Kind = EMHResourceKind::Material;
        Candidate.Uid = Document.Uid;
        Candidate.Name = Document.Name;
        FString HashError;
        if (!MHMaterialSemanticHash(Bytes, Candidate.DescriptorHash, HashError))
        {
            if (!ConfirmPayloadBytes(Path, InitialState, Bytes, SnapshotError))
            {
                QuarantinePayload(Path, SnapshotError);
                return;
            }
            QuarantinePayload(Path, HashError);
            return;
        }
    }

    if (!ConfirmPayloadBytes(Path, InitialState, Bytes, SnapshotError))
    {
        QuarantinePayload(Path, SnapshotError);
        return;
    }

    ++CandidateFileCount;
    CandidatesByUid.FindOrAdd(Candidate.Uid).Add(MoveTemp(Candidate));
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
            if (!bIsDirectory)
            {
                FString File = FPaths::ConvertRelativePathToFull(Path);
                FPaths::NormalizeFilename(File);
                if (!FPaths::IsUnderDirectory(File, SourceRoot))
                {
                    OutError = FString::Printf(
                        TEXT("MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT: discovered payload escapes source_root: %s"),
                        *File);
                    return false;
                }
                if (File.EndsWith(TEXT(".mesh.fbx")) || File.EndsWith(TEXT(".composite")) ||
                    File.EndsWith(TEXT(".material")))
                {
                    if (!ValidateNoNestedFilesystemAlias(SourceRoot, File, OutError))
                    {
                        return false;
                    }
                    OutPaths.Add(MoveTemp(File));
                }
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
    CandidatesByUid.Reset();
    Quarantined.Reset();
    QuarantineEntries.Reset();
    LegacySkipped.Reset();
    CandidateFileCount = 0;

    SourceRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
    if (!IFileManager::Get().DirectoryExists(*SourceRoot))
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_INVALID: source_root does not exist: %s"),
            *SourceRoot);
        return false;
    }

    FPaths::NormalizeDirectoryName(SourceRoot);

    TArray<FString> PayloadPaths;
    if (!DiscoverPayloadPaths(PayloadPaths, OutError))
    {
        return false;
    }

    for (const FString& Path : PayloadPaths)
    {
        AddPayloadFile(Path);
    }

    TArray<FString> ConfirmedPayloadPaths;
    if (!DiscoverPayloadPaths(ConfirmedPayloadPaths, OutError))
    {
        CandidatesByUid.Reset();
        return false;
    }
    if (ConfirmedPayloadPaths != PayloadPaths)
    {
        OutError = FString::Printf(
            TEXT("MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED: source payload set changed during scan: %s"),
            *SourceRoot);
        CandidatesByUid.Reset();
        return false;
    }
    return true;
}

TArray<FString> FMHPayloadScanResolver::GetAllUids() const
{
    TArray<FString> Uids;
    CandidatesByUid.GenerateKeyArray(Uids);
    Uids.Sort();
    return Uids;
}

FMHResolveOutcome FMHPayloadScanResolver::Resolve(
    const FString& ResourceUid,
    const EMHResourceKind ExpectedKind)
{
    FMHResolveOutcome Outcome;

    const TArray<FCandidate>* Candidates = CandidatesByUid.Find(ResourceUid);
    if (Candidates == nullptr || Candidates->Num() == 0)
    {
        Outcome.Status = EMHResolveStatus::Unresolved;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: no valid payload declares UID %s"),
            *ResourceUid);
        return Outcome;
    }

    TArray<const FCandidate*> Matching;
    bool bMixedKinds = false;
    for (const FCandidate& Candidate : *Candidates)
    {
        Outcome.CandidatePaths.Add(Candidate.Path);
        bMixedKinds |= Candidate.Kind != (*Candidates)[0].Kind;
        if (Candidate.Kind == ExpectedKind)
        {
            Matching.Add(&Candidate);
        }
    }
    if (bMixedKinds)
    {
        Outcome.Status = EMHResolveStatus::DivergentRevisions;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_DIVERGENT_REVISIONS: UID %s is declared by multiple resource kinds; manual choice required"),
            *ResourceUid);
        return Outcome;
    }

    for (int32 Index = 1; Index < Candidates->Num(); ++Index)
    {
        if ((*Candidates)[Index].Fingerprint != (*Candidates)[0].Fingerprint)
        {
            Outcome.Status = EMHResolveStatus::DivergentRevisions;
            Outcome.Diagnostic = FString::Printf(
                TEXT("MH_E_DIVERGENT_REVISIONS: UID %s has divergent payload revisions; manual choice required"),
                *ResourceUid);
            return Outcome;
        }
    }
    if (Matching.Num() == 0)
    {
        Outcome.Status = EMHResolveStatus::KindMismatch;
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_E_RESOURCE_NOT_FOUND: UID %s exists but no candidate has the expected kind"),
            *ResourceUid);
        return Outcome;
    }

    Outcome.Status = EMHResolveStatus::Resolved;
    Outcome.PayloadPath = Matching[0]->Path;
    Outcome.Name = Matching[0]->Name;
    Outcome.Fingerprint = Matching[0]->Fingerprint;
    Outcome.GeometryHash = Matching[0]->GeometryHash;
    Outcome.DescriptorHash = Matching[0]->DescriptorHash;
    if (Matching.Num() > 1)
    {
        Outcome.Diagnostic = FString::Printf(
            TEXT("MH_W_DUPLICATE_IDENTICAL_PAYLOAD: UID %s has %d byte-identical payloads"),
            *ResourceUid,
            Matching.Num());
    }
    return Outcome;
}

} // namespace UE::MimirComposite
